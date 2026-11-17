#include "index.hpp"

namespace mse {
namespace {

using Source = std::variant<MappedFile, FileBuffer>;

std::expected<Source, std::string> open_source(const std::filesystem::path& path, LoadMode mode) {
  if (mode == LoadMode::Mmap) {
    auto mapped = MappedFile::open(path);
    if (!mapped) return std::unexpected(mapped.error());
    return Source(*std::move(mapped));
  }
  auto buffer = FileBuffer::open(path);
  if (!buffer) return std::unexpected(buffer.error());
  return Source(*std::move(buffer));
}

}  // namespace

std::expected<Index, std::string> Index::open(const std::filesystem::path& path, LoadMode mode) {
  auto source = open_source(path, mode);
  if (!source) return std::unexpected(source.error());

  const auto bytes = std::visit([](const auto& file) { return file.bytes(); }, *source);
  auto view = IndexView::open(bytes);
  if (!view) return std::unexpected(path.string() + ": " + view.error());
  return Index(*std::move(source), *view);
}

}  // namespace mse
