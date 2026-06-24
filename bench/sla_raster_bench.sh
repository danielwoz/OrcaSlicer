#!/usr/bin/env bash
# SLA rasterization benchmark (AGG / CPU baseline) for OrcaSlicer.
#
# Measures the wall-time of the SLA rasterize step (the discrete `slapsRasterize`
# stage: per-layer polygon fill + PNG encode) across SLA panel resolutions
# (4K -> 8K -> 12K). This is Phase 1 of the GPU SLA rasterization project: it
# establishes the CPU baseline the Vulkan backend will be measured against, and
# calibrates the GPU-vs-CPU selection threshold.
#
# It does NOT use the OrcaSlicer CLI: headless end-to-end SLA slicing is disabled
# in src/OrcaSlicer.cpp. Instead it runs the dedicated `sla_raster_bench`
# executable, which exercises the real libslic3r raster classes
# (sla::RasterGrayscaleAAGammaPower::draw / ::encode) on layers sliced from a
# real mesh in resources/handy_models/. Min-of-N is used (most reproducible
# estimator for a compute benchmark); taskset pins to a fixed cpuset.
#
# Usage:   bench/sla_raster_bench.sh [mesh.drc] [layers] [runs]
# Defaults: mesh=resources/handy_models/Stanford_Bunny.drc  layers=200  runs=5
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/build/tests/sla_print/Release/sla_raster_bench"
MESH="${1:-$REPO/resources/handy_models/Stanford_Bunny.drc}"
LAYERS="${2:-200}"
RUNS="${3:-5}"
RESULTS="$REPO/bench/SLA_RESULTS.md"
unset DISPLAY  # headless; no GL/display path

# Pin to a fixed CPU set for reproducibility (override with BENCH_CPUSET).
BENCH_CPUSET="${BENCH_CPUSET:-24-31}"
PIN=(taskset -c "$BENCH_CPUSET")

# Build the benchmark target if it is missing (tests must be enabled).
if [[ ! -x "$BIN" ]]; then
    echo "benchmark binary not found, building it..." >&2
    cmake -S "$REPO" -B "$REPO/build" -DBUILD_TESTS=ON >/dev/null 2>&1
    cmake --build "$REPO/build" --config Release --target sla_raster_bench -j"$(nproc)" \
        || { echo "build failed" >&2; exit 1; }
fi
[[ -x "$BIN" ]] || { echo "binary not found/executable: $BIN" >&2; exit 1; }

echo "binary : $BIN"
echo "mesh   : $MESH"
echo "layers : $LAYERS"
echo "runs   : $RUNS (min reported), +1 warmup discarded"
echo "cpuset : $BENCH_CPUSET (taskset-pinned)"
echo

OUT="$("${PIN[@]}" "$BIN" "$MESH" "$LAYERS" "$RUNS" 2>/dev/null)"
echo "$OUT"

# Persist the machine-parseable BENCH_RESULT lines plus the table into the
# results file so the AGG baseline is recorded under version control.
{
    echo ""
    echo "## Run $(date -u '+%Y-%m-%dT%H:%M:%SZ')  (cpuset $BENCH_CPUSET, runs=$RUNS, layers=$LAYERS)"
    echo ""
    echo '```'
    echo "$OUT" | grep -v '^\['
    echo '```'
} >> "$RESULTS"

echo
echo "appended results to $RESULTS"
