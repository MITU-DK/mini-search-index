// Splits text into lowercase word tokens. The rules are in DESIGN.md §6.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mse {

// Word characters are 0-9, A-Z, a-z and any byte >= 0x80; everything else separates words.
// ASCII is lowercased. Tokens longer than kMaxTokenLen bytes are dropped.
std::vector<std::string> tokenize(std::string_view text);

// Lowercases one word. Returns "" if the word is longer than kMaxTokenLen bytes.
std::string normalize_term(std::string_view word);

}  // namespace mse
