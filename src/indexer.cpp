#include "indexer.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "corpus.hpp"
#include "tokenizer.hpp"

namespace mse {
namespace {

using Clock = std::chrono::steady_clock;
using PostingsMap = std::unordered_map<std::string, std::vector<Posting>>;

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Runs fn(0) .. fn(count - 1), each on its own thread, and returns when all are done.
// With count == 1 there is no thread: fn(0) runs right here.
template <class Fn>
void run_in_parallel(unsigned count, Fn fn) {
  if (count == 1) {
    fn(0);
    return;
  }
  std::vector<std::jthread> threads;  // joins when it goes out of scope
  threads.reserve(count);
  for (unsigned i = 0; i < count; ++i) threads.emplace_back(fn, i);
}

// ---- the workers: read, tokenize and count ----

// What one worker produces. Its postings are split into one map per shard (term hash modulo the
// number of shards), so that the merge threads each own a disjoint set of terms.
struct WorkerResult {
  std::vector<PostingsMap> shards;
  uint64_t tokens = 0;
  std::optional<std::string> error;
};

std::size_t shard_of(const std::string& term, std::size_t shard_count) {
  return shard_count == 1 ? 0 : std::hash<std::string>{}(term) % shard_count;
}

// Reads all of a batch's lines with a single read. Doc i of the batch starts at
// docs[i].offset - docs[first].offset inside the returned bytes.
std::expected<std::string, std::string> read_batch(const Corpus& corpus, const Batch& batch) {
  const DocRef& first = corpus.docs[batch.first_doc];
  const DocRef& last = corpus.docs[batch.first_doc + batch.count - 1];
  const uint64_t end = last.offset + last.length;
  return read_range(corpus.root / corpus.part_names[first.part], first.offset, end - first.offset);
}

// Appends {doc, tf} to the postings of every distinct term in `line`. A worker takes batches in
// increasing order, so its lists stay sorted by doc. Returns the number of tokens in the line.
std::size_t add_doc(uint32_t doc, std::string_view line, std::vector<PostingsMap>& shards) {
  const std::vector<std::string> tokens = tokenize(line);
  std::unordered_map<std::string, uint32_t> counts;
  for (const std::string& token : tokens) ++counts[token];
  for (const auto& [term, tf] : counts) {
    shards[shard_of(term, shards.size())][term].push_back({doc, tf});
  }
  return tokens.size();
}

// Indexes every doc of one batch. Each doc's slot in `docs` is written by exactly one worker.
std::expected<void, std::string> index_batch(const Corpus& corpus, const Batch& batch,
                                             std::vector<DocInfo>& docs, WorkerResult& result) {
  auto bytes = read_batch(corpus, batch);
  if (!bytes) return std::unexpected(bytes.error());

  const uint64_t batch_offset = corpus.docs[batch.first_doc].offset;
  for (uint32_t doc = batch.first_doc; doc < batch.first_doc + batch.count; ++doc) {
    const DocRef& ref = corpus.docs[doc];
    const std::string_view line(bytes->data() + (ref.offset - batch_offset), ref.length);
    const auto length = static_cast<uint32_t>(add_doc(doc, line, result.shards));
    docs[doc] = {std::string(split_line(line).title), ref, length};
    result.tokens += length;
  }
  return {};
}

// Takes batches from `next_batch` until none are left or some worker has failed.
void run_worker(const Corpus& corpus, std::atomic<std::size_t>& next_batch,
                std::atomic<bool>& failed, std::vector<DocInfo>& docs, WorkerResult& result) {
  try {
    while (!failed.load(std::memory_order_relaxed)) {
      const std::size_t batch_number = next_batch.fetch_add(1, std::memory_order_relaxed);
      if (batch_number >= corpus.batches.size()) return;
      auto done = index_batch(corpus, corpus.batches[batch_number], docs, result);
      if (!done) {
        result.error = done.error();
        failed.store(true, std::memory_order_relaxed);
        return;
      }
    }
  } catch (const std::exception& e) {  // for example std::bad_alloc
    result.error = e.what();
    failed.store(true, std::memory_order_relaxed);
  }
}

// ---- the merge ----

// Combines shard `shard` of every worker into one map, with every list sorted by doc.
// Worker 0's lists are already sorted, so only a term that several workers have needs a sort.
PostingsMap merge_shard(std::span<WorkerResult> workers, std::size_t shard) {
  PostingsMap merged = std::move(workers[0].shards[shard]);
  std::vector<std::vector<Posting>*> needs_sort;

  for (std::size_t w = 1; w < workers.size(); ++w) {
    PostingsMap& other = workers[w].shards[shard];
    merged.merge(other);                // moves over the terms that `merged` doesn't have yet
    for (auto& [term, list] : other) {  // the terms that are left are in both
      std::vector<Posting>& target = merged.at(term);
      target.insert(target.end(), list.begin(), list.end());
      needs_sort.push_back(&target);
    }
    PostingsMap().swap(other);  // give the memory back now
  }

  std::sort(needs_sort.begin(), needs_sort.end());
  needs_sort.erase(std::unique(needs_sort.begin(), needs_sort.end()), needs_sort.end());
  for (std::vector<Posting>* list : needs_sort) std::sort(list->begin(), list->end());
  return merged;
}

// Moves the shards' terms (which are all different) into one map.
PostingsMap union_of(std::vector<PostingsMap>& shards) {
  if (shards.size() == 1) return std::move(shards[0]);
  std::size_t terms = 0;
  for (const PostingsMap& shard : shards) terms += shard.size();
  PostingsMap all;
  all.reserve(terms);
  for (PostingsMap& shard : shards) all.merge(shard);
  return all;
}

}  // namespace

unsigned resolve_threads(std::string_view arg) {
  if (arg == "auto") return std::max(1u, std::thread::hardware_concurrency());

  unsigned value = 0;
  const char* const end = arg.data() + arg.size();
  const auto [parsed_end, error] = std::from_chars(arg.data(), end, value);
  if (error != std::errc() || parsed_end != end || value == 0) {
    throw std::invalid_argument("thread count must be a whole number >= 1 or \"auto\": \"" +
                                std::string(arg) + "\"");
  }
  return value;
}

std::expected<InMemoryIndex, std::string> build_index(const std::filesystem::path& corpus_root,
                                                      const BuildOptions& opts, BuildStats* stats) {
  const auto scan_start = Clock::now();
  auto corpus = open_corpus(corpus_root, opts.batch_size);
  if (!corpus) return std::unexpected(corpus.error());
  const double scan_s = seconds_since(scan_start);

  InMemoryIndex index;
  index.corpus_root = corpus->root.string();
  index.part_names = corpus->part_names;
  index.docs.resize(corpus->docs.size());

  const auto worker_count = static_cast<unsigned>(
      std::max<std::size_t>(1, std::min<std::size_t>(opts.threads, corpus->batches.size())));
  std::vector<WorkerResult> workers(worker_count);
  for (WorkerResult& worker : workers) worker.shards.resize(worker_count);

  // ---- tokenize ----
  const auto tokenize_start = Clock::now();
  std::atomic<std::size_t> next_batch{0};
  std::atomic<bool> failed{false};
  try {
    run_in_parallel(worker_count, [&](unsigned w) {
      run_worker(*corpus, next_batch, failed, index.docs, workers[w]);
    });
  } catch (const std::system_error& e) {  // the operating system refused to start a thread
    return std::unexpected(std::string("cannot start threads: ") + e.what());
  }
  const double tokenize_s = seconds_since(tokenize_start);

  uint64_t tokens = 0;
  for (const WorkerResult& worker : workers) {
    if (worker.error) return std::unexpected(*worker.error);
    tokens += worker.tokens;
  }

  // ---- merge ----
  const auto merge_start = Clock::now();
  std::vector<PostingsMap> merged(worker_count);
  run_in_parallel(worker_count,
                  [&](unsigned shard) { merged[shard] = merge_shard(workers, shard); });
  index.postings = union_of(merged);
  const double merge_s = seconds_since(merge_start);

  if (stats != nullptr) {
    *stats = {.scan_s = scan_s,
              .tokenize_s = tokenize_s,
              .merge_s = merge_s,
              .threads_used = worker_count,
              .docs = index.docs.size(),
              .tokens = tokens,
              .terms = index.postings.size(),
              .postings = index.total_postings()};
  }
  return index;
}

}  // namespace mse
