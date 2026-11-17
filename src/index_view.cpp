#include "index_view.hpp"

#include <algorithm>

#include "spellcheck.hpp"

namespace mse {

std::expected<IndexView, std::string> IndexView::open(std::span<const std::byte> bytes) {
  const auto header = decode_header(bytes);
  if (!header) return std::unexpected(header.error());
  return IndexView(bytes, *header);
}

IndexView::IndexView(std::span<const std::byte> file, const Header& header)
    : file_(file),
      header_(header),
      doc_lengths_(section_bytes(file, header, Section::DocLengths)),
      doc_refs_(section_bytes(file, header, Section::DocRefs)),
      doc_titles_(section_bytes(file, header, Section::DocTitles)),
      part_names_(section_bytes(file, header, Section::PartNames)),
      terms_(section_bytes(file, header, Section::Terms)),
      strings_(section_bytes(file, header, Section::Strings)),
      postings_(section_bytes(file, header, Section::Postings)),
      spell_nodes_(section_bytes(file, header, Section::SpellNodes)),
      spell_edges_(section_bytes(file, header, Section::SpellEdges)) {}

std::string_view IndexView::string_at(uint64_t offset, uint64_t length) const {
  const auto bytes = checked_range(strings_, offset, length, "string");
  if (!bytes) return {};
  return {reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::string_view IndexView::referenced_string(std::span<const std::byte> section,
                                              uint32_t index) const {
  const std::size_t at = std::size_t{index} * kStringRefSize;
  return string_at(load_le<uint32_t>(section, at), load_le<uint32_t>(section, at + 4));
}

TermEntry IndexView::term_entry(uint32_t index) const {
  const auto entry = terms_.subspan(std::size_t{index} * kTermEntrySize, kTermEntrySize);
  return decode_term_entry(entry.first<kTermEntrySize>());
}

std::string_view IndexView::term_at(uint32_t index) const {
  if (index >= header_.num_terms) return {};
  const TermEntry entry = term_entry(index);
  return string_at(entry.str_off, entry.str_len);
}

uint32_t IndexView::df_at(uint32_t index) const {
  return index < header_.num_terms ? term_entry(index).df : 0;
}

std::optional<uint32_t> IndexView::find_term(std::string_view term) const {
  uint32_t low = 0;
  uint32_t high = header_.num_terms;  // the answer, if there is one, is in [low, high)
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    const std::string_view candidate = term_at(middle);
    if (candidate == term) return middle;
    if (candidate < term) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> IndexView::lookup(std::string_view term, std::vector<Posting>& out) const {
  out.clear();
  const auto index = find_term(term);
  if (!index) return std::nullopt;

  const TermEntry entry = term_entry(*index);
  const auto block = checked_range(postings_, entry.postings_off, entry.postings_size, "postings");
  if (!block || entry.df == 0) return std::nullopt;
  if (!decode_postings_block(*block, entry.df, header_.compressed(), header_.num_docs, out)) {
    return std::nullopt;
  }
  return entry.df;
}

uint32_t IndexView::doc_length(uint32_t doc) const {
  if (doc >= header_.num_docs) return 0;
  return load_le<uint32_t>(doc_lengths_, std::size_t{doc} * kDocLengthSize);
}

std::string_view IndexView::doc_title(uint32_t doc) const {
  if (doc >= header_.num_docs) return {};
  return referenced_string(doc_titles_, doc);
}

DocRef IndexView::doc_ref(uint32_t doc) const {
  if (doc >= header_.num_docs) return {};
  const std::size_t at = std::size_t{doc} * kDocRefSize;
  return {load_le<uint32_t>(doc_refs_, at), load_le<uint32_t>(doc_refs_, at + 4),
          load_le<uint64_t>(doc_refs_, at + 8)};
}

std::string_view IndexView::part_name(uint32_t part) const {
  if (part >= header_.num_parts) return {};
  return referenced_string(part_names_, part);
}

std::string_view IndexView::corpus_root() const {
  return string_at(header_.corpus_root_offset, header_.corpus_root_length);
}

std::pair<uint32_t, uint32_t> IndexView::prefix_range(std::string_view prefix) const {
  // Terms sort as: those before the prefix, those starting with it, those after. So two binary
  // searches find where each of the last two groups begins.
  const auto partition_point = [this](auto belongs_to_front) {
    uint32_t low = 0;
    uint32_t high = header_.num_terms;
    while (low < high) {
      const uint32_t middle = low + (high - low) / 2;
      if (belongs_to_front(term_at(middle))) {
        low = middle + 1;
      } else {
        high = middle;
      }
    }
    return low;
  };
  const uint32_t first = partition_point([&](std::string_view term) { return term < prefix; });
  const uint32_t last = partition_point(
      [&](std::string_view term) { return term < prefix || term.starts_with(prefix); });
  return {first, last};
}

std::optional<std::string> IndexView::spell_suggest(std::string_view term) const {
  const std::size_t node_count = spell_nodes_.size() / kSpellNodeSize;
  const std::size_t edge_count = spell_edges_.size() / kSpellEdgeSize;
  if (term.empty() || node_count == 0) return std::nullopt;
  const uint32_t limit = max_edit_distance(term);

  struct Best {
    uint32_t distance;
    uint32_t df;
    uint32_t term_index;
  };
  std::optional<Best> best;
  const auto is_better = [&](const Best& candidate) {
    if (!best) return true;
    if (candidate.distance != best->distance) return candidate.distance < best->distance;
    if (candidate.df != best->df) return candidate.df > best->df;
    return candidate.term_index < best->term_index;  // the table is sorted: alphabetical order
  };

  // A valid tree reaches every node once. More visits than nodes means the file has a cycle.
  std::vector<uint32_t> pending{0};
  std::size_t visits = 0;
  while (!pending.empty() && visits++ < node_count) {
    const std::size_t node_at = std::size_t{pending.back()} * kSpellNodeSize;
    pending.pop_back();
    const auto term_index = load_le<uint32_t>(spell_nodes_, node_at);
    const auto first_edge = load_le<uint32_t>(spell_nodes_, node_at + 4);
    const auto edges_here = load_le<uint32_t>(spell_nodes_, node_at + 8);

    const std::string_view text = term_at(term_index);
    if (text.empty()) continue;  // damaged: nothing can be said about this subtree
    const uint32_t distance = levenshtein(term, text);
    const uint32_t df = df_at(term_index);
    if (distance <= limit && is_spell_eligible(df) && is_better({distance, df, term_index})) {
      best = Best{distance, df, term_index};
    }

    if (uint64_t{first_edge} + edges_here > edge_count) continue;  // damaged
    const uint32_t lowest = distance > limit ? distance - limit : 0;
    const uint64_t highest = uint64_t{distance} + limit;
    for (uint32_t i = 0; i < edges_here; ++i) {
      const std::size_t edge_at = (std::size_t{first_edge} + i) * kSpellEdgeSize;
      const auto edge_distance = load_le<uint32_t>(spell_edges_, edge_at);
      const auto child = load_le<uint32_t>(spell_edges_, edge_at + 4);
      if (edge_distance >= lowest && edge_distance <= highest && child < node_count) {
        pending.push_back(child);
      }
    }
  }
  if (!best) return std::nullopt;
  return std::string(term_at(best->term_index));
}

}  // namespace mse
