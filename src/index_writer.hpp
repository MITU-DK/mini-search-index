// Writes the binary index file, version 1 (DESIGN.md §8), one term at a time.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "index_format.hpp"
#include "inmemory_index.hpp"
#include "temp_files.hpp"

namespace mse {

struct WriteOptions {
  bool compress = true;     // gap + VByte postings; false writes raw {u32 doc, u32 tf} pairs
  bool build_spell = true;  // store the BK-tree of terms in at least kSpellMinDf docs
};

struct WriteStats {
  uint64_t file_bytes = 0;
  uint64_t postings_bytes = 0;      // size of the postings section
  uint64_t raw_postings_bytes = 0;  // what it would be uncompressed: 8 bytes per posting
  double write_s = 0;               // from create() to the end of finish()
  double spell_build_s = 0;         // building the BK-tree; 0 if there are no spell sections
  uint64_t spell_bytes = 0;         // size of the two spell sections together
};

// Memory use does not grow with the number of terms: postings, term entries and term strings go to
// three temporary files next to the output as they arrive, and finish() puts the file together.
class IndexWriter {
 public:
  [[nodiscard]] static std::expected<IndexWriter, std::string> create(
      const std::filesystem::path& out, const WriteOptions& options);

  // Exactly once, before add_term. `docs[i].ref.part` indexes `part_names`.
  [[nodiscard]] std::expected<void, std::string> set_docs(std::string_view corpus_root,
                                                          std::span<const std::string> part_names,
                                                          std::span<const DocInfo> docs);

  // Terms must come in strictly increasing byte order. `postings` must be non-empty, strictly
  // increasing by doc, with doc < num_docs and tf >= 1. A rejected term changes nothing.
  [[nodiscard]] std::expected<void, std::string> add_term(std::string_view term,
                                                          std::span<const Posting> postings);

  // Assembles the file in `<out>.tmp`, syncs it and renames it over `out`.
  [[nodiscard]] std::expected<WriteStats, std::string> finish();

  IndexWriter(IndexWriter&&) noexcept = default;
  IndexWriter& operator=(IndexWriter&&) noexcept = default;

 private:
  IndexWriter() = default;

  // Writes `text` to the strings stream and returns its offset within the strings section.
  [[nodiscard]] std::expected<uint32_t, std::string> append_string(std::string_view text);
  [[nodiscard]] std::expected<void, std::string> close_streams();
  [[nodiscard]] std::expected<void, std::string> assemble(const Header& header);

  std::filesystem::path out_;
  std::filesystem::path tmp_path_;
  std::filesystem::path postings_path_;
  std::filesystem::path terms_path_;
  std::filesystem::path strings_path_;
  WriteOptions options_;
  std::chrono::steady_clock::time_point started_;

  TempFiles temp_files_;  // declared before the streams, so it deletes after they are closed
  std::ofstream postings_;
  std::ofstream terms_;
  std::ofstream strings_;

  // The four per-doc sections, filled by set_docs.
  std::vector<std::byte> doc_lengths_;
  std::vector<std::byte> doc_refs_;
  std::vector<std::byte> doc_titles_;
  std::vector<std::byte> part_names_;
  Header header_;
  bool docs_set_ = false;
  bool finished_ = false;

  uint64_t strings_bytes_ = 0;
  uint64_t postings_bytes_ = 0;
  uint64_t total_postings_ = 0;
  uint32_t num_terms_ = 0;
  std::string last_term_;
  std::vector<std::byte> block_;  // scratch space for one postings block

  // The terms the BK-tree is built from, collected as they arrive: their bytes, concatenated, and
  // where each one is.
  struct SpellTerm {
    std::size_t offset;
    uint32_t length;
    uint32_t term_index;
  };
  std::string spell_text_;
  std::vector<SpellTerm> spell_terms_;
  std::vector<std::byte> spell_nodes_;  // set by finish
  std::vector<std::byte> spell_edges_;
  double spell_build_s_ = 0;
  void build_spell_sections();
};

// Sorts the index's terms and feeds them to an IndexWriter.
[[nodiscard]] std::expected<WriteStats, std::string> write_index(const InMemoryIndex& index,
                                                                 const std::filesystem::path& out,
                                                                 const WriteOptions& options);

}  // namespace mse
