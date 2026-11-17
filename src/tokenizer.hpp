// Splits text into lowercase word tokens. The rules are in DESIGN.md §6.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mse {

// Where one token sits in the original text.
struct TokenSpan {
  std::size_t offset;
  std::size_t length;
};

// Word characters are 0-9, A-Z, a-z and any byte >= 0x80; everything else separates words.
// Returns the byte range of every word, in order. Words longer than kMaxTokenLen are skipped.
std::vector<TokenSpan> tokenize_spans(std::string_view text);

// The words of `text` with ASCII lowercased: the same words as tokenize_spans.
std::vector<std::string> tokenize(std::string_view text);

// Lowercases one word. Returns "" if the word is longer than kMaxTokenLen bytes.
std::string normalize_term(std::string_view word);

}  // namespace mse
