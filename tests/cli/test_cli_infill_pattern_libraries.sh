#!/usr/bin/env bash
# End-to-end check of the built-in infill pattern libraries with the real orca-slicer binary.
#
# --check-infill-patterns lists the libraries and exits with 0 when every one is bound, whatever the infill plugin
# switches say. In a copy of the application without one of the libraries it exits with 1, the start-up names the
# file, and slicing is refused at validation, naming the file, with CLI_VALIDATE_ERROR.
#
# usage: test_cli_infill_pattern_libraries.sh <orca-slicer binary>
set -u

BIN="${1:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
[ -d "$BIN_DIR/infill_patterns" ] || { echo "SKIP: no infill_patterns folder next to $BIN"; exit 77; }

# From src/libslic3r/Utils.hpp. main() returns them, so the shell sees them modulo 256.
CLI_VALIDATE_ERROR=-51
MISSING=orca_infill_crosshatch.so
MODEL="$(cd "$(dirname "$0")/../data" && pwd)/20mm_cube.obj"

# The copy sits next to the folder of the binary, so that it finds the same resources folder. The binary is hard
# linked when the file system allows it; everything else next to it is linked.
WORK="$(mktemp -d "$(dirname "$BIN_DIR")/cli-infill-pattern-libraries.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
APP="$WORK/app"
mkdir -p "$APP/infill_patterns" "$WORK/datadir"
for entry in "$BIN_DIR"/*; do
    case "$(basename "$entry")" in
        "$(basename "$BIN")" | infill_patterns) ;;
        *) ln -s "$entry" "$APP/$(basename "$entry")" ;;
    esac
done
ln "$BIN" "$APP/$(basename "$BIN")" 2>/dev/null || cp "$BIN" "$APP/$(basename "$BIN")"
cp "$BIN_DIR"/infill_patterns/* "$APP/infill_patterns/"
rm -f "$APP/infill_patterns/$MISSING"
COPY="$APP/$(basename "$BIN")"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'JSON'
{
    "type": "machine",
    "from": "User",
    "name": "CLI infill pattern library test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
JSON
cat > "$WORK/process.json" <<'JSON'
{
    "type": "process",
    "from": "User",
    "name": "CLI infill pattern library test process",
    "enable_support": "0",
    "enforce_support_layers": "0"
}
JSON

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <tag> <binary> [option...]: keeps the standard output, standard error and the shell status in $WORK/<tag>.
run() {
    local out="$WORK/$1"; shift
    local bin="$1"; shift
    mkdir -p "$out"
    timeout 300 "$bin" "$@" > "$out/stdout" 2> "$out/stderr"
    echo $? > "$out/status"
}
status() { cat "$WORK/$1/status"; }

echo "== every library is bound"
run check "$BIN" --check-infill-patterns
[ "$(status check)" -eq 0 ] || fail "check: status $(status check), want 0"
grep -q -P '^2\trectilinear\torca_infill_rectilinear\.so\tbound\t' "$WORK/check/stdout" || fail "check: rectilinear is not bound"
grep -q -P '^20\tcrosshatch\torca_infill_crosshatch\.so\tbound\t' "$WORK/check/stdout" || fail "check: cross hatch is not bound"
grep -q -v -P '^#|\tbound\t' "$WORK/check/stdout" && fail "check: a library is not bound"
[ -s "$WORK/check/stderr" ] && fail "check: start-up reported an error"

echo "== whatever the infill plugin switches say"
ORCA_INFILL_PLUGINS=0 run env "$BIN" --check-infill-patterns
[ "$(status env)" -eq 0 ] || fail "ORCA_INFILL_PLUGINS=0: status $(status env), want 0"
run flag "$BIN" --no-infill-plugins --check-infill-patterns
[ "$(status flag)" -eq 0 ] || fail "--no-infill-plugins: status $(status flag), want 0"

echo "== a library is missing"
run missing-check "$COPY" --check-infill-patterns
[ "$(status missing-check)" -eq 1 ] || fail "missing check: status $(status missing-check), want 1"
grep -q -P "^20\tcrosshatch\t$MISSING\tmissing\t" "$WORK/missing-check/stdout" || fail "missing check: cross hatch is not reported missing"
grep -q -P '^2\trectilinear\torca_infill_rectilinear\.so\tbound\t' "$WORK/missing-check/stdout" || fail "missing check: rectilinear is not bound"
grep -q -F "error: infill pattern library $MISSING: missing" "$WORK/missing-check/stderr" || fail "missing check: no start-up error"

echo "== slicing without it"
run slice "$BIN" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$WORK/process.json" --slice 0 \
    --outputdir "$WORK/slice" "$MODEL"
[ "$(status slice)" -eq 0 ] || fail "slice: status $(status slice), want 0"
run missing-slice "$COPY" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$WORK/process.json" --slice 0 \
    --outputdir "$WORK/missing-slice" "$MODEL"
[ "$(status missing-slice)" -eq $(( CLI_VALIDATE_ERROR & 255 )) ] ||
    fail "missing slice: status $(status missing-slice), want $(( CLI_VALIDATE_ERROR & 255 )) (CLI_VALIDATE_ERROR)"
grep -q -F "Infill pattern library $MISSING is missing or damaged (missing). Reinstall OrcaSlicer." "$WORK/missing-slice/stderr" ||
    fail "missing slice: the validation error does not name $MISSING"
ls "$WORK/missing-slice"/*.gcode > /dev/null 2>&1 && fail "missing slice: G-code was written"

if [ "$fails" -ne 0 ]; then
    for log in "$WORK"/*/stdout "$WORK"/*/stderr; do
        echo "--- $log"
        tail -n 20 "$log"
    done
    exit 1
fi
echo "PASS"
