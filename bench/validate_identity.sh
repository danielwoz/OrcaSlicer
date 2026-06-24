#!/usr/bin/env bash
# Byte-identity validation on the 2 deterministic models (3DBenchy + OrcaToleranceTest).
# Strips volatile comment/empty lines, hashes pure motion. Usage:
#   ORCA_SEAM_GPU=0 bench/validate_identity.sh /path/to/orca-slicer
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/src/Release/orca-slicer}"
MODELS_DIR="$REPO/resources/handy_models"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
unset DISPLAY
export ORCA_SEAM_GPU=0
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)
MODELS=(3DBenchy OrcaToleranceTest)
for m in "${MODELS[@]}"; do
    model="$MODELS_DIR/$m.drc"; [[ -f "$model" ]] || { printf '%-22s MISSING\n' "$m"; continue; }
    rm -rf "$OUT"/* 2>/dev/null
    "$BIN" "${OPTS[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1
    g=$(ls "$OUT"/*.gcode 2>/dev/null | head -1)
    if [[ -n "$g" ]]; then
        h=$(grep -vE '^;|^\s*$' "$g" | md5sum | cut -d' ' -f1)
        printf '%-22s %s\n' "$m" "$h"
    else
        printf '%-22s NO_GCODE\n' "$m"
    fi
done
