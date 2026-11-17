#!/usr/bin/env python3
"""Turn WikiExtractor JSON output into packed corpus part files (DESIGN.md §4).

Each output line is "title<TAB>text". Part files are about 64 MiB and named
part-00000.tsv, part-00001.tsv, ... Doc id = global line number.

Usage: prepare_corpus.py <extracted_dir> <out_dir> [--max-docs N] [--stats-out FILE]
Exit code 1 if --max-docs was given and fewer docs than that were written.
"""
import argparse
import json
import sys
from pathlib import Path

PART_BYTES = 64 * 1024 * 1024  # keep in sync with DESIGN.md §4

# Tabs and line breaks would break the one-article-per-line format.
SEPARATORS = str.maketrans({"\t": " ", "\r": " ", "\n": " "})


def clean(s: str) -> str:
    return s.translate(SEPARATORS).strip()


def extracted_files(root: Path):
    """WikiExtractor output files in a stable order: sorted by relative path."""
    return sorted(p for p in root.rglob("wiki_*") if p.is_file())


def records(root: Path):
    """Yield (title, text) for every article, in dump order."""
    for path in extracted_files(root):
        with path.open(encoding="utf-8") as f:
            for line in f:
                rec = json.loads(line)
                yield rec["title"], rec["text"]


def strip_title_line(title: str, text: str) -> str:
    """WikiExtractor may repeat the title as the first line of the text."""
    first, sep, rest = text.partition("\n")
    return rest if sep and first.strip() == title.strip() else text


class PartWriter:
    def __init__(self, out_dir: Path):
        self.out_dir = out_dir
        self.index = 0
        self.size = 0
        self.file = None

    def write(self, line: bytes):
        if self.file is None or self.size + len(line) > PART_BYTES:
            self._next_part()
        self.file.write(line)
        self.size += len(line)

    def _next_part(self):
        if self.file:
            self.file.close()
        self.file = (self.out_dir / f"part-{self.index:05d}.tsv").open("wb")
        self.index += 1
        self.size = 0

    def close(self):
        if self.file:
            self.file.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("extracted_dir", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--max-docs", type=int)
    ap.add_argument("--stats-out", type=Path)
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if any(args.out_dir.glob("part-*.tsv")):
        sys.exit(f"{args.out_dir} already has part files; remove them first")

    writer = PartWriter(args.out_dir)
    written = skipped = total_bytes = 0
    for title, text in records(args.extracted_dir):
        if args.max_docs is not None and written >= args.max_docs:
            break
        title = clean(title)
        text = clean(strip_title_line(title, text))
        if not text:
            skipped += 1
            continue
        line = f"{title}\t{text}\n".encode("utf-8")
        writer.write(line)
        written += 1
        total_bytes += len(line)
    writer.close()

    stats = (
        f"docs_written={written}\ndocs_skipped={skipped}\n"
        f"bytes={total_bytes}\nparts={writer.index}\n"
    )
    print(stats, end="")
    if args.stats_out:
        args.stats_out.parent.mkdir(parents=True, exist_ok=True)
        args.stats_out.write_text(stats)

    if args.max_docs is not None and written < args.max_docs:
        print(f"only {written} docs, wanted {args.max_docs}: fetch the next dump part", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
