#!/bin/sh
# Bit-exactness gate for the optional swdec decoder: CAVLC (Baseline) and
# CABAC clips with five reference frames at 416x234 and 426x240, a 16-ref
# High clip, and clips with 4x4 partitions, several slices and weighted
# prediction, decoded by the PSP build under PPSSPP (each also with chroma
# predicted by the parser, with the parser stopping part-way through rows,
# and that again torn down and reopened twice mid-stream as a seek or a
# cancel does) and compared frame by frame with host FFmpeg.
#
#   tools/swdec-bench/crc-gate.sh PREPARED_DIR OUT_DIR [EXTRA_CLIPS_DIR]
#
# PREPARED_DIR    output of scripts/prepare-swdec-ffmpeg.sh
# OUT_DIR         scratch: generated clips, the bench run, its results
# EXTRA_CLIPS_DIR optional *.aus with matching *.framecrc (au_split.py and
#                 `ffmpeg -f framecrc`, see run-bench.sh), also checked
#
# Needs PSPDEV, PPSSPP (a signed PPSSPPSDL executable) and a host ffmpeg
# with libx264. Exits 77 (skipped) when any of them is missing, 1 on a
# mismatch or an unfinished report, the bench's status when its PPSSPP run
# fails. The clips are synthetic (lavfi), so nothing is checked in.
set -u
[ "$#" -ge 2 ] || { sed -n '2,16p' "$0"; exit 2; }
prepared=$1; out=$2; extra=${3:-}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
skip() { echo "skipped: $*"; exit 77; }
[ -n "${PSPDEV:-}" ] || skip "PSPDEV is not set"
[ -n "${PPSSPP:-}" ] && [ -x "$PPSSPP" ] || skip "PPSSPP is not set"
command -v ffmpeg >/dev/null 2>&1 || skip "no host ffmpeg"
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264 \
    || skip "host ffmpeg has no libx264"
[ -f "$prepared/libs/asm22-diag/libavcodec.a" ] \
    || skip "no prepared swdec workspace at $prepared"
mkdir -p "$out/aus"
clips="$out/aus"
source="testsrc2=size=640x360:rate=30,noise=alls=6:allf=t,format=yuv420p"
make_clip() { # name WxH fps profile x264-params
    ffmpeg -hide_banner -loglevel error -y -f lavfi -t 6 -i "$source" \
        -vf "scale=$2,fps=$3" -c:v libx264 -preset medium \
        -profile:v "$4" -level:v 3.0 -pix_fmt yuv420p -b:v 400k \
        -x264-params "$5" -an -f h264 "$clips/$1.h264" || exit 1
    python3 -I "$root/tools/swdec-bench/au_split.py" \
        "$clips/$1.h264" "$clips/$1.aus" >/dev/null || exit 1
    ffmpeg -hide_banner -loglevel error -y -i "$clips/$1.h264" \
        -fps_mode passthrough -f framecrc "$clips/$1.framecrc" || exit 1
    rm -f "$clips/$1.h264"
}
make_clip cavlc416 416:234 25 baseline "ref=5"
make_clip cavlc426 426:240 30 baseline "ref=5"
make_clip cabac416 416:234 25 main "ref=5:bframes=0"
make_clip cabac426 426:240 30 high "ref=5:bframes=3:b-pyramid=normal"
make_clip high432-r16 432:240 30 high "ref=16:bframes=3:b-pyramid=normal"
# small partitions (4x4 sub-blocks), several slices per picture, explicit
# weighted P and implicit weighted B
make_clip high426-p4x4-slices 426:240 30 high \
    "ref=4:bframes=3:b-pyramid=normal:partitions=all:slices=3:weightp=2:weightb=1:direct=auto"
make_clip cavlc416-p4x4-slices 416:234 25 baseline "ref=3:partitions=all:slices=2"
if [ -n "$extra" ]; then
    for clip in "$extra"/*.aus; do
        [ -f "$clip" ] || continue
        cp "$clip" "${clip%.aus}.framecrc" "$clips/"
    done
fi
config="$out/crc.cfg"
: > "$config"
# Each clip's arena is sized as the player sizes it, from the references
# it declares (16 for the -r16 clip and for unknown extra clips).
for clip in "$clips"/*.aus; do
    name=$(basename "$clip")
    case "$name" in
        cavlc*|cabac*) refs=5 ;;
        *) refs=16 ;;
    esac
    # and again with chroma predicted by the parser, the Media Engine's
    # share of the work (this bench has no Media Engine), and with the
    # parser stopping part-way through rows, as when the main core reaches
    # a macroblock first and predicts the rest of the row itself
    printf 'decode %s refs=%s\n' "$name" "$refs" >> "$config"
    printf 'decode %s refs=%s chromapred=parser\n' "$name" "$refs" >> "$config"
    printf 'decode %s refs=%s chromapred=parser chromapred=partial\n' \
        "$name" "$refs" >> "$config"
    # closed after 37 access units (mid-GOP, pictures still held for
    # reordering), reopened in the same arena and decoded from the start,
    # closed again after 61, then decoded whole: what a seek's reset and a
    # cancel do between access units
    printf 'decode %s refs=%s chromapred=parser chromapred=partial restart=37,61\n' \
        "$name" "$refs" >> "$config"
done
bench_status=0
sh "$root/tools/swdec-bench/run-bench.sh" "$prepared" "$clips" "$config" \
    "$out/run" 1800 > "$out/bench.log" 2>&1 || bench_status=$?
if [ "$bench_status" -ne 0 ]; then
    tail -n 20 "$out/bench.log"
    # The run's own status, except the one CTest would read as a skip.
    [ "$bench_status" -eq 77 ] && exit 1
    exit "$bench_status"
fi
status=0
# The report must be whole (every configured clip summarized, the bench's
# DONE marker), and the decoder arena's real peak must stay inside the size
# the player gives it (swdec_arena_estimate).
python3 -I "$root/tools/swdec-bench/arena_fit.py" \
    "$out/run/bench-results.txt" "$config" || status=1
cd "$clips" || exit 1
for clip in *.aus; do
    python3 -I "$root/tools/swdec-bench/cmp_crc.py" \
        "$out/run/bench-results.txt" "$clip" "${clip%.aus}.framecrc" \
        || status=1
done
exit "$status"
