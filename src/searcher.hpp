// Turns a query string into ranked hits over any IndexReader.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "constants.hpp"
#include "ranker.hpp"
#include "reader.hpp"
#include "tokenizer.hpp"

namespace mse {

struct SearchOptions {
  std::size_t k = kDefaultTopK;
  RankMode mode = RankMode::BM25;
  bool spellcheck = true;
};

struct SearchResponse {
  std::vector<SearchResult> hits;  // best first
  std::vector<std::string> terms;  // the query's unique terms, as they were searched
  std::optional<std::string> corrected_query;
};

// The unique words of `query`, lowercased, in order of first appearance.
inline std::vector<std::string> unique_query_terms(std::string_view query) {
  std::vector<std::string> terms;
  for (std::string& token : tokenize(query)) {
    if (std::find(terms.begin(), terms.end(), token) == terms.end()) {
      terms.push_back(std::move(token));
    }
  }
  return terms;
}

// Searches with OR semantics: a doc matches if it has at least one query term.
// One Searcher per thread: it owns scratch memory that search() reuses. The reader may be shared.
template <IndexReader R>
class Searcher {
 public:
  explicit Searcher(const R& reader) : reader_(reader), accumulator_(reader.num_docs()) {}

  SearchResponse search(std::string_view query, const SearchOptions& options = {}) {
    SearchResponse response;
    response.terms = unique_query_terms(query);

    std::vector<QueryTerm> terms;
    for (const std::string& term : response.terms) {
      QueryTerm entry;
      const std::optional<uint32_t> df = reader_.lookup(term, entry.postings);
      if (!df) continue;  // not in the index: it matches nothing
      entry.df = *df;
      terms.push_back(std::move(entry));
    }

    const auto doc_length = [this](uint32_t doc) { return reader_.doc_length(doc); };
    response.hits = rank(std::span<const QueryTerm>(terms), reader_.num_docs(),
                         reader_.avg_doc_len(), doc_length, options.mode, options.k, accumulator_);
    return response;
  }

 private:
  const R& reader_;
  Accumulator accumulator_;
};

}  // namespace mse
