#include "index_view.hpp"

#include <gtest/gtest.h>

#include <random>
#include <thread>

#include "file_bytes.hpp"
#include "index.hpp"
#include "index_format.hpp"
#include "index_writer.hpp"
#include "indexer.hpp"
#include "parsed_loader.hpp"
#include "searcher.hpp"
#include "synthetic_corpus.hpp"
#include "temp_dir.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

InMemoryIndex build(const fs::path& corpus) {
  auto index = build_index(corpus, {.threads = 2});
  if (!index) throw std::runtime_error(index.error());
  return *std::move(index);
}

// Builds an index, writes it, and keeps its bytes for the view to borrow.
struct Written {
  InMemoryIndex memory;
  Bytes bytes;
  fs::path path;
};

Written write_index_file(const fs::path& corpus, const fs::path& dir, bool compress = true) {
  Written written;
  written.memory = build(corpus);
  written.path = dir / (compress ? "vbyte.bin" : "raw.bin");
  const auto stats = write_index(written.memory, written.path, {.compress = compress});
  if (!stats) throw std::runtime_error(stats.error());
  written.bytes = read_file(written.path);
  return written;
}

IndexView open_or_die(const Bytes& bytes) {
  auto view = IndexView::open(bytes);
  if (!view) throw std::runtime_error(view.error());
  return *view;
}

// ---- same answers as the parsed index ----

void expect_view_matches_parsed(const Written& written) {
  const IndexView view = open_or_die(written.bytes);
  const InMemoryIndex& memory = written.memory;

  EXPECT_EQ(view.num_docs(), memory.docs.size());
  EXPECT_EQ(view.num_terms(), memory.postings.size());
  EXPECT_EQ(view.avg_doc_len(), memory.avg_doc_len());
  EXPECT_EQ(view.corpus_root(), memory.corpus_root);

  std::vector<Posting> postings;
  for (const auto& [term, expected] : memory.postings) {
    const auto df = view.lookup(term, postings);
    ASSERT_TRUE(df.has_value()) << term;
    EXPECT_EQ(*df, expected.size()) << term;
    EXPECT_TRUE(postings == expected) << term;
  }
  for (uint32_t doc = 0; doc < memory.docs.size(); ++doc) {
    EXPECT_EQ(view.doc_length(doc), memory.docs[doc].length);
    EXPECT_EQ(view.doc_title(doc), memory.docs[doc].title);
    EXPECT_EQ(view.doc_ref(doc), memory.docs[doc].ref);
  }
  for (uint32_t part = 0; part < memory.part_names.size(); ++part) {
    EXPECT_EQ(view.part_name(part), memory.part_names[part]);
  }
}

TEST(IndexView, MatchesTheInMemoryIndexOnTheFixture) {
  TempDir dir;
  for (const bool compress : {true, false}) {
    expect_view_matches_parsed(write_index_file(tiny_corpus_dir(), dir.path(), compress));
  }
}

TEST(IndexView, MatchesTheParsedLoaderOnASyntheticCorpus) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 3000, 800, 3, 21);
  for (const bool compress : {true, false}) {
    const Written written = write_index_file(corpus.path(), dir.path(), compress);
    const auto parsed = load_parsed(written.path);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed == written.memory);
    expect_view_matches_parsed(written);
  }
}

TEST(IndexView, UnknownTermsAreNotFound) {
  TempDir dir;
  const Written written = write_index_file(tiny_corpus_dir(), dir.path());
  const IndexView view = open_or_die(written.bytes);
  std::vector<Posting> postings = {{1, 1}};
  for (const char* term : {"", "zzzz", "learnin", "learnings", "A", "!!!"}) {
    EXPECT_FALSE(view.lookup(term, postings).has_value()) << term;
    EXPECT_TRUE(postings.empty()) << term;
  }
}

TEST(IndexView, FindTermAndTermAtAgree) {
  TempDir dir;
  const Written written = write_index_file(tiny_corpus_dir(), dir.path());
  const IndexView view = open_or_die(written.bytes);
  for (uint32_t i = 0; i < view.num_terms(); ++i) {
    const std::string term(view.term_at(i));
    EXPECT_EQ(view.find_term(term), i) << term;
    EXPECT_EQ(view.df_at(i), written.memory.postings.at(term).size()) << term;
  }
  EXPECT_EQ(view.term_at(view.num_terms()), "");  // out of range is safe
  EXPECT_EQ(view.df_at(view.num_terms()), 0u);
  EXPECT_NE(view.find_term("café"), std::nullopt);
  EXPECT_NE(view.find_term("東京"), std::nullopt);
}

TEST(IndexView, PrefixRange) {
  TempDir dir;
  const Written written = write_index_file(tiny_corpus_dir(), dir.path());
  const IndexView view = open_or_die(written.bytes);

  const auto learning = view.prefix_range("lea");
  ASSERT_EQ(learning.second - learning.first, 1u);
  EXPECT_EQ(view.term_at(learning.first), "learning");

  const auto none = view.prefix_range("zz");
  EXPECT_EQ(none.first, none.second);

  const auto a = view.prefix_range("a");  // alpha, au
  ASSERT_EQ(a.second - a.first, 2u);
  EXPECT_EQ(view.term_at(a.first), "alpha");
  EXPECT_EQ(view.term_at(a.first + 1), "au");

  const auto all = view.prefix_range("");
  EXPECT_EQ(all.first, 0u);
  EXPECT_EQ(all.second, view.num_terms());

  const auto cafe = view.prefix_range("caf");
  ASSERT_EQ(cafe.second - cafe.first, 1u);
  EXPECT_EQ(view.term_at(cafe.first), "café");
  EXPECT_EQ(view.prefix_range("東").second - view.prefix_range("東").first, 1u);

  const auto exact = view.prefix_range("learning");  // a term is a prefix of itself
  EXPECT_EQ(exact.second - exact.first, 1u);
}

TEST(IndexView, PrefixRangeCoversExactlyTheMatchingTermsOnALargeIndex) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 2000, 800, 2, 22);
  const Written written = write_index_file(corpus.path(), dir.path());
  const IndexView view = open_or_die(written.bytes);
  for (const std::string prefix : {"w1", "w12", "w7", "w79", "w", "x"}) {
    uint32_t expected = 0;
    for (const auto& entry : written.memory.postings) expected += entry.first.starts_with(prefix);
    const auto range = view.prefix_range(prefix);
    EXPECT_EQ(range.second - range.first, expected) << prefix;
    for (uint32_t i = range.first; i < range.second; ++i) {
      EXPECT_TRUE(view.term_at(i).starts_with(prefix)) << prefix;
    }
  }
}

// ---- the same results through a Searcher ----

TEST(IndexView, SearcherCrossCheck) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 3000, 800, 3, 23);
  for (const bool compress : {true, false}) {
    const Written written = write_index_file(corpus.path(), dir.path(), compress);
    const IndexView view = open_or_die(written.bytes);
    const MemoryReader memory_reader(written.memory);
    Searcher<MemoryReader> from_memory(memory_reader);
    Searcher<IndexView> from_view(view);

    std::mt19937 rng(7);
    for (int query = 0; query < 200; ++query) {
      std::string text;
      const int words = 1 + static_cast<int>(rng() % 3);
      for (int w = 0; w < words; ++w) {
        text += (w > 0 ? " w" : "w") + std::to_string(rng() % 850);  // some words are not indexed
      }
      // Spell-checking is off: only the file-backed reader can do it, and this is about ranking.
      const auto expected = from_memory.search(text, {.spellcheck = false});
      const auto actual = from_view.search(text, {.spellcheck = false});
      EXPECT_TRUE(actual.hits == expected.hits) << "query \"" << text << "\"";
    }
  }
}

TEST(IndexView, SeveralThreadsCanShareOneView) {
  TempDir corpus;
  TempDir dir;
  write_synthetic_corpus(corpus.path(), 2000, 500, 2, 24);
  const Written written = write_index_file(corpus.path(), dir.path());
  const IndexView view = open_or_die(written.bytes);

  Searcher<IndexView> reference(view);
  std::vector<std::vector<SearchResult>> expected;
  for (int q = 0; q < 50; ++q) expected.push_back(reference.search("w" + std::to_string(q)).hits);

  std::vector<int> mismatches(4, 0);
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        Searcher<IndexView> searcher(view);  // one Searcher per thread, one shared view
        for (int q = 0; q < 50; ++q) {
          mismatches[t] += searcher.search("w" + std::to_string(q)).hits != expected[q];
        }
      });
    }
  }
  for (const int count : mismatches) EXPECT_EQ(count, 0);
}

// ---- corrupt files (Review Focus 2) ----

template <class Damage>
void expect_open_fails(const char* what, Damage damage) {
  TempDir dir;
  Bytes bytes = write_index_file(tiny_corpus_dir(), dir.path()).bytes;
  damage(bytes);
  EXPECT_FALSE(IndexView::open(bytes).has_value()) << "accepted: " << what;
}

TEST(IndexView, CorruptFiles) {
  expect_open_fails("wrong magic", [](Bytes& b) { b[3] = std::byte{'X'}; });
  expect_open_fails("version 2", [](Bytes& b) { store_le<uint32_t>(b, 8, 2); });
  expect_open_fails("truncated to 100 bytes", [](Bytes& b) { b.resize(100); });
  expect_open_fails("truncated to half", [](Bytes& b) { b.resize(b.size() / 2); });
  expect_open_fails("empty", [](Bytes& b) { b.clear(); });
  expect_open_fails("file_size off by one",
                    [](Bytes& b) { store_le<uint64_t>(b, 40, load_le<uint64_t>(b, 40) + 1); });
  expect_open_fails("a section offset of 3",
                    [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 4, 3); });
  expect_open_fails("a section past the end",
                    [](Bytes& b) { store_le<uint64_t>(b, 48 + 16 * 6 + 8, uint64_t{1} << 40); });
}

TEST(IndexView, BadRangesAreSafe) {
  TempDir dir;
  Bytes bytes = write_index_file(tiny_corpus_dir(), dir.path()).bytes;
  const auto terms = load_le<uint64_t>(bytes, 48 + 16 * 4);
  const auto refs = load_le<uint64_t>(bytes, 48 + 16 * 1);
  const auto titles = load_le<uint64_t>(bytes, 48 + 16 * 2);

  store_le<uint64_t>(bytes, terms + 16, uint64_t{1} << 40);  // term 0: postings_off past the end
  store_le<uint32_t>(bytes, terms + 24, 1u << 30);           // term 1: string offset past the end
  store_le<uint32_t>(bytes, terms + 24 * 2 + 8, 1000);       // term 2: df larger than its block
  store_le<uint32_t>(bytes, refs + 16 * 2, 99);              // doc 2: part 99
  store_le<uint32_t>(bytes, titles + 8 * 3 + 4, 1u << 30);   // doc 3: title length past the end

  const IndexView view = open_or_die(bytes);  // O(1) checks cannot see any of this
  std::vector<Posting> postings;
  const std::string term0(view.term_at(0));
  EXPECT_FALSE(view.lookup(term0, postings).has_value());
  EXPECT_TRUE(postings.empty());
  EXPECT_EQ(view.term_at(1), "");
  EXPECT_EQ(view.part_name(view.doc_ref(2).part), "");
  EXPECT_EQ(view.part_name(99), "");
  EXPECT_EQ(view.doc_title(3), "");
  EXPECT_EQ(view.doc_title(1000000), "");  // out of range too
  EXPECT_EQ(view.doc_length(1000000), 0u);
  EXPECT_EQ(view.df_at(2), 1000u);  // df is only a stored number until its postings are read
  EXPECT_FALSE(view.lookup(std::string(view.term_at(2)), postings).has_value());

  // Searching a damaged index still answers (with fewer results) and never crashes.
  Searcher<IndexView> searcher(view);
  for (const char* query : {"learning", "alpha", "the quick brown fox", "machine learning"}) {
    (void)searcher.search(query);
  }
}

TEST(IndexView, RawPostingsWithAnImpossibleDocAreNotReturned) {
  TempDir dir;
  Bytes bytes = write_index_file(tiny_corpus_dir(), dir.path(), false).bytes;
  store_le<uint32_t>(bytes, load_le<uint64_t>(bytes, 48 + 16 * 6), 0xFFFFFFFFu);  // first posting
  const IndexView view = open_or_die(bytes);
  std::vector<Posting> postings;
  EXPECT_FALSE(view.lookup(std::string(view.term_at(0)), postings).has_value());
}

TEST(IndexView, AnEmptyIndexOpensAndAnswersNothing) {
  TempDir corpus;
  TempDir dir;
  const Written written = write_index_file(corpus.path(), dir.path());
  const IndexView view = open_or_die(written.bytes);
  EXPECT_EQ(view.num_docs(), 0u);
  EXPECT_EQ(view.num_terms(), 0u);
  EXPECT_EQ(view.avg_doc_len(), 0.0);
  Searcher<IndexView> searcher(view);
  EXPECT_TRUE(searcher.search("anything").hits.empty());
  const auto range = view.prefix_range("a");
  EXPECT_EQ(range.first, range.second);
}

// ---- Index: owns the bytes ----

TEST(Index, OpensInBothModesWithTheSameAnswers) {
  TempDir dir;
  const Written written = write_index_file(tiny_corpus_dir(), dir.path());
  auto mapped = Index::open(written.path, LoadMode::Mmap);
  auto read = Index::open(written.path, LoadMode::Read);
  ASSERT_TRUE(mapped.has_value()) << mapped.error();
  ASSERT_TRUE(read.has_value()) << read.error();

  std::vector<Posting> from_map;
  std::vector<Posting> from_read;
  for (const auto& entry : written.memory.postings) {
    ASSERT_TRUE(mapped->view().lookup(entry.first, from_map).has_value());
    ASSERT_TRUE(read->view().lookup(entry.first, from_read).has_value());
    EXPECT_TRUE(from_map == from_read);
    EXPECT_TRUE(from_map == entry.second);
  }
}

TEST(Index, SurvivesBeingMoved) {
  TempDir dir;
  const Written written = write_index_file(tiny_corpus_dir(), dir.path());
  for (const LoadMode mode : {LoadMode::Mmap, LoadMode::Read}) {
    Index first = *Index::open(written.path, mode);
    Index second = std::move(first);
    std::vector<Posting> postings;
    EXPECT_TRUE(second.view().lookup("learning", postings).has_value());
    EXPECT_EQ(second.view().num_docs(), 6u);
  }
}

TEST(Index, ErrorsForMissingEmptyAndGarbageFiles) {
  TempDir dir;
  for (const LoadMode mode : {LoadMode::Mmap, LoadMode::Read}) {
    EXPECT_FALSE(Index::open(dir.path() / "nope.bin", mode).has_value());
    write_file(dir.path() / "empty.bin", Bytes{});
    EXPECT_FALSE(Index::open(dir.path() / "empty.bin", mode).has_value());
    write_file(dir.path() / "garbage.bin", Bytes(1000, std::byte{0x41}));
    EXPECT_FALSE(Index::open(dir.path() / "garbage.bin", mode).has_value());
  }
}

}  // namespace
}  // namespace mse
