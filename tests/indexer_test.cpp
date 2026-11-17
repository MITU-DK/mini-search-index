#include "indexer.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "corpus.hpp"
#include "temp_dir.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using Postings = std::vector<Posting>;

InMemoryIndex build_fixture(const BuildOptions& options = {}) {
  auto index = build_index(tiny_corpus_dir(), options);
  if (!index) throw std::runtime_error(index.error());
  return *std::move(index);
}

// Fixture docs: 0 Alpha, 1 Beta, 2 Gamma, 3 Delta, 4 (empty), 5 "!!!" (no tokens).

TEST(Indexer, CountsDocs) {
  const InMemoryIndex index = build_fixture();
  ASSERT_EQ(index.docs.size(), 6u);
  EXPECT_EQ(index.docs[1].title, "Beta");
  EXPECT_EQ(index.docs[3].title, "Delta");
  EXPECT_EQ(index.part_names.size(), 2u);
  EXPECT_TRUE(std::filesystem::path(index.corpus_root).is_absolute());
}

TEST(Indexer, DocLengths) {
  const InMemoryIndex index = build_fixture();
  std::vector<uint32_t> lengths;
  for (const DocInfo& doc : index.docs) lengths.push_back(doc.length);
  EXPECT_EQ(lengths, (std::vector<uint32_t>{10, 7, 9, 6, 0, 0}));
}

TEST(Indexer, DocRefsMatchTheCorpus) {
  const InMemoryIndex index = build_fixture();
  auto corpus = open_corpus(tiny_corpus_dir());
  ASSERT_TRUE(corpus.has_value()) << corpus.error();
  ASSERT_EQ(index.docs.size(), corpus->docs.size());
  for (size_t i = 0; i < index.docs.size(); ++i) EXPECT_EQ(index.docs[i].ref, corpus->docs[i]);
}

TEST(Indexer, PostingsForLearning) {
  const InMemoryIndex index = build_fixture();
  EXPECT_EQ(index.postings.at("learning"), (Postings{{1, 2}, {2, 2}}));
  EXPECT_EQ(index.postings.at("machine"), (Postings{{1, 1}, {2, 1}}));
  EXPECT_EQ(index.postings.at("the"), (Postings{{0, 2}}));
}

TEST(Indexer, PostingsSortedByDocWithPositiveTf) {
  const InMemoryIndex index = build_fixture();
  for (const auto& [term, postings] : index.postings) {
    ASSERT_FALSE(postings.empty()) << term;
    for (size_t i = 0; i < postings.size(); ++i) {
      EXPECT_GT(postings[i].tf, 0u) << term;
      if (i > 0) {
        EXPECT_LT(postings[i - 1].doc, postings[i].doc) << term;
      }
    }
  }
}

TEST(Indexer, Utf8Term) {
  const InMemoryIndex index = build_fixture();
  EXPECT_EQ(index.postings.at("東京"), (Postings{{3, 1}}));
  EXPECT_EQ(index.postings.at("café"), (Postings{{3, 1}}));
}

TEST(Indexer, EmptyAndTokenlessDocsHaveNoPostings) {
  const InMemoryIndex index = build_fixture();
  for (const auto& [term, postings] : index.postings) {
    for (const Posting& posting : postings) EXPECT_LT(posting.doc, 4u) << term;
  }
}

TEST(Indexer, Totals) {
  const InMemoryIndex index = build_fixture();
  EXPECT_EQ(index.postings.size(), 27u);
  EXPECT_EQ(index.total_postings(), 29u);
  EXPECT_DOUBLE_EQ(index.avg_doc_len(), 32.0 / 6.0);
}

TEST(Indexer, EmptyCorpus) {
  TempDir dir;
  auto index = build_index(dir.path(), {});
  ASSERT_TRUE(index.has_value()) << index.error();
  EXPECT_TRUE(index->docs.empty());
  EXPECT_TRUE(index->postings.empty());
  EXPECT_EQ(index->avg_doc_len(), 0.0);
  EXPECT_EQ(index->total_postings(), 0u);
}

TEST(Indexer, LineWithoutTabIsAllTitle) {
  TempDir dir;
  write_file(dir.path() / "part-00000.tsv", "just a title\n");
  auto index = build_index(dir.path(), {});
  ASSERT_TRUE(index.has_value()) << index.error();
  ASSERT_EQ(index->docs.size(), 1u);
  EXPECT_EQ(index->docs[0].title, "just a title");
  EXPECT_EQ(index->docs[0].length, 3u);
}

TEST(Indexer, MissingCorpusIsError) {
  EXPECT_FALSE(build_index("/nonexistent/mse", {}).has_value());
}

TEST(Indexer, BatchSizeDoesNotChangeTheIndex) {
  EXPECT_EQ(build_fixture({.batch_size = 1}), build_fixture());
  EXPECT_EQ(build_fixture({.batch_size = 2}), build_fixture());
}

TEST(Indexer, FillsStats) {
  BuildStats stats;
  auto index = build_index(tiny_corpus_dir(), {}, &stats);
  ASSERT_TRUE(index.has_value()) << index.error();
  EXPECT_EQ(stats.docs, 6u);
  EXPECT_EQ(stats.tokens, 32u);
  EXPECT_EQ(stats.terms, 27u);
  EXPECT_EQ(stats.postings, 29u);
  EXPECT_GE(stats.scan_s, 0.0);
  EXPECT_GE(stats.tokenize_s, 0.0);
}

TEST(Indexer, ResolveThreads) {
  EXPECT_GE(resolve_threads("auto"), 1u);
  EXPECT_EQ(resolve_threads("8"), 8u);
  EXPECT_EQ(resolve_threads("1"), 1u);
  for (const char* bad : {"0", "x", "", "-1", "8x", " 8", "+8", "99999999999"}) {
    EXPECT_THROW(resolve_threads(bad), std::invalid_argument) << '"' << bad << '"';
  }
}

}  // namespace
}  // namespace mse
