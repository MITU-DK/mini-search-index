// Builds an InMemoryIndex from a packed corpus (DESIGN.md §4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "constants.hpp"
#include "inmemory_index.hpp"

namespace mse {

// "auto" gives the number of hardware threads (at least 1); otherwise a whole number >= 1.
// Throws std::invalid_argument for anything else.
unsigned resolve_threads(std::string_view arg);

struct BuildOptions {
  unsigned threads = 1;  // never more workers than there are batches
  std::size_t batch_size = kBatchSize;
  // 0: build everything in memory. Otherwise a bound on the postings the workers hold; see
  // build_to_file in build_pipeline.hpp, which is the only function that looks at these two.
  uint64_t memory_budget_bytes = 0;
  std::filesystem::path tmp_dir = {};  // where run files go; empty means next to the output file
  // Most run files open at once while merging. More runs than this are first merged in passes into
  // bigger runs. (It is also limited by what the process may open.)
  std::size_t max_merge_fan_in = 1024;
};

// Seconds spent in each phase, plus totals. Filled in by build_index when requested.
struct BuildStats {
  double scan_s = 0;          // finding the line boundaries of every doc
  double tokenize_s = 0;      // reading, tokenizing and counting
  double merge_s = 0;         // combining the workers' results into one index
  uint32_t threads_used = 1;  // workers that actually ran
  uint64_t docs = 0;
  uint64_t tokens = 0;
  uint64_t terms = 0;
  uint64_t postings = 0;
  uint32_t runs_spilled = 0;  // run files written (0 without a memory budget)
  uint64_t spill_bytes = 0;   // their total size
  uint32_t merge_passes =
      0;  // times the runs had to be merged into bigger runs before the last merge
};

// The result is the same for any thread count and batch size.
[[nodiscard]] std::expected<InMemoryIndex, std::string> build_index(
    const std::filesystem::path& corpus_root, const BuildOptions& opts,
    BuildStats* stats = nullptr);

}  // namespace mse
