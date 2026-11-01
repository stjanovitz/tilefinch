#!/bin/sh
# Alternating baseline/candidate device runs of one recorded page, scored on
# the fixed steady-playback window (tilefinch-swdec-window lines).
#
#   tools/swdec-bench/window-pairs.sh NAME URL SCENARIO PAIRS ADDON \
#       "BASE_BOOT_LINES" "CAND_BOOT_LINES"
#
# NAME      run prefix: runs are NAME-b1, NAME-c1, NAME-b2, ...
# URL       the page (replayed from TRACE_DIR when set: USB-only, offline)
# SCENARIO  input script for the run wrapper
# PAIRS     baseline/candidate pairs, run alternately (at least 3)
# ADDON     release (acceptance: no component diagnostics) or diag
#           (profiling: adds the component's per-window stage report)
# *_LINES   space-separated boot.cfg lines for each lane (for example
#           "validation_playback_ab=1" and "validation_playback_ab=5")
#
# SCENARIO must bracket the steady window with mark-live video-measure-start
# and mark-live video-measure-end. End after the fixed 5+30 second window,
# before closing playback, so teardown diagnostics cannot overflow the RAM
# timing log. Strict scoring rejects missing/overflowed/forced-flush captures.
#
# Environment:
#   RUN        the device run wrapper (a sh script), run as sh RUN NAME URL SCENARIO
#              [BOOT_LINES...]; it writes LOGS/NAME.txt (required)
#   BUILD_DIR  the validation build directory the wrapper serves over host0
#              (required; its components/swdec is replaced by the add-on)
#   LOGS       where the wrapper leaves its logs (required)
#   TRACE_DIR  passed through to the wrapper (a recorded page)
#   BASE_ADDON_DIR, CAND_ADDON_DIR  an add-on bundle directory (holding
#              tilefinch-swdec.prx and swdec-meload.prx) installed into
#              components/swdec before each run of that lane, to A/B two
#              add-on builds on one browser; each defaults to ADDON's
#              bundle in BUILD_DIR
#   LOOSE=1    profiling: score every run that left a window record and
#              exit 0 (the default is strict acceptance, see window-pairs.py)
#   ALLOW_REDUCED=1  accept reduced-quality decodes (adaptive-mode A/Bs)
#
# Writes LOGS/NAME-identity.txt (the browser's and each lane's add-on
# sha1s, lanes) and LOGS/NAME-runs.txt (each run's wrapper status, start
# time and whether it ran on its lane's recorded binaries: the same browser
# in both lanes, the lane's own add-on), then prints the window table
# (window-pairs.py) and exits with its status: in strict mode nonzero when
# either lane has fewer valid runs than PAIRS. Runs nothing itself on the
# device beyond the wrapper.
set -eu
[ "$#" -eq 7 ] || { sed -n '2,41p' "$0"; exit 2; }
name=$1; url=$2; scenario=$3; pairs=$4; addon=$5; base=$6; cand=$7
: "${RUN:?set RUN to the device run wrapper}"
: "${BUILD_DIR:?set BUILD_DIR to the validation build directory}"
: "${LOGS:?set LOGS to the wrapper log directory}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case $addon in
    release) bundle=$BUILD_DIR/tilefinch-swdec-addon-release ;;
    diag) bundle=$BUILD_DIR/tilefinch-swdec-addon ;;
    *) echo "ADDON must be release or diag" >&2; exit 2 ;;
esac
base_addon=${BASE_ADDON_DIR:-$bundle}
cand_addon=${CAND_ADDON_DIR:-$bundle}
for dir in "$base_addon" "$cand_addon"; do
    [ -f "$dir/tilefinch-swdec.prx" ] && [ -f "$dir/swdec-meload.prx" ] || {
        echo "no add-on in $dir" >&2; exit 1; }
done
[ "$pairs" -ge 1 ] 2>/dev/null || { echo "PAIRS must be a number" >&2; exit 2; }
mkdir -p "$BUILD_DIR/components/swdec"
identity=$LOGS/$name-identity.txt
runs_file=$LOGS/$name-runs.txt
# What a run executes: the browser and the installed add-on.
browser() { (cd "$BUILD_DIR" && shasum psp-browser-script-dev.prx); }
lane_addon() { (cd "$1" && shasum tilefinch-swdec.prx swdec-meload.prx); }
install() {
    rm -f "$BUILD_DIR/components/swdec"/*
    cp "$1"/* "$BUILD_DIR/components/swdec/"
}
built=$(browser)
base_built=$(lane_addon "$base_addon")
cand_built=$(lane_addon "$cand_addon")
{
    echo "name=$name pairs=$pairs addon=$addon"
    echo "url=$url"
    echo "scenario=$scenario"
    echo "trace=${TRACE_DIR:-none}"
    echo "base=$base"
    echo "cand=$cand"
    echo "$built"
    echo "base-addon=$base_addon"
    echo "$base_built"
    echo "cand-addon=$cand_addon"
    echo "$cand_built"
} > "$identity"
: > "$runs_file"
runs=""
i=1
while [ "$i" -le "$pairs" ]; do
    for lane in b c; do
        if [ "$lane" = b ]; then
            lines=$base; dir=$base_addon; expected=$base_built
        else
            lines=$cand; dir=$cand_addon; expected=$cand_built
        fi
        run=$name-$lane$i
        install "$dir"
        start=$(date +%s)
        status=0
        # shellcheck disable=SC2086
        sh "$RUN" "$run" "$url" "$scenario" $lines || status=$?
        [ "$status" -eq 0 ] || echo "run $run: wrapper status $status" >&2
        same=same
        [ "$(browser)" = "$built" ] \
            && [ "$(lane_addon "$BUILD_DIR/components/swdec")" = "$expected" ] \
            || same=changed
        echo "run=$run status=$status start=$start build=$same" \
            >> "$runs_file"
        runs="$runs $LOGS/$run.txt"
    done
    i=$((i + 1))
done
mode=""
[ "${LOOSE:-0}" = 1 ] && mode="--loose"
[ "${ALLOW_REDUCED:-0}" = 1 ] && mode="$mode --allow-reduced"
# shellcheck disable=SC2086
python3 "$here/window-pairs.py" --identity "$identity" --runs "$runs_file" \
    --pairs "$pairs" $mode $runs
