#include "config.h"

#include <charconv>
#include <cstdlib>
#include <string>

#include "logger.h"

namespace search_engine {
namespace {

// Returns the variable value, or nullptr if it is unset.
const char* GetEnv(const char* name) { return std::getenv(name); }

void WarnInvalid(const char* name, const std::string& value,
                 const std::string& default_value) {
  Logger::Instance().Warn(std::string("Invalid value '") + value + "' for " +
                          name + ", using default " + default_value);
}

// Reads an integer in [min, max]; keeps `*target` (the default) otherwise.
void ReadInt(const char* name, int min, int max, int* target) {
  const char* raw = GetEnv(name);
  if (raw == nullptr) return;
  const std::string value(raw);
  int parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (value.empty() || ec != std::errc() || ptr != end || parsed < min ||
      parsed > max) {
    WarnInvalid(name, value, std::to_string(*target));
    return;
  }
  *target = parsed;
}

// Reads a non-empty string; keeps `*target` (the default) otherwise.
void ReadString(const char* name, std::string* target) {
  const char* raw = GetEnv(name);
  if (raw == nullptr) return;
  const std::string value(raw);
  if (value.empty()) {
    WarnInvalid(name, value, *target);
    return;
  }
  *target = value;
}

}  // namespace

Config Config::FromEnvironment() {
  Config config;
  ReadInt("PORT", 1, 65535, &config.port);
  ReadString("DATASET_DIR", &config.dataset_dir);
  ReadString("WEB_DIR", &config.web_dir);
  ReadString("OLLAMA_HOST", &config.ollama_host);
  ReadString("OLLAMA_MODEL", &config.ollama_model);
  ReadInt("LLM_TIMEOUT_MS", 1, 3600000, &config.llm_timeout_ms);
  ReadInt("REQUEST_THREADS", 1, 4096, &config.request_threads);
  // 0 selects an unbounded queue (ThreadPool capacity == 0).
  ReadInt("REQUEST_QUEUE", 0, 1000000, &config.request_queue);
  ReadInt("MAX_INDEX_THREADS", 1, 4096, &config.max_index_threads);
  ReadInt("TOP_K", 1, 10000, &config.top_k);

  if (const char* raw = GetEnv("EXPANSION")) {
    const std::string value(raw);
    if (value == "on") {
      config.expansion_enabled = true;
    } else if (value == "off") {
      config.expansion_enabled = false;
    } else {
      WarnInvalid("EXPANSION", value, "on");
    }
  }
  return config;
}

}  // namespace search_engine
