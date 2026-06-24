#!/usr/bin/env bash
# PGO training workload — BROADENED (heavy-workload retrain).
#
# Drives an INSTRUMENTED (-fprofile-generate) orca-slicer through a representative
# training set so the merged profile covers the hot loops of the real cost stages:
#   - Clipper2 boolean compute + Arachne          (light suite, heavy settings)
#   - AMS mm_segmentation + heavy boolean compute (KitCardAMS.3mf, multi-material)
#   - Support generation (tree + normal)          (3DBenchy/OrcaToleranceTest + supports)
#
# vs. the original LIGHT training set (5 handy models only, bench/PGO.md), this adds
# KitCardAMS and three support-on slices.
#
# Writes raw profiles to $PGO_DIR (default /tmp/pgo_heavy_raw). Merge afterwards with:
#   llvm-profdata-14 merge -o <out>.profdata $(find $PGO_DIR -name '*.profraw' -size +0c)
#
# Usage: bench/pgo_train_heavy.sh /path/to/instrumented/orca-slicer [PGO_DIR]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$REPO/build/src/Release/orca-slicer}"
PGO_DIR="${2:-/tmp/pgo_heavy_raw}"
MODELS="$REPO/resources/handy_models"
HEAVY="$REPO/bench/bench_models/KitCardAMS.3mf"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
unset DISPLAY
export ORCA_SEAM_GPU=0
PIN=(taskset -c 24-31)
mkdir -p "$PGO_DIR"

# Same heavier settings the benchmark/light-PGO uses (amplifies infill/perimeter/clipper).
OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)

run() {  # tag  extra-opts...  model
    local tag="$1"; shift
    local model="${@: -1}"; local opts=("${@:1:$#-1}")
    LLVM_PROFILE_FILE="$PGO_DIR/${tag}-%p.profraw" \
        "${PIN[@]}" "$BIN" "${opts[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1
    echo "trained: $tag (exit $?)"
    rm -rf "$OUT"/* 2>/dev/null
}

# 1. Light suite (5 models, heavy settings) — Clipper/Arachne/Fill/perimeter hot loops.
for m in 3DBenchy ksr_fdmtest_v4 OrcaCube_v2 OrcaToleranceTest Orca_stringhell; do
    run "$m" "${OPTS[@]}" "$MODELS/$m.drc"
done

# 2. Heavy / AMS multi-material — boolean compute + mm_segmentation.
run "KitCardAMS" --slice 0 "$HEAVY"

# 3. Support-heavy — tree + normal support generation code paths.
run "3DBenchy_treesup"  --use-relative-e-distances=0 --layer-height=0.12 \
    --enable-support=1 --support-type=tree   --support-threshold-angle=30 --arrange 1 --slice 0 "$MODELS/3DBenchy.drc"
run "3DBenchy_normsup"  --use-relative-e-distances=0 --layer-height=0.12 \
    --enable-support=1 --support-type=normal --support-threshold-angle=40 --arrange 1 --slice 0 "$MODELS/3DBenchy.drc"
run "OrcaTol_treesup"   --use-relative-e-distances=0 --layer-height=0.12 \
    --enable-support=1 --support-type=tree   --support-threshold-angle=30 --arrange 1 --slice 0 "$MODELS/OrcaToleranceTest.drc"

echo "---"
echo "raw profiles in $PGO_DIR:"
ls -1 "$PGO_DIR"/*.profraw 2>/dev/null | wc -l
