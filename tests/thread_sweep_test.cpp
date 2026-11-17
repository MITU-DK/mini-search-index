#include "thread_sweep.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace mse {
namespace {

using Sweep = std::vector<unsigned>;

TEST(ThreadSweep, Auto20) {
  EXPECT_EQ(*parse_thread_sweep("auto", 20), (Sweep{1, 2, 4, 8, 10, 16, 20}));
}

TEST(ThreadSweep, Auto32) {
  EXPECT_EQ(*parse_thread_sweep("auto", 32), (Sweep{1, 2, 4, 8, 16, 32}));
}

TEST(ThreadSweep, Auto1) { EXPECT_EQ(*parse_thread_sweep("auto", 1), (Sweep{1})); }

TEST(ThreadSweep, Auto2And3) {
  EXPECT_EQ(*parse_thread_sweep("auto", 2), (Sweep{1, 2}));
  EXPECT_EQ(*parse_thread_sweep("auto", 3), (Sweep{1, 2, 3}));
}

TEST(ThreadSweep, AutoWithZeroProcessorsStillGivesOne) {
  EXPECT_EQ(*parse_thread_sweep("auto", 0), (Sweep{1}));
}

TEST(ThreadSweep, ListIsKeptAsGivenButSortedAndDeduplicated) {
  EXPECT_EQ(*parse_thread_sweep("1,6,12,20", 20), (Sweep{1, 6, 12, 20}));
  EXPECT_EQ(*parse_thread_sweep("8,2,8,1", 20), (Sweep{1, 2, 8}));
  EXPECT_EQ(*parse_thread_sweep("64", 20), (Sweep{64}));  // more than the cores is allowed
}

TEST(ThreadSweep, BadInputsAreErrors) {
  for (const char* bad : {"0", "a,b", "", "1,,2", "1,0", "-1", "1;2", "2x", " 2", "1,2,"}) {
    EXPECT_FALSE(parse_thread_sweep(bad, 20).has_value()) << "\"" << bad << "\"";
  }
}

}  // namespace
}  // namespace mse
