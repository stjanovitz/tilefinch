#!/bin/sh
# Build the offline YouTube journey fixture: a one-item offline library whose
# item plays like a downloaded YouTube video, with no network at all.
#
#   tools/make-offline-youtube-fixture.sh [OUTPUT_DIR]
#
# OUTPUT_DIR defaults to perf/fixtures/offline-youtube (ignored, like the
# journey traces). It receives offline/ -- library.bin plus the item's media
# -- to be staged beside the EBOOT as host0:/offline. The clip is synthetic
# (testsrc with its frame counter, so a seek target is visible, plus a sine
# tone), 60 s, and shaped like the split streams the resolver picks:
#
# - video: 640x360 H.264 Main, level 3.0, 30 fps, 2 s GOPs (itag 134 shape).
#   At most THREE reference frames and no B-pyramid: libx264's default
#   preset writes a 4-reference SPS, and the PSP-3000 Media Engine refuses
#   its first access unit with 0x80628002 (logged as wide-program-rejected),
#   so the player never leaves startup priming. Real itag 134 streams use 3.
# - audio: AAC-LC 44.1 kHz stereo 128 kbit/s (itag 140 shape).
# - both DASH-fragmented (sidx + moof/mdat), as a real download is.
#
# Encoders are not byte-reproducible across ffmpeg/x264 versions (see
# tools/generate_psp_media_fixtures.sh), so the script prints the digests and
# scripts/run-psplink-offline-youtube.sh records them in its log line.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-$root/perf/fixtures/offline-youtube}
tool=${OFFLINE_FIXTURE_TOOL:-$root/build-preset-release/tilefinch-offline-library-fixture}

[ -x "$tool" ] || {
    printf '%s\n' "missing $tool;" \
        "build it: cmake --build build-preset-release --target tilefinch-offline-library-fixture" >&2
    exit 2
}
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg is required" >&2; exit 2; }
[ ! -e "$output/offline" ] || {
    printf 'refusing to overwrite %s/offline\n' "$output" >&2
    exit 2
}

work=$output/.work
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT HUP INT TERM

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc=size=640x360:rate=30:duration=60' \
    -c:v libx264 -profile:v main -level:v 3.0 -pix_fmt yuv420p \
    -x264-params 'ref=3:bframes=2:b-pyramid=none:weightp=0:keyint=60:min-keyint=60:scenecut=0' \
    -b:v 500k -maxrate 700k -bufsize 1400k -an \
    -metadata creation_time='1970-01-01T00:00:00Z' \
    -movflags frag_keyframe+empty_moov+default_base_moof+global_sidx \
    "$work/video.mp4"

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=60' \
    -ac 2 -c:a aac -profile:a aac_low -b:a 128k -vn \
    -metadata creation_time='1970-01-01T00:00:00Z' \
    -frag_duration 2000000 \
    -movflags frag_keyframe+empty_moov+default_base_moof+global_sidx \
    "$work/audio.mp4"

"$tool" "$output/offline" "$work/video.mp4" "$work/audio.mp4" \
    640 360 60000 "Offline test pattern"

(cd "$output/offline" && shasum -a 256 00000001.video.mp4 00000001.audio.mp4)
ffmpeg -hide_banner -version | head -1
