#!/bin/sh
set -eu

if [ "$#" -lt 5 ] || [ "$#" -gt 6 ]; then
    printf '%s\n' \
        "usage: $0 BUILD_DIR WIKIPEDIA_TRACE WIKIPEDIA_HOMEPAGE_TRACE REDDIT_SEARCH_TRACE REDDIT_POST_TRACE [OUTPUT_DIR]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="$root/benchmarks/run-wikipedia-reddit-interactions.sh"
build_dir=$1
wikipedia_trace=$2
wikipedia_homepage_trace=$3
reddit_search_trace=$4
reddit_post_trace=$5
output_dir=${6:-interaction-resource-matrix}
summary="$output_dir/summary.tsv"

mkdir -p "$output_dir"
printf 'limit_mb\tcache_kb\tqualification\tresource_quality\twikipedia_homepage_peak\twikipedia_peak\treddit_search_peak\treddit_post_peak\n' \
    > "$summary"

run_configuration() {
    limit_mb=$1
    cache_kb=$2
    name="limit-${limit_mb}-cache-${cache_kb}"
    directory="$output_dir/$name"
    status=PASS
    if ! INTERACTION_LIMIT_MB=$limit_mb \
         INTERACTION_SESSION_CACHE_KB=$cache_kb \
         "$runner" "$build_dir" "$wikipedia_trace" \
             "$wikipedia_homepage_trace" "$reddit_search_trace" \
             "$reddit_post_trace" "$directory"; then
        status=FAIL
    fi
    quality=COMPLETE
    if ! grep -q '^resources stylesheets=2/2 ' \
            "$directory/wikipedia-homepage.log" \
        || ! grep -q '^resources stylesheets=2/2 ' "$directory/wikipedia.log" \
        || ! grep -q '^resources stylesheets=12/12 ' \
            "$directory/reddit-search.log" \
        || ! grep -q '^resources stylesheets=4/4 ' \
            "$directory/reddit-post.log"; then
        quality=DEGRADED
    fi
    peaks=""
    for site in wikipedia-homepage wikipedia reddit-search reddit-post; do
        peak=-
        if [ -f "$directory/$site.log" ]; then
            value=$(sed -n 's/^memory-categories phase=interactive-[^ ]* .*global-peak=\([0-9][0-9]*\) .*/\1/p' \
                "$directory/$site.log" | tail -n 1)
            if [ -n "$value" ]; then peak=$value; fi
        fi
        peaks="$peaks\t$peak"
    done
    printf '%s\t%s\t%s\t%s%b\n' "$limit_mb" "$cache_kb" "$status" \
        "$quality" "$peaks" >> "$summary"
    [ "$status" = PASS ] && [ "$quality" = COMPLETE ]
}

run_configuration 20 1024 || true
run_configuration 22 1024 || true
run_configuration 24 512 || true
if ! run_configuration 24 1024; then
    printf 'Required 24 MiB / 1 MiB-cache configuration failed; see %s\n' \
        "$summary" >&2
    exit 1
fi

printf 'Wikipedia/Reddit resource matrix complete: %s\n' "$summary"
