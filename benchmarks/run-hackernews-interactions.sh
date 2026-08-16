#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    printf '%s\n' "usage: $0 BUILD_DIR HACKERNEWS_TRACE [OUTPUT_DIR]" >&2
    exit 2
fi

build_dir=$1
trace=$2
output_dir=${3:-interaction-results}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lab="$build_dir/psp-browser-interactive-lab"
commands="$root/benchmarks/interactions/hackernews.commands"
log="$output_dir/hackernews.log"
limit_mb=${INTERACTION_LIMIT_MB:-24}
script_count=${INTERACTION_SCRIPT_COUNT:-32}
session_cache_kb=${INTERACTION_SESSION_CACHE_KB:-1024}

numeric_between() {
    value=$1
    minimum=$2
    maximum=$3
    case $value in ''|*[!0-9]*) return 1 ;; esac
    [ "$value" -ge "$minimum" ] && [ "$value" -le "$maximum" ]
}

if ! numeric_between "$limit_mb" 16 64 \
    || ! numeric_between "$script_count" 1 256 \
    || ! numeric_between "$session_cache_kb" 64 4096; then
    printf 'Invalid interaction limits: memory=%s scripts=%s cache-kb=%s\n' \
        "$limit_mb" "$script_count" "$session_cache_kb" >&2
    exit 2
fi
if [ ! -x "$lab" ]; then
    printf 'Build the interactive lab first: %s\n' "$lab" >&2
    exit 2
fi
if [ ! -f "$trace/trace.meta" ] \
    || ! grep -q '^capture-complete=yes$' "$trace/trace.meta"; then
    printf 'Complete HTTP replay trace required: %s\n' "$trace" >&2
    exit 2
fi

mkdir -p "$output_dir"
"$lab" --url https://news.ycombinator.com/ --fetch-scripts \
    --ticks 5 --tick-ms 100 --psp-profile realistic --limit-mb "$limit_mb" \
    --script-count "$script_count" --session-cache-kb "$session_cache_kb" \
    --low-memory-navigation --no-loop-capture --replay-http "$trace" \
    --commands "$commands" > "$log" 2>&1

if grep -Eq 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$log"; then
    printf 'Sanitizer diagnostic in %s\n' "$log" >&2
    grep -E 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$log" >&2
    exit 1
fi

grep -q '^loop status=PASS ' "$log"
grep -q '^interactive status=ok ' "$log"
grep -q '^javascript-error="" source=""$' "$log"
grep -q '^javascript-callback-errors uncaught=0 last=""$' "$log"
grep -q '^loop-js ok=yes value="home:30:0" error=""$' "$log"
grep -q '^loop-js ok=yes value="comments:/item?id=49010345:true:0" error=""$' "$log"
grep -Eq '^loop-js ok=yes value="comment-scroll:/item:[1-9][0-9]*" error=""$' "$log"
grep -q '^loop-js ok=yes value="comments-back:/:30:0" error=""$' "$log"
grep -q '^loop-js ok=yes value="page-two:/?p=2:30:0" error=""$' "$log"
grep -Eq '^loop-js ok=yes value="page-two-back:/:30:[1-9][0-9]*" error=""$' "$log"
grep -q '^navigation loads=5 destroys=4 ' "$log"
grep -q '^navigation-replacement mode=low-memory started=4 succeeded=4 failed=0 fallbacks=0 ' "$log"
grep -Eq '^memory-categories phase=interactive-stable .*global-peak=[0-9]+ .*reconcile=yes$' "$log"
grep -Eq '^memory-categories phase=interactive-teardown current=0 expected=0 .*external-reserved=0 reconcile=yes$' "$log"
grep -Eq '^interactive teardown=0 active=0 .*failures=0 status=PASS$' "$log"
grep -q "^runtime-policy scripts=$script_count " "$log"

printf 'Hacker News interaction qualification passed: %s (limit=%s MiB scripts=%s cache=%s KiB)\n' \
    "$log" "$limit_mb" "$script_count" "$session_cache_kb"
