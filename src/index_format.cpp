#include "index_format.hpp"

#include <algorithm>
#include <cmath>

namespace mse {
namespace {

// What each section's size must be, as a multiple of its entry size: {count, entry size}.
struct FixedSize {
  Section section;
  uint64_t count;
  std::size_t entry_size;
  const char* name;
};

std::expected<void, std::string> check_section_sizes(const Header& header) {
  const FixedSize fixed[] = {
      {Section::DocLengths, header.num_docs, kDocLengthSize, "doc_lengths"},
      {Section::DocRefs, header.num_docs, kDocRefSize, "doc_refs"},
      {Section::DocTitles, header.num_docs, kStringRefSize, "doc_titles"},
      {Section::PartNames, header.num_parts, kStringRefSize, "part_names"},
      {Section::Terms, header.num_terms, kTermEntrySize, "terms"},
  };
  for (const FixedSize& f : fixed) {
    if (header.section(f.section).size != f.count * f.entry_size) {
      return std::unexpected(std::string("section ") + f.name + " has the wrong size");
    }
  }
  if (header.section(Section::SpellNodes).size % kSpellNodeSize != 0) {
    return std::unexpected("section spell_nodes has the wrong size");
  }
  if (header.section(Section::SpellEdges).size % kSpellEdgeSize != 0) {
    return std::unexpected("section spell_edges has the wrong size");
  }
  return {};
}

// Each section must be aligned, inside the file, after the header, and after the one before it.
std::expected<void, std::string> check_section_places(const Header& header) {
  uint64_t previous_end = kHeaderSize;
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    const SectionRef& ref = header.sections[i];
    const std::string name = "section " + std::to_string(i);
    if (ref.offset % kSectionAlign != 0) return std::unexpected(name + " is not 8-byte aligned");
    if (ref.offset < previous_end) return std::unexpected(name + " overlaps what comes before it");
    if (ref.offset > header.file_size || ref.size > header.file_size - ref.offset) {
      return std::unexpected(name + " reaches past the end of the file");
    }
    previous_end = ref.offset + ref.size;
  }
  return {};
}

}  // namespace

void encode_header(const Header& header, std::span<std::byte, kHeaderSize> out) {
  std::fill(out.begin(), out.end(), std::byte{0});
  std::memcpy(out.data(), kMagic.data(), kMagic.size());
  store_le<uint32_t>(out, kHeaderVersion, kFormatVersion);
  store_le<uint32_t>(out, kHeaderFlags, header.flags);
  store_le<uint32_t>(out, kHeaderNumDocs, header.num_docs);
  store_le<uint32_t>(out, kHeaderNumTerms, header.num_terms);
  store_le<double>(out, kHeaderAvgDocLen, header.avg_doc_len);
  store_le<uint64_t>(out, kHeaderTotalPostings, header.total_postings);
  store_le<uint64_t>(out, kHeaderFileSize, header.file_size);
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    store_le<uint64_t>(out, kHeaderSections + 16 * i, header.sections[i].offset);
    store_le<uint64_t>(out, kHeaderSections + 16 * i + 8, header.sections[i].size);
  }
  store_le<uint32_t>(out, kHeaderCorpusRoot, header.corpus_root_offset);
  store_le<uint32_t>(out, kHeaderCorpusRoot + 4, header.corpus_root_length);
  store_le<uint32_t>(out, kHeaderNumParts, header.num_parts);
}

void encode_term_entry(const TermEntry& entry, std::span<std::byte, kTermEntrySize> out) {
  store_le<uint32_t>(out, 0, entry.str_off);
  store_le<uint32_t>(out, 4, entry.str_len);
  store_le<uint32_t>(out, 8, entry.df);
  store_le<uint32_t>(out, 12, entry.postings_size);
  store_le<uint64_t>(out, 16, entry.postings_off);
}

TermEntry decode_term_entry(std::span<const std::byte, kTermEntrySize> in) {
  return {.str_off = load_le<uint32_t>(in, 0),
          .str_len = load_le<uint32_t>(in, 4),
          .df = load_le<uint32_t>(in, 8),
          .postings_size = load_le<uint32_t>(in, 12),
          .postings_off = load_le<uint64_t>(in, 16)};
}

std::expected<Header, std::string> decode_header(std::span<const std::byte> file) {
  if (file.size() < kHeaderSize) return std::unexpected("file is smaller than the 256-byte header");
  if (std::memcmp(file.data(), kMagic.data(), kMagic.size()) != 0) {
    return std::unexpected("not an index file (wrong magic)");
  }
  const auto version = load_le<uint32_t>(file, kHeaderVersion);
  if (version != kFormatVersion) {
    return std::unexpected("unsupported index version " + std::to_string(version));
  }

  Header header;
  header.flags = load_le<uint32_t>(file, kHeaderFlags);
  header.num_docs = load_le<uint32_t>(file, kHeaderNumDocs);
  header.num_terms = load_le<uint32_t>(file, kHeaderNumTerms);
  header.avg_doc_len = load_le<double>(file, kHeaderAvgDocLen);
  header.total_postings = load_le<uint64_t>(file, kHeaderTotalPostings);
  header.file_size = load_le<uint64_t>(file, kHeaderFileSize);
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    header.sections[i] = {load_le<uint64_t>(file, kHeaderSections + 16 * i),
                          load_le<uint64_t>(file, kHeaderSections + 16 * i + 8)};
  }
  header.corpus_root_offset = load_le<uint32_t>(file, kHeaderCorpusRoot);
  header.corpus_root_length = load_le<uint32_t>(file, kHeaderCorpusRoot + 4);
  header.num_parts = load_le<uint32_t>(file, kHeaderNumParts);

  if ((header.flags & ~kFlagVByte) != 0) return std::unexpected("unknown header flags");
  if (header.file_size != file.size()) {
    return std::unexpected("file size does not match the header (truncated or extended?)");
  }
  if (!std::isfinite(header.avg_doc_len) || header.avg_doc_len < 0) {
    return std::unexpected("average document length is not a valid number");
  }
  if (auto ok = check_section_places(header); !ok) return std::unexpected(ok.error());
  if (auto ok = check_section_sizes(header); !ok) return std::unexpected(ok.error());

  const uint64_t strings_size = header.section(Section::Strings).size;
  if (header.corpus_root_offset > strings_size ||
      header.corpus_root_length > strings_size - header.corpus_root_offset) {
    return std::unexpected("corpus root lies outside the strings section");
  }
  return header;
}

std::span<const std::byte> section_bytes(std::span<const std::byte> file, const Header& header,
                                         Section section) {
  const SectionRef& ref = header.section(section);
  return file.subspan(ref.offset, ref.size);
}

std::expected<std::span<const std::byte>, std::string> checked_range(
    std::span<const std::byte> data, uint64_t offset, uint64_t size, std::string_view what) {
  if (offset > data.size() || size > data.size() - offset) {
    return std::unexpected(std::string(what) + " reaches outside its section");
  }
  return data.subspan(offset, size);
}

}  // namespace mse
