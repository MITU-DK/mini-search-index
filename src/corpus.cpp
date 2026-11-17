#include "corpus.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>

#include "file_descriptor.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kScanBufferSize = 1 << 20;  // 1 MiB

std::unexpected<std::string> fail(const fs::path& file, std::string_view what) {
  return std::unexpected(file.string() + ": " + std::string(what));
}

// Appends one DocRef per line of `file`. Line i ends at its '\n'; a last line without '\n' counts.
std::expected<void, std::string> scan_part(const fs::path& file, uint32_t part,
                                           std::vector<DocRef>& docs) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(file, "cannot open");

  uint64_t line_start = 0;    // file offset where the current line begins
  uint64_t buffer_start = 0;  // file offset of buffer[0]
  std::vector<char> buffer(kScanBufferSize);

  // Records the line [line_start, end). Returns false if it is too long for DocRef::length.
  auto add_line = [&](uint64_t end) {
    const uint64_t length = end - line_start;
    if (length > std::numeric_limits<uint32_t>::max()) return false;
    docs.push_back({part, static_cast<uint32_t>(length), line_start});
    return true;
  };

  for (;;) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n == 0) break;

    std::size_t pos = 0;
    while (pos < n) {
      const void* newline = std::memchr(buffer.data() + pos, '\n', n - pos);
      if (newline == nullptr) break;
      const auto newline_pos =
          static_cast<std::size_t>(static_cast<const char*>(newline) - buffer.data());
      if (!add_line(buffer_start + newline_pos)) return fail(file, "line longer than 4 GiB");
      line_start = buffer_start + newline_pos + 1;
      pos = newline_pos + 1;
    }
    buffer_start += n;
  }
  if (in.bad()) return fail(file, "read error");

  const bool last_line_has_no_newline = line_start < buffer_start;
  if (last_line_has_no_newline && !add_line(buffer_start)) {
    return fail(file, "line longer than 4 GiB");
  }
  return {};
}

std::expected<std::vector<std::string>, std::string> list_part_files(const fs::path& root) {
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    const bool is_part = name.starts_with("part-") && name.ends_with(".tsv");
    if (is_part && it->is_regular_file(ec)) names.push_back(name);
  }
  if (ec) return fail(root, ec.message());
  std::sort(names.begin(), names.end());
  return names;
}

// Appends batches of at most `batch_size` docs covering docs [first, last).
void add_batches(uint32_t first, uint32_t last, std::size_t batch_size,
                 std::vector<Batch>& batches) {
  for (uint32_t start = first; start < last;) {
    const auto count = static_cast<uint32_t>(std::min<std::size_t>(batch_size, last - start));
    batches.push_back({start, count});
    start += count;
  }
}

}  // namespace

std::expected<Corpus, std::string> open_corpus(const fs::path& root, std::size_t batch_size) {
  if (batch_size == 0) return fail(root, "batch size must be at least 1");
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return fail(root, "not a directory");

  auto names = list_part_files(root);
  if (!names) return std::unexpected(names.error());

  Corpus corpus;
  corpus.root = fs::absolute(root);
  corpus.part_names = std::move(*names);
  for (std::size_t part = 0; part < corpus.part_names.size(); ++part) {
    const auto first_doc = static_cast<uint32_t>(corpus.docs.size());
    auto scanned =
        scan_part(corpus.root / corpus.part_names[part], static_cast<uint32_t>(part), corpus.docs);
    if (!scanned) return std::unexpected(scanned.error());
    if (corpus.docs.size() > std::numeric_limits<uint32_t>::max()) {
      return fail(root, "more than 2^32 - 1 docs");
    }
    add_batches(first_doc, static_cast<uint32_t>(corpus.docs.size()), batch_size, corpus.batches);
  }
  return corpus;
}

DocText split_line(std::string_view line) {
  const std::size_t tab = line.find('\t');
  if (tab == std::string_view::npos) return {line, {}};
  return {line.substr(0, tab), line.substr(tab + 1)};
}

std::expected<std::string, std::string> read_range(const fs::path& file, uint64_t offset,
                                                   std::size_t length) {
  const FileDescriptor fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return fail(file, std::strerror(errno));

  // Check the range first, so a corrupt offset or length can't trigger a huge allocation.
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0) return fail(file, std::strerror(errno));
  const auto file_size = static_cast<uint64_t>(info.st_size);
  if (offset > file_size || length > file_size - offset) {
    return fail(file, "range is past the end of the file");
  }

  std::string bytes(length, '\0');
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n =
        ::pread(fd.get(), bytes.data() + done, length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail(file, std::strerror(errno));
    }
    if (n == 0) return fail(file, "file ended while reading");
    done += static_cast<std::size_t>(n);
  }
  return bytes;
}

}  // namespace mse
