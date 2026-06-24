#!/usr/bin/env bash
# Smoke test: slice the multi-material KitCardAMS.3mf with precompute ON and OFF and
# report exit code + whether g-code was produced + motion-hash equality.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build-spike/src/orca-slicer}"
MODEL="$REPO/bench/bench_models/KitCardAMS.3mf"
unset DISPLAY
export ORCA_SEAM_GPU="${ORCA_SEAM_GPU:-0}"
run() {
    local out; out="$(mktemp -d)"
    "$BIN" --slice 0 --outputdir "$out" "$MODEL" >/dev/null 2>&1
    local rc=$?
    local g; g="$(ls "$out"/*.gcode 2>/dev/null | head -1)"
    local h="NO_GCODE"
    [[ -n "$g" ]] && h="$(grep -vE '^;|^\s*$' "$g" | md5sum | cut -d' ' -f1)"
    printf 'exit=%s gcode=%s hash=%s\n' "$rc" "$([[ -n "$g" ]] && echo yes || echo no)" "$h"
    rm -rf "$out"
}
echo -n "ON  : "; run
echo -n "OFF : "; ORCA_NO_SEAM_PRECOMPUTE=1 run
