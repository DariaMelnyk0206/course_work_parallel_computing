#ifndef SEARCH_ENGINE_INDEX_BUILDER_H_
#define SEARCH_ENGINE_INDEX_BUILDER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "inverted_index.h"

namespace search_engine {

struct IndexBuildResult {
  std::shared_ptr<const InvertedIndex> index;
  uint32_t documents_indexed = 0;
  int64_t time_ms = 0;
};

// Scans a directory, builds an InvertedIndex using `num_threads` workers in
// a dedicated ThreadPool (Requirement 9.2-9.3), and measures wall-clock
// time with std::chrono::steady_clock (Requirement 9.6). Exposed as a
// standalone function (rather than requiring a dataset) so the TSan driver
// can point it at a synthetic corpus directory (Requirement 28.4-28.5).
class IndexBuilder {
 public:
  // `dataset_dir`: recursively scanned for "*.txt", sorted lexicographically
  // by relative path for deterministic doc_id assignment (Requirement 2.9).
  // Files that fail to read are skipped and logged, not counted
  // (Requirement 2.11). An absent or empty directory is not an error: the
  // result has documents_indexed == 0 (Requirement 2.10).
  static IndexBuildResult Build(const std::string& dataset_dir,
                                int num_threads);

  // One metadata.csv row `id,title,year,url` (no header).
  struct MetadataRow {
    std::string id;
    std::string title;
    std::string year;
  };

  // `id` is the text up to the first comma, `url` the text after the last
  // comma, `year` the text between the second-to-last and the last comma,
  // `title` everything in the middle, so commas inside a title do not break
  // parsing (Requirement 9.11). Returns std::nullopt for a row with fewer
  // than three commas (the row is skipped and logged by the caller).
  static std::optional<MetadataRow> ParseMetadataLine(const std::string& line);
};

}  // namespace search_engine

#endif  // SEARCH_ENGINE_INDEX_BUILDER_H_
