// The msearch command line: argument parsing, result formatting and the commands.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ranker.hpp"
#include "reader.hpp"
#include "searcher.hpp"
#include "snippet.hpp"

namespace mse {

// One hit as it is shown to the user. A missing snippet means the doc file could not be read.
struct ResultLine {
  std::string title;
  float score;
  std::optional<Snippet> snippet;
};

// Joins hits with their titles and snippets. `response.terms` are what get highlighted.
template <IndexReader R>
std::vector<ResultLine> make_result_lines(const R& reader, const SearchResponse& response) {
  std::vector<ResultLine> lines;
  lines.reserve(response.hits.size());
  for (const SearchResult& hit : response.hits) {
    std::optional<Snippet> snippet;
    if (auto made = snippet_for(reader, hit.doc, response.terms)) snippet = std::move(*made);
    lines.push_back({std::string(reader.doc_title(hit.doc)), hit.score, std::move(snippet)});
  }
  return lines;
}

// The text printed for a search. With `color`, highlighted words are bold (ANSI escapes).
std::string format_results(std::span<const ResultLine> lines, double took_ms, bool color);

struct QueryArgs {
  std::filesystem::path corpus;
  unsigned threads = 1;
  RankMode mode = RankMode::BM25;
  std::size_t k = kDefaultTopK;
  std::string text;
};

// Parses what follows `msearch query`. Words that are not flags are joined with spaces into the
// query text, and everything after "--" is query text.
[[nodiscard]] std::expected<QueryArgs, std::string> parse_query_args(
    std::span<const std::string_view> args);

// Runs `msearch <args>`: results go to `out`, progress and errors to `err`.
// Returns the process exit code: 0 on success, 1 on any error.
int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
            bool color);

}  // namespace mse
