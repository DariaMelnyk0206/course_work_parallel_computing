#include "search_service.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <cstddef>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <utility>

#include "index_builder.h"
#include "logger.h"
#include "snippet.h"
#include "striped_hash_table.h"
#include "tokenizer.h"

namespace search_engine {

SearchService::SearchService(Config config)
    : config_(std::move(config)),
      ollama_client_(config_.ollama_host, config_.ollama_model,
                     std::chrono::milliseconds(config_.llm_timeout_ms)) {}

// Claims the indexing flag with compare_exchange_strong and releases it in
// the destructor, so the flag is reset on every exit path (Requirement 10.4).
class SearchService::IndexingGuard {
 public:
  explicit IndexingGuard(std::atomic<bool>& flag) : flag_(flag) {
    bool expected = false;
    acquired_ = flag_.compare_exchange_strong(expected, true);
  }
  ~IndexingGuard() {
    if (acquired_) flag_.store(false);
  }
  IndexingGuard(const IndexingGuard&) = delete;
  IndexingGuard& operator=(const IndexingGuard&) = delete;

  bool acquired() const { return acquired_; }

 private:
  std::atomic<bool>& flag_;
  bool acquired_ = false;
};

IndexOutcome SearchService::BuildIndex(int num_threads,
                                       uint32_t* documents_indexed,
                                       int64_t* time_ms) {
  IndexingGuard guard(indexing_in_progress_);
  if (!guard.acquired()) return IndexOutcome::kAlreadyInProgress;

  // The new index is built apart from the current one; searches keep using
  // the old snapshot until PublishIndex swaps the pointer.
  IndexBuildResult result =
      IndexBuilder::Build(config_.dataset_dir, num_threads);
  PublishIndex(std::move(result.index), result.documents_indexed,
               result.time_ms, num_threads);
  if (documents_indexed != nullptr)
    *documents_indexed = result.documents_indexed;
  if (time_ms != nullptr) *time_ms = result.time_ms;
  return IndexOutcome::kOk;
}

QueryExpansion SearchService::ExpandQuery(const std::string& query) {
  QueryExpansion expansion;

  // Unique original terms in order of appearance.
  std::unordered_set<std::string> seen;
  for (std::string& token : Tokenizer::Tokenize(query)) {
    if (seen.insert(token).second)
      expansion.original_terms.push_back(std::move(token));
  }

  if (config_.expansion_enabled) {
    // No index lock is held while waiting for the LLM (Requirement 15.7).
    ExpansionResult llm =
        ollama_client_.Expand(query, expansion.original_terms);
    expansion.new_terms = std::move(llm.new_terms);
    expansion.used_fallback = llm.used_fallback;
  }

  expansion.expanded_terms = expansion.original_terms;
  expansion.expanded_terms.insert(expansion.expanded_terms.end(),
                                  expansion.new_terms.begin(),
                                  expansion.new_terms.end());

  expansion.weighted_terms.reserve(expansion.expanded_terms.size());
  for (size_t i = 0; i < expansion.expanded_terms.size(); ++i) {
    const std::string& term = expansion.expanded_terms[i];
    const double weight = i < expansion.original_terms.size() ? 2.0 : 1.0;
    expansion.weighted_terms.push_back(
        {term, StripedHashTable::Fnv1aHash64(term), weight});
  }
  return expansion;
}

KeywordSearchOutcome SearchService::RunKeywordSearch(
    const QueryExpansion& expansion) {
  KeywordSearchOutcome outcome;
  const auto start = std::chrono::steady_clock::now();
  std::shared_ptr<const InvertedIndex> index = CurrentIndex();
  if (index != nullptr && !expansion.weighted_terms.empty()) {
    const std::vector<RankedDocument> ranked =
        Ranker::Rank(expansion.weighted_terms, *index, config_.top_k,
                     &outcome.total_matches);
    outcome.results = AttachSnippets(config_.dataset_dir, *index, ranked);
  }
  outcome.search_time_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  return outcome;
}

std::vector<SearchResult> SearchService::AttachSnippets(
    const std::string& dataset_dir, const InvertedIndex& index,
    const std::vector<RankedDocument>& ranked) {
  std::vector<SearchResult> results;
  results.reserve(ranked.size());
  for (const RankedDocument& doc : ranked) {
    const DocumentInfo& info = index.Documents().Get(doc.doc_index);
    const std::string path =
        (std::filesystem::path(dataset_dir) / info.relative_path).string();
    std::optional<std::string> kwic =
        BuildKwicSnippet(path, doc.anchor_offset, doc.anchor_term);
    if (!kwic.has_value()) {
      Logger::Instance().Info("KWIC snippet unavailable for " + info.name +
                              ", using static snippet");
    }
    results.push_back({info.name, doc.score,
                       kwic.has_value() ? std::move(*kwic) : info.snippet,
                       info.title});
  }
  return results;
}

IndexStats SearchService::Stats() const {
  std::lock_guard<std::mutex> lock(index_mutex_);
  return stats_;
}

bool SearchService::OllamaAvailable() const {
  return ollama_client_.IsAvailable();
}

std::shared_ptr<const InvertedIndex> SearchService::CurrentIndex() const {
  std::lock_guard<std::mutex> lock(index_mutex_);
  return index_;
}

void SearchService::PublishIndex(std::shared_ptr<const InvertedIndex> next,
                                 uint32_t docs, int64_t build_time_ms,
                                 int threads) {
  std::shared_ptr<const InvertedIndex> old_index;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    old_index = std::move(index_);
    index_ = std::move(next);
    stats_.documents = docs;
    stats_.index_build_time_ms = build_time_ms;
    stats_.threads_used = threads;
  }
  // The lock is released; the old index (a large destructor) is freed here,
  // outside the mutex, unless a running search still holds a snapshot.
  old_index.reset();
#ifdef __GLIBC__
  // Returns memory of the old index to the operating system: glibc does not
  // give back freed memory from per-thread arenas on its own.
  malloc_trim(0);
#endif
}

}  // namespace search_engine
