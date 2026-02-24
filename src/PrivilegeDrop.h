/**
 * @file PrivilegeDrop.h
 * @brief Drop root privileges while retaining network capabilities
 *
 * Uses Linux-native syscalls (prctl, capset) — no libcap dependency.
 * Pattern: start as root → init network → setuid(user) → restore caps.
 */

#pragma once

#include <string>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <linux/capability.h>

/**
 * @brief Drop process privileges to the specified user while retaining
 *        network capabilities needed for Diretta protocol.
 *
 * Must be called from the main thread AFTER all network initialization.
 * Uses PR_SET_KEEPCAPS + capset() to retain CAP_NET_RAW, CAP_NET_ADMIN,
 * and CAP_SYS_NICE after the UID change.
 *
 * Note: PR_SET_KEEPCAPS is per-thread on Linux. Only the calling thread
 * retains capabilities. Worker threads (SDK, audio) lose theirs but
 * can still use already-opened sockets.
 *
 * @param username Target user name (empty = no-op)
 * @return true on success, false on failure
 */
inline bool dropPrivileges(const std::string& username) {
    if (username.empty()) return true;

    if (getuid() != 0) {
        std::cout << "[PrivilegeDrop] Not running as root, skipping privilege drop"
                  << std::endl;
        return true;
    }

    // Resolve target user
    struct passwd* pw = getpwnam(username.c_str());
    if (!pw) {
        std::cerr << "[PrivilegeDrop] User '" << username << "' not found"
                  << std::endl;
        return false;
    }

    uid_t targetUid = pw->pw_uid;
    gid_t targetGid = pw->pw_gid;

    if (targetUid == 0) {
        std::cerr << "[PrivilegeDrop] Target user '" << username
                  << "' is root, no privilege drop needed" << std::endl;
        return true;
    }

    // 1. Set PR_SET_KEEPCAPS so permitted capabilities survive setuid()
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
        std::cerr << "[PrivilegeDrop] prctl(PR_SET_KEEPCAPS) failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    // 2. Drop group privileges first (must be done while still root)
    if (setgid(targetGid) < 0) {
        std::cerr << "[PrivilegeDrop] setgid(" << targetGid << ") failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    if (initgroups(username.c_str(), targetGid) < 0) {
        std::cerr << "[PrivilegeDrop] initgroups() failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    // 3. Drop user privileges
    if (setuid(targetUid) < 0) {
        std::cerr << "[PrivilegeDrop] setuid(" << targetUid << ") failed: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    // 4. Verify the drop was effective
    if (getuid() == 0 || geteuid() == 0) {
        std::cerr << "[PrivilegeDrop] Failed to drop root privileges"
                  << std::endl;
        return false;
    }

    // 5. Restore effective capabilities via capset() syscall
    //    After setuid with keepcaps, permitted set is preserved but
    //    effective set is cleared — we must restore it explicitly
    struct __user_cap_header_struct hdr = {};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;  // Current thread

    struct __user_cap_data_struct data[2] = {};
    uint32_t caps = (1u << CAP_NET_RAW)
                  | (1u << CAP_NET_ADMIN)
                  | (1u << CAP_SYS_NICE);
    data[0].effective   = caps;
    data[0].permitted   = caps;
    data[0].inheritable = 0;
    // data[1] stays zeroed (caps 32-63, none needed)

    if (syscall(SYS_capset, &hdr, data) < 0) {
        std::cerr << "[PrivilegeDrop] capset() failed: "
                  << std::strerror(errno)
                  << " — running without capabilities" << std::endl;
        // Non-fatal: process continues with reduced capabilities
        // Network operations may fail on format transitions
    }

    // 6. Clear keepcaps for security hardening
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);

    std::cout << "[PrivilegeDrop] Dropped privileges to user '"
              << username << "' (uid=" << targetUid
              << ", gid=" << targetGid << ")" << std::endl;

    return true;
}
