// Preview text for a search hit: a short window of the doc with the query words marked.
// The rules are in DESIGN.md §4 (Snippets).
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "corpus.hpp"
#include "reader.hpp"

namespace mse {

// A byte range inside Snippet::text.
struct Highlight {
  std::size_t offset;
  std::size_t length;
};

struct Snippet {
  std::string text;
  std::vector<Highlight> highlights;  // in order, never overlapping
};

// A window of kSnippetWindow bytes of `doc_text`, starting kSnippetLead bytes before the first
// word that is one of `terms` (or at the start if none is). `terms` must be lowercase words as
// produced by unique_query_terms. The window never cuts a UTF-8 character, runs of whitespace
// become one space, and whitespace at both ends is dropped.
Snippet make_snippet(std::string_view doc_text, std::span<const std::string> terms);

// Reads the doc's line from `part_file` and makes a snippet from its text (the part after the
// first TAB). Fails if the file can't be read.
[[nodiscard]] std::expected<Snippet, std::string> snippet_from_file(
    const std::filesystem::path& part_file, const DocRef& ref, std::span<const std::string> terms);

// Snippet for `doc` of `reader`. The part files are looked up in the reader's corpus root, or in
// `corpus_root_override` if that is not empty.
template <IndexReader R>
[[nodiscard]] std::expected<Snippet, std::string> snippet_for(
    const R& reader, uint32_t doc, std::span<const std::string> terms,
    std::string_view corpus_root_override = {}) {
  if (doc >= reader.num_docs()) {
    return std::unexpected("doc " + std::to_string(doc) + " does not exist");
  }
  const DocRef ref = reader.doc_ref(doc);
  const std::string_view part_name = reader.part_name(ref.part);
  // A part name is a plain file name. Anything with a '/' could point outside the corpus.
  if (part_name.empty() || part_name.find('/') != std::string_view::npos) {
    return std::unexpected("doc " + std::to_string(doc) + " has an invalid part file name");
  }
  const std::string_view root =
      corpus_root_override.empty() ? reader.corpus_root() : corpus_root_override;
  return snippet_from_file(std::filesystem::path(root) / part_name, ref, terms);
}

}  // namespace mse
