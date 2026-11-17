#include "spellcheck.hpp"

#include <gtest/gtest.h>

#include "saved_index.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

// A corpus where each line is one doc made of the given words.
std::unique_ptr<SavedIndex> index_of_docs(const TempDir& corpus,
                                          const std::vector<std::string>& docs) {
  std::string part;
  for (std::size_t i = 0; i < docs.size(); ++i)
    part += "T" + std::to_string(i) + "\t" + docs[i] + "\n";
  write_file(corpus.path() / "part-00000.tsv", part);
  return save_index(corpus.path());
}

TEST(Levenshtein, KnownDistances) {
  EXPECT_EQ(levenshtein("kitten", "sitting"), 3u);
  EXPECT_EQ(levenshtein("", "abc"), 3u);
  EXPECT_EQ(levenshtein("abc", ""), 3u);
  EXPECT_EQ(levenshtein("", ""), 0u);
  EXPECT_EQ(levenshtein("same", "same"), 0u);
  EXPECT_EQ(levenshtein("flaw", "lawn"), 2u);
  EXPECT_EQ(levenshtein("lerning", "learning"), 1u);   // one insertion
  EXPECT_EQ(levenshtein("learnign", "learning"), 2u);  // a swap costs 2: it is not a single edit
  EXPECT_EQ(levenshtein("cat", "cut"), 1u);            // one substitution
}

TEST(Levenshtein, IsSymmetricAndWorksOnBytes) {
  EXPECT_EQ(levenshtein("saturday", "sunday"), levenshtein("sunday", "saturday"));
  // "é" is two bytes, so "café" and "cafe" differ by one substitution and one deletion
  EXPECT_EQ(levenshtein("café", "cafe"), 2u);
}

TEST(Levenshtein, LongInputsBeyondTheTokenLimitStillWork) {
  const std::string a(200, 'a');
  std::string b = a;
  b[100] = 'b';
  EXPECT_EQ(levenshtein(a, b), 1u);
  EXPECT_EQ(levenshtein(a, ""), 200u);
}

TEST(Spell, MaxDistanceRule) {
  EXPECT_EQ(max_edit_distance(""), 1u);
  EXPECT_EQ(max_edit_distance("a"), 1u);
  EXPECT_EQ(max_edit_distance("four"), 1u);
  EXPECT_EQ(max_edit_distance("fives"), 2u);
  EXPECT_EQ(max_edit_distance("learning"), 2u);
}

TEST(Spell, BruteFindsLearning) {
  const auto saved = save_index(spell_corpus_dir());
  EXPECT_EQ(suggest_brute(*saved->view, "lerning"), "learning");
  EXPECT_EQ(suggest_brute(*saved->view, "machne"), "machine");
  EXPECT_EQ(suggest_brute(*saved->view, "learnnig"), "learning");  // 2 edits, allowed for 8 bytes
}

TEST(Spell, AnEligibleTermSuggestsItself) {
  const auto saved = save_index(spell_corpus_dir());
  EXPECT_EQ(suggest_brute(*saved->view, "machine"), "machine");
}

TEST(Spell, BruteRespectsMinDf) {
  const auto saved = save_index(spell_corpus_dir());
  // "vision" is a real term but occurs in only 2 docs, below kSpellMinDf, so it is never offered
  ASSERT_TRUE(saved->view->find_term("vision").has_value());
  EXPECT_LT(saved->view->df_at(*saved->view->find_term("vision")), kSpellMinDf);
  EXPECT_FALSE(suggest_brute(*saved->view, "vison").has_value());
}

TEST(Spell, NothingCloseEnoughGivesNothing) {
  const auto saved = save_index(spell_corpus_dir());
  EXPECT_FALSE(suggest_brute(*saved->view, "zzzzzzzz").has_value());
  EXPECT_EQ(suggest_brute(*saved->view, "lernin"), "learning");  // 2 insertions: allowed
  // 3 edits (a swap counts as 2): too far
  EXPECT_FALSE(suggest_brute(*saved->view, "lernnig").has_value());
  EXPECT_FALSE(suggest_brute(*saved->view, "").has_value());
}

TEST(Spell, ShortTermsAllowOnlyOneEdit) {
  TempDir corpus;
  const auto saved = index_of_docs(corpus, {"cat", "cat", "cat"});
  EXPECT_EQ(suggest_brute(*saved->view, "cut"), "cat");          // 1 edit
  EXPECT_FALSE(suggest_brute(*saved->view, "cuz").has_value());  // 2 edits, but "cuz" is short
}

TEST(Spell, HighestDfWinsAmongTheSameDistance) {
  TempDir corpus;
  const auto saved =
      index_of_docs(corpus, {"care card cart", "care card cart", "care card cart", "care", "care"});
  // care is in 5 docs, card and cart in 3; all are 1 edit from "carx"
  EXPECT_EQ(suggest_brute(*saved->view, "carx"), "care");
}

TEST(Spell, BruteTieBreak) {
  TempDir corpus;
  const auto saved = index_of_docs(corpus, {"card cart", "card cart", "card cart"});
  // the same distance and the same df: the alphabetically first term wins
  EXPECT_EQ(suggest_brute(*saved->view, "carx"), "card");
}

TEST(Spell, SmallestDistanceBeatsHigherDf) {
  TempDir corpus;
  std::vector<std::string> docs(9, "test");
  for (int i = 0; i < 3; ++i) docs[i] += " tested";
  const auto saved = index_of_docs(corpus, docs);  // test: df 9, tested: df 3
  // "testee" is 1 edit from "tested" and 2 edits from "test": the closer one wins
  EXPECT_EQ(suggest_brute(*saved->view, "testee"), "tested");
  // "testd" is 1 edit from both: now the commoner one wins
  EXPECT_EQ(suggest_brute(*saved->view, "testd"), "test");
}

}  // namespace
}  // namespace mse
