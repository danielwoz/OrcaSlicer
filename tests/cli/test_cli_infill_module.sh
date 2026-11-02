#!/usr/bin/env bash
# End-to-end check of an infill module that adds a pattern and its settings: the worked example
# tests/infill_modules/staggered, built with -DORCA_INFILL_MODULE_DIRS=tests/infill_modules/staggered.
#
# The CLI accepts the pattern's token and the module's settings as flags, slices sparse infill with
# them, and writes the settings into the G-code config block. A wider line distance prints less infill.
#
# usage: test_cli_infill_module.sh <orca-slicer binary>
set -u

BIN="${1:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

MODEL="$(cd "$(dirname "$0")/../data" && pwd)/20mm_cube.obj"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-infill-module.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'JSON'
{
    "type": "machine",
    "from": "User",
    "name": "CLI infill module test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
JSON
cat > "$WORK/process.json" <<'JSON'
{
    "type": "process",
    "from": "User",
    "name": "CLI infill module test process",
    "enable_support": "0",
    "enforce_support_layers": "0"
}
JSON

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <name> <flags...>: slices the cube into $WORK/<name>, keeping the log and the shell status there.
run() {
    local out="$WORK/$1"
    shift
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$WORK/process.json" "$@" \
        --slice 0 --outputdir "$out" "$MODEL" > "$out/log" 2>&1
    echo $? > "$out/status"
}

# The number of extruding moves of the sparse infill; the feature comment depends on the G-code flavor.
sparse_moves() {
    awk '/^;TYPE:|^; FEATURE: / { sparse = ($0 ~ /Sparse infill/); next }
         sparse && /^G1 / && / E[0-9.]/ { n++ }
         END { print n + 0 }' "$1"
}

run default --sparse-infill-pattern=staggered
run wide --sparse-infill-pattern=staggered --staggered-spacing-scale=2

for name in default wide; do
    [ "$(cat "$WORK/$name/status")" -eq 0 ] || fail "$name: shell status $(cat "$WORK/$name/status")"
    [ -f "$WORK/$name/plate_1.gcode" ] || fail "$name: no G-code"
    grep -qx "; sparse_infill_pattern = staggered" "$WORK/$name/plate_1.gcode" || fail "$name: pattern missing from the config block"
done
if [ "$fails" -eq 0 ]; then
    grep -qx "; staggered_spacing_scale = 1" "$WORK/default/plate_1.gcode" || fail "default: staggered_spacing_scale missing from the config block"
    grep -qx "; staggered_spacing_scale = 2" "$WORK/wide/plate_1.gcode" || fail "wide: staggered_spacing_scale missing from the config block"
    default_moves="$(sparse_moves "$WORK/default/plate_1.gcode")"
    wide_moves="$(sparse_moves "$WORK/wide/plate_1.gcode")"
    echo "sparse infill moves: default $default_moves, wide $wide_moves"
    [ "$default_moves" -gt 0 ] || fail "default: no sparse infill"
    [ "$wide_moves" -gt 0 ] && [ "$wide_moves" -lt "$default_moves" ] || fail "wide: sparse infill does not follow the line distance"
fi

if [ "$fails" -ne 0 ]; then
    for log in "$WORK"/*/log; do
        echo "--- $log"
        tail -n 20 "$log"
    done
    exit 1
fi
echo "PASS"
