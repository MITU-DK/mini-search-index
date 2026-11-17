// The msearch command line: argument parsing, result formatting and the commands.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"
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

// Joins hits with their titles and snippets. `response.terms` are what get highlighted. Snippets
// are read from the reader's corpus root, or from `corpus_root_override` if that is not empty.
template <IndexReader R>
std::vector<ResultLine> make_result_lines(const R& reader, const SearchResponse& response,
                                          std::string_view corpus_root_override = {}) {
  std::vector<ResultLine> lines;
  lines.reserve(response.hits.size());
  for (const SearchResult& hit : response.hits) {
    std::optional<Snippet> snippet;
    if (auto made = snippet_for(reader, hit.doc, response.terms, corpus_root_override))
      snippet = std::move(*made);
    lines.push_back({std::string(reader.doc_title(hit.doc)), hit.score, std::move(snippet)});
  }
  return lines;
}

// The text printed for a search. With `color`, highlighted words are bold (ANSI escapes).
std::string format_results(std::span<const ResultLine> lines, double took_ms, bool color);

struct QueryArgs {
  std::filesystem::path corpus;  // build in memory from this; with `index`, snippets come from here
  std::filesystem::path index;   // a saved index to open instead of building one
  LoadMode load = LoadMode::Mmap;
  unsigned threads = 1;
  RankMode mode = RankMode::BM25;
  std::size_t k = kDefaultTopK;
  bool count_only = false;  // print the document frequency of the one query word and nothing else
  std::string text;
};

// Parses what follows `msearch query` (or `msearch search`, with text_required = false). Words that
// are not flags are joined with spaces into the query text, and everything after "--" is query
// text. Either `-i <index>` or `--corpus <dir>` is needed; `search` needs `-i` and takes no text.
[[nodiscard]] std::expected<QueryArgs, std::string> parse_query_args(
    std::span<const std::string_view> args, bool text_required = true);

struct IndexArgs {
  std::filesystem::path corpus;
  std::filesystem::path out;
  unsigned threads = 1;
  bool compress = true;
  uint64_t memory_budget_bytes = 0;  // 0: build in memory; otherwise spill sorted runs to disk
  std::filesystem::path tmp_dir;     // where the runs go; empty: next to the output file
};

// Parses what follows `msearch index`: one corpus directory and `-o <file>`, in any order.
[[nodiscard]] std::expected<IndexArgs, std::string> parse_index_args(
    std::span<const std::string_view> args);

struct ServeArgs {
  std::filesystem::path index;
  std::filesystem::path web_dir = "web";
  std::filesystem::path
      corpus;  // read previews from here instead of the folder stored in the index
  std::string host = "127.0.0.1";
  int port = 8080;  // 0 picks a free port
  unsigned threads = 1;
};

// Parses what follows `msearch serve`: `-i <index>` is required. --threads defaults to `auto`.
[[nodiscard]] std::expected<ServeArgs, std::string> parse_serve_args(
    std::span<const std::string_view> args);

// Runs `msearch <args>`: results go to `out`, progress and errors to `err`, and `search` reads its
// queries from `in`. Returns the process exit code: 0 on success, 1 on any error.
int run_cli(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
            std::ostream& err, bool color);

// The same with standard input.
int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
            bool color);

}  // namespace mse
