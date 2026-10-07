#!/bin/sh
# Cold/warm check of the persistent compiled-script tier (Keep compiled
# scripts) on the physical PSP over PSPLink, with zero Memory Stick writes:
# the cache directory is boot.cfg's module_cache_dir on host0 (the Mac), and
# the page comes from a recorded HTTP trace on host0.
#
#   scripts/script-cache-device.sh [BUILD_DIR] [PAGE] [URL]
#
# BUILD_DIR  a PSP validation build (default build-preset-psp-validation;
#            psplink-device.sh builds its dev PRX if stale)
# PAGE       a site-census page name (default mdn: 47 external classic
#            scripts, a known-good device replay) read from
#            ${CENSUS_CORPUS:-../census/corpus}/PAGE/capture, or a path to
#            any response-keyed trace directory. Avoid Wikipedia: the PSP
#            runs it with page JavaScript off by default, so nothing
#            compiles and the tier has nothing to keep.
# URL        the page the trace answers (default: the census meta.json's)
#
# It runs the UNMODIFIED scripts/psplink-device.sh memory --wait twice
# against the same host0:/script-cache-check directory: a cold run (compiles;
# idle work stores the scripts and writes the packs) and a warm run (a
# restarted browser that reads them). Each run's tilefinch-validation.txt is
# kept beside the build as script-cache-cold.txt / script-cache-warm.txt,
# and the tilefinch-script-cache lines (configuration, then per mark: page
# compiles and compile time, classic hits/misses/stores, disk hits, packs
# and bytes read and written) and the directory's size are printed. boot.cfg
# is restored afterwards.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
H=${1:-$ROOT/build-preset-psp-validation}
PAGE=${2:-mdn}
CORPUS=${CENSUS_CORPUS:-$ROOT/../census/corpus}
case "$PAGE" in
    */*) TRACE=$PAGE; META="" ;;
    *) TRACE=$CORPUS/$PAGE/capture; META=$CORPUS/$PAGE/meta.json ;;
esac
URL=${3:-}
if [ -z "$URL" ] && [ -n "$META" ]; then
    URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["url"])' "$META")
fi
CACHE_NAME=script-cache-check
[ -n "${PSPDEV:-}" ] || { echo "PSPDEV must name the pspdev SDK root" >&2; exit 2; }
[ -f "$TRACE/trace.meta" ] || { echo "no trace.meta in $TRACE" >&2; exit 2; }
[ -n "$URL" ] || { echo "no URL for $TRACE (pass it as the third argument)" >&2; exit 2; }
case "$H" in
    *:*) echo "BUILD_DIR must be a host directory" >&2; exit 2 ;;
esac
[ -d "$H" ] || { echo "no build directory $H" >&2; exit 2; }

backup=""
if [ -f "$H/boot.cfg" ]; then
    backup="$H/boot.cfg.script-cache-backup"
    cp "$H/boot.cfg" "$backup"
fi
restore() {
    if [ -n "$backup" ]; then
        mv "$backup" "$H/boot.cfg"
    else
        rm -f "$H/boot.cfg"
    fi
}
trap restore EXIT

rm -rf "$H/$CACHE_NAME" "$H/script-cache-trace"
cp -R "$TRACE" "$H/script-cache-trace"
cp "$ROOT/tests/input-scripts/news-settle.txt" "$H/"
# module_cache_dir is on host0, so every pack and index write lands on the
# Mac; the only ms0: path is the read-only exit_to back to PSPLink. The JS
# profiler stays off (it wraps every call and would distort compile time).
printf '%s\n' "url=$URL" "trace=script-cache-trace" "trace_keyed=1" \
    "module_cache_dir=host0:/$CACHE_NAME" "module_cache_write=1" \
    "profile=realistic" "network_profile=1" "ticks=0" "dump_frame=0" \
    "exit_after_report=0" "interactive_validation_ticks=0" \
    "validation_cancel_after_ms=0" "validation_preview_scroll=0" \
    "validation_media_play=0" "validation_media_stability_auto=0" \
    "validation_power_test_auto=0" "validation_js_profile=0" \
    "validation_update_auto=0" "input_script=news-settle.txt" \
    "exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP" > "$H/boot.cfg"

for run in cold warm; do
    rm -f "$H/tilefinch-validation.txt"
    echo "== $run run ($PAGE; cache: $H/$CACHE_NAME)"
    PSPDEV=$PSPDEV HOST_ROOT=$H BUILD_DIR=$H \
        "$ROOT/scripts/psplink-device.sh" memory --wait
    cp "$H/tilefinch-validation.txt" "$H/script-cache-$run.txt"
    grep -E 'tilefinch-script-cache:|tilefinch-js-profiler:' \
        "$H/script-cache-$run.txt" || true
    files=$(ls "$H/$CACHE_NAME" 2>&1 | grep -c . || true)
    [ -d "$H/$CACHE_NAME" ] || files=0
    bytes=$(du -sk "$H/$CACHE_NAME" 2>&1 | cut -f1)
    [ -d "$H/$CACHE_NAME" ] || bytes=0
    echo "cache after $run: $files files, ${bytes} KiB"
done
echo "logs: $H/script-cache-cold.txt $H/script-cache-warm.txt"
