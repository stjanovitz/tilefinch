#!/bin/sh
set -eu

if [ "$#" -lt 5 ] || [ "$#" -gt 7 ]; then
    printf '%s\n' \
        "usage: $0 BUILD_DIR WIKIPEDIA_TRACE WIKIPEDIA_HOMEPAGE_TRACE REDDIT_SEARCH_TRACE REDDIT_POST_TRACE [OUTPUT_DIR [ROUNDS]]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="$root/benchmarks/run-wikipedia-reddit-interactions.sh"
build_dir=$1
wikipedia_trace=$2
wikipedia_homepage_trace=$3
reddit_search_trace=$4
reddit_post_trace=$5
output_dir=${6:-interaction-soak-results}
rounds=${7:-3}
maximum_peak_drift=${INTERACTION_SOAK_MAX_PEAK_DRIFT:-524288}

case $rounds in ''|*[!0-9]*) rounds=0 ;; esac
if [ "$rounds" -lt 2 ] || [ "$rounds" -gt 5 ]; then
    printf 'Rounds must be between 2 and 5\n' >&2
    exit 2
fi
case $maximum_peak_drift in
    ''|*[!0-9]*)
        printf 'INTERACTION_SOAK_MAX_PEAK_DRIFT must be a byte count\n' >&2
        exit 2
        ;;
esac

mkdir -p "$output_dir"
summary="$output_dir/summary.tsv"
printf 'round\tsite\tpeak_bytes\tstable_bytes\trejections\tdynamic_failures\n' \
    > "$summary"
started=$(date +%s)
round=1
while [ "$round" -le "$rounds" ]; do
    round_dir="$output_dir/round-$round"
    "$runner" "$build_dir" "$wikipedia_trace" "$wikipedia_homepage_trace" \
        "$reddit_search_trace" "$reddit_post_trace" "$round_dir"
    for site in wikipedia-homepage wikipedia reddit-search reddit-post; do
        log="$round_dir/$site.log"
        peak=$(sed -n 's/^memory-categories phase=interactive-stable .*global-peak=\([0-9][0-9]*\) .*/\1/p' \
            "$log" | tail -n 1)
        stable=$(sed -n 's/^memory-categories phase=interactive-stable current=\([0-9][0-9]*\) .*/\1/p' \
            "$log" | tail -n 1)
        rejections=$(sed -n 's/^javascript-rejections count=\([0-9][0-9]*\) .*/\1/p' \
            "$log" | tail -n 1)
        dynamic_failures=$(sed -n 's/^javascript-dynamic-scripts .* failed=\([0-9][0-9]*\) .*/\1/p' \
            "$log" | tail -n 1)
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$round" "$site" \
            "$peak" "$stable" "${rejections:-0}" "${dynamic_failures:-0}" \
            >> "$summary"
    done
    round=$((round + 1))
done

for site in wikipedia-homepage wikipedia reddit-search reddit-post; do
    bounds=$(awk -F '\t' -v site="$site" '
        NR > 1 && $2 == site {
            if (count == 0 || $3 < minimum) minimum=$3
            if (count == 0 || $3 > maximum) maximum=$3
            count++
        }
        END { if (count != 0) print minimum, maximum }
    ' "$summary")
    minimum=${bounds%% *}
    maximum=${bounds##* }
    drift=$((maximum - minimum))
    if [ "$drift" -gt "$maximum_peak_drift" ]; then
        printf 'Peak drift exceeded for %s: %s > %s bytes\n' \
            "$site" "$drift" "$maximum_peak_drift" >&2
        exit 1
    fi
done

elapsed=$(( $(date +%s) - started ))
printf 'Wikipedia/Reddit interaction soak passed: rounds=%s elapsed=%ss summary=%s\n' \
    "$rounds" "$elapsed" "$summary"
