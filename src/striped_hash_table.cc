#include "striped_hash_table.h"

#include <algorithm>
#include <mutex>

namespace search_engine {

namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

// Rounds `value` up to the nearest power of two (minimum 1).
size_t RoundUpToPowerOfTwo(size_t value) {
  size_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

}  // namespace

StripedHashTable::StripedHashTable(size_t num_buckets, size_t num_stripes)
    : num_buckets_(RoundUpToPowerOfTwo(std::max<size_t>(num_buckets, 1))),
      num_stripes_(RoundUpToPowerOfTwo(std::max<size_t>(num_stripes, 1))),
      buckets_(),
      stripes_() {
  // B must be a multiple of S so that a stripe owns a disjoint set of buckets.
  num_stripes_ = std::min(num_stripes_, num_buckets_);
  buckets_.resize(num_buckets_);
  std::vector<std::shared_mutex> stripes(num_stripes_);
  stripes_.swap(stripes);
}

size_t StripedHashTable::BucketFor(uint64_t hash) const {
  return static_cast<size_t>(hash) & (num_buckets_ - 1);
}

size_t StripedHashTable::StripeFor(uint64_t hash) const {
  return BucketFor(hash) & (num_stripes_ - 1);
}

void StripedHashTable::InsertBatch(
    size_t stripe, const std::vector<StripedInsertItem>& items) {
  if (items.empty()) {
    return;
  }
  // The only stripe mutex held by this thread for the whole batch.
  std::unique_lock<std::shared_mutex> lock(stripes_[stripe]);
  for (const StripedInsertItem& item : items) {
    // Callers group items by StripeFor(hash); skip foreign items rather than
    // touching buckets guarded by a mutex we do not hold.
    if (StripeFor(item.hash) != stripe) {
      continue;
    }
    std::vector<Entry>& bucket = buckets_[BucketFor(item.hash)];
    auto it = std::find_if(bucket.begin(), bucket.end(),
                           [&](const Entry& e) { return e.term == item.term; });
    if (it == bucket.end()) {
      bucket.push_back(Entry{item.term, {item.doc_id}});
    } else if (it->postings.empty() || it->postings.back() != item.doc_id) {
      // A doc_id is added at most once per term by the caller (Requirement
      // 7.11); the back() check cheaply guards immediate repeats.
      it->postings.push_back(item.doc_id);
    }
  }
}

std::vector<uint32_t> StripedHashTable::Lookup(const std::string& term,
                                               uint64_t hash) const {
  std::shared_lock<std::shared_mutex> lock(stripes_[StripeFor(hash)]);
  const std::vector<Entry>& bucket = buckets_[BucketFor(hash)];
  for (const Entry& entry : bucket) {
    if (entry.term == term) {
      return entry.postings;  // Copy made under the shared lock.
    }
  }
  return {};
}

size_t StripedHashTable::UniqueTermCount() const {
  size_t total = 0;
  for (size_t stripe = 0; stripe < num_stripes_; ++stripe) {
    // One stripe at a time: the lock is released before the next iteration.
    std::shared_lock<std::shared_mutex> lock(stripes_[stripe]);
    for (size_t bucket = stripe; bucket < num_buckets_;
         bucket += num_stripes_) {
      total += buckets_[bucket].size();
    }
  }
  return total;
}

uint64_t StripedHashTable::Fnv1aHash64(const std::string& term) {
  uint64_t hash = kFnvOffsetBasis;
  for (unsigned char c : term) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

}  // namespace search_engine
