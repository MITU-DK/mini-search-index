#include "snippet.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "indexer.hpp"
#include "reader.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using Terms = std::vector<std::string>;

bool is_valid_utf8(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    const auto lead = static_cast<unsigned char>(s[i]);
    std::size_t length = 0;
    if (lead < 0x80)
      length = 1;
    else if ((lead & 0xE0) == 0xC0)
      length = 2;
    else if ((lead & 0xF0) == 0xE0)
      length = 3;
    else if ((lead & 0xF8) == 0xF0)
      length = 4;
    else
      return false;
    if (i + length > s.size()) return false;
    for (std::size_t j = 1; j < length; ++j) {
      if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) return false;
    }
    i += length;
  }
  return true;
}

std::string highlighted(const Snippet& snippet, std::size_t i) {
  return snippet.text.substr(snippet.highlights[i].offset, snippet.highlights[i].length);
}

// ---- make_snippet ----

TEST(Snippet, ShortTextIsKeptWhole) {
  const Snippet s = make_snippet("the quick brown fox jumps over the lazy dog", Terms{"fox"});
  EXPECT_EQ(s.text, "the quick brown fox jumps over the lazy dog");
  ASSERT_EQ(s.highlights.size(), 1u);
  EXPECT_EQ(highlighted(s, 0), "fox");
  EXPECT_EQ(s.highlights[0].offset, 16u);
}

TEST(Snippet, WindowStarts40BytesBeforeTheFirstMatchAndIs160Long) {
  // "fox" is at byte 101, so the window is bytes [61, 221).
  const std::string text = std::string(100, 'x') + " fox " + std::string(200, 'y');
  const Snippet s = make_snippet(text, Terms{"fox"});
  EXPECT_EQ(s.text, std::string(39, 'x') + " fox " + std::string(116, 'y'));
  ASSERT_EQ(s.highlights.size(), 1u);
  EXPECT_EQ(s.highlights[0].offset, 40u);
  EXPECT_EQ(highlighted(s, 0), "fox");
}

TEST(Snippet, NoMatchUsesTheStartOfTheText) {
  const Snippet s = make_snippet(std::string(300, 'x'), Terms{"fox"});
  EXPECT_EQ(s.text, std::string(160, 'x'));
  EXPECT_TRUE(s.highlights.empty());
}

TEST(Snippet, EmptyTextAndNoTerms) {
  EXPECT_TRUE(make_snippet("", Terms{"fox"}).text.empty());
  const Snippet s = make_snippet("a fox", Terms{});
  EXPECT_EQ(s.text, "a fox");
  EXPECT_TRUE(s.highlights.empty());
}

TEST(Snippet, CollapsesWhitespaceAndMovesHighlights) {
  const Snippet s = make_snippet("a  \n b fox", Terms{"fox"});
  EXPECT_EQ(s.text, "a b fox");
  ASSERT_EQ(s.highlights.size(), 1u);
  EXPECT_EQ(s.highlights[0].offset, 4u);
  EXPECT_EQ(highlighted(s, 0), "fox");
}

TEST(Snippet, TrimsWhitespaceAtBothEnds) {
  const Snippet s = make_snippet("  \n fox \t", Terms{"fox"});
  EXPECT_EQ(s.text, "fox");
  EXPECT_EQ(s.highlights[0].offset, 0u);
}

TEST(Snippet, HighlightsKeepTheOriginalCaseAndEveryMatch) {
  const Snippet s = make_snippet("Machine Learning is machine fun", Terms{"machine", "learning"});
  ASSERT_EQ(s.highlights.size(), 3u);
  EXPECT_EQ(highlighted(s, 0), "Machine");
  EXPECT_EQ(highlighted(s, 1), "Learning");
  EXPECT_EQ(highlighted(s, 2), "machine");
}

TEST(Snippet, MatchesWholeWordsOnly) {
  const Snippet s = make_snippet("learning", Terms{"learn"});
  EXPECT_TRUE(s.highlights.empty());
}

TEST(Snippet, DoesNotHighlightAWordThatTheWindowStartCutInHalf) {
  // The window starts at byte 4, in the middle of "learning". "ning" is not a word of the text.
  const std::string text = "learning" + std::string(36, ' ') + "fox";
  const Snippet s = make_snippet(text, Terms{"ning", "fox"});
  EXPECT_EQ(s.text, "ning fox");
  ASSERT_EQ(s.highlights.size(), 1u);
  EXPECT_EQ(highlighted(s, 0), "fox");
}

// Review focus 3: never cut a UTF-8 character in half.
TEST(Snippet, WindowStartMovesToACharacterBoundary) {
  // "fox" is at byte 401, so the raw start (361) is inside a 2-byte "é".
  std::string text;
  for (int i = 0; i < 200; ++i) text += "é";
  text += " fox";
  const Snippet s = make_snippet(text, Terms{"fox"});
  EXPECT_TRUE(is_valid_utf8(s.text));
  ASSERT_EQ(s.highlights.size(), 1u);
  EXPECT_EQ(highlighted(s, 0), "fox");
}

TEST(Snippet, WindowEndMovesToACharacterBoundary) {
  // The raw end (byte 160) is inside a 2-byte "é", so the window ends one byte earlier.
  std::string text = "fox";
  for (int i = 0; i < 200; ++i) text += "é";
  const Snippet s = make_snippet(text, Terms{"fox"});
  EXPECT_TRUE(is_valid_utf8(s.text));
  EXPECT_EQ(s.text.size(), 159u);
}

TEST(Snippet, HighlightsNonAsciiTerms) {
  const Snippet s = make_snippet("Café au lait in 東京", Terms{"café", "東京"});
  ASSERT_EQ(s.highlights.size(), 2u);
  EXPECT_EQ(highlighted(s, 0), "Café");
  EXPECT_EQ(highlighted(s, 1), "東京");
}

// ---- snippet_for (reads the doc's line from the corpus files) ----

const InMemoryIndex& fixture_index() {
  static const InMemoryIndex index = [] {
    auto built = build_index(tiny_corpus_dir(), {});
    if (!built) throw std::runtime_error(built.error());
    return *std::move(built);
  }();
  return index;
}

TEST(Snippet, SnippetForReadsTheCorpusFile) {
  const MemoryReader reader(fixture_index());
  auto s = snippet_for(reader, 0, Terms{"fox"});
  ASSERT_TRUE(s.has_value()) << s.error();
  EXPECT_EQ(s->text, "the quick brown fox jumps over the lazy dog");  // text only, not the title
  ASSERT_EQ(s->highlights.size(), 1u);
  EXPECT_EQ(highlighted(*s, 0), "fox");
}

TEST(Snippet, SnippetForUsesThePartFileOfTheDoc) {
  const MemoryReader reader(fixture_index());
  auto s = snippet_for(reader, 3, Terms{"café"});  // doc 3 is in the second part file
  ASSERT_TRUE(s.has_value()) << s.error();
  EXPECT_EQ(s->text, "Café au lait in 東京");
  EXPECT_EQ(highlighted(*s, 0), "Café");
}

TEST(Snippet, SnippetForDocWithoutTextGivesAnEmptySnippet) {
  const MemoryReader reader(fixture_index());
  auto s = snippet_for(reader, 4, Terms{"fox"});
  ASSERT_TRUE(s.has_value()) << s.error();
  EXPECT_TRUE(s->text.empty());
}

TEST(Snippet, SnippetForCorpusRootOverride) {
  InMemoryIndex moved = fixture_index();
  moved.corpus_root = "/nonexistent/mse";
  const MemoryReader reader(moved);
  EXPECT_FALSE(snippet_for(reader, 0, Terms{"fox"}).has_value());
  auto s = snippet_for(reader, 0, Terms{"fox"}, tiny_corpus_dir().string());
  ASSERT_TRUE(s.has_value()) << s.error();
  EXPECT_EQ(highlighted(*s, 0), "fox");
}

TEST(Snippet, SnippetForBadInputIsAnError) {
  const MemoryReader reader(fixture_index());
  EXPECT_FALSE(snippet_for(reader, 6, Terms{"fox"}).has_value());  // only docs 0-5 exist

  InMemoryIndex bad = fixture_index();
  bad.part_names[0] = "../../etc/passwd";  // a part name must be a plain file name
  EXPECT_FALSE(snippet_for(MemoryReader(bad), 0, Terms{"fox"}).has_value());

  InMemoryIndex bad_part = fixture_index();
  bad_part.docs[0].ref.part = 99;
  EXPECT_FALSE(snippet_for(MemoryReader(bad_part), 0, Terms{"fox"}).has_value());
}

}  // namespace
}  // namespace mse
