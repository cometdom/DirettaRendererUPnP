// SPDX-License-Identifier: MIT
// This file is part of DirettaRendererUPnP.
// See LICENSE for copyright holders and terms.

/**
 * @file TimestampedLogger.h
 * @brief Automatic timestamp addition to all console output
 *
 * Usage: Install at the beginning of main() to automatically add
 *        timestamps to ALL std::cout and std::cerr output.
 *
 * @version 2.0.0
 * @date 2025-01-22
 */

#ifndef TIMESTAMPED_LOGGER_H
#define TIMESTAMPED_LOGGER_H

#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>

/**
 * @brief Custom streambuf that automatically adds timestamps to each line
 *
 * This class wraps an existing streambuf and intercepts all output to add
 * a timestamp at the beginning of each new line.
 *
 * Format: [HH:MM:SS.mmm] message
 */
class TimestampedStreambuf : public std::streambuf {
private:
    std::streambuf* m_dest;      // Destination buffer (original cout/cerr)
    bool m_atLineStart = true;   // Track if we're at the start of a line

    /**
     * @brief Generate current timestamp string
     * @return Formatted timestamp: "HH:MM:SS.mmm"
     */
    std::string getTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

protected:
    /**
     * @brief Override character output to add timestamps
     *
     * Automatically adds "[HH:MM:SS.mmm] " at the start of each line.
     */
    int overflow(int c) override {
        // Add timestamp at the beginning of each line
        if (m_atLineStart && c != '\n') {
            std::string timestamp = "[" + getTimestamp() + "] ";
            m_dest->sputn(timestamp.c_str(), timestamp.size());
            m_atLineStart = false;
        }

        // Track newlines to know when we're starting a new line
        if (c == '\n') {
            m_atLineStart = true;
        }

        // Forward the character to the destination buffer
        return m_dest->sputc(c);
    }

    /**
     * @brief Block write: one sputn per line segment instead of one sputc per
     *        character. std::cerr's stdio stream is unbuffered, so the
     *        per-character path cost one write(2) per character on every
     *        warning/error line.
     */
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize done = 0;
        while (done < n) {
            if (m_atLineStart && s[done] != '\n') {
                std::string timestamp = "[" + getTimestamp() + "] ";
                m_dest->sputn(timestamp.c_str(), timestamp.size());
                m_atLineStart = false;
            }
            const char* nl = static_cast<const char*>(memchr(s + done, '\n', n - done));
            std::streamsize len = nl ? (nl - (s + done)) + 1 : n - done;
            if (m_dest->sputn(s + done, len) != len) return done;
            done += len;
            if (nl) m_atLineStart = true;
        }
        return done;
    }

    /**
     * @brief Propagate flushes (std::endl, std::flush) to the wrapped buffer.
     *
     * Without this override a flush stopped here and never reached the
     * stdio buffer behind std::cout. On a terminal that buffer is
     * line-buffered so nothing was noticed; under systemd stdout is a
     * socket, fully buffered (4 KB), and log lines only surfaced once
     * enough of them had piled up — or never, for a quiet renderer.
     */
    int sync() override {
        return m_dest->pubsync();
    }

public:
    /**
     * @brief Constructor
     * @param dest Original streambuf to wrap (e.g., cout.rdbuf())
     */
    TimestampedStreambuf(std::streambuf* dest) : m_dest(dest) {}
};

/**
 * @brief Install timestamped logging for std::cout and std::cerr
 *
 * Call this at the beginning of main() to enable automatic timestamps
 * on all console output.
 *
 * Example:
 * @code
 * int main() {
 *     installTimestampedLogging();
 *     std::cout << "This will have a timestamp!" << std::endl;
 * }
 * @endcode
 *
 * @note The buffers must remain in scope for the entire program lifetime.
 *       Declare them as static variables in main().
 */
inline void installTimestampedLogging(TimestampedStreambuf*& coutBuf, TimestampedStreambuf*& cerrBuf) {
    // Create new buffers (caller must manage lifetime)
    coutBuf = new TimestampedStreambuf(std::cout.rdbuf());
    cerrBuf = new TimestampedStreambuf(std::cerr.rdbuf());

    // Install the new buffers
    std::cout.rdbuf(coutBuf);
    std::cerr.rdbuf(cerrBuf);
}

#endif // TIMESTAMPED_LOGGER_H
