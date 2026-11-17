#include "tokenizer.hpp"

#include <array>
#include <cstddef>

#include "constants.hpp"

namespace mse {
namespace {

constexpr std::array<bool, 256> make_word_table() {
  std::array<bool, 256> table{};
  for (int c = '0'; c <= '9'; ++c) table[c] = true;
  for (int c = 'A'; c <= 'Z'; ++c) table[c] = true;
  for (int c = 'a'; c <= 'z'; ++c) table[c] = true;
  for (int c = 0x80; c <= 0xFF; ++c) table[c] = true;  // UTF-8 bytes are never split
  return table;
}

constexpr std::array<bool, 256> kIsWord = make_word_table();

bool is_word(char c) { return kIsWord[static_cast<unsigned char>(c)]; }

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

}  // namespace

std::string normalize_term(std::string_view word) {
  if (word.size() > kMaxTokenLen) return {};
  std::string out(word);
  for (char& c : out) c = to_lower(c);
  return out;
}

std::vector<TokenSpan> tokenize_spans(std::string_view text) {
  std::vector<TokenSpan> spans;
  std::size_t i = 0;
  while (i < text.size()) {
    if (!is_word(text[i])) {
      ++i;
      continue;
    }
    const std::size_t start = i;
    while (i < text.size() && is_word(text[i])) ++i;
    if (i - start <= kMaxTokenLen) spans.push_back({start, i - start});
  }
  return spans;
}

std::vector<std::string> tokenize(std::string_view text) {
  const std::vector<TokenSpan> spans = tokenize_spans(text);
  std::vector<std::string> tokens;
  tokens.reserve(spans.size());
  for (const TokenSpan& span : spans) {
    tokens.push_back(normalize_term(text.substr(span.offset, span.length)));
  }
  return tokens;
}

}  // namespace mse
