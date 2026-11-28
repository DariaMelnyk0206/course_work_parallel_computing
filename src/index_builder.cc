#include "index_builder.h"

namespace search_engine {

// Stub: the parallel build is implemented in a later task.
IndexBuildResult IndexBuilder::Build(const std::string& dataset_dir,
                                     int num_threads) {
  (void)dataset_dir;
  (void)num_threads;
  return {};
}

}  // namespace search_engine
