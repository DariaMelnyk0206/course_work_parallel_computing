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

// A ranked candidate with its anchor (Requirement 16.8): the offset of the
// query term with the largest w(t)*IDF(t) among the terms present in the
// document. On equal weights the term that comes earlier in expanded_terms
// wins, so original terms take priority over LLM terms.
struct RankedDocument {
  uint32_t doc_index;       // Position in the sorted file list (doc_id).
  double score;             // Rounded to 4 decimals, in (0, 1] (R16.6).
  uint32_t anchor_offset;   // Offset from the anchor term's Posting.
  std::string anchor_term;  // Normalized anchor term, used for verification.
};

struct SearchResult {
  std::string doc_id;  // File name (Requirement 17.2).
  double score;        // Rounded to 4 decimals, in (0, 1] (Requirement 16.6).
  std::string snippet;
  std::string title;  // Book title from metadata.csv; may be empty.
};

// Computes score(d, Q) = sum(w(t)*IDF(t) for t matching d) /
// sum(w(t)*IDF(t) for all t with DF(t)>0), IDF(t) = ln(1 + N/DF(t))
// (Requirement 16.1-16.2, PD Section 8.2). Accumulates per-call state only
// -- no shared state between concurrent requests (Requirement 15.6).
class Ranker {
 public:
  // Returns at most `top_k` candidates sorted by score descending, ties
  // broken by doc_index ascending (Requirement 16.7); doc_index is the
  // position in the sorted relative-path list. The terms must be passed in
  // expanded_terms order: the position in `terms` is the tie-break for the
  // anchor (Requirement 16.8). The anchor is accumulated in the same
  // query-local structure as the score, so no state is shared between
  // queries (Requirement 15.6). Returns an empty vector if the
  // weighted denominator is zero (Requirement 16.5). If `total_matches` is
  // not null, it receives the number of documents with score > 0 before the
  // top-k cut (the size of the already computed candidate set; 0 when the
  // result is empty). It is shown by the WebSocket stage only (R19.4).
  static std::vector<RankedDocument> Rank(
      const std::vector<WeightedTerm>& terms, const InvertedIndex& index,
      int top_k, size_t* total_matches = nullptr);
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_RANKER_H_
