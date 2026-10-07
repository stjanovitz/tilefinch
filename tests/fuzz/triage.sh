#!/bin/sh
# Re-run every saved artifact of a target and print a one-screen summary
# (sanitizer kind, top frames) so duplicate crashes can be grouped by stack.
# usage: tests/fuzz/triage.sh TARGET [FRAMES]
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${TILEFINCH_FUZZ_BUILD:-$root/build-fuzz}
target=$1
frames=${2:-6}
binary="$build/tests/fuzz/fuzz_$target"
for artifact in "$build/fuzz-work/$target/artifacts/"*; do
    [ -f "$artifact" ] || continue
    echo "== $(basename "$artifact") ($(wc -c < "$artifact" | tr -d ' ') bytes)"
    "$binary" -runs=1 "$artifact" 2>&1 \
        | grep -E 'ERROR: |SUMMARY: |runtime error|^    #[0-9]+ ' \
        | grep -vE 'libFuzzer|fuzzer::|LLVMFuzzerTestOneInput|start\+|main ' \
        | head -n "$((frames + 2))"
done
