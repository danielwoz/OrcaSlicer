#!/usr/bin/env bash
# Workaround for the Ninja Multi-Config segfault in this build dir.
# Recompiles changed libslic3r .cpp files, updates liblibslic3r.a, relinks orca-slicer
# by replaying the exact commands recorded in CMakeFiles/impl-Release.ninja.
# Usage: bench/rebuild_obj.sh src/libslic3r/PrintObject.cpp [more .cpp ...]
set -euo pipefail
ROOT=/mnt/cephfs/ssd/OrcaSlicer-gpudiff
BUILD="$ROOT/build"
IMPL="$BUILD/CMakeFiles/impl-Release.ninja"
cd "$BUILD"

# Extract a per-build-target variable value from the ninja file.
# $1 = the literal "build <out>:" target prefix to anchor on, $2 = var name
extract_var() {
  awk -v anchor="$1" -v var="$2" '
    index($0, anchor)==1 {found=1; next}
    found && $0 ~ ("^  " var " =") { sub("^  " var " = ",""); print; exit }
    found && /^build / {exit}
  ' "$IMPL"
}

CXX=/usr/bin/clang++
for SRC in "$@"; do
  REL=${SRC#"$ROOT/"}; REL=${REL#./}
  OBJ="${REL/src\/libslic3r\//src/libslic3r/CMakeFiles/libslic3r.dir/Release/}.o"
  ANCHOR="build $OBJ:"
  echo ">> compiling $REL"
  DEFINES=$(extract_var "$ANCHOR" DEFINES)
  FLAGS=$(extract_var "$ANCHOR" FLAGS)
  INCLUDES=$(extract_var "$ANCHOR" INCLUDES)
  DEPFILE="${OBJ}.d"
  mkdir -p "$(dirname "$OBJ")"
  eval $CXX $DEFINES $INCLUDES $FLAGS -MD -MT "$OBJ" -MF "$DEPFILE" -o "$OBJ" -c "$ROOT/$REL"
  echo ">> ar update liblibslic3r.a <- $OBJ"
  /usr/bin/ar r src/libslic3r/Release/liblibslic3r.a "$OBJ"
done
/usr/bin/ranlib src/libslic3r/Release/liblibslic3r.a

echo ">> relinking orca-slicer"
ANCHOR="build src/Release/orca-slicer:"
LFLAGS=$(extract_var "$ANCHOR" FLAGS)
LINK_FLAGS=$(extract_var "$ANCHOR" LINK_FLAGS || true)
LINK_PATH=$(extract_var "$ANCHOR" LINK_PATH)
LINK_LIBRARIES=$(extract_var "$ANCHOR" LINK_LIBRARIES)
TARGET_FILE=$(extract_var "$ANCHOR" TARGET_FILE)
IN=src/CMakeFiles/OrcaSlicer.dir/Release/OrcaSlicer.cpp.o
eval $CXX $LFLAGS $LINK_FLAGS "$IN" -o "$TARGET_FILE" $LINK_PATH $LINK_LIBRARIES
echo ">> done -> $TARGET_FILE"
