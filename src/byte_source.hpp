// Two ways to get a whole file as bytes: map it (pages are loaded as they are touched) or read it
// into memory (everything is loaded up front). Both are read-only and move-only.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace mse {

// mmap(PROT_READ, MAP_PRIVATE). If another process shortens the file while it is mapped, touching
// the missing pages kills this process with SIGBUS; indexes are written once and replaced by
// rename, never edited in place, so this does not happen in normal use.
class MappedFile {
 public:
  // A file of 0 bytes is an error, because mmap cannot map it.
  [[nodiscard]] static std::expected<MappedFile, std::string> open(
      const std::filesystem::path& path);

  ~MappedFile();
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  std::span<const std::byte> bytes() const { return {data_, size_}; }

 private:
  MappedFile(const std::byte* data, std::size_t size) : data_(data), size_(size) {}
  void release();

  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

// The whole file, read with read() into a heap buffer.
class FileBuffer {
 public:
  [[nodiscard]] static std::expected<FileBuffer, std::string> open(
      const std::filesystem::path& path);

  FileBuffer(FileBuffer&& other) noexcept;
  FileBuffer& operator=(FileBuffer&& other) noexcept;
  FileBuffer(const FileBuffer&) = delete;
  FileBuffer& operator=(const FileBuffer&) = delete;
  ~FileBuffer() = default;

  std::span<const std::byte> bytes() const { return {data_.get(), size_}; }

 private:
  FileBuffer(std::unique_ptr<std::byte[]> data, std::size_t size)
      : data_(std::move(data)), size_(size) {}

  std::unique_ptr<std::byte[]> data_;
  std::size_t size_ = 0;
};

// Asks the kernel to drop the file's pages from the page cache, so that the next read comes from
// disk (a cold start). Dirty pages are written out first, because only clean pages can be dropped.
[[nodiscard]] std::expected<void, std::string> evict_from_page_cache(
    const std::filesystem::path& path);

}  // namespace mse
