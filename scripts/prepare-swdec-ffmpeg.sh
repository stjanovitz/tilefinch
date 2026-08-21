#!/bin/sh
# Prepare the narrow FFmpeg build used by Tilefinch's optional PSP software
# decoder. The output directory is suitable for TILEFINCH_SWDEC_SOURCE_DIR.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT-DIRECTORY [PGO-DIRECTORY]" >&2
    exit 2
fi
if [ -z "${PSPDEV:-}" ]; then
    echo "PSPDEV must name a PSPDEV installation" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$1
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
pgo_flags=
if [ "$#" -eq 2 ]; then
    pgo=$(CDPATH= cd -- "$2" && pwd)
    if ! find "$pgo" -name '*.gcda' -print -quit | grep -q .; then
        echo "PGO-DIRECTORY has no swdec training profiles" >&2
        exit 2
    fi
    pgo_flags="-fprofile-use -fprofile-correction -Wno-missing-profile \
        -Wno-coverage-mismatch -fprofile-dir=$pgo \
        -fprofile-prefix-path=$output/ffmpeg"
else
    echo "Building without device-trained PGO; decoding may be slower" >&2
fi
ffmpeg="$output/ffmpeg"
libraries="$output/libs/asm22"
patch="$root/third_party/patches/ffmpeg-n8.1.2-swdec.patch"
export PATH="$PSPDEV/bin:$PATH"
pspsdk=$(psp-config --pspsdk-path)

if [ ! -d "$ffmpeg/.git" ]; then
    git clone --depth 1 --branch n8.1.2 \
        https://github.com/FFmpeg/FFmpeg.git "$ffmpeg"
fi

if [ "$(git -C "$ffmpeg" rev-parse HEAD)" != \
        "38b88335f99e76ed89ff3c93f877fdefce736c13" ]; then
    echo "FFmpeg source is not the expected n8.1.2 revision" >&2
    exit 1
fi
if git -C "$ffmpeg" apply --reverse --check "$patch" >/dev/null 2>&1; then
    : # already prepared
elif git -C "$ffmpeg" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$ffmpeg" apply "$patch"
else
    echo "FFmpeg tree is neither pristine nor exactly patched" >&2
    exit 1
fi

cd "$ffmpeg"
./configure \
    --enable-cross-compile --cross-prefix=psp- --arch=mips --cpu=generic \
    --target-os=none --disable-everything --disable-programs --disable-doc \
    --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-avfilter --disable-network \
    --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-asm --disable-inline-asm --disable-mipsdsp \
    --disable-mipsdspr2 --disable-msa --disable-mmi --disable-debug \
    --enable-decoder=h264 --enable-decoder=aac_fixed --enable-parser=h264 \
    --disable-hwaccels --disable-iconv --disable-zlib --disable-bzlib \
    --disable-lzma --disable-securetransport --disable-audiotoolbox \
    --disable-videotoolbox --disable-coreimage --disable-appkit \
    --disable-avfoundation --disable-metal --disable-vulkan --disable-xlib \
    --disable-sdl2 --malloc-prefix=swdec_ --enable-small \
    --extra-cflags="-G0 -Os -D_DEFAULT_SOURCE -DPSP -I$pspsdk/include \
        -DSWDEC_LF_STRENGTH_C=1 -DSWDEC_PSP_LF_CHROMA=1 \
        -DSWDEC_STAGE_TIMING=1 -DSWDEC_FRAME_ONLY=1 \
        -DSWDEC_PSP_ASM=1 -Wno-error=incompatible-pointer-types \
        $pgo_flags" \
    --optflags=-O2
make -j"${TILEFINCH_BUILD_JOBS:-8}" \
    libavcodec/libavcodec.a libavutil/libavutil.a
mkdir -p "$libraries"
cp libavcodec/libavcodec.a libavutil/libavutil.a config.h "$libraries/"

echo "Prepared Tilefinch FFmpeg input at $output"
