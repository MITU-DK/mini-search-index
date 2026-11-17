#!/usr/bin/env python3
"""Writes benchmarks/RESULTS.md: every benchmark table, for every machine, from the CSV files.

    python3 tools/make_results.py benchmarks/results > benchmarks/RESULTS.md

Timings are medians with the min to max range of the runs. Nothing is typed by hand: the numbers all
come from the CSV files. The explanations of what the numbers mean are in benchmarks/NOTES.md.
"""
import csv
import statistics as st
import sys
from pathlib import Path

MIB = 2**20


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def fmt(x):
    if x < 1:
        return f"{x:.3f}"
    if x < 10:
        return f"{x:.2f}"
    if x < 100:
        return f"{x:.1f}"
    return f"{x:,.0f}"


def med_range(values):
    values = [float(v) for v in values]
    return f"{fmt(st.median(values))} ({fmt(min(values))} to {fmt(max(values))})"


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(out)


def profiles_of(folder, prefix):
    return sorted(p.stem.split("-", 1)[1] for p in folder.glob(f"{prefix}-*.csv"))


# ---------------------------------------------------------------- per-benchmark tables


def b1_runs(folder, profile):
    by_threads = {}
    for row in read_csv(folder / f"b1_build-{profile}.csv"):
        by_threads.setdefault(int(row["threads"]), []).append(row)
    return dict(sorted(by_threads.items()))


def b1_table(folder, profile):
    runs = b1_runs(folder, profile)
    base = st.median(float(r["total_s"]) for r in runs[min(runs)])
    rows = []
    for t, rs in runs.items():
        total = st.median(float(r["total_s"]) for r in rs)
        rows.append([
            t,
            med_range(r["total_s"] for r in rs),
            fmt(st.median(float(r["scan_s"]) for r in rs)),
            fmt(st.median(float(r["tokenize_s"]) for r in rs)),
            fmt(st.median(float(r["merge_s"]) for r in rs)),
            f"{base / total:.2f}x",
            f"{st.median(float(r['peak_rss_kb']) for r in rs) / MIB:.2f}",
        ])
    commits = sorted({r["commit"] for rs in runs.values() for r in rs})
    return (
        table(["Threads", "Total s, median (min to max)", "Scan s", "Tokenize s", "Merge s",
               "Speed-up vs 1", "Peak RSS (GiB)"], rows)
        + f"\n\nCommit: {', '.join(f'`{c}`' for c in commits)}. {len(next(iter(runs.values())))} runs per row."
    )


def b2_table(folder, profile):
    r = read_csv(folder / f"b2_size-{profile}.csv")[0]
    i = lambda k: int(r[k])
    rows = [
        ["Corpus text", f"{i('corpus_bytes') / MIB:,.1f}", "100%"],
        ["Index, raw postings", f"{i('raw_file_bytes') / MIB:,.1f}", f"{100 * i('raw_file_bytes') / i('corpus_bytes'):.1f}%"],
        ["Index, gap + VByte postings", f"{i('vbyte_file_bytes') / MIB:,.1f}", f"{100 * i('vbyte_file_bytes') / i('corpus_bytes'):.1f}%"],
    ]
    text = table(["", "MiB", "% of corpus"], rows)
    text += (f"\n\n{i('docs'):,} docs, {i('terms'):,} terms, {i('postings'):,} postings; "
             f"{float(r['bytes_per_posting_vbyte']):.2f} bytes per posting compressed")
    if "vbyte_spell_bytes" in r:
        text += f"; spell tree {i('vbyte_spell_bytes') / MIB:.1f} MiB, built in {float(r['spell_build_s']):.2f} s"
    return text + "."


def b3_table(folder, profile):
    groups = {}
    for r in read_csv(folder / f"b3_load-{profile}.csv"):
        groups.setdefault((r["cache"], r["mode"]), []).append(r)
    rows = []
    for (cache, mode), rs in sorted(groups.items(), key=lambda kv: (kv[0][0] != "cold", kv[0][1])):
        rows.append([
            cache, mode,
            med_range(r["open_ms"] for r in rs),
            med_range(r["first_query_ms"] for r in rs),
            f"{st.median(float(r['rss_after_open_kb']) for r in rs) / 1024:.0f}",
            f"{st.median(float(r['rss_after_1000q_kb']) for r in rs) / 1024:.0f}",
            f"{st.median(float(r['major_faults_first_query']) for r in rs):.0f}",
        ])
    commits = sorted({r["commit"] for rs in groups.values() for r in rs})
    return (table(["Cache", "Mode", "Open ms", "First query ms", "RSS after open (MiB)",
                   "RSS after 1000 queries (MiB)", "Major faults, first query"], rows)
            + f"\n\nCommit: {', '.join(f'`{c}`' for c in commits)}.")


def b4_tables(folder, profile):
    latency = read_csv(folder / f"b4_query-{profile}.csv")
    quality = read_csv(folder / f"b4_quality-{profile}.csv")
    a = table(["Cache", "Mode", "p50 ms", "p95 ms", "p99 ms", "mean ms", "max ms", "Major faults"],
              [[r["cache"], r["mode"], fmt(float(r["p50_ms"])), fmt(float(r["p95_ms"])),
                fmt(float(r["p99_ms"])), fmt(float(r["mean_ms"])), fmt(float(r["max_ms"])),
                r["major_faults"]] for r in latency])
    b = table(["Ranking", "MRR@10", "Hit@1", "Hit@10", "Queries"],
              [[r["mode"], f"{float(r['mrr_at_10']):.3f}", f"{100 * float(r['hit_at_1']):.1f}%",
                f"{100 * float(r['hit_at_10']):.1f}%", r["queries"]] for r in quality])
    return a + "\n\n" + b


def b5_table(folder, profile):
    rows = read_csv(folder / f"b5_grep-{profile}.csv")
    summary = read_csv(folder / f"b5_breakeven-{profile}.csv")[0]
    t = table(["Word", "Docs (index)", "Docs (grep)", "grep ms", "Index ms", "Speed-up"],
              [[f"`{r['term']}`", f"{int(r['df']):,}", f"{int(r['grep_docs']):,}", fmt(float(r["grep_ms"])),
                fmt(float(r["index_ms"])), f"{float(r['speedup']):.0f}x"] for r in rows])
    return (t + f"\n\nBuild {float(summary['build_s']):.1f} s at {summary['threads']} threads; "
            f"break-even after {float(summary['break_even_queries']):.1f} queries.")


def b6_table(folder, profile):
    rows = read_csv(folder / f"b6_spell-{profile}.csv")
    return table(["Method", "p50 us", "p95 us", "Accuracy", "Distances per typo"],
                 [[r["method"], fmt(float(r["p50_us"])), fmt(float(r["p95_us"])),
                   f"{100 * float(r['accuracy']):.1f}%", f"{float(r['avg_distance_calls']):,.0f}"]
                  for r in rows])


def b7_table(folder, profile):
    rows = read_csv(folder / f"b7_suggest-{profile}.csv")
    return table(["Method", "Build ms", "Extra memory (MiB)", "p50 us", "p95 us"],
                 [[r["method"], fmt(float(r["build_ms"])), f"{int(r['extra_memory_bytes']) / MIB:,.0f}",
                   fmt(float(r["p50_us"])), fmt(float(r["p95_us"]))] for r in rows])


BENCHMARKS = [
    ("b1", "B1: indexing time and scaling", b1_table, "b1_build"),
    ("b2", "B2: index size", b2_table, "b2_size"),
    ("b3", "B3: loading the index", b3_table, "b3_load"),
    ("b4", "B4: query latency and ranking quality", b4_tables, "b4_query"),
    ("b5", "B5: grep against the index", b5_table, "b5_grep"),
    ("b6", "B6: spell check", b6_table, "b6_spell"),
    ("b7", "B7: autocomplete", b7_table, "b7_suggest"),
]


# ---------------------------------------------------------------- the whole document


def combined_b1(results, machines):
    """One column per machine: the median total time and the speed-up at each thread count."""
    data = {}
    for machine, folder in machines:
        for profile in profiles_of(folder, "b1_build"):
            runs = b1_runs(folder, profile)
            base = st.median(float(r["total_s"]) for r in runs[min(runs)])
            data.setdefault(profile, {})[machine] = {
                t: (st.median(float(r["total_s"]) for r in rs), base / st.median(float(r["total_s"]) for r in rs))
                for t, rs in runs.items()
            }
    out = []
    for profile, per_machine in data.items():
        names = list(per_machine)
        threads = sorted({t for m in per_machine.values() for t in m})
        header = ["Threads"] + [f"{n}: s (speed-up)" for n in names]
        rows = []
        for t in threads:
            row = [t]
            for n in names:
                cell = per_machine[n].get(t)
                row.append(f"{fmt(cell[0])} ({cell[1]:.2f}x)" if cell else "")
            rows.append(row)
        out.append(f"### Indexing time on every machine, `{profile}` corpus\n\n" + table(header, rows))
    return "\n\n".join(out)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_results.py benchmarks/results > benchmarks/RESULTS.md")
    results = Path(sys.argv[1])
    machines = [(p.name, p) for p in sorted(results.iterdir()) if p.is_dir()]
    out = [
        "# Benchmark results",
        "",
        "Generated by `tools/make_results.py` from the CSV files in `benchmarks/results/`; do not edit by "
        "hand. What the numbers mean, and what to be careful of, is in [NOTES.md](NOTES.md). "
        "Timings are medians with the min to max range of the runs.",
    ]
    if len(machines) > 1:
        out += ["", "## Scaling across machines", "", combined_b1(results, machines)]
    for machine, folder in machines:
        out += ["", f"## Machine: {machine}", ""]
        info = folder / "machine.txt"
        if info.exists():
            out += ["```", info.read_text().strip(), "```"]
        for key, title, render, prefix in BENCHMARKS:
            for profile in profiles_of(folder, prefix):
                try:
                    body = render(folder, profile)
                except FileNotFoundError as e:
                    body = f"_not available: {Path(e.filename).name} is missing_"
                out += ["", f"### {title} (`{profile}`)", "", body]
    print("\n".join(out))


if __name__ == "__main__":
    main()
