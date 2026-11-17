// What the Searcher needs from an index (the IndexReader concept), and the in-memory version.
// The on-disk version is IndexView (index_view.hpp).
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "corpus.hpp"
#include "inmemory_index.hpp"

namespace mse {

// lookup(term, out) clears `out`, fills it with the term's postings sorted by doc, and returns
// the term's document frequency, or nullopt (and an empty `out`) if the term is not indexed.
// part_name(part) returns "" for a part index that does not exist.
template <class R>
concept IndexReader =
    requires(const R& reader, std::string_view term, uint32_t number, std::vector<Posting>& out) {
      { reader.num_docs() } -> std::convertible_to<uint32_t>;
      { reader.avg_doc_len() } -> std::convertible_to<double>;
      { reader.lookup(term, out) } -> std::same_as<std::optional<uint32_t>>;
      { reader.doc_length(number) } -> std::convertible_to<uint32_t>;
      { reader.doc_title(number) } -> std::convertible_to<std::string_view>;
      { reader.doc_ref(number) } -> std::same_as<DocRef>;
      { reader.part_name(number) } -> std::convertible_to<std::string_view>;
      { reader.corpus_root() } -> std::convertible_to<std::string_view>;
    };

// Reads from an InMemoryIndex, which must outlive this object.
class MemoryReader {
 public:
  explicit MemoryReader(const InMemoryIndex& index)
      : index_(index), avg_doc_len_(index.avg_doc_len()) {}

  uint32_t num_docs() const { return static_cast<uint32_t>(index_.docs.size()); }
  double avg_doc_len() const { return avg_doc_len_; }

  std::optional<uint32_t> lookup(std::string_view term, std::vector<Posting>& out) const {
    out.clear();
    const auto found = index_.postings.find(std::string(term));
    if (found == index_.postings.end()) return std::nullopt;
    out = found->second;
    return static_cast<uint32_t>(out.size());
  }

  // The doc-based getters require doc < num_docs().
  uint32_t doc_length(uint32_t doc) const { return index_.docs[doc].length; }
  std::string_view doc_title(uint32_t doc) const { return index_.docs[doc].title; }
  DocRef doc_ref(uint32_t doc) const { return index_.docs[doc].ref; }

  std::string_view part_name(uint32_t part) const {
    return part < index_.part_names.size() ? std::string_view(index_.part_names[part])
                                           : std::string_view();
  }
  std::string_view corpus_root() const { return index_.corpus_root; }

 private:
  const InMemoryIndex& index_;
  double avg_doc_len_;  // kept here because InMemoryIndex::avg_doc_len() scans every doc
};

}  // namespace mse
