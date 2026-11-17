// Reads the packed corpus: part-*.tsv files with one "title<TAB>text" article per line
// (DESIGN.md §4). Doc id = line number across all parts, in part-name order.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"

namespace mse {

// Where one article's line lives: part index, and byte range inside that part (without '\n').
struct DocRef {
  uint32_t part;
  uint32_t length;
  uint64_t offset;
  auto operator<=>(const DocRef&) const = default;
};

// A run of consecutive docs inside a single part. Workers take whole batches.
struct Batch {
  uint32_t first_doc;
  uint32_t count;
};

struct Corpus {
  std::filesystem::path root;           // absolute
  std::vector<std::string> part_names;  // sorted; DocRef::part indexes this
  std::vector<DocRef> docs;             // index = doc id
  std::vector<Batch> batches;
};

// Scans every part-*.tsv in `root` for line boundaries. An empty directory gives an empty corpus.
// Errors: root is not a directory, batch_size is 0, a part can't be read, or a line is >= 4 GiB.
[[nodiscard]] std::expected<Corpus, std::string> open_corpus(const std::filesystem::path& root,
                                                             std::size_t batch_size = kBatchSize);

struct DocText {
  std::string_view title;
  std::string_view text;
};

// Splits at the first TAB. A line with no TAB is all title.
DocText split_line(std::string_view line);

// Reads exactly `length` bytes at `offset`. A range that is not fully inside the file is an error.
// Safe to call from several threads at once.
[[nodiscard]] std::expected<std::string, std::string> read_range(const std::filesystem::path& file,
                                                                 uint64_t offset,
                                                                 std::size_t length);

}  // namespace mse
