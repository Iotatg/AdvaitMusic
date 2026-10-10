#include "advait/core/logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

namespace advait {

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> g(mu_);
    level_ = level;
}

void Logger::log(LogLevel level, std::string_view tag, std::string_view msg) {
    std::lock_guard<std::mutex> g(mu_);
    if (static_cast<int>(level) < static_cast<int>(level_)) return;
    const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    int idx = static_cast<int>(level);
    if (idx < 0 || idx > 4) idx = 1;
    std::cerr << buf << " [" << names[idx] << "] " << tag << ": " << msg << "\n";
    if (level == LogLevel::Fatal) std::abort();
}

}  // namespace advait
