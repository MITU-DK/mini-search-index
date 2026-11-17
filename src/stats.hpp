// Small statistics for the benchmarks.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace mse {

// The nearest-rank percentile (DESIGN.md §10): the value at rank ceil(p / 100 * n) of the sorted
// values, so the answer is always one of the measurements and never an interpolation.
// `p` is in [0, 100]; 0 gives the minimum. No values give 0.
inline double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double n = static_cast<double>(values.size());
  const double rank = std::clamp(std::ceil(p * n / 100.0), 1.0, n);
  return values[static_cast<std::size_t>(rank) - 1];
}

}  // namespace mse
