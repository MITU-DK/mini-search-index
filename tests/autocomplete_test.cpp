#include "autocomplete.hpp"

#include <gtest/gtest.h>

#include "saved_index.hpp"
#include "synthetic_corpus.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using Terms = std::vector<std::string>;

TEST(Autocomplete, SuggestByDf) {
  const auto saved = save_index(tiny_corpus_dir());
  // "lait" and "lazy" both occur in one doc, so the order between them is alphabetical
  EXPECT_EQ(suggest(*saved->view, "la"), (Terms{"lait", "lazy"}));
  EXPECT_EQ(suggest(*saved->view, "ma"), (Terms{"machine"}));
  // "learning" is in 2 docs, "lait" and "lazy" in 1: the commoner term comes first
  EXPECT_EQ(suggest(*saved->view, "le"), (Terms{"learning"}));
}

TEST(Autocomplete, ShortPrefixEmpty) {
  const auto saved = save_index(tiny_corpus_dir());
  EXPECT_TRUE(suggest(*saved->view, "l").empty());
  EXPECT_TRUE(suggest(*saved->view, "").empty());
}

TEST(Autocomplete, CaseInsensitive) {
  const auto saved = save_index(tiny_corpus_dir());
  EXPECT_EQ(suggest(*saved->view, "LEA"), suggest(*saved->view, "lea"));
  EXPECT_FALSE(suggest(*saved->view, "LEA").empty());
}

TEST(Autocomplete, NoMatch) {
  const auto saved = save_index(tiny_corpus_dir());
  EXPECT_TRUE(suggest(*saved->view, "zz").empty());
  EXPECT_TRUE(suggest(*saved->view, "learnings").empty());
  EXPECT_TRUE(suggest(*saved->view, "a b").empty());  // a space is not part of any term
  EXPECT_TRUE(suggest(*saved->view, std::string(100, 'a')).empty());  // longer than any token
}

TEST(Autocomplete, NonAsciiPrefix) {
  const auto saved = save_index(tiny_corpus_dir());
  EXPECT_EQ(suggest(*saved->view, "caf"), (Terms{"café"}));
  EXPECT_EQ(suggest(*saved->view, "東"), (Terms{"東京"}));
}

TEST(Autocomplete, KLimitsTheResult) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 2000, 300, 2, 31);
  const auto saved = save_index(corpus.path());
  const Terms five = suggest(*saved->view, "w1");
  EXPECT_EQ(five.size(), 5u);
  const Terms two = suggest(*saved->view, "w1", 2);
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[0], five[0]);
  EXPECT_EQ(two[1], five[1]);
  EXPECT_TRUE(suggest(*saved->view, "w1", 0).empty());
}

TEST(Autocomplete, MatchesABruteForceOverAllTerms) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 3000, 600, 2, 32);
  const auto saved = save_index(corpus.path());
  for (const std::string prefix : {"w1", "w12", "w5", "w59", "w"}) {
    std::vector<std::pair<uint64_t, std::string>> all;  // (df, term)
    for (const auto& [term, list] : saved->memory.postings) {
      if (term.starts_with(prefix) && prefix.size() >= kSuggestMinPrefix) {
        all.push_back({list.size(), term});
      }
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    Terms expected;
    for (std::size_t i = 0; i < std::min<std::size_t>(all.size(), kSuggestTopK); ++i) {
      expected.push_back(all[i].second);
    }
    EXPECT_EQ(suggest(*saved->view, prefix), expected) << prefix;
  }
}

}  // namespace
}  // namespace mse
