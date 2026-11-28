#include "search_cli.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace search_engine {

namespace {

constexpr int kDefaultTimeoutSeconds = 10;
constexpr int kSearchTimeoutSeconds = 60;

bool ParsePositiveInt(const std::string& text, int* value) {
  if (text.empty() || text.size() > 9) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  int parsed = std::atoi(text.c_str());
  if (parsed < 1) {
    return false;
  }
  *value = parsed;
  return true;
}

int RunHealth(const CliOptions& options) {
  httplib::Client client(options.host);
  client.set_connection_timeout(kDefaultTimeoutSeconds, 0);
  client.set_read_timeout(kDefaultTimeoutSeconds, 0);
  client.set_write_timeout(kDefaultTimeoutSeconds, 0);
  httplib::Result res = client.Get("/api/health");
  if (!res) {
    std::cerr << "Error: server is unavailable at " << options.host << " ("
              << httplib::to_string(res.error()) << ")\n";
    return 1;
  }
  nlohmann::json body = nlohmann::json::parse(res->body, nullptr, false);
  if (res->status != 200) {
    if (body.is_object() && body.contains("error") &&
        body["error"].is_string()) {
      std::cerr << "Error: " << body["error"].get<std::string>() << "\n";
    } else {
      std::cerr << "Error: server returned HTTP " << res->status << "\n";
    }
    return 1;
  }
  if (!body.is_object() || !body.contains("status") ||
      !body["status"].is_string()) {
    std::cerr << "Error: unexpected response from server\n";
    return 1;
  }
  bool ollama = body.value("ollama", false);
  std::cout << "status: " << body["status"].get<std::string>()
            << "\nollama: " << (ollama ? "true" : "false") << "\n";
  return 0;
}

int RunSearch(const CliOptions& options) {
  httplib::Client client(options.host);
  client.set_connection_timeout(kSearchTimeoutSeconds, 0);
  client.set_read_timeout(kSearchTimeoutSeconds, 0);
  client.set_write_timeout(kSearchTimeoutSeconds, 0);
  nlohmann::json request = {{"query", options.query}, {"mode", "keyword"}};
  httplib::Result res = client.Post(
      "/api/search",
      request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
      "application/json");
  if (!res) {
    std::cerr << "Error: server is unavailable at " << options.host << " ("
              << httplib::to_string(res.error()) << ")\n";
    return 1;
  }
  nlohmann::json body = nlohmann::json::parse(res->body, nullptr, false);
  if (res->status != 200) {
    if (body.is_object() && body.contains("error") &&
        body["error"].is_string()) {
      std::cerr << "Error: " << body["error"].get<std::string>() << "\n";
    } else {
      std::cerr << "Error: server returned HTTP " << res->status << "\n";
    }
    return 1;
  }
  if (!body.is_object() || !body.contains("results") ||
      !body["results"].is_array() || !body.contains("time_ms") ||
      !body["time_ms"].is_number()) {
    std::cerr << "Error: unexpected response from server\n";
    return 1;
  }
  const nlohmann::json& results = body["results"];
  size_t shown = std::min(static_cast<size_t>(options.top_k), results.size());
  if (shown > 0) {
    std::printf("%-5s %-16s %-8s %s\n", "Rank", "doc_id", "Score", "Snippet");
  }
  for (size_t i = 0; i < shown; ++i) {
    const nlohmann::json& item = results[i];
    std::string doc_id = item.value("doc_id", std::string());
    double score = item.value("score", 0.0);
    std::string snippet = item.value("snippet", std::string());
    std::printf("%-5zu %-16s %-8.4f %s\n", i + 1, doc_id.c_str(), score,
                snippet.c_str());
  }
  std::printf("%zu results in %lld ms\n", shown,
              static_cast<long long>(body["time_ms"].get<int64_t>()));
  return 0;
}

}  // namespace

int ParseArgs(int argc, char** argv, CliOptions* options) {
  if (const char* env_host = std::getenv("SEARCH_HOST");
      env_host != nullptr && *env_host != '\0') {
    options->host = env_host;
  }
  bool has_query = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--health") {
      options->show_health = true;
    } else if (arg == "--stats") {
      options->show_stats = true;
    } else if (arg == "--host") {
      if (i + 1 >= argc || std::string(argv[i + 1]).empty()) {
        std::cerr << "Error: --host requires a URL\n";
        return 2;
      }
      options->host = argv[++i];
    } else if (arg == "--top") {
      if (i + 1 >= argc || !ParsePositiveInt(argv[i + 1], &options->top_k)) {
        std::cerr << "Error: --top requires an integer >= 1\n";
        return 2;
      }
      ++i;
    } else if (arg == "--index") {
      if (i + 1 >= argc ||
          !ParsePositiveInt(argv[i + 1], &options->index_threads)) {
        std::cerr << "Error: --index requires an integer >= 1\n";
        return 2;
      }
      ++i;
    } else if (arg.size() > 1 && arg[0] == '-' && arg[1] == '-') {
      std::cerr << "Error: unknown option " << arg << "\n";
      return 2;
    } else {
      if (has_query) {
        std::cerr << "Error: only one query argument is allowed\n";
        return 2;
      }
      options->query = arg;
      has_query = true;
    }
  }
  if (!options->show_health && !options->show_stats &&
      options->index_threads == 0 && !has_query) {
    std::cerr << "Usage: search [--host URL] [--top K] "
                 "(\"query\" | --health | --stats | --index N)\n";
    return 2;
  }
  return 0;
}

int Run(const CliOptions& options) {
  if (options.show_health) {
    return RunHealth(options);
  }
  if (!options.show_stats && options.index_threads == 0 &&
      !options.query.empty()) {
    return RunSearch(options);
  }
  std::cerr << "Error: this mode is not implemented yet\n";
  return 2;
}

}  // namespace search_engine
