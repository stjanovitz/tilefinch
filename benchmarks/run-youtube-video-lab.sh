#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 BUILD_DIR INPUT_MP4 [OUTPUT_MP4] [FRAME_DIR]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$1
input=$2
output=${3:-"$root/youtube-video-lab.mp4"}
frames=${4:-"${TMPDIR:-/tmp}/tilefinch-youtube-video-frames"}
lab="$build/psp-browser-interactive-lab"
ffmpeg=${FFMPEG:-/opt/homebrew/bin/ffmpeg}

if [ ! -x "$lab" ]; then
    echo "interactive lab not found: $lab" >&2
    exit 1
fi
if [ ! -f "$input" ]; then
    echo "input MP4 not found: $input" >&2
    exit 1
fi
if [ ! -x "$ffmpeg" ]; then
    ffmpeg=$(command -v ffmpeg || true)
fi
if [ -z "$ffmpeg" ] || [ ! -x "$ffmpeg" ]; then
    echo "ffmpeg executable not found" >&2
    exit 1
fi

mkdir -p "$frames"
find "$frames" -type f -name 'frame-*.ppm' -delete

"$lab" \
    --fixture "$root/benchmarks/fixtures/youtube-video-lab.html" \
    --media-file "$input" \
    --commands "$root/benchmarks/fixtures/youtube-video-lab.commands" \
    --loop-output-dir "$frames" \
    --limit-mb 24 \
    --output "$frames/final.ppm"

"$ffmpeg" -hide_banner -loglevel error -y \
    -framerate 15 -start_number 0 -i "$frames/frame-%04d.ppm" \
    -i "$input" \
    -map 0:v:0 -map 1:a:0? \
    -c:v libx264 -pix_fmt yuv420p -preset veryfast \
    -c:a aac -shortest \
    "$output"

echo "youtube-video-lab status=PASS output=$output frames=$frames"
