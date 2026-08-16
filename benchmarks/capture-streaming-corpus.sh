#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
output=${2:-"$root/../tilefinch-corpora/streaming-current"}
manifest=${3:-"$root/benchmarks/acceptance-sites-streaming.tsv"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-lab"

if [ ! -x "$lab" ]; then
    printf 'Capture lab is missing: %s\n' "$lab" >&2
    exit 2
fi
mkdir -p "$output/http" "$output/results"
cp "$manifest" "$output/manifest.tsv"
summary="$output/summary.tsv"
printf 'site\tstatus\thttp_status\tbody_bytes\trequests\tcontent_type\tengine_status\n' >"$summary"

failures=0
tab=$(printf '\t')
while IFS="$tab" read -r name url limit max_download minimum_body purpose; do
    case "$name" in ''|'#'*|name) continue ;; esac
    trace="$output/http/$name"
    result="$output/results/$name"
    log="$output/results/$name.log"
    mkdir -p "$trace" "$result"
    printf 'Capturing %s\n' "$name"
    engine_status=ok
    if ! "$lab" --url "$url" --reader-profile none \
        --limit-mb "$limit" --max-download-kb "$max_download" \
        --fetch-css --max-stylesheets 8 --max-css-kb 1536 \
        --max-css-file-kb 512 --fetch-images --max-images 32 \
        --max-image-kb 768 --max-image-file-kb 256 \
        --max-decoded-image-kb 512 --resource-stage-ms 3000 \
        --viewport-width 480 --viewport-height 272 --no-render \
        --capture-http "$trace" --deterministic-replay-seed 42 \
        --dump-links "$result/links.tsv" \
        --dump-layout "$result/layout.tsv" >"$log" 2>&1; then
        engine_status=failed
    fi

    meta="$trace/0000.meta"
    body="$trace/0000.body"
    status=invalid
    http_status=-
    body_bytes=0
    content_type=-
    requests=$(find "$trace" -name '*.meta' ! -name trace.meta -type f | wc -l | tr -d ' ')
    if [ -f "$meta" ]; then
        http_status=$(sed -n 's/^status=//p' "$meta" | tail -n 1)
        content_type=$(sed -n 's/^content-type=//p' "$meta" | tail -n 1)
    fi
    if [ -f "$body" ]; then body_bytes=$(wc -c <"$body" | tr -d ' '); fi

    if [ "$http_status" = 200 ] \
        && [ "$body_bytes" -ge "$minimum_body" ] \
        && printf '%s' "$content_type" | grep -Eqi '(^|/)html|xhtml'; then
        status=captured
    else
        status=invalid-response
    fi
    if [ -f "$body" ] && head -c 262144 "$body" | grep -Eiq \
        'cf-chl-|enable javascript and cookies to continue|captcha|robot check|access denied|unusual traffic'; then
        status=suspected-challenge
    fi
    if [ "$engine_status" != ok ] && [ "$status" = captured ]; then
        status=captured-engine-failed
    fi
    case "$status" in
        captured|captured-engine-failed) ;;
        *) failures=$((failures + 1)) ;;
    esac
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$status" "$http_status" "$body_bytes" "$requests" \
        "$content_type" "$engine_status" >>"$summary"
done <"$manifest"

printf 'captured_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" \
    >"$output/capture.txt"
printf 'Streaming corpus capture complete: %s (%s flagged)\n' \
    "$output" "$failures"
cat "$summary"
[ "$failures" -eq 0 ]
