#include "spill.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "build_pipeline.hpp"
#include "file_bytes.hpp"
#include "synthetic_corpus.hpp"
#include "temp_dir.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

std::size_t entries_in(const fs::path& dir) {
  return static_cast<std::size_t>(
      std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

// ---- run files ----

TEST(Spill, RunRoundTrip) {
  TempDir dir;
  const fs::path path = dir.path() / "a.run";
  const std::vector<RunRecord> records = {
      {"apple", {{1, 2}, {5, 1}, {900000, 77}}},
      {"banana", {{0, 1}}},
      {"caf\xC3\xA9", {{3, 4}, {4, 4}}},
  };
  auto writer = RunWriter::create(path);
  ASSERT_TRUE(writer.has_value()) << writer.error();
  for (const RunRecord& record : records) {
    ASSERT_TRUE(writer->write(record.term, record.postings).has_value());
  }
  const auto size = writer->finish();
  ASSERT_TRUE(size.has_value()) << size.error();
  EXPECT_EQ(*size, fs::file_size(path));

  auto reader = RunReader::open(path);
  ASSERT_TRUE(reader.has_value()) << reader.error();
  for (const RunRecord& expected : records) {
    const auto record = reader->next();
    ASSERT_TRUE(record.has_value()) << record.error();
    ASSERT_TRUE(record->has_value());
    EXPECT_EQ(**record, expected);
  }
  const auto end = reader->next();
  ASSERT_TRUE(end.has_value());
  EXPECT_FALSE(end->has_value());
  EXPECT_FALSE(reader->next()->has_value());  // and it stays at the end
}

TEST(Spill, EmptyRunAndRecordsLargerThanTheBuffer) {
  TempDir dir;
  {
    auto writer = RunWriter::create(dir.path() / "empty.run");
    ASSERT_TRUE(writer.has_value());
    EXPECT_EQ(writer->finish().value(), 0u);
    auto reader = RunReader::open(dir.path() / "empty.run");
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader->next()->has_value());
  }
  // one record of 400,000 postings (3.2 MB) and many small ones, across several 1 MiB buffers
  std::vector<Posting> big;
  for (uint32_t doc = 0; doc < 400000; ++doc) big.push_back({doc * 3, doc % 7 + 1});
  auto writer = RunWriter::create(dir.path() / "big.run");
  ASSERT_TRUE(writer.has_value());
  std::vector<std::string> terms;
  for (int i = 0; i < 5000; ++i) terms.push_back(std::format("t{:06}", i));
  for (const std::string& term : terms) {
    ASSERT_TRUE(writer->write(term, std::vector<Posting>{{1, 1}, {2, 2}}).has_value());
  }
  ASSERT_TRUE(writer->write("zzz", big).has_value());
  ASSERT_TRUE(writer->finish().has_value());

  auto reader = RunReader::open(dir.path() / "big.run");
  ASSERT_TRUE(reader.has_value());
  RunRecord record;
  for (const std::string& term : terms) {
    ASSERT_TRUE(reader->read(record).value());
    EXPECT_EQ(record.term, term);
    EXPECT_EQ(record.postings.size(), 2u);
  }
  ASSERT_TRUE(reader->read(record).value());
  EXPECT_EQ(record.term, "zzz");
  EXPECT_EQ(record.postings, big);
  EXPECT_FALSE(reader->read(record).value());
}

TEST(Spill, WriterRejectsBadRecords) {
  TempDir dir;
  auto writer = RunWriter::create(dir.path() / "a.run");
  ASSERT_TRUE(writer.has_value());
  const std::vector<Posting> ok = {{1, 1}, {2, 1}};
  EXPECT_FALSE(writer->write("", ok).has_value());                       // empty term
  EXPECT_FALSE(writer->write("a", std::vector<Posting>{}).has_value());  // no postings
  EXPECT_FALSE(writer->write("a", std::vector<Posting>{{2, 1}, {1, 1}}).has_value());  // not sorted
  EXPECT_FALSE(writer->write("a", std::vector<Posting>{{1, 1}, {1, 2}}).has_value());  // same doc
  EXPECT_FALSE(writer->write("a", std::vector<Posting>{{1, 0}}).has_value());          // tf of 0
  ASSERT_TRUE(writer->write("b", ok).has_value());   // a failed write changed nothing
  EXPECT_FALSE(writer->write("b", ok).has_value());  // same term again
  EXPECT_FALSE(writer->write("a", ok).has_value());  // going backwards
  ASSERT_TRUE(writer->write("c", ok).has_value());
}

TEST(Spill, ReaderRejectsDamagedFiles) {
  TempDir dir;
  const fs::path good = dir.path() / "good.run";
  {
    auto writer = RunWriter::create(good);
    ASSERT_TRUE(writer.has_value());
    ASSERT_TRUE(writer->write("alpha", std::vector<Posting>{{1, 1}, {2, 3}}).has_value());
    ASSERT_TRUE(writer->write("beta", std::vector<Posting>{{4, 1}}).has_value());
    ASSERT_TRUE(writer->finish().has_value());
  }
  const Bytes bytes = read_file(good);

  const auto drained = [&](const Bytes& data) -> std::expected<int, std::string> {
    const fs::path path = dir.path() / "bad.run";
    write_file(path, data);
    auto reader = RunReader::open(path);
    if (!reader) return std::unexpected(reader.error());
    int records = 0;
    while (true) {
      auto record = reader->next();
      if (!record) return std::unexpected(record.error());
      if (!record->has_value()) return records;
      ++records;
    }
  };
  ASSERT_EQ(drained(bytes).value(), 2);

  // cut off at every length that is not a record boundary: always an error, never a crash
  const std::size_t first_record = 4 + 5 + 4 + 2 * 8;
  for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
    if (cut == first_record) continue;
    EXPECT_FALSE(drained(Bytes(bytes.begin(), bytes.begin() + static_cast<long>(cut))).has_value())
        << "cut at " << cut;
  }

  Bytes zero_count = bytes;  // the first record claims 0 postings
  zero_count[4 + 5] = std::byte{0};
  EXPECT_FALSE(drained(zero_count).has_value());

  Bytes huge_term = bytes;  // a term length far past the end of the file
  huge_term[3] = std::byte{0x7f};
  EXPECT_FALSE(drained(huge_term).has_value());

  Bytes out_of_order = bytes;  // "alpha" becomes "zlpha", which sorts after "beta"
  out_of_order[4] = std::byte{'z'};
  EXPECT_FALSE(drained(out_of_order).has_value());

  EXPECT_FALSE(RunReader::open(dir.path() / "missing.run").has_value());
}

// ---- the memory budget ----

TEST(Spill, ParseBudget) {
  EXPECT_EQ(parse_memory_budget("512M").value(), 536870912u);
  EXPECT_EQ(parse_memory_budget("4G").value(), 4294967296u);
  EXPECT_EQ(parse_memory_budget("64K").value(), 65536u);
  EXPECT_EQ(parse_memory_budget("1024").value(), 1024u);
  EXPECT_EQ(parse_memory_budget("2g").value(), 2147483648u);
  EXPECT_EQ(parse_memory_budget("0").value(), 0u);
  for (const char* bad : {"x", "", "M", "-1", "4 G", "4GB", "1.5G", "5T", "0x10", " 4G",
                          "99999999999999999999", "17179869184G"}) {
    EXPECT_FALSE(parse_memory_budget(bad).has_value()) << '"' << bad << '"';
  }
}

struct Built {
  Bytes file;
  BuildStats stats;
  WriteStats written;
};

Built build(const fs::path& corpus, const fs::path& dir, const std::string& name, unsigned threads,
            uint64_t budget, const fs::path& tmp = {}) {
  Built built;
  BuildOptions options;
  options.threads = threads;
  options.memory_budget_bytes = budget;
  options.tmp_dir = tmp;
  const fs::path out = dir / name;
  auto written = build_to_file(corpus, out, options, {}, &built.stats);
  EXPECT_TRUE(written.has_value()) << (written ? "" : written.error());
  if (written) built.written = *written;
  built.file = read_file(out);
  return built;
}

TEST(Spill, SpillingEqualsInMemory) {
  TempDir corpus, work, tmp;
  write_synthetic_corpus(corpus.path(), 20000, 3000, 3, 91);

  const Built in_memory = build(corpus.path(), work.path(), "memory.bin", 4, 0);
  ASSERT_FALSE(in_memory.file.empty());
  EXPECT_EQ(in_memory.stats.runs_spilled, 0u);
  EXPECT_EQ(in_memory.stats.spill_bytes, 0u);

  const Built spilled = build(corpus.path(), work.path(), "spill.bin", 4, 64 * 1024, tmp.path());
  EXPECT_GT(spilled.stats.runs_spilled, 10u);  // the budget really forced many runs
  EXPECT_GT(spilled.stats.spill_bytes, 0u);
  EXPECT_EQ(spilled.file, in_memory.file);  // byte for byte
  EXPECT_EQ(spilled.stats.docs, in_memory.stats.docs);
  EXPECT_EQ(spilled.stats.terms, in_memory.stats.terms);
  EXPECT_EQ(spilled.stats.postings, in_memory.stats.postings);
  EXPECT_EQ(spilled.stats.tokens, in_memory.stats.tokens);
  EXPECT_EQ(spilled.written.file_bytes, in_memory.written.file_bytes);

  // any number of threads gives the same file, with and without a budget
  for (const unsigned threads : {1u, 2u, 7u}) {
    const Built other =
        build(corpus.path(), work.path(), "other.bin", threads, 64 * 1024, tmp.path());
    EXPECT_EQ(other.file, in_memory.file) << threads << " threads";
  }
  // and so does a budget so large that nothing needs to be spilled early
  const Built roomy =
      build(corpus.path(), work.path(), "roomy.bin", 4, uint64_t{1} << 30, tmp.path());
  EXPECT_EQ(roomy.file, in_memory.file);
  EXPECT_LE(roomy.stats.runs_spilled, 4u);  // at most the final flush of each worker
}

// More runs than may be open at once are first merged into bigger runs; the file is the same.
TEST(Spill, MergePassesGiveTheSameFile) {
  TempDir corpus, work, tmp;
  write_synthetic_corpus(corpus.path(), 6000, 1500, 2, 97);
  const Built in_memory = build(corpus.path(), work.path(), "memory.bin", 3, 0);

  for (const std::size_t fan_in : {2u, 3u, 5u, 64u}) {
    BuildOptions options;
    options.threads = 3;
    options.memory_budget_bytes = 32 * 1024;
    options.tmp_dir = tmp.path();
    options.max_merge_fan_in = fan_in;
    BuildStats stats;
    ASSERT_TRUE(
        build_to_file(corpus.path(), work.path() / "fan.bin", options, {}, &stats).has_value());
    EXPECT_EQ(read_file(work.path() / "fan.bin"), in_memory.file) << "fan-in " << fan_in;
    if (fan_in < stats.runs_spilled) {
      EXPECT_GT(stats.merge_passes, 0u)
          << "fan-in " << fan_in << ", " << stats.runs_spilled << " runs";
    } else {
      EXPECT_EQ(stats.merge_passes, 0u);
    }
    EXPECT_EQ(entries_in(tmp.path()), 0u) << "fan-in " << fan_in;
  }
}

TEST(Spill, SpillingEqualsInMemoryWithoutCompression) {
  TempDir corpus, work, tmp;
  write_synthetic_corpus(corpus.path(), 3000, 500, 2, 92);
  BuildOptions plain;
  plain.threads = 2;
  BuildOptions limited = plain;
  limited.memory_budget_bytes = 16 * 1024;
  limited.tmp_dir = tmp.path();
  const WriteOptions raw{.compress = false, .build_spell = true};
  ASSERT_TRUE(build_to_file(corpus.path(), work.path() / "a.bin", plain, raw).has_value());
  ASSERT_TRUE(build_to_file(corpus.path(), work.path() / "b.bin", limited, raw).has_value());
  EXPECT_EQ(read_file(work.path() / "a.bin"), read_file(work.path() / "b.bin"));
}

TEST(Spill, SpillCleansUp) {
  TempDir corpus, work, tmp;
  write_synthetic_corpus(corpus.path(), 4000, 800, 2, 93);
  const Built built = build(corpus.path(), work.path(), "a.bin", 3, 32 * 1024, tmp.path());
  ASSERT_GT(built.stats.runs_spilled, 3u);
  EXPECT_EQ(entries_in(tmp.path()), 0u);   // every run file is gone
  EXPECT_EQ(entries_in(work.path()), 1u);  // only the index is left next to the output
}

TEST(Spill, SpillCleansUpAfterAnError) {
  TempDir corpus, work, tmp;
  write_synthetic_corpus(corpus.path(), 4000, 800, 2, 94);
  fs::create_directory(work.path() / "in-the-way");  // the output path is a directory: rename fails
  fs::create_directory(work.path() / "in-the-way" / "x");
  BuildOptions options;
  options.threads = 2;
  options.memory_budget_bytes = 32 * 1024;
  options.tmp_dir = tmp.path();
  const auto written = build_to_file(corpus.path(), work.path() / "in-the-way", options, {});
  EXPECT_FALSE(written.has_value());
  EXPECT_EQ(entries_in(tmp.path()), 0u);
  EXPECT_EQ(entries_in(work.path()), 1u);  // no half-written index.bin.tmp either
}

TEST(Spill, TmpDirIsCreatedAndDefaultsToTheOutputFolder) {
  TempDir corpus, work;
  write_synthetic_corpus(corpus.path(), 2000, 300, 1, 95);
  const fs::path tmp = work.path() / "a" / "b";
  const Built made = build(corpus.path(), work.path(), "a.bin", 2, 16 * 1024, tmp);
  EXPECT_GT(made.stats.runs_spilled, 0u);
  EXPECT_TRUE(fs::is_directory(tmp));
  EXPECT_EQ(entries_in(tmp), 0u);
  const Built fallback = build(corpus.path(), work.path(), "b.bin", 2, 16 * 1024);  // no tmp_dir
  EXPECT_EQ(fallback.file, made.file);
  EXPECT_EQ(entries_in(work.path()), 3u);  // a.bin, b.bin and the folder made above
}

TEST(Spill, EmptyAndTinyCorporaWork) {
  TempDir corpus, work, tmp;
  const Built empty = build(corpus.path(), work.path(), "empty.bin", 4, 1024, tmp.path());
  EXPECT_EQ(empty.stats.docs, 0u);
  EXPECT_EQ(empty.stats.runs_spilled, 0u);
  const Built plain = build(corpus.path(), work.path(), "empty2.bin", 4, 0);
  EXPECT_EQ(empty.file, plain.file);

  write_synthetic_corpus(corpus.path(), 3, 5, 1, 96);  // fewer docs than threads
  const Built tiny = build(corpus.path(), work.path(), "tiny.bin", 8, 1, tmp.path());
  const Built tiny_plain = build(corpus.path(), work.path(), "tiny2.bin", 8, 0);
  EXPECT_EQ(tiny.file, tiny_plain.file);
}

TEST(Spill, ErrorsAreReported) {
  TempDir work, tmp;
  BuildOptions options;
  options.memory_budget_bytes = 1024;
  options.tmp_dir = tmp.path();
  EXPECT_FALSE(build_to_file(work.path() / "no-such-corpus", work.path() / "a.bin", options, {})
                   .has_value());
  EXPECT_EQ(entries_in(tmp.path()), 0u);
}

}  // namespace
}  // namespace mse
