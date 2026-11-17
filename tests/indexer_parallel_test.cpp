#include <gtest/gtest.h>

#include <stdexcept>

#include "indexer.hpp"
#include "synthetic_corpus.hpp"
#include "temp_dir.hpp"
#include "test_data.hpp"

namespace mse {
namespace {

InMemoryIndex build(const std::filesystem::path& corpus, unsigned threads,
                    std::size_t batch_size = kBatchSize, BuildStats* stats = nullptr) {
  auto index = build_index(corpus, {.threads = threads, .batch_size = batch_size}, stats);
  if (!index) throw std::runtime_error(index.error());
  return *std::move(index);
}

// Compared with == rather than EXPECT_EQ, so a failure doesn't print a whole index.
::testing::AssertionResult same_index(const InMemoryIndex& a, const InMemoryIndex& b) {
  if (a == b) return ::testing::AssertionSuccess();
  return ::testing::AssertionFailure()
         << "indexes differ: " << a.docs.size() << " vs " << b.docs.size() << " docs, "
         << a.postings.size() << " vs " << b.postings.size() << " terms, " << a.total_postings()
         << " vs " << b.total_postings() << " postings";
}

TEST(IndexerParallel, EqualsSerialOnTheFixture) {
  const InMemoryIndex serial = build(tiny_corpus_dir(), 1);
  for (unsigned threads : {2u, 4u, 7u}) {
    EXPECT_TRUE(same_index(build(tiny_corpus_dir(), threads), serial)) << threads << " threads";
  }
}

TEST(IndexerParallel, EqualsSerialOnASyntheticCorpus) {
  TempDir dir;
  write_synthetic_corpus(dir.path(), 3000, 500, 3, 1);
  // A batch size of 7 makes many small batches, so they get spread over the workers.
  const InMemoryIndex serial = build(dir.path(), 1, 7);
  ASSERT_GT(serial.postings.size(), 500u);  // the test is only meaningful on a real-sized index
  ASSERT_GT(serial.total_postings(), 50000u);
  for (unsigned threads : {2u, 3u, 8u, 32u}) {
    EXPECT_TRUE(same_index(build(dir.path(), threads, 7), serial)) << threads << " threads";
  }
}

TEST(IndexerParallel, BatchSizeAndThreadCountDoNotMatter) {
  TempDir dir;
  write_synthetic_corpus(dir.path(), 1000, 200, 2, 2);
  const InMemoryIndex reference = build(dir.path(), 1);
  for (std::size_t batch_size : {1u, 7u, 256u, 100000u}) {
    for (unsigned threads : {1u, 5u}) {
      EXPECT_TRUE(same_index(build(dir.path(), threads, batch_size), reference))
          << "batch " << batch_size << ", " << threads << " threads";
    }
  }
}

TEST(IndexerParallel, MoreThreadsThanBatches) {
  // The fixture has 2 batches, so most of these threads would have nothing to do.
  EXPECT_TRUE(same_index(build(tiny_corpus_dir(), 64), build(tiny_corpus_dir(), 1)));
}

TEST(IndexerParallel, RepeatedRunsGiveTheSameIndex) {
  TempDir dir;
  write_synthetic_corpus(dir.path(), 2000, 300, 4, 3);
  const InMemoryIndex first = build(dir.path(), 8, 16);
  for (int run = 0; run < 4; ++run) {
    EXPECT_TRUE(same_index(build(dir.path(), 8, 16), first)) << "run " << run;
  }
}

TEST(IndexerParallel, EmptyCorpusWithManyThreads) {
  TempDir dir;
  const InMemoryIndex index = build(dir.path(), 8);
  EXPECT_TRUE(index.docs.empty());
  EXPECT_TRUE(index.postings.empty());
}

TEST(IndexerParallel, UsesTheRequestedNumberOfWorkers) {
  TempDir dir;
  write_synthetic_corpus(dir.path(), 1000, 100, 2, 5);
  BuildStats stats;
  build(dir.path(), 1, 10, &stats);
  EXPECT_EQ(stats.threads_used, 1u);
  build(dir.path(), 8, 10, &stats);  // 100 batches, so all 8 workers have work
  EXPECT_EQ(stats.threads_used, 8u);

  build(tiny_corpus_dir(), 64, kBatchSize, &stats);  // 2 batches: more workers would be idle
  EXPECT_EQ(stats.threads_used, 2u);

  TempDir empty;
  build(empty.path(), 8, kBatchSize, &stats);
  EXPECT_EQ(stats.threads_used, 1u);
}

TEST(IndexerParallel, StatsAddUpAcrossThreads) {
  TempDir dir;
  write_synthetic_corpus(dir.path(), 500, 100, 2, 4);
  BuildStats serial_stats;
  BuildStats parallel_stats;
  const InMemoryIndex serial = build(dir.path(), 1, 10, &serial_stats);
  const InMemoryIndex parallel = build(dir.path(), 6, 10, &parallel_stats);
  EXPECT_EQ(parallel_stats.docs, serial_stats.docs);
  EXPECT_EQ(parallel_stats.tokens, serial_stats.tokens);
  EXPECT_EQ(parallel_stats.terms, serial_stats.terms);
  EXPECT_EQ(parallel_stats.postings, serial_stats.postings);
  EXPECT_EQ(parallel_stats.terms, parallel.postings.size());
  EXPECT_GE(parallel_stats.merge_s, 0.0);
}

}  // namespace
}  // namespace mse
