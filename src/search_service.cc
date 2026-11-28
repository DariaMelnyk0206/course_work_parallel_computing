#include "search_service.h"

#include <utility>

#include "index_builder.h"

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

// Stub: returns an empty expansion.
QueryExpansion SearchService::ExpandQuery(const std::string& query) {
  (void)query;
  return {};
}

// Stub: returns empty results.
KeywordSearchOutcome SearchService::RunKeywordSearch(
    const QueryExpansion& expansion) {
  (void)expansion;
  return {};
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
  std::lock_guard<std::mutex> lock(index_mutex_);
  index_ = std::move(next);
  stats_.documents = docs;
  stats_.index_build_time_ms = build_time_ms;
  stats_.threads_used = threads;
}

}  // namespace search_engine
