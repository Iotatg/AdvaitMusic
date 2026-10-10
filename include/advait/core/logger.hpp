#pragma once

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace advait {

enum class LogLevel { Debug, Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& instance();
    void set_level(LogLevel level);
    void log(LogLevel level, std::string_view tag, std::string_view msg);

private:
    std::mutex mu_;
    LogLevel level_{LogLevel::Info};
};

#define ADVAIT_LOG_DEBUG(tag, msg) ::advait::Logger::instance().log(::advait::LogLevel::Debug, tag, msg)
#define ADVAIT_LOG_INFO(tag, msg)  ::advait::Logger::instance().log(::advait::LogLevel::Info, tag, msg)
#define ADVAIT_LOG_WARN(tag, msg)  ::advait::Logger::instance().log(::advait::LogLevel::Warn, tag, msg)
#define ADVAIT_LOG_ERROR(tag, msg) ::advait::Logger::instance().log(::advait::LogLevel::Error, tag, msg)
#define ADVAIT_LOG_FATAL(tag, msg) ::advait::Logger::instance().log(::advait::LogLevel::Fatal, tag, msg)

}  // namespace advait
