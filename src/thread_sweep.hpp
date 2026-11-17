// Which thread counts a scaling benchmark tries.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace mse {

// "auto" gives the powers of two below `nproc`, plus nproc / 2 and nproc (so 20 gives
// 1 2 4 8 10 16 20). Anything else must be a comma-separated list of whole numbers >= 1, with no
// spaces. The result is sorted, without duplicates.
[[nodiscard]] std::expected<std::vector<unsigned>, std::string> parse_thread_sweep(
    std::string_view arg, unsigned nproc);

}  // namespace mse
