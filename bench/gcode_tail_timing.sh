#!/usr/bin/env bash
# gcode-tail: print the seam_placer.init / process_layers / _do_export stage timings
# for the two deterministic models at a given core set. min-of-N for stability.
#   ORCA_SEAM_GPU=0 bench/gcode_tail_timing.sh <bin> <cpuset> [runs]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build-spike/src/orca-slicer}"
CPUSET="${2:-0-7}"
RUNS="${3:-3}"
MODELS_DIR="$REPO/resources/handy_models"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
unset DISPLAY
export ORCA_SEAM_GPU="${ORCA_SEAM_GPU:-0}"
export ORCA_STAGE_TIMING=1
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)
MODELS=(3DBenchy OrcaToleranceTest)
echo "bin=$BIN cpuset=$CPUSET runs=$RUNS"
for m in "${MODELS[@]}"; do
    best_export=99999; best_seam=99999; best_e2e=99999
    for ((i=0;i<RUNS;i++)); do
        rm -rf "$OUT"/* 2>/dev/null
        t0=$(date +%s.%N)
        log=$(taskset -c "$CPUSET" "$BIN" "${OPTS[@]}" --outputdir "$OUT" "$MODELS_DIR/$m.drc" 2>&1)
        t1=$(date +%s.%N)
        e2e=$(echo "($t1-$t0)*1000" | bc)
        seam=$(echo "$log" | grep "seam_placer.init" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
        exp=$(echo "$log" | grep "_do_export (tool" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
        awk -v a="$exp" -v b="$best_export" 'BEGIN{exit !(a<b)}' && best_export=$exp
        awk -v a="$seam" -v b="$best_seam" 'BEGIN{exit !(a<b)}' && best_seam=$seam
        awk -v a="$e2e" -v b="$best_e2e" 'BEGIN{exit !(a<b)}' && best_e2e=$e2e
    done
    printf '%-20s seam_init=%8.1f  _do_export=%8.1f  e2e=%8.1f ms\n' "$m" "$best_seam" "$best_export" "$best_e2e"
done
