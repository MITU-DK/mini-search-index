#include "spellcheck.hpp"

#include <algorithm>
#include <vector>

namespace mse {

uint32_t levenshtein(std::string_view a, std::string_view b) {
  if (a.size() < b.size()) std::swap(a, b);  // keep the rows as short as the shorter word
  std::vector<uint32_t> previous(b.size() + 1);
  std::vector<uint32_t> current(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) previous[j] = static_cast<uint32_t>(j);

  for (std::size_t i = 1; i <= a.size(); ++i) {
    current[0] = static_cast<uint32_t>(i);
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const uint32_t substitute = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({substitute, previous[j] + 1, current[j - 1] + 1});
    }
    std::swap(previous, current);
  }
  return previous[b.size()];
}

uint32_t max_edit_distance(std::string_view term) { return term.size() <= 4 ? 1 : 2; }

std::optional<std::string> suggest_brute(const IndexView& index, std::string_view term) {
  if (term.empty()) return std::nullopt;
  const uint32_t limit = max_edit_distance(term);

  std::optional<uint32_t> best;  // the term number of the best candidate so far
  uint32_t best_distance = 0;
  uint32_t best_df = 0;
  // Terms are visited in alphabetical order, so on a full tie the first one found is kept.
  for (uint32_t i = 0; i < index.num_terms(); ++i) {
    const uint32_t df = index.df_at(i);
    if (!is_spell_eligible(df)) continue;
    const std::string_view candidate = index.term_at(i);
    const std::size_t length_gap = candidate.size() > term.size() ? candidate.size() - term.size()
                                                                  : term.size() - candidate.size();
    if (length_gap > limit) continue;  // the distance is at least the difference in length

    const uint32_t distance = levenshtein(term, candidate);
    if (distance > limit) continue;
    if (!best || distance < best_distance || (distance == best_distance && df > best_df)) {
      best = i;
      best_distance = distance;
      best_df = df;
    }
  }
  if (!best) return std::nullopt;
  return std::string(index.term_at(*best));
}

}  // namespace mse
