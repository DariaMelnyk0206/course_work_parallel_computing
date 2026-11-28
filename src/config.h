#ifndef SEARCH_ENGINE_CONFIG_H_
#define SEARCH_ENGINE_CONFIG_H_

#include <cstdint>
#include <string>

namespace search_engine {

// Holds all environment-derived settings. Invalid values fall back to
// defaults and are reported through Logger (Requirement 3.3).
struct Config {
  int port = 8080;
  std::string dataset_dir = "./dataset";
  std::string web_dir = "./web";
  std::string ollama_host = "http://localhost:11434";
  std::string ollama_model = "qwen2.5:1.5b";
  int llm_timeout_ms = 5000;
  bool expansion_enabled = true;
  int request_threads = 64;
  int request_queue = 128;
  int max_index_threads = 256;
  int top_k = 20;

  // Reads environment variables. Defaults in this struct are the native-run
  // values (Requirement 3.2); the Docker values are supplied by the
  // `environment:` section of docker-compose.yml (Requirement 3.1).
  // Invalid values are logged and replaced by the default (Requirement 3.3).
  static Config FromEnvironment();
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_CONFIG_H_
