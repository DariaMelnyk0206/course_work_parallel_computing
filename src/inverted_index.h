#ifndef SEARCH_ENGINE_INVERTED_INDEX_H_
#define SEARCH_ENGINE_INVERTED_INDEX_H_

#include <cstdint>
#include <memory>

#include "document_table.h"
#include "striped_hash_table.h"

namespace search_engine {

// Immutable snapshot published after a build completes
// (Requirement 11.1-11.2). Always accessed through
// std::shared_ptr<const InvertedIndex> so a search in flight keeps working
// against the old snapshot during a rebuild (Requirement 11.4).
class InvertedIndex {
 public:
  InvertedIndex(StripedHashTable table, DocumentTable documents,
                uint32_t document_count);

  const StripedHashTable& Table() const;
  const DocumentTable& Documents() const;
  uint32_t DocumentCount() const;

 private:
  StripedHashTable table_;
  DocumentTable documents_;
  uint32_t document_count_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_INVERTED_INDEX_H_
