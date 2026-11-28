#ifndef SEARCH_ENGINE_STRIPED_HASH_TABLE_H_
#define SEARCH_ENGINE_STRIPED_HASH_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

namespace search_engine {

// A single term's postings, grouped by the stripe its bucket falls into.
// Produced by the caller before calling InsertBatch so that all terms
// landing in one stripe are inserted under a single lock acquisition
// (Requirement 7.8, 9.4).
struct StripedInsertItem {
  std::string term;
  uint64_t hash;
  uint32_t doc_id;
};

// Own striped hash table mapping term -> sorted-on-read list of doc_id.
// Fixed bucket count (power of two, default 2^21), no rehashing
// (Requirement 7.3). Separate chaining: each bucket is
// std::vector<Entry>. A fixed array of std::shared_mutex (power of two,
// default 1024) guards disjoint groups of buckets; a thread never holds
// more than one stripe mutex at a time (Requirement 7.5, 7.10).
class StripedHashTable {
 public:
  struct Entry {
    std::string term;
    std::vector<uint32_t> postings;
  };

  explicit StripedHashTable(size_t num_buckets = (1u << 21),
                            size_t num_stripes = 1024);

  // Computes the stripe index for a precomputed FNV-1a 64-bit hash
  // (Requirement 7.2, 7.4): bucket = hash & (num_buckets - 1);
  // stripe = bucket & (num_stripes - 1).
  size_t StripeFor(uint64_t hash) const;

  // Inserts every item whose StripeFor(hash) equals `stripe` under one
  // std::unique_lock on that stripe (Requirement 7.6, 7.8). Each (term,
  // doc_id) pair is inserted at most once per call; IndexBuilder guarantees
  // doc_id appears at most once per term overall (Requirement 7.11).
  void InsertBatch(size_t stripe, const std::vector<StripedInsertItem>& items);

  // Returns a copy of the postings list for `term`/`hash`, taken under a
  // std::shared_lock so concurrent readers proceed in parallel
  // (Requirement 7.7, 7.9). Returns an empty vector if the term is absent.
  std::vector<uint32_t> Lookup(const std::string& term, uint64_t hash) const;

  // Number of distinct terms currently stored (Requirement 7.12). Not a
  // running counter: IndexBuilder calls it once after all tasks have
  // finished. It walks the stripes one at a time, taking a std::shared_lock
  // on each and releasing it before moving on, so the rule "at most one
  // stripe mutex per thread" (Requirement 7.10) is kept.
  size_t UniqueTermCount() const;

  // Own FNV-1a 64-bit hash, computed once per term by callers
  // (Requirement 7.2).
  static uint64_t Fnv1aHash64(const std::string& term);

 private:
  size_t BucketFor(uint64_t hash) const;

  size_t num_buckets_;
  size_t num_stripes_;
  std::vector<std::vector<Entry>> buckets_;
  mutable std::vector<std::shared_mutex> stripes_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_STRIPED_HASH_TABLE_H_
