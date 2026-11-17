#include "indexer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "corpus.hpp"
#include "tokenizer.hpp"

namespace mse {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// Reads all of a batch's lines with a single read. Doc i of the batch starts at
// docs[i].offset - docs[first].offset inside the returned bytes.
std::expected<std::string, std::string> read_batch(const Corpus& corpus, const Batch& batch) {
  const DocRef& first = corpus.docs[batch.first_doc];
  const DocRef& last = corpus.docs[batch.first_doc + batch.count - 1];
  const uint64_t end = last.offset + last.length;
  return read_range(corpus.root / corpus.part_names[first.part], first.offset, end - first.offset);
}

// Appends {doc, tf} to the postings of every distinct term in `line`.
// Docs must be added in increasing doc order, which keeps every list sorted by doc.
// Returns the number of tokens in the line.
std::size_t add_doc(uint32_t doc, std::string_view line,
                    std::unordered_map<std::string, std::vector<Posting>>& postings) {
  const std::vector<std::string> tokens = tokenize(line);
  std::unordered_map<std::string, uint32_t> counts;
  for (const std::string& token : tokens) ++counts[token];
  for (const auto& [term, tf] : counts) postings[term].push_back({doc, tf});
  return tokens.size();
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

  const auto tokenize_start = Clock::now();
  InMemoryIndex index;
  index.corpus_root = corpus->root.string();
  index.part_names = corpus->part_names;
  index.docs.reserve(corpus->docs.size());

  uint64_t tokens = 0;
  for (const Batch& batch : corpus->batches) {
    auto bytes = read_batch(*corpus, batch);
    if (!bytes) return std::unexpected(bytes.error());

    const uint64_t batch_offset = corpus->docs[batch.first_doc].offset;
    for (uint32_t doc = batch.first_doc; doc < batch.first_doc + batch.count; ++doc) {
      const DocRef& ref = corpus->docs[doc];
      const std::string_view line(bytes->data() + (ref.offset - batch_offset), ref.length);
      const auto length = static_cast<uint32_t>(add_doc(doc, line, index.postings));
      index.docs.push_back({std::string(split_line(line).title), ref, length});
      tokens += length;
    }
  }
  const double tokenize_s = seconds_since(tokenize_start);

  if (stats != nullptr) {
    *stats = {.scan_s = scan_s,
              .tokenize_s = tokenize_s,
              .merge_s = 0,
              .docs = index.docs.size(),
              .tokens = tokens,
              .terms = index.postings.size(),
              .postings = index.total_postings()};
  }
  return index;
}

}  // namespace mse
