#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
manifest=${2:-"$root/benchmarks/experimental-paired-acceptance.tsv"}
replay_root=${3:-"$root/../tilefinch-corpora/acceptance-psp-envelope-interactive-20260716"}
output=${4:-"${TMPDIR:-/tmp}/tilefinch-experimental-paired"}
repetitions=${5:-3}
transport=${6:-replay}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
interactive="$build/psp-browser-interactive-lab"
commands="$root/fixtures/section-resource-commands.txt"

case "$repetitions" in
  ''|*[!0-9]*) printf 'repetitions must be an odd integer\n' >&2; exit 2 ;;
esac
[ "$repetitions" -gt 0 ] && [ $((repetitions % 2)) -eq 1 ] || {
    printf 'repetitions must be an odd integer\n' >&2
    exit 2
}
case "$transport" in replay|fixture) ;; *)
    printf 'transport must be replay or fixture\n' >&2; exit 2 ;;
esac
[ -x "$interactive" ] && [ -f "$manifest" ] && [ -f "$commands" ] || {
    printf 'paired benchmark inputs are missing\n' >&2
    exit 2
}
mkdir -p "$output"
runs="$output/runs.tsv"
summary="$output/summary.tsv"
printf 'site\tpath\trepetition\tstatus\tmode\treal_s\tpeak_bytes\tready_max_us\n' >"$runs"
printf 'site\tpath\tstatus\tmode\tmedian_real_s\tmedian_peak_bytes\tmax_ready_us\n' >"$summary"

field() {
    pattern=$1
    file=$2
    sed -n "s/.*$pattern\([0-9][0-9]*\).*/\\1/p" "$file" | tail -1
}

median_column() {
    path=$1
    sort -n "$path" | awk -v n="$repetitions" 'NR == (n + 1) / 2 { print $1 }'
}

run_path() {
    site=$1
    url=$2
    limit=$3
    max_download=$4
    path=$5
    minimum_body=$6
    body_file="$replay_root/$site/0000.body"
    case "$minimum_body" in
      ''|*[!0-9]*)
        printf '%s\t%s\t0\tfail\tinvalid-minimum\t0\t0\t0\n' \
            "$site" "$path" >>"$runs"
        printf '%s\t%s\tfail\tinvalid-minimum\t0\t0\t0\n' \
            "$site" "$path" >>"$summary"
        return 1
        ;;
    esac
    body_bytes=0
    if [ -f "$body_file" ]; then
        body_bytes=$(wc -c <"$body_file")
    fi
    if [ "$body_bytes" -lt "$minimum_body" ]; then
        printf '%s: captured body is %s bytes; minimum is %s\n' \
            "$site" "$body_bytes" "$minimum_body" >&2
        printf '%s\t%s\t0\tfail\tbody-too-small\t0\t0\t0\n' \
            "$site" "$path" >>"$runs"
        printf '%s\t%s\tfail\tbody-too-small\t0\t0\t0\n' \
            "$site" "$path" >>"$summary"
        return 1
    fi
    values_real="$output/$site-$path-real.values"
    values_peak="$output/$site-$path-peak.values"
    : >"$values_real"
    : >"$values_peak"
    overall=pass
    selected_mode=default
    maximum_ready=0
    repetition=1
    while [ "$repetition" -le "$repetitions" ]; do
        log="$output/$site-$path-$repetition.log"
        if [ "$transport" = fixture ]; then
            set -- "$interactive" --fixture "$replay_root/$site/0000.body" \
                --limit-mb "$limit" --no-external-resources \
                --commands "$commands" --no-loop-capture \
                --output "$output/$site-$path-$repetition.ppm"
        else
            set -- "$interactive" --url "$url" --limit-mb "$limit" \
                --max-download-kb "$max_download" --no-external-resources \
                --commands "$commands" --no-loop-capture \
                --replay-http "$replay_root/$site" \
                --deterministic-replay-seed 42 \
                --output "$output/$site-$path-$repetition.ppm"
        fi
        if [ "$path" = experimental ]; then
            set -- "$@" --experimental-compressed-sections
        fi
        set +e
        /usr/bin/time -p "$@" >"$log" 2>&1
        code=$?
        set -e
        real=$(sed -n 's/^real[[:space:]][[:space:]]*//p' "$log" | tail -1)
        peak=$(field 'interactive teardown=.* peak=' "$log")
        ready=$(field 'experimental-timing .*ready-max-us=' "$log")
        mode=$(sed -n 's/^experimental-adaptive mode=\([^ ]*\).*/\1/p' "$log" | tail -1)
        real=${real:-0}; peak=${peak:-0}; ready=${ready:-0}
        mode=${mode:-default}
        status=pass
        if [ "$code" -ne 0 ] || ! grep -q 'interactive teardown=.*status=PASS' "$log"; then
            status=fail
            overall=fail
        else
            printf '%s\n' "$real" >>"$values_real"
            printf '%s\n' "$peak" >>"$values_peak"
        fi
        [ "$ready" -le "$maximum_ready" ] || maximum_ready=$ready
        selected_mode=$mode
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$site" "$path" "$repetition" "$status" "$mode" "$real" \
            "$peak" "$ready" >>"$runs"
        repetition=$((repetition + 1))
    done
    median_real=0
    median_peak=0
    if [ "$overall" = pass ]; then
        median_real=$(median_column "$values_real")
        median_peak=$(median_column "$values_peak")
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$site" "$path" "$overall" "$selected_mode" "$median_real" \
        "$median_peak" "$maximum_ready" >>"$summary"
    [ "$overall" = pass ]
}

failures=0
tab=$(printf '\t')
while IFS="$tab" read -r name url limit max_download minimum_body purpose; do
    case "$name" in ''|'#'*|name) continue ;; esac
    if ! run_path "$name" "$url" "$limit" "$max_download" default \
            "$minimum_body"; then
        failures=$((failures + 1))
    fi
    if ! run_path "$name" "$url" "$limit" "$max_download" experimental \
            "$minimum_body"; then
        failures=$((failures + 1))
    fi
done <"$manifest"
cat "$summary"
[ "$failures" -eq 0 ]
