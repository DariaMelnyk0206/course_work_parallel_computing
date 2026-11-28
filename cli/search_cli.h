#ifndef SEARCH_ENGINE_SEARCH_CLI_H_
#define SEARCH_ENGINE_SEARCH_CLI_H_

#include <string>
#include <vector>

namespace search_engine {

struct CliOptions {
  std::string host = "http://localhost:8080";
  std::string query;
  int top_k = 20;
  bool show_health = false;
  bool show_stats = false;
  int index_threads = 0;  // 0 == not requested.
};

// Parses argv (Requirement 23.4). Returns exit code 2 on malformed
// arguments without making any network call (Requirement 23.8).
int ParseArgs(int argc, char** argv, CliOptions* options);

// Executes the requested action (search, --health, --stats, --index) over
// REST only (Requirement 23.1-23.2), prints results or the server's
// `error` field (Requirement 23.3, 23.6), and returns the CLI's exit code
// (Requirement 23.8).
int Run(const CliOptions& options);

}  // namespace search_engine

#endif  // SEARCH_ENGINE_SEARCH_CLI_H_
