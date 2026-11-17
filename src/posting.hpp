// One entry of a postings list.
#pragma once

#include <cstdint>

namespace mse {

// One term in one doc: the doc id and how many times the term occurs there.
struct Posting {
  uint32_t doc;
  uint32_t tf;
  auto operator<=>(const Posting&) const = default;
};

}  // namespace mse
