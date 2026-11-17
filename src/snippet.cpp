#include "snippet.hpp"

#include <algorithm>
#include <optional>

#include "constants.hpp"
#include "tokenizer.hpp"

namespace mse {
namespace {

// The bytes [start, end) of the doc text that go into the snippet.
struct Window {
  std::size_t start;
  std::size_t end;
};

// A UTF-8 continuation byte (10xxxxxx) is the middle of a character, never its start.
bool is_continuation_byte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// Does `word` equal `term` once ASCII is lowercased? `term` is already lowercase.
bool equals_lowercased(std::string_view word, const std::string& term) {
  if (word.size() != term.size()) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    const char c = word[i];
    const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    if (lower != term[i]) return false;
  }
  return true;
}

bool is_query_word(std::string_view word, std::span<const std::string> terms) {
  return std::any_of(terms.begin(), terms.end(),
                     [word](const std::string& term) { return equals_lowercased(word, term); });
}

Window choose_window(std::string_view text, std::optional<std::size_t> first_match) {
  std::size_t start = 0;
  if (first_match && *first_match > kSnippetLead) start = *first_match - kSnippetLead;
  while (start < text.size() && is_continuation_byte(text[start])) ++start;

  std::size_t end = std::min(text.size(), start + kSnippetWindow);
  while (end > start && end < text.size() && is_continuation_byte(text[end])) --end;
  return {start, end};
}

// Copies the window with whitespace collapsed. `marks` are the query words inside the window,
// in order; their new positions become the highlights.
Snippet copy_window(std::string_view text, const Window& window, std::span<const TokenSpan> marks) {
  Snippet snippet;
  bool space_pending = false;
  const auto write_pending_space = [&] {
    if (space_pending && !snippet.text.empty()) snippet.text += ' ';
    space_pending = false;
  };

  std::size_t next_mark = 0;
  for (std::size_t i = window.start; i < window.end;) {
    if (next_mark < marks.size() && marks[next_mark].offset == i) {
      write_pending_space();
      snippet.highlights.push_back({snippet.text.size(), marks[next_mark].length});
      snippet.text.append(text.substr(i, marks[next_mark].length));
      i += marks[next_mark].length;
      ++next_mark;
    } else if (is_space(text[i])) {
      space_pending = true;
      ++i;
    } else {
      write_pending_space();
      snippet.text += text[i];
      ++i;
    }
  }
  return snippet;
}

}  // namespace

Snippet make_snippet(std::string_view doc_text, std::span<const std::string> terms) {
  const std::vector<TokenSpan> words = tokenize_spans(doc_text);
  const auto is_match = [&](const TokenSpan& word) {
    return is_query_word(doc_text.substr(word.offset, word.length), terms);
  };
  const auto first_match = std::find_if(words.begin(), words.end(), is_match);

  const Window window = choose_window(
      doc_text,
      first_match == words.end() ? std::nullopt : std::optional<std::size_t>(first_match->offset));

  // Only words that fit completely inside the window are highlighted, so a word that the window
  // cuts in half is never mistaken for a different word.
  std::vector<TokenSpan> marks;
  for (auto word = first_match; word != words.end() && word->offset < window.end; ++word) {
    const bool inside = word->offset >= window.start && word->offset + word->length <= window.end;
    if (inside && is_match(*word)) marks.push_back(*word);
  }
  Snippet snippet = copy_window(doc_text, window, marks);
  snippet.cut_before = window.start > 0;
  snippet.cut_after = window.end < doc_text.size();
  return snippet;
}

std::expected<Snippet, std::string> snippet_from_file(const std::filesystem::path& part_file,
                                                      const DocRef& ref,
                                                      std::span<const std::string> terms) {
  auto line = read_range(part_file, ref.offset, ref.length);
  if (!line) return std::unexpected(line.error());
  return make_snippet(split_line(*line).text, terms);
}

}  // namespace mse
