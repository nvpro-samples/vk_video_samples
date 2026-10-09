/*
* Copyright (C) 2026 Igalia, S.L.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <iostream>
#include <fstream>
#include <string>
#include <stdarg.h>

#if defined(__GNUC__) || defined(__clang__)
#define VKVS_PRINTF_FORMAT(fmtIndex, firstArg) __attribute__((format(printf, fmtIndex, firstArg)))
#else
#define VKVS_PRINTF_FORMAT(fmtIndex, firstArg)
#endif

// Enum for log levels. Scoped to keep LOG_* out of the global namespace, where
// they would collide with the LogPriority enumerators in VulkanDeviceContext and Shell.
enum class LogLevel {
    LOG_NONE = 0,  // Use this to disable logging
    LOG_ERROR,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG
};

#define LOG_S_DEBUG Logger::instance()(LogLevel::LOG_DEBUG)
#define LOG_S_INFO Logger::instance()(LogLevel::LOG_INFO)
#define LOG_S_WARN Logger::instance()(LogLevel::LOG_WARNING)
#define LOG_S_ERROR Logger::instance()(LogLevel::LOG_ERROR)

#define LOG_CAT_LEVEL(LEVEL, CAT, ...) Logger::instance().printf(LEVEL, CAT, __VA_ARGS__)


#define LOG_DEBUG_CAT(CAT, ...) LOG_CAT_LEVEL(LogLevel::LOG_DEBUG, CAT, __VA_ARGS__)
#define LOG_INFO_CAT(CAT, ...) LOG_CAT_LEVEL(LogLevel::LOG_INFO, CAT, __VA_ARGS__)
#define LOG_WARN_CAT(CAT, ...) LOG_CAT_LEVEL(LogLevel::LOG_WARNING, CAT, __VA_ARGS__)
#define LOG_ERROR_CAT(CAT, ...) LOG_CAT_LEVEL(LogLevel::LOG_ERROR, CAT, __VA_ARGS__)

#define LOG_DEBUG(...) LOG_DEBUG_CAT("", __VA_ARGS__)
#define LOG_INFO(...) LOG_INFO_CAT("", __VA_ARGS__)
#define LOG_WARN(...) LOG_WARN_CAT("", __VA_ARGS__)
#define LOG_ERROR(...) LOG_ERROR_CAT("", __VA_ARGS__)

#define LOG_DEBUG_CONFIG(...) LOG_DEBUG_CAT("config:\t", __VA_ARGS__)
#define LOG_INFO_CONFIG(...) LOG_INFO_CAT("config:\t", __VA_ARGS__)
#define LOG_WARN_CONFIG(...) LOG_WARN_CAT("config:\t", __VA_ARGS__)
#define LOG_ERROR_CONFIG(...) LOG_ERROR_CAT("config:\t", __VA_ARGS__)

class Logger {
private:
    std::ostream& os;      // The output stream (e.g., std::cout or std::ofstream)
    std::ostream& err;      // The error stream (e.g., std::cerr)
    LogLevel currentLevel; // Current log level
    LogLevel messageLevel; // The log level for the current message

public:
    static Logger &instance ()
    {
      static Logger instance;
      return instance;
    }
    // Constructor to set the output stream and log level (default is INFO)
    Logger(std::ostream& outStream = std::cout, std::ostream& errStream = std::cerr, LogLevel level = LogLevel::LOG_INFO)
        : os(outStream), err(errStream), currentLevel(level), messageLevel(LogLevel::LOG_INFO) {}

    // Set the log level for the logger
    void setLogLevel(int level) {
        if (level > static_cast<int>(LogLevel::LOG_DEBUG))
            level = static_cast<int>(LogLevel::LOG_DEBUG);
        if (level < static_cast<int>(LogLevel::LOG_NONE))
            level = static_cast<int>(LogLevel::LOG_NONE);
        currentLevel = static_cast<LogLevel>(level);
    }

    // Set the log level for the current message
    Logger& operator()(LogLevel level) {
        messageLevel = level;
        if (messageLevel <= currentLevel) {
            const char* prefix = levelPrefix(level);
            if (prefix[0] != '\0') {
                streamFor(messageLevel) << prefix;
            }
        }
        return *this;
    }

    // Overload the << operator for generic types
    template<typename T>
    Logger& operator<<(const T& data) {
        if (messageLevel <= currentLevel) {
            streamFor(messageLevel) << data;
        }
        return *this;
    }

    // Overload for stream manipulators (like std::endl)
    typedef std::ostream& (*StreamManipulator)(std::ostream&);
    Logger& operator<<(StreamManipulator manip) {
        if (messageLevel <= currentLevel) {
            streamFor(messageLevel) << manip;
        }
        return *this;
    }

    // Argument 1 is the implicit 'this'.
    VKVS_PRINTF_FORMAT(4, 5)
    void printf(LogLevel level, const char* category, const char* format, ...) {
        if (level <= currentLevel) {
            FILE* out = isDiagnostic(level) ? stderr : stdout;
            va_list args;
            va_start(args, format);
            fputs(levelPrefix(level), out);
            fputs(category, out);
            vfprintf(out, format, args);
            va_end(args);
        }
    }

private:
    // Errors and warnings go to stderr so tools scanning diagnostics (e.g. the test runner) see them.
    static bool isDiagnostic(LogLevel level) { return level <= LogLevel::LOG_WARNING; }
    std::ostream& streamFor(LogLevel level) { return isDiagnostic(level) ? err : os; }

    static const char* levelPrefix(LogLevel level) {
        switch (level) {
        case LogLevel::LOG_ERROR:   return "Error: ";
        case LogLevel::LOG_WARNING: return "Warning: ";
        default:          return "";
        }
    }
};


#endif
