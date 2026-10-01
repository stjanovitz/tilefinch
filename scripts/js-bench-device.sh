#!/bin/sh
# Stage a JavaScript engine micro-benchmark run for the physical PSP over
# PSPLink (host0, zero Memory Stick writes) and print the launch command.
# It does not touch the device itself.
#
#   scripts/js-bench-device.sh [BUILD_DIR] [TRACE_DIR] [SCALE]
#
# BUILD_DIR  a PSP validation build (default build-preset-psp-validation;
#            build-psp-qjs-o2 for the engine-at--O2 variant)
# TRACE_DIR  an HTTP trace whose JavaScript records the bench compiles
#            (default ../tf-replay/perf/traces/chatgpt-ask-send76)
# SCALE      validation_js_bench iteration scale (default 1; about 35 s of
#            kernels under PPSSPP at 333 MHz, 2-3x that on the device)
#
# The report is the run's tilefinch-validation.txt: every
# `tilefinch-js-bench:` line, ending in `outcome=complete`. Compare with
#   python3 tools/js_bench_compare.py host=HOST.txt ppsspp=PPSSPP.txt device=LOG
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
H=${1:-$ROOT/build-preset-psp-validation}
TRACE=${2:-$ROOT/../tf-replay/perf/traces/chatgpt-ask-send76}
SCALE=${3:-1}
[ -f "$H/EBOOT.PBP" ] || { echo "no EBOOT.PBP in $H" >&2; exit 2; }
[ -f "$TRACE/trace.meta" ] || { echo "no trace.meta in $TRACE" >&2; exit 2; }
for f in profile.cfg profile.cfg.bak local-storage.bin local-storage.bin.bak \
         http-cache.bin http-cache.bin.bak site-storage modcache; do
    rm -rf "$H/$f"
done
rm -rf "$H/replay-trace"
cp -R "$TRACE" "$H/replay-trace"
printf '%s\n' "url=https://chatgpt.com/" "trace=replay-trace" "trace_keyed=1" \
    "validation_js_bench=$SCALE" "js_bench_dir=replay-trace" \
    "validation_js_profile=0" "profile=realistic" "network_profile=1" \
    "ticks=0" "dump_frame=0" "exit_after_report=0" \
    "interactive_validation_ticks=0" "validation_cancel_after_ms=0" \
    "validation_preview_scroll=0" "validation_media_play=0" \
    "validation_media_stability_auto=0" "validation_power_test_auto=0" \
    "input_script=js-bench.txt" \
    "exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP" > "$H/boot.cfg"
rm -f "$H/tilefinch-validation.txt"
cp "$ROOT/tests/input-scripts/js-bench.txt" "$H/"
echo "staged $H/boot.cfg (validation_js_bench=$SCALE, trace $TRACE)"
echo "launch: PSPDEV=\$PSPDEV HOST_ROOT=$H BUILD_DIR=$H $ROOT/scripts/psplink-device.sh memory"
echo "then poll $H/tilefinch-validation.txt for 'tilefinch-js-bench: outcome=' and 'tilefinch-log: finish'"
