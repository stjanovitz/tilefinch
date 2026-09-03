#!/bin/sh
set -eu

if [ "$#" -lt 5 ] || [ "$#" -gt 6 ]; then
    printf '%s\n' \
        "usage: $0 BUILD_DIR WIKIPEDIA_TRACE GOOGLE_TASK_TRACE CNN_HOME_TRACE CNN_ARTICLE_TRACE [OUTPUT_DIR]" >&2
    exit 2
fi

build_dir=$1
wikipedia_trace=$2
google_trace=$3
cnn_trace=$4
cnn_article_trace=$5
output_dir=${6:-everyday-browsing-results}
lab="$build_dir/psp-browser-interactive-lab"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
commands="$root/benchmarks/interactions"

if [ ! -x "$lab" ]; then
    printf 'Build the interactive lab first: %s\n' "$lab" >&2
    exit 2
fi
for trace in "$wikipedia_trace" "$google_trace" "$cnn_trace" \
             "$cnn_article_trace"; do
    if [ ! -f "$trace/trace.meta" ] \
        || ! grep -q '^capture-complete=yes$' "$trace/trace.meta"; then
        printf 'Complete HTTP replay trace required: %s\n' "$trace" >&2
        exit 2
    fi
done
mkdir -p "$output_dir"

run_case() {
    name=$1
    url=$2
    trace=$3
    command_file=$4
    log="$output_dir/$name.log"
    "$lab" --url "$url" --fetch-scripts --ticks 3 --tick-ms 100 \
        --psp-profile realistic --limit-mb 24 --script-count 32 \
        --session-cache-kb 1024 --low-memory-navigation \
        --max-download-kb 16384 --no-loop-capture \
        --replay-http-response-keyed "$trace" \
        --commands "$command_file" > "$log" 2>&1
    grep -q '^loop status=PASS ' "$log"
    grep -q '^interactive status=ok ' "$log"
    grep -q '^javascript-callback-errors uncaught=0 last=""$' "$log"
    grep -Eq '^memory-categories phase=interactive-teardown current=0 expected=0 .*external-reserved=0 reconcile=yes$' "$log"
    grep -Eq '^interactive teardown=0 active=0 .*status=PASS$' "$log"
}

run_case wikipedia-reader \
    https://en.wikipedia.org/wiki/PlayStation_Portable \
    "$wikipedia_trace" "$commands/wikipedia-reader.commands"
run_case google-search https://www.google.com/ \
    "$google_trace" "$commands/google-search.commands"
run_case cnn-home https://www.cnn.com/ \
    "$cnn_trace" "$commands/cnn-home.commands"
run_case cnn-article \
    https://www.cnn.com/2026/08/30/business/kalshi-polymarket-casino-vegas-trump \
    "$cnn_article_trace" "$commands/cnn-article-reader.commands"

grep -q '^loop-reader mode=reader ' "$output_dir/wikipedia-reader.log"
grep -q '^loop-reader mode=raw ' "$output_dir/wikipedia-reader.log"
grep -q '^loop-body-contains found=yes text="Google needs JavaScript"$' \
    "$output_dir/google-search.log"
grep -q '^experimental-control id="lst-ib" value="PlayStation Portable"$' \
    "$output_dir/google-search.log"
grep -q 'title="Web search"' \
    "$output_dir/google-search.log"
grep -q '^loop-reader mode=reader ' "$output_dir/cnn-article.log"
grep -q '^loop-reader mode=raw ' "$output_dir/cnn-article.log"
grep -q 'title="Breaking News, Latest News and Videos | CNN"' \
    "$output_dir/cnn-home.log"

printf '%s\n' \
    'Everyday browsing cohort: PASS' \
    '  Wikipedia: Reader/Raw toggle retained a usable document.' \
    '  Google: query reached the explicit script-light fallback; Back restored the query.' \
    '  CNN: the full home page and a long article completed at 24 MiB; Reader/Raw switching remained usable.'
