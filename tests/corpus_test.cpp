#include "corpus.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "temp_dir.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

fs::path fixture_root() { return fs::path(MSE_TEST_DATA_DIR) / "tiny_corpus"; }

// ---- open_corpus on the fixture (part-00000: docs 0-2, part-00001: docs 3-5) ----

TEST(Corpus, OpensFixture) {
  auto corpus = open_corpus(fixture_root());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  EXPECT_TRUE(corpus->root.is_absolute());
  EXPECT_EQ(corpus->part_names, (std::vector<std::string>{"part-00000.tsv", "part-00001.tsv"}));
  ASSERT_EQ(corpus->docs.size(), 6u);

  const auto delta_len =
      static_cast<uint32_t>(std::string_view("Delta\tCafé au lait in 東京").size());
  EXPECT_EQ(corpus->docs[3], (DocRef{1, delta_len, 0}));
  EXPECT_EQ(corpus->docs[0].offset, 0u);
  EXPECT_EQ(corpus->docs[1].offset, corpus->docs[0].length + 1u);  // +1 for the '\n'
}

TEST(Corpus, LastLineWithoutNewline) {
  auto corpus = open_corpus(fixture_root());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  const DocRef last = corpus->docs[5];
  EXPECT_EQ(last.length, 11u);

  auto line = read_range(corpus->root / corpus->part_names[last.part], last.offset, last.length);
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(*line, "!!!\t??? ...");
}

TEST(Corpus, EmptyDocIsKept) {
  auto corpus = open_corpus(fixture_root());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  EXPECT_EQ(corpus->docs[4].length, 1u);  // the line "\t"
}

TEST(Corpus, DefaultBatchesCoverEachPart) {
  auto corpus = open_corpus(fixture_root());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  ASSERT_EQ(corpus->batches.size(), 2u);
  EXPECT_EQ(corpus->batches[0].first_doc, 0u);
  EXPECT_EQ(corpus->batches[0].count, 3u);
  EXPECT_EQ(corpus->batches[1].first_doc, 3u);
  EXPECT_EQ(corpus->batches[1].count, 3u);
}

TEST(Corpus, BatchesNeverCrossParts) {
  auto corpus = open_corpus(fixture_root(), 2);
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  ASSERT_EQ(corpus->batches.size(), 4u);
  const uint32_t first[] = {0, 2, 3, 5};
  const uint32_t count[] = {2, 1, 2, 1};
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(corpus->batches[i].first_doc, first[i]);
    EXPECT_EQ(corpus->batches[i].count, count[i]);
  }
}

// ---- open_corpus errors and edge cases ----

TEST(Corpus, MissingRootIsError) { EXPECT_FALSE(open_corpus("/nonexistent/mse").has_value()); }

TEST(Corpus, RootThatIsAFileIsError) {
  TempDir dir;
  write_file(dir.path() / "file", "x");
  EXPECT_FALSE(open_corpus(dir.path() / "file").has_value());
}

TEST(Corpus, BatchSizeZeroIsError) { EXPECT_FALSE(open_corpus(fixture_root(), 0).has_value()); }

TEST(Corpus, EmptyDirIsEmpty) {
  TempDir dir;
  auto corpus = open_corpus(dir.path());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  EXPECT_TRUE(corpus->part_names.empty());
  EXPECT_TRUE(corpus->docs.empty());
  EXPECT_TRUE(corpus->batches.empty());
}

TEST(Corpus, OnlyPartFilesAreRead) {
  TempDir dir;
  write_file(dir.path() / "part-00000.tsv", "a\tb\n");
  write_file(dir.path() / "notes.txt", "ignored\n");
  write_file(dir.path() / "part-00001.tsv.bak", "ignored\n");
  auto corpus = open_corpus(dir.path());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  EXPECT_EQ(corpus->part_names, (std::vector<std::string>{"part-00000.tsv"}));
  EXPECT_EQ(corpus->docs.size(), 1u);
}

TEST(Corpus, PartsAreSortedByName) {
  TempDir dir;
  write_file(dir.path() / "part-00001.tsv", "second\tx\n");
  write_file(dir.path() / "part-00000.tsv", "first\tx\n");
  auto corpus = open_corpus(dir.path());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  EXPECT_EQ(corpus->part_names, (std::vector<std::string>{"part-00000.tsv", "part-00001.tsv"}));
  EXPECT_EQ(corpus->docs[0].part, 0u);
  EXPECT_EQ(corpus->docs[1].part, 1u);
}

TEST(Corpus, NoPhantomDocAfterFinalNewline) {
  TempDir dir;
  write_file(dir.path() / "part-00000.tsv", "x\ty\n");
  auto one = open_corpus(dir.path());
  ASSERT_TRUE(one.has_value()) << one.error();
  EXPECT_EQ(one->docs.size(), 1u);

  write_file(dir.path() / "part-00000.tsv", "x\ty\n\n");  // a blank line is a real (empty) doc
  auto two = open_corpus(dir.path());
  ASSERT_TRUE(two.has_value()) << two.error();
  ASSERT_EQ(two->docs.size(), 2u);
  EXPECT_EQ(two->docs[1].length, 0u);
}

TEST(Corpus, EmptyPartFileHasNoDocs) {
  TempDir dir;
  write_file(dir.path() / "part-00000.tsv", "");
  write_file(dir.path() / "part-00001.tsv", "a\tb");
  auto corpus = open_corpus(dir.path());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  ASSERT_EQ(corpus->docs.size(), 1u);
  EXPECT_EQ(corpus->docs[0].part, 1u);
  ASSERT_EQ(corpus->batches.size(), 1u);
  EXPECT_EQ(corpus->batches[0].first_doc, 0u);
}

TEST(Corpus, LinesLongerThanTheScanBuffer) {
  // 3 lines of 700000 bytes each, so line ends fall in the 2nd and 3rd 1 MiB scan buffers.
  const std::string line = "t\t" + std::string(699998, 'x');
  TempDir dir;
  write_file(dir.path() / "part-00000.tsv", line + "\n" + line + "\n" + line + "\n");
  auto corpus = open_corpus(dir.path());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  ASSERT_EQ(corpus->docs.size(), 3u);
  for (uint64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(corpus->docs[i].length, 700000u);
    EXPECT_EQ(corpus->docs[i].offset, i * 700001u);
  }
}

// ---- split_line ----

TEST(Corpus, SplitLine) {
  EXPECT_EQ(split_line("T\tx y").title, "T");
  EXPECT_EQ(split_line("T\tx y").text, "x y");
  EXPECT_EQ(split_line("no tab").title, "no tab");
  EXPECT_EQ(split_line("no tab").text, "");
  EXPECT_EQ(split_line("\t").title, "");
  EXPECT_EQ(split_line("\t").text, "");
  EXPECT_EQ(split_line("a\tb\tc").text, "b\tc");  // only the first TAB splits
}

// ---- read_range ----

TEST(Corpus, ReadRangeReturnsExactBytes) {
  auto bytes = read_range(fixture_root() / "part-00000.tsv", 6, 11);
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  EXPECT_EQ(*bytes, "the quick b");  // "Alpha\t" is 6 bytes, then 11 bytes
}

TEST(Corpus, ReadRangeZeroLengthIsEmpty) {
  auto bytes = read_range(fixture_root() / "part-00000.tsv", 0, 0);
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  EXPECT_TRUE(bytes->empty());
}

TEST(Corpus, ReadRangePastEndIsError) {
  EXPECT_FALSE(read_range(fixture_root() / "part-00001.tsv", 0, 100000).has_value());
  EXPECT_FALSE(read_range(fixture_root() / "part-00001.tsv", 100000, 1).has_value());
}

TEST(Corpus, ReadRangeMissingFileIsError) {
  EXPECT_FALSE(read_range("/nonexistent/mse/part-00000.tsv", 0, 1).has_value());
}

}  // namespace
}  // namespace mse
