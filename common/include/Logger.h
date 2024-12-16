#ifndef LOGGER_H_
#define LOGGER_H_

#include <iostream>
#include <fstream>
#include <string>
#include <stdarg.h>

// Enum for log levels
enum LogLevel {
    NONE = 0,  // Use this to disable logging
    ERROR,
    WARNING,
    INFO,
    DEBUG
};

#define LOG_S_DEBUG Logger::instance()(LogLevel::DEBUG)
#define LOG_S_INFO Logger::instance()(LogLevel::INFO)
#define LOG_S_WARN Logger::instance()(LogLevel::WARNING)
#define LOG_S_ERROR Logger::instance()(LogLevel::ERROR)

#define LOG_DEBUG(ARGS ...) Logger::instance().printf(LogLevel::DEBUG, ## ARGS)
#define LOG_INFO(ARGS...) Logger::instance().printf(LogLevel::INFO, ## ARGS)
#define LOG_WARN(ARGS...) Logger::instance().printf(LogLevel::WARNING, ## ARGS)
#define LOG_ERROR(ARGS...) Logger::instance().printf(LogLevel::ERROR, ## ARGS)

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
    Logger(std::ostream& outStream = std::cout, std::ostream& errStream = std::cerr, LogLevel level = LogLevel::INFO)
        : os(outStream), err(errStream), currentLevel(level), messageLevel(LogLevel::INFO) {}

    // Set the log level for the logger
    void setLogLevel(int level) {
        if (level > DEBUG)
            level = DEBUG;
        currentLevel = static_cast<LogLevel>(level);
    }

    // Set the log level for the current message
    Logger& operator()(LogLevel level) {
        messageLevel = level;
        return *this;
    }

    // Overload the << operator for generic types
    template<typename T>
    Logger& operator<<(const T& data) {
        if (messageLevel <= currentLevel) {
            if (messageLevel == ERROR)
              err << data;
            else
              os << data;
        }
        return *this;
    }

    // Overload for stream manipulators (like std::endl)
    typedef std::ostream& (*StreamManipulator)(std::ostream&);
    Logger& operator<<(StreamManipulator manip) {
        if (messageLevel <= currentLevel) {
            if (messageLevel == ERROR)
              err << manip;
            else
              os << manip;  // Handle std::endl, std::flush, etc.
        }
        return *this;
    }

    void printf(LogLevel level, const char* format, ...) {
        if (level <= currentLevel) {
            va_list args;
            va_start(args, format);
            if (level == ERROR)
              vfprintf(stderr, format, args);
            else
              vfprintf(stdout,format, args);
            va_end(args);
        }
    }
};


#endif