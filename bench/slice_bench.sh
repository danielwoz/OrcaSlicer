#!/usr/bin/env bash
# End-to-end slicing benchmark for OrcaSlicer.
#
# Slices a fixed set of representative models headless to G-code and reports the
# best (min) wall-clock per model over N runs, plus the suite total. Min-of-N is
# used because it is the most reproducible estimator for a compute benchmark
# (filters out scheduler/IO interference). Use it to measure the effect of build
# optimizations (mimalloc / ThinLTO / PGO): run against the baseline binary, then
# against each optimized binary, and compare the suite totals.
#
# Usage:   bench/slice_bench.sh [/path/to/orca-slicer] [runs]
# Default binary: build/src/Release/orca-slicer ; default runs: 5
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/src/Release/orca-slicer}"
RUNS="${2:-8}"
MODELS_DIR="$REPO/resources/handy_models"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
unset DISPLAY   # slicing is headless; avoids any GL/display path

# Pin to a fixed CPU set so the benchmark is reproducible regardless of other
# system load: bounds slicing parallelism to a constant width (TBB inherits the
# affinity mask) and keeps per-core compute the dominant cost — which is exactly
# what mimalloc/ThinLTO/PGO improve. Override with BENCH_CPUSET (e.g. "0-7").
BENCH_CPUSET="${BENCH_CPUSET:-24-31}"
PIN=(taskset -c "$BENCH_CPUSET")

# Heavier-than-default settings to amplify the slicing-core signal (infill,
# perimeters, clipper) over fixed startup cost. --use-relative-e-distances=0
# fixes the only invalid default-profile combination for headless slicing.
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)

# Diverse working set (all load + slice cleanly with the default profile).
MODELS=(3DBenchy ksr_fdmtest_v4 OrcaCube_v2 OrcaToleranceTest Orca_stringhell)

if [[ ! -x "$BIN" ]]; then echo "binary not found/executable: $BIN" >&2; exit 1; fi
echo "binary : $BIN"
echo "runs   : $RUNS (min reported), +1 warmup discarded"
echo "cpuset : $BENCH_CPUSET (taskset-pinned)"
echo "opts   : ${OPTS[*]}"
printf '%-22s %10s %10s %8s\n' "model" "min(s)" "median(s)" "gcode"
echo "------------------------------------------------------------"

now(){ date +%s.%N; }
total_min=0
for m in "${MODELS[@]}"; do
    model="$MODELS_DIR/$m.drc"
    [[ -f "$model" ]] || { printf '%-22s  MISSING\n' "$m"; continue; }
    # warmup (populates any caches, discarded)
    rm -rf "$OUT"/* 2>/dev/null; "${PIN[@]}" "$BIN" "${OPTS[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1
    times=()
    gsize="?"
    for ((i=0;i<RUNS;i++)); do
        rm -rf "$OUT"/* 2>/dev/null
        t0=$(now); "${PIN[@]}" "$BIN" "${OPTS[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1; rc=$?; t1=$(now)
        if [[ $rc -ne 0 ]]; then printf '%-22s  FAILED (exit %s)\n' "$m" "$rc"; times=(); break; fi
        times+=("$(echo "$t1 - $t0" | bc)")
        g=$(ls "$OUT"/*.gcode 2>/dev/null | head -1); [[ -n "$g" ]] && gsize=$(du -h "$g" | cut -f1)
    done
    [[ ${#times[@]} -eq 0 ]] && continue
    mn=$(printf '%s\n' "${times[@]}" | sort -n | head -1)
    md=$(printf '%s\n' "${times[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
    printf '%-22s %10.3f %10.3f %8s\n' "$m" "$mn" "$md" "$gsize"
    total_min=$(echo "$total_min + $mn" | bc)
done
echo "------------------------------------------------------------"
printf '%-22s %10.3f\n' "SUITE TOTAL (min)" "$total_min"
echo "BENCH_RESULT suite_min_total=$total_min binary=$BIN runs=$RUNS"
