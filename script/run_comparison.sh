#!/usr/bin/env bash
#
# Builds and runs the MPHF comparison benchmark (PTHash in the SSHash
# configurations vs. PHast vs. PHastR) and saves the full log, whose tail
# is a markdown summary table with the medians over the runs.
#
# Usage:
#   ./script/run_comparison.sh [num_keys] [num_lookups] [num_runs] [seed] [num_threads]
#
# Defaults: 300M keys, 10M lookups, 3 runs, seed = 1, all cores.
#
# Notes:
#   - requires the pthash submodule: git submodule update --init --recursive
#   - memory: the benchmark keeps the keys (8 bytes per key) plus one
#     structure under construction at a time; 300M keys need about 10 GiB
#     of RAM and, for the external-memory builds, about 5 GiB of free disk
#     in the build directory.

set -euo pipefail

NUM_KEYS=${1:-300000000}
NUM_LOOKUPS=${2:-10000000}
NUM_RUNS=${3:-3}
SEED=${4:-1}
NUM_THREADS=${5:-$(nproc)}

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -f external/pthash/CMakeLists.txt ]; then
    echo "error: the pthash submodule is missing; run:"
    echo "    git submodule update --init --recursive"
    exit 1
fi

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null
make -j"$(nproc)" compare_mphf

LOG="$ROOT/comparison-${NUM_KEYS}keys-${NUM_THREADS}threads-$(date +%Y%m%d-%H%M%S).log"
echo "logging to $LOG"
./compare_mphf "$NUM_KEYS" "$NUM_LOOKUPS" "$NUM_RUNS" "$SEED" "$NUM_THREADS" 2>&1 | tee "$LOG"

echo
echo "done; summary table at the end of $LOG"
