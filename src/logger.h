#ifndef SEARCH_ENGINE_LOGGER_H_
#define SEARCH_ENGINE_LOGGER_H_

#include <mutex>
#include <string>

namespace search_engine {

// Thread-safe line logger writing to stderr. A single mutex serializes
// whole-line writes so interleaved output from multiple threads never
// splits a line (Requirement 5.7).
class Logger {
 public:
  static Logger& Instance();

  void Info(const std::string& message);
  void Warn(const std::string& message);
  void Error(const std::string& message);

 private:
  Logger() = default;
  void WriteLine(const char* level, const std::string& message);

  std::mutex mutex_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_LOGGER_H_
