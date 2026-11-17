#include "ranker.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace mse {
namespace {

// "a is a better result than b": higher score first, and the lower doc id wins a tie.
bool is_better(const SearchResult& a, const SearchResult& b) {
  if (a.score != b.score) return a.score > b.score;
  return a.doc < b.doc;
}

}  // namespace

void Accumulator::add(uint32_t doc, float score) {
  assert(doc < scores_.size() && score > 0.0f);
  if (scores_[doc] == 0.0f) touched_.push_back(doc);
  scores_[doc] += score;
}

void Accumulator::reset() {
  for (uint32_t doc : touched_) scores_[doc] = 0.0f;
  touched_.clear();
}

double bm25_idf(uint32_t num_docs, uint32_t df) {
  const double n = num_docs;
  const double d = df;
  return std::log(1.0 + (n - d + 0.5) / (d + 0.5));
}

double bm25_term_score(double idf, uint32_t tf, uint32_t doc_len, double avg_doc_len) {
  const double length_ratio = avg_doc_len > 0.0 ? doc_len / avg_doc_len : 0.0;
  const double norm = kBm25K1 * (1.0 - kBm25B + kBm25B * length_ratio);
  return idf * tf * (kBm25K1 + 1.0) / (tf + norm);
}

double tfidf_idf(uint32_t num_docs, uint32_t df) {
  return std::log(static_cast<double>(num_docs) / df);
}

double tfidf_term_score(double idf, uint32_t tf) { return (1.0 + std::log(tf)) * idf; }

std::vector<SearchResult> top_k(const Accumulator& acc, std::size_t k) {
  // A heap whose front is the worst result kept so far, so it is the one to replace.
  std::vector<SearchResult> heap;
  if (k == 0) return heap;
  heap.reserve(std::min<std::size_t>(k, acc.touched().size()));

  for (uint32_t doc : acc.touched()) {
    const SearchResult candidate{doc, acc.score(doc)};
    if (heap.size() < k) {
      heap.push_back(candidate);
      std::push_heap(heap.begin(), heap.end(), is_better);
    } else if (is_better(candidate, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), is_better);
      heap.back() = candidate;
      std::push_heap(heap.begin(), heap.end(), is_better);
    }
  }
  std::sort_heap(heap.begin(), heap.end(), is_better);  // best first
  return heap;
}

}  // namespace mse
