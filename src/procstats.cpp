#include "procstats.hpp"

#include <sys/resource.h>

#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace mse {
namespace {

// For a line like "VmRSS:\t  1234 kB", returns 1234 if the line starts with `key` (e.g. "VmRSS:").
bool parse_kb_line(std::string_view line, std::string_view key, uint64_t& out) {
  if (!line.starts_with(key)) return false;
  std::string_view rest = line.substr(key.size());
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
  std::from_chars(rest.data(), rest.data() + rest.size(), out);
  return true;
}

}  // namespace

ProcStats read_proc_stats() {
  ProcStats stats;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    parse_kb_line(line, "VmRSS:", stats.rss_kb) ||
        parse_kb_line(line, "RssAnon:", stats.rss_anon_kb) ||
        parse_kb_line(line, "RssFile:", stats.rss_file_kb) ||
        parse_kb_line(line, "VmHWM:", stats.hwm_kb);
  }

  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    stats.minor_faults = static_cast<uint64_t>(usage.ru_minflt);
    stats.major_faults = static_cast<uint64_t>(usage.ru_majflt);
  }
  return stats;
}

}  // namespace mse
