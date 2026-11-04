#!/usr/bin/env bash
# Checks a packaged OrcaSlicer: it binds every built-in infill pattern library, and it slices the 20 mm cube of
# tests/data with the default settings of a printer that resets the extruder at each layer.
#
#   check_packaged_infill_patterns.sh <command that starts the application>...
#
# for example check_packaged_infill_patterns.sh ./OrcaSlicer.AppImage --appimage-extract-and-run, or
# check_packaged_infill_patterns.sh arch -x86_64 OrcaSlicer.app/Contents/MacOS/OrcaSlicer.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir" "$WORK/out"

"$@" --check-infill-patterns

# The default printer extrudes relatively, which needs G92 E0 at each layer change.
printf '{ "type": "machine", "from": "User", "name": "Packaged application check", "layer_change_gcode": "G92 E0" }\n' \
    > "$WORK/machine.json"
"$@" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json" --arrange 1 --slice 0 --outputdir "$WORK/out" \
    "$ROOT_DIR/tests/data/20mm_cube.obj"
gcode="$WORK/out/plate_1.gcode"
if ! grep -q '^G1 ' "$gcode" 2>/dev/null; then
    echo "error: slicing tests/data/20mm_cube.obj wrote no G-code" >&2
    exit 1
fi
echo "OK sliced tests/data/20mm_cube.obj: $(grep -c '^G1 ' "$gcode") moves, $(grep -m1 '^; sparse_infill_pattern = ' "$gcode")"
