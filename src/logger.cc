#include "logger.h"

#include <cstdio>

namespace search_engine {

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

void Logger::Info(const std::string& message) { WriteLine("INFO", message); }

void Logger::Warn(const std::string& message) { WriteLine("WARN", message); }

void Logger::Error(const std::string& message) { WriteLine("ERROR", message); }

void Logger::WriteLine(const char* level, const std::string& message) {
  // Build the whole line first so the critical section is a single write.
  std::string line;
  line.reserve(message.size() + 16);
  line += '[';
  line += level;
  line += "] ";
  line += message;
  line += '\n';
  std::lock_guard<std::mutex> lock(mutex_);
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
}

}  // namespace search_engine
