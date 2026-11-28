#ifndef SEARCH_ENGINE_DOCUMENT_TABLE_H_
#define SEARCH_ENGINE_DOCUMENT_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace search_engine {

struct DocumentInfo {
  std::string name;           // File name used as doc_id in API responses.
  std::string relative_path;  // Path relative to the dataset directory.
  std::string title;          // "<title> (<year>)" from metadata.csv or empty.
  std::string snippet;        // Precomputed at index time (Requirement 12).
  bool valid = false;         // False if the file could not be read.
};

// Metadata for all documents, indexed by uint32_t doc_id. The vector is
// sized before any task starts; each indexing task writes only to its own
// slot, so no lock is needed (Requirement 8.1, 8.2).
class DocumentTable {
 public:
  explicit DocumentTable(size_t document_count);

  void Set(uint32_t doc_id, DocumentInfo info);
  const DocumentInfo& Get(uint32_t doc_id) const;
  size_t Size() const;

 private:
  std::vector<DocumentInfo> documents_;
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_DOCUMENT_TABLE_H_
