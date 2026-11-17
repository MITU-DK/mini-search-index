#!/usr/bin/env bash
# B5: grep against the index on 20 single-word queries (DESIGN.md §11).
#
#   tools/bench_grep.sh --machine laptop --profile main
#
# For each word: `LC_ALL=C grep -cwiF` over every part file, summed (which counts the matching
# docs, because a doc is one line), against `msearch query -i <index> --count-only <word>`. Both
# are whole processes, timed the same way, 5 runs each, median. The corpus and the index are read
# into the page cache first, so this compares the work, not the disk.
#
# Writes benchmarks/results/<machine>/b5_grep-<profile>.csv and b5_breakeven-<profile>.csv.
# Needs b2 (the index) and b1 (the build time) to have been run for this machine and profile.
set -euo pipefail

machine="" profile="" repeat=5
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || { echo "usage: $0 --machine M --profile P" >&2; exit 2; }
  case $1 in
    --machine) machine=$2 ;;
    --profile) profile=$2 ;;
    --repeat) repeat=$2 ;;
    *) echo "usage: $0 --machine M --profile P" >&2; exit 2 ;;
  esac
  shift 2
done
[ -n "$machine" ] && [ -n "$profile" ] || { echo "usage: $0 --machine M --profile P" >&2; exit 2; }

cd "$(dirname "$0")/.."
corpus="data/corpus/$profile"
index="data/index/$profile/vbyte.bin"
terms_file="benchmarks/queries/grep_terms-$profile.txt"
out="benchmarks/results/$machine"
b1="$out/b1_build-$profile.csv"
[ -d "$corpus" ] && [ -f "$index" ] || { echo "error: needs $corpus and $index (run b2)" >&2; exit 1; }
[ -f "$b1" ] || { echo "error: $b1 is missing (run b1)" >&2; exit 1; }

cmake --preset release > /dev/null
cmake --build --preset release > /dev/null
msearch=build/release/msearch
bench=build/release/msearch-bench
[ -f "$terms_file" ] || "$bench" pick-grep-terms --index "$index" --seed 42 --out "$terms_file"

cat "$index" "$corpus"/part-*.tsv > /dev/null  # into the page cache

now_ns() { date +%s%N; }

# GNU grep stops at the first match when its output is /dev/null, since nothing could tell the
# difference. That would make a common word look instant, so the output goes to a real file.
sink=$(mktemp)
trap 'rm -f "$sink"' EXIT

# Runs a command $repeat times, with one unrecorded run first, and prints the median in ms.
median_ms() {
  "$@" > "$sink" 2>&1 || true
  local times=() i start stop
  for ((i = 0; i < repeat; i++)); do
    start=$(now_ns)
    "$@" > "$sink" 2>&1 || true
    stop=$(now_ns)
    times+=($(((stop - start) / 1000)))  # microseconds
  done
  printf '%s\n' "${times[@]}" | sort -n | awk '{ v[NR] = $1 } END { printf "%.3f", v[int((NR + 1) / 2)] / 1000 }'
}

grep_all() { LC_ALL=C grep -cwiF -e "$1" "$corpus"/part-*.tsv; }
grep_docs() { grep_all "$1" | awk -F: '{ s += $NF } END { print s + 0 }'; }

# Not timed. The docs in which the word stands between bytes that our tokenizer treats as
# separators: the same rule as the index (letters, digits and every byte >= 0x80 are word
# characters; "_" is a separator), where grep -w differs in both. If this equals the index's df,
# the disagreement with grep is explained by those two rules and nothing else.
tokenizer_docs() {
  LC_ALL=C grep -cPi -e "(?<![A-Za-z0-9\x80-\xff])$1(?![A-Za-z0-9\x80-\xff])" "$corpus"/part-*.tsv |
    awk -F: '{ s += $NF } END { print s + 0 }'
}

csv="$out/b5_grep-$profile.csv"
echo "term,df,grep_docs,grep_ms,index_ms,speedup,tokenizer_regex_docs" > "$csv"
mismatches=0
while read -r term; do
  [ -n "$term" ] || continue
  df=$("$msearch" query -i "$index" --count-only "$term" 2> /dev/null)
  docs=$(grep_docs "$term")
  grep_ms=$(median_ms grep_all "$term")
  index_ms=$(median_ms "$msearch" query -i "$index" --count-only "$term")
  speedup=$(awk -v g="$grep_ms" -v i="$index_ms" 'BEGIN { printf "%.1f", g / i }')
  regex_docs=$(tokenizer_docs "$term")
  echo "$term,$df,$docs,$grep_ms,$index_ms,$speedup,$regex_docs" >> "$csv"
  if [ "$df" != "$docs" ]; then
    mismatches=$((mismatches + 1))
    echo "note: \"$term\": the index says $df docs, grep says $docs, the tokenizer's rule says $regex_docs" >&2
  fi
  if [ "$df" != "$regex_docs" ]; then
    echo "error: \"$term\": the index says $df docs but the tokenizer's rule finds $regex_docs;" \
         "the difference with grep is not explained" >&2
    exit 1
  fi
done < "$terms_file"
echo "wrote $csv ($mismatches words where grep and the index disagree)"

# Break-even: the build time at the fastest thread count in b1, over what one grep costs more than
# one indexed query.
python3 - "$b1" "$csv" "$out/b5_breakeven-$profile.csv" <<'PY'
import csv, statistics, sys
b1, grep_csv, out = sys.argv[1:4]
by_threads = {}
for row in csv.DictReader(open(b1)):
    by_threads.setdefault(int(row["threads"]), []).append(float(row["total_s"]))
threads, build_s = min(((t, statistics.median(v)) for t, v in by_threads.items()), key=lambda x: x[1])
rows = list(csv.DictReader(open(grep_csv)))
grep_ms = statistics.mean(float(r["grep_ms"]) for r in rows)
index_ms = statistics.mean(float(r["index_ms"]) for r in rows)
queries = build_s * 1000 / (grep_ms - index_ms)
with open(out, "w") as f:
    f.write("threads,build_s,grep_ms_mean,index_ms_mean,break_even_queries\n")
    f.write(f"{threads},{build_s:.3f},{grep_ms:.3f},{index_ms:.3f},{queries:.1f}\n")
print(f"wrote {out}")
PY
