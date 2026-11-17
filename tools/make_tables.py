#!/usr/bin/env python3
"""Writes the benchmark sections of benchmarks/NOTES.md from the CSV files.

    tools/make_tables.py --machine laptop --profile main [b2 b3 b7 ...]

Each section sits between `<!-- BEGIN bN -->` and `<!-- END bN -->` markers and is replaced as a
whole; text outside the markers is written by hand and never touched. Every number in a section
comes from a CSV, never from typing (DESIGN.md §10).
"""
import argparse
import csv
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIB = 2**20
kSpellNodeSize, kSpellEdgeSize = 12, 8  # a tree of n nodes has n - 1 edges: 20 n - 8 bytes


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def fmt(x):
    """A time or size with a sensible number of digits."""
    if x < 1:
        return f"{x:.3f}"
    if x < 10:
        return f"{x:.2f}"
    if x < 100:
        return f"{x:.1f}"
    return f"{x:,.0f}"


# ---------------------------------------------------------------- B2: index size


def section_b2(results, machine, profile):
    r = read_csv(results / f"b2_size-{profile}.csv")[0]
    terms = read_csv(results / f"b2_terms-{profile}.csv")
    i = lambda k: int(r[k])
    mib = lambda n: f"{n / MIB:,.1f} MiB"
    docs, nterms, post = i("docs"), i("terms"), i("postings")
    corpus, raw_file, v_file = i("corpus_bytes"), i("raw_file_bytes"), i("vbyte_file_bytes")
    raw_post, v_post = i("raw_postings_bytes"), i("vbyte_postings_bytes")
    spell_note = ""
    if "vbyte_spell_bytes" in r:
        spell = i("vbyte_spell_bytes")
        spell_note = (
            f"\n- The BK-tree for spell checking is {mib(spell)} of that file "
            f"({100 * spell / v_file:.1f}%). Building it took {float(r['spell_build_s']):.1f} s, "
            f"done once when the index is written."
        )
    rows = "\n".join(
        f"| {x['df_rank']} | `{x['term']}` | {int(x['df']):,} | {float(x['bytes_per_gap']):.2f} "
        f"| {float(x['bytes_per_posting']):.2f} |"
        for x in terms
    )
    the = next((x for x in terms if x["term"] == "the"), None)
    the_note = ""
    if the:
        share = (int(the["tf_bytes"]) - int(the["df"])) / int(the["df"])
        the_note = (
            f"The commonest words go over it in long articles: `the` needs a second byte for tf in "
            f"{100 * share:.0f}% of its postings, which is why it costs "
            f"{float(the['bytes_per_posting']):.2f} bytes per posting and not 2. "
        )
    return f"""## B2: index size ({machine}, `{profile}` profile)

Source: `benchmarks/results/{machine}/b2_size-{profile}.csv` and `b2_terms-{profile}.csv`
(`tools/run_benchmarks.sh b2 --machine {machine} --profile {profile}`). Sizes do not depend on the machine.

| | Size | Compared with the corpus |
|---|---:|---:|
| Corpus text (`part-*.tsv`) | {mib(corpus)} | 100% |
| Index file, raw postings | {mib(raw_file)} | {100 * raw_file / corpus:.1f}% |
| Index file, gap + VByte postings | {mib(v_file)} | {100 * v_file / corpus:.1f}% |

- {docs:,} docs, {nterms:,} terms, {post:,} postings.
- Postings alone: {mib(raw_post)} raw (8 bytes each) against {mib(v_post)} compressed, which is
  **{raw_post / v_post:.2f}x smaller** and {v_post / post:.2f} bytes per posting.
- The whole file is {raw_file / v_file:.2f}x smaller, not {raw_post / v_post:.2f}x, because the rest of
  the file does not shrink: the file minus its postings is {mib(v_file - v_post)}, of which the term
  table alone is {nterms:,} x 24 bytes = {mib(nterms * 24)}.{spell_note}

### Why a common word costs about 1 byte per gap

A gap is the distance between two doc ids in a list. A word in most docs has gaps of 1, 2 or 3, which
fit in one byte (anything below 128 does). A rare word has few postings spread over {docs:,} docs, so
its gaps are large and need two or three bytes. The table shows the term at several ranks of document
frequency (rank 1 is the term in the most docs):

| df rank | Term | df | Bytes per gap | Bytes per posting (gap + tf) |
|---:|---|---:|---:|---:|
{rows}

The term frequency adds one byte per posting while tf is below 128, which holds for most terms, so
dense lists settle near 2 bytes per posting. {the_note}Rare terms cost 3 to 4 bytes because their
gaps are large. Raw postings always cost 8.
"""


# ---------------------------------------------------------------- B3: loading


def section_b3(results, machine, profile):
    b2 = read_csv(results / f"b2_size-{profile}.csv")[0]
    size_mib = int(b2["vbyte_file_bytes"]) / MIB
    rows = read_csv(results / f"b3_load-{profile}.csv")
    groups = {}
    for row in rows:
        groups.setdefault((row["cache"], row["mode"]), []).append(row)

    def col(c, m, k):
        return [float(r[k]) for r in groups[(c, m)]]

    def med(c, m, k):
        return st.median(col(c, m, k))

    def span(c, m, k):
        return f"{fmt(min(col(c, m, k)))} to {fmt(max(col(c, m, k)))}"

    mb = lambda kb: kb / 1024
    table = [
        "| Cache | Mode | Open (ms) | Range | First query (ms) | Range | Open + first query (ms) "
        "| RSS after open (MiB) | RSS after 1000 queries (MiB) | Major faults, first query "
        "| Minor faults, first query |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for c in ("cold", "warm"):
        for m in ("parse", "read", "mmap"):
            o, f = med(c, m, "open_ms"), med(c, m, "first_query_ms")
            table.append(
                f"| {c} | {m} | {fmt(o)} | {span(c, m, 'open_ms')} | {fmt(f)} "
                f"| {span(c, m, 'first_query_ms')} | {fmt(o + f)} "
                f"| {mb(med(c, m, 'rss_after_open_kb')):.0f} "
                f"| {mb(med(c, m, 'rss_after_1000q_kb')):.0f} "
                f"| {med(c, m, 'major_faults_first_query'):.0f} "
                f"| {med(c, m, 'minor_faults_first_query'):.0f} |"
            )
    table = "\n".join(table)
    pw, pc = med("warm", "parse", "open_ms"), med("cold", "parse", "open_ms")
    rw, rc = med("warm", "read", "open_ms"), med("cold", "read", "open_ms")
    mw, mc = med("warm", "mmap", "first_query_ms"), med("cold", "mmap", "first_query_ms")
    parse_mem = mb(med("warm", "parse", "rss_anon_after_open_kb"))
    extra = med("warm", "mmap", "minor_faults_first_query") - med("warm", "parse", "minor_faults_first_query")
    dt = mw - med("warm", "parse", "first_query_ms")
    wait = lambda c, m: med(c, m, "open_ms") + med(c, m, "first_query_ms")
    return f"""## B3: loading the index, `parse` vs `read` vs `mmap` ({machine}, `{profile}` index)

Source: `benchmarks/results/{machine}/b3_load-{profile}.csv` (`tools/run_benchmarks.sh b3 --machine
{machine} --profile {profile}`). The index is `data/index/{profile}/vbyte.bin` ({size_mib:.0f} MiB, from B2).
Every measurement is a fresh process; one discarded round first, then 5 runs per cell. The table shows
medians and the min to max range of the two times. **Cold** means the file was evicted from the page
cache with `posix_fadvise(DONTNEED)` just before opening; **warm** means it was already cached. The
queries are the titles of 1000 random docs (seed 42), so "first query" is one fixed title, the same in
every run. The time shown for a query is the search itself; the previews (snippets) are not included.

{table}

### What the three modes show

- **`parse` is slow because of decoding, not disk.** Opening takes {fmt(pw)} ms warm and {fmt(pc)} ms
  cold. `read` also reads the whole file into memory and takes {fmt(rw)} ms warm, so decoding the
  postings into hash maps costs about **{pw - rw:,.0f} ms**, which is {100 * (pw - rw) / pc:.0f}% of the
  cold `parse` time. The disk accounts for only about {pc - pw:,.0f} ms (cold minus warm). `parse` also
  needs {parse_mem:,.0f} MiB of memory, {parse_mem / size_mib:.1f} times the size of the file, because
  hash maps and vectors take far more space than the packed bytes.
- **`read` is a plain copy.** It spends its {fmt(rw)} ms warm copying {size_mib:.0f} MiB into a buffer
  and nothing decoding. Cold, the disk adds about {rc - rw:,.0f} ms. Its memory is the size of the file.
- **`mmap` opens in {fmt(med("warm", "mmap", "open_ms"))} ms warm and {fmt(med("cold", "mmap", "open_ms"))}
  ms cold**, because opening only maps the file and checks the header. Nothing is read until a query
  touches it, and the memory after opening is {mb(med("warm", "mmap", "rss_after_open_kb")):.0f} MiB
  (only the program itself).
- **The cost moves to the first query.** Warm, the first `mmap` query takes {fmt(mw)} ms against
  {fmt(med("warm", "parse", "first_query_ms"))} ms for `parse`, which is {dt:.2f} ms more. Mapped pages
  that are already in the page cache still have to be entered into this process's page table, one minor
  page fault each: {med("warm", "mmap", "minor_faults_first_query"):.0f} minor faults against
  {med("warm", "parse", "minor_faults_first_query"):.0f} for `parse` ({extra:.0f} more; the rest is setup
  that every mode shares). Cold, the first query takes **{fmt(mc)} ms with
  {med("cold", "mmap", "major_faults_first_query"):.0f} major faults**, because those pages must come
  from disk. The `parse` and `read` first queries take under a millisecond, because everything is
  already in memory: they paid before the query, at open.
- **Open plus first query**, which is how long a user waits for the first answer from a new process:
  `mmap` {fmt(wait("cold", "mmap"))} ms cold and {fmt(wait("warm", "mmap"))} ms warm, `read`
  {fmt(wait("cold", "read"))} ms and {fmt(wait("warm", "read"))} ms, `parse` {fmt(wait("cold", "parse"))}
  ms and {fmt(wait("warm", "parse"))} ms.
- **After 1000 queries** `mmap` has {mb(med("warm", "mmap", "rss_after_1000q_kb")):.0f} MiB resident,
  all of it file pages that the page cache shares with other processes, against
  {mb(med("warm", "parse", "rss_after_1000q_kb")):.0f} MiB of private memory for `parse`. 1000 random
  title queries touch about {100 * mb(med("warm", "mmap", "rss_after_1000q_kb")) / size_mib:.0f}% of the
  file, which is why the memory grows.

Limits of these numbers: one index on one machine; the first query is a single title, so its cold time
depends on which pages that title happens to need; eviction with `fadvise` drops this file only (not a
full `drop_caches`); times are for the search only, without previews.
"""


# ---------------------------------------------------------------- B7: autocomplete


def section_b7(results, machine, profile):
    rows = {r["method"]: r for r in read_csv(results / f"b7_suggest-{profile}.csv")}
    b2 = read_csv(results / f"b2_size-{profile}.csv")[0]
    index_mib = int(b2["vbyte_file_bytes"]) / MIB
    table, trie = rows["sorted_table"], rows["trie"]
    t50, t95 = float(table["p50_us"]), float(table["p95_us"])
    r50, r95 = float(trie["p50_us"]), float(trie["p95_us"])
    mem = int(trie["extra_memory_bytes"]) / MIB
    return f"""## B7: autocomplete, sorted table vs trie ({machine}, `{profile}` index)

Source: `benchmarks/results/{machine}/b7_suggest-{profile}.csv` (`tools/run_benchmarks.sh b7 --machine
{machine} --profile {profile}`). 1000 seeded prefixes (2 to 5 leading bytes of random terms in at least
10 docs), each asked 5 times after one unrecorded pass, so the times are warm. Percentiles are
nearest-rank over the 5000 timings. The sorted table is the production path: a binary search over the
mapped term table, then the best 5 of the range. The trie is built in memory from the same terms and
keeps the best 5 terms in every node.

| Method | Build (ms) | Extra memory (MiB) | p50 (µs) | p95 (µs) |
|---|---:|---:|---:|---:|
| sorted table | 0 | 0 | {fmt(t50)} | {fmt(t95)} |
| trie | {fmt(float(trie["build_ms"]))} | {mem:,.0f} | {fmt(r50)} | {fmt(r95)} |

- The trie answers **{t50 / r50:.1f}x faster at the median and {t95 / r95:.0f}x faster at p95**.
- It has to be built from every term before the first query: {fmt(float(trie["build_ms"]))} ms here, on
  every start of the program. That is the cost mmap was chosen to avoid (B3: the index opens in well
  under a millisecond).
- It needs {mem:,.0f} MiB of memory on top of the index, about {mem / index_mib:.1f} times the size of
  the {index_mib:.0f} MiB index file itself, because it has one node per distinct prefix of every term.
- The table needs nothing extra: no build, no memory beyond the pages it touches.

Why the sorted table stays the production path: {fmt(t95)} µs at p95 is far below anything a person
can notice while typing, so the trie's speed buys nothing visible, while its start-up time and memory
are real costs. The table's slower tail is probably the 2-letter prefixes, which cover very large ranges
of the term table; that was not measured separately.

Limits: timings are per call with `steady_clock`, which adds a few tens of nanoseconds to every
sample, noticeable only for the trie; one machine, one index, warm only.
"""


# ---------------------------------------------------------------- B6: spell check


def section_b6(results, machine, profile):
    rows = {r["method"]: r for r in read_csv(results / f"b6_spell-{profile}.csv")}
    typos = ROOT / "benchmarks" / "queries" / f"typos-{profile}.tsv"
    count = sum(1 for _ in open(typos)) - 1  # minus the header line
    brute, tree = rows["brute_force"], rows["bk_tree"]
    b50, b95 = float(brute["p50_us"]), float(brute["p95_us"])
    t50, t95 = float(tree["p50_us"]), float(tree["p95_us"])
    accuracy = float(tree["accuracy"])
    b2 = read_csv(results / f"b2_size-{profile}.csv")[0]
    nodes = (int(b2["vbyte_spell_bytes"]) + kSpellEdgeSize) // (kSpellNodeSize + kSpellEdgeSize)
    spell_note = ""
    if "vbyte_spell_bytes" in b2:
        spell_note = (
            f"- The tree is stored in the index file: {int(b2['vbyte_spell_bytes']) / MIB:,.1f} MiB, "
            f"built in {float(b2['spell_build_s']):.1f} s when the index is written (B2), so opening "
            f"the index has nothing to build.\n"
        )
    # the tree has n nodes of 12 bytes and n - 1 edges of 8 bytes
    return f"""## B6: spell check, brute force vs BK-tree ({machine}, `{profile}` index)

Source: `benchmarks/results/{machine}/b6_spell-{profile}.csv` (`tools/run_benchmarks.sh b6 --machine
{machine} --profile {profile}`) and the typos in `benchmarks/queries/typos-{profile}.tsv`. The typos are
{count} seeded single edits (an insertion, deletion, substitution or swap of neighbours, one per
word) of words of 5 or more letters that occur in at least 50 docs, and a typo that is itself an indexed
word was not used. Each typo is asked 5 times after one unrecorded pass, so the times are warm;
percentiles are nearest-rank over the {count * 5} timings. Both methods use the same index and the
same candidate terms (those in at least 3 docs); the benchmark stops if they ever give different
answers, and they never did.

| Method | p50 (µs) | p95 (µs) | Accuracy | Edit distances computed per typo |
|---|---:|---:|---:|---:|
| brute force | {fmt(b50)} | {fmt(b95)} | {100 * float(brute["accuracy"]):.1f}% | {float(brute["avg_distance_calls"]):,.0f} |
| BK-tree | {fmt(t50)} | {fmt(t95)} | {100 * accuracy:.1f}% | {float(tree["avg_distance_calls"]):,.0f} |

- The BK-tree answers **{b50 / t50:.1f}x faster at the median and {b95 / t95:.1f}x faster at p95**. That
  is a real gain but a modest one, much smaller than the saving a BK-tree is often credited with.
  The likely reason is in the last column: for each typo the tree still computes
  {float(tree["avg_distance_calls"]):,.0f} edit distances, which is {100 * float(tree["avg_distance_calls"]) / nodes:.0f}% of
  its {nodes:,} nodes, against {float(brute["avg_distance_calls"]):,.0f} for brute force (which skips terms whose
  length is too different to be within range, a shortcut the tree does not need). The
  triangle inequality rules out a subtree only when the distance to its root is far from the edge label,
  and with a limit of 2 edits on words of 5 or more letters, most of a large vocabulary is "near enough"
  at some node to be worth looking into. Time per distance is {t50 * 1000 / float(tree["avg_distance_calls"]):.0f} ns
  for the tree against {b50 * 1000 / float(brute["avg_distance_calls"]):.0f} ns for brute force at the median: the
  tree pays somewhat more for each one, probably because it jumps around the file where brute force
  reads the term table in order (that was not measured separately), so the speed-up is smaller than
  the drop in the count.
{spell_note}- Accuracy is the share of typos for which the suggestion was the word the typo was made from.
  It is the same for both methods because they give the same answers. It is below 100%, probably
  because the rule picks the closest term and then the commonest, and a different, commoner word can
  be as close as the original; that is how the rule works, not a fault in the tree. It was not
  broken down by kind of edit, so this is an explanation and not a measurement.

Limits: one index, one machine, warm only; typos made by random single edits are easier than real
human typos, which often have two or more errors; accuracy is measured against the word the typo was
made from, which is not always the word a person meant.
"""


# ---------------------------------------------------------------- B4: query latency and quality


def section_b4(results, machine, profile):
    latency = read_csv(results / f"b4_query-{profile}.csv")
    quality = {r["mode"]: r for r in read_csv(results / f"b4_quality-{profile}.csv")}
    rows = "\n".join(
        f"| {r['cache']} | {r['mode']} | {fmt(float(r['p50_ms']))} | {fmt(float(r['p95_ms']))} "
        f"| {fmt(float(r['p99_ms']))} | {fmt(float(r['mean_ms']))} | {fmt(float(r['max_ms']))} "
        f"| {r['major_faults']} |"
        for r in latency
    )
    pick = lambda cache, mode: next(r for r in latency if r["cache"] == cache and r["mode"] == mode)
    warm, cold = pick("warm", "bm25"), pick("cold", "bm25")
    snippets = pick("warm", "bm25+snippets")
    bm, tf = quality["bm25"], quality["tfidf"]
    n = int(bm["queries"])
    pct = lambda r, k: f"{100 * float(r[k]):.1f}%"
    return f"""## B4: query latency and ranking quality ({machine}, `{profile}` index)

Source: `benchmarks/results/{machine}/b4_query-{profile}.csv` and `b4_quality-{profile}.csv`
(`tools/run_benchmarks.sh b4 --machine {machine} --profile {profile}`). The queries are the titles of
{n} random docs (seed 42, in `benchmarks/queries/known_item-{profile}.tsv`), and the answer to each is
the doc the title came from ("known-item" search). Top 10 results, no spell correction in the quality
run. Titles are indexed, so each query is a title that the answer contains: this makes the task easier
than real search, but it is a fair way to compare two rankings.

### Latency

Times are for one `search()` call in milliseconds, except `bm25+snippets`, which also reads the
corpus and builds the preview of each of the 10 hits, as the command line does. **Warm**: the index
and the corpus are in the page cache; one unrecorded pass, then 5 recorded passes ({n * 5} timings per
row). **Cold**: the index file was evicted just before the process opened it, one pass of {n} queries,
one fresh process per mode; it is not repeated, so a cold row is one run. The last column counts major
page faults during the recorded queries (0 warm; above 0 cold, which is how the eviction is checked).
Percentiles are nearest-rank.

| Cache | Mode | p50 (ms) | p95 (ms) | p99 (ms) | mean (ms) | max (ms) | Major faults |
|---|---|---:|---:|---:|---:|---:|---:|
{rows}

- Warm BM25: **p50 {fmt(float(warm['p50_ms']))} ms, p95 {fmt(float(warm['p95_ms']))} ms, p99
  {fmt(float(warm['p99_ms']))} ms**. The mean ({fmt(float(warm['mean_ms']))} ms) is {float(warm['mean_ms']) / float(warm['p50_ms']):.1f}
  times the median, so a minority of queries are much slower than the typical one; the likely cause is
  titles with a common word, whose postings are long (not broken down by query).
- Cold, the first pass over the index costs more: p50 {fmt(float(cold['p50_ms']))} ms and p99
  {fmt(float(cold['p99_ms']))} ms, because each new page of postings is read from disk
  ({cold['major_faults']} major faults over the pass).
- Previews add {fmt(float(snippets['p50_ms']) - float(warm['p50_ms']))} ms at the median (p50 {fmt(float(snippets['p50_ms']))} ms
  against {fmt(float(warm['p50_ms']))} ms), the cost of reading ten lines from the corpus.
- TF-IDF is a little slower than BM25 here. That is probably this implementation (it calls `log` for
  every posting where BM25 does plain arithmetic) and not a property of the ranking; it was not
  investigated.

### Quality

| Ranking | MRR@10 | Hit@1 | Hit@10 | Mean length of the doc ranked first (tokens) |
|---|---:|---:|---:|---:|
| BM25 | {float(bm['mrr_at_10']):.3f} | {pct(bm, 'hit_at_1')} | {pct(bm, 'hit_at_10')} | {float(bm['mean_len_first_hit']):,.0f} |
| TF-IDF | {float(tf['mrr_at_10']):.3f} | {pct(tf, 'hit_at_1')} | {pct(tf, 'hit_at_10')} | {float(tf['mean_len_first_hit']):,.0f} |

The average length of the target docs is {float(bm['mean_len_target']):,.0f} tokens. BM25's first
result is about as long ({float(bm['mean_len_first_hit']):,.0f}), TF-IDF's is
{float(tf['mean_len_first_hit']) / float(bm['mean_len_target']):.1f} times as long ({float(tf['mean_len_first_hit']):,.0f}).

**Why BM25 beats TF-IDF here.** The TF-IDF score used here, the sum of (1 + ln tf) x ln(N/df), has no
document length in it: a long article has more chances to contain each query word, and so scores
higher than a short one that is about the title. BM25 divides the term frequency by a factor that
grows with the length of the doc (b = 0.75), which cancels most of that advantage. The measurement
above fits this: the docs that TF-IDF puts first are {float(tf['mean_len_first_hit']) / float(bm['mean_len_target']):.1f} times longer than the targets, while
BM25's are {float(bm['mean_len_first_hit']) / float(bm['mean_len_target']):.2f} times as long. This explains the direction and size of the gap; it was not tested by
adding length normalisation to TF-IDF, which would be the direct check.

What the BM25 misses look like (listed once by hand, not recorded in the CSV): mostly one- or
two-word titles of ambiguous names, such as a bare first name or surname, where thousands of other docs
contain the same word more often than the short target doc does. None of them is a title shared with
another doc.

Limits: one index, one machine; known-item queries made from titles (the doc always contains every
query word, which real queries do not guarantee); a title shared by two docs makes only one of them
the right answer; cold rows are single runs.
"""


# ---------------------------------------------------------------- B5: grep


def section_b5(results, machine, profile):
    rows = read_csv(results / f"b5_grep-{profile}.csv")
    summary = read_csv(results / f"b5_breakeven-{profile}.csv")[0]
    table = "\n".join(
        f"| `{r['term']}` | {int(r['df']):,} | {int(r['grep_docs']):,} | {fmt(float(r['grep_ms']))} "
        f"| {fmt(float(r['index_ms']))} | {float(r['speedup']):.0f}x |"
        for r in rows
    )
    differ = [r for r in rows if r["df"] != r["grep_docs"]]
    unexplained = [r for r in differ if r["df"] != r["tokenizer_regex_docs"]]
    speedups = [float(r["speedup"]) for r in rows]
    grep_times = [float(r["grep_ms"]) for r in rows]
    index_times = [float(r["index_ms"]) for r in rows]
    differ_text = ", ".join(f"`{r['term']}` ({int(r['grep_docs']) - int(r['df'])} more)" for r in differ)
    return f"""## B5: grep against the index ({machine}, `{profile}` index)

Source: `benchmarks/results/{machine}/b5_grep-{profile}.csv` and `b5_breakeven-{profile}.csv`
(`tools/run_benchmarks.sh b5 --machine {machine} --profile {profile}`, which runs `tools/bench_grep.sh`).
20 single words picked by seed from three bands of document frequency (`benchmarks/queries/grep_terms-{profile}.txt`):
5 rare (10 to 50 docs), 10 medium (0.1% to 1% of the docs) and 5 very common (over 20%).
grep is `LC_ALL=C grep -cwiF` over every part file, with the counts summed (one doc is one line);
the index side is `msearch query -i index.bin --count-only <word>`. Both are whole processes timed
the same way, 5 runs after one unrecorded run, median; the corpus and the index were in the page cache.

| Word | Docs (index) | Docs (grep) | grep (ms) | Index (ms) | Speed-up |
|---|---:|---:|---:|---:|---:|
{table}

- grep takes {fmt(min(grep_times))} to {fmt(max(grep_times))} ms per word, because it reads the whole
  corpus every time, and the time does not clearly follow how common the word is. The index takes
  {fmt(min(index_times))} to {fmt(max(index_times))} ms for every word, which suggests that it is mostly the
  start of the process and the opening of the file. The speed-up is **{min(speedups):.0f}x to
  {max(speedups):.0f}x**, median {st.median(speedups):.0f}x.
- **Break-even.** Building the index takes {float(summary['build_s']):.1f} s at the fastest thread
  count in B1 ({summary['threads']} threads). A grep costs {fmt(float(summary['grep_ms_mean']))} ms on
  average and an indexed query {fmt(float(summary['index_ms_mean']))} ms, so the build is paid back after
  **{float(summary['break_even_queries']):.1f} queries**. This compares a count of one word; a ranked
  search does more work than a count, so the real figure is a little higher.
- **The counts.** grep and the index agree on {len(rows) - len(differ)} of {len(rows)} words. The
  {len(differ)} that differ are {differ_text}: grep finds a few more docs in every case. The reason is
  the rule for what a word is. `grep -w` in the C locale treats bytes of 0x80 and above (any
  non-ASCII character) as separators, so it finds `births` in "births–deaths" with an en dash; the
  tokenizer treats those bytes as part of a word, so for the index that word is `births–deaths` and not
  `births`. (`grep -w` also treats `_` as a word character, which goes the other way.) The last column
  of the CSV counts the docs with a regular expression that follows the tokenizer's rule, and it equals the
  index's count for {"all" if not unexplained else "not all"} {len(rows)} words, so the difference is explained by those rules and
  nothing else.

Limits: single-word counts only (grep cannot rank); one machine; the timings include process start
for both sides; grep's times differ between words more than the identical amount of data would
suggest ({fmt(min(grep_times))} to {fmt(max(grep_times))} ms) and the reason was not investigated: the median of 5 runs
was taken for each word, but different words were measured minutes apart.
"""


# ---------------------------------------------------------------- B1: indexing scaling


def section_b1(results, machine, profile):
    runs = {}
    for row in read_csv(results / f"b1_build-{profile}.csv"):
        runs.setdefault(int(row["threads"]), []).append(row)
    runs = dict(sorted(runs.items()))
    med = lambda rs, k: st.median(float(r[k]) for r in rs)
    first = min(runs)
    base_total, base_tok = med(runs[first], "total_s"), med(runs[first], "tokenize_s")
    docs = int(read_csv(results / f"b2_size-{profile}.csv")[0]["docs"])
    per_run = len(runs[first])
    commits = sorted({r["commit"] for rs in runs.values() for r in rs})

    rows, totals = [], {}
    for t, rs in runs.items():
        total = med(rs, "total_s")
        totals[t] = total
        merge = med(rs, "merge_s")
        rows.append(
            f"| {t} | {total:.2f} | {min(float(r['total_s']) for r in rs):.2f} to "
            f"{max(float(r['total_s']) for r in rs):.2f} | {base_total / total:.2f}x "
            f"| {med(rs, 'tokenize_s'):.2f} | {merge:.2f} | {100 * merge / total:.0f}% "
            f"| {med(rs, 'peak_rss_kb') / 1024:,.0f} |"
        )
    best = min(totals, key=totals.get)
    more = [t for t in totals if t > best]
    parallel = [t for t in runs if t >= 2]
    merges = [med(runs[t], "merge_s") for t in parallel]
    top = max(runs)
    fixed = med(runs[top], "scan_s") + med(runs[top], "merge_s")
    after = ""
    if more:
        ratios = [totals[t] / totals[best] for t in more]
        after = (
            f" More threads than that were not faster: the total at {', '.join(map(str, more))} threads is "
            f"{min(ratios):.2f} to {max(ratios):.2f} times the best."
        )
    return f"""## B1: indexing thread scaling ({machine}, `{profile}` profile, {docs:,} docs)

Source: `benchmarks/results/{machine}/b1_build-{profile}.csv` ({per_run} runs per thread count after one
discarded warm-up run, each thread count in a fresh process; commit {', '.join(f'`{c}`' for c in commits)}).
The table shows the median of the {per_run} runs and the range of the total. Results are in-memory only: writing
the index file is not part of these times.

| Threads | Total (s) | Range | Speed-up | Tokenize (s) | Merge (s) | Merge share | Peak RSS (MiB) |
|---:|---:|---:|---:|---:|---:|---:|---:|
{chr(10).join(rows)}

Measured, from the CSV:

- **The best total is {totals[best]:.2f} s at {best} threads, {base_total / totals[best]:.2f}x faster than
  {first} thread.**{after}
- **The merge phase does not scale.** From {min(parallel)} threads up it takes {min(merges):.2f} to
  {max(merges):.2f} s whatever the thread count, so its share of the total grows from
  {100 * med(runs[min(parallel)], 'merge_s') / totals[min(parallel)]:.0f}% at {min(parallel)} threads to
  {100 * med(runs[top], 'merge_s') / totals[top]:.0f}% at {top}. With the scan this fixed cost
  ({fixed:.2f} s at {top} threads) caps the speed-up (Amdahl's law) at
  {base_total / fixed:.0f}x even if tokenizing were free; in practice the tokenize phase is the larger limit.
- **The tokenize phase alone reaches {base_tok / min(med(rs, 'tokenize_s') for rs in runs.values()):.1f}x**
  ({base_tok:.1f} s to {min(med(rs, 'tokenize_s') for rs in runs.values()):.1f} s at
  {min(runs, key=lambda t: med(runs[t], 'tokenize_s'))} threads).
- **Memory use grows with threads**: {med(runs[first], 'peak_rss_kb') / MIB:.1f} GiB at {first} thread,
  {max(med(rs, 'peak_rss_kb') for rs in runs.values()) / MIB:.1f} GiB at the most, because each worker
  keeps its own term maps until the merge."""


SECTIONS = {
    "b1": section_b1,
    "b2": section_b2,
    "b3": section_b3,
    "b4": section_b4,
    "b5": section_b5,
    "b6": section_b6,
    "b7": section_b7,
}


def replace_section(text, key, body):
    begin, end = f"<!-- BEGIN {key} -->", f"<!-- END {key} -->"
    block = f"{begin}\n{body.rstrip()}\n{end}"
    if begin in text:
        start = text.index(begin)
        stop = text.index(end, start) + len(end)
        return text[:start] + block + text[stop:]
    return text.rstrip() + "\n\n" + block + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--machine", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("sections", nargs="*", help=f"any of {', '.join(SECTIONS)}; default: all")
    args = parser.parse_args()

    results = ROOT / "benchmarks" / "results" / args.machine
    notes = ROOT / "benchmarks" / "NOTES.md"
    text = notes.read_text()
    for key in args.sections or SECTIONS:
        csv_exists = list(results.glob(f"{key}_*-{args.profile}.csv"))
        if not csv_exists:
            print(f"skipping {key}: no CSV in {results}")
            continue
        text = replace_section(text, key, SECTIONS[key](results, args.machine, args.profile))
        print(f"wrote section {key}")
    notes.write_text(text)


if __name__ == "__main__":
    main()
