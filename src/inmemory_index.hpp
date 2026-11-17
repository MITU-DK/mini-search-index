// The index as plain C++ containers, before it is written to disk (DESIGN.md §4).
#pragma once

#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "corpus.hpp"
#include "posting.hpp"

namespace mse {

struct DocInfo {
  std::string title;
  DocRef ref;
  uint32_t length;  // number of tokens in the doc's line (title + text)
  bool operator==(const DocInfo&) const = default;
};

struct InMemoryIndex {
  std::string corpus_root;                                         // absolute
  std::vector<std::string> part_names;                             // DocRef::part indexes this
  std::vector<DocInfo> docs;                                       // index = doc id
  std::unordered_map<std::string, std::vector<Posting>> postings;  // each list sorted by doc

  double avg_doc_len() const {
    if (docs.empty()) return 0.0;
    const uint64_t total =
        std::accumulate(docs.begin(), docs.end(), uint64_t{0},
                        [](uint64_t sum, const DocInfo& d) { return sum + d.length; });
    return static_cast<double>(total) / static_cast<double>(docs.size());
  }

  uint64_t total_postings() const {
    return std::accumulate(
        postings.begin(), postings.end(), uint64_t{0},
        [](uint64_t sum, const auto& entry) { return sum + entry.second.size(); });
  }

  bool operator==(const InMemoryIndex&) const = default;
};

}  // namespace mse
