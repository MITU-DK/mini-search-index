#!/usr/bin/env bash
# Runs the benchmarks and writes CSV files under benchmarks/results/<machine>/ (DESIGN.md §11).
#
#   tools/run_benchmarks.sh b1 --machine laptop --profile main [--threads 1,2,4 | auto] [--repeat 5]
#   tools/run_benchmarks.sh b2 --machine laptop --profile main
#   tools/run_benchmarks.sh b3 --machine laptop --profile main [--repeat 5]   (needs b2 first)
#   tools/run_benchmarks.sh b4 --machine laptop --profile main                (needs b2 first)
#   tools/run_benchmarks.sh b5 --machine laptop --profile main                (needs b2 and b1)
#   tools/run_benchmarks.sh b6 --machine laptop --profile main                (needs b2 first)
#   tools/run_benchmarks.sh b7 --machine laptop --profile main                (needs b2 first)
#   tools/run_benchmarks.sh b8 --machine laptop --profile main [--budgets 256M,512M,1G] [--threads 4,auto]
#                                                      [--repeat N]   (bounded-memory builds)
#
#   tools/run_benchmarks.sh all --machine laptop --profile main               (b2, then the rest, b8 last)
#
# b8 builds the corpus within each memory budget in a fresh process (so the peak memory is that
# build's alone) and keeps the index of the last one as data/index/<profile>/vbyte.bin if there is none
# yet, which is how the `full` index for b3, b4, b6 and b7 is made. Where vbyte.bin is already there
# (the in-memory index of b2) it is compared with the new file byte for byte, and a difference is an
# error. Defaults: budgets 64M,128M for dev, 256M,512M,1G for main and 2G,4G,8G for full; threads 4 and
# all of them for dev and main, all of them for full; repeat 3 (1 for full).
#
# Each thread count runs in a fresh process, so peak memory is not carried over from a
# previous one. The CSV of the benchmark is started again (not appended to) on every invocation.
set -euo pipefail

usage() { echo "usage: $0 <b1|b2|b3|b4|b5|b6|b7|b8|all> --machine M --profile P [--threads SWEEP] [--repeat N] [--budgets LIST]" >&2; exit 2; }

[ $# -ge 1 ] || usage
benchmark=$1
shift
machine="" profile="" threads="auto" threads_set=no repeat=5 repeat_set=no budgets=""
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case $1 in
    --machine) machine=$2 ;;
    --profile) profile=$2 ;;
    --threads) threads=$2; threads_set=yes ;;
    --repeat) repeat=$2; repeat_set=yes ;;
    --budgets) budgets=$2 ;;
    *) usage ;;
  esac
  shift 2
done
[ -n "$machine" ] && [ -n "$profile" ] || usage

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if [ "$benchmark" = all ]; then
  # b2 first: it writes the index files that the others read. b5 needs the build time from b1.
  # b8 is a separate step: for `full`, b1 and b2 (in-memory builds) do not fit.
  for each in b2 b1 b3 b4 b5 b6 b7 b8; do
    if [ "$each" = b8 ] && [ "$profile" = full ]; then continue; fi
    echo "==== $each ===="
    if [ "$each" = b8 ]; then
      # b8 has its own thread counts (4 and all of them): a sweep is not what it wants
      "$0" b8 --machine "$machine" --profile "$profile" --repeat "$repeat"
    else
      "$0" "$each" --machine "$machine" --profile "$profile" --threads "$threads" --repeat "$repeat"
    fi
  done
  exit 0
fi
corpus="data/corpus/$profile"
[ -d "$corpus" ] || { echo "error: $corpus does not exist (run tools/fetch_wikipedia.sh and tools/prepare_corpus.py)" >&2; exit 1; }

commit=$(git rev-parse --short HEAD)
# The results folder changes during a run (earlier benchmarks write CSVs), so it does not count.
git diff --quiet HEAD -- . ':!benchmarks/results' || commit="$commit-dirty"

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
    queries="benchmarks/queries/known_item-$profile.tsv"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    # Known-item queries (DESIGN.md §11): the titles of 1000 random docs, seed 42. Kept in git.
    [ -f "$queries" ] || "$bench" gen-queries --index "$index" --n 1000 --seed 42 --out "$queries"

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
  b4)
    index="data/index/$profile/vbyte.bin"
    queries="benchmarks/queries/known_item-$profile.tsv"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    [ -f "$queries" ] || "$bench" gen-queries --index "$index" --n 1000 --seed 42 --out "$queries"

    # Quality does not depend on timing, so it runs once.
    "$bench" quality --index "$index" --queries "$queries" --machine "$machine" --profile "$profile"

    csv="$out/b4_query-$profile.csv"
    rm -f "$csv"
    # Warm: the index and the corpus (for the snippets) are in the page cache. One process runs
    # every mode: an unrecorded pass, then 5 recorded ones.
    cat "$index" "$corpus"/part-*.tsv > /dev/null
    "$bench" queries --index "$index" --queries "$queries" --cache warm \
      --machine "$machine" --profile "$profile" --corpus "$corpus"
    # Cold: the index file is evicted from the page cache first, and each mode gets its own fresh
    # process, because the pages one mode touches are warm for the next.
    for mode in bm25 tfidf; do
      "$bench" queries --index "$index" --queries "$queries" --cache cold --modes "$mode" \
        --machine "$machine" --profile "$profile"
    done
    echo "wrote $csv"
    ;;
  b5)
    tools/bench_grep.sh --machine "$machine" --profile "$profile"
    ;;
  b6)
    index="data/index/$profile/vbyte.bin"
    typos="benchmarks/queries/typos-$profile.tsv"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    # 200 seeded single-edit typos of common words (DESIGN.md §11). Kept in git, so the same
    # typos are used on every machine; made again only if the file is missing.
    [ -f "$typos" ] || "$bench" gen-typos --index "$index" --n 200 --seed 42 --out "$typos"
    "$bench" spell --index "$index" --typos "$typos" --machine "$machine" --profile "$profile"
    ;;
  b7)
    index="data/index/$profile/vbyte.bin"
    [ -f "$index" ] || { echo "error: $index is missing; run b2 first" >&2; exit 1; }
    "$bench" suggest --index "$index" --n 1000 --seed 42 --machine "$machine" --profile "$profile"
    ;;
  b8)
    # The bounded-memory build. One fresh process per budget and per repeat.
    case $profile in
      dev) default_budgets=64M,128M; default_repeat=3; default_threads=4,auto ;;
      main) default_budgets=256M,512M,1G; default_repeat=3; default_threads=4,auto ;;
      *) default_budgets=2G,4G,8G; default_repeat=1; default_threads=auto ;;
    esac
    [ "$threads_set" = yes ] || threads=$default_threads
    [ -n "$budgets" ] || budgets=$default_budgets
    [ "$repeat_set" = yes ] || repeat=$default_repeat
    thread_counts=()
    for t in ${threads//,/ }; do
      [ "$t" != auto ] || t=$(nproc)
      case $t in *[!0-9]*|"") echo "error: b8 thread counts are numbers or auto, not \"$t\"" >&2; exit 2 ;; esac
      thread_counts+=("$t")
    done

    index_dir="data/index/$profile"
    tmp_dir="data/tmp/$profile-runs"
    mkdir -p "$index_dir" "$tmp_dir"
    candidate="$index_dir/b8.bin"
    existing="$index_dir/vbyte.bin"

    # Fail now, not hours in, if the disk is too small: the runs are about half the corpus, and the
    # output and the writer's temporary files together about as much again.
    corpus_bytes=$(cat "$corpus"/part-*.tsv | wc -c)
    need_bytes=$((corpus_bytes * 6 / 5))
    free_bytes=$(df --output=avail -B1 "$index_dir" | tail -1)
    if [ "$free_bytes" -lt "$need_bytes" ]; then
      echo "error: b8 needs about $((need_bytes / 1000000000)) GB free in $index_dir and has $((free_bytes / 1000000000)) GB" >&2
      exit 1
    fi

    rm -f "$out/b8_full-$profile.csv"
    compare=()
    [ -f "$existing" ] && compare=(--compare "$existing")
    for t in "${thread_counts[@]}"; do
      for budget in ${budgets//,/ }; do
        for run in $(seq 1 "$repeat"); do
          echo "b8: $t threads, budget $budget, run $run of $repeat"
          "$bench" full-build --corpus "$corpus" --budget "$budget" --threads "$t" \
            --tmp-dir "$tmp_dir" --out "$candidate" "${compare[@]}" \
            --machine "$machine" --profile "$profile" --commit "$commit"
        done
      done
    done
    rmdir "$tmp_dir" 2> /dev/null || true
    if [ -f "$existing" ]; then
      rm -f "$candidate"
    else
      mv "$candidate" "$existing"
      echo "kept the index as $existing"
    fi
    echo "wrote $out/b8_full-$profile.csv"
    ;;
  *) usage ;;
esac
