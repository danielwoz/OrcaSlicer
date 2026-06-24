#!/usr/bin/env bash
# Wait for the running deps build to finish, verify it, then build the slicer.
set -u
cd /mnt/cephfs/ssd/OrcaSlicer

echo "[chain] waiting for deps build to finish..."
while pgrep -f "build_linux.sh -d" >/dev/null; do sleep 30; done
echo "[chain] deps build process exited at $(date)"

# Verify deps completed: the final step is [202/202] and no fatal ninja error
if ! grep -qE '\[202/202\]|Completed .dep_OpenCV.' deps_build.log; then
  echo "[chain] WARNING: deps build may not have reached final step. Last lines:"
  tail -15 deps_build.log
fi
if grep -qiE 'FAILED:|ninja: build stopped|Error [0-9]' deps_build.log; then
  echo "[chain] ERROR: deps build reported failures. Aborting slicer build."
  grep -iE 'FAILED:|ninja: build stopped|Error [0-9]' deps_build.log | tail -20
  exit 1
fi

echo "[chain] deps OK. Starting slicer build (./build_linux.sh -s) at $(date)"
./build_linux.sh -s
rc=$?
echo "[chain] slicer build exited rc=$rc at $(date)"
exit $rc
