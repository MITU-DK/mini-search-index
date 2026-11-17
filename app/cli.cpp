#include "cli.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <stdexcept>
#include <system_error>

#include "index_writer.hpp"
#include "indexer.hpp"

namespace mse {
namespace {

constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kReset = "\x1b[0m";
constexpr std::size_t kTitleColumns = 36;  // titles are padded to this width before the score
constexpr std::string_view kIndent = "    ";

constexpr std::string_view kUsage =
    "Usage:\n"
    "  msearch index <corpus> -o <index.bin> [--threads N|auto] [--no-compress]\n"
    "  msearch query --corpus <dir> [--threads N|auto] [--rank bm25|tfidf] [-k N] <text>...\n"
    "  msearch --help\n"
    "\n"
    "index builds the index and saves it to a file.\n"
    "query builds the index in memory, then prints the top results with previews.\n"
    "Use \"--\" before the query text if the text itself starts with \"-\".\n";

// The width of `text` in characters (UTF-8 continuation bytes don't count).
std::size_t display_columns(std::string_view text) {
  std::size_t columns = 0;
  for (char c : text) columns += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  return columns;
}

// The snippet text, with highlighted words in bold if `color` is set.
std::string render_snippet(const Snippet& snippet, bool color) {
  std::string out;
  if (snippet.cut_before) out += "... ";
  std::size_t pos = 0;
  for (const Highlight& highlight : snippet.highlights) {
    const bool valid =
        highlight.offset >= pos && highlight.offset + highlight.length <= snippet.text.size();
    if (!valid) continue;
    out.append(snippet.text, pos, highlight.offset - pos);
    if (color) out += kBold;
    out.append(snippet.text, highlight.offset, highlight.length);
    if (color) out += kReset;
    pos = highlight.offset + highlight.length;
  }
  out.append(snippet.text, pos, std::string::npos);
  if (snippet.cut_after) out += " ...";
  return out;
}

std::expected<unsigned, std::string> parse_threads(std::string_view value) {
  try {
    return resolve_threads(value);
  } catch (const std::invalid_argument& e) {
    return std::unexpected(e.what());
  }
}

std::expected<std::size_t, std::string> parse_count(std::string_view flag, std::string_view value) {
  std::size_t number = 0;
  const char* const end = value.data() + value.size();
  const auto [parsed_end, error] = std::from_chars(value.data(), end, number);
  if (error != std::errc() || parsed_end != end || number == 0) {
    return std::unexpected(std::string(flag) + " needs a whole number >= 1, got \"" +
                           std::string(value) + "\"");
  }
  return number;
}

std::string mebibytes(uint64_t bytes) {
  return std::format("{:.2f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

int run_index(const IndexArgs& args, std::ostream& out, std::ostream& err) {
  BuildStats build;
  auto index = build_index(args.corpus, {.threads = args.threads}, &build);
  if (!index) {
    err << "error: " << index.error() << "\n";
    return 1;
  }
  const auto written = write_index(*index, args.out, {.compress = args.compress});
  if (!written) {
    err << "error: " << written.error() << "\n";
    return 1;
  }

  out << std::format(
      "Indexed {} with {} thread(s)\n"
      "  docs:      {}\n"
      "  terms:     {}\n"
      "  postings:  {}\n"
      "  scan:      {:.2f} s\n"
      "  tokenize:  {:.2f} s\n"
      "  merge:     {:.2f} s\n"
      "  write:     {:.2f} s\n"
      "  file:      {} ({}, {})\n",
      args.corpus.string(), build.threads_used, build.docs, build.terms, build.postings,
      build.scan_s, build.tokenize_s, build.merge_s, written->write_s,
      mebibytes(written->file_bytes), args.out.string(), args.compress ? "gap + VByte" : "raw");
  return 0;
}

int usage_error(std::string_view message, std::ostream& err) {
  err << "error: " << message << "\n\n" << kUsage;
  return 1;
}

int run_query(const QueryArgs& args, std::ostream& out, std::ostream& err, bool color) {
  using Clock = std::chrono::steady_clock;

  const auto build_start = Clock::now();
  auto index = build_index(args.corpus, {.threads = args.threads});
  if (!index) {
    err << "error: " << index.error() << "\n";
    return 1;
  }
  err << std::format("Indexed {} docs in {:.1f} s\n", index->docs.size(),
                     std::chrono::duration<double>(Clock::now() - build_start).count());

  const MemoryReader reader(*index);
  Searcher searcher(reader);
  const auto search_start = Clock::now();
  const SearchResponse response = searcher.search(args.text, {.k = args.k, .mode = args.mode});
  const std::vector<ResultLine> lines = make_result_lines(reader, response);
  const double took_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - search_start).count();

  out << format_results(lines, took_ms, color);
  return 0;
}

}  // namespace

std::string format_results(std::span<const ResultLine> lines, double took_ms, bool color) {
  if (lines.empty()) return std::format("No results (found in {:.2f} ms).\n", took_ms);

  std::string out = std::format("Top results (found in {:.2f} ms):\n", took_ms);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const ResultLine& line = lines[i];
    const std::size_t columns = display_columns(line.title);
    const std::size_t padding = columns < kTitleColumns - 2 ? kTitleColumns - columns : 2;
    out += std::format("{:>2}. {}{}score: {:.4f}\n", i + 1, line.title, std::string(padding, ' '),
                       line.score);

    if (!line.snippet) {
      out += std::string(kIndent) + "(document unavailable)\n";
    } else if (!line.snippet->text.empty()) {
      out += std::string(kIndent) + render_snippet(*line.snippet, color) + "\n";
    }
  }
  return out;
}

std::expected<QueryArgs, std::string> parse_query_args(std::span<const std::string_view> args) {
  QueryArgs parsed;
  std::vector<std::string_view> words;
  bool saw_corpus = false;
  bool only_words_left = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (only_words_left || !arg.starts_with('-')) {
      words.push_back(arg);
      continue;
    }
    if (arg == "--") {
      only_words_left = true;
      continue;
    }

    if (arg != "--corpus" && arg != "--threads" && arg != "--rank" && arg != "-k") {
      return std::unexpected("unknown option \"" + std::string(arg) + "\"");
    }
    if (i + 1 >= args.size()) return std::unexpected(std::string(arg) + " needs a value");
    const std::string_view value = args[++i];

    if (arg == "--corpus") {
      parsed.corpus = value;
      saw_corpus = true;
    } else if (arg == "--threads") {
      const auto threads = parse_threads(value);
      if (!threads) return std::unexpected(threads.error());
      parsed.threads = *threads;
    } else if (arg == "--rank") {
      if (value == "bm25") {
        parsed.mode = RankMode::BM25;
      } else if (value == "tfidf") {
        parsed.mode = RankMode::TFIDF;
      } else {
        return std::unexpected("--rank must be bm25 or tfidf, got \"" + std::string(value) + "\"");
      }
    } else {  // -k
      auto k = parse_count("-k", value);
      if (!k) return std::unexpected(k.error());
      parsed.k = *k;
    }
  }

  if (!saw_corpus) return std::unexpected("--corpus is required");
  if (words.empty()) return std::unexpected("missing query text");
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i > 0) parsed.text += ' ';
    parsed.text += words[i];
  }
  return parsed;
}

std::expected<IndexArgs, std::string> parse_index_args(std::span<const std::string_view> args) {
  IndexArgs parsed;
  bool saw_corpus = false;
  bool saw_out = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (!arg.starts_with('-')) {
      if (saw_corpus) return std::unexpected("only one corpus directory can be given");
      parsed.corpus = arg;
      saw_corpus = true;
    } else if (arg == "--no-compress") {
      parsed.compress = false;
    } else if (arg == "-o" || arg == "--threads") {
      if (i + 1 >= args.size()) return std::unexpected(std::string(arg) + " needs a value");
      const std::string_view value = args[++i];
      if (arg == "-o") {
        parsed.out = value;
        saw_out = true;
        continue;
      }
      const auto threads = parse_threads(value);
      if (!threads) return std::unexpected(threads.error());
      parsed.threads = *threads;
    } else {
      return std::unexpected("unknown option \"" + std::string(arg) + "\"");
    }
  }

  if (!saw_corpus) return std::unexpected("missing corpus directory");
  if (!saw_out) return std::unexpected("-o <index file> is required");
  return parsed;
}

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
            bool color) {
  if (args.empty()) {
    err << kUsage;
    return 1;
  }
  const std::string_view command = args[0];
  if (command == "--help" || command == "-h" || command == "help") {
    out << kUsage;
    return 0;
  }
  if (command == "index") {
    const auto parsed = parse_index_args(args.subspan(1));
    if (!parsed) return usage_error(parsed.error(), err);
    return run_index(*parsed, out, err);
  }
  if (command == "query") {
    const auto parsed = parse_query_args(args.subspan(1));
    if (!parsed) return usage_error(parsed.error(), err);
    return run_query(*parsed, out, err, color);
  }
  return usage_error("unknown command \"" + std::string(command) + "\"", err);
}

}  // namespace mse
