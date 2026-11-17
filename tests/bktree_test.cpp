#include "bktree.hpp"

#include <gtest/gtest.h>

#include <random>

#include "saved_index.hpp"
#include "searcher.hpp"
#include "spellcheck.hpp"
#include "synthetic_corpus.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using TermList = std::vector<std::pair<std::string_view, uint32_t>>;

// One random edit: insert, delete, substitute or swap two neighbours.
std::string misspell(std::string word, std::mt19937& rng) {
  const std::string letters = "abcdefghijklmnopqrstuvwxyz0123456789";
  const auto letter = [&] { return letters[rng() % letters.size()]; };
  switch (rng() % 4) {
    case 0:
      word.insert(word.begin() + static_cast<std::ptrdiff_t>(rng() % (word.size() + 1)), letter());
      break;
    case 1:
      if (word.size() > 1) word.erase(rng() % word.size(), 1);
      break;
    case 2:
      word[rng() % word.size()] = letter();
      break;
    default:
      if (word.size() > 1) std::swap(word[rng() % (word.size() - 1)], word[0]);
      break;
  }
  return word;
}

TEST(BkTree, NoTermsGiveAnEmptyTree) {
  const FlatBkTree tree = build_bktree({});
  EXPECT_TRUE(tree.nodes.empty());
  EXPECT_TRUE(tree.edges.empty());
}

TEST(BkTree, FlattensBreadthFirstWithSortedEdges) {
  const TermList terms = {{"book", 10}, {"books", 11}, {"boo", 12}, {"bike", 13}, {"cook", 14}};
  const FlatBkTree tree = build_bktree(terms);

  ASSERT_EQ(tree.nodes.size(), terms.size());
  EXPECT_EQ(tree.nodes[0].term_index, 10u);  // the first term is the root
  for (const SpellNode& node : tree.nodes) {
    ASSERT_LE(std::size_t{node.first_edge} + node.edge_count, tree.edges.size());
    for (uint32_t i = 0; i < node.edge_count; ++i) {
      const SpellEdge& edge = tree.edges[node.first_edge + i];
      EXPECT_LT(edge.child_node, tree.nodes.size());
      if (i > 0) {
        EXPECT_LT(tree.edges[node.first_edge + i - 1].distance, edge.distance);
      }
    }
  }
  // breadth first: every child comes after its parent, and a node's children are consecutive
  // in the order of their edges
  uint32_t next_child = 1;
  for (const SpellNode& node : tree.nodes) {
    for (uint32_t i = 0; i < node.edge_count; ++i) {
      EXPECT_EQ(tree.edges[node.first_edge + i].child_node, next_child++);
    }
  }
  EXPECT_EQ(next_child, terms.size());  // one edge per non-root node
  EXPECT_EQ(tree.edges.size(), terms.size() - 1);
}

TEST(BkTree, EveryEdgeDistanceIsTheRealDistanceToTheChild) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 800, 150, 1, 41);
  const auto saved = save_index(corpus.path());
  TermList terms;
  for (uint32_t i = 0; i < saved->view->num_terms(); ++i) {
    if (saved->view->df_at(i) >= kSpellMinDf) terms.push_back({saved->view->term_at(i), i});
  }
  ASSERT_GT(terms.size(), 50u);
  const FlatBkTree tree = build_bktree(terms);
  ASSERT_EQ(tree.nodes.size(), terms.size());
  for (const SpellNode& node : tree.nodes) {
    for (uint32_t i = 0; i < node.edge_count; ++i) {
      const SpellEdge& edge = tree.edges[node.first_edge + i];
      EXPECT_EQ(edge.distance,
                levenshtein(saved->view->term_at(node.term_index),
                            saved->view->term_at(tree.nodes[edge.child_node].term_index)));
    }
  }
}

TEST(BkTree, BkTreeEqualsBrute) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 4000, 900, 2, 42);
  const auto saved = save_index(corpus.path());
  ASSERT_GT(saved->view->num_terms(), 500u);

  std::mt19937 rng(42);
  int answered = 0;
  for (int i = 0; i < 500; ++i) {
    const std::string source(saved->view->term_at(rng() % saved->view->num_terms()));
    std::string word = misspell(source, rng);
    if (i % 5 == 0) word = misspell(word, rng);  // some are 2 edits away
    const auto expected = suggest_brute(*saved->view, word);
    EXPECT_EQ(saved->view->spell_suggest(word), expected) << word;
    answered += expected.has_value();
  }
  EXPECT_GT(answered, 100);  // the test is meaningless if almost nothing gets a suggestion
}

TEST(BkTree, TheTreeComparesWithFewerTermsThanBruteForce) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 4000, 900, 2, 45);
  const auto saved = save_index(corpus.path());
  uint64_t tree_calls = 0;
  uint64_t brute_calls = 0;
  const auto header = decode_header(saved->bytes);
  ASSERT_TRUE(header.has_value());
  const uint64_t nodes = header->section(Section::SpellNodes).size / kSpellNodeSize;
  for (const char* word : {"w12x", "w5", "wq3", "w701", "w3w"}) {
    const uint64_t before = tree_calls;
    EXPECT_EQ(saved->view->spell_suggest(word, &tree_calls),
              suggest_brute(*saved->view, word, &brute_calls));
    EXPECT_GT(tree_calls, before) << word;          // the root is always compared
    EXPECT_LE(tree_calls - before, nodes) << word;  // and no node is compared twice
  }
  EXPECT_LT(tree_calls, brute_calls);
}

TEST(BkTree, AgreesWithBruteOnTheSpellFixture) {
  const auto saved = save_index(spell_corpus_dir());
  for (const char* word : {"lerning", "machne", "learnnig", "vison", "zzzzzzzz", "machine", "mach",
                           "a", "learningg", "lernin"}) {
    EXPECT_EQ(saved->view->spell_suggest(word), suggest_brute(*saved->view, word)) << word;
  }
  EXPECT_EQ(saved->view->spell_suggest("lerning"), "learning");
  EXPECT_EQ(saved->view->spell_suggest(""), std::nullopt);
}

TEST(BkTree, SpellSectionEmptyWhenNoEligibleTerms) {
  TempDir corpus;
  // every word occurs in one doc only, below kSpellMinDf
  write_file(corpus.path() / "part-00000.tsv", "A\tone two\nB\tthree four\nC\tfive six\n");
  const auto saved = save_index(corpus.path());
  const auto header = decode_header(saved->bytes);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->section(Section::SpellNodes).size, 0u);
  EXPECT_EQ(header->section(Section::SpellEdges).size, 0u);
  EXPECT_EQ(saved->view->spell_suggest("onee"), std::nullopt);
}

TEST(BkTree, SpellSectionsExistWhenTermsAreEligible) {
  const auto saved = save_index(spell_corpus_dir());
  const auto header = decode_header(saved->bytes);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->section(Section::SpellNodes).size, 2u * kSpellNodeSize);  // machine, learning
  EXPECT_EQ(header->section(Section::SpellEdges).size, 1u * kSpellEdgeSize);
}

TEST(BkTree, BuildSpellOffLeavesTheSectionsEmpty) {
  const auto saved = save_index(spell_corpus_dir(), {.build_spell = false});
  const auto header = decode_header(saved->bytes);
  ASSERT_TRUE(header.has_value());
  EXPECT_EQ(header->section(Section::SpellNodes).size, 0u);
  EXPECT_EQ(saved->view->spell_suggest("lerning"), std::nullopt);
}

TEST(BkTree, WriterReportsTheTimeToBuildTheTree) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 3000, 800, 1, 43);
  auto built = build_index(corpus.path(), {.threads = 1});
  ASSERT_TRUE(built.has_value());
  TempDir out;
  const auto with = write_index(*built, out.path() / "a.bin", {});
  const auto without = write_index(*built, out.path() / "b.bin", {.build_spell = false});
  ASSERT_TRUE(with.has_value() && without.has_value());
  EXPECT_GT(with->spell_build_s, 0.0);
  EXPECT_EQ(without->spell_build_s, 0.0);
  EXPECT_GT(with->file_bytes, without->file_bytes);
  EXPECT_GT(with->spell_bytes, 0u);
  EXPECT_EQ(without->spell_bytes, 0u);
  // the file grew by the spell sections, plus at most the padding that aligns them
  EXPECT_GE(with->file_bytes - without->file_bytes, with->spell_bytes);
  EXPECT_LT(with->file_bytes - without->file_bytes, with->spell_bytes + 2 * kSectionAlign);
}

TEST(BkTree, RawAndCompressedFilesGiveTheSameSuggestions) {
  const auto compressed = save_index(spell_corpus_dir(), {.compress = true});
  const auto raw = save_index(spell_corpus_dir(), {.compress = false});
  for (const char* word : {"lerning", "machne", "zzzz"}) {
    EXPECT_EQ(compressed->view->spell_suggest(word), raw->view->spell_suggest(word)) << word;
  }
}

// A damaged file must not make a suggestion loop forever or read outside the file.
TEST(BkTree, DamagedSpellSectionsAreSafe) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 2000, 300, 1, 44);
  const auto saved = save_index(corpus.path());
  const auto header = decode_header(saved->bytes);
  ASSERT_TRUE(header.has_value());
  const SectionRef edges = header->section(Section::SpellEdges);
  const SectionRef nodes = header->section(Section::SpellNodes);
  ASSERT_GT(edges.size, 0u);

  Bytes cycles = saved->bytes;  // every edge leads back to the root
  for (uint64_t at = edges.offset; at < edges.offset + edges.size; at += kSpellEdgeSize) {
    store_le<uint32_t>(cycles, at + 4, 0);
  }
  Bytes wild = saved->bytes;  // every edge leads outside the node table
  for (uint64_t at = edges.offset; at < edges.offset + edges.size; at += kSpellEdgeSize) {
    store_le<uint32_t>(wild, at + 4, 0xFFFFFFF0u);
  }
  Bytes bad_terms = saved->bytes;  // every node names a term that does not exist
  for (uint64_t at = nodes.offset; at < nodes.offset + nodes.size; at += kSpellNodeSize) {
    store_le<uint32_t>(bad_terms, at, 0xFFFFFFF0u);
  }
  Bytes bad_ranges = saved->bytes;  // every node's edge list runs past the end of the table
  for (uint64_t at = nodes.offset; at < nodes.offset + nodes.size; at += kSpellNodeSize) {
    store_le<uint32_t>(bad_ranges, at + 4, 0xFFFFFFF0u);
    store_le<uint32_t>(bad_ranges, at + 8, 0xFFFFFFF0u);
  }
  for (const Bytes* damaged : {&cycles, &wild, &bad_terms, &bad_ranges}) {
    const auto view = IndexView::open(*damaged);
    ASSERT_TRUE(view.has_value());
    for (const char* word : {"w1x", "w22", "w100", "q"}) {
      (void)view->spell_suggest(word);  // only has to return, without crashing under the sanitizers
    }
  }
}

TEST(DidYouMean, DidYouMean) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const SearchResponse response = searcher.search("machne lerning");
  EXPECT_EQ(response.corrected_query, "machine learning");
  EXPECT_FALSE(response.hits.empty());
  EXPECT_EQ(response.terms, (std::vector<std::string>{"machine", "learning"}));
}

TEST(DidYouMean, OnlyTheMisspelledWordIsReplaced) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const SearchResponse response = searcher.search("Machine lerning");  // the first is right
  EXPECT_EQ(response.corrected_query, "machine learning");
  EXPECT_FALSE(response.hits.empty());
}

TEST(DidYouMean, NoCorrectionWhenAllKnown) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  EXPECT_EQ(searcher.search("machine learning").corrected_query, std::nullopt);
  EXPECT_EQ(searcher.search("vision").corrected_query, std::nullopt);  // known, if rare
}

TEST(DidYouMean, NoCorrectionWhenNothingIsClose) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const SearchResponse response = searcher.search("zzzzzzzz");
  EXPECT_EQ(response.corrected_query, std::nullopt);
  EXPECT_TRUE(response.hits.empty());
}

TEST(DidYouMean, CanBeSwitchedOff) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const SearchResponse response = searcher.search("machne lerning", {.spellcheck = false});
  EXPECT_EQ(response.corrected_query, std::nullopt);
  EXPECT_TRUE(response.hits.empty());
}

TEST(DidYouMean, TwoMisspellingsOfTheSameWordGiveOneTerm) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const SearchResponse response = searcher.search("lerning learnin");
  EXPECT_EQ(response.corrected_query, "learning");
  EXPECT_EQ(response.terms, (std::vector<std::string>{"learning"}));
}

TEST(DidYouMean, ReadersWithoutASpellIndexJustSearch) {
  const auto saved = save_index(spell_corpus_dir());
  const MemoryReader reader(saved->memory);
  Searcher<MemoryReader> searcher(reader);
  const SearchResponse response = searcher.search("machne lerning");
  EXPECT_EQ(response.corrected_query, std::nullopt);
  EXPECT_TRUE(response.hits.empty());
}

}  // namespace
}  // namespace mse
