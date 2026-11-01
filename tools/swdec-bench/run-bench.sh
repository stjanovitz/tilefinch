#!/bin/sh
# PPSSPP bench for the optional swdec decoder: decode instruction-count proxy,
# bit-exactness against host ffmpeg, and colour-conversion A/B.
#
#   tools/swdec-bench/run-bench.sh PREPARED_DIR CLIPS_DIR CFG OUT_DIR [timeout-s]
#
# PREPARED_DIR  output of scripts/prepare-swdec-ffmpeg.sh (ffmpeg/, libs/asm22-diag/)
# CLIPS_DIR     *.aus files: access units as [u32 LE length][Annex-B bytes],
#               made from an MP4 with a host ffmpeg and au_split.py:
#                 ffmpeg -i clip.mp4 -map 0:v -c:v copy \
#                     -bsf:v h264_mp4toannexb -f h264 clip.h264
#                 python3 -I tools/swdec-bench/au_split.py clip.h264 clip.aus
#                 ffmpeg -i clip.h264 -fps_mode passthrough -f framecrc clip.framecrc
# CFG           bench.cfg lines, run in order:
#                 decode <clip.aus>          per-frame adler32 + CSC A/B
#                 (either form takes refs=N: the arena is sized for N held
#                 pictures, default 16, and the summary reports its peak)
#                 decode <clip.aus> stages   per-stage totals (heavier clock)
#                 cscx                       every (y, u, v) through both converters
# OUT_DIR       receives EBOOT.PBP and bench-results.txt
#
# Compare frames with:
#   python3 -I tools/swdec-bench/cmp_crc.py OUT/bench-results.txt clip.aus clip.framecrc
#
# PSPDEV must name the PSP toolchain. PPSSPP must name the executable inside a
# validly signed PPSSPPSDL.app (the runner repairs nothing then). The run goes
# through scripts/run-ppsspp-input-script.sh (isolated HOME, LaunchServices)
# at TILEFINCH_PPSSPP_CPU_MHZ (default 333); that runner reports a failed
# trace afterwards, which is expected for this EBOOT. Any other runner
# failure (no clean exit, a timeout, a graphics backend that never started)
# fails the bench with the runner's status, and so does a report without the
# bench's DONE marker. PPSSPP's clock is about one cycle per instruction:
# compare runs with each other, never with device time.
#
# SWDEC_BENCH_RUNNER replaces the PPSSPP runner (the bench scripts' tests).
set -u
[ "$#" -ge 4 ] || { sed -n '2,37p' "$0"; exit 2; }
prepared=$(cd "$1" && pwd); clips=$(cd "$2" && pwd); cfg=$3; out=$4
limit=${5:-1800}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
: "${PSPDEV:?PSPDEV must name the PSP toolchain}"
: "${PPSSPP:?PPSSPP must name a signed PPSSPPSDL executable}"
export PATH="$PSPDEV/bin:$PATH"
rm -rf "$out"; mkdir -p "$out/obj" "$out/bdir" "$out/data"; out=$(cd "$out" && pwd)
make -C "$out/obj" -f "$root/tools/swdec-bench/Makefile" \
    SWDEC_SOURCE_DIR="$root/src/swdec" TILEFINCH_SOURCE_DIR="$root" \
    FFMPEG_SOURCE_DIR="$prepared/ffmpeg" \
    FFMPEG_LIBRARY_DIR="$prepared/libs/asm22-diag" > "$out/build.log" 2>&1 \
    || { tail -n 20 "$out/build.log"; exit 1; }
cp "$out/obj/EBOOT.PBP" "$out/EBOOT.PBP"
cp "$out/obj/EBOOT.PBP" "$out/bdir/"
printf 'TILEFINCH_PSP_VALIDATION_LOG:BOOL=ON\n' > "$out/bdir/CMakeCache.txt"
touch "$out/bdir/roots.pem" "$out/bdir/tilefinch-wasm.prx" "$out/data/profile.cfg"
cp "$cfg" "$out/data/bench.cfg"
cp "$clips"/*.aus "$out/data/"
runner=${SWDEC_BENCH_RUNNER:-$root/scripts/run-ppsspp-input-script.sh}
status=0
TILEFINCH_PPSSPP_CPU_MHZ=${TILEFINCH_PPSSPP_CPU_MHZ:-333} \
    sh "$runner" --build-dir "$out/bdir" \
    --script menu-tour --data-dir "$out/data" --measure \
    --timeout "$limit" > "$out/runner.log" 2>&1 || status=$?
# The runner checks for a browser boot this EBOOT never performs, and it does
# so only after the run reached its clean exit: that one complaint is the
# bench's success.
if [ "$status" -ne 0 ] && ! { [ "$status" -eq 1 ] \
        && grep -q 'did not use the shipping native-HOME boot' \
            "$out/runner.log"; }; then
    tail -n 20 "$out/runner.log" >&2
    printf 'run-bench: the PPSSPP run failed (runner status %s)\n' \
        "$status" >&2
    exit "$status"
fi
cp "$out/bdir/ppsspp-input-script-latest/run-1/tilefinch-validation.txt" \
    "$out/bench-results.txt" || exit 1
rm -rf "$out/data" "$out/obj"
grep -qx 'DONE' "$out/bench-results.txt" || {
    printf 'run-bench: %s has no DONE marker: the bench did not finish\n' \
        "$out/bench-results.txt" >&2
    exit 1
}
grep -v '^frame ' "$out/bench-results.txt"
exit 0
