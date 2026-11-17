// Searches an index file in place: no parsing, no copying. The file's bytes are borrowed, so they
// must outlive the view and must not change. A view is immutable, so threads can share one.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "corpus.hpp"
#include "index_format.hpp"
#include "posting.hpp"

namespace mse {

class IndexView {
 public:
  // Does the O(1) checks of DESIGN.md §8 and nothing else. Whatever they cannot see is checked
  // when it is used: a damaged range makes lookup() return nullopt and the string getters return
  // "". It never reads outside the file.
  [[nodiscard]] static std::expected<IndexView, std::string> open(std::span<const std::byte> bytes);

  uint32_t num_docs() const { return header_.num_docs; }
  uint32_t num_terms() const { return header_.num_terms; }
  double avg_doc_len() const { return header_.avg_doc_len; }
  bool compressed() const { return header_.compressed(); }

  // The index of `term` in the sorted term table, by binary search.
  std::optional<uint32_t> find_term(std::string_view term) const;
  std::string_view term_at(uint32_t index) const;  // "" if out of range or damaged
  uint32_t df_at(uint32_t index) const;            // 0 if out of range

  // Clears `out`, fills it with the term's postings and returns its df; nullopt (and an empty
  // `out`) if the term is not indexed or its postings are damaged.
  std::optional<uint32_t> lookup(std::string_view term, std::vector<Posting>& out) const;

  // Docs and parts that do not exist give 0, "" or an empty DocRef.
  uint32_t doc_length(uint32_t doc) const;
  std::string_view doc_title(uint32_t doc) const;
  DocRef doc_ref(uint32_t doc) const;
  std::string_view part_name(uint32_t part) const;
  std::string_view corpus_root() const;

  // The terms that start with `prefix` are the entries [first, last) of the term table.
  std::pair<uint32_t, uint32_t> prefix_range(std::string_view prefix) const;

 private:
  IndexView(std::span<const std::byte> file, const Header& header);

  // The string at [offset, offset + length) of the strings section, or "" if it is not all there.
  std::string_view string_at(uint64_t offset, uint64_t length) const;
  // Both read an {offset, length} pair of a string-reference section; the index is in range.
  std::string_view referenced_string(std::span<const std::byte> section, uint32_t index) const;
  TermEntry term_entry(uint32_t index) const;  // the index is in range

  std::span<const std::byte> file_;
  Header header_;
  std::span<const std::byte> doc_lengths_;
  std::span<const std::byte> doc_refs_;
  std::span<const std::byte> doc_titles_;
  std::span<const std::byte> part_names_;
  std::span<const std::byte> terms_;
  std::span<const std::byte> strings_;
  std::span<const std::byte> postings_;
};

}  // namespace mse
