#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 VIDEO_ID [OUTPUT_MP4]" >&2
    exit 2
fi

video_id=$1
case "$video_id" in
    *[!A-Za-z0-9_-]*)
        echo "video ID contains unsupported characters" >&2
        exit 2
        ;;
esac
if [ "${#video_id}" -ne 11 ]; then
    echo "video ID must contain exactly 11 characters" >&2
    exit 2
fi

output=${2:-"${TMPDIR:-/tmp}/tilefinch-youtube-$video_id.mp4"}
ytdlp=${YTDLP:-yt-dlp}

if command -v "$ytdlp" >/dev/null 2>&1; then
    runner=command
elif python3 -c 'import yt_dlp' >/dev/null 2>&1; then
    runner=module
else
    echo "yt-dlp is required for this lab-only acquisition step" >&2
    exit 1
fi

run_ytdlp()
{
    if [ "$runner" = command ]; then
        "$ytdlp" "$@"
    else
        python3 -m yt_dlp "$@"
    fi
}

run_ytdlp --no-playlist \
  --extractor-args 'youtube:player_client=android_vr,web_safari' \
  --format 'best[ext=mp4][vcodec^=avc1][acodec^=mp4a][height<=360]' \
  --write-info-json \
  --output "$output" \
  "https://www.youtube.com/watch?v=$video_id"

echo "youtube-capture status=PASS id=$video_id output=$output"
