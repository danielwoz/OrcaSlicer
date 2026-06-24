#!/usr/bin/env bash
# Compare the time-estimate + filament-stats comments between two binaries on the
# deterministic models. Behind-the-scenes optimizations (e.g. the seam-occlusion
# precompute) must leave these byte-identical. Usage:
#   ORCA_SEAM_GPU=0 bench/validate_time_estimate.sh <baseline_bin> <new_bin>
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B1="${1:?baseline bin}"
B2="${2:?new bin}"
MODELS_DIR="$REPO/resources/handy_models"
O1="$(mktemp -d)"; O2="$(mktemp -d)"; trap 'rm -rf "$O1" "$O2"' EXIT
unset DISPLAY
export ORCA_SEAM_GPU="${ORCA_SEAM_GPU:-0}"
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)
MODELS=(3DBenchy OrcaToleranceTest)
PAT='estimated printing time|filament used|total filament|model printing time|total estimated'
rc=0
for m in "${MODELS[@]}"; do
    model="$MODELS_DIR/$m.drc"; [[ -f "$model" ]] || { printf '%-20s MISSING\n' "$m"; continue; }
    rm -rf "$O1"/* "$O2"/* 2>/dev/null
    "$B1" "${OPTS[@]}" --outputdir "$O1" "$model" >/dev/null 2>&1
    "$B2" "${OPTS[@]}" --outputdir "$O2" "$model" >/dev/null 2>&1
    g1=$(ls "$O1"/*.gcode 2>/dev/null | head -1)
    g2=$(ls "$O2"/*.gcode 2>/dev/null | head -1)
    grep -iE "$PAT" "$g1" | sort -u > "$O1/stats.txt"
    grep -iE "$PAT" "$g2" | sort -u > "$O2/stats.txt"
    if diff -q "$O1/stats.txt" "$O2/stats.txt" >/dev/null; then
        printf '%-20s IDENTICAL time/filament stats (%d lines)\n' "$m" "$(wc -l < "$O1/stats.txt")"
    else
        printf '%-20s DIFFER:\n' "$m"; diff "$O1/stats.txt" "$O2/stats.txt"; rc=1
    fi
done
exit $rc
