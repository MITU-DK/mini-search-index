#include "tokenizer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mse {
namespace {

using V = std::vector<std::string>;

TEST(Tokenizer, LowercasesAndSplits) {
  EXPECT_EQ(tokenize("Hello, World! x2"), (V{"hello", "world", "x2"}));
}

TEST(Tokenizer, TabAndUnderscoreSplit) { EXPECT_EQ(tokenize("a\tb_c"), (V{"a", "b", "c"})); }

TEST(Tokenizer, NonWordAsciiNextToLettersAndDigitsSplits) {
  // '@' '[' '`' '{' sit just outside A-Z/a-z, and '/' ':' just outside 0-9.
  EXPECT_EQ(tokenize("a@b[c`d{e/f:g"), (V{"a", "b", "c", "d", "e", "f", "g"}));
}

TEST(Tokenizer, EmptyAndPunct) {
  EXPECT_TRUE(tokenize("").empty());
  EXPECT_TRUE(tokenize("!!! ...").empty());
}

TEST(Tokenizer, Utf8KeptIntact) { EXPECT_EQ(tokenize("Café 東京"), (V{"café", "東京"})); }

TEST(Tokenizer, DropsOverlongTokens) {
  EXPECT_EQ(tokenize(std::string(65, 'a') + " ok"), (V{"ok"}));
}

TEST(Tokenizer, KeepsExactly64) { EXPECT_EQ(tokenize(std::string(64, 'a')).size(), 1u); }

TEST(Tokenizer, NormalizeTerm) { EXPECT_EQ(normalize_term("MaChInE"), "machine"); }

TEST(Tokenizer, NormalizeTermTooLongIsEmpty) {
  EXPECT_EQ(normalize_term(std::string(65, 'a')), "");
}

TEST(Tokenizer, SpansGiveByteRanges) {
  // "café" is 5 bytes, so it starts at byte 4 and is 5 bytes long.
  const auto spans = tokenize_spans("Hi, café!");
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].offset, 0u);
  EXPECT_EQ(spans[0].length, 2u);
  EXPECT_EQ(spans[1].offset, 4u);
  EXPECT_EQ(spans[1].length, 5u);
}

TEST(Tokenizer, SpansSkipOverlongTokens) {
  const auto spans = tokenize_spans(std::string(65, 'a') + " ok");
  ASSERT_EQ(spans.size(), 1u);
  EXPECT_EQ(spans[0].offset, 66u);
  EXPECT_EQ(spans[0].length, 2u);
}

TEST(Tokenizer, SpansOfNothingAreEmpty) {
  EXPECT_TRUE(tokenize_spans("").empty());
  EXPECT_TRUE(tokenize_spans(" !! ").empty());
}

}  // namespace
}  // namespace mse
