#!/bin/sh
# Configure and build the libFuzzer targets in build-fuzz/ (see README.md).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${TILEFINCH_FUZZ_BUILD:-$root/build-fuzz}
if [ -z "${CC:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then
        CC=/opt/homebrew/opt/llvm/bin/clang
    else
        CC=clang
    fi
fi
CXX=${CXX:-$(dirname "$CC")/clang++}
flags="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g -O1"
cmake -S "$root" -B "$build" -G "Unix Makefiles" \
    -DTILEFINCH_ALLOW_BUILD_DIR=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DPSP_BROWSER_BUILD_TESTS=OFF \
    -DPSP_BROWSER_BUILD_JSC_SPIKE=OFF \
    -DPSP_BROWSER_USE_BELLARD_QUICKJS=ON \
    -DPSP_BROWSER_USE_COMPILER_CACHE=OFF \
    -DPSP_BROWSER_ENABLE_PSP_VOICE=OFF \
    -DTILEFINCH_BUILD_FUZZERS=ON
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
cmake --build "$build" --target tilefinch-fuzzers -j "$jobs"
