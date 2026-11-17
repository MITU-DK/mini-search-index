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
  unsigned threads = 1;
  std::size_t batch_size = kBatchSize;
};

// Seconds spent in each phase, plus totals. Filled in by build_index when requested.
struct BuildStats {
  double scan_s = 0;      // finding the line boundaries of every doc
  double tokenize_s = 0;  // reading, tokenizing and counting
  double merge_s = 0;     // combining per-thread results (0 until the build is parallel)
  uint64_t docs = 0;
  uint64_t tokens = 0;
  uint64_t terms = 0;
  uint64_t postings = 0;
};

// The result is the same for any thread count and batch size.
// Today every build runs on one thread; `opts.threads` is not used yet.
[[nodiscard]] std::expected<InMemoryIndex, std::string> build_index(
    const std::filesystem::path& corpus_root, const BuildOptions& opts,
    BuildStats* stats = nullptr);

}  // namespace mse
