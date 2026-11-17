#include "procstats.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace mse {
namespace {

TEST(ProcStats, GrowsWithAllocation) {
  const ProcStats before = read_proc_stats();
  std::vector<char> block(64 << 20);
  std::memset(block.data(), 1, block.size());  // touching the pages is what makes them resident
  const ProcStats after = read_proc_stats();
  EXPECT_GE(after.rss_anon_kb, before.rss_anon_kb + 60000);
  EXPECT_GE(after.hwm_kb, after.rss_kb);
  EXPECT_GT(after.minor_faults, before.minor_faults);
  EXPECT_EQ(block[123], 1);  // keeps the allocation alive past the second read
}

TEST(ProcStats, ReportsAPlausibleProcess) {
  const ProcStats stats = read_proc_stats();
  EXPECT_GT(stats.rss_kb, 0u);
  EXPECT_GE(stats.rss_kb, stats.rss_file_kb);
}

}  // namespace
}  // namespace mse
