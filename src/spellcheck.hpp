// Spelling suggestions by edit distance. This file is the brute-force speller: it compares the
// word with every eligible term. It is the oracle that the BK-tree must agree with, and the
// baseline of benchmark B6. The rules are in DESIGN.md §4.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "constants.hpp"
#include "index_view.hpp"

namespace mse {

// The Levenshtein distance between two byte strings: the fewest single-byte insertions,
// deletions and substitutions that turn `a` into `b`. A swap of two neighbours costs 2.
uint32_t levenshtein(std::string_view a, std::string_view b);

// The largest distance at which a suggestion is still offered: 1 for words of up to 4 bytes
// (a short word is within 2 edits of too many other words), 2 for longer ones.
uint32_t max_edit_distance(std::string_view term);

// True if a term in `df` docs may be offered as a suggestion (it is not a junk token).
inline bool is_spell_eligible(uint32_t df) { return df >= kSpellMinDf; }

// The best correction of `term`, or nothing. Only terms in at least kSpellMinDf docs and within
// max_edit_distance(term) are candidates. The best has the smallest distance, then the most docs,
// then the alphabetically first term. A word that is itself eligible suggests itself.
std::optional<std::string> suggest_brute(const IndexView& index, std::string_view term);

}  // namespace mse
