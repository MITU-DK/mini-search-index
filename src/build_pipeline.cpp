#include "build_pipeline.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <queue>
#include <system_error>
#include <unordered_map>

#include "build_common.hpp"
#include "corpus.hpp"
#include "spill.hpp"
#include "temp_files.hpp"
#include "tokenizer.hpp"

namespace mse {
namespace fs = std::filesystem;

std::expected<uint64_t, std::string> parse_memory_budget(std::string_view text) {
  const std::string shown = "\"" + std::string(text) + "\"";
  uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto [digits_end, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc() || digits_end == text.data() || text.front() == '+') {
    return std::unexpected("memory budget must be a number of bytes with an optional K, M or G: " +
                           shown);
  }
  const std::string_view suffix(digits_end, static_cast<std::size_t>(end - digits_end));
  uint64_t multiplier = 1;
  if (suffix == "K" || suffix == "k") {
    multiplier = uint64_t{1} << 10;
  } else if (suffix == "M" || suffix == "m") {
    multiplier = uint64_t{1} << 20;
  } else if (suffix == "G" || suffix == "g") {
    multiplier = uint64_t{1} << 30;
  } else if (!suffix.empty()) {
    return std::unexpected("memory budget must be a number of bytes with an optional K, M or G: " +
                           shown);
  }
  if (value > UINT64_MAX / multiplier)
    return std::unexpected("memory budget is too large: " + shown);
  return value * multiplier;
}

namespace {

using PostingsMap = std::unordered_map<std::string, std::vector<Posting>>;

// ---- the k-way merge ----

// Merges the run files into one stream: for every distinct term, in increasing order, calls
// sink(term, postings) with the postings of all runs together, sorted by doc. Every run holds each
// doc at most once (a doc is indexed by one worker, once), so the merged list has no duplicates.
// `buffer_bytes` is the read buffer of each open run.
template <class Sink>
std::expected<void, std::string> merge_runs(std::span<const fs::path> paths,
                                            std::size_t buffer_bytes, Sink&& sink) {
  std::vector<RunReader> readers;
  readers.reserve(paths.size());
  for (const fs::path& path : paths) {
    auto reader = RunReader::open(path, buffer_bytes);
    if (!reader) return std::unexpected(reader.error());
    readers.push_back(std::move(*reader));
  }

  std::vector<RunRecord> current(readers.size());
  // A min-heap of the runs, by the term each is at.
  const auto later = [&](std::size_t a, std::size_t b) {
    return current[a].term > current[b].term;
  };
  std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> heap(later);

  const auto advance = [&](std::size_t i) -> std::expected<void, std::string> {
    const auto more = readers[i].read(current[i]);
    if (!more) return std::unexpected(more.error());
    if (*more) heap.push(i);
    return {};
  };
  for (std::size_t i = 0; i < readers.size(); ++i) {
    if (auto ok = advance(i); !ok) return ok;
  }

  // `merged` is the only place where lists are joined, so the memory for big lists (the commonest
  // words) is held once, not once per run. A term that is in a single run is passed on as it is.
  std::vector<Posting> merged;
  std::vector<std::size_t> group;  // the runs that have the current term
  while (!heap.empty()) {
    group.clear();
    group.push_back(heap.top());
    heap.pop();
    const std::string& term = current[group[0]].term;  // stays valid until that run advances
    while (!heap.empty() && current[heap.top()].term == term) {
      group.push_back(heap.top());
      heap.pop();
    }

    std::span<const Posting> list = current[group[0]].postings;
    if (group.size() > 1) {
      merged.clear();
      for (const std::size_t i : group) {
        merged.insert(merged.end(), current[i].postings.begin(), current[i].postings.end());
      }
      if (!std::is_sorted(merged.begin(), merged.end())) std::sort(merged.begin(), merged.end());
      list = merged;
    }
    if (auto ok = sink(std::string_view(term), list); !ok) return ok;
    for (const std::size_t i : group) {
      if (auto ok = advance(i); !ok) return ok;
    }
  }
  return {};
}

// ---- the workers ----

// Memory that is there whatever the workers hold, and that the budget has to pay for first: per
// thread the batch being read and tokenized, the tokenizer's scratch space and the allocator's
// slack (measured at about 13 MiB per thread on the `main` corpus, rounded up), and per document
// the entries of the document table (the DocInfo, the corpus's own DocRef, an average title).
constexpr uint64_t kThreadReserve = 16 << 20;
constexpr uint64_t kTitleBytes = 24;

uint64_t worker_budget_for(uint64_t budget, unsigned workers, std::size_t docs) {
  const uint64_t fixed =
      docs * (sizeof(DocInfo) + sizeof(DocRef) + kTitleBytes) + uint64_t{workers} * kThreadReserve;
  // With a budget too small to cover that, the workers get half of it, and the process is over.
  const uint64_t left = std::max(budget > fixed ? budget - fixed : 0, budget / 2);
  return std::max<uint64_t>(1, left / workers);
}

// What a term costs in a worker's map besides its postings: a hash node (next pointer, cached hash,
// the key string, the vector header: 72 bytes, 80 with the allocator's header), a bucket pointer,
// and the first allocation of the postings vector. A key longer than 15 bytes is a second
// allocation, counted by its length. The postings themselves are counted by the capacity of their
// vector, which is up to twice their number after growing.
constexpr uint64_t kTermOverhead = 104;
constexpr uint64_t kPostingBytes = sizeof(Posting);

struct SpillWorker {
  PostingsMap map;
  uint64_t estimate = 0;  // bytes: for each term its length + 48, for each posting 8
  uint64_t tokens = 0;
  std::vector<fs::path> runs;  // the run files this worker wrote, in the order it wrote them
  uint64_t spill_bytes = 0;
  std::optional<std::string> error;
};

// State shared by the workers.
struct SpillContext {
  SpillContext(const Corpus& corpus_, std::vector<DocInfo>& docs_, fs::path tmp_dir_,
               uint64_t worker_budget_)
      : corpus(corpus_), docs(docs_), tmp_dir(std::move(tmp_dir_)), worker_budget(worker_budget_) {}

  const Corpus& corpus;
  std::vector<DocInfo>& docs;
  fs::path tmp_dir;
  uint64_t worker_budget;  // a worker writes a run when its estimate reaches this
  std::atomic<std::size_t> next_batch{0};
  std::atomic<bool> failed{false};
  std::atomic<uint32_t> next_run{0};
  std::mutex files_mutex;
  TempFiles run_files;  // every run file ever named, so that all are removed at the end

  fs::path new_run_path() {
    const uint32_t id = next_run.fetch_add(1);
    fs::path path = tmp_dir / std::format("mse-run-{}-{:06}.run", ::getpid(), id);
    std::lock_guard lock(files_mutex);
    run_files.add(path);  // before the file exists, so even a half-written one is removed
    return path;
  }
};

// Sorts the worker's terms, writes them as one run and empties the worker's map.
std::expected<void, std::string> write_run(SpillContext& context, SpillWorker& worker) {
  if (worker.map.empty()) return {};
  std::vector<const PostingsMap::value_type*> sorted;
  sorted.reserve(worker.map.size());
  for (const auto& entry : worker.map) sorted.push_back(&entry);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });

  fs::path run_path = context.new_run_path();
  auto writer = RunWriter::create(run_path);
  if (!writer) return std::unexpected(writer.error());
  for (const auto* entry : sorted) {
    if (auto ok = writer->write(entry->first, entry->second); !ok)
      return std::unexpected(ok.error());
  }
  const auto size = writer->finish();
  if (!size) return std::unexpected(size.error());

  worker.runs.push_back(run_path);
  worker.spill_bytes += *size;
  PostingsMap().swap(worker.map);  // give the memory back, not just empty the map
  worker.estimate = 0;
  return {};
}

// Counts the terms of one doc into the worker's map. A worker takes batches in increasing order,
// so each list stays sorted by doc, also across the runs it writes.
std::size_t add_doc(uint32_t doc, std::string_view line, SpillWorker& worker) {
  const std::vector<std::string> tokens = tokenize(line);
  std::unordered_map<std::string, uint32_t> counts;
  for (const std::string& token : tokens) ++counts[token];
  for (const auto& [term, tf] : counts) {
    auto [slot, inserted] = worker.map.try_emplace(term);
    if (inserted) worker.estimate += term.size() + kTermOverhead;
    const std::size_t capacity_before = slot->second.capacity();
    slot->second.push_back({doc, tf});
    worker.estimate += (slot->second.capacity() - capacity_before) * kPostingBytes;
  }
  return tokens.size();
}

std::expected<void, std::string> index_batch(SpillContext& context, const Batch& batch,
                                             SpillWorker& worker) {
  auto bytes = read_batch(context.corpus, batch);
  if (!bytes) return std::unexpected(bytes.error());
  const uint64_t batch_offset = context.corpus.docs[batch.first_doc].offset;
  for (uint32_t doc = batch.first_doc; doc < batch.first_doc + batch.count; ++doc) {
    const DocRef& ref = context.corpus.docs[doc];
    const std::string_view line(bytes->data() + (ref.offset - batch_offset), ref.length);
    const auto length = static_cast<uint32_t>(add_doc(doc, line, worker));
    context.docs[doc] = {std::string(split_line(line).title), ref, length};
    worker.tokens += length;
  }
  return {};
}

void run_worker(SpillContext& context, SpillWorker& worker) {
  const auto fail = [&](std::string message) {
    worker.error = std::move(message);
    context.failed.store(true, std::memory_order_relaxed);
  };
  try {
    while (!context.failed.load(std::memory_order_relaxed)) {
      const std::size_t number = context.next_batch.fetch_add(1, std::memory_order_relaxed);
      if (number >= context.corpus.batches.size()) break;
      if (auto ok = index_batch(context, context.corpus.batches[number], worker); !ok) {
        return fail(ok.error());
      }
      if (worker.estimate >= context.worker_budget) {
        if (auto ok = write_run(context, worker); !ok) return fail(ok.error());
      }
    }
    if (!context.failed.load(std::memory_order_relaxed)) {
      if (auto ok = write_run(context, worker); !ok) fail(ok.error());  // what is left
    }
  } catch (const std::exception& e) {  // for example std::bad_alloc
    fail(e.what());
  }
}

// How many run files may be open at once: the option, but no more than the process may open.
std::size_t usable_fan_in(std::size_t wanted) {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
    if (limit.rlim_cur < limit.rlim_max) {  // ask for as many as the hard limit allows
      limit.rlim_cur = limit.rlim_max;
      ::setrlimit(RLIMIT_NOFILE, &limit);
      ::getrlimit(RLIMIT_NOFILE, &limit);
    }
    constexpr rlim_t kReserved = 64;  // for the index files, the corpus and the rest
    const rlim_t usable = limit.rlim_cur > kReserved + 2 ? limit.rlim_cur - kReserved : 2;
    wanted = std::min<std::size_t>(wanted, static_cast<std::size_t>(usable));
  }
  return std::max<std::size_t>(wanted, 2);
}

// While there are more runs than may be open at once, merges groups of them into bigger runs.
std::expected<std::vector<fs::path>, std::string> reduce_runs(SpillContext& context,
                                                              std::vector<fs::path> runs,
                                                              std::size_t fan_in,
                                                              std::size_t buffer_bytes,
                                                              uint32_t& passes) {
  while (runs.size() > fan_in) {
    std::vector<fs::path> next;
    for (std::size_t first = 0; first < runs.size(); first += fan_in) {
      const std::size_t count = std::min(fan_in, runs.size() - first);
      const std::span<const fs::path> group(runs.data() + first, count);
      if (count == 1) {
        next.push_back(group[0]);
        continue;
      }
      fs::path merged_path = context.new_run_path();
      auto writer = RunWriter::create(merged_path);
      if (!writer) return std::unexpected(writer.error());
      const auto ok = merge_runs(group, buffer_bytes,
                                 [&](std::string_view term, std::span<const Posting> postings) {
                                   return writer->write(term, postings);
                                 });
      if (!ok) return std::unexpected(ok.error());
      if (const auto size = writer->finish(); !size) return std::unexpected(size.error());
      std::error_code ignored;
      for (const fs::path& done : group) fs::remove(done, ignored);  // free the disk space now
      next.push_back(std::move(merged_path));
    }
    runs = std::move(next);
    ++passes;
  }
  return runs;
}

std::expected<WriteStats, std::string> build_with_spilling(const fs::path& corpus_root,
                                                           const fs::path& out,
                                                           const BuildOptions& opts,
                                                           const WriteOptions& write_options,
                                                           BuildStats* stats) {
  const auto scan_start = BuildClock::now();
  auto corpus = open_corpus(corpus_root, opts.batch_size);
  if (!corpus) return std::unexpected(corpus.error());
  const double scan_s = seconds_since(scan_start);

  // Open the output first, so that a bad output path fails now and not after hours of work.
  auto writer = IndexWriter::create(out, write_options);
  if (!writer) return std::unexpected(writer.error());

  fs::path tmp_dir = opts.tmp_dir;
  if (tmp_dir.empty()) tmp_dir = out.parent_path().empty() ? fs::path(".") : out.parent_path();
  std::error_code mkdir_error;
  fs::create_directories(tmp_dir, mkdir_error);
  if (mkdir_error) {
    return std::unexpected("cannot create the temporary folder " + tmp_dir.string() + ": " +
                           mkdir_error.message());
  }

  std::vector<DocInfo> docs(corpus->docs.size());
  const auto worker_count = static_cast<unsigned>(
      std::max<std::size_t>(1, std::min<std::size_t>(opts.threads, corpus->batches.size())));
  SpillContext context(*corpus, docs, tmp_dir,
                       worker_budget_for(opts.memory_budget_bytes, worker_count, docs.size()));
  std::vector<SpillWorker> workers(worker_count);

  // ---- tokenize, writing a run whenever a worker is over its share of the budget ----
  const auto tokenize_start = BuildClock::now();
  try {
    run_in_parallel(worker_count, [&](unsigned w) { run_worker(context, workers[w]); });
  } catch (const std::system_error& e) {
    return std::unexpected(std::string("cannot start threads: ") + e.what());
  }
  const double tokenize_s = seconds_since(tokenize_start);

  uint64_t tokens = 0;
  uint64_t spill_bytes = 0;
  std::vector<fs::path> runs;
  for (const SpillWorker& worker : workers) {
    if (worker.error) return std::unexpected(*worker.error);
    tokens += worker.tokens;
    spill_bytes += worker.spill_bytes;
    runs.insert(runs.end(), worker.runs.begin(), worker.runs.end());
  }
  const auto runs_spilled = static_cast<uint32_t>(runs.size());

  // ---- merge ----
  const auto merge_start = BuildClock::now();
  if (auto ok = writer->set_docs(corpus->root.string(), corpus->part_names, docs); !ok) {
    return std::unexpected(ok.error());
  }
  // The workers' memory is free again, so the budget pays for the read buffers of the merge.
  constexpr std::size_t kMinBuffer = 64 * 1024;
  const std::size_t fan_in = usable_fan_in(opts.max_merge_fan_in);
  const std::size_t buffer_bytes = std::clamp<std::size_t>(
      opts.memory_budget_bytes / 2 / std::max<std::size_t>(1, std::min(runs.size(), fan_in)),
      kMinBuffer, kRunBufferBytes);
  uint32_t merge_passes = 0;
  auto final_runs = reduce_runs(context, std::move(runs), fan_in, buffer_bytes, merge_passes);
  if (!final_runs) return std::unexpected(final_runs.error());

  uint64_t terms = 0;
  uint64_t postings = 0;
  const auto merged = merge_runs(*final_runs, buffer_bytes,
                                 [&](std::string_view term, std::span<const Posting> list) {
                                   ++terms;
                                   postings += list.size();
                                   return writer->add_term(term, list);
                                 });
  if (!merged) return std::unexpected(merged.error());
  const double merge_s = seconds_since(merge_start);

  // The writer was created before the first doc was read, so its own clock covers the whole build;
  // what is reported as the write time is the assembly of the file, after the last term.
  const auto finish_start = BuildClock::now();
  auto written = writer->finish();
  if (!written) return std::unexpected(written.error());
  written->write_s = seconds_since(finish_start);
  context.run_files.remove_all();

  if (stats != nullptr) {
    *stats = {.scan_s = scan_s,
              .tokenize_s = tokenize_s,
              .merge_s = merge_s,
              .threads_used = worker_count,
              .docs = docs.size(),
              .tokens = tokens,
              .terms = terms,
              .postings = postings,
              .runs_spilled = runs_spilled,
              .spill_bytes = spill_bytes,
              .merge_passes = merge_passes};
  }
  return written;
}

}  // namespace

std::expected<WriteStats, std::string> build_to_file(const fs::path& corpus_root,
                                                     const fs::path& out, const BuildOptions& opts,
                                                     const WriteOptions& write_options,
                                                     BuildStats* stats) {
  if (opts.memory_budget_bytes == 0) {
    auto index = build_index(corpus_root, opts, stats);
    if (!index) return std::unexpected(index.error());
    return write_index(*index, out, write_options);
  }
  return build_with_spilling(corpus_root, out, opts, write_options, stats);
}

}  // namespace mse
