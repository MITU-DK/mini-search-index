// Autocomplete on the sorted term table: the terms that start with a prefix are one contiguous
// range of the table, found by binary search; the most common ones are picked from that range.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"
#include "index_view.hpp"

namespace mse {

// Up to `k` terms starting with `prefix` (lowercased first), the ones in the most docs first and
// terms in the same number of docs in alphabetical order. A prefix shorter than
// kSuggestMinPrefix bytes gives nothing, because it would match too much to be useful.
std::vector<std::string> suggest(const IndexView& index, std::string_view prefix,
                                 std::size_t k = kSuggestTopK);

}  // namespace mse
