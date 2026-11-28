#include "document_table.h"

#include <utility>

namespace search_engine {

// Stubs: real implementation is added in a later task.
DocumentTable::DocumentTable(size_t document_count)
    : documents_(document_count) {}

void DocumentTable::Set(uint32_t /*doc_id*/, DocumentInfo /*info*/) {}

const DocumentInfo& DocumentTable::Get(uint32_t /*doc_id*/) const {
  static const DocumentInfo kEmpty;
  return kEmpty;
}

size_t DocumentTable::Size() const { return 0; }

}  // namespace search_engine
