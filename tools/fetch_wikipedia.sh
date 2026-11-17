#!/usr/bin/env bash
# Download the pinned English Wikipedia dump files for a profile into data/raw/.
# Usage: tools/fetch_wikipedia.sh <dev|main|full>
# Files are checked against the dump's sha1sums, and an interrupted download resumes.
set -euo pipefail

DUMP=20261001
BASE="https://dumps.wikimedia.org/enwiki/${DUMP}"

# Part file names for the numbered dump parts we use (DESIGN.md §3).
part_names=(
  [1]="enwiki-${DUMP}-pages-articles1.xml-p1p41242.bz2"
  [2]="enwiki-${DUMP}-pages-articles2.xml-p41243p151573.bz2"
  [3]="enwiki-${DUMP}-pages-articles3.xml-p151574p311329.bz2"
  [4]="enwiki-${DUMP}-pages-articles4.xml-p311330p558391.bz2"
  [5]="enwiki-${DUMP}-pages-articles5.xml-p558392p958045.bz2"
)

case "${1:-}" in
  dev)  files=("${part_names[1]}") ;;
  main) files=("${part_names[1]}" "${part_names[2]}" "${part_names[3]}" "${part_names[4]}") ;;
  full) files=("enwiki-${DUMP}-pages-articles.xml.bz2") ;;
  *)    echo "usage: $0 <dev|main|full>" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/.."
mkdir -p data/raw
cd data/raw

wget -q -N "${BASE}/enwiki-${DUMP}-sha1sums.txt"

for f in "${files[@]}"; do
  echo "Downloading ${f}"
  wget -c --progress=dot:giga "${BASE}/${f}"
  # Match the whole name at the end of the line so "articles1" can't match "articles10".
  grep " ${f}\$" "enwiki-${DUMP}-sha1sums.txt" | sha1sum -c -
done
