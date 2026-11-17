#include "spill.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <utility>

static_assert(std::endian::native == std::endian::little, "run files are little-endian");

namespace mse {
namespace {

static_assert(sizeof(Posting) == 8, "a Posting is written to the run file as it is in memory");

constexpr std::size_t kMaxRunTermBytes = 1 << 20;

std::string errno_text(int error) { return std::strerror(error); }

void append_u32(std::vector<std::byte>& out, uint32_t value) {
  const auto* bytes = reinterpret_cast<const std::byte*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(value));
}

}  // namespace

// ---- RunWriter ----

std::expected<RunWriter, std::string> RunWriter::create(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return std::unexpected("cannot create run file " + path.string() + ": " + errno_text(errno));
  }
  RunWriter writer(fd, path);
  writer.buffer_.reserve(kRunBufferBytes + 4096);
  return writer;
}

RunWriter::~RunWriter() { close_quietly(); }

RunWriter::RunWriter(RunWriter&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      buffer_(std::move(other.buffer_)),
      bytes_written_(other.bytes_written_),
      last_term_(std::move(other.last_term_)),
      wrote_any_(other.wrote_any_) {}

RunWriter& RunWriter::operator=(RunWriter&& other) noexcept {
  if (this != &other) {
    close_quietly();
    fd_ = std::exchange(other.fd_, -1);
    path_ = std::move(other.path_);
    buffer_ = std::move(other.buffer_);
    bytes_written_ = other.bytes_written_;
    last_term_ = std::move(other.last_term_);
    wrote_any_ = other.wrote_any_;
  }
  return *this;
}

void RunWriter::close_quietly() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

std::expected<void, std::string> RunWriter::flush() {
  std::size_t done = 0;
  while (done < buffer_.size()) {
    const ssize_t n = ::write(fd_, buffer_.data() + done, buffer_.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected("cannot write run file " + path_.string() + ": " + errno_text(errno));
    }
    done += static_cast<std::size_t>(n);
  }
  bytes_written_ += buffer_.size();
  buffer_.clear();
  return {};
}

std::expected<void, std::string> RunWriter::write(std::string_view term,
                                                  std::span<const Posting> postings) {
  if (term.empty() || term.size() > kMaxRunTermBytes) {
    return std::unexpected("run record: bad term length " + std::to_string(term.size()));
  }
  if (wrote_any_ && term <= last_term_) {
    return std::unexpected("run record: terms must be strictly increasing (\"" + std::string(term) +
                           "\" after \"" + last_term_ + "\")");
  }
  if (postings.empty() || postings.size() > UINT32_MAX) {
    return std::unexpected("run record: \"" + std::string(term) + "\" has no postings");
  }
  for (std::size_t i = 0; i < postings.size(); ++i) {
    if (postings[i].tf == 0 || (i > 0 && postings[i].doc <= postings[i - 1].doc)) {
      return std::unexpected("run record: postings of \"" + std::string(term) +
                             "\" must be increasing by doc, with tf >= 1");
    }
  }

  append_u32(buffer_, static_cast<uint32_t>(term.size()));
  const auto* term_bytes = reinterpret_cast<const std::byte*>(term.data());
  buffer_.insert(buffer_.end(), term_bytes, term_bytes + term.size());
  append_u32(buffer_, static_cast<uint32_t>(postings.size()));
  const auto* posting_bytes = reinterpret_cast<const std::byte*>(postings.data());
  buffer_.insert(buffer_.end(), posting_bytes, posting_bytes + postings.size_bytes());
  last_term_.assign(term);
  wrote_any_ = true;
  if (buffer_.size() >= kRunBufferBytes) return flush();
  return {};
}

std::expected<uint64_t, std::string> RunWriter::finish() {
  if (fd_ < 0) return std::unexpected("run file " + path_.string() + " is already closed");
  if (auto ok = flush(); !ok) return std::unexpected(ok.error());
  const int fd = std::exchange(fd_, -1);
  if (::close(fd) != 0) {
    return std::unexpected("cannot close run file " + path_.string() + ": " + errno_text(errno));
  }
  return bytes_written_;
}

// ---- RunReader ----

std::expected<RunReader, std::string> RunReader::open(const std::filesystem::path& path,
                                                      std::size_t buffer_bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::unexpected("cannot open run file " + path.string() + ": " + errno_text(errno));
  }
  struct stat info{};
  if (::fstat(fd, &info) != 0) {
    const int error = errno;
    ::close(fd);
    return std::unexpected("cannot stat run file " + path.string() + ": " + errno_text(error));
  }
  RunReader reader(fd, path);
  reader.buffer_.resize(std::max<std::size_t>(buffer_bytes, 4096));
  reader.file_size_ = static_cast<uint64_t>(info.st_size);
  return reader;
}

RunReader::~RunReader() {
  if (fd_ >= 0) ::close(fd_);
}

RunReader::RunReader(RunReader&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      path_(std::move(other.path_)),
      buffer_(std::move(other.buffer_)),
      pos_(other.pos_),
      end_(other.end_),
      file_size_(other.file_size_),
      consumed_(other.consumed_),
      last_term_(std::move(other.last_term_)),
      read_any_(other.read_any_) {}

RunReader& RunReader::operator=(RunReader&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    path_ = std::move(other.path_);
    buffer_ = std::move(other.buffer_);
    pos_ = other.pos_;
    end_ = other.end_;
    file_size_ = other.file_size_;
    consumed_ = other.consumed_;
    last_term_ = std::move(other.last_term_);
    read_any_ = other.read_any_;
  }
  return *this;
}

std::unexpected<std::string> RunReader::bad(std::string_view what) const {
  return std::unexpected("run file " + path_.string() + ": " + std::string(what));
}

std::expected<std::size_t, std::string> RunReader::read_bytes(void* dst, std::size_t n) {
  auto* out = static_cast<std::byte*>(dst);
  std::size_t got = 0;
  while (got < n) {
    if (pos_ < end_) {  // from the buffer first
      const std::size_t take = std::min(n - got, end_ - pos_);
      std::memcpy(out + got, buffer_.data() + pos_, take);
      pos_ += take;
      got += take;
      continue;
    }
    // A request at least as big as the buffer goes straight to the destination.
    const bool direct = n - got >= buffer_.size();
    std::byte* target = direct ? out + got : buffer_.data();
    const std::size_t want = direct ? n - got : buffer_.size();
    const ssize_t r = ::read(fd_, target, want);
    if (r < 0) {
      if (errno == EINTR) continue;
      return std::unexpected("cannot read run file " + path_.string() + ": " + errno_text(errno));
    }
    if (r == 0) break;  // end of file
    if (direct) {
      got += static_cast<std::size_t>(r);
    } else {
      pos_ = 0;
      end_ = static_cast<std::size_t>(r);
    }
  }
  consumed_ += got;
  return got;
}

std::expected<bool, std::string> RunReader::read(RunRecord& out) {
  uint32_t term_len = 0;
  auto got = read_bytes(&term_len, sizeof(term_len));
  if (!got) return std::unexpected(got.error());
  if (*got == 0) return false;  // a clean end, at a record boundary
  if (*got < sizeof(term_len)) return bad("ends in the middle of a record");
  if (term_len == 0 || term_len > kMaxRunTermBytes || term_len > file_size_ - consumed_) {
    return bad("bad term length " + std::to_string(term_len));
  }

  out.term.resize(term_len);
  got = read_bytes(out.term.data(), term_len);
  if (!got) return std::unexpected(got.error());
  if (*got < term_len) return bad("ends in the middle of a record");

  uint32_t count = 0;
  got = read_bytes(&count, sizeof(count));
  if (!got) return std::unexpected(got.error());
  if (*got < sizeof(count)) return bad("ends in the middle of a record");
  // The count is checked against what is left of the file before anything is allocated for it.
  if (count == 0 || uint64_t{count} * sizeof(Posting) > file_size_ - consumed_) {
    return bad("bad posting count " + std::to_string(count) + " for \"" + out.term + "\"");
  }

  out.postings.resize(count);
  got = read_bytes(out.postings.data(), std::size_t{count} * sizeof(Posting));
  if (!got) return std::unexpected(got.error());
  if (*got < std::size_t{count} * sizeof(Posting)) return bad("ends in the middle of a record");

  if (read_any_ && out.term <= last_term_) {
    return bad("terms are not in increasing order at \"" + out.term + "\"");
  }
  for (std::size_t i = 0; i < out.postings.size(); ++i) {
    if (out.postings[i].tf == 0 || (i > 0 && out.postings[i].doc <= out.postings[i - 1].doc)) {
      return bad("postings of \"" + out.term + "\" are not increasing by doc");
    }
  }
  last_term_ = out.term;
  read_any_ = true;
  return true;
}

std::expected<std::optional<RunRecord>, std::string> RunReader::next() {
  RunRecord record;
  const auto more = read(record);
  if (!more) return std::unexpected(more.error());
  if (!*more) return std::optional<RunRecord>();
  return std::optional<RunRecord>(std::move(record));
}

}  // namespace mse
