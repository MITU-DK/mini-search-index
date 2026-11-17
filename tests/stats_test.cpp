#include "stats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <optional>

namespace mse {
namespace {

TEST(Stats, NearestRankPercentiles) {
  const std::vector<double> ten = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};  // unsorted on purpose
  EXPECT_EQ(percentile(ten, 50), 5.0);                              // rank ceil(0.5 * 10) = 5
  EXPECT_EQ(percentile(ten, 95), 10.0);                             // rank ceil(9.5) = 10
  EXPECT_EQ(percentile(ten, 99), 10.0);
  EXPECT_EQ(percentile(ten, 10), 1.0);
  EXPECT_EQ(percentile(ten, 11), 2.0);  // rank ceil(1.1) = 2
  EXPECT_EQ(percentile(ten, 100), 10.0);
}

TEST(Stats, EveryResultIsARealMeasurement) {
  const std::vector<double> values = {1.5, 100.0, 7.25};
  for (const double p : {1.0, 33.0, 34.0, 50.0, 66.0, 67.0, 99.0}) {
    const double got = percentile(values, p);
    EXPECT_TRUE(got == 1.5 || got == 7.25 || got == 100.0) << p;  // never an interpolation
  }
}

TEST(Stats, EdgeCases) {
  EXPECT_TRUE(std::isnan(percentile({}, 50)));  // no values, no percentile: not a fake 0
  EXPECT_EQ(percentile(std::vector<double>{42.0}, 1), 42.0);
  EXPECT_EQ(percentile(std::vector<double>{42.0}, 99), 42.0);
  EXPECT_EQ(percentile(std::vector<double>{3, 1, 2}, 0), 1.0);  // p = 0 is the minimum
}

TEST(Stats, PercentileOfOneToAHundred) {
  std::vector<double> hundred(100);
  std::iota(hundred.begin(), hundred.end(), 1.0);
  EXPECT_EQ(percentile(hundred, 95), 95.0);
  EXPECT_EQ(percentile(hundred, 99), 99.0);
  EXPECT_EQ(percentile(hundred, 50), 50.0);
  EXPECT_EQ(percentile({5.0}, 50), 5.0);
}

TEST(Stats, Mrr) {
  const std::vector<std::optional<std::size_t>> ranks = {1, 2, std::nullopt};
  EXPECT_DOUBLE_EQ(mean_reciprocal_rank(ranks), (1.0 + 0.5 + 0.0) / 3.0);
}

TEST(Stats, MrrEdgeCases) {
  EXPECT_TRUE(std::isnan(mean_reciprocal_rank(std::vector<std::optional<std::size_t>>{})));
  const std::vector<std::optional<std::size_t>> none = {std::nullopt, std::nullopt};
  EXPECT_EQ(mean_reciprocal_rank(none), 0.0);
  const std::vector<std::optional<std::size_t>> all_first = {1, 1, 1};
  EXPECT_EQ(mean_reciprocal_rank(all_first), 1.0);
  const std::vector<std::optional<std::size_t>> tenth = {10};
  EXPECT_DOUBLE_EQ(mean_reciprocal_rank(tenth), 0.1);
}

TEST(Stats, Mean) {
  const std::vector<double> values = {1.0, 2.0, 6.0};
  EXPECT_DOUBLE_EQ(mean(values), 3.0);
  EXPECT_TRUE(std::isnan(mean(std::vector<double>{})));
}

}  // namespace
}  // namespace mse
