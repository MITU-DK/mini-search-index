# Benchmark notes

## B1: indexing thread scaling (laptop, `main` profile, 150,000 docs)

Source: `benchmarks/results/laptop/b1_build-main.csv` (5 runs per thread count after one discarded
warm-up run, each thread count in a fresh process). The machine is described in
`benchmarks/results/laptop/machine.txt`: i7-12700H, 6 performance cores with SMT (12 logical CPUs) and
8 efficiency cores (20 logical CPUs in all), plugged in, `performance` power profile. The table shows
the median of the 5 runs.

| Threads | Total (s) | Speedup | Tokenize (s) | Merge (s) | Merge share | Peak RSS (MiB) |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 26.78 | 1.00x | 26.52 | 0.00 | 0% | 1117 |
| 2 | 16.91 | 1.58x | 15.07 | 1.62 | 10% | 1739 |
| 4 | 10.45 | 2.56x | 8.78 | 1.39 | 13% | 1693 |
| 6 | 8.20 | 3.27x | 6.50 | 1.36 | 17% | 1715 |
| 8 | 7.88 | 3.40x | 6.18 | 1.42 | 18% | 1881 |
| 10 | 7.12 | 3.76x | 5.46 | 1.37 | 19% | 1929 |
| 12 | 6.66 | 4.02x | 5.08 | 1.30 | 19% | 1902 |
| 14 | 7.04 | 3.80x | 5.29 | 1.42 | 20% | 1924 |
| 16 | 6.96 | 3.85x | 5.21 | 1.44 | 21% | 1959 |
| 20 | 7.00 | 3.83x | 5.16 | 1.52 | 22% | 2034 |

Results are in-memory only; writing the index file comes in Phase 4.

### Where the speedup flattens, and what is known about why

The speedup rises to **4.0x at 12 threads** and stays there. 14, 16 and 20 threads
are no faster (they are within the run-to-run spread of the 12-thread result or slightly slower).

Measured, from the CSV:

- **The merge phase does not scale.** It takes about 1.3 to 1.5 s at every thread count from 2 up, so its
  share of the total grows from 10% at 2 threads to 22% at 20. Together with the scan (0.14 s) this
  fixed cost caps the speedup (Amdahl's law): even if tokenizing were free, 20 threads could not beat
  `26.8 / 1.7`, about 16x, and in practice the tokenize phase is the larger limit.
- **The tokenize phase alone reaches about 5x** (26.5 s to 5.1 s at 12 threads) and does not improve
  after that.
- **Memory use grows with threads** (1.1 GiB at 1 thread, about 1.9 to 2.0 GiB from 8 up) because
  each worker keeps its own term maps until the merge.

Exploratory runs (single runs with a scratch driver under `taskset`, **not in the CSV and not to be
quoted until redone with `msearch-bench` under `taskset`**):

- One thread takes 28.8 s tokenizing on a P-core and 42.5 s on an E-core, so an E-core does about
  68% of a P-core's work.
- 6 threads on 6 separate P-cores: 6.6 s, which is 4.4x faster than 1 thread (73% efficiency).
- 12 threads on the P-cores' 12 logical CPUs (SMT): 5.6 s. SMT adds only about 1.2x over 6 threads.
- 8 threads on the 8 E-cores: 7.9 s.
- The merge's "parallel" shard step takes about 1.2 s whether T is 4, 12 or 20, plus about 0.25 s
  for the final serial union of the shard maps.

What this suggests, but has not been isolated:

- The hardware is not 20 equal cores: SMT siblings share one core's execution units, and E-cores are
  slower, so the ideal speedup is about 9 to 10 "P-core equivalents", not 20.
- Efficiency is already only 73% with 6 threads on 6 separate P-cores, which points at something
  shared that the threads wait on (the memory allocator, memory bandwidth or the shared L3 cache).
  Which one it is has **not** been measured. Nothing here should be quoted as the cause.

### Why the merge does not scale (measured)

CPU seconds summed over all merge threads, from a scratch driver (single runs, exploratory, same
caveat as above), for the three merge steps:

| Threads | Combining the workers' lists | Sorting | Freeing the leftover maps |
|---:|---:|---:|---:|
| 4 | 2.3 | 1.7 | 1.0 |
| 12 | 7.9 | 2.1 | 4.1 |
| 20 | 16.4 | 2.6 | 9.9 |

Dividing by the thread count gives the 1.2 to 1.5 s wall time seen in B1. The sort is the cheapest
step and barely grows. The other two grow with the thread count, because every worker keeps its own
entry for almost every common term, so 20 workers mean about 20 copies of each to combine and free.
A first guess that re-sorting was the cost was wrong.

A real fix has to stop the work from growing with the number of workers, for example a shared term
dictionary or sorted runs merged in one pass (the same structure the bounded-memory SPIMI build in
Phase 9 needs). Small tweaks to the current merge would save maybe a quarter of it, so the current
design is kept and this limit is reported as it is.

<!-- BEGIN b2 -->
## B2: index size (laptop, `main` profile)

Source: `benchmarks/results/laptop/b2_size-main.csv` and `b2_terms-main.csv`
(`tools/run_benchmarks.sh b2 --machine laptop --profile main`). Sizes do not depend on the machine.

| | Size | Compared with the corpus |
|---|---:|---:|
| Corpus text (`part-*.tsv`) | 1,513.1 MiB | 100% |
| Index file, raw postings | 697.9 MiB | 46.1% |
| Index file, gap + VByte postings | 228.4 MiB | 15.1% |

- 150,000 docs, 1,467,863 terms, 84,624,579 postings.
- Postings alone: 645.6 MiB raw (8 bytes each) against 176.2 MiB compressed, which is
  **3.66x smaller** and 2.18 bytes per posting.
- The whole file is 3.06x smaller, not 3.66x, because the rest of
  the file does not shrink: the file minus its postings is 52.2 MiB, of which the term
  table alone is 1,467,863 x 24 bytes = 33.6 MiB.

### Why a common word costs about 1 byte per gap

A gap is the distance between two doc ids in a list. A word in most docs has gaps of 1, 2 or 3, which
fit in one byte (anything below 128 does). A rare word has few postings spread over 150,000 docs, so
its gaps are large and need two or three bytes. The table shows the term at several ranks of document
frequency (rank 1 is the term in the most docs):

| df rank | Term | df | Bytes per gap | Bytes per posting (gap + tf) |
|---:|---|---:|---:|---:|
| 1 | `to` | 142,023 | 1.00 | 2.07 |
| 2 | `the` | 141,322 | 1.00 | 2.32 |
| 3 | `of` | 140,423 | 1.00 | 2.14 |
| 10 | `was` | 128,116 | 1.00 | 2.02 |
| 100 | `then` | 63,322 | 1.00 | 2.00 |
| 1000 | `growth` | 14,773 | 1.00 | 2.00 |
| 10000 | `dynasties` | 1,127 | 1.27 | 2.27 |
| 100000 | `romola` | 22 | 2.00 | 3.00 |
| 1000000 | `kalakajoro` | 1 | 3.00 | 4.00 |

The term frequency adds one byte per posting while tf is below 128, which holds for most terms, so
dense lists settle near 2 bytes per posting. The commonest words go over it in long articles: `the` needs a second byte for tf in 32% of its postings, which is why it costs 2.32 bytes per posting and not 2. Rare terms cost 3 to 4 bytes because their
gaps are large. Raw postings always cost 8.
<!-- END b2 -->

<!-- BEGIN b3 -->
## B3: loading the index, `parse` vs `read` vs `mmap` (laptop, `main` index)

Source: `benchmarks/results/laptop/b3_load-main.csv` (`tools/run_benchmarks.sh b3 --machine
laptop --profile main`). The index is `data/index/main/vbyte.bin` (228 MiB, from B2).
Every measurement is a fresh process; one discarded round first, then 5 runs per cell. The table shows
medians and the min to max range of the two times. **Cold** means the file was evicted from the page
cache with `posix_fadvise(DONTNEED)` just before opening; **warm** means it was already cached. The
queries are the titles of 1000 random docs (seed 42), so "first query" is one fixed title, the same in
every run. The time shown for a query is the search itself; the previews (snippets) are not included.

| Cache | Mode | Open (ms) | Range | First query (ms) | Range | Open + first query (ms) | RSS after open (MiB) | RSS after 1000 queries (MiB) | Major faults, first query | Minor faults, first query |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| cold | parse | 1,282 | 1,227 to 1,343 | 0.808 | 0.776 to 0.853 | 1,283 | 816 | 819 | 0 | 313 |
| cold | read | 253 | 247 to 256 | 0.689 | 0.656 to 1.25 | 254 | 233 | 236 | 0 | 310 |
| cold | mmap | 0.537 | 0.513 to 0.674 | 11.9 | 10.8 to 15.4 | 12.4 | 4 | 127 | 32 | 321 |
| warm | parse | 1,131 | 1,097 to 1,179 | 0.811 | 0.770 to 0.975 | 1,132 | 816 | 819 | 0 | 313 |
| warm | read | 111 | 107 to 121 | 0.656 | 0.641 to 0.688 | 112 | 233 | 236 | 0 | 310 |
| warm | mmap | 0.016 | 0.009 to 0.017 | 1.13 | 0.738 to 1.18 | 1.15 | 4 | 133 | 0 | 350 |

### What the three modes show

- **`parse` is slow because of decoding, not disk.** Opening takes 1,131 ms warm and 1,282 ms
  cold. `read` also reads the whole file into memory and takes 111 ms warm, so decoding the
  postings into hash maps costs about **1,020 ms**, which is 80% of the
  cold `parse` time. The disk accounts for only about 151 ms (cold minus warm). `parse` also
  needs 812 MiB of memory, 3.6 times the size of the file, because
  hash maps and vectors take far more space than the packed bytes.
- **`read` is a plain copy.** It spends its 111 ms warm copying 228 MiB into a buffer
  and nothing decoding. Cold, the disk adds about 142 ms. Its memory is the size of the file.
- **`mmap` opens in 0.016 ms warm and 0.537
  ms cold**, because opening only maps the file and checks the header. Nothing is read until a query
  touches it, and the memory after opening is 4 MiB
  (only the program itself).
- **The cost moves to the first query.** Warm, the first `mmap` query takes 1.13 ms against
  0.811 ms for `parse`, which is 0.32 ms more. Mapped pages
  that are already in the page cache still have to be entered into this process's page table, one minor
  page fault each: 350 minor faults against
  313 for `parse` (37 more; the rest is setup
  that every mode shares). Cold, the first query takes **11.9 ms with
  32 major faults**, because those pages must come
  from disk. The `parse` and `read` first queries take under a millisecond, because everything is
  already in memory: they paid before the query, at open.
- **Open plus first query**, which is how long a user waits for the first answer from a new process:
  `mmap` 12.4 ms cold and 1.15 ms warm, `read`
  254 ms and 112 ms, `parse` 1,283
  ms and 1,132 ms.
- **After 1000 queries** `mmap` has 133 MiB resident,
  all of it file pages that the page cache shares with other processes, against
  819 MiB of private memory for `parse`. 1000 random
  title queries touch about 58% of the
  file, which is why the memory grows.

Limits of these numbers: one index on one machine; the first query is a single title, so its cold time
depends on which pages that title happens to need; eviction with `fadvise` drops this file only (not a
full `drop_caches`); times are for the search only, without previews.
<!-- END b3 -->

<!-- BEGIN b7 -->
## B7: autocomplete, sorted table vs trie (laptop, `main` index)

Source: `benchmarks/results/laptop/b7_suggest-main.csv` (`tools/run_benchmarks.sh b7 --machine
laptop --profile main`). 1000 seeded prefixes (2 to 5 leading bytes of random terms in at least
10 docs), each asked 5 times after one unrecorded pass, so the times are warm. Percentiles are
nearest-rank over the 5000 timings. The sorted table is the production path: a binary search over the
mapped term table, then the best 5 of the range. The trie is built in memory from the same terms and
keeps the best 5 terms in every node.

| Method | Build (ms) | Extra memory (MiB) | p50 (µs) | p95 (µs) |
|---|---:|---:|---:|---:|
| sorted table | 0 | 0 | 2.42 | 27.5 |
| trie | 352 | 216 | 0.582 | 1.16 |

- The trie answers **4.2x faster at the median and 24x faster at p95**.
- It has to be built from every term before the first query: 352 ms here, on
  every start of the program. That is the cost mmap was chosen to avoid (B3: the index opens in well
  under a millisecond).
- It needs 216 MiB of memory on top of the index, about 0.9 times the size of
  the 228 MiB index file itself, because it has one node per distinct prefix of every term.
- The table needs nothing extra: no build, no memory beyond the pages it touches.

Why the sorted table stays the production path: 27.5 µs at p95 is far below anything a person
can notice while typing, so the trie's speed buys nothing visible, while its start-up time and memory
are real costs. The table's slower tail is probably the 2-letter prefixes, which cover very large ranges
of the term table; that was not measured separately.

Limits: timings are per call with `steady_clock`, which adds a few tens of nanoseconds to every
sample, noticeable only for the trie; one machine, one index, warm only.
<!-- END b7 -->
