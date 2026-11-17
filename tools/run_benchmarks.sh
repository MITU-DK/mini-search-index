#!/usr/bin/env bash
# Runs the benchmarks and writes CSV files under benchmarks/results/<machine>/ (DESIGN.md §11).
#
#   tools/run_benchmarks.sh b1 --machine laptop --profile main [--threads 1,2,4 | auto] [--repeat 5]
#   tools/run_benchmarks.sh b2 --machine laptop --profile main
#   tools/run_benchmarks.sh b3 --machine laptop --profile main [--repeat 5]   (needs b2 first)
#   tools/run_benchmarks.sh b7 --machine laptop --profile main                (needs b2 first)
#
# Each thread count runs in a fresh process, so peak memory is not carried over from a
# previous one. The CSV of the benchmark is started again (not appended to) on every invocation.
set -euo pipefail

usage() { echo "usage: $0 <b1|b2|b3|b7> --machine M --profile P [--threads SWEEP] [--repeat N]" >&2; exit 2; }

[ $# -ge 1 ] || usage
benchmark=$1
shift
machine="" profile="" threads="auto" repeat=5
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case $1 in
    --machine) machine=$2 ;;
    --profile) profile=$2 ;;
    --threads) threads=$2 ;;
    --repeat) repeat=$2 ;;
    *) usage ;;
  esac
  shift 2
done
[ -n "$machine" ] && [ -n "$profile" ] || usage

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
corpus="data/corpus/$profile"
[ -d "$corpus" ] || { echo "error: $corpus does not exist (run tools/fetch_wikipedia.sh and tools/prepare_corpus.py)" >&2; exit 1; }

commit=$(git rev-parse --short HEAD)
git diff --quiet HEAD -- || commit="$commit-dirty"

cmake --preset release >/dev/null
cmake --build --preset release --target msearch-bench >/dev/null
bench=build/release/msearch-bench

out="benchmarks/results/$machine"
mkdir -p "$out"
tools/machine_info.sh > "$out/machine.txt"

case $benchmark in
  b1)
    rm -f "$out/b1_build-$profile.csv"
    for t in $("$bench" sweep --threads "$threads"); do
      "$bench" build --corpus "$corpus" --threads "$t" --repeat "$repeat" \
        --machine "$machine" --profile "$profile" --commit "$commit"
    done
    echo "wrote $out/b1_build-$profile.csv"
    ;;
  b2)
    "$bench" size --corpus "$corpus" --machine "$machine" --profile "$profile"
    ;;
  b3)
    index="data/index/$profile/vbyte.bin"
    queries="data/index/$profile/queries.txt"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    # Known-item queries (DESIGN.md §11): the titles of 1000 random docs, seed 42.
    [ -f "$queries" ] || "$bench" queries --index "$index" --count 1000 --seed 42 --out "$queries"

    csv="$out/b3_load-$profile.csv"
    rm -f "$csv"
    # Round 0 only warms things up and is not recorded. Every measurement is a fresh process.
    for round in $(seq 0 "$repeat"); do
      record=yes
      [ "$round" -eq 0 ] && record=no
      for cache in cold warm; do
        for mode in parse read mmap; do
          # a warm run needs the file in the page cache; a cold run evicts it inside the bench
          [ "$cache" = warm ] && cat "$index" > /dev/null
          "$bench" load --index "$index" --mode "$mode" --cache "$cache" --queries "$queries" \
            --machine "$machine" --profile "$profile" --commit "$commit" --record "$record"
        done
      done
    done

    # A cold number means nothing if eviction did not work. Only mmap shows it: a read() from disk
    # is not a page fault, but the first touch of an evicted mapped page is a major one.
    # Columns: 2 mode, 3 cache, 11 major_faults_first_query.
    awk -F, 'NR > 1 && $2 == "mmap" && $3 == "cold" && $11 == 0 { bad++ } END { exit bad > 0 }' "$csv" || {
      echo "error: a cold mmap run had no major page faults, so the page cache was not evicted;" \
           "its numbers cannot be trusted ($csv)" >&2
      exit 1
    }
    echo "wrote $csv"
    ;;
  b7)
    index="data/index/$profile/vbyte.bin"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    "$bench" suggest --index "$index" --n 1000 --seed 42 --machine "$machine" --profile "$profile"
    ;;
  *) usage ;;
esac
