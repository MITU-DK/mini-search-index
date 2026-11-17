#include "server.hpp"

#include <httplib.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

#include "autocomplete.hpp"
#include "snippet.hpp"

namespace mse {

using nlohmann::json;

namespace {

double round_to(double value, double scale) { return std::round(value * scale) / scale; }

}  // namespace

json search_to_json(const IndexView& view, Searcher<IndexView>& searcher, std::string_view query,
                    std::size_t k, RankMode mode, std::string_view corpus_root_override) {
  const auto start = std::chrono::steady_clock::now();
  const SearchResponse response = searcher.search(query, {.k = k, .mode = mode});

  json hits = json::array();
  for (const SearchResult& hit : response.hits) {
    json item = {{"doc", hit.doc},
                 {"title", std::string(view.doc_title(hit.doc))},
                 {"score", round_to(hit.score, 1e4)},
                 {"snippet", nullptr},
                 {"highlights", json::array()}};
    if (auto snippet = snippet_for(view, hit.doc, response.terms, corpus_root_override)) {
      item["snippet"] = snippet->text;
      for (const Highlight& highlight : snippet->highlights) {
        item["highlights"].push_back({highlight.offset, highlight.length});
      }
      item["cut_before"] = snippet->cut_before;
      item["cut_after"] = snippet->cut_after;
    }
    hits.push_back(std::move(item));
  }

  const double took_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return {{"query", std::string(query)},
          {"corrected_query", response.corrected_query ? json(*response.corrected_query) : json()},
          {"took_ms", round_to(took_ms, 1e3)},
          {"hits", std::move(hits)}};
}

json suggest_to_json(const IndexView& view, std::string_view prefix) {
  return json(suggest(view, prefix));
}

std::string dump_json(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---- the server ----

namespace {

std::atomic<uint64_t> next_server_id{1};

// A whole decimal number in [1, kMaxHttpK], or nullopt.
std::optional<std::size_t> parse_k(const std::string& text) {
  std::size_t value = 0;
  const char* const end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || error != std::errc() || stop != end || value < 1 || value > kMaxHttpK) {
    return std::nullopt;
  }
  return value;
}

void send_json(httplib::Response& response, int status, const json& body) {
  response.status = status;
  response.set_content(dump_json(body), "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
}

void send_error(httplib::Response& response, int status, std::string_view message) {
  send_json(response, status, {{"error", std::string(message)}});
}

}  // namespace

struct SearchServer::Impl {
  Impl(const IndexView& v, ServerConfig c)
      : view(&v), config(std::move(c)), id(next_server_id.fetch_add(1)) {
    config.threads = std::max(1u, config.threads);
    install_routes();
  }

  // The calling thread's own Searcher for this server. It is made on first use, so every worker
  // gets one, and a thread that served another server before gets a fresh one.
  Searcher<IndexView>& searcher() const {
    thread_local std::unique_ptr<Searcher<IndexView>> mine;
    thread_local uint64_t owner = 0;
    if (!mine || owner != id) {
      mine = std::make_unique<Searcher<IndexView>>(*view);
      owner = id;
    }
    return *mine;
  }

  void install_routes() {
    server.set_default_headers({{"X-Content-Type-Options", "nosniff"}});
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);
    server.set_keep_alive_max_count(100);
    // cpp-httplib's default also sets SO_REUSEPORT, which lets a second server bind the same port
    // and share the connections with the first, silently. Only SO_REUSEADDR (restart right after
    // stopping) is wanted: a port that is in use must be an error.
    server.set_socket_options([](socket_t socket) {
      const int yes = 1;
      ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                   sizeof(yes));
    });
    server.set_payload_max_length(8 * 1024);
    const unsigned threads = config.threads;
    server.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };

    server.Get("/api/search", [this](const httplib::Request& req, httplib::Response& res) {
      handle_search(req, res);
    });
    server.Get("/api/suggest", [this](const httplib::Request& req, httplib::Response& res) {
      handle_suggest(req, res);
    });
    server.Get("/", [this](const httplib::Request&, httplib::Response& res) { handle_page(res); });

    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
      if (res.body.empty()) send_error(res, res.status, res.status == 404 ? "not found" : "error");
    });
    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
          send_error(res, 500, "internal error");
        });
  }

  void handle_search(const httplib::Request& req, httplib::Response& res) const {
    if (!req.has_param("q")) return send_error(res, 400, "missing parameter q");
    const std::string query = req.get_param_value("q");
    if (query.size() > kMaxQueryBytes) {
      return send_error(res, 400, "q is longer than " + std::to_string(kMaxQueryBytes) + " bytes");
    }

    std::size_t k = kDefaultTopK;
    if (req.has_param("k")) {
      const auto parsed = parse_k(req.get_param_value("k"));
      if (!parsed) {
        return send_error(res, 400,
                          "k must be a whole number from 1 to " + std::to_string(kMaxHttpK));
      }
      k = *parsed;
    }
    RankMode mode = RankMode::BM25;
    if (req.has_param("rank")) {
      const std::string rank = req.get_param_value("rank");
      if (rank == "tfidf") {
        mode = RankMode::TFIDF;
      } else if (rank != "bm25") {
        return send_error(res, 400, "rank must be bm25 or tfidf");
      }
    }
    send_json(res, 200,
              search_to_json(*view, searcher(), query, k, mode, config.corpus_root.string()));
  }

  void handle_suggest(const httplib::Request& req, httplib::Response& res) const {
    if (!req.has_param("prefix")) return send_error(res, 400, "missing parameter prefix");
    const std::string prefix = req.get_param_value("prefix");
    if (prefix.size() > kMaxQueryBytes) {
      return send_error(res, 400,
                        "prefix is longer than " + std::to_string(kMaxQueryBytes) + " bytes");
    }
    send_json(res, 200, suggest_to_json(*view, prefix));
  }

  // Only this one file is ever served, so there is no path that could point outside the folder.
  void handle_page(httplib::Response& res) const {
    std::ifstream in(config.web_dir / "index.html", std::ios::binary);
    if (!in)
      return send_error(res, 404,
                        "web page not found: " + (config.web_dir / "index.html").string());
    std::ostringstream page;
    page << in.rdbuf();
    res.set_content(page.str(), "text/html; charset=utf-8");
    // The page's own script and style are inline; nothing is loaded from anywhere else.
    res.set_header(
        "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  }

  const IndexView* view;
  ServerConfig config;
  uint64_t id;
  httplib::Server server;
};

SearchServer::SearchServer(const IndexView& view, ServerConfig config)
    : impl_(std::make_unique<Impl>(view, std::move(config))) {}

SearchServer::~SearchServer() { stop(); }

std::expected<int, std::string> SearchServer::bind(int port) {
  const std::string& host = impl_->config.host;
  const int bound = port == 0 ? impl_->server.bind_to_any_port(host)
                              : (impl_->server.bind_to_port(host, port) ? port : -1);
  if (bound < 0) {
    return std::unexpected("cannot listen on " + host + ":" + std::to_string(port) +
                           " (is the port already in use?)");
  }
  return bound;
}

void SearchServer::run() { impl_->server.listen_after_bind(); }

void SearchServer::wait_until_ready() const { impl_->server.wait_until_ready(); }

void SearchServer::stop() { impl_->server.stop(); }

}  // namespace mse
