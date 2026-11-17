#include "thread_sweep.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace mse {
namespace {

std::vector<unsigned> auto_sweep(unsigned nproc) {
  nproc = std::max(1u, nproc);
  std::vector<unsigned> sweep;
  for (unsigned t = 1; t < nproc; t *= 2) sweep.push_back(t);
  sweep.push_back(std::max(1u, nproc / 2));
  sweep.push_back(nproc);
  return sweep;
}

}  // namespace

std::expected<std::vector<unsigned>, std::string> parse_thread_sweep(std::string_view arg,
                                                                     unsigned nproc) {
  std::vector<unsigned> sweep;
  if (arg == "auto") {
    sweep = auto_sweep(nproc);
  } else {
    while (true) {
      const std::size_t comma = arg.find(',');
      const std::string_view item = arg.substr(0, comma);
      unsigned value = 0;
      const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), value);
      if (error != std::errc() || end != item.data() + item.size() || value == 0) {
        return std::unexpected("bad thread count \"" + std::string(item) +
                               "\" in the sweep: expected whole numbers >= 1 or \"auto\"");
      }
      sweep.push_back(value);
      if (comma == std::string_view::npos) break;
      arg.remove_prefix(comma + 1);
    }
  }
  std::sort(sweep.begin(), sweep.end());
  sweep.erase(std::unique(sweep.begin(), sweep.end()), sweep.end());
  return sweep;
}

}  // namespace mse
