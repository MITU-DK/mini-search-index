// Memory and page-fault counters of the running process, for the benchmarks.
#pragma once

#include <cstdint>

namespace mse {

struct ProcStats {
  uint64_t rss_kb = 0;        // resident memory now (anonymous + file-backed)
  uint64_t rss_anon_kb = 0;   // resident heap and stack
  uint64_t rss_file_kb = 0;   // resident pages of mapped files (the page cache, once we mmap)
  uint64_t hwm_kb = 0;        // the highest rss_kb so far: the peak
  uint64_t minor_faults = 0;  // page faults served without disk I/O
  uint64_t major_faults = 0;  // page faults that had to read from disk
};

// Reads /proc/self/status and getrusage. A counter that cannot be read stays 0.
ProcStats read_proc_stats();

}  // namespace mse
