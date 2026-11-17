#include "index_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "file_bytes.hpp"
#include "index_format.hpp"
#include "indexer.hpp"
#include "parsed_loader.hpp"
#include "synthetic_corpus.hpp"
#include "temp_dir.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

InMemoryIndex build(const fs::path& corpus, unsigned threads = 1) {
  auto index = build_index(corpus, {.threads = threads});
  if (!index) throw std::runtime_error(index.error());
  return *std::move(index);
}

WriteStats write_or_die(const InMemoryIndex& index, const fs::path& out, bool compress = true) {
  auto stats = write_index(index, out, {.compress = compress});
  if (!stats) throw std::runtime_error(stats.error());
  return *stats;
}

// Does `==` rather than EXPECT_EQ so that a failure doesn't print a whole index.
::testing::AssertionResult same_index(const InMemoryIndex& a, const InMemoryIndex& b) {
  if (a == b) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
         << "indexes differ: " << a.docs.size() << " vs " << b.docs.size() << " docs, "
         << a.postings.size() << " vs " << b.postings.size() << " terms";
}

// ---- round trips ----

TEST(IndexWriter, RoundTripCompressed) {
  TempDir dir;
  const InMemoryIndex index = build(tiny_corpus_dir());
  write_or_die(index, dir.path() / "i.bin", true);
  auto loaded = load_parsed(dir.path() / "i.bin");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(same_index(*loaded, index));
}

TEST(IndexWriter, RoundTripRaw) {
  TempDir dir;
  const InMemoryIndex index = build(tiny_corpus_dir());
  write_or_die(index, dir.path() / "i.bin", false);
  auto loaded = load_parsed(dir.path() / "i.bin");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(same_index(*loaded, index));
}

TEST(IndexWriter, RoundTripSyntheticCorpusBothModes) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 3000, 800, 3, 11);
  const InMemoryIndex index = build(corpus.path(), 4);
  for (const bool compress : {true, false}) {
    write_or_die(index, dir.path() / "i.bin", compress);
    auto loaded = load_parsed(dir.path() / "i.bin");
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_TRUE(same_index(*loaded, index)) << (compress ? "compressed" : "raw");
  }
}

TEST(IndexWriter, EmptyCorpusRoundTrip) {
  TempDir corpus;
  TempDir dir;
  const InMemoryIndex index = build(corpus.path());
  ASSERT_TRUE(index.docs.empty());
  write_or_die(index, dir.path() / "i.bin");
  auto loaded = load_parsed(dir.path() / "i.bin");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(same_index(*loaded, index));
  EXPECT_EQ(loaded->avg_doc_len(), 0.0);
}

// ---- the bytes on disk ----

TEST(IndexWriter, HeaderFields) {
  TempDir dir;
  const fs::path path = dir.path() / "i.bin";
  const WriteStats stats = write_or_die(build(tiny_corpus_dir()), path);
  const Bytes file = read_file(path);
  ASSERT_GE(file.size(), 256u);

  EXPECT_EQ(std::memcmp(file.data(), "MSEINDEX", 8), 0);
  EXPECT_EQ(load_le<uint32_t>(file, 8), 1u);         // version
  EXPECT_EQ(load_le<uint32_t>(file, 12) & 1u, 1u);   // postings are VByte
  EXPECT_EQ(load_le<uint32_t>(file, 16), 6u);        // num_docs
  EXPECT_EQ(load_le<uint32_t>(file, 20), 27u);       // num_terms
  EXPECT_EQ(load_le<double>(file, 24), 32.0 / 6.0);  // avg_doc_len: 32 tokens, 6 docs
  EXPECT_EQ(load_le<uint64_t>(file, 32), 29u);       // total_postings
  EXPECT_EQ(load_le<uint64_t>(file, 40), fs::file_size(path));
  EXPECT_EQ(stats.file_bytes, fs::file_size(path));
  EXPECT_EQ(load_le<uint32_t>(file, 200), 2u);  // num_parts
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(load_le<uint64_t>(file, 48 + 16 * i) % 8, 0u) << "section " << i;
  }
  for (std::size_t i = 204; i < 256; ++i) EXPECT_EQ(file[i], std::byte{0}) << "byte " << i;
}

TEST(IndexWriter, RawFlagIsClear) {
  TempDir dir;
  write_or_die(build(tiny_corpus_dir()), dir.path() / "i.bin", false);
  EXPECT_EQ(load_le<uint32_t>(read_file(dir.path() / "i.bin"), 12) & 1u, 0u);
}

TEST(IndexWriter, TermTableIsSortedByBytes) {
  TempDir dir;
  const fs::path path = dir.path() / "i.bin";
  write_or_die(build(tiny_corpus_dir()), path);
  const Bytes file = read_file(path);
  const auto terms_offset = load_le<uint64_t>(file, 48 + 16 * 4);
  const auto strings_offset = load_le<uint64_t>(file, 48 + 16 * 5);

  std::vector<std::string> terms;
  for (uint32_t i = 0; i < 27; ++i) {
    const std::size_t entry = terms_offset + 24 * i;
    const auto off = load_le<uint32_t>(file, entry);
    const auto len = load_le<uint32_t>(file, entry + 4);
    terms.emplace_back(reinterpret_cast<const char*>(file.data()) + strings_offset + off, len);
  }
  for (std::size_t i = 1; i < terms.size(); ++i) {
    EXPECT_LT(terms[i - 1].compare(terms[i]), 0) << terms[i - 1] << " before " << terms[i];
  }
  EXPECT_NE(std::find(terms.begin(), terms.end(), "café"), terms.end());
  EXPECT_EQ(terms.back(), "東京");  // bytes >= 0x80 sort after every ASCII letter
}

TEST(IndexWriter, CompressedIsSmallerThanRaw) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 3000, 800, 3, 12);
  const InMemoryIndex index = build(corpus.path());
  const WriteStats compressed = write_or_die(index, dir.path() / "c.bin", true);
  const WriteStats raw = write_or_die(index, dir.path() / "r.bin", false);
  EXPECT_LT(compressed.postings_bytes, compressed.raw_postings_bytes);
  EXPECT_EQ(raw.postings_bytes, raw.raw_postings_bytes);
  EXPECT_EQ(raw.raw_postings_bytes, compressed.raw_postings_bytes);
  EXPECT_EQ(raw.raw_postings_bytes, index.total_postings() * 8);
  EXPECT_LT(compressed.file_bytes, raw.file_bytes);
}

TEST(IndexWriter, ThreadCountDoesNotChangeTheFileByAnyByte) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 3000, 800, 3, 13);
  write_or_die(build(corpus.path(), 1), dir.path() / "one.bin");
  write_or_die(build(corpus.path(), 8), dir.path() / "eight.bin");
  EXPECT_TRUE(read_file(dir.path() / "one.bin") == read_file(dir.path() / "eight.bin"));

  write_or_die(build(tiny_corpus_dir(), 1), dir.path() / "t1.bin");
  write_or_die(build(tiny_corpus_dir(), 8), dir.path() / "t8.bin");
  EXPECT_TRUE(read_file(dir.path() / "t1.bin") == read_file(dir.path() / "t8.bin"));
}

// ---- using the writer directly ----

DocInfo doc(std::string title, uint32_t length = 1) {
  return {std::move(title), DocRef{0, 0, 0}, length};
}

struct WriterFixture {
  TempDir dir;
  fs::path out = dir.path() / "i.bin";
  std::vector<std::string> parts = {"part-00000.tsv"};
  std::vector<DocInfo> docs = {doc("a"), doc("b"), doc("c")};

  IndexWriter writer_with_docs() {
    auto writer = IndexWriter::create(out, {});
    EXPECT_TRUE(writer.has_value());
    EXPECT_TRUE(writer->set_docs("/root", parts, docs).has_value());
    return *std::move(writer);
  }
};

TEST(IndexWriter, AddTermOutOfOrderIsError) {
  WriterFixture f;
  IndexWriter writer = f.writer_with_docs();
  const std::vector<Posting> ok = {{0, 1}};
  ASSERT_TRUE(writer.add_term("b", ok).has_value());
  EXPECT_FALSE(writer.add_term("a", ok).has_value());  // goes backwards
  EXPECT_FALSE(writer.add_term("b", ok).has_value());  // repeats
  EXPECT_TRUE(writer.add_term("c", ok).has_value());   // the writer is still usable
}

TEST(IndexWriter, AddTermRejectsBadTermsAndPostings) {
  WriterFixture f;
  IndexWriter writer = f.writer_with_docs();
  const std::vector<Posting> ok = {{0, 1}};
  EXPECT_FALSE(writer.add_term("", ok).has_value());   // empty term
  EXPECT_FALSE(writer.add_term("a", {}).has_value());  // no postings
  EXPECT_FALSE(writer.add_term("a", std::vector<Posting>{{1, 1}, {0, 1}}).has_value());  // unsorted
  EXPECT_FALSE(
      writer.add_term("a", std::vector<Posting>{{1, 1}, {1, 1}}).has_value());   // repeated doc
  EXPECT_FALSE(writer.add_term("a", std::vector<Posting>{{0, 0}}).has_value());  // tf = 0
  EXPECT_FALSE(writer.add_term("a", std::vector<Posting>{{3, 1}}).has_value());  // doc >= num_docs
  EXPECT_TRUE(writer.add_term("a", ok).has_value());
}

TEST(IndexWriter, CallsInTheWrongOrderAreErrors) {
  WriterFixture f;
  auto writer = IndexWriter::create(f.out, {});
  ASSERT_TRUE(writer.has_value());
  EXPECT_FALSE(writer->add_term("a", std::vector<Posting>{{0, 1}}).has_value());  // before set_docs
  EXPECT_FALSE(writer->finish().has_value());                                     // before set_docs
  ASSERT_TRUE(writer->set_docs("/root", f.parts, f.docs).has_value());
  EXPECT_FALSE(writer->set_docs("/root", f.parts, f.docs).has_value());  // only once
}

TEST(IndexWriter, SetDocsRejectsAPartThatDoesNotExist) {
  WriterFixture f;
  f.docs[1].ref.part = 1;  // there is only part 0
  auto writer = IndexWriter::create(f.out, {});
  ASSERT_TRUE(writer.has_value());
  EXPECT_FALSE(writer->set_docs("/root", f.parts, f.docs).has_value());
}

std::vector<fs::path> temporary_files(const fs::path& dir) {
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() == ".tmp") found.push_back(entry.path());
  }
  return found;
}

TEST(IndexWriter, FinishLeavesNoTemporaryFiles) {
  WriterFixture f;
  IndexWriter writer = f.writer_with_docs();
  ASSERT_TRUE(writer.add_term("a", std::vector<Posting>{{0, 1}, {2, 3}}).has_value());
  EXPECT_FALSE(temporary_files(f.dir.path()).empty());  // while writing, they exist
  ASSERT_TRUE(writer.finish().has_value());
  EXPECT_TRUE(temporary_files(f.dir.path()).empty());
  EXPECT_TRUE(fs::exists(f.out));
}

TEST(IndexWriter, DestroyingAWriterWithoutFinishLeavesNothingBehind) {
  WriterFixture f;
  {
    IndexWriter writer = f.writer_with_docs();
    ASSERT_TRUE(writer.add_term("a", std::vector<Posting>{{0, 1}}).has_value());
  }
  EXPECT_TRUE(temporary_files(f.dir.path()).empty());
  EXPECT_FALSE(fs::exists(f.out));
}

TEST(IndexWriter, AMovedWriterStillCleansUp) {
  WriterFixture f;
  {
    IndexWriter first = f.writer_with_docs();
    IndexWriter second = std::move(first);
    ASSERT_TRUE(second.add_term("a", std::vector<Posting>{{0, 1}}).has_value());
  }
  EXPECT_TRUE(temporary_files(f.dir.path()).empty());
}

TEST(IndexWriter, WritingIntoAMissingDirectoryIsAnError) {
  TempDir dir;
  EXPECT_FALSE(IndexWriter::create(dir.path() / "nope" / "i.bin", {}).has_value());
  EXPECT_FALSE(
      write_index(build(tiny_corpus_dir()), dir.path() / "nope" / "i.bin", {}).has_value());
}

TEST(IndexWriter, AnExistingIndexIsReplaced) {
  TempDir dir;
  const fs::path path = dir.path() / "i.bin";
  write_or_die(build(tiny_corpus_dir()), path);
  const auto first_size = fs::file_size(path);
  TempDir empty_corpus;
  write_or_die(build(empty_corpus.path()), path);
  EXPECT_LT(fs::file_size(path), first_size);
  EXPECT_TRUE(load_parsed(path).has_value());
}

// ---- corrupt files (Review Focus 2) ----

// Writes a good index, applies `damage` to its bytes, and expects the loader to say no.
template <class Damage>
void expect_rejected(const char* what, Damage damage, bool compress = true) {
  TempDir dir;
  const fs::path good = dir.path() / "good.bin";
  write_or_die(build(tiny_corpus_dir()), good, compress);
  Bytes bytes = read_file(good);
  damage(bytes);
  write_file(dir.path() / "bad.bin", bytes);
  const auto loaded = load_parsed(dir.path() / "bad.bin");
  EXPECT_FALSE(loaded.has_value()) << "accepted: " << what;
}

uint64_t section_offset(const Bytes& file, int section) {
  return load_le<uint64_t>(file, 48 + 16 * section);
}

TEST(IndexLoader, ASmallOrMissingFileIsAnError) {
  TempDir dir;
  EXPECT_FALSE(load_parsed(dir.path() / "missing.bin").has_value());
  write_file(dir.path() / "empty.bin", Bytes{});
  EXPECT_FALSE(load_parsed(dir.path() / "empty.bin").has_value());
  write_file(dir.path() / "small.bin", Bytes(100));
  EXPECT_FALSE(load_parsed(dir.path() / "small.bin").has_value());
}

TEST(IndexLoader, HeaderDamageIsRejected) {
  expect_rejected("wrong magic", [](Bytes& b) { b[0] = std::byte{'X'}; });
  expect_rejected("wrong version", [](Bytes& b) { store_le<uint32_t>(b, 8, 2); });
  expect_rejected("unknown flag", [](Bytes& b) { store_le<uint32_t>(b, 12, 3); });
  expect_rejected("truncated", [](Bytes& b) { b.resize(b.size() - 10); });
  expect_rejected("extended", [](Bytes& b) { b.push_back(std::byte{0}); });
  expect_rejected("file_size field", [](Bytes& b) { store_le<uint64_t>(b, 40, 12345); });
  expect_rejected("average length is NaN", [](Bytes& b) { store_le<double>(b, 24, std::nan("")); });
}

TEST(IndexLoader, SectionDamageIsRejected) {
  expect_rejected("section past the end",
                  [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 6, b.size()); });
  expect_rejected("misaligned section",
                  [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 4, section_offset(b, 4) + 1); });
  expect_rejected("sections overlap", [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 1, 8); });
  expect_rejected("size of the term table", [](Bytes& b) {
    store_le<uint64_t>(b, 48 + 16 * 4 + 8, load_le<uint64_t>(b, 48 + 16 * 4 + 8) - 24);
  });
  expect_rejected("size of the doc lengths",
                  [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 0 + 8, 4); });
  expect_rejected("num_docs", [](Bytes& b) { store_le<uint32_t>(b, 16, 7); });
  expect_rejected("corpus root outside strings",
                  [](Bytes& b) { store_le<uint32_t>(b, 196, 1u << 30); });
}

TEST(IndexLoader, DocDamageIsRejected) {
  expect_rejected("part index out of range",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 1) + 16 * 2, 99); });
  expect_rejected("title outside strings",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 2), 1u << 30); });
  expect_rejected("part name outside strings",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 3) + 4, 1u << 30); });
}

TEST(IndexLoader, TermDamageIsRejected) {
  expect_rejected("term string outside strings",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 4), 1u << 30); });
  expect_rejected("postings outside their section",
                  [](Bytes& b) { store_le<uint64_t>(b, section_offset(b, 4) + 16, 1ull << 40); });
  expect_rejected("df does not match the postings",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 4) + 8, 50); });
  expect_rejected("postings size too small",
                  [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 4) + 12, 1); });
  expect_rejected("terms out of order", [](Bytes& b) {  // swap the strings of terms 0 and 1
    const std::size_t t = section_offset(b, 4);
    for (std::size_t i = 0; i < 8; ++i) std::swap(b[t + i], b[t + 24 + i]);
  });
  expect_rejected("total_postings", [](Bytes& b) { store_le<uint64_t>(b, 32, 30); });
}

TEST(IndexLoader, RawPostingDamageIsRejected) {
  expect_rejected(
      "doc id past num_docs",
      [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 6), 0xFFFFFFFFu); }, false);
  expect_rejected(
      "raw size is not df * 8",
      [](Bytes& b) { store_le<uint32_t>(b, section_offset(b, 4) + 12, 7); }, false);
}

}  // namespace
}  // namespace mse
