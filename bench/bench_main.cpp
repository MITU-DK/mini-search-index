// msearch-bench: runs the benchmarks of DESIGN.md §11 and appends their rows to CSV files.
//
//   msearch-bench sweep --threads <sweep>        prints the thread counts, one per line
//   msearch-bench size --corpus D --machine M --profile P [--index-dir DIR] [--results DIR]
//                                                 writes the index raw and VByte, records sizes
//   msearch-bench build --corpus D --threads <sweep> --repeat N --machine M --profile P
//                       --commit H [--results DIR]
//
//   msearch-bench gen-queries --index F --n N --seed S --out FILE
//                                                 known-item queries: "doc<TAB>title" of N random
//                                                 docs
//   msearch-bench queries --index F --queries Q --cache warm|cold [--modes bm25,tfidf,...]
//                         --machine M --profile P     query latency (B4); adds rows to the CSV
//   msearch-bench quality --index F --queries Q --machine M --profile P
//                                                 MRR and hits of BM25 vs TF-IDF (B4)
//   msearch-bench pick-grep-terms --index F --seed S --out FILE
//                                                 20 words of three document-frequency bands (B5)
//   msearch-bench suggest --index F --n N --seed S --machine M --profile P
//                                                 autocomplete: sorted table vs trie (B7)
//   msearch-bench gen-typos --index F --n N --seed S --out FILE
//                                                 N seeded single-edit typos of common words
//   msearch-bench spell --index F --typos FILE --machine M --profile P
//                                                 spell check: brute force vs BK-tree (B6)
//   msearch-bench full-build --corpus D --budget B --threads T --tmp-dir DIR --out FILE
//                            --machine M --profile P --commit H [--compare FILE]
//                                                 one bounded-memory build (B8), in this fresh
//                                                 process; --compare checks the file against
//                                                 another
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
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "autocomplete.hpp"
#include "build_pipeline.hpp"
#include "byte_source.hpp"
#include "index.hpp"
#include "index_writer.hpp"
#include "indexer.hpp"
#include "parsed_loader.hpp"
#include "procstats.hpp"
#include "searcher.hpp"
#include "snippet.hpp"
#include "spellcheck.hpp"
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

// Whether two files have the same bytes, compared a megabyte at a time.
bool same_bytes(const std::filesystem::path& a, const std::filesystem::path& b) {
  if (std::filesystem::file_size(a) != std::filesystem::file_size(b)) return false;
  std::ifstream first(a, std::ios::binary);
  std::ifstream second(b, std::ios::binary);
  std::vector<char> left(1 << 20);
  std::vector<char> right(1 << 20);
  while (first && second) {
    first.read(left.data(), static_cast<std::streamsize>(left.size()));
    second.read(right.data(), static_cast<std::streamsize>(right.size()));
    if (first.gcount() != second.gcount() ||
        !std::equal(left.begin(), left.begin() + first.gcount(), right.begin())) {
      return false;
    }
  }
  return true;
}

constexpr std::string_view kFullBuildHeader =
    "commit,machine,budget_bytes,threads,total_s,scan_s,tokenize_s,merge_s,write_s,peak_rss_kb,"
    "runs_spilled,merge_passes,spill_bytes,index_bytes,docs,terms,postings,identical_to_in_"
    "memory\n";

// One bounded-memory build (B8). The process is new for each budget, so the peak RSS is this
// build's alone; the caller (tools/run_benchmarks.sh) starts one per budget and per repeat.
int command_full_build(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing. "
               "Use: cmake --preset release && cmake --build --preset release\n";
  return 1;
#endif
  const std::filesystem::path corpus = need(options, "corpus");
  const std::filesystem::path out = need(options, "out");
  const std::string machine = need(options, "machine");
  const std::string profile = need(options, "profile");
  const std::string commit = need(options, "commit");
  const auto budget = mse::parse_memory_budget(need(options, "budget"));
  if (!budget || *budget == 0)
    throw std::invalid_argument("--budget must be a size above 0, like 2G");
  const std::vector<unsigned> sweep = resolve_sweep(options);
  if (sweep.size() != 1) throw std::invalid_argument("--threads must be one number or \"auto\"");

  mse::BuildOptions build_options;
  build_options.threads = sweep[0];
  build_options.memory_budget_bytes = *budget;
  build_options.tmp_dir = need(options, "tmp-dir");
  mse::BuildStats stats;
  const auto start = std::chrono::steady_clock::now();
  const auto written = mse::build_to_file(corpus, out, build_options, {}, &stats);
  const double total_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!written) throw std::runtime_error(written.error());
  const uint64_t peak_rss_kb = mse::read_proc_stats().hwm_kb;

  std::string identical = "not_compared";
  if (const auto other = options.find("compare"); other != options.end()) {
    identical = same_bytes(out, other->second) ? "yes" : "no";
  }

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) / machine /
      ("b8_full-" + profile + ".csv");
  std::ofstream csv = open_csv(csv_path, kFullBuildHeader);
  csv << commit << ',' << machine << ',' << *budget << ',' << stats.threads_used << ',' << total_s
      << ',' << stats.scan_s << ',' << stats.tokenize_s << ',' << stats.merge_s << ','
      << written->write_s << ',' << peak_rss_kb << ',' << stats.runs_spilled << ','
      << stats.merge_passes << ',' << stats.spill_bytes << ',' << written->file_bytes << ','
      << stats.docs << ',' << stats.terms << ',' << stats.postings << ',' << identical << std::endl;
  std::cerr << "budget " << *budget / (1024 * 1024) << " MiB, " << stats.threads_used
            << " threads: " << total_s << " s, peak " << peak_rss_kb / 1024 << " MiB, "
            << stats.runs_spilled << " runs, identical: " << identical << "\n";
  return identical == "no" ? 2 : 0;
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
         "vbyte_postings_bytes,bytes_per_posting_vbyte,vbyte_spell_bytes,spell_build_s\n"
      << index->docs.size() << ',' << index->postings.size() << ',' << postings << ','
      << corpus_bytes(corpus) << ',' << raw.file_bytes << ',' << vbyte.file_bytes << ','
      << raw.postings_bytes << ',' << vbyte.postings_bytes << ','
      << static_cast<double>(vbyte.postings_bytes) / static_cast<double>(postings) << ','
      << vbyte.spell_bytes << ',' << vbyte.spell_build_s << '\n';
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << "\n";

  const auto terms_path = csv_path.parent_path() / ("b2_terms-" + profile + ".csv");
  write_term_cost_csv(*index, terms_path);
  std::cerr << "wrote " << terms_path.string() << "\n";
  return 0;
}

// ---- queries and load measurements ----

// A known-item query: the title of one doc, which is the answer.
struct KnownItem {
  uint32_t doc;
  std::string title;
};

// The titles of `count` docs picked with raw mt19937 output and modulo (portable), one per line
// as "doc_id<TAB>title".
int command_gen_queries(const Options& options) {
  auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  if (view.num_docs() == 0) throw std::runtime_error("the index has no docs to take queries from");

  const unsigned count = parse_repeat(get_or(options, "n", "1000"));
  std::mt19937 rng(static_cast<uint32_t>(std::stoul(get_or(options, "seed", "42"))));
  const std::filesystem::path out_path = need(options, "out");
  if (out_path.has_parent_path()) std::filesystem::create_directories(out_path.parent_path());
  std::ofstream out(out_path, std::ios::trunc);
  for (unsigned i = 0; i < count; ++i) {
    const auto doc = static_cast<uint32_t>(rng() % view.num_docs());
    out << doc << '\t' << view.doc_title(doc) << '\n';
  }
  if (!out) throw std::runtime_error("cannot write " + out_path.string());
  std::cerr << "wrote " << out_path.string() << "\n";
  return 0;
}

std::vector<KnownItem> read_known_items(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::vector<KnownItem> items;
  for (std::string line; std::getline(in, line);) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) throw std::runtime_error("bad line in " + path + ": " + line);
    items.push_back({static_cast<uint32_t>(std::stoul(line.substr(0, tab))), line.substr(tab + 1)});
  }
  if (items.empty()) throw std::runtime_error(path + " has no queries");
  return items;
}

std::vector<std::string> read_queries(const std::string& path) {
  std::vector<std::string> titles;
  for (KnownItem& item : read_known_items(path)) titles.push_back(std::move(item.title));
  return titles;
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

// ---- B6: spell check ----

// One seeded edit of `word`: an insertion, deletion, substitution or swap of two neighbours.
std::string seeded_typo(std::string word, std::mt19937& rng) {
  const auto letter = [&] { return static_cast<char>('a' + rng() % 26); };
  switch (rng() % 4) {
    case 0:
      word.insert(word.begin() + static_cast<std::ptrdiff_t>(rng() % (word.size() + 1)), letter());
      break;
    case 1:
      word.erase(rng() % word.size(), 1);
      break;
    case 2: {
      const std::size_t at = rng() % word.size();
      char replacement = letter();
      while (replacement == word[at]) replacement = letter();
      word[at] = replacement;
      break;
    }
    default: {
      const std::size_t at = rng() % (word.size() - 1);
      std::swap(word[at], word[at + 1]);
      break;
    }
  }
  return word;
}

bool is_plain_word(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

int command_gen_typos(const Options& options) {
  const auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();

  // Words of 5 or more letters in at least 50 docs, so that a typo has one obvious right answer.
  std::vector<uint32_t> candidates;
  for (uint32_t i = 0; i < view.num_terms(); ++i) {
    const std::string_view term = view.term_at(i);
    if (view.df_at(i) >= 50 && term.size() >= 5 && is_plain_word(term)) candidates.push_back(i);
  }
  const unsigned count = parse_repeat(get_or(options, "n", "200"));
  if (candidates.size() < count) throw std::runtime_error("the index has too few common words");

  std::mt19937 rng(static_cast<uint32_t>(std::stoul(get_or(options, "seed", "42"))));
  const std::filesystem::path out_path = need(options, "out");
  if (out_path.has_parent_path()) std::filesystem::create_directories(out_path.parent_path());
  std::ofstream out(out_path, std::ios::trunc);
  out << "typo\tword\n";
  for (unsigned made = 0; made < count;) {
    const std::size_t pick = made + rng() % (candidates.size() - made);
    std::swap(candidates[made], candidates[pick]);  // a partial shuffle: no word is used twice
    const std::string word(view.term_at(candidates[made]));
    std::string typo = seeded_typo(word, rng);
    // A typo that is itself a word of the index would have more than one right answer.
    if (typo == word || view.find_term(typo)) continue;
    out << typo << '\t' << word << '\n';
    ++made;
  }
  if (!out) throw std::runtime_error("cannot write " + out_path.string());
  std::cerr << "wrote " << out_path.string() << "\n";
  return 0;
}

struct Typo {
  std::string typo;
  std::string word;
};

std::vector<Typo> read_typos(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::vector<Typo> typos;
  std::string line;
  std::getline(in, line);  // the header
  while (std::getline(in, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) throw std::runtime_error("bad line in " + path.string());
    typos.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  if (typos.empty()) throw std::runtime_error(path.string() + " has no typos");
  return typos;
}

struct SpellTiming {
  std::vector<double> microseconds;
  std::vector<std::optional<std::string>> answers;  // one per typo
};

constexpr unsigned kSpellPasses = 5;

// One unrecorded pass (which also gives the answers), then kSpellPasses recorded ones.
template <class Suggest>
SpellTiming time_spell(const std::vector<Typo>& typos, const Suggest& suggest) {
  SpellTiming result;
  for (const Typo& typo : typos) result.answers.push_back(suggest(typo.typo));
  std::size_t sink = 0;
  for (unsigned pass = 0; pass < kSpellPasses; ++pass) {
    for (const Typo& typo : typos) {
      const auto start = std::chrono::steady_clock::now();
      sink += suggest(typo.typo).has_value();
      result.microseconds.push_back(milliseconds_since(start) * 1000.0);
    }
  }
  if (sink == 0) throw std::runtime_error("no typo got a suggestion");  // also keeps `sink` used
  return result;
}

double accuracy_of(const SpellTiming& timing, const std::vector<Typo>& typos) {
  std::size_t right = 0;
  for (std::size_t i = 0; i < typos.size(); ++i) right += timing.answers[i] == typos[i].word;
  return static_cast<double>(right) / static_cast<double>(typos.size());
}

int command_spell(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing.\n";
  return 1;
#endif
  const auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  const std::vector<Typo> typos = read_typos(need(options, "typos"));

  const SpellTiming brute =
      time_spell(typos, [&](const std::string& word) { return mse::suggest_brute(view, word); });
  const SpellTiming tree =
      time_spell(typos, [&](const std::string& word) { return view.spell_suggest(word); });
  for (std::size_t i = 0; i < typos.size(); ++i) {
    if (brute.answers[i] != tree.answers[i]) {
      throw std::runtime_error("the BK-tree and the brute-force speller disagree on \"" +
                               typos[i].typo + "\"; the numbers would mean nothing");
    }
  }

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) /
      need(options, "machine") / ("b6_spell-" + need(options, "profile") + ".csv");
  std::filesystem::create_directories(csv_path.parent_path());
  std::ofstream csv(csv_path, std::ios::trunc);
  // How many edit distances each method computes per typo; counted once, outside the timings.
  uint64_t brute_calls = 0;
  uint64_t tree_calls = 0;
  for (const Typo& typo : typos) {
    (void)mse::suggest_brute(view, typo.typo, &brute_calls);
    (void)view.spell_suggest(typo.typo, &tree_calls);
  }
  const auto per_typo = [&](uint64_t calls) {
    return static_cast<double>(calls) / static_cast<double>(typos.size());
  };
  csv << "method,p50_us,p95_us,accuracy,avg_distance_calls\n"
      << "brute_force," << mse::percentile(brute.microseconds, 50) << ','
      << mse::percentile(brute.microseconds, 95) << ',' << accuracy_of(brute, typos) << ','
      << per_typo(brute_calls) << '\n'
      << "bk_tree," << mse::percentile(tree.microseconds, 50) << ','
      << mse::percentile(tree.microseconds, 95) << ',' << accuracy_of(tree, typos) << ','
      << per_typo(tree_calls) << '\n';
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << "\n";
  return 0;
}

// ---- B4: query latency and quality ----

mse::RankMode parse_rank_mode(std::string_view name) {
  if (name == "bm25") return mse::RankMode::BM25;
  if (name == "tfidf") return mse::RankMode::TFIDF;
  throw std::invalid_argument("unknown ranking \"" + std::string(name) + "\"");
}

std::vector<std::string> split_commas(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = std::min(text.find(',', start), text.size());
    parts.emplace_back(text.substr(start, comma - start));
    start = comma + 1;
  }
  return parts;
}

constexpr std::size_t kLatencyPasses = 5;
constexpr std::size_t kTopTen = 10;
constexpr std::string_view kQueryHeader =
    "cache,mode,p50_ms,p95_ms,p99_ms,mean_ms,max_ms,major_faults\n";

// One pass over the queries; every query's time in milliseconds. `snippets` also reads and builds
// the preview of every hit, as the command line does.
std::vector<double> time_pass(mse::Searcher<mse::IndexView>& searcher, const mse::IndexView& view,
                              const std::vector<KnownItem>& items, mse::RankMode mode,
                              bool snippets, const std::string& corpus, std::size_t& sink) {
  std::vector<double> milliseconds;
  milliseconds.reserve(items.size());
  for (const KnownItem& item : items) {
    const auto start = std::chrono::steady_clock::now();
    const mse::SearchResponse response = searcher.search(item.title, {.k = kTopTen, .mode = mode});
    sink += response.hits.size();
    if (snippets) {
      for (const mse::SearchResult& hit : response.hits) {
        const auto snippet = mse::snippet_for(view, hit.doc, response.terms, corpus);
        if (snippet) sink += snippet->text.size();
      }
    }
    milliseconds.push_back(milliseconds_since(start));
  }
  return milliseconds;
}

int command_query_latency(const Options& options) {
#ifndef NDEBUG
  std::cerr << "error: this is not a release build; its numbers would mean nothing.\n";
  return 1;
#endif
  const std::filesystem::path index_path = need(options, "index");
  const std::string cache = need(options, "cache");
  const std::string profile = need(options, "profile");
  if (cache != "cold" && cache != "warm")
    throw std::invalid_argument("--cache must be cold or warm");
  const std::vector<KnownItem> items = read_known_items(need(options, "queries"));
  const std::vector<std::string> modes =
      split_commas(get_or(options, "modes", cache == "warm" ? "bm25,tfidf,bm25+snippets" : "bm25"));
  if (cache == "cold" && modes.size() != 1) {
    // Once a mode has run, the pages it touched are mapped and warm; one fresh process per mode.
    throw std::invalid_argument("a cold run measures one mode, in its own process");
  }
  if (cache == "cold" && modes[0] == "bm25+snippets") {
    throw std::invalid_argument("snippets are measured warm only");
  }
  const std::string corpus = get_or(options, "corpus", "data/corpus/" + profile);

  if (cache == "cold") {
    const auto evicted = mse::evict_from_page_cache(index_path);
    if (!evicted) throw std::runtime_error(evicted.error());
  }
  const auto index = mse::Index::open(index_path, mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  mse::Searcher<mse::IndexView> searcher(view);

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) /
      need(options, "machine") / ("b4_query-" + profile + ".csv");
  std::ofstream csv = open_csv(csv_path, kQueryHeader);
  std::size_t sink = 0;
  for (const std::string& name : modes) {
    const bool snippets = name == "bm25+snippets";
    const mse::RankMode mode = parse_rank_mode(snippets ? "bm25" : name);
    std::vector<double> milliseconds;
    uint64_t major_faults = 0;  // during the recorded passes
    if (cache == "warm") {
      (void)time_pass(searcher, view, items, mode, snippets, corpus, sink);  // not recorded
      const uint64_t faults_before = mse::read_proc_stats().major_faults;
      for (std::size_t pass = 0; pass < kLatencyPasses; ++pass) {
        const auto times = time_pass(searcher, view, items, mode, snippets, corpus, sink);
        milliseconds.insert(milliseconds.end(), times.begin(), times.end());
      }
      major_faults = mse::read_proc_stats().major_faults - faults_before;
    } else {
      const uint64_t faults_before = mse::read_proc_stats().major_faults;
      milliseconds = time_pass(searcher, view, items, mode, false, corpus, sink);
      major_faults = mse::read_proc_stats().major_faults - faults_before;
      // A cold number means nothing if the eviction did not work (DESIGN.md §10.5).
      if (major_faults == 0) {
        throw std::runtime_error(
            "a cold run had no major page faults: the page cache was not "
            "evicted, so its numbers cannot be trusted");
      }
    }
    csv << cache << ',' << name << ',' << mse::percentile(milliseconds, 50) << ','
        << mse::percentile(milliseconds, 95) << ',' << mse::percentile(milliseconds, 99) << ','
        << mse::mean(milliseconds) << ','
        << *std::max_element(milliseconds.begin(), milliseconds.end()) << ',' << major_faults
        << '\n';
  }
  if (sink == 0) throw std::runtime_error("no query found anything");  // also keeps `sink` used
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << " (" << cache << ")\n";
  return 0;
}

int command_quality(const Options& options) {
  const auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  const std::vector<KnownItem> items = read_known_items(need(options, "queries"));
  mse::Searcher<mse::IndexView> searcher(view);

  const std::filesystem::path csv_path =
      std::filesystem::path(get_or(options, "results", "benchmarks/results")) /
      need(options, "machine") / ("b4_quality-" + need(options, "profile") + ".csv");
  std::filesystem::create_directories(csv_path.parent_path());
  std::ofstream csv(csv_path, std::ios::trunc);
  csv << "mode,mrr_at_10,hit_at_1,hit_at_10,queries,mean_len_first_hit,mean_len_target\n";
  for (const std::string name : {"bm25", "tfidf"}) {
    std::vector<std::optional<std::size_t>> ranks;  // 1-based, nullopt: not in the top 10
    std::size_t first = 0;
    std::size_t found = 0;
    double first_hit_length = 0;  // total length, in tokens, of the docs ranked first
    double target_length = 0;
    for (const KnownItem& item : items) {
      const auto response = searcher.search(
          item.title, {.k = kTopTen, .mode = parse_rank_mode(name), .spellcheck = false});
      std::optional<std::size_t> rank;
      for (std::size_t i = 0; i < response.hits.size(); ++i) {
        if (response.hits[i].doc == item.doc) rank = i + 1;
      }
      ranks.push_back(rank);
      target_length += view.doc_length(item.doc);
      if (!response.hits.empty()) first_hit_length += view.doc_length(response.hits[0].doc);
      first += rank == 1;
      found += rank.has_value();
    }
    const auto share = [&](std::size_t n) {
      return static_cast<double>(n) / static_cast<double>(items.size());
    };
    csv << name << ',' << mse::mean_reciprocal_rank(ranks) << ',' << share(first) << ','
        << share(found) << ',' << items.size() << ','
        << first_hit_length / static_cast<double>(items.size()) << ','
        << target_length / static_cast<double>(items.size()) << '\n';
  }
  if (!csv) throw std::runtime_error("cannot write " + csv_path.string());
  std::cerr << "wrote " << csv_path.string() << "\n";
  return 0;
}

// ---- B5: terms for the grep comparison ----

// 20 single words of plain lowercase letters, picked by seed from three bands of document
// frequency: 5 rare (10 to 50 docs), 10 medium (0.1% to 1% of the docs) and 5 very common (over
// 20%).
int command_pick_grep_terms(const Options& options) {
  const auto index = mse::Index::open(need(options, "index"), mse::LoadMode::Mmap);
  if (!index) throw std::runtime_error(index.error());
  const mse::IndexView& view = index->view();
  const double docs = view.num_docs();

  struct Band {
    const char* name;
    double low;
    double high;
    unsigned count;
  };
  const Band bands[] = {{"rare", 10, 50, 5},
                        {"medium", docs * 0.001, docs * 0.01, 10},
                        {"common", docs * 0.2, docs, 5}};
  std::mt19937 rng(static_cast<uint32_t>(std::stoul(get_or(options, "seed", "42"))));
  const std::filesystem::path out_path = need(options, "out");
  if (out_path.has_parent_path()) std::filesystem::create_directories(out_path.parent_path());
  std::ofstream out(out_path, std::ios::trunc);
  for (const Band& band : bands) {
    std::vector<uint32_t> candidates;
    for (uint32_t i = 0; i < view.num_terms(); ++i) {
      const std::string_view term = view.term_at(i);
      if (term.size() >= 3 && is_plain_word(term) && view.df_at(i) >= band.low &&
          view.df_at(i) <= band.high) {
        candidates.push_back(i);
      }
    }
    if (candidates.size() < band.count) {
      throw std::runtime_error(std::string("too few ") + band.name + " words in the index");
    }
    for (unsigned made = 0; made < band.count; ++made) {  // a partial shuffle
      const std::size_t pick = made + rng() % (candidates.size() - made);
      std::swap(candidates[made], candidates[pick]);
      out << view.term_at(candidates[made]) << '\n';
      std::cerr << band.name << ": " << view.term_at(candidates[made]) << " (df "
                << view.df_at(candidates[made]) << ")\n";
    }
  }
  if (!out) throw std::runtime_error("cannot write " + out_path.string());
  std::cerr << "wrote " << out_path.string() << "\n";
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
    if (command == "gen-queries") return command_gen_queries(options);
    if (command == "queries") return command_query_latency(options);
    if (command == "quality") return command_quality(options);
    if (command == "pick-grep-terms") return command_pick_grep_terms(options);
    if (command == "load") return command_load(options);
    if (command == "full-build") return command_full_build(options);
    if (command == "suggest") return command_suggest(options);
    if (command == "gen-typos") return command_gen_typos(options);
    if (command == "spell") return command_spell(options);
    throw std::invalid_argument("unknown command \"" + std::string(command) + "\"");
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
