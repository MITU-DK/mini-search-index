# Benchmark notes

<!-- BEGIN b1 -->
## B1: indexing thread scaling (laptop, `main` profile, 150,000 docs)

Source: `benchmarks/results/laptop/b1_build-main.csv` (5 runs per thread count after one
discarded warm-up run, each thread count in a fresh process; commit `78dd832`).
The table shows the median of the 5 runs and the range of the total. Results are in-memory only: writing
the index file is not part of these times.

| Threads | Total (s) | Range | Speed-up | Tokenize (s) | Merge (s) | Merge share | Peak RSS (MiB) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 26.70 | 26.45 to 28.34 | 1.00x | 26.42 | 0.00 | 0% | 1,117 |
| 2 | 17.16 | 17.09 to 17.83 | 1.56x | 15.31 | 1.61 | 9% | 1,671 |
| 4 | 10.47 | 10.38 to 10.54 | 2.55x | 8.79 | 1.38 | 13% | 1,666 |
| 6 | 8.22 | 8.18 to 8.28 | 3.25x | 6.54 | 1.40 | 17% | 1,740 |
| 8 | 7.48 | 7.47 to 7.56 | 3.57x | 5.85 | 1.32 | 18% | 1,813 |
| 10 | 7.10 | 6.95 to 7.12 | 3.76x | 5.41 | 1.31 | 18% | 1,925 |
| 12 | 6.91 | 6.87 to 7.46 | 3.86x | 5.29 | 1.32 | 19% | 1,936 |
| 14 | 6.65 | 6.56 to 6.86 | 4.02x | 5.00 | 1.32 | 20% | 1,943 |
| 16 | 6.70 | 6.60 to 6.76 | 3.98x | 4.99 | 1.38 | 21% | 1,965 |
| 20 | 6.87 | 6.71 to 7.04 | 3.89x | 5.08 | 1.49 | 22% | 2,046 |

Measured, from the CSV:

- **The best total is 6.65 s at 14 threads, 4.02x faster than
  1 thread.** More threads than that were not faster: the total at 16, 20 threads is 1.01 to 1.03 times the best.
- **The merge phase does not scale.** From 2 threads up it takes 1.31 to
  1.61 s whatever the thread count, so its share of the total grows from
  9% at 2 threads to
  22% at 20. With the scan this fixed cost
  (1.65 s at 20 threads) caps the speed-up (Amdahl's law) at
  16x even if tokenizing were free; in practice the tokenize phase is the larger limit.
- **The tokenize phase alone reaches 5.3x**
  (26.4 s to 5.0 s at
  16 threads).
- **Memory use grows with threads**: 1.1 GiB at 1 thread,
  2.0 GiB at the most, because each worker
  keeps its own term maps until the merge.
<!-- END b1 -->

The machine is described in `benchmarks/results/laptop/machine.txt`: i7-12700H, 6 performance cores
with SMT (12 logical CPUs) and 8 efficiency cores (20 logical CPUs in all), plugged in, `performance`
power profile.

### What else is known about why the speed-up flattens

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
| Index file, raw postings | 705.4 MiB | 46.6% |
| Index file, gap + VByte postings | 236.0 MiB | 15.6% |

- 150,000 docs, 1,467,863 terms, 84,624,579 postings.
- Postings alone: 645.6 MiB raw (8 bytes each) against 176.2 MiB compressed, which is
  **3.66x smaller** and 2.18 bytes per posting.
- The whole file is 2.99x smaller, not 3.66x, because the rest of
  the file does not shrink: the file minus its postings is 59.8 MiB, of which the term
  table alone is 1,467,863 x 24 bytes = 33.6 MiB.
- The BK-tree for spell checking is 7.5 MiB of that file (3.2%). Building it took 0.7 s, done once when the index is written.

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
laptop --profile main`). The index is `data/index/main/vbyte.bin` (236 MiB, from B2).
Every measurement is a fresh process; one discarded round first, then 5 runs per cell. The table shows
medians and the min to max range of the two times. **Cold** means the file was evicted from the page
cache with `posix_fadvise(DONTNEED)` just before opening; **warm** means it was already cached. The
queries are the titles of 1000 random docs (seed 42), so "first query" is one fixed title, the same in
every run. The time shown for a query is the search itself; the previews (snippets) are not included.

| Cache | Mode | Open (ms) | Range | First query (ms) | Range | Open + first query (ms) | RSS after open (MiB) | RSS after 1000 queries (MiB) | Major faults, first query | Minor faults, first query |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| cold | parse | 1,366 | 1,347 to 1,426 | 0.821 | 0.807 to 0.904 | 1,366 | 816 | 819 | 0 | 313 |
| cold | read | 342 | 338 to 349 | 0.737 | 0.643 to 1.13 | 343 | 240 | 243 | 0 | 299 |
| cold | mmap | 0.842 | 0.554 to 1.29 | 20.8 | 19.6 to 23.8 | 21.6 | 4 | 127 | 32 | 310 |
| warm | parse | 1,139 | 1,125 to 1,169 | 0.824 | 0.813 to 0.854 | 1,140 | 816 | 819 | 0 | 313 |
| warm | read | 112 | 110 to 115 | 0.701 | 0.669 to 0.889 | 112 | 240 | 243 | 0 | 299 |
| warm | mmap | 0.016 | 0.012 to 0.017 | 1.17 | 0.750 to 1.18 | 1.18 | 4 | 133 | 0 | 339 |

### What the three modes show

- **`parse` is slow because of decoding, not disk.** Opening takes 1,139 ms warm and 1,366 ms
  cold. `read` also reads the whole file into memory and takes 112 ms warm, so decoding the
  postings into hash maps costs about **1,028 ms**, which is 75% of the
  cold `parse` time. The disk accounts for only about 226 ms (cold minus warm). `parse` also
  needs 812 MiB of memory, 3.4 times the size of the file, because
  hash maps and vectors take far more space than the packed bytes.
- **`read` is a plain copy.** It spends its 112 ms warm copying 236 MiB into a buffer
  and nothing decoding. Cold, the disk adds about 231 ms. Its memory is the size of the file.
- **`mmap` opens in 0.016 ms warm and 0.842
  ms cold**, because opening only maps the file and checks the header. Nothing is read until a query
  touches it, and the memory after opening is 4 MiB
  (only the program itself).
- **The cost moves to the first query.** Warm, the first `mmap` query takes 1.17 ms against
  0.824 ms for `parse`, which is 0.34 ms more. Mapped pages
  that are already in the page cache still have to be entered into this process's page table, one minor
  page fault each: 339 minor faults against
  313 for `parse` (26 more; the rest is setup
  that every mode shares). Cold, the first query takes **20.8 ms with
  32 major faults**, because those pages must come
  from disk. The `parse` and `read` first queries take under a millisecond, because everything is
  already in memory: they paid before the query, at open.
- **Open plus first query**, which is how long a user waits for the first answer from a new process:
  `mmap` 21.6 ms cold and 1.18 ms warm, `read`
  343 ms and 112 ms, `parse` 1,366
  ms and 1,140 ms.
- **After 1000 queries** `mmap` has 133 MiB resident,
  all of it file pages that the page cache shares with other processes, against
  819 MiB of private memory for `parse`. 1000 random
  title queries touch about 57% of the
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
| sorted table | 0 | 0 | 2.30 | 25.9 |
| trie | 364 | 216 | 0.586 | 1.25 |

- The trie answers **3.9x faster at the median and 21x faster at p95**.
- It has to be built from every term before the first query: 364 ms here, on
  every start of the program. That is the cost mmap was chosen to avoid (B3: the index opens in well
  under a millisecond).
- It needs 216 MiB of memory on top of the index, about 0.9 times the size of
  the 236 MiB index file itself, because it has one node per distinct prefix of every term.
- The table needs nothing extra: no build, no memory beyond the pages it touches.

Why the sorted table stays the production path: 25.9 µs at p95 is far below anything a person
can notice while typing, so the trie's speed buys nothing visible, while its start-up time and memory
are real costs. The table's slower tail is probably the 2-letter prefixes, which cover very large ranges
of the term table; that was not measured separately.

Limits: timings are per call with `steady_clock`, which adds a few tens of nanoseconds to every
sample, noticeable only for the trie; one machine, one index, warm only.
<!-- END b7 -->

<!-- BEGIN b6 -->
## B6: spell check, brute force vs BK-tree (laptop, `main` index)

Source: `benchmarks/results/laptop/b6_spell-main.csv` (`tools/run_benchmarks.sh b6 --machine
laptop --profile main`) and the typos in `benchmarks/queries/typos-main.tsv`. The typos are
200 seeded single edits (an insertion, deletion, substitution or swap of neighbours, one per
word) of words of 5 or more letters that occur in at least 50 docs, and a typo that is itself an indexed
word was not used. Each typo is asked 5 times after one unrecorded pass, so the times are warm;
percentiles are nearest-rank over the 1000 timings. Both methods use the same index and the
same candidate terms (those in at least 3 docs); the benchmark stops if they ever give different
answers, and they never did.

| Method | p50 (µs) | p95 (µs) | Accuracy | Edit distances computed per typo |
|---|---:|---:|---:|---:|
| brute force | 34,035 | 40,571 | 73.0% | 205,261 |
| BK-tree | 12,731 | 17,333 | 73.0% | 57,475 |

- The BK-tree answers **2.7x faster at the median and 2.3x faster at p95**. That
  is a real gain but a modest one, much smaller than the saving a BK-tree is often credited with.
  The likely reason is in the last column: for each typo the tree still computes
  57,475 edit distances, which is 15% of
  its 395,539 nodes, against 205,261 for brute force (which skips terms whose
  length is too different to be within range, a shortcut the tree does not need). The
  triangle inequality rules out a subtree only when the distance to its root is far from the edge label,
  and with a limit of 2 edits on words of 5 or more letters, most of a large vocabulary is "near enough"
  at some node to be worth looking into. Time per distance is 222 ns
  for the tree against 166 ns for brute force at the median: the
  tree pays somewhat more for each one, probably because it jumps around the file where brute force
  reads the term table in order (that was not measured separately), so the speed-up is smaller than
  the drop in the count.
- The tree is stored in the index file: 7.5 MiB, built in 0.7 s when the index is written (B2), so opening the index has nothing to build.
- Accuracy is the share of typos for which the suggestion was the word the typo was made from.
  It is the same for both methods because they give the same answers. It is below 100%, probably
  because the rule picks the closest term and then the commonest, and a different, commoner word can
  be as close as the original; that is how the rule works, not a fault in the tree. It was not
  broken down by kind of edit, so this is an explanation and not a measurement.

Limits: one index, one machine, warm only; typos made by random single edits are easier than real
human typos, which often have two or more errors; accuracy is measured against the word the typo was
made from, which is not always the word a person meant.
<!-- END b6 -->

<!-- BEGIN b4 -->
## B4: query latency and ranking quality (laptop, `main` index)

Source: `benchmarks/results/laptop/b4_query-main.csv` and `b4_quality-main.csv`
(`tools/run_benchmarks.sh b4 --machine laptop --profile main`). The queries are the titles of
1000 random docs (seed 42, in `benchmarks/queries/known_item-main.tsv`), and the answer to each is
the doc the title came from ("known-item" search). Top 10 results, no spell correction in the quality
run. Titles are indexed, so each query is a title that the answer contains: this makes the task easier
than real search, but it is a fair way to compare two rankings.

### Latency

Times are for one `search()` call in milliseconds, except `bm25+snippets`, which also reads the
corpus and builds the preview of each of the 10 hits, as the command line does. **Warm**: the index
and the corpus are in the page cache; one unrecorded pass, then 5 recorded passes (5000 timings per
row). **Cold**: the index file was evicted just before the process opened it, one pass of 1000 queries,
one fresh process per mode; it is not repeated, so a cold row is one run. The last column counts major
page faults during the recorded queries (0 warm; above 0 cold, which is how the eviction is checked).
Percentiles are nearest-rank.

| Cache | Mode | p50 (ms) | p95 (ms) | p99 (ms) | mean (ms) | max (ms) | Major faults |
|---|---|---:|---:|---:|---:|---:|---:|
| warm | bm25 | 0.097 | 2.14 | 3.93 | 0.471 | 6.76 | 0 |
| warm | tfidf | 0.128 | 2.63 | 4.80 | 0.614 | 9.36 | 0 |
| warm | bm25+snippets | 0.518 | 2.57 | 4.47 | 0.811 | 7.46 | 0 |
| cold | bm25 | 0.455 | 3.37 | 7.52 | 0.941 | 18.5 | 1371 |
| cold | tfidf | 0.472 | 3.46 | 7.08 | 0.964 | 15.3 | 1371 |

- Warm BM25: **p50 0.097 ms, p95 2.14 ms, p99
  3.93 ms**. The mean (0.471 ms) is 4.8
  times the median, so a minority of queries are much slower than the typical one; the likely cause is
  titles with a common word, whose postings are long (not broken down by query).
- Cold, the first pass over the index costs more: p50 0.455 ms and p99
  7.52 ms, because each new page of postings is read from disk
  (1371 major faults over the pass).
- Previews add 0.421 ms at the median (p50 0.518 ms
  against 0.097 ms), the cost of reading ten lines from the corpus.
- TF-IDF is a little slower than BM25 here. That is probably this implementation (it calls `log` for
  every posting where BM25 does plain arithmetic) and not a property of the ranking; it was not
  investigated.

### Quality

| Ranking | MRR@10 | Hit@1 | Hit@10 | Mean length of the doc ranked first (tokens) |
|---|---:|---:|---:|---:|
| BM25 | 0.890 | 83.4% | 97.9% | 1,677 |
| TF-IDF | 0.594 | 49.1% | 80.7% | 4,915 |

The average length of the target docs is 1,669 tokens. BM25's first
result is about as long (1,677), TF-IDF's is
2.9 times as long (4,915).

**Why BM25 beats TF-IDF here.** The TF-IDF score used here, the sum of (1 + ln tf) x ln(N/df), has no
document length in it: a long article has more chances to contain each query word, and so scores
higher than a short one that is about the title. BM25 divides the term frequency by a factor that
grows with the length of the doc (b = 0.75), which cancels most of that advantage. The measurement
above fits this: the docs that TF-IDF puts first are 2.9 times longer than the targets, while
BM25's are 1.00 times as long. This explains the direction and size of the gap; it was not tested by
adding length normalisation to TF-IDF, which would be the direct check.

What the BM25 misses look like (listed once by hand, not recorded in the CSV): mostly one- or
two-word titles of ambiguous names, such as a bare first name or surname, where thousands of other docs
contain the same word more often than the short target doc does. None of them is a title shared with
another doc.

Limits: one index, one machine; known-item queries made from titles (the doc always contains every
query word, which real queries do not guarantee); a title shared by two docs makes only one of them
the right answer; cold rows are single runs.
<!-- END b4 -->

<!-- BEGIN b5 -->
## B5: grep against the index (laptop, `main` index)

Source: `benchmarks/results/laptop/b5_grep-main.csv` and `b5_breakeven-main.csv`
(`tools/run_benchmarks.sh b5 --machine laptop --profile main`, which runs `tools/bench_grep.sh`).
20 single words picked by seed from three bands of document frequency (`benchmarks/queries/grep_terms-main.txt`):
5 rare (10 to 50 docs), 10 medium (0.1% to 1% of the docs) and 5 very common (over 20%).
grep is `LC_ALL=C grep -cwiF` over every part file, with the counts summed (one doc is one line);
the index side is `msearch query -i index.bin --count-only <word>`. Both are whole processes timed
the same way, 5 runs after one unrecorded run, median; the corpus and the index were in the page cache.

| Word | Docs (index) | Docs (grep) | grep (ms) | Index (ms) | Speed-up |
|---|---:|---:|---:|---:|---:|
| `ophrys` | 13 | 13 | 1,126 | 4.53 | 249x |
| `squats` | 42 | 42 | 1,240 | 4.32 | 287x |
| `glossop` | 48 | 48 | 843 | 4.33 | 195x |
| `mutapa` | 11 | 11 | 1,289 | 4.48 | 288x |
| `buttigieg` | 17 | 17 | 780 | 4.58 | 170x |
| `undisturbed` | 440 | 440 | 875 | 4.64 | 189x |
| `coarser` | 185 | 185 | 1,244 | 4.58 | 272x |
| `sorrow` | 659 | 659 | 861 | 4.47 | 192x |
| `bedrock` | 660 | 660 | 517 | 4.58 | 113x |
| `bonner` | 170 | 170 | 1,314 | 4.54 | 289x |
| `births` | 1,179 | 1,180 | 1,187 | 4.43 | 268x |
| `outweighed` | 211 | 211 | 860 | 4.43 | 194x |
| `procession` | 1,422 | 1,423 | 993 | 4.31 | 230x |
| `fender` | 383 | 383 | 1,519 | 4.34 | 350x |
| `administering` | 768 | 768 | 736 | 4.55 | 162x |
| `husband` | 40,003 | 40,019 | 1,349 | 4.16 | 324x |
| `established` | 40,015 | 40,032 | 1,036 | 4.59 | 226x |
| `either` | 30,299 | 30,368 | 1,628 | 4.42 | 368x |
| `several` | 61,047 | 61,056 | 1,271 | 4.65 | 274x |
| `central` | 33,823 | 33,857 | 1,312 | 4.31 | 305x |

- grep takes 517 to 1,628 ms per word, because it reads the whole
  corpus every time, and the time does not clearly follow how common the word is. The index takes
  4.16 to 4.65 ms for every word, which suggests that it is mostly the
  start of the process and the opening of the file. The speed-up is **113x to
  368x**, median 258x.
- **Break-even.** Building the index takes 6.6 s at the fastest thread
  count in B1 (14 threads). A grep costs 1,099 ms on
  average and an indexed query 4.46 ms, so the build is paid back after
  **6.1 queries**. This compares a count of one word; a ranked
  search does more work than a count, so the real figure is a little higher.
- **The counts.** grep and the index agree on 13 of 20 words. The
  7 that differ are `births` (1 more), `procession` (1 more), `husband` (16 more), `established` (17 more), `either` (69 more), `several` (9 more), `central` (34 more): grep finds a few more docs in every case. The reason is
  the rule for what a word is. `grep -w` in the C locale treats bytes of 0x80 and above (any
  non-ASCII character) as separators, so it finds `births` in "births–deaths" with an en dash; the
  tokenizer treats those bytes as part of a word, so for the index that word is `births–deaths` and not
  `births`. (`grep -w` also treats `_` as a word character, which goes the other way.) The last column
  of the CSV counts the docs with a regular expression that follows the tokenizer's rule, and it equals the
  index's count for all 20 words, so the difference is explained by those rules and
  nothing else.

Limits: single-word counts only (grep cannot rank); one machine; the timings include process start
for both sides; grep's times differ between words more than the identical amount of data would
suggest (517 to 1,628 ms) and the reason was not investigated: the median of 5 runs
was taken for each word, but different words were measured minutes apart.
<!-- END b5 -->

<!-- BEGIN b8 -->
## B8: bounded-memory build (laptop, `main` profile, 150,000 docs)

Source: `benchmarks/results/laptop/b8_full-main.csv` (`tools/run_benchmarks.sh b8 --machine laptop
--profile main`; commit `499593d`). Every row of the CSV is one build with
`msearch index --memory-budget`'s code path (`build_to_file`) in a fresh process, so the peak RSS (the
process's high-water mark, `VmHWM`) is that build's alone; there are 3 builds per row below, and the table
shows the median and the range of the total. No warm-up run and no cache eviction: the corpus was most likely
in the page cache. The run files and the index were written to the same SSD that the corpus is on. Output
goes to a file next to the existing in-memory index (B2), and the program compares the two byte for byte.

| Threads | Budget (MiB) | Total (s) | Range | Tokenize (s) | Merge (s) | Write (s) | Peak RSS (MiB) | Peak / budget | Runs | Spilled (MiB) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 | 256 | 13.82 | 13.59 to 14.03 | 9.43 | 3.40 | 0.84 | 267 | 104% | 48 | 743 |
| 4 | 512 | 13.14 | 13.11 to 13.27 | 9.23 | 2.83 | 0.96 | 484 | 95% | 16 | 709 |
| 4 | 1,024 | 12.79 | 12.73 to 16.16 | 9.10 | 2.58 | 0.93 | 993 | 97% | 8 | 694 |
| 20 | 256 | 12.22 | 12.14 to 12.25 | 5.80 | 5.35 | 0.89 | 511 | 200% | 291 | 845 |
| 20 | 512 | 11.28 | 11.18 to 11.39 | 5.78 | 4.44 | 0.90 | 661 | 129% | 143 | 795 |
| 20 | 1,024 | 10.37 | 10.35 to 10.46 | 5.68 | 3.60 | 0.90 | 867 | 85% | 60 | 751 |

Measured, from the CSV:

- **The file is the same.** 18 of 18 builds gave an index that is byte-identical to the
  in-memory one built by B2, whatever the budget, the thread count and the number of runs.
- **The peak is within the budget in 3 of 6 settings.** Over it: 4 threads, 256 MiB: 104%; 20 threads, 256 MiB: 200%; 20 threads, 512 MiB: 129%.
  The budget pays first for the document table and for memory that each thread needs whatever it holds
  (the batch it is reading, the tokenizer's scratch space, the allocator's slack); what is left is shared by the
  workers. When that fixed part is larger than the budget there is nothing to share, the workers are given half
  the budget, and the process is over it by the rest. So a budget has a lower limit that grows with the thread
  count and the number of documents, and below it more threads do not help. This is how the code works, and the
  CSV shows it; the size of each fixed part was estimated from exploratory runs that are not in the CSV.
- **What it costs, in time.** Compared with the in-memory build at the same thread count, with the largest
  budget tried:
  - 4 threads: in memory (B1) 10.47 s in total, tokenize 8.79 s, merge 1.38 s; with a 1,024 MiB budget 12.79 s (1.22 times), tokenize 9.10 s, merge 2.58 s.
  - 20 threads: in memory (B1) 6.87 s in total, tokenize 5.08 s, merge 1.49 s; with a 1,024 MiB budget 10.37 s (1.51 times), tokenize 5.68 s, merge 3.60 s.
  The budgeted build's tokenize phase includes writing the runs, which is probably why it is a little longer.
  Its merge is longer because one thread merges all the runs, where the in-memory merge works in parallel on
  shards of the terms, and it takes longer when there are more runs (the table shows it for the smaller budgets).

Limits: one machine; two thread counts and three budgets; 3 builds per setting; the corpus was
probably cached; a budget smaller than the fixed part is not honoured; the fixed parts were set from `main` and
are checked again on `full` only if B8 is run there.
<!-- END b8 -->
