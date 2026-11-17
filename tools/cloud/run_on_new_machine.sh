#!/usr/bin/env bash
# Runs the whole benchmark suite on a fresh machine, from an empty checkout to a tarball of results.
#
#   tools/cloud/run_on_new_machine.sh <machine-name> <profile> [--threads <sweep>] [--repeat N]
#
#   machine-name  a label for the results folder, for example cloud-32c
#   profile       dev (20,000 docs, a few minutes) or main (150,000 docs)
#   --threads     the thread counts to try, such as 1,2,4,8,16,32; the default is "auto"
#
# It checks the tools and the disk space, sets up the Python environment, downloads and prepares the
# corpus, builds the release binaries, runs the tests, runs every benchmark, and packs
# benchmarks/results/<machine-name>/ into <machine-name>-results.tar.gz in the repository root.
#
# Every step that finished is remembered in data/.done/ and skipped next time, so an interrupted run
# can be started again with the same command. Delete data/.done/ to redo everything.
set -euo pipefail

usage() { echo "usage: $0 <machine-name> <profile: dev|main> [--threads SWEEP] [--repeat N]" >&2; exit 2; }

[ $# -ge 2 ] || usage
machine=$1
profile=$2
shift 2
threads=auto repeat=5
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || usage
  case $1 in
    --threads) threads=$2 ;;
    --repeat) repeat=$2 ;;
    *) usage ;;
  esac
  shift 2
done

case $profile in
  dev) max_docs=20000 need_gb=4 raw_pattern='*pages-articles1.xml-*.bz2' ;;
  main) max_docs=150000 need_gb=16 raw_pattern='*pages-articles[1-4].xml-*.bz2' ;;
  full) echo "error: the full dump is prepared by the Phase 9 steps, not by this script" >&2; exit 2 ;;
  *) usage ;;
esac
case $machine in *[!A-Za-z0-9._-]*|"") echo "error: machine name may only use letters, digits, . _ -" >&2; exit 2 ;; esac

cd "$(dirname "$0")/../.."
mkdir -p data/.done
step() { echo; echo "==== $* ===="; }
done_marker() { [ -f "data/.done/$1" ]; }
mark_done() { touch "data/.done/$1"; }

# ---- 1. tools and disk space ----
step "checking tools and disk space"
missing=0
for tool in cmake ninja python3 wget git tar; do
  command -v "$tool" > /dev/null || { echo "missing: $tool"; missing=1; }
done
python3 -c "import venv, ensurepip" 2> /dev/null || { echo "missing: python3-venv"; missing=1; }
gxx=$(command -v g++-14 || command -v g++ || true)
if [ -z "$gxx" ]; then
  echo "missing: g++ 14 or newer"; missing=1
else
  major=$("$gxx" -dumpversion | cut -d. -f1)
  [ "$major" -ge 14 ] || { echo "g++ is version $major; version 14 or newer is needed"; missing=1; }
fi
command -v gcc > /dev/null || command -v cc > /dev/null || { echo "missing: a C compiler (gcc); googletest needs one"; missing=1; }
[ "$missing" -eq 0 ] || { echo "error: install the missing tools first (see docs/CLOUD.md)" >&2; exit 1; }
free_gb=$(df -P --block-size=1G . | awk 'NR == 2 { print $4 }')
if ! done_marker "corpus-$profile" && [ "$free_gb" -lt "$need_gb" ]; then
  echo "error: $free_gb GB free, the $profile profile needs about $need_gb GB" >&2
  exit 1
fi
# The cold-cache benchmarks evict the index from the page cache, which a memory-backed filesystem
# cannot do; they would stop with an error after the long steps, so refuse here.
case $(stat -f -c %T .) in
  tmpfs|ramfs) echo "error: this folder is on a memory-backed filesystem; use a real disk" >&2; exit 1 ;;
esac
echo "tools ok, $free_gb GB free, $(nproc) logical CPUs, $gxx $major"

# ---- 2. the Python environment ----
if ! done_marker venv; then
  step "setting up the Python environment"
  rm -rf .venv
  if [ "$(python3 -c 'import sys; print(sys.version_info[1])')" -le 10 ]; then
    python3 -m venv .venv
    .venv/bin/pip install --quiet -r tools/requirements.txt
  else
    # WikiExtractor 3.0.6 fails on Python 3.11 and newer ("re.error: global flags not at the
    # start of the expression"), and Ubuntu 24.04 has 3.12. uv fetches a Python 3.10 for the venv.
    rm -rf data/.uv-bootstrap
    python3 -m venv data/.uv-bootstrap
    data/.uv-bootstrap/bin/pip install --quiet uv
    data/.uv-bootstrap/bin/uv venv --python 3.10 .venv
    data/.uv-bootstrap/bin/uv pip install --quiet --python .venv/bin/python -r tools/requirements.txt
  fi
  .venv/bin/python -c "import wikiextractor, matplotlib"  # fail here, not an hour later
  mark_done venv
fi

# ---- 3. the corpus ----
if [ -f "data/corpus/$profile/part-00000.tsv" ] && ! done_marker "corpus-$profile"; then
  mark_done "corpus-$profile"  # prepared by hand earlier
fi
if ! done_marker "corpus-$profile"; then
  step "downloading the $profile dump"
  tools/fetch_wikipedia.sh "$profile"

  step "extracting the articles (this takes a while)"
  rm -rf "data/extracted/$profile"
  for dump in data/raw/$raw_pattern; do
    # one folder per dump file: WikiExtractor names its output the same way for every input
    name=$(basename "$dump" .bz2)
    .venv/bin/python -m wikiextractor.WikiExtractor "$dump" --json \
      -o "data/extracted/$profile/${name#enwiki-*-pages-}" --processes "$(nproc)" --quiet
  done

  step "preparing the corpus"
  rm -rf "data/corpus/$profile"
  mkdir -p benchmarks/results/"$machine"
  .venv/bin/python tools/prepare_corpus.py "data/extracted/$profile" "data/corpus/$profile" \
    --max-docs "$max_docs" --stats-out "benchmarks/results/$machine/corpus-$profile.txt"
  rm -rf data/raw "data/extracted/$profile"
  mark_done "corpus-$profile"
fi

# ---- 4. build and test ----
commit=$(git rev-parse --short HEAD)
if ! done_marker "tests-$commit"; then
  step "building the release binaries and running the tests"
  cmake --preset release > /dev/null
  cmake --build --preset release
  ctest --preset release
  mark_done "tests-$commit"
else
  cmake --preset release > /dev/null
  cmake --build --preset release > /dev/null
fi

# ---- 5. the benchmarks ----
if ! done_marker "bench-$profile-$machine-$commit"; then
  step "running the benchmarks (profile $profile, threads $threads)"
  tools/run_benchmarks.sh all --machine "$machine" --profile "$profile" --threads "$threads" --repeat "$repeat"
  mark_done "bench-$profile-$machine-$commit"
fi

# ---- 6. the tarball ----
step "packing the results"
tarball="$machine-results.tar.gz"
tar -czf "$tarball" -C benchmarks/results "$machine"
echo
echo "Done. Copy $tarball to your own machine, unpack it into benchmarks/results/ and commit it:"
echo "  tar -xzf $tarball -C benchmarks/results"
