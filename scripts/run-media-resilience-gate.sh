#!/bin/sh
# Explicit host qualification for media delivery under deterministic CDN and
# Wi-Fi faults. The generated MP4 is synthetic, temporary, and never committed.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_arg=${1:-build-preset-release}
case "$build_arg" in
    /*) build=$build_arg ;;
    *) build=$root/$build_arg ;;
esac

command -v ffmpeg >/dev/null 2>&1 || {
    printf '%s\n' "ffmpeg is required for the media resilience gate" >&2
    exit 2
}

work=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-media-resilience.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
fixture=$work/synthetic-240p-fragmented.mp4

cmake --build "$build" --target tilefinch-media-http-range-tests -j8

# Match the split 240p shape used by the native player: video-only AVC,
# authored at 23.976 fps, with an index and several fragments. Sixty seconds
# gives every 30-second cadence scenario enough byte boundaries without
# retaining or redistributing third-party media.
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=426x240:rate=24000/1001' -t 60 -an \
    -c:v libx264 -profile:v baseline -level:v 2.1 -pix_fmt yuv420p \
    -b:v 180k -maxrate 240k -bufsize 360k \
    -x264-params 'keyint=144:min-keyint=144:scenecut=0:bframes=0' \
    -metadata creation_time='1970-01-01T00:00:00Z' \
    -movflags +frag_keyframe+empty_moov+default_base_moof+global_sidx \
    "$fixture"

for profile in \
    live-cdn-prefetch bursty setup-latency drop-once trickle-reconnect \
    coupled-asymmetric chaos-recovery psp-48k
do
    python3 "$root/tests/run_media_http_range_test.py" \
        "$build/tilefinch-media-http-range-tests" \
        --cadence "$fixture" --profile "$profile"
done

printf '%s\n' "media-resilience-gate status=PASS"
