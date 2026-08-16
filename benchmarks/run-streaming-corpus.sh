#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-bellard-current}
corpus=${2:-"$root/../tilefinch-corpora/streaming-20260715"}
output=${3:-"$root/streaming-corpus-results"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-lab"
manifest="$corpus/manifest.tsv"
[ -x "$lab" ] && [ -f "$manifest" ] || {
    printf 'streaming corpus inputs are missing\n' >&2
    exit 2
}
mkdir -p "$output"
summary="$output/summary.tsv"
printf 'site\tresult\tbytes\tpeak_buffer\tcommands\tpage_height\tpeak_bytes\n' >"$summary"
failures=0
tab=$(printf '\t')
while IFS="$tab" read -r name url limit max_download minimum_body purpose; do
    case "$name" in ''|'#'*|name) continue ;; esac
    log="$output/$name.log"
    layout="$output/$name-layout.tsv"
    links="$output/$name-links.tsv"
    set +e
    "$lab" --url "$url" --reader-profile none \
        --limit-mb "$limit" --max-download-kb "$max_download" \
        --fetch-css --max-stylesheets 8 --max-css-kb 1536 \
        --max-css-file-kb 512 --fetch-images --max-images 32 \
        --max-image-kb 768 --max-image-file-kb 256 \
        --max-decoded-image-kb 512 --resource-stage-ms 3000 \
        --viewport-width 480 --viewport-height 272 --no-render \
        --replay-http "$corpus/http/$name" \
        --deterministic-replay-seed 42 --dump-links "$links" \
        --dump-layout "$layout" >"$log" 2>&1
    code=$?
    set -e
    bytes=$(sed -n 's/.*stream bytes=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    peak_buffer=$(sed -n 's/.*peak-buffer=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    commands=$(sed -n 's/.* commands=\([0-9][0-9]*\) .*/\1/p' "$log" | tail -1)
    page_height=$(sed -n 's/.* page_height=\([0-9][0-9]*\) .*/\1/p' "$log" | tail -1)
    peak_bytes=$(sed -n 's/.* peak_bytes=\([0-9][0-9]*\) .*/\1/p' "$log" | tail -1)
    bytes=${bytes:-0}; peak_buffer=${peak_buffer:-0}; commands=${commands:-0}
    page_height=${page_height:-0}; peak_bytes=${peak_bytes:-0}
    result=pass
    if [ "$name" = whatwg-html ] && [ "$code" -ne 0 ]; then
        result=clean-pressure-rejection
        grep -q 'memory teardown       current=  0.00 MiB' "$log" \
            || result=fail
    elif [ "$code" -ne 0 ] || [ "$bytes" -lt "$minimum_body" ] \
        || [ "$peak_buffer" -gt 65536 ] || [ "$commands" -eq 0 ]; then
        result=fail
    fi
    [ "$result" != fail ] || failures=$((failures + 1))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$result" "$bytes" "$peak_buffer" "$commands" \
        "$page_height" "$peak_bytes" >>"$summary"
done <"$manifest"
cat "$summary"
[ "$failures" -eq 0 ]
