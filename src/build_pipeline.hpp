// Builds an index file from a corpus, in memory or within a memory budget (DESIGN.md §4).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include "index_writer.hpp"
#include "indexer.hpp"

namespace mse {

// A number of bytes with an optional suffix K, M or G (powers of 1024, either case): "512M" is
// 536870912. A plain number is bytes. Anything else, or a value too large for 64 bits, is an error.
[[nodiscard]] std::expected<uint64_t, std::string> parse_memory_budget(std::string_view text);

// Indexes `corpus_root` and writes the index file `out`.
//
// With opts.memory_budget_bytes == 0 the whole index is built in memory and then written. With a
// budget, each worker thread counts postings in memory until its estimate passes budget / threads,
// then sorts them and writes a run file to opts.tmp_dir (the output's folder if that is empty); at
// the end one k-way merge of all the runs streams the terms, in order, into the IndexWriter. The
// file is byte-identical either way, for any thread count. Run files are deleted before returning,
// also on an error. `stats`, if given, is filled in (runs_spilled and spill_bytes are 0 without a
// budget).
//
// In `stats`, merge_s is the k-way merge (reading the runs and handing terms to the writer), and
// in the returned WriteStats write_s is only the final assembly of the file (the term table, the
// spell tree, the sync), because with a budget the two overlap. Without a budget they are separate.
//
// The budget is for the whole process, as far as the build controls it. It first pays for what is
// there anyway: the document table and some memory per thread (batch, tokenizer scratch, allocator
// slack). What is left is shared by the workers, and a worker writes a run when its estimate of
// what it holds (postings by the capacity of their lists, plus a fixed cost per term) reaches its
// share. A budget too small for the fixed part is not honoured: the workers get half of it and the
// process will be over. benchmarks/NOTES.md compares the budget with the measured peak.
[[nodiscard]] std::expected<WriteStats, std::string> build_to_file(
    const std::filesystem::path& corpus_root, const std::filesystem::path& out,
    const BuildOptions& opts, const WriteOptions& write_options, BuildStats* stats = nullptr);

}  // namespace mse
