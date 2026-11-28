#include "search_service.h"

#include <utility>

namespace search_engine {

SearchService::SearchService(Config config)
    : config_(std::move(config)),
      ollama_client_(config_.ollama_host, config_.ollama_model,
                     std::chrono::milliseconds(config_.llm_timeout_ms)) {}

// Stub: indexing, the guard and publication are implemented later.
IndexOutcome SearchService::BuildIndex(int num_threads,
                                       uint32_t* documents_indexed,
                                       int64_t* time_ms) {
  (void)num_threads;
  if (documents_indexed != nullptr) *documents_indexed = 0;
  if (time_ms != nullptr) *time_ms = 0;
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
