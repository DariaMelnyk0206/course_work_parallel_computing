#include "ranker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace search_engine {

namespace {

struct ScoredDoc {
  uint32_t doc_id;
  double score;
};

double RoundTo4Decimals(double value) { return std::round(value * 1e4) / 1e4; }

}  // namespace

std::vector<SearchResult> Ranker::Rank(const std::vector<WeightedTerm>& terms,
                                       const InvertedIndex& index, int top_k,
                                       size_t* total_matches) {
  if (total_matches != nullptr) *total_matches = 0;
  if (top_k <= 0 || terms.empty()) return {};

  const double document_count = static_cast<double>(index.DocumentCount());

  // Terms with DF > 0 and their precomputed weight * IDF. Duplicate terms are
  // counted once (Q is a set); the first occurrence wins.
  std::vector<double> term_weights;
  std::vector<std::vector<Posting>> postings;
  std::unordered_set<std::string> seen;
  for (const WeightedTerm& term : terms) {
    if (!seen.insert(term.term).second) continue;
    std::vector<Posting> list = index.Table().Lookup(term.term, term.hash);
    if (list.empty()) continue;  // DF = 0: excluded everywhere.
    const double idf =
        std::log(1.0 + document_count / static_cast<double>(list.size()));
    term_weights.push_back(term.weight * idf);
    postings.push_back(std::move(list));
  }

  // Same summation order as the per-document numerator, so a document
  // matching every term gets exactly 1.0.
  double denominator = 0.0;
  for (double weight : term_weights) denominator += weight;
  if (denominator <= 0.0) return {};

  std::unordered_map<uint32_t, double> numerators;
  for (size_t i = 0; i < postings.size(); ++i) {
    for (const Posting& posting : postings[i]) {
      numerators[posting.doc_id] += term_weights[i];
    }
  }

  if (total_matches != nullptr) *total_matches = numerators.size();
  std::vector<ScoredDoc> scored;
  scored.reserve(numerators.size());
  for (const auto& [doc_id, numerator] : numerators) {
    scored.push_back({doc_id, RoundTo4Decimals(numerator / denominator)});
  }

  auto better = [](const ScoredDoc& a, const ScoredDoc& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.doc_id < b.doc_id;
  };
  const size_t limit = std::min(scored.size(), static_cast<size_t>(top_k));
  std::partial_sort(scored.begin(), scored.begin() + limit, scored.end(),
                    better);

  std::vector<SearchResult> results;
  results.reserve(limit);
  for (size_t i = 0; i < limit; ++i) {
    const DocumentInfo& info = index.Documents().Get(scored[i].doc_id);
    results.push_back({info.name, scored[i].score, info.snippet});
  }
  return results;
}

}  // namespace search_engine
