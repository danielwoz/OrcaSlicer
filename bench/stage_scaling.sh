#!/usr/bin/env bash
# Stage scaling: run the heavy model with ORCA_STAGE_TIMING at several core counts.
# A stage whose wall-time stays ~flat as cores increase is the serial bottleneck.
# Usage: bench/stage_scaling.sh /path/to/orca-slicer
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/src/Release/orca-slicer}"
MODEL="$REPO/bench/bench_models/KitCardAMS.3mf"
unset DISPLAY
export ORCA_SEAM_GPU=0
for cs in "24:1" "24-31:8" "16-31:16"; do
  cpuset="${cs%:*}"; n="${cs#*:}"
  rm -rf /tmp/o; mkdir -p /tmp/o
  log=/tmp/stage_${n}c.log
  ORCA_STAGE_TIMING=1 taskset -c "$cpuset" "$BIN" --slice 0 --outputdir /tmp/o "$MODEL" 2>"$log" >/dev/null
  echo "===== $n cores ($cpuset) ====="
  grep '\[STAGE\]' "$log"
done
