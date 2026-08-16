#!/bin/sh
# Local qualification for promoting the PSP's 360p path.
#
# This intentionally remains an explicit gate: the range profiles preserve
# real wall cadence and can take up to about 90 seconds per long capture.  It
# never writes to a PSP or Memory Stick.  Extra MP4 arguments are lab inputs
# only; third-party captures do not belong in the repository.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_arg=${1:-build-preset-release}
case "$build_arg" in
    /*) build=$build_arg ;;
    *) build=$root/$build_arg ;;
esac
if [ "$#" -gt 0 ]; then shift; fi

cmake --build "$build" --target \
    tilefinch-media-mp4-tests \
    tilefinch-host-media-timing-tests \
    tilefinch-media-promotion-tests \
    tilefinch-psp-media-ownership-tests \
    tilefinch-psp-media-state-tests \
    tilefinch-psp-media-present-tests \
    tilefinch-psp-ui-tests \
    tilefinch-media-http-range-tests \
    psp-browser-media-probe -j8

for test in \
    tilefinch-media-mp4-tests \
    tilefinch-host-media-timing-tests \
    tilefinch-media-promotion-tests \
    tilefinch-psp-media-ownership-tests \
    tilefinch-psp-media-state-tests \
    tilefinch-psp-media-present-tests \
    tilefinch-psp-ui-tests
do
    "$build/$test"
done

probe_capture()
{
    capture=$1
    [ -f "$capture" ] || return 0
    echo "promotion-corpus probe=$capture"
    "$build/psp-browser-media-probe" "$capture"

    # The deterministic range scenario covers thirty seconds of authored
    # time. Short fixtures still exercise the full demux/backend probe above;
    # only long captures enter the real-time transport matrix.
    command -v ffprobe >/dev/null 2>&1 || return 0
    duration=$(ffprobe -v error -show_entries format=duration \
        -of default=noprint_wrappers=1:nokey=1 "$capture")
    seconds=${duration%%.*}
    case "$seconds" in
        ''|*[!0-9]*) return 0 ;;
    esac
    [ "$seconds" -ge 31 ] || return 0
    for profile in drop-once trickle-reconnect coupled-asymmetric chaos-recovery
    do
        python3 "$root/tests/run_media_http_range_test.py" \
            "$build/tilefinch-media-http-range-tests" \
            --cadence "$capture" --profile "$profile"
    done
}

probe_capture "$root/tests/fixtures/psp-media/baseline-320x240.mp4"
probe_capture "$root/tests/fixtures/psp-media/main-640x360.mp4"

for capture in "$@"
do
    probe_capture "$capture"
done

echo "media-promotion-gate status=PASS"
