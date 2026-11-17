#!/usr/bin/env bash
# Builds and tests every preset: debug, asan, tsan and release. Run before every commit.
set -u
cd "$(dirname "$0")/.."
failed=0
for preset in debug asan tsan release; do
  cmake --preset "$preset" > /dev/null || { echo "$preset: configure failed"; failed=1; continue; }
  if ! cmake --build --preset "$preset" 2>&1 | grep -E "error|warning:" -A4 | head -20 | grep . ; then
    :  # no errors or warnings printed
  fi
  result=$(ctest --preset "$preset" 2>&1 | grep -E "tests passed|tests failed|FAILED|Not Run")
  echo "$preset: ${result:-no test result (build failed?)}"
  echo "$result" | grep -q "100% tests passed" || failed=1
done
exit $failed
