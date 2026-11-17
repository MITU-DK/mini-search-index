#include "searcher.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "indexer.hpp"
#include "reader.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

static_assert(IndexReader<MemoryReader>);

// Fixture docs: 0 Alpha (fox), 1 Beta, 2 Gamma (both about machine learning), 3 Delta (café 東京),
// 4 and 5 have no words.
const InMemoryIndex& fixture_index() {
  static const InMemoryIndex index = [] {
    auto built = build_index(tiny_corpus_dir(), {});
    if (!built) throw std::runtime_error(built.error());
    return *std::move(built);
  }();
  return index;
}

std::vector<uint32_t> docs_of(const SearchResponse& response) {
  std::vector<uint32_t> docs;
  for (const SearchResult& hit : response.hits) docs.push_back(hit.doc);
  return docs;
}

// ---- MemoryReader ----

TEST(MemoryReader, LooksUpTermsAndDocs) {
  const MemoryReader reader(fixture_index());
  EXPECT_EQ(reader.num_docs(), 6u);
  EXPECT_DOUBLE_EQ(reader.avg_doc_len(), 32.0 / 6.0);
  EXPECT_EQ(reader.doc_length(1), 7u);
  EXPECT_EQ(reader.doc_title(1), "Beta");
  EXPECT_EQ(reader.doc_ref(3).part, 1u);
  EXPECT_EQ(reader.part_name(1), "part-00001.tsv");
  EXPECT_EQ(reader.part_name(99), "");
  EXPECT_TRUE(std::filesystem::path(reader.corpus_root()).is_absolute());

  std::vector<Posting> postings = {{7, 7}};  // must be cleared by lookup
  EXPECT_EQ(reader.lookup("learning", postings), 2u);
  EXPECT_EQ(postings, (std::vector<Posting>{{1, 2}, {2, 2}}));
  EXPECT_EQ(reader.lookup("zzzz", postings), std::nullopt);
  EXPECT_TRUE(postings.empty());
}

// ---- Searcher ----

TEST(Searcher, MachineLearning) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse response = searcher.search("machine learning");
  // Beta and Gamma have the same term counts, and Beta is the shorter doc.
  EXPECT_EQ(docs_of(response), (std::vector<uint32_t>{1, 2}));
  EXPECT_GT(response.hits[0].score, response.hits[1].score);
  EXPECT_EQ(response.terms, (std::vector<std::string>{"machine", "learning"}));
  EXPECT_EQ(response.corrected_query, std::nullopt);
}

TEST(Searcher, DuplicateQueryTermsCountOnce) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse twice = searcher.search("learning learning");
  const SearchResponse once = searcher.search("learning");
  EXPECT_EQ(twice.terms, (std::vector<std::string>{"learning"}));
  EXPECT_EQ(twice.hits, once.hits);
}

TEST(Searcher, CaseAndPunctuationDoNotMatter) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  EXPECT_EQ(searcher.search("Machine, LEARNING!").hits, searcher.search("machine learning").hits);
}

TEST(Searcher, UnknownTermFindsNothing) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse response = searcher.search("zzzz");
  EXPECT_TRUE(response.hits.empty());
  EXPECT_EQ(response.terms, (std::vector<std::string>{"zzzz"}));
}

TEST(Searcher, UnknownTermIsIgnoredNextToKnownOnes) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  EXPECT_EQ(searcher.search("machine zzzz").hits, searcher.search("machine").hits);
}

// Review focus 1: a query with no words is not an error and finds nothing.
TEST(Searcher, QueriesWithoutWordsFindNothing) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  for (const char* query : {"", "   ", "!!!", "\t\n"}) {
    const SearchResponse response = searcher.search(query);
    EXPECT_TRUE(response.hits.empty()) << '"' << query << '"';
    EXPECT_TRUE(response.terms.empty()) << '"' << query << '"';
    EXPECT_EQ(response.corrected_query, std::nullopt) << '"' << query << '"';
  }
}

TEST(Searcher, OverlongQueryWordIsDropped) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse response = searcher.search(std::string(65, 'a') + " fox");
  EXPECT_EQ(response.terms, (std::vector<std::string>{"fox"}));
  EXPECT_EQ(docs_of(response), (std::vector<uint32_t>{0}));
}

TEST(Searcher, FindsNonAsciiWords) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  EXPECT_EQ(docs_of(searcher.search("東京")), (std::vector<uint32_t>{3}));
  EXPECT_EQ(docs_of(searcher.search("CAFÉ")), std::vector<uint32_t>{});  // only ASCII is lowercased
  EXPECT_EQ(docs_of(searcher.search("café")), (std::vector<uint32_t>{3}));
}

TEST(Searcher, TopKLimitsTheHits) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse response = searcher.search("machine learning", {.k = 1});
  EXPECT_EQ(docs_of(response), (std::vector<uint32_t>{1}));
}

TEST(Searcher, RankModeIsUsed) {
  const MemoryReader reader(fixture_index());
  Searcher searcher(reader);
  const SearchResponse bm25 = searcher.search("machine learning");
  const SearchResponse tfidf = searcher.search("machine learning", {.mode = RankMode::TFIDF});
  EXPECT_NE(bm25.hits[0].score, bm25.hits[1].score);    // BM25 sees the length difference
  EXPECT_EQ(tfidf.hits[0].score, tfidf.hits[1].score);  // TF-IDF does not
  EXPECT_EQ(docs_of(tfidf), (std::vector<uint32_t>{1, 2}));
}

TEST(Searcher, EarlierSearchesDoNotChangeLaterOnes) {
  const MemoryReader reader(fixture_index());
  Searcher reused(reader);
  reused.search("machine learning");
  reused.search("fox");
  Searcher fresh(reader);
  EXPECT_EQ(reused.search("learning").hits, fresh.search("learning").hits);
}

TEST(Searcher, EmptyIndexFindsNothing) {
  const InMemoryIndex empty;
  const MemoryReader reader(empty);
  Searcher searcher(reader);
  EXPECT_TRUE(searcher.search("anything").hits.empty());
}

}  // namespace
}  // namespace mse
