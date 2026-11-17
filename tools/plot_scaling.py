#!/usr/bin/env python3
"""Draws the indexing speed-up against the thread count, one line per machine (B1).

    .venv/bin/python tools/plot_scaling.py benchmarks/results docs/img/scaling.svg [profile]

The speed-up at T threads is the median build time at 1 thread over the median at T. The dashed
line is ideal scaling (speed-up = T). The profile defaults to "main".
"""
import csv
import statistics as st
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
matplotlib.rcParams["svg.hashsalt"] = "mini-search-index"  # fixed ids: the same data gives the same file
import matplotlib.pyplot as plt  # noqa: E402


def speedups(path):
    by_threads = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            by_threads.setdefault(int(row["threads"]), []).append(float(row["total_s"]))
    medians = {t: st.median(v) for t, v in sorted(by_threads.items())}
    base = medians[min(medians)]
    return {t: base / m for t, m in medians.items()}


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: plot_scaling.py <results dir> <output.svg> [profile]")
    results, output = Path(sys.argv[1]), Path(sys.argv[2])
    profile = sys.argv[3] if len(sys.argv) == 4 else "main"

    lines = {}
    for folder in sorted(p for p in results.iterdir() if p.is_dir()):
        csv_path = folder / f"b1_build-{profile}.csv"
        if csv_path.exists():
            lines[folder.name] = speedups(csv_path)
    if not lines:
        sys.exit(f"no b1_build-{profile}.csv under {results}")

    fig, ax = plt.subplots(figsize=(7, 4.5))
    top = max(max(s) for s in lines.values())
    ax.plot([1, top], [1, top], linestyle="--", color="0.55", label="ideal (speed-up = threads)")
    for name, points in lines.items():
        ax.plot(list(points), list(points.values()), marker="o", label=name)
    ax.set_xlabel("indexing threads")
    ax.set_ylabel("speed-up over 1 thread")
    ax.set_title(f"Indexing speed-up ({profile} corpus)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    # no date or random ids in the file, so that an unchanged chart is an unchanged file
    fig.savefig(output, format="svg", metadata={"Date": None, "Creator": None})
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
