#!/bin/sh
# Run one scripted-input scenario under PPSSPP with program-counter sampling
# (tools/ppsspp_pc_sampler.py) for code-layout work. The caller owns the
# shared PPSSPP lock. Arguments after OUT go to run-ppsspp-input-script.sh.
#
#   tools/ppsspp-pc-profile.sh OUT.tsv [--hz N] [--backtrace] -- RUNNER-ARGS...
#
# Example (chatgpt journey):
#   TILEFINCH_PPSSPP_CPU_MHZ=333 tools/ppsspp-pc-profile.sh samples.tsv -- \
#     --script chatgpt-ask --measure --trace TRACE --trace-keyed \
#     --boot trace_ignore_request_body=1 --boot trace_volatile_uuids=1 \
#     --boot validation_js_profile=0 --timeout 600
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$1; shift
hz=100
bt=
while [ "$#" -gt 0 ] && [ "$1" != -- ]; do
    case "$1" in
        --hz) hz=$2; shift 2 ;;
        --backtrace) bt=--backtrace; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ "${1:-}" = -- ] && shift
port=${TILEFINCH_PPSSPP_DEBUGGER_PORT:-45123}
run_base=${TMPDIR:?set TMPDIR to a scratch directory}
before=$(ls -d "${run_base%/}"/tilefinch-ppsspp-script.* 2>&1 || true)
TILEFINCH_PPSSPP_DEBUGGER_PORT=$port "$root/scripts/run-ppsspp-input-script.sh" "$@" \
    >"$out.runner.log" 2>&1 &
runner=$!
# The runner creates its session directory before launching the emulator.
log=
i=0
while [ -z "$log" ] && [ "$i" -lt 600 ]; do
    for d in "${run_base%/}"/tilefinch-ppsspp-script.*; do
        case "$before" in *"$d"*) continue ;; esac
        [ -d "$d" ] && log="$d/run-1/home/.config/ppsspp/PSP/GAME/TILEFINCH/tilefinch-validation.txt"
    done
    [ -n "$log" ] || { i=$((i + 1)); python3 -c 'import time; time.sleep(0.2)'; }
done
python3 "$root/tools/ppsspp_pc_sampler.py" --port "$port" --out "$out" \
    --hz "$hz" $bt --log "$log" --reset-dir "$(dirname "$log")" \
    --until-log 'tilefinch-validation: outcome=' --max-seconds 1200 || true
wait "$runner" || echo "runner exit $?" >>"$out.runner.log"
# Keep the run's own log beside the samples: tools/pc_profile_report.py
# --timeline-log splits samples into windows at its load-timeline milestones.
build_dir=$root/build-preset-psp-validation
prev=
for a in "$@"; do
    [ "$prev" = --build-dir ] && build_dir=$a
    case "$a" in --build-dir=*) build_dir=${a#--build-dir=} ;; esac
    prev=$a
done
cp "$build_dir/ppsspp-input-script-latest/run-1/tilefinch-validation.txt" \
    "$out.log" 2>&1 || true
tail -3 "$out.runner.log"
