// Small statistics for the benchmarks.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

namespace mse {

// The nearest-rank percentile (DESIGN.md §10): the value at rank ceil(p / 100 * n) of the sorted
// values, so the answer is always one of the measurements and never an interpolation.
// `p` is in [0, 100]; 0 gives the minimum. No values give NaN.
inline double percentile(std::vector<double> values, double p) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const double n = static_cast<double>(values.size());
  const double rank = std::clamp(std::ceil(p * n / 100.0), 1.0, n);
  return values[static_cast<std::size_t>(rank) - 1];
}

// The mean of the values, or NaN if there are none.
inline double mean(std::span<const double> values) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

// Mean reciprocal rank: the average of 1 / rank over the queries. A rank is 1-based; nullopt means
// the target was not found in the results, which counts as 0. No queries give NaN.
inline double mean_reciprocal_rank(std::span<const std::optional<std::size_t>> ranks) {
  if (ranks.empty()) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0.0;
  for (const auto& rank : ranks) {
    if (rank) sum += 1.0 / static_cast<double>(*rank);
  }
  return sum / static_cast<double>(ranks.size());
}

}  // namespace mse
