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
constexpr int kIndexTimeoutSeconds = 3600;

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

// Sends one request and returns the parsed JSON body of a 200 response.
// Prints a message and returns false on network, HTTP or format errors.
bool Fetch(const CliOptions& options, const std::string& method,
           const std::string& path, const nlohmann::json& request,
           int timeout_seconds, nlohmann::json* body) {
  httplib::Client client(options.host);
  client.set_connection_timeout(timeout_seconds, 0);
  client.set_read_timeout(timeout_seconds, 0);
  client.set_write_timeout(timeout_seconds, 0);
  httplib::Result res =
      method == "POST"
          ? client.Post(path,
                        request.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json")
          : client.Get(path);
  if (!res) {
    std::cerr << "Error: server is unavailable at " << options.host << " ("
              << httplib::to_string(res.error()) << ")\n";
    return false;
  }
  nlohmann::json parsed = nlohmann::json::parse(res->body, nullptr, false);
  if (res->status != 200) {
    if (parsed.is_object() && parsed.contains("error") &&
        parsed["error"].is_string()) {
      std::cerr << "Error: " << parsed["error"].get<std::string>() << "\n";
    } else {
      std::cerr << "Error: server returned HTTP " << res->status << "\n";
    }
    return false;
  }
  if (!parsed.is_object()) {
    std::cerr << "Error: unexpected response from server\n";
    return false;
  }
  *body = std::move(parsed);
  return true;
}

int RunHealth(const CliOptions& options) {
  nlohmann::json body;
  if (!Fetch(options, "GET", "/api/health", nullptr, kDefaultTimeoutSeconds,
             &body)) {
    return 1;
  }
  if (!body.contains("status") || !body["status"].is_string()) {
    std::cerr << "Error: unexpected response from server\n";
    return 1;
  }
  bool ollama = body.value("ollama", false);
  std::cout << "status: " << body["status"].get<std::string>()
            << "\nollama: " << (ollama ? "true" : "false") << "\n";
  return 0;
}

int RunStats(const CliOptions& options) {
  nlohmann::json body;
  if (!Fetch(options, "GET", "/api/stats", nullptr, kDefaultTimeoutSeconds,
             &body)) {
    return 1;
  }
  if (!body.contains("documents") || !body["documents"].is_number_integer() ||
      !body.contains("index_build_time_ms") ||
      !body["index_build_time_ms"].is_number_integer() ||
      !body.contains("threads_used") ||
      !body["threads_used"].is_number_integer()) {
    std::cerr << "Error: unexpected response from server\n";
    return 1;
  }
  std::cout << "documents: " << body["documents"].get<int64_t>()
            << "\nindex_build_time_ms: "
            << body["index_build_time_ms"].get<int64_t>()
            << "\nthreads_used: " << body["threads_used"].get<int64_t>()
            << "\n";
  return 0;
}

int RunIndex(const CliOptions& options) {
  nlohmann::json body;
  nlohmann::json request = {{"threads", options.index_threads}};
  if (!Fetch(options, "POST", "/api/index", request, kIndexTimeoutSeconds,
             &body)) {
    return 1;
  }
  if (!body.contains("documents_indexed") ||
      !body["documents_indexed"].is_number_integer() ||
      !body.contains("time_ms") || !body["time_ms"].is_number_integer()) {
    std::cerr << "Error: unexpected response from server\n";
    return 1;
  }
  std::cout << "Indexed " << body["documents_indexed"].get<int64_t>()
            << " documents in " << body["time_ms"].get<int64_t>() << " ms ("
            << options.index_threads << " threads)\n";
  return 0;
}

int RunSearch(const CliOptions& options) {
  nlohmann::json body;
  nlohmann::json request = {{"query", options.query}, {"mode", "keyword"}};
  if (!Fetch(options, "POST", "/api/search", request, kSearchTimeoutSeconds,
             &body)) {
    return 1;
  }
  if (!body.contains("results") || !body["results"].is_array() ||
      !body.contains("time_ms") || !body["time_ms"].is_number()) {
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
  int modes = static_cast<int>(options->show_health) +
              static_cast<int>(options->show_stats) +
              static_cast<int>(options->index_threads != 0) +
              static_cast<int>(has_query);
  if (modes > 1) {
    std::cerr << "Error: choose only one of query, --health, --stats, "
                 "--index\n";
    return 2;
  }
  if (modes == 0) {
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
  if (options.show_stats) {
    return RunStats(options);
  }
  if (options.index_threads != 0) {
    return RunIndex(options);
  }
  if (!options.query.empty()) {
    return RunSearch(options);
  }
  std::cerr << "Error: nothing to do\n";
  return 2;
}

}  // namespace search_engine
