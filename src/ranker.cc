#include "ranker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace search_engine {

namespace {

// Per-query accumulator (local to one Rank call).
struct Accumulator {
  double numerator = 0.0;
  double anchor_weight = -1.0;
  uint32_t anchor_offset = 0;
  size_t anchor_term_index = 0;  // Index into the filtered term list.
};

struct ScoredDoc {
  uint32_t doc_id;
  double score;
  uint32_t anchor_offset;
  size_t anchor_term_index;
};

double RoundTo4Decimals(double value) { return std::round(value * 1e4) / 1e4; }

}  // namespace

std::vector<RankedDocument> Ranker::Rank(const std::vector<WeightedTerm>& terms,
                                         const InvertedIndex& index, int top_k,
                                         size_t* total_matches) {
  if (total_matches != nullptr) *total_matches = 0;
  if (top_k <= 0 || terms.empty()) return {};

  const double document_count = static_cast<double>(index.DocumentCount());

  // Terms with DF > 0 and their precomputed weight * IDF. Duplicate terms are
  // counted once (Q is a set); the first occurrence wins.
  std::vector<double> term_weights;
  std::vector<std::vector<Posting>> postings;
  std::vector<const std::string*> term_names;
  std::unordered_set<std::string> seen;
  for (const WeightedTerm& term : terms) {
    if (!seen.insert(term.term).second) continue;
    std::vector<Posting> list = index.Table().Lookup(term.term, term.hash);
    if (list.empty()) continue;  // DF = 0: excluded everywhere.
    const double idf =
        std::log(1.0 + document_count / static_cast<double>(list.size()));
    term_weights.push_back(term.weight * idf);
    postings.push_back(std::move(list));
    term_names.push_back(&term.term);
  }

  // Same summation order as the per-document numerator, so a document
  // matching every term gets exactly 1.0.
  double denominator = 0.0;
  for (double weight : term_weights) denominator += weight;
  if (denominator <= 0.0) return {};

  // Terms are visited in expanded_terms order; the anchor is replaced only
  // on a strictly larger weight, so on a tie the earlier term wins.
  std::unordered_map<uint32_t, Accumulator> accumulators;
  for (size_t i = 0; i < postings.size(); ++i) {
    for (const Posting& posting : postings[i]) {
      Accumulator& acc = accumulators[posting.doc_id];
      acc.numerator += term_weights[i];
      if (term_weights[i] > acc.anchor_weight) {
        acc.anchor_weight = term_weights[i];
        acc.anchor_offset = posting.offset;
        acc.anchor_term_index = i;
      }
    }
  }

  if (total_matches != nullptr) *total_matches = accumulators.size();
  std::vector<ScoredDoc> scored;
  scored.reserve(accumulators.size());
  for (const auto& [doc_id, acc] : accumulators) {
    scored.push_back({doc_id, RoundTo4Decimals(acc.numerator / denominator),
                      acc.anchor_offset, acc.anchor_term_index});
  }

  auto better = [](const ScoredDoc& a, const ScoredDoc& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
  };
  const size_t limit = std::min(scored.size(), static_cast<size_t>(top_k));
  std::partial_sort(scored.begin(), scored.begin() + limit, scored.end(),
                    better);

  std::vector<RankedDocument> results;
  results.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    results.push_back({scored[i].doc_id, scored[i].score,
                       scored[i].anchor_offset,
                       *term_names[scored[i].anchor_term_index]});
  }
  return results;
}

}  // namespace search_engine
