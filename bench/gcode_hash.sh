#!/usr/bin/env bash
# Output-identity check: slice the benchmark models and emit a normalized hash of
# the g-code (strip volatile lines: comments, timestamps, version, est. times) so
# an optimization can be verified to NOT change slicing output. Usage:
#   bench/gcode_hash.sh /path/to/orca-slicer
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/src/Release/orca-slicer}"
MODELS_DIR="$REPO/resources/handy_models"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
unset DISPLAY
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)
MODELS=(3DBenchy ksr_fdmtest_v4 OrcaCube_v2 OrcaToleranceTest Orca_stringhell)
for m in "${MODELS[@]}"; do
    model="$MODELS_DIR/$m.drc"; [[ -f "$model" ]] || continue
    rm -rf "$OUT"/* 2>/dev/null
    "$BIN" "${OPTS[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1
    g=$(ls "$OUT"/*.gcode 2>/dev/null | head -1)
    if [[ -n "$g" ]]; then
        # strip volatile lines (comments ';', empty), hash the pure motion commands.
        # GCODE_HASH_STRIP_M73=1 additionally drops M73 progress/time-remaining lines,
        # whose exact line placement is a deterministic-but-noisy artifact of the time
        # estimator's interval bucketing, not a toolpath change.
        strip='^;|^\s*$'
        [[ "${GCODE_HASH_STRIP_M73:-0}" != 0 ]] && strip='^;|^\s*$|^M73 '
        h=$(grep -vE "$strip" "$g" | md5sum | cut -d' ' -f1)
        printf '%-22s %s\n' "$m" "$h"
    else
        printf '%-22s NO_GCODE\n' "$m"
    fi
done
