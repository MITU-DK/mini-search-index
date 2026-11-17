#include "trie.hpp"

#include <gtest/gtest.h>

#include <random>

#include "autocomplete.hpp"
#include "saved_index.hpp"
#include "synthetic_corpus.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using Terms = std::vector<std::string>;

TEST(Trie, KnownAnswersOnTheFixture) {
  const auto saved = save_index(tiny_corpus_dir());
  const Trie trie = Trie::build(*saved->view);
  EXPECT_EQ(trie.suggest("la"), (Terms{"lait", "lazy"}));
  EXPECT_EQ(trie.suggest("ma"), (Terms{"machine"}));
  EXPECT_EQ(trie.suggest("caf"), (Terms{"café"}));
  EXPECT_EQ(trie.suggest("東"), (Terms{"東京"}));
  EXPECT_EQ(trie.suggest("LEA"), trie.suggest("lea"));
  EXPECT_TRUE(trie.suggest("zz").empty());
  EXPECT_TRUE(trie.suggest("learnings").empty());
  EXPECT_GT(trie.memory_bytes(), 0u);
}

TEST(Trie, TrieShortPrefixEmpty) {
  const auto saved = save_index(tiny_corpus_dir());
  const Trie trie = Trie::build(*saved->view);
  EXPECT_TRUE(trie.suggest("l").empty());
  EXPECT_TRUE(trie.suggest("").empty());
}

TEST(Trie, TrieEqualsSortedTable) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 3000, 700, 2, 33);
  const auto saved = save_index(corpus.path());
  const Trie trie = Trie::build(*saved->view);

  // 1000 seeded prefixes: 2 to 5 leading bytes of random terms, plus some that match nothing
  std::mt19937 rng(42);
  for (int i = 0; i < 1000; ++i) {
    const std::string term(saved->view->term_at(rng() % saved->view->num_terms()));
    std::string prefix = term.substr(0, 2 + rng() % 4);
    if (i % 10 == 0) prefix += "q";  // very likely matches no term
    EXPECT_EQ(trie.suggest(prefix), suggest(*saved->view, prefix)) << prefix;
    EXPECT_EQ(trie.suggest(prefix, 3), suggest(*saved->view, prefix, 3)) << prefix;
  }
}

TEST(Trie, AskingForMoreThanWasCachedGivesWhatWasCached) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 2000, 300, 2, 34);
  const auto saved = save_index(corpus.path());
  const Trie trie = Trie::build(*saved->view);
  EXPECT_EQ(trie.suggest("w1", 50).size(), kSuggestTopK);
  EXPECT_TRUE(trie.suggest("w1", 0).empty());
}

TEST(Trie, AnEmptyIndexGivesAnEmptyTrie) {
  TempDir corpus;
  const auto saved = save_index(corpus.path());
  const Trie trie = Trie::build(*saved->view);
  EXPECT_TRUE(trie.suggest("ab").empty());
}

TEST(Trie, NodesTakeMoreMemoryThanTheTermsThemselves) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 3000, 700, 2, 35);
  const auto saved = save_index(corpus.path());
  const Trie trie = Trie::build(*saved->view);
  std::size_t term_bytes = 0;
  for (uint32_t i = 0; i < saved->view->num_terms(); ++i)
    term_bytes += saved->view->term_at(i).size();
  EXPECT_GT(trie.memory_bytes(), term_bytes);
}

}  // namespace
}  // namespace mse
