#ifndef SEARCH_ENGINE_SEARCH_SERVICE_H_
#define SEARCH_ENGINE_SEARCH_SERVICE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "inverted_index.h"
#include "ollama_client.h"
#include "ranker.h"

namespace search_engine {

struct IndexStats {
  uint32_t documents = 0;
  int64_t index_build_time_ms = 0;
  int threads_used = 0;
};

// Outcome codes so ApiServer can map them to HTTP statuses without
// SearchService depending on httplib.
enum class IndexOutcome { kOk, kAlreadyInProgress };

// Result of the first pipeline stage (Requirement 15.1-15.3, 19.3).
struct QueryExpansion {
  std::vector<std::string> original_terms;  // Unique, in order of appearance.
  // From the LLM only, at most 10, already free of original_terms
  // (OllamaClient::Expand removes them before applying the limit).
  std::vector<std::string> new_terms;
  std::vector<std::string> expanded_terms;   // original_terms + new_terms.
  std::vector<WeightedTerm> weighted_terms;  // Weight 2 original, 1 new.
  bool used_fallback = false;
};

// Result of the second pipeline stage (Requirement 19.4).
struct KeywordSearchOutcome {
  std::vector<SearchResult> results;
  size_t total_matches = 0;    // Documents with score > 0 before the top-k cut.
  int64_t search_time_ms = 0;  // Lookup + ranking + KWIC windows.
};

// Facade over indexing, search, and stats. Owns the current index snapshot
// and the indexing-in-progress flag.
class SearchService {
 public:
  explicit SearchService(Config config);

  // Builds a new index with `num_threads` workers and publishes it on
  // success (Requirement 9, 11). Returns kAlreadyInProgress without
  // touching the current snapshot or timer if a build is already running
  // (Requirement 10.4-10.5); the flag is claimed via
  // std::atomic<bool>::compare_exchange_strong (Requirement 8.5, 10.4).
  // The flag is owned by an RAII guard (see IndexingGuard below), so it is
  // reset on every exit path, including an exception thrown by the build.
  IndexOutcome BuildIndex(int num_threads, uint32_t* documents_indexed,
                          int64_t* time_ms);

  // Stage 1 of the shared pipeline: Tokenizer -> OllamaClient::Expand
  // (Requirement 15.1-15.3, 15.8). original_terms is computed first and
  // passed to Expand(), which removes them from the LLM terms before the
  // limit of 10; ExpandQuery does no further filtering, so
  // expanded_terms = original_terms + expansion.new_terms. Releases the LLM
  // queue mutex before returning and touches no index lock
  // (Requirement 15.7). Never fails validation on a query with zero original
  // terms (Requirement 15.9-15.10). The WebSocket handler sends
  // `query_expansion` right after this call.
  QueryExpansion ExpandQuery(const std::string& query);

  // Stage 2: InvertedIndex lookups -> Ranker on the current snapshot
  // (Requirement 15.4-15.6). Returns empty results for an empty snapshot or
  // empty expansion. The WebSocket handler sends `keyword_search` with
  // `search_time_ms` right after this call. REST calls ExpandQuery and
  // RunKeywordSearch sequentially and reports the total as `time_ms`
  // (Requirement 17.3).
  KeywordSearchOutcome RunKeywordSearch(const QueryExpansion& expansion);

  // For each ranked candidate (top-k only) opens dataset_dir/relative_path
  // in a separate stream, reads a window around the anchor offset and builds
  // the KWIC snippet via BuildKwicSnippet; on std::nullopt (file missing,
  // shorter, or the anchor term is not at the offset) the document's static
  // snippet is used (Requirement 12.6-12.10). Performs no locking: file
  // reads happen outside every lock (Requirement 12.7).
  static std::vector<SearchResult> AttachSnippets(
      const std::string& dataset_dir, const InvertedIndex& index,
      const std::vector<RankedDocument>& ranked);

  IndexStats Stats() const;

  // True if Ollama responded to /api/tags and the configured model is
  // present (Requirement 14.1-14.2).
  bool OllamaAvailable() const;

 private:
  std::shared_ptr<const InvertedIndex> CurrentIndex() const;
  void PublishIndex(std::shared_ptr<const InvertedIndex> next, uint32_t docs,
                    int64_t build_time_ms, int threads);

  Config config_;
  OllamaClient ollama_client_;

  mutable std::mutex index_mutex_;  // Guards snapshot pointer + stats only.
  std::shared_ptr<const InvertedIndex> index_;
  IndexStats stats_;

  // RAII guard: claims indexing_in_progress_ with compare_exchange_strong in
  // its factory and resets the flag in its destructor, so the flag is
  // released even if the build throws (Requirement 10.4).
  class IndexingGuard;

  std::atomic<bool> indexing_in_progress_{false};
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_SEARCH_SERVICE_H_
