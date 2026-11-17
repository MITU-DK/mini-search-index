// Small helpers that the in-memory build (indexer.cpp) and the spilling build
// (build_pipeline.cpp) both use.
#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <thread>
#include <vector>

#include "corpus.hpp"

namespace mse {

using BuildClock = std::chrono::steady_clock;

inline double seconds_since(BuildClock::time_point start) {
  return std::chrono::duration<double>(BuildClock::now() - start).count();
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

// Reads all of a batch's lines with a single read. Doc i of the batch starts at
// docs[i].offset - docs[first].offset inside the returned bytes.
inline std::expected<std::string, std::string> read_batch(const Corpus& corpus,
                                                          const Batch& batch) {
  const DocRef& first = corpus.docs[batch.first_doc];
  const DocRef& last = corpus.docs[batch.first_doc + batch.count - 1];
  const uint64_t end = last.offset + last.length;
  return read_range(corpus.root / corpus.part_names[first.part], first.offset, end - first.offset);
}

}  // namespace mse
