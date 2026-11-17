// An index file opened for searching: owns the bytes (mapped or read) and the view over them.
#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <variant>

#include "byte_source.hpp"
#include "index_view.hpp"

namespace mse {

enum class LoadMode { Mmap, Read };

class Index {
 public:
  [[nodiscard]] static std::expected<Index, std::string> open(const std::filesystem::path& path,
                                                              LoadMode mode);

  // Moving keeps the view valid: the bytes stay where they are (a mapping, or a heap buffer).
  Index(Index&&) noexcept = default;
  Index& operator=(Index&&) noexcept = default;

  const IndexView& view() const { return view_; }

 private:
  Index(std::variant<MappedFile, FileBuffer> source, IndexView view)
      : source_(std::move(source)), view_(view) {}

  std::variant<MappedFile, FileBuffer> source_;
  IndexView view_;
};

}  // namespace mse
