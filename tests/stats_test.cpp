#include "stats.hpp"

#include <gtest/gtest.h>

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
  EXPECT_EQ(percentile({}, 50), 0.0);
  EXPECT_EQ(percentile(std::vector<double>{42.0}, 1), 42.0);
  EXPECT_EQ(percentile(std::vector<double>{42.0}, 99), 42.0);
  EXPECT_EQ(percentile(std::vector<double>{3, 1, 2}, 0), 1.0);  // p = 0 is the minimum
}

}  // namespace
}  // namespace mse
