#!/usr/bin/env bash
# End-to-end checks that --load-filaments may load more filaments, or sparser filament presets,
# than the per-filament arrays of the project or the presets provide.
#
# - Filament presets without filament_diameter or filament_is_support still give each filament its
#   own slot: an object assigned to filament 2 prints with filament 2.
# - A one-filament project sliced with two --load-filaments gets a column per filament in
#   inherits_group and different_settings_to_system, with the printer column still last, so the
#   printer keys the project lists as changed keep their values.
#
# usage: test_cli_load_more_filaments.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-more-filaments.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    # Faces wound counter-clockwise seen from outside: -z, +z, -y, +y, -x, +x.
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((a, b, c), (a, c, d)):
            f.write("facet normal 0 0 0\nouter loop\n")
            for i in tri:
                f.write("vertex %g %g %g\n" % v[i])
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <tag> [option...]: run into $WORK/<tag>, keeping the log there.
run() {
    local out="$WORK/$1"; shift
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" --outputdir "$out" "$@" > "$out/log" 2>&1
    local status=$?
    [ "$status" -eq 0 ] || fail "$(basename "$out"): orca-slicer exited $status"
}

echo "== filament presets without filament_diameter or filament_is_support keep a slot each"
# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{"type": "machine", "from": "User", "name": "CLI more filaments test printer",
 "printable_area": ["0x0", "200x0", "200x200", "0x200"], "printable_height": "100", "layer_change_gcode": "G92 E0"}
EOF
cat > "$WORK/process.json" <<'EOF'
{"type": "process", "from": "User", "name": "CLI more filaments test process"}
EOF
for i in 1 2; do
    cat > "$WORK/filament$i.json" <<EOF
{"type": "filament", "from": "User", "name": "CLI more filaments test filament $i", "nozzle_temperature": ["20$i"]}
EOF
done
cat > "$WORK/assemble.json" <<EOF
{"plates": [{"plate_name": "p", "need_arrange": false,
             "objects": [{"path": "$WORK/cube.stl", "count": 1, "filaments": [2],
                          "pos_x": [100], "pos_y": [100]}]}]}
EOF
run sparse --load-settings "$WORK/machine.json;$WORK/process.json" --load-filaments "$WORK/filament1.json;$WORK/filament2.json" \
    --load-assemble-list "$WORK/assemble.json" --slice 0 --export-3mf out.3mf
"$PY" - "$WORK/sparse/out.3mf" <<'EOF' || fail "sparse: project check"
import json, re, sys, zipfile

try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        config = json.loads(z.read("Metadata/project_settings.config"))
        gcode = z.read("Metadata/plate_1.gcode").decode()
except (OSError, KeyError, ValueError, zipfile.BadZipFile) as e:
    sys.exit("cannot read %s: %s" % (sys.argv[1], e))
errors = []
for key in ("filament_diameter", "filament_is_support"):
    if len(config.get(key, [])) != 2:
        errors.append("%s is %r, want 2 entries" % (key, config.get(key)))
if not re.search(r"^T1\s*$", gcode, re.M):
    errors.append("the object assigned to filament 2 is not printed with it (no T1)")
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF

echo "== a one-filament project sliced with two --load-filaments keeps its printer column last"
PRINTER="Bambu Lab P1S 0.4 nozzle"
PROCESS="0.20mm Standard @BBL X1C"
PLA="$PROFILES/filament/Bambu PLA Basic @BBL X1C.json"
PETG="$PROFILES/filament/Bambu PETG HF @BBL X1C.json"
run export --load-settings "$PROFILES/machine/$PRINTER.json;$PROFILES/process/$PROCESS.json" --load-filaments "$PLA" \
    --slice 0 --export-3mf project.3mf "$WORK/cube.stl"
# The project's printer is a user preset that changed printable_height.
"$PY" - "$WORK/export/project.3mf" "$WORK/project.3mf" "$PRINTER" <<'EOF' || fail "export: project rewrite"
import json, os, sys, zipfile

src, dst, printer = sys.argv[1:4]
if not os.path.exists(src):
    sys.exit("cannot read %s" % src)
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        data = zin.read(info.filename)
        if info.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            config["printer_settings_id"] = "CLI more filaments test printer"
            config["printable_height"] = "240"
            config["inherits_group"] = ["", "", printer]
            config["different_settings_to_system"] = ["", "", "printable_height"]
            data = json.dumps(config, indent=4).encode()
        zout.writestr(info, data)
EOF
run grown --load-filaments "$PLA;$PETG" --slice 0 --export-3mf out.3mf "$WORK/project.3mf"
"$PY" - "$WORK/grown/out.3mf" "$PRINTER" <<'EOF' || fail "grown: project check"
import json, sys, zipfile

try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        config = json.loads(z.read("Metadata/project_settings.config"))
except (OSError, KeyError, ValueError, zipfile.BadZipFile) as e:
    sys.exit("cannot read %s: %s" % (sys.argv[1], e))
errors = []
want = {"inherits_group": sys.argv[2], "different_settings_to_system": "printable_height"}
for key, printer_column in want.items():
    columns = config.get(key, [])
    if len(columns) != 4 or columns[-1] != printer_column:
        errors.append("%s is %r, want 4 entries ending in %r" % (key, columns, printer_column))
if config.get("printable_height") != "240":
    errors.append("printable_height is %r, want the project's 240" % config.get("printable_height"))
if len(config.get("filament_settings_id", [])) != 2:
    errors.append("filament_settings_id is %r, want 2 entries" % config.get("filament_settings_id"))
if sorted(set(config.get("filament_self_index", []))) != ["1", "2"]:
    errors.append("filament_self_index is %r, want entries for filaments 1 and 2" % config.get("filament_self_index"))
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF

if [ "$fails" -ne 0 ]; then
    for log in "$WORK"/*/log; do
        echo "--- $log"
        tail -n 40 "$log"
    done
    exit 1
fi
echo "PASS"
