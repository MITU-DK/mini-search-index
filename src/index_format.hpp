// The binary index file, version 1 (DESIGN.md §8): constants, little-endian access and the header.
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "constants.hpp"

namespace mse {

inline constexpr std::string_view kMagic = "MSEINDEX";
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint32_t kFlagVByte = 1;  // bit 0 of the header flags: postings are gap + VByte

enum class Section : std::size_t {
  DocLengths,
  DocRefs,
  DocTitles,
  PartNames,
  Terms,
  Strings,
  Postings,
  SpellNodes,
  SpellEdges,
};
inline constexpr std::size_t kSectionCount = 9;

// Entry sizes of the fixed-size sections.
inline constexpr std::size_t kDocLengthSize = 4;
inline constexpr std::size_t kDocRefSize = 16;    // u32 part, u32 length, u64 offset
inline constexpr std::size_t kStringRefSize = 8;  // u32 offset, u32 length (titles, part names)
inline constexpr std::size_t kTermEntrySize = 24;
inline constexpr std::size_t kSpellNodeSize = 12;
inline constexpr std::size_t kSpellEdgeSize = 8;
inline constexpr std::size_t kRawPostingSize = 8;  // u32 doc, u32 tf

// Byte offsets of the header fields.
inline constexpr std::size_t kHeaderVersion = 8;
inline constexpr std::size_t kHeaderFlags = 12;
inline constexpr std::size_t kHeaderNumDocs = 16;
inline constexpr std::size_t kHeaderNumTerms = 20;
inline constexpr std::size_t kHeaderAvgDocLen = 24;
inline constexpr std::size_t kHeaderTotalPostings = 32;
inline constexpr std::size_t kHeaderFileSize = 40;
inline constexpr std::size_t kHeaderSections = 48;  // 9 x {u64 offset, u64 size}
inline constexpr std::size_t kHeaderCorpusRoot = 192;
inline constexpr std::size_t kHeaderNumParts = 200;

// Reads and writes a little-endian number at `offset` with memcpy, never through a cast pointer.
// The caller has checked that the bytes are inside `data`.
template <class T>
T load_le(std::span<const std::byte> data, std::size_t offset) {
  assert(offset <= data.size() && sizeof(T) <= data.size() - offset);
  T value;
  std::memcpy(&value, data.data() + offset, sizeof(T));
  return value;
}

template <class T>
void store_le(std::span<std::byte> data, std::size_t offset, T value) {
  assert(offset <= data.size() && sizeof(T) <= data.size() - offset);
  std::memcpy(data.data() + offset, &value, sizeof(T));
}

constexpr uint64_t align_up(uint64_t value) {
  return (value + kSectionAlign - 1) / kSectionAlign * kSectionAlign;
}

struct SectionRef {
  uint64_t offset = 0;  // from the start of the file
  uint64_t size = 0;
  bool operator==(const SectionRef&) const = default;
};

struct Header {
  uint32_t flags = 0;
  uint32_t num_docs = 0;
  uint32_t num_terms = 0;
  double avg_doc_len = 0;
  uint64_t total_postings = 0;
  uint64_t file_size = 0;
  std::array<SectionRef, kSectionCount> sections{};
  uint32_t corpus_root_offset = 0;  // within the strings section
  uint32_t corpus_root_length = 0;
  uint32_t num_parts = 0;

  const SectionRef& section(Section s) const { return sections[static_cast<std::size_t>(s)]; }
  SectionRef& section(Section s) { return sections[static_cast<std::size_t>(s)]; }
  bool compressed() const { return (flags & kFlagVByte) != 0; }
};

// One entry of the term table. Offsets are relative to their own section.
struct TermEntry {
  uint32_t str_off = 0;
  uint32_t str_len = 0;
  uint32_t df = 0;
  uint32_t postings_size = 0;  // bytes
  uint64_t postings_off = 0;
};

void encode_header(const Header& header, std::span<std::byte, kHeaderSize> out);
void encode_term_entry(const TermEntry& entry, std::span<std::byte, kTermEntrySize> out);
TermEntry decode_term_entry(std::span<const std::byte, kTermEntrySize> in);

// Checks everything that can be checked in O(1) (§8 Validation) and returns the header.
// `file` is the whole file.
[[nodiscard]] std::expected<Header, std::string> decode_header(std::span<const std::byte> file);

// The bytes of one section of `file`. Only valid after decode_header accepted the file.
std::span<const std::byte> section_bytes(std::span<const std::byte> file, const Header& header,
                                         Section section);

// The `size` bytes at `offset` inside `data`, or an error naming `what` if they are not all there.
[[nodiscard]] std::expected<std::span<const std::byte>, std::string> checked_range(
    std::span<const std::byte> data, uint64_t offset, uint64_t size, std::string_view what);

}  // namespace mse
