/**
 * @file LogLevel.h
 * @brief Centralized log level system for DirettaRendererUPnP
 *
 * Provides 4 log levels (ERROR, WARN, INFO, DEBUG) with macros.
 * Default level is INFO. --verbose sets DEBUG, --quiet sets WARN.
 *
 * Replaces per-file DEBUG_LOG definitions with a unified system.
 * In NOLOG builds, all logging is compiled out.
 */

#ifndef LOG_LEVEL_H
#define LOG_LEVEL_H

#include <iostream>

enum class LogLevel { ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3 };

extern LogLevel g_logLevel;

#ifdef NOLOG
// Production build: compile out all logging for zero overhead
#define LOG_ERROR(x) do {} while(0)
#define LOG_WARN(x)  do {} while(0)
#define LOG_INFO(x)  do {} while(0)
#define LOG_DEBUG(x) do {} while(0)
#else
#define LOG_ERROR(x) do { \
    if (g_logLevel >= LogLevel::ERROR) { std::cerr << x << std::endl; } \
} while(0)

#define LOG_WARN(x) do { \
    if (g_logLevel >= LogLevel::WARN) { std::cout << "[WARN] " << x << std::endl; } \
} while(0)

#define LOG_INFO(x) do { \
    if (g_logLevel >= LogLevel::INFO) { std::cout << x << std::endl; } \
} while(0)

#define LOG_DEBUG(x) do { \
    if (g_logLevel >= LogLevel::DEBUG) { std::cout << x << std::endl; } \
} while(0)
#endif

#endif // LOG_LEVEL_H
