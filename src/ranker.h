#ifndef SEARCH_ENGINE_RANKER_H_
#define SEARCH_ENGINE_RANKER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "inverted_index.h"

namespace search_engine {

// A single query term with its origin-based weight (Requirement 16.3):
// weight 2 for original query terms, weight 1 for LLM-added terms.
struct WeightedTerm {
  std::string term;
  uint64_t hash;
  double weight;
};

struct SearchResult {
  std::string doc_id;  // File name (Requirement 17.2).
  double score;        // Rounded to 4 decimals, in (0, 1] (Requirement 16.6).
  std::string snippet;
};

// Computes score(d, Q) = sum(w(t)*IDF(t) for t matching d) /
// sum(w(t)*IDF(t) for all t with DF(t)>0), IDF(t) = ln(1 + N/DF(t))
// (Requirement 16.1-16.2, PD Section 8.2). Accumulates per-call state only
// -- no shared state between concurrent requests (Requirement 15.6).
class Ranker {
 public:
  // Returns at most `top_k` results sorted by score descending, ties broken
  // by doc_id ascending (Requirement 16.7). Returns an empty vector if the
  // weighted denominator is zero (Requirement 16.5). If `total_matches` is
  // not null, it receives the number of documents with score > 0 before the
  // top-k cut (the size of the already computed candidate set; 0 when the
  // result is empty). It is shown by the WebSocket stage only (R19.4).
  static std::vector<SearchResult> Rank(const std::vector<WeightedTerm>& terms,
                                        const InvertedIndex& index, int top_k,
                                        size_t* total_matches = nullptr);
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_RANKER_H_
