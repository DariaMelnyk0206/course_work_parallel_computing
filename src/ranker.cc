#include "ranker.h"

namespace search_engine {

// Stub: the scoring formula is implemented in a later task.
std::vector<SearchResult> Ranker::Rank(const std::vector<WeightedTerm>& terms,
                                       const InvertedIndex& index, int top_k) {
  (void)terms;
  (void)index;
  (void)top_k;
  return {};
}

}  // namespace search_engine
