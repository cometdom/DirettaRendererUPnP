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

/**
 * @brief Sink that swallows everything written to it
 *
 * --quiet promises "errors and warnings only", but ~250 raw std::cout
 * statements sit on the track path (open, gapless, prefill, format change…)
 * and each std::endl is a write(2) into the journald pipe from whichever
 * thread emits it — decode thread included. Rather than touching every
 * call site, quiet mode replaces std::cout's buffer with this one: the
 * formatting still happens, the syscall never does. Errors and warnings use
 * std::cerr and are untouched.
 */
class NullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c == EOF ? '\0' : c; }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

inline void installQuietStdout() {
    static NullStreambuf nullBuf;
    std::cout.flush();
    std::cout.rdbuf(&nullBuf);
}

#endif // TIMESTAMPED_LOGGER_H
