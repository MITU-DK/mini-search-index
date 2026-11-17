// Test helper: builds an index from a corpus directory, saves it, and opens a view over its bytes.
#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>

#include "file_bytes.hpp"
#include "index_view.hpp"
#include "index_writer.hpp"
#include "indexer.hpp"
#include "temp_dir.hpp"

namespace mse {

// Not movable: `view` points into `bytes`.
struct SavedIndex {
  TempDir dir;
  InMemoryIndex memory;
  Bytes bytes;
  std::optional<IndexView> view;
};

inline std::unique_ptr<SavedIndex> save_index(const std::filesystem::path& corpus,
                                              const WriteOptions& options = {}) {
  auto saved = std::make_unique<SavedIndex>();
  auto built = build_index(corpus, {.threads = 2});
  if (!built) throw std::runtime_error(built.error());
  saved->memory = *std::move(built);
  const auto path = saved->dir.path() / "index.bin";
  const auto written = write_index(saved->memory, path, options);
  if (!written) throw std::runtime_error(written.error());
  saved->bytes = read_file(path);
  auto view = IndexView::open(saved->bytes);
  if (!view) throw std::runtime_error(view.error());
  saved->view = *view;
  return saved;
}

}  // namespace mse
