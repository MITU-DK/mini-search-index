// The HTTP server: a JSON search API and the web page, over a shared read-only index.
//
//   GET /api/search?q=<text>&k=<1..100>&rank=<bm25|tfidf>   ranked hits, with previews
//   GET /api/suggest?prefix=<text>                          up to 5 completions
//   GET /                                                   web/index.html
//
// The index view is immutable, so every worker thread can read it at once. Each thread has its own
// Searcher, because a Searcher owns scratch memory that a search writes to.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

#include "index_view.hpp"
#include "ranker.hpp"
#include "searcher.hpp"

namespace mse {

inline constexpr std::size_t kMaxHttpK = 100;        // the most hits one request may ask for
inline constexpr std::size_t kMaxQueryBytes = 1024;  // the longest query or prefix accepted

// The answer to one search, as in DESIGN.md §7:
//   {"query", "corrected_query": null|"...", "took_ms",
//    "hits": [{"doc", "title", "score", "snippet": null|"...", "highlights": [[offset, length]]}]}
// A highlight is a byte range of the UTF-8 `snippet`, not a character range. `snippet` is null
// when the doc's file cannot be read (and `highlights` is then empty). `took_ms` covers the
// search and the previews. Snippets are read from the index's corpus folder, or from
// `corpus_root_override` if that is not empty.
nlohmann::json search_to_json(const IndexView& view, Searcher<IndexView>& searcher,
                              std::string_view query, std::size_t k, RankMode mode,
                              std::string_view corpus_root_override);

// An array of up to kSuggestTopK completions of `prefix`.
nlohmann::json suggest_to_json(const IndexView& view, std::string_view prefix);

// The JSON text. Bytes that are not valid UTF-8 (a query typed wrongly, or an odd byte in the
// corpus) become U+FFFD instead of making the dump throw.
std::string dump_json(const nlohmann::json& value);

struct ServerConfig {
  unsigned threads = 1;                   // worker threads answering requests (at least 1)
  std::filesystem::path web_dir = "web";  // the folder with index.html
  std::filesystem::path corpus_root;      // where previews are read from; empty: as stored
  std::string host = "127.0.0.1";         // the address to listen on; only this machine by default
};

class SearchServer {
 public:
  // `view` must outlive the server.
  SearchServer(const IndexView& view, ServerConfig config);
  ~SearchServer();
  SearchServer(const SearchServer&) = delete;
  SearchServer& operator=(const SearchServer&) = delete;

  // Starts listening on `port` (0 picks a free one) and returns the port.
  [[nodiscard]] std::expected<int, std::string> bind(int port);
  // Answers requests until stop() is called. Call bind() first.
  void run();
  // Waits until run() is accepting connections.
  void wait_until_ready() const;
  // Makes run() return. Safe to call from another thread, and more than once.
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mse
