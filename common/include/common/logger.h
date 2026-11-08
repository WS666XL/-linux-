#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace common {

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR };

inline LogLevel& log_level() {
  static LogLevel lv = LogLevel::INFO;
  return lv;
}

inline void log(LogLevel lv, const char* fmt, ...) {
  if (lv < log_level()) return;
  static std::mutex mu;
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm tmbuf{};
  localtime_r(&ts.tv_sec, &tmbuf);
  static const char* kNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  std::lock_guard<std::mutex> lk(mu);
  fprintf(stderr, "[%02d:%02d:%02d.%03ld][%s] %s\n", tmbuf.tm_hour, tmbuf.tm_min,
          tmbuf.tm_sec, ts.tv_nsec / 1000000, kNames[static_cast<int>(lv)], buf);
}

}  // namespace common

#define LOG_DEBUG(...) ::common::log(::common::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...) ::common::log(::common::LogLevel::INFO, __VA_ARGS__)
#define LOG_WARN(...) ::common::log(::common::LogLevel::WARN, __VA_ARGS__)
#define LOG_ERROR(...) ::common::log(::common::LogLevel::ERROR, __VA_ARGS__)
