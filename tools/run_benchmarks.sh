#!/usr/bin/env bash
# Runs the benchmarks and writes CSV files under benchmarks/results/<machine>/ (DESIGN.md §11).
#
#   tools/run_benchmarks.sh b1 --machine laptop --profile main [--threads 1,2,4 | auto] [--repeat 5]
#   tools/run_benchmarks.sh b2 --machine laptop --profile main
#
# Each thread count runs in a fresh process, so peak memory is not carried over from a
# previous one. The CSV of the benchmark is started again (not appended to) on every invocation.
set -euo pipefail

usage() { echo "usage: $0 <b1|b2> --machine M --profile P [--threads SWEEP] [--repeat N]" >&2; exit 2; }

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
  *) usage ;;
esac
