// msearch-bench: runs the benchmarks of DESIGN.md §11 and appends their rows to CSV files.
//
//   msearch-bench sweep --threads <sweep>        prints the thread counts, one per line
//   msearch-bench build --corpus D --threads <sweep> --repeat N --machine M --profile P
//                       --commit H [--results DIR]
//
// For a clean peak-RSS number, tools/run_benchmarks.sh starts a new process for each thread count.
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "indexer.hpp"
#include "procstats.hpp"
#include "thread_sweep.hpp"

namespace {

using Options = std::map<std::string, std::string, std::less<>>;

std::string need(const Options& options, const std::string& name) {
  const auto found = options.find(name);
  if (found == options.end()) throw std::invalid_argument("missing --" + name);
  return found->second;
}

std::string get_or(const Options& options, const std::string& name, std::string fallback) {
  const auto found = options.find(name);
  return found == options.end() ? std::move(fallback) : found->second;
}

// Reads "--name value" pairs after the command word.
Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 2; i < argc; i += 2) {
    const std::string_view flag = argv[i];
    if (!flag.starts_with("--") || i + 1 >= argc) {
      throw std::invalid_argument("expected \"--name value\" pairs, got \"" + std::string(flag) +
                                  "\"");
    }
    options[std::string(flag.substr(2))] = argv[i + 1];
  }
  return options;
}

std::vector<unsigned> resolve_sweep(const Options& options) {
  const auto sweep = mse::parse_thread_sweep(get_or(options, "threads", "auto"),
                                             std::thread::hardware_concurrency());
  if (!sweep) throw std::invalid_argument(sweep.error());
  return *sweep;
}

unsigned parse_repeat(const std::string& text) {
  unsigned repeat = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), repeat);
  if (error != std::errc() || end != text.data() + text.size() || repeat == 0) {
    throw std::invalid_argument("--repeat must be a whole number >= 1");
  }
  return repeat;
}

struct Run {
  mse::BuildStats stats;
  double total_s = 0;
  uint64_t peak_rss_kb = 0;
};

Run run_build_once(const std::filesystem::path& corpus, unsigned threads) {
  Run run;
  const auto start = std::chrono::steady_clock::now();
  auto index = mse::build_index(corpus, {.threads = threads}, &run.stats);
  run.total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!index) throw std::runtime_error(index.error());
  run.peak_rss_kb = mse::read_proc_stats().hwm_kb;
  return run;
}

constexpr std::string_view kBuildHeader =
    "commit,machine,threads,run,scan_s,tokenize_s,merge_s,total_s,peak_rss_kb\n";

std::ofstream open_csv(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  const bool is_new = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
  std::ofstream csv(path, std::ios::app);
  if (!csv) throw std::runtime_error("cannot write " + path.string());
  if (is_new) csv << kBuildHeader;
  return csv;
}

int command_build(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing. "
               "Use: cmake --preset release && cmake --build --preset release\n";
  return 1;
#endif
  const std::filesystem::path corpus = need(options, "corpus");
  const std::string machine = need(options, "machine");
  const std::string profile = need(options, "profile");
  const std::string commit = need(options, "commit");
  const unsigned repeat = parse_repeat(get_or(options, "repeat", "5"));
  const std::vector<unsigned> sweep = resolve_sweep(options);
  if (sweep.size() > 1) {
    std::cerr << "warning: " << sweep.size() << " thread counts in one process; peak RSS is "
              << "cumulative. Use tools/run_benchmarks.sh for clean numbers.\n";
  }

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) / machine /
      ("b1_build-" + profile + ".csv");
  std::ofstream csv = open_csv(csv_path);

  for (const unsigned threads : sweep) {
    std::cerr << "T=" << threads << ": warm-up run (discarded)\n";
    run_build_once(corpus, threads);
    for (unsigned run_number = 1; run_number <= repeat; ++run_number) {
      const Run run = run_build_once(corpus, threads);
      csv << commit << ',' << machine << ',' << threads << ',' << run_number << ','
          << run.stats.scan_s << ',' << run.stats.tokenize_s << ',' << run.stats.merge_s << ','
          << run.total_s << ',' << run.peak_rss_kb << std::endl;  // flushed: a crash keeps the rows
      std::cerr << "T=" << threads << " run " << run_number << ": " << run.total_s << " s, peak "
                << run.peak_rss_kb / 1024 << " MiB\n";
    }
  }
  return 0;
}

int command_sweep(const Options& options) {
  for (const unsigned threads : resolve_sweep(options)) std::cout << threads << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2)
      throw std::invalid_argument("usage: msearch-bench <build|sweep> [--name value]...");
    const std::string_view command = argv[1];
    const Options options = parse_options(argc, argv);
    if (command == "build") return command_build(options);
    if (command == "sweep") return command_sweep(options);
    throw std::invalid_argument("unknown command \"" + std::string(command) + "\"");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
