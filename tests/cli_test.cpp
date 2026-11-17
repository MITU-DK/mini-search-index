#include "cli.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "index_writer.hpp"
#include "indexer.hpp"
#include "parsed_loader.hpp"
#include "temp_dir.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

using Args = std::vector<std::string_view>;

// ---- format_results ----

TEST(Cli, FormatResultsNoColor) {
  const std::vector<ResultLine> lines = {
      {"Beta", 1.2345f,
       Snippet{"machine learning is learning from data", {{0, 7}, {8, 8}}, true, false}}};
  const std::string expected =
      "Top results (found in 0.42 ms):\n"
      " 1. Beta" +
      std::string(32, ' ') +
      "score: 1.2345\n"
      "    ... machine learning is learning from data\n";
  EXPECT_EQ(format_results(lines, 0.42, false), expected);
}

TEST(Cli, FormatResultsColorMarksHighlightsInBold) {
  const std::vector<ResultLine> lines = {{"A", 1.0f, Snippet{"a fox b", {{2, 3}}, false, false}}};
  const std::string out = format_results(lines, 1.0, true);
  EXPECT_NE(out.find("    a \x1b[1mfox\x1b[0m b\n"), std::string::npos) << out;
}

TEST(Cli, FormatResultsEllipsisOnlyWhereTextWasCut) {
  const auto snippet_line = [](bool before, bool after) {
    const std::vector<ResultLine> lines = {{"A", 1.0f, Snippet{"x", {}, before, after}}};
    return format_results(lines, 1.0, false);
  };
  EXPECT_NE(snippet_line(false, false).find("\n    x\n"), std::string::npos);
  EXPECT_NE(snippet_line(true, false).find("\n    ... x\n"), std::string::npos);
  EXPECT_NE(snippet_line(false, true).find("\n    x ...\n"), std::string::npos);
  EXPECT_NE(snippet_line(true, true).find("\n    ... x ...\n"), std::string::npos);
}

TEST(Cli, FormatResultsUnavailableAndEmptySnippets) {
  const std::vector<ResultLine> lines = {{"Gone", 2.0f, std::nullopt},
                                         {"Bare", 1.0f, Snippet{"", {}, false, false}}};
  const std::string expected = "Top results (found in 1.00 ms):\n 1. Gone" + std::string(32, ' ') +
                               "score: 2.0000\n    (document unavailable)\n 2. Bare" +
                               std::string(32, ' ') + "score: 1.0000\n";
  EXPECT_EQ(format_results(lines, 1.0, false), expected);
}

TEST(Cli, FormatResultsNoHits) {
  EXPECT_EQ(format_results({}, 0.42, false), "No results (found in 0.42 ms).\n");
}

TEST(Cli, FormatResultsAlignsScoresByCharactersNotBytes) {
  // "東京" is 2 characters (6 bytes), so it gets 34 spaces. A 40-character title gets 2.
  const std::string long_title(40, 'x');
  const std::vector<ResultLine> lines = {{"東京", 1.0f, Snippet{}}, {long_title, 1.0f, Snippet{}}};
  const std::string out = format_results(lines, 1.0, false);
  EXPECT_NE(out.find(" 1. 東京" + std::string(34, ' ') + "score: 1.0000\n"), std::string::npos);
  EXPECT_NE(out.find(" 2. " + long_title + "  score: 1.0000\n"), std::string::npos);
}

TEST(Cli, FormatResultsNumbersTenResultsInTwoColumns) {
  std::vector<ResultLine> lines;
  for (int i = 0; i < 10; ++i) lines.push_back({"t", 1.0f, Snippet{}});
  const std::string out = format_results(lines, 1.0, false);
  EXPECT_NE(out.find("\n 9. t"), std::string::npos);
  EXPECT_NE(out.find("\n10. t"), std::string::npos);
}

// ---- parse_query_args ----

TEST(Cli, ParseQueryArgsFull) {
  const Args args = {"--corpus", "data/dev", "--threads",       "8", "--rank", "tfidf",
                     "-k",       "5",        "machine learning"};
  auto parsed = parse_query_args(args);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->corpus, "data/dev");
  EXPECT_EQ(parsed->threads, 8u);
  EXPECT_EQ(parsed->mode, RankMode::TFIDF);
  EXPECT_EQ(parsed->k, 5u);
  EXPECT_EQ(parsed->text, "machine learning");
}

TEST(Cli, ParseQueryArgsDefaults) {
  auto parsed = parse_query_args(Args{"--corpus", "d", "fox"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->threads, 1u);
  EXPECT_EQ(parsed->mode, RankMode::BM25);
  EXPECT_EQ(parsed->k, kDefaultTopK);
}

TEST(Cli, ParseQueryArgsAutoThreads) {
  auto parsed = parse_query_args(Args{"--corpus", "d", "--threads", "auto", "fox"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_GE(parsed->threads, 1u);
}

TEST(Cli, ParseQueryArgsJoinsSeveralWordsIntoTheQuery) {
  auto parsed = parse_query_args(Args{"--corpus", "d", "machine", "learning"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->text, "machine learning");
}

TEST(Cli, ParseQueryArgsDoubleDashEndsTheFlags) {
  auto parsed = parse_query_args(Args{"--corpus", "d", "--", "-k", "--rank"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->text, "-k --rank");
}

TEST(Cli, ParseQueryArgsAnEmptyQueryIsAllowed) {
  auto parsed = parse_query_args(Args{"--corpus", "d", ""});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->text, "");
}

TEST(Cli, ParseQueryArgsErrors) {
  const std::vector<Args> bad = {
      {"fox"},                                      // no --corpus
      {"--corpus", "d"},                            // no query text
      {"--corpus"},                                 // flag without a value
      {"--corpus", "d", "--nope", "x", "fox"},      // unknown flag
      {"--corpus", "d", "--rank", "magic", "fox"},  // unknown rank mode
      {"--corpus", "d", "-k", "0", "fox"},          // k must be at least 1
      {"--corpus", "d", "-k", "ten", "fox"},        // k is not a number
      {"--corpus", "d", "-k", "5x", "fox"},         // trailing junk
      {"--corpus", "d", "--threads", "0", "fox"},   // bad thread count
      {"--corpus", "d", "--threads", "many", "fox"},
  };
  for (const Args& args : bad) {
    EXPECT_FALSE(parse_query_args(args).has_value()) << args.size() << " args, first: " << args[0];
  }
}

// ---- run_cli (end to end on the fixture corpus) ----

struct CliRun {
  int code;
  std::string out;
  std::string err;
};

CliRun run(const Args& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = run_cli(args, out, err, false);
  return {code, out.str(), err.str()};
}

TEST(Cli, QueryCommandPrintsRankedResultsWithSnippets) {
  const std::string corpus = tiny_corpus_dir().string();
  const CliRun result = run({"query", "--corpus", corpus, "machine learning"});
  EXPECT_EQ(result.code, 0) << result.err;
  EXPECT_NE(result.out.find("Top results (found in "), std::string::npos) << result.out;
  const auto beta = result.out.find(" 1. Beta");
  const auto gamma = result.out.find(" 2. Gamma");
  EXPECT_NE(beta, std::string::npos) << result.out;
  EXPECT_NE(gamma, std::string::npos) << result.out;
  EXPECT_LT(beta, gamma);
  EXPECT_NE(result.out.find("\n    machine learning is learning from data\n"), std::string::npos)
      << result.out;
  EXPECT_NE(result.err.find("Indexed 6 docs"), std::string::npos) << result.err;
}

TEST(Cli, QueryCommandWithNoHits) {
  const CliRun result = run({"query", "--corpus", tiny_corpus_dir().string(), "zzzz"});
  EXPECT_EQ(result.code, 0) << result.err;
  EXPECT_NE(result.out.find("No results"), std::string::npos) << result.out;
}

TEST(Cli, QueryCommandRespectsKAndRank) {
  const CliRun result = run({"query", "--corpus", tiny_corpus_dir().string(), "-k", "1", "--rank",
                             "tfidf", "machine learning"});
  EXPECT_EQ(result.code, 0) << result.err;
  EXPECT_NE(result.out.find(" 1. Beta"), std::string::npos) << result.out;
  EXPECT_EQ(result.out.find(" 2. "), std::string::npos) << result.out;
}

TEST(Cli, QueryCommandOnAMissingCorpusFails) {
  const CliRun result = run({"query", "--corpus", "/nonexistent/mse", "fox"});
  EXPECT_EQ(result.code, 1);
  EXPECT_NE(result.err.find("error:"), std::string::npos) << result.err;
  EXPECT_TRUE(result.out.empty());
}

TEST(Cli, BadUsageFailsWithUsageOnStderr) {
  for (const Args& args :
       std::vector<Args>{{}, {"frobnicate"}, {"query"}, {"query", "--bad", "x"}}) {
    const CliRun result = run(args);
    EXPECT_EQ(result.code, 1);
    EXPECT_NE(result.err.find("Usage:"), std::string::npos) << result.err;
    EXPECT_TRUE(result.out.empty());
  }
}

TEST(Cli, HelpPrintsUsageAndSucceeds) {
  for (const char* flag : {"--help", "-h", "help"}) {
    const CliRun result = run({flag});
    EXPECT_EQ(result.code, 0);
    EXPECT_NE(result.out.find("Usage:"), std::string::npos);
  }
}

// ---- parse_index_args ----

TEST(Cli, ParseIndexArgsFull) {
  auto parsed =
      parse_index_args(Args{"data/dev", "-o", "idx.bin", "--threads", "8", "--no-compress"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->corpus, "data/dev");
  EXPECT_EQ(parsed->out, "idx.bin");
  EXPECT_EQ(parsed->threads, 8u);
  EXPECT_FALSE(parsed->compress);
}

TEST(Cli, ParseIndexArgsDefaults) {
  auto parsed = parse_index_args(Args{"-o", "idx.bin", "data/dev"});  // flags can come first
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->threads, 1u);
  EXPECT_TRUE(parsed->compress);
}

TEST(Cli, ParseIndexArgsErrors) {
  const std::vector<Args> bad = {
      {"-o", "idx.bin"},                  // no corpus
      {"data/dev"},                       // no output file
      {"a", "b", "-o", "idx.bin"},        // two corpora
      {"data/dev", "-o"},                 // -o without a value
      {"data/dev", "-o", "i", "--nope"},  // unknown flag
      {"data/dev", "-o", "i", "--threads", "0"},
      {"data/dev", "-o", "i", "--threads"},
  };
  for (const Args& args : bad) {
    EXPECT_FALSE(parse_index_args(args).has_value()) << args.size() << " args, first: " << args[0];
  }
}

// ---- the index command ----

TEST(Cli, IndexCommandWritesAnIndexAndReportsIt) {
  TempDir dir;
  const std::string out_file = (dir.path() / "i.bin").string();
  const CliRun result = run({"index", tiny_corpus_dir().string(), "-o", out_file});
  EXPECT_EQ(result.code, 0) << result.err;
  EXPECT_NE(result.out.find("docs:      6"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("terms:     27"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("postings:  29"), std::string::npos) << result.out;
  for (const char* phase : {"scan:", "tokenize:", "merge:", "write:", "file:"}) {
    EXPECT_NE(result.out.find(phase), std::string::npos) << phase;
  }

  const auto loaded = load_parsed(out_file);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->docs.size(), 6u);
  EXPECT_EQ(loaded->postings.size(), 27u);
}

TEST(Cli, IndexCommandNoCompressWritesALargerFile) {
  TempDir dir;
  const std::string corpus = tiny_corpus_dir().string();
  ASSERT_EQ(run({"index", corpus, "-o", (dir.path() / "c.bin").string()}).code, 0);
  ASSERT_EQ(run({"index", corpus, "-o", (dir.path() / "r.bin").string(), "--no-compress"}).code, 0);
  EXPECT_LT(std::filesystem::file_size(dir.path() / "c.bin"),
            std::filesystem::file_size(dir.path() / "r.bin"));
}

TEST(Cli, IndexCommandFailsCleanly) {
  TempDir dir;
  const CliRun missing_corpus =
      run({"index", "/nonexistent/mse", "-o", (dir.path() / "i.bin").string()});
  EXPECT_EQ(missing_corpus.code, 1);
  EXPECT_NE(missing_corpus.err.find("error:"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "i.bin"));

  const CliRun missing_dir =
      run({"index", tiny_corpus_dir().string(), "-o", (dir.path() / "no" / "i.bin").string()});
  EXPECT_EQ(missing_dir.code, 1);
  EXPECT_NE(missing_dir.err.find("error:"), std::string::npos);

  const CliRun bad_usage = run({"index", tiny_corpus_dir().string()});
  EXPECT_EQ(bad_usage.code, 1);
  EXPECT_NE(bad_usage.err.find("Usage:"), std::string::npos);
}

TEST(Cli, UsageMentionsBothCommands) {
  const CliRun result = run({"--help"});
  EXPECT_NE(result.out.find("msearch query"), std::string::npos);
  EXPECT_NE(result.out.find("msearch index"), std::string::npos);
}

}  // namespace
}  // namespace mse
