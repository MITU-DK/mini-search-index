#include "byte_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "temp_dir.hpp"

namespace mse {
namespace {

std::string as_text(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

TEST(ByteSource, MapsContent) {
  TempDir dir;
  write_file(dir.path() / "f", "hello mmap");
  auto mapped = MappedFile::open(dir.path() / "f");
  ASSERT_TRUE(mapped.has_value()) << mapped.error();
  EXPECT_EQ(as_text(mapped->bytes()), "hello mmap");
}

TEST(ByteSource, BufferContent) {
  TempDir dir;
  write_file(dir.path() / "f", "hello read");
  auto buffer = FileBuffer::open(dir.path() / "f");
  ASSERT_TRUE(buffer.has_value()) << buffer.error();
  EXPECT_EQ(as_text(buffer->bytes()), "hello read");
}

TEST(ByteSource, ALargeFileIsReadCompletely) {
  TempDir dir;
  std::string big(5 * 1024 * 1024 + 123, 'x');
  big[big.size() / 2] = 'y';
  write_file(dir.path() / "big", big);
  auto buffer = FileBuffer::open(dir.path() / "big");
  auto mapped = MappedFile::open(dir.path() / "big");
  ASSERT_TRUE(buffer.has_value());
  ASSERT_TRUE(mapped.has_value());
  EXPECT_TRUE(as_text(buffer->bytes()) == big);
  EXPECT_TRUE(as_text(mapped->bytes()) == big);
}

TEST(ByteSource, ZeroByteFileIsError) {
  TempDir dir;
  write_file(dir.path() / "empty", "");
  EXPECT_FALSE(MappedFile::open(dir.path() / "empty").has_value());
  EXPECT_FALSE(FileBuffer::open(dir.path() / "empty").has_value());
}

TEST(ByteSource, MissingFileIsError) {
  TempDir dir;
  const auto mapped = MappedFile::open(dir.path() / "nope");
  const auto buffer = FileBuffer::open(dir.path() / "nope");
  ASSERT_FALSE(mapped.has_value());
  ASSERT_FALSE(buffer.has_value());
  EXPECT_NE(mapped.error().find("nope"), std::string::npos);
}

TEST(ByteSource, ADirectoryIsError) {
  TempDir dir;
  EXPECT_FALSE(MappedFile::open(dir.path()).has_value());
  EXPECT_FALSE(FileBuffer::open(dir.path()).has_value());
}

TEST(ByteSource, MoveLeavesSourceEmpty) {
  TempDir dir;
  write_file(dir.path() / "f", "abc");

  MappedFile mapped = *MappedFile::open(dir.path() / "f");
  MappedFile mapped_to = std::move(mapped);
  EXPECT_TRUE(mapped.bytes().empty());  // NOLINT: checking the moved-from state on purpose
  EXPECT_EQ(as_text(mapped_to.bytes()), "abc");

  FileBuffer buffer = *FileBuffer::open(dir.path() / "f");
  FileBuffer buffer_to = std::move(buffer);
  EXPECT_TRUE(buffer.bytes().empty());  // NOLINT
  EXPECT_EQ(as_text(buffer_to.bytes()), "abc");
}

TEST(ByteSource, MoveAssignmentReleasesTheOldMapping) {
  TempDir dir;
  write_file(dir.path() / "a", "first");
  write_file(dir.path() / "b", "second");
  MappedFile a = *MappedFile::open(dir.path() / "a");
  a = *MappedFile::open(dir.path() / "b");  // ASan/leak checks catch a forgotten munmap
  EXPECT_EQ(as_text(a.bytes()), "second");
}

TEST(ByteSource, EvictingSucceedsAndLeavesTheContentReadable) {
  TempDir dir;
  write_file(dir.path() / "f", std::string(100000, 'z'));
  ASSERT_TRUE(evict_from_page_cache(dir.path() / "f").has_value());
  const auto buffer = FileBuffer::open(dir.path() / "f");
  ASSERT_TRUE(buffer.has_value());
  EXPECT_EQ(buffer->bytes().size(), 100000u);
}

TEST(ByteSource, EvictingAMissingFileIsError) {
  TempDir dir;
  EXPECT_FALSE(evict_from_page_cache(dir.path() / "nope").has_value());
}

}  // namespace
}  // namespace mse
