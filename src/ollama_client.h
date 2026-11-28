#ifndef SEARCH_ENGINE_OLLAMA_CLIENT_H_
#define SEARCH_ENGINE_OLLAMA_CLIENT_H_

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace search_engine {

struct ExpansionResult {
  // At most 10, deduplicated, never containing a term from original_terms.
  std::vector<std::string> new_terms;
  bool used_fallback = false;  // True if Ollama was not used.
};

// Integrates with Ollama's /api/generate (query expansion) and /api/tags
// (availability check). Serializes generate calls through a
// std::timed_mutex so at most one LLM request is in flight at a time
// (Requirement 18.5, PD Section 2.2 "Фаза 2"). A single budget
// (Config::llm_timeout_ms) covers both the queue wait (try_lock_for) and
// the HTTP call (Requirement 18.6).
class OllamaClient {
 public:
  OllamaClient(std::string host, std::string model,
               std::chrono::milliseconds timeout);

  // Builds the prompt from the unmodified query text (Requirement 18.2,
  // 15.8), calls /api/generate, and parses the "response" field in this
  // order: strip the "Keywords:" prefix, Tokenizer, deduplicate, remove every
  // term present in `original_terms`, keep the first 10. The limit therefore
  // applies to NEW terms only, and matches with the original query do not
  // consume slots (Requirement 18.3). `original_terms` is the unique,
  // ordered output of Tokenizer for `original_query`, computed by the caller
  // (SearchService::ExpandQuery) (Requirement 15.3). On unavailability,
  // HTTP error, exhausted budget, or an empty parse, returns a fallback
  // result and logs the event; never throws (Requirement 18.7-18.8).
  ExpansionResult Expand(const std::string& original_query,
                         const std::vector<std::string>& original_terms);

  // GET {host}/api/tags with a 1 s timeout, without acquiring the mutex
  // used by Expand() (Requirement 14.2-14.3). True if the server responded
  // and the model is present in the returned list. The model counts as
  // present when a listed name equals `model_` exactly, or equals
  // `model_ + ":latest"` (a configured name without a tag).
  bool IsAvailable() const;

  // Exact prompt template (Requirement 18.4).
  static std::string BuildPrompt(const std::string& original_query);

 private:
  std::string host_;
  std::string model_;
  std::chrono::milliseconds timeout_;
  mutable std::timed_mutex generate_mutex_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_OLLAMA_CLIENT_H_
