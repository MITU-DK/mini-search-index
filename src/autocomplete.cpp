#include "autocomplete.hpp"

#include <algorithm>
#include <queue>

#include "tokenizer.hpp"

namespace mse {
namespace {

struct Candidate {
  uint32_t df;
  uint32_t index;  // position in the term table; terms are sorted, so this orders them by name
};

// True if `a` should be suggested before `b`.
bool better(const Candidate& a, const Candidate& b) {
  return a.df != b.df ? a.df > b.df : a.index < b.index;
}

}  // namespace

std::vector<std::string> suggest(const IndexView& index, std::string_view prefix, std::size_t k) {
  const std::string normalized = normalize_term(prefix);
  if (k == 0 || normalized.size() < kSuggestMinPrefix) return {};
  const auto [first, last] = index.prefix_range(normalized);

  // A heap whose top is the worst candidate kept so far, so that a better one can replace it.
  std::priority_queue<Candidate, std::vector<Candidate>, decltype(&better)> best(&better);
  for (uint32_t i = first; i < last; ++i) {
    const Candidate candidate{index.df_at(i), i};
    if (best.size() < k) {
      best.push(candidate);
    } else if (better(candidate, best.top())) {
      best.pop();
      best.push(candidate);
    }
  }

  std::vector<std::string> terms(best.size());
  for (std::size_t i = terms.size(); i > 0; --i) {  // the heap gives the worst first
    terms[i - 1] = std::string(index.term_at(best.top().index));
    best.pop();
  }
  return terms;
}

}  // namespace mse
