// BM25 and TF-IDF scoring with a top-k heap. The exact formulas are in DESIGN.md §7.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "constants.hpp"
#include "inmemory_index.hpp"

namespace mse {

enum class RankMode { BM25, TFIDF };

struct SearchResult {
  uint32_t doc;
  float score;
  bool operator==(const SearchResult&) const = default;
};

// One query term as the ranker sees it: its document frequency and its postings.
struct QueryTerm {
  uint32_t df;
  std::vector<Posting> postings;
};

// Scores for every doc, kept in a dense array so adding is O(1), plus a list of the docs that
// have a score so that clearing and scanning cost O(touched), not O(num_docs).
class Accumulator {
 public:
  explicit Accumulator(uint32_t num_docs) : scores_(num_docs, 0.0f) {}

  // Requires doc < num_docs and score > 0 (a zero score is how "not touched yet" is detected).
  void add(uint32_t doc, float score);
  void reset();

  std::span<const uint32_t> touched() const { return touched_; }
  float score(uint32_t doc) const { return scores_[doc]; }

 private:
  std::vector<float> scores_;
  std::vector<uint32_t> touched_;
};

// The score one posting adds for one term (DESIGN.md §7).
double bm25_idf(uint32_t num_docs, uint32_t df);
double bm25_term_score(double idf, uint32_t tf, uint32_t doc_len, double avg_doc_len);
double tfidf_idf(uint32_t num_docs, uint32_t df);
double tfidf_term_score(double idf, uint32_t tf);

// The k best touched docs: score descending, then doc id ascending. O(touched * log k).
std::vector<SearchResult> top_k(const Accumulator& acc, std::size_t k);

// Scores every posting of every term, returns the top k, and leaves `acc` empty again.
// `terms` must be unique, and `acc` must have been made with `num_docs`.
// `doc_len(doc)` returns a doc's length in tokens. Postings with doc >= num_docs and postings
// that score 0 are skipped, so a corrupt index can't write outside the accumulator.
template <class DocLen>
std::vector<SearchResult> rank(std::span<const QueryTerm> terms, uint32_t num_docs,
                               double avg_doc_len, const DocLen& doc_len, RankMode mode,
                               std::size_t k, Accumulator& acc) {
  if (num_docs == 0) return {};

  for (const QueryTerm& term : terms) {
    if (term.df == 0) continue;
    const double idf =
        mode == RankMode::BM25 ? bm25_idf(num_docs, term.df) : tfidf_idf(num_docs, term.df);
    for (const Posting& posting : term.postings) {
      if (posting.doc >= num_docs) continue;
      const double score = mode == RankMode::BM25
                               ? bm25_term_score(idf, posting.tf, doc_len(posting.doc), avg_doc_len)
                               : tfidf_term_score(idf, posting.tf);
      const auto contribution = static_cast<float>(score);
      if (contribution > 0.0f) acc.add(posting.doc, contribution);  // also false for NaN
    }
  }

  std::vector<SearchResult> results = top_k(acc, k);
  acc.reset();
  return results;
}

}  // namespace mse
