#include "ranker.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace mse {
namespace {

using Results = std::vector<SearchResult>;

// Four hand-made docs. Lengths are {10, 6, 9, 3}, so the average is 7.
// The expected scores below were computed independently in Python from DESIGN.md §7.
constexpr uint32_t kNumDocs = 4;
constexpr double kAvgLen = 7.0;
uint32_t length_of_doc(uint32_t doc) {
  constexpr uint32_t lengths[] = {10, 6, 9, 3};
  return lengths[doc];
}

Results rank_four_docs(const std::vector<QueryTerm>& terms, RankMode mode = RankMode::BM25,
                       std::size_t k = 10) {
  Accumulator acc(kNumDocs);
  return rank(terms, kNumDocs, kAvgLen, length_of_doc, mode, k, acc);
}

std::vector<uint32_t> docs_of(const Results& results) {
  std::vector<uint32_t> docs;
  for (const SearchResult& r : results) docs.push_back(r.doc);
  return docs;
}

// ---- scoring formulas ----

TEST(Ranker, Bm25MatchesHandComputed) {
  const Results results = rank_four_docs({{2, {{1, 2}, {3, 1}}}});
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc, 1u);  // tf 2 beats tf 1, though doc 3 is shorter
  EXPECT_NEAR(results[0].score, 0.992974, 1e-5);
  EXPECT_EQ(results[1].doc, 3u);
  EXPECT_NEAR(results[1].score, 0.904616, 1e-5);
}

TEST(Ranker, TfIdfMatchesHandComputed) {
  const Results results = rank_four_docs({{2, {{1, 2}, {3, 1}}}}, RankMode::TFIDF);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc, 1u);  // TF-IDF ignores doc length
  EXPECT_NEAR(results[0].score, 1.1736, 1e-5);
  EXPECT_EQ(results[1].doc, 3u);
  EXPECT_NEAR(results[1].score, 0.693147, 1e-5);
}

TEST(Ranker, TermsAddUp) {
  // Doc 1 matches both terms: 0.992974 (df 2, tf 2) + 1.278702 (df 1, tf 1).
  const Results results = rank_four_docs({{2, {{1, 2}, {3, 1}}}, {1, {{1, 1}}}});
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].doc, 1u);
  EXPECT_NEAR(results[0].score, 2.271676, 1e-5);
}

TEST(Ranker, Bm25SaturatesInTf) {
  const double idf = bm25_idf(4, 2);
  const double k1_plus_1 = kBm25K1 + 1.0;
  double previous = 0;
  for (uint32_t tf : {1u, 2u, 5u, 20u, 1000u}) {
    const double score = bm25_term_score(idf, tf, 7, kAvgLen);
    EXPECT_GT(score, previous);
    EXPECT_LT(score, idf * k1_plus_1);  // never reaches idf * (k1 + 1)
    previous = score;
  }
}

TEST(Ranker, Bm25LongerDocScoresLower) {
  const double idf = bm25_idf(4, 2);
  EXPECT_GT(bm25_term_score(idf, 2, 3, kAvgLen), bm25_term_score(idf, 2, 7, kAvgLen));
  EXPECT_GT(bm25_term_score(idf, 2, 7, kAvgLen), bm25_term_score(idf, 2, 30, kAvgLen));
}

// ---- ordering and top-k ----

TEST(Ranker, TieBreakLowerDocFirst) {
  // Docs 3 and 1 get identical scores, and doc 3 is touched first.
  Accumulator acc(4);
  const std::vector<QueryTerm> terms = {{1, {{3, 1}}}, {1, {{1, 1}}}};
  const Results results = rank(terms, 4, 5.0, [](uint32_t) { return 5u; }, RankMode::BM25, 10, acc);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].score, results[1].score);
  EXPECT_EQ(docs_of(results), (std::vector<uint32_t>{1, 3}));
}

TEST(Ranker, TopKTruncates) {
  // Five docs of equal length; a higher tf means a higher score.
  Accumulator acc(5);
  const std::vector<QueryTerm> terms = {{5, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}}}};
  const auto same_length = [](uint32_t) { return 5u; };
  const Results top2 = rank(terms, 5, 5.0, same_length, RankMode::BM25, 2, acc);
  EXPECT_EQ(docs_of(top2), (std::vector<uint32_t>{4, 3}));
  const Results top10 = rank(terms, 5, 5.0, same_length, RankMode::BM25, 10, acc);
  EXPECT_EQ(docs_of(top10), (std::vector<uint32_t>{4, 3, 2, 1, 0}));
  EXPECT_TRUE(rank(terms, 5, 5.0, same_length, RankMode::BM25, 0, acc).empty());
}

// ---- edge cases ----

TEST(Ranker, NoTermsNoResults) { EXPECT_TRUE(rank_four_docs({}).empty()); }

TEST(Ranker, TermWithZeroDfIsIgnored) { EXPECT_TRUE(rank_four_docs({{0, {}}}).empty()); }

TEST(Ranker, ZeroDocsNoResults) {
  Accumulator acc(0);
  const std::vector<QueryTerm> terms = {{1, {{0, 1}}}};
  EXPECT_TRUE(rank(terms, 0, 0.0, [](uint32_t) { return 0u; }, RankMode::BM25, 10, acc).empty());
}

TEST(Ranker, AvgLenZeroGivesFiniteScores) {
  Accumulator acc(4);
  const std::vector<QueryTerm> terms = {{1, {{2, 1}}}};
  const Results results = rank(terms, 4, 0.0, [](uint32_t) { return 0u; }, RankMode::BM25, 10, acc);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(std::isfinite(results[0].score));
  EXPECT_NEAR(results[0].score, 2.037492, 1e-5);  // doc length / avg length counts as 0
}

TEST(Ranker, TfIdfTermInEveryDocScoresZeroAndIsExcluded) {
  const std::vector<QueryTerm> terms = {{4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}}};
  EXPECT_TRUE(rank_four_docs(terms, RankMode::TFIDF).empty());
}

TEST(Ranker, PostingPastNumDocsIsIgnored) {
  // A corrupt index could hold a doc id >= num_docs. It must not be read or written.
  const Results results = rank_four_docs({{2, {{1, 1}, {99, 1}}}});
  EXPECT_EQ(docs_of(results), (std::vector<uint32_t>{1}));
}

TEST(Ranker, ZeroTfPostingIsIgnored) {
  const Results bm25 = rank_four_docs({{2, {{1, 0}, {3, 1}}}});
  EXPECT_EQ(docs_of(bm25), (std::vector<uint32_t>{3}));
  const Results tfidf = rank_four_docs({{2, {{1, 0}, {3, 1}}}}, RankMode::TFIDF);
  EXPECT_EQ(docs_of(tfidf), (std::vector<uint32_t>{3}));
}

// ---- Accumulator ----

TEST(Accumulator, AddsScoresAndListsEachDocOnce) {
  Accumulator acc(5);
  acc.add(3, 1.5f);
  acc.add(3, 0.5f);
  acc.add(1, 1.0f);
  EXPECT_EQ(acc.score(3), 2.0f);
  EXPECT_EQ(acc.score(1), 1.0f);
  EXPECT_EQ(acc.score(0), 0.0f);
  EXPECT_EQ(std::vector<uint32_t>(acc.touched().begin(), acc.touched().end()),
            (std::vector<uint32_t>{3, 1}));
}

TEST(Accumulator, ResetClearsEverything) {
  Accumulator acc(5);
  acc.add(3, 1.5f);
  acc.reset();
  EXPECT_EQ(acc.score(3), 0.0f);
  EXPECT_TRUE(acc.touched().empty());
}

// Review focus 5: leftover scores from one query must never leak into the next.
TEST(Ranker, AccumulatorResetNoStaleScores) {
  const std::vector<QueryTerm> query_a = {{2, {{0, 1}, {1, 1}}}};
  const std::vector<QueryTerm> query_b = {{2, {{1, 3}, {2, 1}}}};

  Accumulator reused(kNumDocs);
  rank(query_a, kNumDocs, kAvgLen, length_of_doc, RankMode::BM25, 10, reused);
  const Results after_a =
      rank(query_b, kNumDocs, kAvgLen, length_of_doc, RankMode::BM25, 10, reused);

  Accumulator fresh(kNumDocs);
  const Results alone = rank(query_b, kNumDocs, kAvgLen, length_of_doc, RankMode::BM25, 10, fresh);
  EXPECT_EQ(after_a, alone);
}

}  // namespace
}  // namespace mse
