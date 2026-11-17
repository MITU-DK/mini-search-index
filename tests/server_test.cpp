#include "server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

#include "saved_index.hpp"
#include "synthetic_corpus.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using nlohmann::json;

std::string percent_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) != 0) {
      out += c;
    } else {
      out += '%';
      out += kHex[byte >> 4];
      out += kHex[byte & 15];
    }
  }
  return out;
}

ServerConfig config_with_web(const std::filesystem::path& web) {
  ServerConfig config;
  config.web_dir = web;
  return config;
}

ServerConfig config_with_threads(unsigned threads) {
  ServerConfig config;
  config.threads = threads;
  return config;
}

// Runs a server over `view` on a free port for as long as the object lives.
class RunningServer {
 public:
  explicit RunningServer(const IndexView& view, ServerConfig config = {}) : server_(view, config) {
    const auto port = server_.bind(0);
    if (!port) throw std::runtime_error(port.error());
    port_ = *port;
    thread_ = std::jthread([this] { server_.run(); });
    server_.wait_until_ready();
  }
  ~RunningServer() {
    server_.stop();
    thread_.join();
  }
  httplib::Client client() const { return httplib::Client("127.0.0.1", port_); }
  int port() const { return port_; }

 private:
  SearchServer server_;
  int port_ = 0;
  std::jthread thread_;
};

std::string search_path(std::string_view query, std::string_view extra = {}) {
  return "/api/search?q=" + percent_encode(query) + std::string(extra);
}

// ---- the JSON, without a socket ----

TEST(ServerJson, JsonShape) {
  const auto saved = save_index(tiny_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const json out =
      search_to_json(*saved->view, searcher, "machine learning", 10, RankMode::BM25, {});
  EXPECT_EQ(out.at("query"), "machine learning");
  EXPECT_TRUE(out.at("corrected_query").is_null());
  EXPECT_TRUE(out.at("took_ms").is_number());
  EXPECT_GE(out.at("took_ms").get<double>(), 0.0);
  const json& hits = out.at("hits");
  ASSERT_TRUE(hits.is_array());
  ASSERT_FALSE(hits.empty());
  for (const json& hit : hits) {
    EXPECT_TRUE(hit.at("doc").is_number_unsigned());
    EXPECT_TRUE(hit.at("title").is_string());
    EXPECT_TRUE(hit.at("score").is_number());
    ASSERT_TRUE(hit.at("snippet").is_string());
    ASSERT_TRUE(hit.at("highlights").is_array());
    const std::size_t length = hit.at("snippet").get<std::string>().size();
    for (const json& range : hit.at("highlights")) {
      ASSERT_EQ(range.size(), 2u);  // [offset, length], in bytes of the UTF-8 snippet
      EXPECT_LE(range[0].get<std::size_t>() + range[1].get<std::size_t>(), length);
      EXPECT_GT(range[1].get<std::size_t>(), 0u);
    }
  }
  // the best hit for this query is the doc titled Beta (see the command-line tests)
  EXPECT_EQ(hits[0].at("title"), "Beta");
  EXPECT_FALSE(hits[0].at("highlights").empty());
}

TEST(ServerJson, NoHitsGivesAnEmptyArray) {
  const auto saved = save_index(tiny_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const json out = search_to_json(*saved->view, searcher, "zzzzqqqq", 10, RankMode::BM25, {});
  EXPECT_TRUE(out.at("hits").is_array());
  EXPECT_TRUE(out.at("hits").empty());
  const json blank = search_to_json(*saved->view, searcher, "   ", 10, RankMode::BM25, {});
  EXPECT_TRUE(blank.at("hits").empty());
  EXPECT_TRUE(blank.at("corrected_query").is_null());
}

TEST(ServerJson, KLimitsTheHits) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 500, 50, 1, 61);
  const auto saved = save_index(corpus.path());
  Searcher<IndexView> searcher(*saved->view);
  EXPECT_EQ(
      search_to_json(*saved->view, searcher, "w1 w2", 3, RankMode::BM25, {}).at("hits").size(), 3u);
}

TEST(ServerJson, TfIdfAndBm25CanBeChosen) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 1000, 100, 1, 62);
  const auto saved = save_index(corpus.path());
  Searcher<IndexView> searcher(*saved->view);
  const json bm25 = search_to_json(*saved->view, searcher, "w3 w7 w20", 10, RankMode::BM25, {});
  const json tfidf = search_to_json(*saved->view, searcher, "w3 w7 w20", 10, RankMode::TFIDF, {});
  EXPECT_NE(bm25.at("hits"), tfidf.at("hits"));  // different scores at least
}

TEST(ServerJson, JsonEscapesUtf8AndQuotes) {
  TempDir corpus;
  write_file(corpus.path() / "part-00000.tsv",
             "Say \"hi\" \\ caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC\tthe <b>bold</b> & \"quoted\" "
             "caf\xC3\xA9 "
             "\xE6\x9D\xB1\xE4\xBA\xAC text\n");
  const auto saved = save_index(corpus.path());
  Searcher<IndexView> searcher(*saved->view);
  const json out =
      search_to_json(*saved->view, searcher, "caf\xC3\xA9 \"quoted\"", 10, RankMode::BM25, {});
  const std::string text = dump_json(out);

  const json parsed = json::parse(text);  // it is valid JSON, and it says what we put in
  EXPECT_EQ(parsed.at("query"), "caf\xC3\xA9 \"quoted\"");
  ASSERT_EQ(parsed.at("hits").size(), 1u);
  EXPECT_EQ(parsed.at("hits")[0].at("title"), "Say \"hi\" \\ caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC");
  const std::string snippet = parsed.at("hits")[0].at("snippet");
  EXPECT_NE(snippet.find("<b>bold</b> & \"quoted\""), std::string::npos) << snippet;
  // quotes and backslashes are escaped on the wire; the UTF-8 is sent as UTF-8, not as \u escapes
  EXPECT_NE(text.find("\\\"hi\\\""), std::string::npos) << text;
  EXPECT_NE(text.find("\\\\"), std::string::npos) << text;
  EXPECT_NE(text.find("caf\xC3\xA9"), std::string::npos) << text;
  for (const char c : text) EXPECT_GE(static_cast<unsigned char>(c), 0x20) << "a raw control byte";
}

TEST(ServerJson, InvalidUtf8NeverBreaksTheResponse) {
  TempDir corpus;
  write_file(corpus.path() / "part-00000.tsv",
             "Bad \xFF title\ttext with \xC3 a broken byte and word\n");
  const auto saved = save_index(corpus.path());
  Searcher<IndexView> searcher(*saved->view);
  const json out = search_to_json(*saved->view, searcher, "word \xFF\xFE", 10, RankMode::BM25, {});
  std::string text;
  ASSERT_NO_THROW(text = dump_json(out));
  json reparsed;
  EXPECT_NO_THROW(reparsed = json::parse(text));  // bad bytes became U+FFFD
  EXPECT_TRUE(reparsed.is_object());
}

TEST(ServerJson, CorrectedQueryIsReported) {
  const auto saved = save_index(spell_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const json out = search_to_json(*saved->view, searcher, "machne lerning", 10, RankMode::BM25, {});
  EXPECT_EQ(out.at("query"), "machne lerning");
  EXPECT_EQ(out.at("corrected_query"), "machine learning");
  EXPECT_FALSE(out.at("hits").empty());
}

TEST(ServerJson, SnippetIsNullWhenTheDocFileIsGone) {
  const auto saved = save_index(tiny_corpus_dir());
  Searcher<IndexView> searcher(*saved->view);
  const json out = search_to_json(*saved->view, searcher, "machine learning", 10, RankMode::BM25,
                                  "/nonexistent/mse");
  ASSERT_FALSE(out.at("hits").empty());
  EXPECT_TRUE(out.at("hits")[0].at("snippet").is_null());
  EXPECT_TRUE(out.at("hits")[0].at("highlights").empty());
}

TEST(ServerJson, SuggestJson) {
  const auto saved = save_index(tiny_corpus_dir());
  EXPECT_EQ(suggest_to_json(*saved->view, "la"), json::parse(R"(["lait","lazy"])"));
  EXPECT_EQ(suggest_to_json(*saved->view, "l"), json::array());
  EXPECT_EQ(suggest_to_json(*saved->view, "zz"), json::array());
}

// ---- over HTTP ----

TEST(Server, SearchOverHttp) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer server(*saved->view);
  auto client = server.client();
  const auto response = client.Get(search_path("machine learning", "&k=2&rank=tfidf"));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->get_header_value("Content-Type"), "application/json; charset=utf-8");
  const json body = json::parse(response->body);
  EXPECT_EQ(body.at("query"), "machine learning");
  EXPECT_LE(body.at("hits").size(), 2u);
  EXPECT_FALSE(body.at("hits").empty());
}

TEST(Server, UnicodeQueriesWork) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer server(*saved->view);
  auto client = server.client();
  const auto response = client.Get(search_path("caf\xC3\xA9"));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  const json body = json::parse(response->body);
  EXPECT_EQ(body.at("query"), "caf\xC3\xA9");
  EXPECT_FALSE(body.at("hits").empty());
}

TEST(Server, SuggestOverHttp) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer server(*saved->view);
  auto client = server.client();
  const auto response = client.Get("/api/suggest?prefix=LA");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(json::parse(response->body), json::parse(R"(["lait","lazy"])"));
  const auto none = client.Get("/api/suggest?prefix=l");
  ASSERT_TRUE(none);
  EXPECT_EQ(json::parse(none->body), json::array());
}

TEST(Server, BadParams400) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer server(*saved->view);
  auto client = server.client();
  for (const std::string& path : {std::string("/api/search"),                     // no q
                                  search_path("fox", "&k=abc"),                   // not a number
                                  search_path("fox", "&k=0"),                     // too small
                                  search_path("fox", "&k=101"),                   // too big
                                  search_path("fox", "&k=-3"),                    // negative
                                  search_path("fox", "&k=99999999999999999999"),  // overflows
                                  search_path("fox", "&k="),                      // empty
                                  search_path("fox", "&rank=pagerank"),           // unknown ranking
                                  search_path(std::string(2000, 'a')),  // far too long a query
                                  std::string("/api/suggest"),          // no prefix
                                  "/api/suggest?prefix=" + std::string(2000, 'a')}) {
    const auto response = client.Get(path);
    ASSERT_TRUE(response) << path.substr(0, 60);
    EXPECT_EQ(response->status, 400) << path.substr(0, 60);
    const json body = json::parse(response->body);
    ASSERT_TRUE(body.at("error").is_string()) << path.substr(0, 60);
    EXPECT_FALSE(body.at("error").get<std::string>().empty());
  }
  // the edges that are allowed
  EXPECT_EQ(client.Get(search_path("fox", "&k=1"))->status, 200);
  EXPECT_EQ(client.Get(search_path("fox", "&k=100"))->status, 200);
  EXPECT_EQ(client.Get(search_path("fox", "&rank=bm25"))->status, 200);
  EXPECT_EQ(client.Get(search_path(""))->status, 200);  // an empty query is not an error
}

TEST(Server, UnknownPathsAreNotFound) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer server(*saved->view);
  auto client = server.client();
  const auto response = client.Get("/api/nothing");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 404);
  EXPECT_EQ(client.Get("/../etc/passwd")->status, 404);
  EXPECT_EQ(client.Post("/api/search", "q=fox", "text/plain")->status, 404);
}

TEST(Server, ServesTheWebPage) {
  const auto saved = save_index(tiny_corpus_dir());
  TempDir web;
  write_file(web.path() / "index.html", "<!doctype html><title>hello</title>");
  write_file(web.path() / "secret.txt", "not for the web");
  RunningServer server(*saved->view, config_with_web(web.path()));
  auto client = server.client();
  const auto page = client.Get("/");
  ASSERT_TRUE(page);
  EXPECT_EQ(page->status, 200);
  EXPECT_EQ(page->body, "<!doctype html><title>hello</title>");
  EXPECT_EQ(page->get_header_value("Content-Type"), "text/html; charset=utf-8");
  EXPECT_EQ(page->get_header_value("X-Content-Type-Options"), "nosniff");
  EXPECT_FALSE(page->get_header_value("Content-Security-Policy").empty());
  EXPECT_EQ(client.Get("/secret.txt")->status, 404);  // only index.html is ever served
}

TEST(Server, MissingWebPageIsNotFoundNotACrash) {
  const auto saved = save_index(tiny_corpus_dir());
  TempDir web;  // empty
  RunningServer server(*saved->view, config_with_web(web.path()));
  auto client = server.client();
  const auto page = client.Get("/");
  ASSERT_TRUE(page);
  EXPECT_EQ(page->status, 404);
}

TEST(Server, TheSecondServerOnTheSamePortFailsCleanly) {
  const auto saved = save_index(tiny_corpus_dir());
  RunningServer first(*saved->view);
  SearchServer second(*saved->view, {});
  const auto port = second.bind(first.port());
  EXPECT_FALSE(port.has_value());
  EXPECT_FALSE(port.error().empty());
}

TEST(Server, StopMakesRunReturn) {
  const auto saved = save_index(tiny_corpus_dir());
  SearchServer server(*saved->view, {});
  ASSERT_TRUE(server.bind(0).has_value());
  std::jthread runner([&] { server.run(); });
  server.wait_until_ready();
  server.stop();
  runner.join();  // would hang if stop() did not work
  SUCCEED();
}

// Many clients at once get exactly what one thread alone would have got. Run under TSan.
TEST(Server, ConcurrentSearches) {
  TempDir corpus;
  write_synthetic_corpus(corpus.path(), 3000, 400, 2, 63);
  const auto saved = save_index(corpus.path());

  std::mt19937 rng(7);
  std::vector<std::string> queries;
  for (int i = 0; i < 30; ++i) {
    queries.push_back("w" + std::to_string(rng() % 80) + " w" + std::to_string(rng() % 400) +
                      (i % 3 == 0 ? " w" + std::to_string(rng() % 450) : ""));
  }
  // what a single thread gets, with the timing taken out
  Searcher<IndexView> searcher(*saved->view);
  std::vector<json> expected;
  for (const std::string& query : queries) {
    json one = search_to_json(*saved->view, searcher, query, 10, RankMode::BM25, {});
    one.erase("took_ms");
    expected.push_back(std::move(one));
  }

  RunningServer server(*saved->view, config_with_threads(4));
  std::atomic<int> wrong{0};
  std::atomic<int> failed{0};
  {
    std::vector<std::jthread> clients;
    for (int c = 0; c < 8; ++c) {
      clients.emplace_back([&, c] {
        auto client = server.client();
        std::mt19937 pick(100 + c);
        for (int i = 0; i < 50; ++i) {
          const std::size_t which = pick() % queries.size();
          const auto response = client.Get(search_path(queries[which]));
          if (!response || response->status != 200) {
            ++failed;
            continue;
          }
          json got = json::parse(response->body);
          got.erase("took_ms");
          if (got != expected[which]) ++wrong;
        }
      });
    }
  }
  EXPECT_EQ(failed.load(), 0);
  EXPECT_EQ(wrong.load(), 0);
}

}  // namespace
}  // namespace mse
