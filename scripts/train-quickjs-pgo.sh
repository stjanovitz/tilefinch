#!/bin/sh
# Collect a QuickJS PGO profile under PPSSPP (never on a device).
#
#   scripts/train-quickjs-pgo.sh INSTRUMENTED_BUILD OUT_DIR TRACE_CORPUS \
#       [--bench-weight W]
#
# INSTRUMENTED_BUILD is a validation PSP tree configured with
# -DPSP_BROWSER_QUICKJS_PGO_GENERATE=ON (and a .text override, since the
# counters put it far over the ratchet) and built with the
# psp-browser-script target. TRACE_CORPUS holds the chatgpt-ask-send76 trace
# (perf/traces). The chatgpt-ask journey is replayed at 333 MHz; with
# --bench-weight W the engine micro-benchmark is also run and merged at
# weight W (0 < W; the journey weighs 1). The merged .gcda files and a
# PROVENANCE file land in OUT_DIR, ready for
# -DPSP_BROWSER_QUICKJS_PGO_USE=OUT_DIR. The caller owns any lock that
# serializes PPSSPP runs on the machine.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$#" -ge 3 ] || { sed -n '2,19p' "$0" >&2; exit 2; }
build=$(CDPATH= cd -- "$1" && pwd)
out=$2
corpus=$(CDPATH= cd -- "$3" && pwd)
shift 3
bench_weight=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --bench-weight) bench_weight=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
: "${PSPDEV:?export PSPDEV}"
gcov_tool="$PSPDEV/bin/psp-gcov-tool"
grep -q '^PSP_BROWSER_QUICKJS_PGO_GENERATE:BOOL=ON$' "$build/CMakeCache.txt" || {
    echo "$build is not an instrumented (PGO_GENERATE) build" >&2; exit 2; }
trace="$corpus/chatgpt-ask-send76"
[ -d "$trace" ] || { echo "missing trace $trace" >&2; exit 2; }
work="$build/pgo-training"
rm -rf "$work"
mkdir -p "$work"

run() {  # run NAME RUNNER-ARGS...: one scripted run; keep its counters
    name=$1; shift
    TILEFINCH_PPSSPP_CPU_MHZ=333 TILEFINCH_PPSSPP_GRAPHICS=vulkan \
        "$root/scripts/run-ppsspp-input-script.sh" --build-dir "$build" \
        --measure --timeout 1500 "$@" >"$work/$name.out" 2>&1 || true
    latest="$build/ppsspp-input-script-latest/run-1"
    grep -q 'tilefinch-pgo: counters written' \
        "$latest/tilefinch-validation.txt" && [ -d "$latest/pgo" ] || {
        echo "$name: no profile counters (see $work/$name.out)" >&2; exit 1; }
    cp "$latest/tilefinch-validation.txt" "$work/$name.log"
    cp -R "$latest/pgo" "$work/$name"
}

run journey --script chatgpt-ask --url https://chatgpt.com/ \
    --trace "$trace" --trace-keyed --boot trace_ignore_request_body=1 \
    --boot trace_volatile_uuids=1 --boot validation_js_profile=0
grep -q 'tilefinch-input-script: outcome=complete' "$work/journey.log" || {
    echo "journey did not complete (see $work/journey.log)" >&2; exit 1; }
rm -rf "$out"
if [ -n "$bench_weight" ]; then
    # The runner's native-HOME gate fails after a bench boot by design.
    run bench --script js-bench --trace "$trace" --boot validation_js_bench=1
    "$gcov_tool" merge -w "1,$bench_weight" -o "$out" \
        "$work/journey" "$work/bench"
else
    mkdir -p "$out"
    cp "$work/journey"/*.gcda "$out/"
fi
fingerprint=$(sed -n 's/^TILEFINCH_QUICKJS_PGO_FINGERPRINT:INTERNAL=//p' \
    "$build/CMakeCache.txt")
[ -n "$fingerprint" ] || { echo "no engine fingerprint in $build" >&2; exit 1; }
{
    echo "fingerprint=$fingerprint"
    echo "commit=$(git -C "$root" rev-parse HEAD)"
    echo "trace=chatgpt-ask-send76"
    echo "bench-weight=${bench_weight:-0}"
    echo "milestones:"
    sed -n 's/^\(tilefinch-input-script: until-met.*\)$/  \1/p' \
        "$work/journey.log"
} >"$out/PROVENANCE"
ls -l "$out"
