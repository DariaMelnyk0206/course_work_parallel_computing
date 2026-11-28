#include "document_table.h"

#include <utility>

namespace search_engine {

DocumentTable::DocumentTable(size_t document_count)
    : documents_(document_count) {}

void DocumentTable::Set(uint32_t doc_id, DocumentInfo info) {
  // Each task owns its slot exclusively, so no lock is needed.
  documents_.at(doc_id) = std::move(info);
}

const DocumentInfo& DocumentTable::Get(uint32_t doc_id) const {
  return documents_.at(doc_id);
}

size_t DocumentTable::Size() const { return documents_.size(); }

}  // namespace search_engine
