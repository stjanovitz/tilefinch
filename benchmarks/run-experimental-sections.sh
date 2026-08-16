#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
corpus=${2:-"$root/../tilefinch-corpora/streaming-20260715"}
output=${3:-"$root/experimental-section-results"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
interactive="$build/psp-browser-interactive-lab"
manifest="$corpus/manifest.tsv"
commands="$root/fixtures/section-resource-commands.txt"
[ -x "$interactive" ] && [ -f "$manifest" ] && [ -f "$commands" ] || {
    printf 'experimental section corpus inputs are missing\n' >&2
    exit 2
}

mkdir -p "$output"
summary="$output/summary.tsv"
printf 'site\tresult\tsource\tstored\tsections\tanchors\tswaps\tpeak_bytes\n' \
    >"$summary"
failures=0
tab=$(printf '\t')
while IFS="$tab" read -r name url unused_limit max_download minimum_body purpose; do
    case "$name" in ''|'#'*|name) continue ;; esac
    log="$output/$name.log"
    set +e
    "$interactive" --url "$url" \
        --psp-profile strict --max-download-kb "$max_download" \
        --no-external-resources --experimental-compressed-sections \
        --experimental-section 0 --commands "$commands" --no-loop-capture \
        --replay-http "$corpus/http/$name" --deterministic-replay-seed 42 \
        --output "$output/$name.ppm" >"$log" 2>&1
    code=$?
    set -e
    source_bytes=$(sed -n 's/.*experimental-pager-input source=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    stored_bytes=$(sed -n 's/.* stored=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    sections=$(sed -n 's/.*experimental-pager-input .* sections=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    anchors=$(sed -n 's/.* anchors=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    swaps=$(sed -n 's/.*experimental-pager status=active.* swaps=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    peak=$(sed -n 's/.*interactive teardown=.* peak=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
    source_bytes=${source_bytes:-0}; stored_bytes=${stored_bytes:-0}
    sections=${sections:-0}; anchors=${anchors:-0}; swaps=${swaps:-0}
    peak=${peak:-0}
    result=pass
    if [ "$code" -ne 0 ] || [ "$source_bytes" -lt "$minimum_body" ] \
        || ! grep -q '^loop status=PASS ' "$log" \
        || ! grep -q '^interactive teardown=0 .* status=PASS$' "$log"; then
        result=fail
        failures=$((failures + 1))
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$result" "$source_bytes" "$stored_bytes" "$sections" \
        "$anchors" "$swaps" "$peak" >>"$summary"
done <"$manifest"
cat "$summary"
[ "$failures" -eq 0 ]
