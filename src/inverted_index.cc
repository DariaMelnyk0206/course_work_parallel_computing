#include "inverted_index.h"

#include <utility>

namespace search_engine {

InvertedIndex::InvertedIndex(StripedHashTable table, DocumentTable documents,
                             uint32_t document_count)
    : table_(std::move(table)),
      documents_(std::move(documents)),
      document_count_(document_count) {}

const StripedHashTable& InvertedIndex::Table() const { return table_; }

const DocumentTable& InvertedIndex::Documents() const { return documents_; }

uint32_t InvertedIndex::DocumentCount() const { return document_count_; }

}  // namespace search_engine
