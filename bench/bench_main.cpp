// msearch-bench: runs the benchmarks of DESIGN.md §11 and appends their rows to CSV files.
//
//   msearch-bench sweep --threads <sweep>        prints the thread counts, one per line
//   msearch-bench size --corpus D --machine M --profile P [--index-dir DIR] [--results DIR]
//                                                 writes the index raw and VByte, records sizes
//   msearch-bench build --corpus D --threads <sweep> --repeat N --machine M --profile P
//                       --commit H [--results DIR]
//
//   msearch-bench queries --index F --count N --seed S --out FILE
//                                                 known-item queries: the titles of N random docs
//   msearch-bench suggest --index F --n N --seed S --machine M --profile P
//                                                 autocomplete: sorted table vs trie (B7)
//   msearch-bench load --index F --mode parse|read|mmap --cache cold|warm --queries Q
//                      --machine M --profile P --commit H [--record yes|no]
//                                                 one load measurement, in this fresh process
//
// For a clean peak-RSS number, tools/run_benchmarks.sh starts a new process for each thread count.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "autocomplete.hpp"
#include "byte_source.hpp"
#include "index.hpp"
#include "index_writer.hpp"
#include "indexer.hpp"
#include "parsed_loader.hpp"
#include "procstats.hpp"
#include "searcher.hpp"
#include "stats.hpp"
#include "thread_sweep.hpp"
#include "trie.hpp"
#include "varint.hpp"

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

std::ofstream open_csv(const std::filesystem::path& path, std::string_view header) {
  std::filesystem::create_directories(path.parent_path());
  const bool is_new = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
  std::ofstream csv(path, std::ios::app);
  if (!csv) throw std::runtime_error("cannot write " + path.string());
  if (is_new) csv << header;
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
  std::ofstream csv = open_csv(csv_path, kBuildHeader);

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

// The bytes of all part files of a corpus.
uint64_t corpus_bytes(const std::filesystem::path& corpus) {
  uint64_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with("part-") && name.ends_with(".tsv")) total += entry.file_size();
  }
  return total;
}

mse::WriteStats write_or_throw(const mse::InMemoryIndex& index, const std::filesystem::path& path,
                               bool compress) {
  const auto stats = mse::write_index(index, path, {.compress = compress});
  if (!stats) throw std::runtime_error(stats.error());
  return *stats;
}

struct TermCost {
  const std::string* term;
  uint64_t df;
  uint64_t gap_bytes;
  uint64_t tf_bytes;
};

// What each term's postings would cost in VByte, split into the gap numbers and the tf numbers.
std::vector<TermCost> term_costs(const mse::InMemoryIndex& index) {
  std::vector<TermCost> costs;
  costs.reserve(index.postings.size());
  for (const auto& [term, list] : index.postings) {
    TermCost cost{&term, list.size(), 0, 0};
    uint32_t previous = 0;
    for (const mse::Posting& posting : list) {
      cost.gap_bytes += mse::uleb128_size(posting.doc - previous);
      cost.tf_bytes += mse::uleb128_size(posting.tf);
      previous = posting.doc;
    }
    costs.push_back(cost);
  }
  return costs;
}

// One row for the term of each df rank in kRanks (1 = the term in the most docs), to show how the
// cost per posting depends on how dense a list is.
void write_term_cost_csv(const mse::InMemoryIndex& index, const std::filesystem::path& path) {
  std::vector<TermCost> costs = term_costs(index);
  std::sort(costs.begin(), costs.end(), [](const TermCost& a, const TermCost& b) {
    return a.df != b.df ? a.df > b.df : *a.term < *b.term;
  });
  std::ofstream csv(path, std::ios::trunc);
  csv << "df_rank,term,df,gap_bytes,tf_bytes,bytes_per_gap,bytes_per_posting\n";
  for (const std::size_t rank : {1u, 2u, 3u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u}) {
    if (rank > costs.size()) break;
    const TermCost& cost = costs[rank - 1];
    const auto df = static_cast<double>(cost.df);
    csv << rank << ',' << *cost.term << ',' << cost.df << ',' << cost.gap_bytes << ','
        << cost.tf_bytes << ',' << static_cast<double>(cost.gap_bytes) / df << ','
        << static_cast<double>(cost.gap_bytes + cost.tf_bytes) / df << '\n';
  }
  if (!csv) throw std::runtime_error("cannot write " + path.string());
}

int command_size(const Options& options) {
  const std::filesystem::path corpus = need(options, "corpus");
  const std::string machine = need(options, "machine");
  const std::string profile = need(options, "profile");
  const std::filesystem::path index_dir = get_or(options, "index-dir", "data/index/" + profile);

  auto index =
      mse::build_index(corpus, {.threads = std::max(1u, std::thread::hardware_concurrency())});
  if (!index) throw std::runtime_error(index.error());
  std::filesystem::create_directories(index_dir);
  const mse::WriteStats raw = write_or_throw(*index, index_dir / "raw.bin", false);
  const mse::WriteStats vbyte = write_or_throw(*index, index_dir / "vbyte.bin", true);

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) / machine /
      ("b2_size-" + profile + ".csv");
  std::filesystem::create_directories(csv_path.parent_path());
  std::ofstream csv(csv_path, std::ios::trunc);
  const uint64_t postings = index->total_postings();
  csv << "docs,terms,postings,corpus_bytes,raw_file_bytes,vbyte_file_bytes,raw_postings_bytes,"
         "vbyte_postings_bytes,bytes_per_posting_vbyte\n"
      << index->docs.size() << ',' << index->postings.size() << ',' << postings << ','
      << corpus_bytes(corpus) << ',' << raw.file_bytes << ',' << vbyte.file_bytes << ','
      << raw.postings_bytes << ',' << vbyte.postings_bytes << ','
      << static_cast<double>(vbyte.postings_bytes) / static_cast<double>(postings) << '\n';
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << "\n";

  const auto terms_path = csv_path.parent_path() / ("b2_terms-" + profile + ".csv");
  write_term_cost_csv(*index, terms_path);
  std::cerr << "wrote " << terms_path.string() << "\n";
  return 0;
}

// ---- queries and load measurements ----

int command_queries(const Options& options) {
  auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  if (view.num_docs() == 0) throw std::runtime_error("the index has no docs to take queries from");

  const unsigned count = parse_repeat(need(options, "count"));
  std::mt19937 rng(static_cast<uint32_t>(std::stoul(need(options, "seed"))));
  std::ofstream out(need(options, "out"), std::ios::trunc);
  for (unsigned i = 0; i < count; ++i) {
    out << view.doc_title(static_cast<uint32_t>(rng() % view.num_docs())) << '\n';
  }
  if (!out) throw std::runtime_error("cannot write " + need(options, "out"));
  return 0;
}

std::vector<std::string> read_queries(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::vector<std::string> queries;
  for (std::string line; std::getline(in, line);) queries.push_back(line);
  if (queries.empty()) throw std::runtime_error(path + " has no queries");
  return queries;
}

constexpr std::size_t kLoadQueries = 1000;

constexpr std::string_view kLoadHeader =
    "commit,mode,cache,open_ms,first_query_ms,rss_after_open_kb,rss_anon_after_open_kb,"
    "rss_file_after_open_kb,rss_after_1000q_kb,major_faults_open,major_faults_first_query,"
    "minor_faults_first_query\n";

struct LoadRow {
  double open_ms = 0;
  double first_query_ms = 0;
  mse::ProcStats after_open;
  uint64_t rss_after_queries_kb = 0;
  uint64_t major_faults_open = 0;
  uint64_t major_faults_first_query = 0;
  uint64_t minor_faults_first_query = 0;
};

double milliseconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

// Runs the queries over an opened index. The first one, with the searcher's setup, is timed on its
// own, because on a cold index that is where the pages get loaded.
template <mse::IndexReader R>
void measure_queries(const R& reader, const std::vector<std::string>& queries, LoadRow& row) {
  const auto first_start = std::chrono::steady_clock::now();
  mse::Searcher<R> searcher(reader);
  (void)searcher.search(queries[0]);
  row.first_query_ms = milliseconds_since(first_start);
  const mse::ProcStats after_first = mse::read_proc_stats();
  row.major_faults_first_query = after_first.major_faults - row.after_open.major_faults;
  row.minor_faults_first_query = after_first.minor_faults - row.after_open.minor_faults;

  for (std::size_t i = 1; i < std::min(queries.size(), kLoadQueries); ++i) {
    (void)searcher.search(queries[i]);
  }
  row.rss_after_queries_kb = mse::read_proc_stats().rss_kb;
}

// Opens the index the way `mode` says, then runs the queries.
LoadRow measure_load(const std::filesystem::path& index, const std::string& mode,
                     const std::vector<std::string>& queries) {
  LoadRow row;
  const mse::ProcStats before = mse::read_proc_stats();
  const auto start = std::chrono::steady_clock::now();
  const auto finish_open = [&] {
    row.open_ms = milliseconds_since(start);
    row.after_open = mse::read_proc_stats();
    row.major_faults_open = row.after_open.major_faults - before.major_faults;
  };

  if (mode == "parse") {
    const auto parsed = mse::load_parsed(index);
    if (!parsed) throw std::runtime_error(parsed.error());
    finish_open();
    const mse::MemoryReader reader(*parsed);
    measure_queries(reader, queries, row);
  } else {
    const auto opened =
        mse::Index::open(index, mode == "mmap" ? mse::LoadMode::Mmap : mse::LoadMode::Read);
    if (!opened) throw std::runtime_error(opened.error());
    finish_open();
    measure_queries(opened->view(), queries, row);
  }
  return row;
}

int command_load(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing.\n";
  return 1;
#endif
  const std::filesystem::path index = need(options, "index");
  const std::string mode = need(options, "mode");
  const std::string cache = need(options, "cache");
  if (mode != "parse" && mode != "read" && mode != "mmap") {
    throw std::invalid_argument("--mode must be parse, read or mmap");
  }
  if (cache != "cold" && cache != "warm")
    throw std::invalid_argument("--cache must be cold or warm");
  const std::vector<std::string> queries = read_queries(need(options, "queries"));

  if (cache == "cold") {
    const auto evicted = mse::evict_from_page_cache(index);
    if (!evicted) throw std::runtime_error(evicted.error());
  }
  const LoadRow row = measure_load(index, mode, queries);

  std::ostringstream line;
  line << need(options, "commit") << ',' << mode << ',' << cache << ',' << row.open_ms << ','
       << row.first_query_ms << ',' << row.after_open.rss_kb << ',' << row.after_open.rss_anon_kb
       << ',' << row.after_open.rss_file_kb << ',' << row.rss_after_queries_kb << ','
       << row.major_faults_open << ',' << row.major_faults_first_query << ','
       << row.minor_faults_first_query << '\n';
  std::cerr << mode << '/' << cache << ": " << line.str();
  if (get_or(options, "record", "yes") == "yes") {
    const std::filesystem::path csv_path =
        std::filesystem::path(get_or(options, "results", "benchmarks/results")) /
        need(options, "machine") / ("b3_load-" + need(options, "profile") + ".csv");
    open_csv(csv_path, kLoadHeader) << line.str();
  }
  return 0;
}

// ---- B7: autocomplete ----

// 2 to 5 leading bytes of seeded random terms that occur in at least 10 docs.
std::vector<std::string> pick_prefixes(const mse::IndexView& view, unsigned count, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::string> prefixes;
  for (std::size_t attempts = 0; prefixes.size() < count; ++attempts) {
    if (attempts > std::size_t{count} * 10000 + 1000000) {
      throw std::runtime_error("the index has too few terms with df >= 10 and 2 or more bytes");
    }
    const auto term = static_cast<uint32_t>(rng() % view.num_terms());
    const std::string_view text = view.term_at(term);
    const std::size_t length = 2 + rng() % 4;
    if (view.df_at(term) < 10 || text.size() < 2) continue;
    prefixes.emplace_back(text.substr(0, std::min(length, text.size())));
  }
  return prefixes;
}

constexpr unsigned kSuggestPasses = 5;

// Latencies in microseconds of `ask` over the prefixes: one unrecorded pass to load pages, then
// kSuggestPasses recorded ones, all pooled.
template <class Ask>
std::vector<double> time_suggestions(const std::vector<std::string>& prefixes, const Ask& ask) {
  std::size_t sink = 0;
  for (const std::string& prefix : prefixes) sink += ask(prefix).size();
  std::vector<double> microseconds;
  for (unsigned pass = 0; pass < kSuggestPasses; ++pass) {
    for (const std::string& prefix : prefixes) {
      const auto start = std::chrono::steady_clock::now();
      sink += ask(prefix).size();
      microseconds.push_back(milliseconds_since(start) * 1000.0);
    }
  }
  if (sink == 0) throw std::runtime_error("no prefix matched any term");  // also keeps `sink` used
  return microseconds;
}

int command_suggest(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing.\n";
  return 1;
#endif
  const auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  const std::vector<std::string> prefixes =
      pick_prefixes(view, parse_repeat(get_or(options, "n", "1000")),
                    static_cast<uint32_t>(std::stoul(get_or(options, "seed", "42"))));

  const std::vector<double> table = time_suggestions(
      prefixes, [&](const std::string& prefix) { return mse::suggest(view, prefix); });

  const auto build_start = std::chrono::steady_clock::now();
  const mse::Trie trie = mse::Trie::build(view);
  const double build_ms = milliseconds_since(build_start);
  const std::vector<double> trie_times =
      time_suggestions(prefixes, [&](const std::string& prefix) { return trie.suggest(prefix); });

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) /
      need(options, "machine") / ("b7_suggest-" + need(options, "profile") + ".csv");
  std::filesystem::create_directories(csv_path.parent_path());
  std::ofstream csv(csv_path, std::ios::trunc);
  csv << "method,build_ms,extra_memory_bytes,p50_us,p95_us\n"
      << "sorted_table,0,0," << mse::percentile(table, 50) << ',' << mse::percentile(table, 95)
      << '\n'
      << "trie," << build_ms << ',' << trie.memory_bytes() << ',' << mse::percentile(trie_times, 50)
      << ',' << mse::percentile(trie_times, 95) << '\n';
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << "\n";
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
    if (command == "size") return command_size(options);
    if (command == "queries") return command_queries(options);
    if (command == "load") return command_load(options);
    if (command == "suggest") return command_suggest(options);
    throw std::invalid_argument("unknown command \"" + std::string(command) + "\"");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
