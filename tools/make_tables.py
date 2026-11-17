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
  table alone is {nterms:,} x 24 bytes = {mib(nterms * 24)}.

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


SECTIONS = {"b2": section_b2, "b3": section_b3, "b7": section_b7}


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
