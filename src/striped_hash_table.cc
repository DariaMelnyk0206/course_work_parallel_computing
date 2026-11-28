#include "striped_hash_table.h"

namespace search_engine {

// Stubs: real implementation is added in a later task.
StripedHashTable::StripedHashTable(size_t num_buckets, size_t num_stripes)
    : num_buckets_(num_buckets), num_stripes_(num_stripes) {}

size_t StripedHashTable::StripeFor(uint64_t /*hash*/) const { return 0; }

void StripedHashTable::InsertBatch(
    size_t /*stripe*/, const std::vector<StripedInsertItem>& /*items*/) {}

std::vector<uint32_t> StripedHashTable::Lookup(const std::string& /*term*/,
                                               uint64_t /*hash*/) const {
  return {};
}

size_t StripedHashTable::UniqueTermCount() const { return 0; }

uint64_t StripedHashTable::Fnv1aHash64(const std::string& /*term*/) {
  return 0;
}

size_t StripedHashTable::BucketFor(uint64_t /*hash*/) const { return 0; }

}  // namespace search_engine
