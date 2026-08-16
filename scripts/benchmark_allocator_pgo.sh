#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "$0")/.." && pwd)
training_build="${root_dir}/build-allocator-pgo-training"
optimized_build="${root_dir}/build-allocator-pgo"
script_file="${root_dir}/fixtures/js-closure-vm-benchmark.js"
profile_dir=$(mktemp -d "${TMPDIR:-/tmp}/psp-browser-pgo.XXXXXX")
profile_data="${optimized_build}/allocator-$$.profdata"
trap 'rm -rf "${profile_dir}"' EXIT

if [[ $(uname -s) != Darwin ]]; then
    echo "allocator PGO comparison requires macOS JavaScriptCore" >&2
    exit 2
fi

cmake -S "${root_dir}" -B "${training_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSP_BROWSER_BUILD_TESTS=OFF \
    -DPSP_BROWSER_BUILD_JSC_SPIKE=OFF \
    -DPSP_BROWSER_PGO_GENERATE="${profile_dir}/quickjs-%p.profraw" \
    "$@"
cmake --build "${training_build}" \
    --target psp-browser-quickjs-allocator-bench -j 8
"${training_build}/psp-browser-quickjs-allocator-bench" \
    --script "${script_file}" --allocator pool --runs 3

mkdir -p "${optimized_build}"
xcrun llvm-profdata merge -output="${profile_data}" \
    "${profile_dir}"/*.profraw

cmake -S "${root_dir}" -B "${optimized_build}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSP_BROWSER_BUILD_TESTS=OFF \
    -DPSP_BROWSER_BUILD_JSC_SPIKE=ON \
    -DPSP_BROWSER_PGO_GENERATE= \
    -DPSP_BROWSER_PGO_USE="${profile_data}" \
    "$@"
cmake --build "${optimized_build}" \
    --target psp-browser-quickjs-allocator-bench psp-browser-jsc-spike -j 8

quickjs_output=$("${optimized_build}/psp-browser-quickjs-allocator-bench" \
    --script "${script_file}" --allocator pool --runs 7)
quickjs_ms=$(sed -E 's/.*median-ms=([0-9]+).*/\1/' <<<"${quickjs_output}")

jsc_times=()
for _ in 1 2 3 4 5 6 7; do
    output=$("${optimized_build}/psp-browser-jsc-spike" \
        --script "${script_file}")
    jsc_times+=("$(sed -E 's/.*elapsed-ms=([0-9]+).*/\1/' <<<"${output}")")
done
jsc_ms=$(printf '%s\n' "${jsc_times[@]}" | sort -n | sed -n '4p')
ratio=$(awk -v quickjs="${quickjs_ms}" -v jsc="${jsc_ms}" \
    'BEGIN { printf "%.3f", quickjs / jsc }')

echo "${quickjs_output}"
echo "allocator-bench comparison quickjs-median-ms=${quickjs_ms} jsc-median-ms=${jsc_ms} ratio=${ratio}x"

if (( quickjs_ms > 2 * jsc_ms )); then
    echo "allocator benchmark exceeds the 2.0x JavaScriptCore boundary" >&2
    exit 1
fi
