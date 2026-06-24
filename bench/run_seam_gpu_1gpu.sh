#!/usr/bin/env bash
# Run OrcaSlicer with the Vulkan seam-visibility GPU path, restricted to ONE GPU.
# On multi-GPU NVIDIA hosts, vkCreateInstance eagerly inits ALL GPUs (~4.3s on a
# 3-GPU box); restricting to one cuts that to ~0.16s. We hide the other GPU device
# nodes from THIS process only, via an unprivileged user+mount namespace -- it does
# NOT affect other processes using those GPUs. Single-GPU machines don't need this.
#   ORCA_GPU_KEEP=N  -> keep /dev/nvidiaN (default 0)
#   ORCA_SLICER=...  -> slicer binary (default build/src/Release/orca-slicer)
# Usage: bench/run_seam_gpu_1gpu.sh <orca-slicer args...>
set -u
KEEP="${ORCA_GPU_KEEP:-0}"
SLICER="${ORCA_SLICER:-/mnt/cephfs/ssd/OrcaSlicer/build/src/Release/orca-slicer}"
BLK="$(mktemp)"; chmod 000 "$BLK"
HIDE=""
for n in /dev/nvidia[0-9]*; do
  [ "${n#/dev/nvidia}" = "$KEEP" ] && continue
  HIDE="$HIDE mount --bind '$BLK' '$n';"
done
exec unshare --user --map-root-user --mount bash -c \
  "$HIDE exec env ORCA_SEAM_GPU=1 \"\$@\"" _ "$SLICER" "$@"
