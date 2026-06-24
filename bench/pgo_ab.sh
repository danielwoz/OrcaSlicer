#!/usr/bin/env bash
# Interleaved A/B cold-slice benchmark: control (light-suite PGO) vs patched
# (heavy-workload PGO). Both are full production LTO+PGO+mimalloc builds, byte-for-byte
# the same source — only the PGO profile (codegen) differs.
#
# Interleaves the two binaries run-by-run (control, patched, control, ...) so transient
# host load hits both equally; reports min-of-N per binary (most reproducible estimator).
# Pinned to cpuset 24-31. Heavy KitCardAMS (--slice 0) + handy suite (heavy settings).
#
# Usage: pgo_ab.sh <control-bin> <patched-bin> [runs]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CTRL="${1:?control binary}"
PATCHED="${2:?patched binary}"
RUNS="${3:-9}"
MODELS_DIR="$REPO/resources/handy_models"
HEAVY="$REPO/bench/bench_models/KitCardAMS.3mf"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
unset DISPLAY
export ORCA_SEAM_GPU=0
PIN=(taskset -c 24-31)
SUITE_OPTS=(--use-relative-e-distances=0 --layer-height=0.12 --sparse-infill-density=35% --wall-loops=4 --arrange 1 --slice 0)

now(){ date +%s.%N; }

# slice one model with one binary; echo elapsed seconds
slice_one(){ # bin  model  is_heavy(0/1)
    local bin="$1" model="$2" heavy="$3"
    rm -rf "$OUT"/* 2>/dev/null
    local t0 t1
    t0=$(now)
    if [[ "$heavy" == 1 ]]; then
        "${PIN[@]}" "$bin" --slice 0 --outputdir "$OUT" "$model" >/dev/null 2>&1
    else
        "${PIN[@]}" "$bin" "${SUITE_OPTS[@]}" --outputdir "$OUT" "$model" >/dev/null 2>&1
    fi
    t1=$(now)
    echo "$t1 - $t0" | bc
}

declare -A CMIN PMIN
bench_model(){ # label  model  is_heavy
    local label="$1" model="$2" heavy="$3"
    [[ -f "$model" ]] || { echo "MISSING $label"; return; }
    # warmup both (discarded)
    slice_one "$CTRL" "$model" "$heavy" >/dev/null
    slice_one "$PATCHED" "$model" "$heavy" >/dev/null
    local cmin=999999 pmin=999999 c p
    for ((i=0;i<RUNS;i++)); do
        c=$(slice_one "$CTRL" "$model" "$heavy")
        p=$(slice_one "$PATCHED" "$model" "$heavy")
        (( $(echo "$c < $cmin"|bc) )) && cmin=$c
        (( $(echo "$p < $pmin"|bc) )) && pmin=$p
    done
    CMIN[$label]=$cmin; PMIN[$label]=$pmin
    local delta; delta=$(echo "scale=2; ($pmin-$cmin)/$cmin*100" | bc)
    printf '%-22s ctrl=%8.3f  heavyPGO=%8.3f  Δ=%+6.2f%%\n' "$label" "$cmin" "$pmin" "$delta"
}

echo "control(light-PGO): $CTRL"
echo "patched(heavy-PGO): $PATCHED"
echo "runs/model: $RUNS interleaved, min reported, cpuset 24-31"
echo "load@start: $(uptime | grep -oE 'load average.*')"
echo "------------------------------------------------------------------------"
bench_model "KitCardAMS(heavy)" "$HEAVY" 1
for m in 3DBenchy ksr_fdmtest_v4 OrcaCube_v2 OrcaToleranceTest Orca_stringhell; do
    bench_model "$m" "$MODELS_DIR/$m.drc" 0
done
echo "------------------------------------------------------------------------"
# Aggregate suite (handy 5) and heavy+suite totals from the per-model mins.
csuite=0; psuite=0
for m in 3DBenchy ksr_fdmtest_v4 OrcaCube_v2 OrcaToleranceTest Orca_stringhell; do
    csuite=$(echo "$csuite + ${CMIN[$m]}"|bc); psuite=$(echo "$psuite + ${PMIN[$m]}"|bc)
done
ch=${CMIN["KitCardAMS(heavy)"]}; ph=${PMIN["KitCardAMS(heavy)"]}
call=$(echo "$csuite + $ch"|bc); pall=$(echo "$psuite + $ph"|bc)
sd=$(echo "scale=2; ($psuite-$csuite)/$csuite*100"|bc)
hd=$(echo "scale=2; ($ph-$ch)/$ch*100"|bc)
ad=$(echo "scale=2; ($pall-$call)/$call*100"|bc)
printf 'SUITE (5)        ctrl=%8.3f  heavyPGO=%8.3f  Δ=%+6.2f%%\n' "$csuite" "$psuite" "$sd"
printf 'HEAVY            ctrl=%8.3f  heavyPGO=%8.3f  Δ=%+6.2f%%\n' "$ch" "$ph" "$hd"
printf 'HEAVY+SUITE      ctrl=%8.3f  heavyPGO=%8.3f  Δ=%+6.2f%%\n' "$call" "$pall" "$ad"
echo "load@end: $(uptime | grep -oE 'load average.*')"
