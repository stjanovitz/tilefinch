#!/bin/sh
# Regenerate the committed PSP media device-contract fixtures.
#
# The output is NOT reproducible across encoder versions. libx264 writes its
# core number and full option string into the SEI of every stream, and its
# rate-control and macroblock decisions change between releases, so a
# different ffmpeg or x264 produces different bytes for the same command line
# -- occasionally different enough to change which decoder policy the PSP
# picks. Regenerating therefore silently rebases what the media tests are
# measuring against unless the drift is noticed.
#
# The checksums below are what the committed fixtures actually hash to, so a
# regeneration that changes them is visible immediately instead of at the
# next unexplained PPSSPP media failure. If they change, that is a decision
# to make deliberately: confirm the new clips still exercise both decoder
# policies, re-run the PPSSPP media qualification, and update both the
# checksums and the recorded encoder version in the same commit.
#
# Committed fixtures (verify with --check):
#   a2436a4219446081f361b9c19d9bbef63ad92b049ae5e787e6888ab94679c5c7
#       tests/fixtures/psp-media/baseline-320x240.mp4   93,830 bytes
#   161f1d138b67d1fb88a4323f9b0e0f3aa7ca31640873566461d07546463f8cb0
#       tests/fixtures/psp-media/main-640x360.mp4      171,876 bytes
#
# Produced by ffmpeg 8.1.2 with libx264 core 165 (Homebrew, macOS arm64).
#
# Usage:
#   tools/generate_psp_media_fixtures.sh            regenerate and report
#   tools/generate_psp_media_fixtures.sh --check    verify only, no writes
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output="$root/tests/fixtures/psp-media"
manifest="$output/SHA256SUMS"

check_only=0
case "${1:-}" in
    --check) check_only=1 ;;
    -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
    "") ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
esac

verify_manifest() {
    [ -f "$manifest" ] || {
        printf 'missing %s\n' "$manifest" >&2
        return 1
    }
    (cd "$output" && shasum -a 256 -c SHA256SUMS)
}

if [ "$check_only" -eq 1 ]; then
    verify_manifest
    exit 0
fi

mkdir -p "$output"

# These clips are device-contract probes, not fidelity media. Keep them short,
# independently generated, and free of network/provider dependencies. Fixed
# GOPs and disabled B-frames make the first decoded picture easy to diagnose;
# the two profiles deliberately exercise both PSP decoder-program policies.
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=320x240:rate=15:duration=2' \
    -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' \
    -c:v libx264 -profile:v baseline -level:v 2.1 -pix_fmt yuv420p \
    -x264-params 'keyint=15:min-keyint=15:scenecut=0:bframes=0' \
    -c:a aac -profile:a aac_low -ar 48000 -ac 2 \
    -metadata creation_time='1970-01-01T00:00:00Z' -movflags +faststart \
    "$output/baseline-320x240.mp4"

ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i 'testsrc2=size=640x360:rate=15:duration=2' \
    -f lavfi -i 'sine=frequency=660:sample_rate=48000:duration=2' \
    -c:v libx264 -profile:v main -level:v 3.0 -pix_fmt yuv420p \
    -x264-params 'keyint=15:min-keyint=15:scenecut=0:bframes=0' \
    -c:a aac -profile:a aac_low -ar 48000 -ac 2 \
    -metadata creation_time='1970-01-01T00:00:00Z' -movflags +faststart \
    "$output/main-640x360.mp4"

for fixture in "$output"/*.mp4; do
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,pix_fmt \
        -of default=noprint_wrappers=1 "$fixture"
    shasum -a 256 "$fixture"
done

# Rewrite the manifest last, so `--check` on the committed tree compares the
# committed bytes rather than whatever was just produced.
(cd "$output" && shasum -a 256 baseline-320x240.mp4 main-640x360.mp4 \
    >SHA256SUMS)
printf '\nwrote %s\n' "$manifest"
printf '%s\n' \
    "If these digests differ from the ones recorded at the top of this" \
    "script, the encoder changed. Re-run the PPSSPP media qualification" \
    "and update the header before committing the new fixtures."
ffmpeg -hide_banner -version | head -1
