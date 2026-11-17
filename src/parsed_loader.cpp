#include "parsed_loader.hpp"

#include <system_error>

#include "corpus.hpp"
#include "index_format.hpp"
#include "varint.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

// What the decoding functions need from the file, gathered once.
struct Context {
  std::span<const std::byte> file;
  Header header;
  std::span<const std::byte> strings;
};

std::expected<std::string, std::string> read_string(const Context& ctx, uint64_t offset,
                                                    uint64_t length, std::string_view what) {
  const auto bytes = checked_range(ctx.strings, offset, length, what);
  if (!bytes) return std::unexpected(bytes.error());
  return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// Entry `index` of a fixed-size section.
template <std::size_t kEntrySize>
std::span<const std::byte, kEntrySize> entry_at(std::span<const std::byte> section,
                                                std::size_t index) {
  return section.subspan(index * kEntrySize, kEntrySize).template first<kEntrySize>();
}

std::expected<void, std::string> load_part_names(const Context& ctx, InMemoryIndex& index) {
  const auto section = section_bytes(ctx.file, ctx.header, Section::PartNames);
  for (std::size_t i = 0; i < ctx.header.num_parts; ++i) {
    const auto entry = entry_at<kStringRefSize>(section, i);
    auto name =
        read_string(ctx, load_le<uint32_t>(entry, 0), load_le<uint32_t>(entry, 4), "part name");
    if (!name) return std::unexpected(name.error());
    index.part_names.push_back(*std::move(name));
  }
  return {};
}

std::expected<void, std::string> load_docs(const Context& ctx, InMemoryIndex& index) {
  const auto lengths = section_bytes(ctx.file, ctx.header, Section::DocLengths);
  const auto refs = section_bytes(ctx.file, ctx.header, Section::DocRefs);
  const auto titles = section_bytes(ctx.file, ctx.header, Section::DocTitles);

  index.docs.reserve(ctx.header.num_docs);
  for (std::size_t i = 0; i < ctx.header.num_docs; ++i) {
    const auto ref = entry_at<kDocRefSize>(refs, i);
    const auto title = entry_at<kStringRefSize>(titles, i);
    const DocRef doc_ref{load_le<uint32_t>(ref, 0), load_le<uint32_t>(ref, 4),
                         load_le<uint64_t>(ref, 8)};
    if (doc_ref.part >= ctx.header.num_parts) {
      return std::unexpected("doc " + std::to_string(i) + " names a part that does not exist");
    }
    auto text =
        read_string(ctx, load_le<uint32_t>(title, 0), load_le<uint32_t>(title, 4), "doc title");
    if (!text) return std::unexpected(text.error());
    index.docs.push_back(
        {*std::move(text), doc_ref, load_le<uint32_t>(lengths, i * kDocLengthSize)});
  }
  return {};
}

std::expected<std::vector<Posting>, std::string> decode_list(const Context& ctx,
                                                             const TermEntry& entry,
                                                             std::span<const std::byte> postings) {
  const auto block =
      checked_range(postings, entry.postings_off, entry.postings_size, "postings block");
  if (!block) return std::unexpected(block.error());

  std::vector<Posting> list;
  const auto decoded =
      decode_postings_block(*block, entry.df, ctx.header.compressed(), ctx.header.num_docs, list);
  if (!decoded) return std::unexpected(decoded.error());
  return list;
}

std::expected<void, std::string> load_terms(const Context& ctx, InMemoryIndex& index) {
  const auto terms = section_bytes(ctx.file, ctx.header, Section::Terms);
  const auto postings = section_bytes(ctx.file, ctx.header, Section::Postings);

  index.postings.reserve(ctx.header.num_terms);
  std::string previous;
  uint64_t total_postings = 0;
  for (std::size_t i = 0; i < ctx.header.num_terms; ++i) {
    const TermEntry entry = decode_term_entry(entry_at<kTermEntrySize>(terms, i));
    auto term = read_string(ctx, entry.str_off, entry.str_len, "term");
    if (!term) return std::unexpected(term.error());
    if (term->empty()) return std::unexpected("the index contains an empty term");
    if (i > 0 && term->compare(previous) <= 0) {
      return std::unexpected("terms are not in strictly increasing order at \"" + *term + "\"");
    }
    if (entry.df == 0) return std::unexpected("term \"" + *term + "\" has no postings");

    auto list = decode_list(ctx, entry, postings);
    if (!list) return std::unexpected("term \"" + *term + "\": " + list.error());
    total_postings += list->size();
    previous = *term;
    index.postings.emplace(*std::move(term), *std::move(list));
  }
  if (total_postings != ctx.header.total_postings) {
    return std::unexpected("total_postings in the header does not match the postings");
  }
  return {};
}

}  // namespace

std::expected<InMemoryIndex, std::string> load_parsed(const fs::path& path) {
  std::error_code error;
  const uint64_t size = fs::file_size(path, error);
  if (error) return std::unexpected(path.string() + ": " + error.message());
  const auto content = read_range(path, 0, size);
  if (!content) return std::unexpected(content.error());

  const auto file = std::as_bytes(std::span(*content));
  const auto header = decode_header(file);
  if (!header) return std::unexpected(path.string() + ": " + header.error());

  const Context ctx{file, *header, section_bytes(file, *header, Section::Strings)};
  InMemoryIndex index;
  auto root =
      read_string(ctx, header->corpus_root_offset, header->corpus_root_length, "corpus root");
  if (!root) return std::unexpected(path.string() + ": " + root.error());
  index.corpus_root = *std::move(root);

  for (const auto& step : {load_part_names, load_docs, load_terms}) {
    if (auto ok = step(ctx, index); !ok) return std::unexpected(path.string() + ": " + ok.error());
  }
  return index;
}

}  // namespace mse
