// Turns a query string into ranked hits over any IndexReader.
#pragma once

#include <algorithm>
#include <concepts>
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

// A reader that can also suggest a correction for a misspelled word (IndexView can; the
// in-memory reader cannot).
template <class R>
concept SpellSource = requires(const R& reader, std::string_view term) {
  { reader.spell_suggest(term) } -> std::same_as<std::optional<std::string>>;
};

// Searches with OR semantics: a doc matches if it has at least one query term.
// If a query term is not in the index and the reader can spell-check, the words it has no match for
// are replaced by their best suggestion and the corrected query is searched instead
// (SearchResponse::corrected_query says what was searched).
// One Searcher per thread: it owns scratch memory that search() reuses. The reader may be shared.
template <IndexReader R>
class Searcher {
 public:
  explicit Searcher(const R& reader) : reader_(reader), accumulator_(reader.num_docs()) {}

  SearchResponse search(std::string_view query, const SearchOptions& options = {}) {
    SearchResponse response;
    response.terms = unique_query_terms(query);

    std::vector<QueryTerm> terms;
    std::vector<std::size_t> unknown;  // positions in response.terms of words that are not indexed
    look_up(response.terms, terms, unknown);

    if constexpr (SpellSource<R>) {
      if (options.spellcheck && !unknown.empty()) {
        if (auto corrected = correct(response.terms, unknown)) {
          response.terms = std::move(corrected->first);
          response.corrected_query = std::move(corrected->second);
          look_up(response.terms, terms, unknown);
        }
      }
    }

    const auto doc_length = [this](uint32_t doc) { return reader_.doc_length(doc); };
    response.hits = rank(std::span<const QueryTerm>(terms), reader_.num_docs(),
                         reader_.avg_doc_len(), doc_length, options.mode, options.k, accumulator_);
    return response;
  }

 private:
  // Fills `terms` with the postings of the words that are indexed, and `unknown` with the
  // positions of those that are not.
  void look_up(const std::vector<std::string>& words, std::vector<QueryTerm>& terms,
               std::vector<std::size_t>& unknown) const {
    terms.clear();
    unknown.clear();
    for (std::size_t i = 0; i < words.size(); ++i) {
      QueryTerm entry;
      const std::optional<uint32_t> df = reader_.lookup(words[i], entry.postings);
      if (!df) {
        unknown.push_back(i);
        continue;
      }
      entry.df = *df;
      terms.push_back(std::move(entry));
    }
  }

  // Replaces each unknown word that has a suggestion. Returns the new unique words and the
  // corrected query text, or nothing if no word could be corrected.
  std::optional<std::pair<std::vector<std::string>, std::string>> correct(
      const std::vector<std::string>& words, const std::vector<std::size_t>& unknown) const {
    if constexpr (SpellSource<R>) {
      std::vector<std::string> replaced = words;
      bool changed = false;
      for (const std::size_t i : unknown) {
        if (auto suggestion = reader_.spell_suggest(words[i])) {
          replaced[i] = std::move(*suggestion);
          changed = true;
        }
      }
      if (!changed) return std::nullopt;

      std::vector<std::string> unique;  // two words may have been corrected to the same one
      std::string text;
      for (std::string& word : replaced) {
        if (std::find(unique.begin(), unique.end(), word) != unique.end()) continue;
        text += (unique.empty() ? "" : " ") + word;
        unique.push_back(std::move(word));
      }
      return std::pair{std::move(unique), std::move(text)};
    } else {
      return std::nullopt;
    }
  }

  const R& reader_;
  Accumulator accumulator_;
};

}  // namespace mse
