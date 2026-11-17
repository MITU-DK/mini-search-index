#!/usr/bin/env python3
"""Fills the generated blocks of README.md from the benchmark CSV files.

    python3 tools/make_readme.py README.md benchmarks/results [machine] [profile]

README.md is written by hand, except for the parts between "<!-- BEGIN generated:NAME -->" and
"<!-- END generated:NAME -->", which this script replaces. Every number in those parts is computed
from the CSV files (the same ones tools/make_results.py reads), so none is typed by hand. The
machine defaults to "laptop" and the profile to "main".
"""
import re
import statistics as st
import sys
from pathlib import Path

import make_results as mr

MIB = 2**20


def machine_line(folder):
    info = {}
    for line in (folder / "machine.txt").read_text().splitlines():
        key, _, value = line.partition(": ")
        info[key] = value
    return (f"{info['cpu']}, {info['logical_cpus']} logical CPUs, {info['ram'].split(' total')[0]} RAM, "
            f"{info['kernel']}, {info['compiler'].split(' (')[0]} {info['compiler'].split()[-1]}")


def headline(folder, profile):
    b2 = mr.read_csv(folder / f"b2_size-{profile}.csv")[0]
    corpus, vbyte, raw = int(b2["corpus_bytes"]), int(b2["vbyte_file_bytes"]), int(b2["raw_file_bytes"])

    runs = mr.b1_runs(folder, profile)
    med = {t: st.median(float(r["total_s"]) for r in rs) for t, rs in runs.items()}
    one, best_t = med[min(med)], min(med, key=med.get)

    b3 = {}
    for r in mr.read_csv(folder / f"b3_load-{profile}.csv"):
        b3.setdefault((r["cache"], r["mode"]), []).append(r)
    open_ms = lambda c, m: st.median(float(r["open_ms"]) for r in b3[(c, m)])
    first_ms = lambda c, m: st.median(float(r["first_query_ms"]) for r in b3[(c, m)])
    rss = lambda m: st.median(float(r["rss_after_open_kb"]) for r in b3[("warm", m)]) / 1024

    lat = {(r["cache"], r["mode"]): r for r in mr.read_csv(folder / f"b4_query-{profile}.csv")}
    q = {r["mode"]: r for r in mr.read_csv(folder / f"b4_quality-{profile}.csv")}

    grep = mr.read_csv(folder / f"b5_grep-{profile}.csv")
    speedups = [float(r["speedup"]) for r in grep]
    even = mr.read_csv(folder / f"b5_breakeven-{profile}.csv")[0]

    sp = {r["method"]: r for r in mr.read_csv(folder / f"b6_spell-{profile}.csv")}
    ac = {r["method"]: r for r in mr.read_csv(folder / f"b7_suggest-{profile}.csv")}
    f = mr.fmt

    rows = [
        ("Corpus", f"{int(b2['docs']):,} English Wikipedia articles, {corpus / MIB:,.0f} MiB of text", "B2"),
        ("Index size",
         f"{vbyte / MIB:,.0f} MiB with gap + VByte postings ({100 * vbyte / corpus:.1f}% of the text), "
         f"against {raw / MIB:,.0f} MiB ({100 * raw / corpus:.1f}%) with raw postings; "
         f"{float(b2['bytes_per_posting_vbyte']):.2f} bytes per posting", "B2"),
        ("Indexing time",
         f"{f(one)} s with 1 thread, {f(med[best_t])} s with {best_t} threads "
         f"({one / med[best_t]:.2f}x faster)", "B1"),
        ("Opening the index",
         f"warm: `mmap` {f(open_ms('warm', 'mmap'))} ms, `read` {f(open_ms('warm', 'read'))} ms, "
         f"`parse` {f(open_ms('warm', 'parse'))} ms. Cold: `mmap` {f(open_ms('cold', 'mmap'))} ms, "
         f"`read` {f(open_ms('cold', 'read'))} ms, `parse` {f(open_ms('cold', 'parse'))} ms; "
         f"but the first query after a cold `mmap` takes {f(first_ms('cold', 'mmap'))} ms (page faults), "
         f"against {f(first_ms('cold', 'parse'))} ms after a cold `parse`", "B3"),
        ("Memory after opening",
         f"{rss('mmap'):,.0f} MiB with `mmap`, {rss('parse'):,.0f} MiB with `parse`", "B3"),
        ("Query latency (warm, one thread)",
         f"BM25 p50 {f(float(lat[('warm', 'bm25')]['p50_ms']))} ms, p95 {f(float(lat[('warm', 'bm25')]['p95_ms']))} ms; "
         f"with result previews p50 {f(float(lat[('warm', 'bm25+snippets')]['p50_ms']))} ms, "
         f"p95 {f(float(lat[('warm', 'bm25+snippets')]['p95_ms']))} ms", "B4"),
        ("Ranking quality",
         f"MRR@10 {float(q['bm25']['mrr_at_10']):.3f} for BM25, {float(q['tfidf']['mrr_at_10']):.3f} for TF-IDF "
         f"(1000 known-item queries; see Limitations)", "B4"),
        ("Against `grep`",
         f"{f(min(speedups))}x to {f(max(speedups))}x faster for a one-word count "
         f"(median {f(st.median(speedups))}x, {len(speedups)} words); the index build is paid back after "
         f"{float(even['break_even_queries']):.1f} queries", "B5"),
        ("Spell check",
         f"BK-tree p50 {float(sp['bk_tree']['p50_us']) / 1000:.1f} ms against "
         f"{float(sp['brute_force']['p50_us']) / 1000:.1f} ms for brute force "
         f"({float(sp['brute_force']['p50_us']) / float(sp['bk_tree']['p50_us']):.1f}x); "
         f"the right word is suggested for {100 * float(sp['bk_tree']['accuracy']):.0f}% of 200 single-edit typos", "B6"),
        ("Autocomplete",
         f"sorted term table p50 {f(float(ac['sorted_table']['p50_us']))} us, p95 {f(float(ac['sorted_table']['p95_us']))} us, "
         f"nothing to build; trie p50 {f(float(ac['trie']['p50_us']))} us but "
         f"{f(float(ac['trie']['build_ms']))} ms to build and "
         f"{int(ac['trie']['extra_memory_bytes']) / MIB:,.0f} MiB of memory", "B7"),
    ]
    head = f"Measured on: {machine_line(folder)}. Profile `{profile}`.\n\n"
    return head + mr.table(["", "Result", "Benchmark"], [list(r) for r in rows])


def scaling(results, machines, profile):
    folder = machines[0][1]
    runs = mr.b1_runs(folder, profile)
    base = st.median(float(r["total_s"]) for r in runs[min(runs)])
    rows = []
    for t, rs in runs.items():
        total = st.median(float(r["total_s"]) for r in rs)
        rows.append([t, mr.fmt(total), f"{base / total:.2f}x"])
    merges = [st.median(float(r["merge_s"]) for r in rs) for t, rs in runs.items() if t > 1]
    tok = {t: st.median(float(r["tokenize_s"]) for r in rs) for t, rs in runs.items()}
    out = [f"![Indexing speed-up against thread count](docs/img/scaling.svg)", ""]
    out.append(mr.table(["Threads", f"Build time (s), {machines[0][0]}", "Speed-up"], rows))
    out += ["",
            f"Tokenizing scales ({mr.fmt(tok[min(tok)])} s with {min(tok)} thread, {mr.fmt(tok[max(tok)])} s with "
            f"{max(tok)}), but the merge stage takes {mr.fmt(min(merges))} to {mr.fmt(max(merges))} s whatever the thread "
            "count. It runs in parallel, but each worker keeps its own list for almost every common term, so "
            "the work to combine them grows with the number of workers (exploratory measurements in "
            "[benchmarks/NOTES.md](benchmarks/NOTES.md)). That is a known limit of this design, and part of why "
            "the speed-up flattens. (`--memory-budget` selects a different build, a bounded-memory one with a "
            "single k-way merge, which is not part of this table.)"]
    if len(machines) > 1:
        out += ["", mr.combined_b1(results, machines)]
    else:
        out += ["", f"Only one machine ({machines[0][0]}) has been measured so far. Its 20 logical CPUs are 6 fast cores "
                "with two threads each plus 8 slower cores, so the curve flattens early; a run on a machine with uniform "
                "cores is planned and will be added here (see [docs/CLOUD.md](docs/CLOUD.md))."]
    return "\n".join(out)


def replace_block(text, name, body):
    pattern = re.compile(rf"(<!-- BEGIN generated:{name} -->\n).*?(<!-- END generated:{name} -->)", re.S)
    if not pattern.search(text):
        sys.exit(f"README has no generated:{name} block")
    return pattern.sub(lambda m: m.group(1) + body + "\n" + m.group(2), text)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    readme, results = Path(sys.argv[1]), Path(sys.argv[2])
    machine = sys.argv[3] if len(sys.argv) > 3 else "laptop"
    profile = sys.argv[4] if len(sys.argv) > 4 else "main"
    machines = [(p.name, p) for p in sorted(results.iterdir()) if p.is_dir()]
    machines.sort(key=lambda m: m[0] != machine)  # the main machine first
    text = readme.read_text()
    text = replace_block(text, "headline", headline(results / machine, profile))
    text = replace_block(text, "scaling", scaling(results, machines, profile))
    readme.write_text(text)


if __name__ == "__main__":
    main()
