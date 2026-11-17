#include "byte_source.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "file_descriptor.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

std::unexpected<std::string> fail(const fs::path& path, std::string_view what) {
  return std::unexpected(path.string() + ": " + std::string(what) + ": " + std::strerror(errno));
}

// The size of the open file, which must be a regular file with at least one byte.
std::expected<std::size_t, std::string> usable_size(int fd, const fs::path& path) {
  struct stat info;
  if (::fstat(fd, &info) != 0) return fail(path, "cannot stat");
  if (!S_ISREG(info.st_mode)) return std::unexpected(path.string() + ": not a regular file");
  if (info.st_size == 0) return std::unexpected(path.string() + ": file is empty");
  return static_cast<std::size_t>(info.st_size);
}

}  // namespace

// ---- MappedFile ----

std::expected<MappedFile, std::string> MappedFile::open(const fs::path& path) {
  const FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return fail(path, "cannot open");
  const auto size = usable_size(fd.get(), path);
  if (!size) return std::unexpected(size.error());

  void* address = ::mmap(nullptr, *size, PROT_READ, MAP_PRIVATE, fd.get(), 0);
  if (address == MAP_FAILED) return fail(path, "cannot map");
  return MappedFile(static_cast<const std::byte*>(address), *size);  // the mapping outlives fd
}

void MappedFile::release() {
  if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
  data_ = nullptr;
  size_ = 0;
}

MappedFile::~MappedFile() { release(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    release();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

// ---- FileBuffer ----

std::expected<FileBuffer, std::string> FileBuffer::open(const fs::path& path) {
  const FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return fail(path, "cannot open");
  const auto size = usable_size(fd.get(), path);
  if (!size) return std::unexpected(size.error());

  auto data = std::make_unique<std::byte[]>(*size);
  std::size_t done = 0;
  while (done < *size) {
    const ssize_t n = ::read(fd.get(), data.get() + done, *size - done);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return fail(path, "read failed");
    if (n == 0) return std::unexpected(path.string() + ": file became shorter while reading");
    done += static_cast<std::size_t>(n);
  }
  return FileBuffer(std::move(data), *size);
}

FileBuffer::FileBuffer(FileBuffer&& other) noexcept
    : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

FileBuffer& FileBuffer::operator=(FileBuffer&& other) noexcept {
  data_ = std::move(other.data_);
  size_ = std::exchange(other.size_, 0);
  return *this;
}

// ---- page cache ----

std::expected<void, std::string> evict_from_page_cache(const fs::path& path) {
  const FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return fail(path, "cannot open");
  if (::fdatasync(fd.get()) != 0) return fail(path, "cannot sync");
  const int error = ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED);
  if (error != 0) {
    errno = error;  // posix_fadvise returns the error number instead of setting errno
    return fail(path, "cannot evict");
  }
  return {};
}

}  // namespace mse
