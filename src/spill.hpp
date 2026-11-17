// Run files: sorted postings that a build writes to disk when it runs out of memory budget, and
// reads back in one k-way merge (DESIGN.md §4, "Memory bound").
//
// A run file is a series of records in strictly increasing term order, with no header:
//   u32 term_len | term bytes | u32 count | count x { u32 doc, u32 tf }
// Numbers are little-endian. The postings of one record are in increasing doc order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "posting.hpp"

namespace mse {

inline constexpr std::size_t kRunBufferBytes = 1 << 20;  // 1 MiB of buffered I/O per open run

struct RunRecord {
  std::string term;
  std::vector<Posting> postings;
  bool operator==(const RunRecord&) const = default;
};

class RunWriter {
 public:
  // Creates (or truncates) `path`.
  [[nodiscard]] static std::expected<RunWriter, std::string> create(
      const std::filesystem::path& path);

  // Terms must be non-empty and strictly increasing, and `postings` non-empty and strictly
  // increasing by doc. A rejected record changes nothing.
  [[nodiscard]] std::expected<void, std::string> write(std::string_view term,
                                                       std::span<const Posting> postings);

  // Writes out what is buffered and closes the file. Returns the size of the file in bytes.
  [[nodiscard]] std::expected<uint64_t, std::string> finish();

  ~RunWriter();
  RunWriter(RunWriter&& other) noexcept;
  RunWriter& operator=(RunWriter&& other) noexcept;
  RunWriter(const RunWriter&) = delete;
  RunWriter& operator=(const RunWriter&) = delete;

 private:
  RunWriter(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}
  [[nodiscard]] std::expected<void, std::string> flush();
  void close_quietly();

  int fd_ = -1;
  std::filesystem::path path_;
  std::vector<std::byte> buffer_;
  uint64_t bytes_written_ = 0;
  std::string last_term_;
  bool wrote_any_ = false;
};

class RunReader {
 public:
  // `buffer_bytes` is the size of the read buffer; more runs open at once want smaller buffers.
  [[nodiscard]] static std::expected<RunReader, std::string> open(
      const std::filesystem::path& path, std::size_t buffer_bytes = kRunBufferBytes);

  // Reads the next record into `out`, reusing its memory. Returns false at the end of the file.
  // A file that is cut short, has a bad record, or whose terms are not strictly increasing is an
  // error.
  [[nodiscard]] std::expected<bool, std::string> read(RunRecord& out);

  // The same, as a new record: nullopt at the end of the file.
  [[nodiscard]] std::expected<std::optional<RunRecord>, std::string> next();

  ~RunReader();
  RunReader(RunReader&& other) noexcept;
  RunReader& operator=(RunReader&& other) noexcept;
  RunReader(const RunReader&) = delete;
  RunReader& operator=(const RunReader&) = delete;

 private:
  RunReader(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}
  // Reads up to `n` bytes into `dst`: fewer only if the file ends first.
  [[nodiscard]] std::expected<std::size_t, std::string> read_bytes(void* dst, std::size_t n);
  [[nodiscard]] std::unexpected<std::string> bad(std::string_view what) const;

  int fd_ = -1;
  std::filesystem::path path_;
  std::vector<std::byte> buffer_;
  std::size_t pos_ = 0;  // read position inside buffer_
  std::size_t end_ = 0;  // bytes of buffer_ that hold file data
  uint64_t file_size_ = 0;
  uint64_t consumed_ = 0;  // bytes of the file handed out so far
  std::string last_term_;
  bool read_any_ = false;
};

}  // namespace mse
