#include "cli.hpp"

#include <charconv>
#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include "autocomplete.hpp"
#include "index_writer.hpp"
#include "indexer.hpp"
#include "searcher.hpp"

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
    "  msearch query -i <index.bin> [--load mmap|read] [--corpus <dir>] [--rank ...] [-k N] "
    "<text>...\n"
    "  msearch query -i <index.bin> --count-only <word>\n"
    "  msearch search -i <index.bin> [--load mmap|read] [--corpus <dir>] [--rank ...] [-k N]\n"
    "  msearch --help\n"
    "\n"
    "index builds the index and saves it to a file.\n"
    "query with --corpus builds the index in memory first; with -i it opens a saved index.\n"
    "search opens a saved index once and answers one query per line until the input ends.\n"
    "In search, \":suggest <prefix>\" completes a word, and \"quit\" ends the session.\n"
    "With -i, --corpus tells where to read the previews from instead of the stored folder.\n"
    "Use \"--\" before the query text if the text itself starts with \"-\".\n";

std::string_view trim(std::string_view text) {
  constexpr std::string_view kBlanks = " \t\r\n";
  const std::size_t first = text.find_first_not_of(kBlanks);
  if (first == std::string_view::npos) return {};
  return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

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

bool is_query_option(std::string_view flag) {
  return flag == "--corpus" || flag == "-i" || flag == "--load" || flag == "--threads" ||
         flag == "--rank" || flag == "-k";
}

// Applies one "--flag value" pair to `parsed`.
std::expected<void, std::string> apply_query_option(QueryArgs& parsed, std::string_view flag,
                                                    std::string_view value) {
  if (flag == "--corpus") {
    parsed.corpus = value;
  } else if (flag == "-i") {
    parsed.index = value;
  } else if (flag == "--load") {
    if (value != "mmap" && value != "read") {
      return std::unexpected("--load must be mmap or read, got \"" + std::string(value) + "\"");
    }
    parsed.load = value == "mmap" ? LoadMode::Mmap : LoadMode::Read;
  } else if (flag == "--threads") {
    const auto threads = parse_threads(value);
    if (!threads) return std::unexpected(threads.error());
    parsed.threads = *threads;
  } else if (flag == "--rank") {
    if (value != "bm25" && value != "tfidf") {
      return std::unexpected("--rank must be bm25 or tfidf, got \"" + std::string(value) + "\"");
    }
    parsed.mode = value == "bm25" ? RankMode::BM25 : RankMode::TFIDF;
  } else {  // -k
    const auto k = parse_count("-k", value);
    if (!k) return std::unexpected(k.error());
    parsed.k = *k;
  }
  return {};
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

const char* load_mode_name(LoadMode mode) { return mode == LoadMode::Mmap ? "mmap" : "read"; }

// Searches `text` and returns the printed results. The time shown covers the search and the
// snippets, which is what the user waits for.
template <IndexReader R>
std::string answer(const R& reader, Searcher<R>& searcher, const QueryArgs& args,
                   std::string_view text, std::string_view corpus_root_override, bool color) {
  const auto start = std::chrono::steady_clock::now();
  const SearchResponse response = searcher.search(text, {.k = args.k, .mode = args.mode});
  const std::vector<ResultLine> lines = make_result_lines(reader, response, corpus_root_override);
  const double took_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::string printed;
  if (response.corrected_query) {
    printed = std::format("Showing results for \"{}\" (no match for \"{}\").\n",
                          *response.corrected_query, trim(text));
  }
  return printed + format_results(lines, took_ms, color);
}

int query_in_memory(const QueryArgs& args, std::ostream& out, std::ostream& err, bool color) {
  const auto build_start = std::chrono::steady_clock::now();
  auto index = build_index(args.corpus, {.threads = args.threads});
  if (!index) {
    err << "error: " << index.error() << "\n";
    return 1;
  }
  err << std::format(
      "Indexed {} docs in {:.1f} s\n", index->docs.size(),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count());

  const MemoryReader reader(*index);
  Searcher searcher(reader);
  out << answer(reader, searcher, args, args.text, {}, color);
  return 0;
}

// Opens the saved index and says how long that took.
std::expected<Index, std::string> open_saved_index(const QueryArgs& args, std::ostream& err) {
  const auto start = std::chrono::steady_clock::now();
  auto index = Index::open(args.index, args.load);
  if (!index) return std::unexpected(index.error());
  err << std::format(
      "Index opened via {} in {:.2f} ms\n", load_mode_name(args.load),
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return index;
}

// --count-only: the number of docs that contain the one word of `text`, and nothing else.
int print_document_frequency(const IndexView& view, std::string_view text, std::ostream& out,
                             std::ostream& err) {
  const std::vector<std::string> words = unique_query_terms(text);
  if (words.size() != 1) {
    err << "error: --count-only needs exactly one word\n";
    return 1;
  }
  const auto term = view.find_term(words[0]);
  out << (term ? view.df_at(*term) : 0u) << "\n";
  return 0;
}

int query_saved_index(const QueryArgs& args, std::ostream& out, std::ostream& err, bool color) {
  const auto index = open_saved_index(args, err);
  if (!index) {
    err << "error: " << index.error() << "\n";
    return 1;
  }
  if (args.count_only) return print_document_frequency(index->view(), args.text, out, err);
  Searcher<IndexView> searcher(index->view());
  out << answer(index->view(), searcher, args, args.text, args.corpus.string(), color);
  return 0;
}

int run_query(const QueryArgs& args, std::ostream& out, std::ostream& err, bool color) {
  if (args.count_only && args.index.empty()) {
    err << "error: --count-only needs a saved index (-i)\n";
    return 1;
  }
  return args.index.empty() ? query_in_memory(args, out, err, color)
                            : query_saved_index(args, out, err, color);
}

constexpr std::string_view kSuggestCommand = ":suggest";

std::string format_suggestions(const std::vector<std::string>& terms) {
  if (terms.empty()) return "No suggestions.\n";
  std::string out = "Suggestions: ";
  for (std::size_t i = 0; i < terms.size(); ++i) out += (i > 0 ? ", " : "") + terms[i];
  return out + "\n";
}

// One query per input line, until the input ends or a line says "quit". The prompt goes to `err`
// so that `out` holds nothing but results, even when it is piped.
int run_search(const QueryArgs& args, std::istream& in, std::ostream& out, std::ostream& err,
               bool color) {
  const auto index = open_saved_index(args, err);
  if (!index) {
    err << "error: " << index.error() << "\n";
    return 1;
  }
  Searcher<IndexView> searcher(index->view());

  std::string line;
  while (err << "> " << std::flush, std::getline(in, line)) {
    const std::string_view text = trim(line);
    if (text == "quit" || text == "exit") break;
    if (text.empty()) continue;
    if (text.starts_with(kSuggestCommand)) {
      out << format_suggestions(suggest(index->view(), trim(text.substr(kSuggestCommand.size()))))
          << std::flush;
      continue;
    }
    out << answer(index->view(), searcher, args, text, args.corpus.string(), color) << std::flush;
  }
  err << "\n";
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

std::expected<QueryArgs, std::string> parse_query_args(std::span<const std::string_view> args,
                                                       bool text_required) {
  QueryArgs parsed;
  std::vector<std::string_view> words;
  bool only_words_left = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (only_words_left || !arg.starts_with('-')) {
      words.push_back(arg);
    } else if (arg == "--") {
      only_words_left = true;
    } else if (arg == "--count-only") {
      parsed.count_only = true;
    } else {
      if (!is_query_option(arg))
        return std::unexpected("unknown option \"" + std::string(arg) + "\"");
      if (i + 1 >= args.size()) return std::unexpected(std::string(arg) + " needs a value");
      if (auto ok = apply_query_option(parsed, arg, args[++i]); !ok) {
        return std::unexpected(ok.error());
      }
    }
  }

  if (parsed.index.empty() && (!text_required || parsed.corpus.empty())) {
    return std::unexpected(text_required ? "either -i <index> or --corpus <dir> is required"
                                         : "-i <index> is required");
  }
  if (!text_required && !words.empty()) return std::unexpected("unexpected text after the options");
  if (text_required && words.empty()) return std::unexpected("missing query text");
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

int run_cli(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
            std::ostream& err, bool color) {
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
  if (command == "query" || command == "search") {
    const bool is_query = command == "query";
    const auto parsed = parse_query_args(args.subspan(1), is_query);
    if (!parsed) return usage_error(parsed.error(), err);
    return is_query ? run_query(*parsed, out, err, color)
                    : run_search(*parsed, in, out, err, color);
  }
  return usage_error("unknown command \"" + std::string(command) + "\"", err);
}

int run_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
            bool color) {
  return run_cli(args, std::cin, out, err, color);
}

}  // namespace mse
