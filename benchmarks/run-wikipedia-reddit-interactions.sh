#!/bin/sh
set -eu

if [ "$#" -lt 5 ] || [ "$#" -gt 6 ]; then
    printf '%s\n' \
        "usage: $0 BUILD_DIR WIKIPEDIA_TRACE WIKIPEDIA_HOMEPAGE_TRACE REDDIT_SEARCH_TRACE REDDIT_POST_TRACE [OUTPUT_DIR]" >&2
    exit 2
fi

build_dir=$1
wikipedia_trace=$2
wikipedia_homepage_trace=$3
reddit_search_trace=$4
reddit_post_trace=$5
output_dir=${6:-interaction-results}
lab="$build_dir/psp-browser-interactive-lab"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
commands="$root/benchmarks/interactions"
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

check_trace() {
    trace=$1
    if [ ! -f "$trace/trace.meta" ] \
        || ! grep -q '^capture-complete=yes$' "$trace/trace.meta"; then
        printf 'Complete HTTP replay trace required: %s\n' "$trace" >&2
        exit 2
    fi
}

check_trace "$wikipedia_trace"
check_trace "$wikipedia_homepage_trace"
check_trace "$reddit_search_trace"
check_trace "$reddit_post_trace"
mkdir -p "$output_dir"

run_case() {
    name=$1
    url=$2
    trace=$3
    command_file=$4
    require_navigation=$5
    log="$output_dir/$name.log"

    "$lab" --url "$url" --fetch-scripts --ticks 5 --tick-ms 100 \
        --psp-profile realistic --limit-mb "$limit_mb" \
        --script-count "$script_count" \
        --session-cache-kb "$session_cache_kb" \
        --low-memory-navigation --no-loop-capture \
        --replay-http "$trace" --commands "$command_file" > "$log" 2>&1

    if grep -Eq 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' \
        "$log"; then
        printf 'Sanitizer diagnostic in %s\n' "$log" >&2
        grep -E 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' \
            "$log" >&2
        return 1
    fi
    grep -q '^loop status=PASS ' "$log"
    grep -q '^interactive status=ok ' "$log"
    grep -q '^javascript-error="" source=""$' "$log"
    grep -q '^javascript-callback-errors uncaught=0 last=""$' "$log"
    if [ "$require_navigation" = yes ]; then
        grep -Eq '^navigation-replacement mode=low-memory started=[1-9][0-9]* succeeded=[1-9][0-9]* failed=0 fallbacks=0 ' "$log"
    fi
    grep -Eq '^memory-categories phase=interactive-stable .*global-peak=[0-9]+ .*reconcile=yes$' "$log"
    grep -Eq '^memory-categories phase=interactive-teardown current=0 expected=0 .*external-reserved=0 reconcile=yes$' "$log"
    grep -Eq '^interactive teardown=0 active=0 .*failures=0 status=PASS$' "$log"
    grep -q "^runtime-policy scripts=$script_count " "$log"
}

run_case wikipedia-homepage https://en.wikipedia.org/wiki/Main_Page \
    "$wikipedia_homepage_trace" "$commands/wikipedia-homepage.commands" no
run_case wikipedia \
    https://en.wikipedia.org/wiki/PlayStation_Portable \
    "$wikipedia_trace" "$commands/wikipedia.commands" yes
run_case wikipedia-spatial \
    https://en.wikipedia.org/wiki/PlayStation_Portable \
    "$wikipedia_trace" "$commands/wikipedia-spatial-focus.commands" no
run_case reddit-search https://www.reddit.com/r/PSP/ \
    "$reddit_search_trace" "$commands/reddit-search.commands" yes
run_case reddit-post https://www.reddit.com/r/PSP/ \
    "$reddit_post_trace" "$commands/reddit-post.commands" yes

# The mobile homepage has ordinary content panels rather than collapsible
# article sections. Qualify its initial structure, deep-panel scrolling, and
# exact return to the top separately from the article section interaction.
grep -q '^loop-js ok=yes value="homepage:/wiki/Main_Page:9:0" error=""$' \
    "$output_dir/wikipedia-homepage.log"
grep -Eq '^loop-js ok=yes value="homepage-news:In the news:[1-9][0-9]*" error=""$' \
    "$output_dir/wikipedia-homepage.log"
grep -Eq '^loop-js ok=yes value="homepage-sisters:Wikipedia.s sister projects:[1-9][0-9]*" error=""$' \
    "$output_dir/wikipedia-homepage.log"
grep -q '^loop-js ok=yes value="homepage-top:/wiki/Main_Page:0" error=""$' \
    "$output_dir/wikipedia-homepage.log"
grep -q '^resources stylesheets=2/2 ' "$output_dir/wikipedia-homepage.log"
grep -q '^navigation loads=1 destroys=0 ' "$output_dir/wikipedia-homepage.log"
grep -Eq '^interactive status=ok title="Wikipedia, the free encyclopedia" .*scroll-y=0 ' \
    "$output_dir/wikipedia-homepage.log"

# Wikipedia's primary mobile stylesheet is large enough that an over-aggressive
# cache can leave a technically navigable but desktop-shaped final document.
# Qualify the bounded default by requiring both author sheets after six swaps.
grep -q '^resources stylesheets=2/2 ' "$output_dir/wikipedia.log"
grep -q '^navigation loads=7 destroys=6 ' "$output_dir/wikipedia.log"
grep -q '^navigation-replacement mode=low-memory started=6 succeeded=6 failed=0 fallbacks=0 ' \
    "$output_dir/wikipedia.log"
grep -q 'url="https://en.wikipedia.org/w/index.php?search=PlayStation+Vita' \
    "$output_dir/wikipedia.log"
grep -q 'url="https://en.wikipedia.org/wiki/PlayStation_Vita"' \
    "$output_dir/wikipedia.log"
article_before=$(sed -n \
    's/^loop-js ok=yes value="article-before:\([^"]*\)" error=""$/\1/p' \
    "$output_dir/wikipedia.log" | tail -n 1)
article_restored=$(sed -n \
    's/^loop-js ok=yes value="article-restored:\([^"]*\)" error=""$/\1/p' \
    "$output_dir/wikipedia.log" | tail -n 1)
if [ -z "$article_before" ] || [ "$article_before" != "$article_restored" ]; then
    printf 'Wikipedia Back did not restore article, scroll, and focus: %s -> %s\n' \
        "$article_before" "$article_restored" >&2
    exit 1
fi
grep -q '^loop-js ok=yes value="section-before:History" error=""$' \
    "$output_dir/wikipedia.log"
grep -q '^loop-js ok=yes value="section-open:History" error=""$' \
    "$output_dir/wikipedia.log"
section_states=$(awk '
    /^loop frame=/ {
        if (match($0, /scroll=[0-9]+\/[0-9]+/)) {
            scroll = substr($0, RSTART + 7, RLENGTH - 7)
            split(scroll, parts, "/")
            height = parts[2]
        }
        if (match($0, /links=[0-9]+/)) {
            links = substr($0, RSTART + 6, RLENGTH - 6)
        }
    }
    /loop-js ok=yes value="section-before:History"/ {
        print "collapsed " height " " links
    }
    /loop-js ok=yes value="section-open:History"/ {
        print "expanded " height " " links
    }
' "$output_dir/wikipedia.log")
collapsed_state=$(printf '%s\n' "$section_states" \
    | sed -n 's/^collapsed //p' | tail -n 1)
expanded_state=$(printf '%s\n' "$section_states" \
    | sed -n 's/^expanded //p' | tail -n 1)
collapsed_height=${collapsed_state%% *}
collapsed_links=${collapsed_state##* }
expanded_height=${expanded_state%% *}
expanded_links=${expanded_state##* }
if [ -z "$collapsed_state" ] || [ -z "$expanded_state" ] \
    || [ "$expanded_height" -le "$collapsed_height" ] \
    || [ "$expanded_links" -le "$collapsed_links" ]; then
    printf 'Wikipedia History section did not visibly expand: %s -> %s\n' \
        "$collapsed_state" "$expanded_state" >&2
    exit 1
fi
grep -Eq '^interactive status=ok title="PlayStation Portable - Wikipedia" .*scroll-y=[1-9][0-9]* ' \
    "$output_dir/wikipedia.log"
awk '
    /^interaction-latency command=(type|activate) / {
        if (match($0, /total-us=[0-9]+/)) {
            value = substr($0, RSTART + 9, RLENGTH - 9) + 0
            if (value > maximum) maximum = value
        } else {
            missing = 1
        }
    }
    END {
        if (missing || maximum > 150000) exit 1
        printf "Wikipedia input/activation latency: max-us=%d\n", maximum
    }
' "$output_dir/wikipedia.log"

# The explicit spatial commands use the same geometry-directed controller
# path as the PSP d-pad. Keep the destination sequence stable and prove that
# paint-only header controls stay below the host interaction budget while
# geometry, inheritance, and relational changes still fall back in unit
# coverage.
grep -q '^loop-js ok=yes value="spatial-start:searchIcon" error=""$' \
    "$output_dir/wikipedia-spatial.log"
grep -q '^loop-js ok=yes value="spatial-right:minerva-user-menu-checkbox" error=""$' \
    "$output_dir/wikipedia-spatial.log"
grep -q '^loop-js ok=yes value="spatial-down:minerva-user-menu-toggle" error=""$' \
    "$output_dir/wikipedia-spatial.log"
grep -q '^loop-js ok=yes value="spatial-left:searchIcon" error=""$' \
    "$output_dir/wikipedia-spatial.log"
grep -q '^loop-js ok=yes value="spatial-up:minerva-user-menu-checkbox" error=""$' \
    "$output_dir/wikipedia-spatial.log"
grep -q '^controller moves=5 ' "$output_dir/wikipedia-spatial.log"
grep -Eq '^relayout-policy .*focus-outline-skips=0 focus-paint-skips=5$' \
    "$output_dir/wikipedia-spatial.log"
awk '
    /^interaction-latency command=focus-(id|up|down|left|right) / {
        samples++
        if (match($0, /total-us=[0-9]+/)) {
            value = substr($0, RSTART + 9, RLENGTH - 9) + 0
            if (value > maximum) maximum = value
        } else {
            missing = 1
        }
    }
    END {
        if (samples != 5 || missing || maximum > 8000) exit 1
        printf "Wikipedia spatial focus latency: samples=%d max-us=%d\n",
               samples, maximum
    }
' "$output_dir/wikipedia-spatial.log"

# Each Reddit corpus includes two document navigations plus the final Back.
# Search uses native implicit form submission; the post case exercises an
# ordinary link and scroll restoration. Rejections from deliberately bounded
# optional hydration bundles are reported, but must not become fatal/callback
# errors or prevent the server-rendered interactions.
for name in reddit-search reddit-post; do
    grep -q '^navigation loads=4 destroys=3 ' "$output_dir/$name.log"
    grep -q '^navigation-replacement mode=low-memory started=3 succeeded=3 failed=0 fallbacks=0 ' \
        "$output_dir/$name.log"
    rejection_line=$(grep '^javascript-rejections ' "$output_dir/$name.log")
    rejection_count=$(printf '%s\n' "$rejection_line" \
        | sed -n 's/.* count=\([0-9][0-9]*\) .*/\1/p')
    undefined_count=$(printf '%s\n' "$rejection_line" \
        | sed -n 's/.* undefined=\([0-9][0-9]*\) .*/\1/p')
    dynamic_line=$(grep '^javascript-dynamic-scripts ' "$output_dir/$name.log")
    dynamic_failed=$(printf '%s\n' "$dynamic_line" \
        | sed -n 's/.* failed=\([0-9][0-9]*\) .*/\1/p')
    if [ -z "$rejection_count" ] || [ -z "$undefined_count" ] \
        || [ -z "$dynamic_failed" ]; then
        printf 'Missing rejection attribution in %s\n' "$output_dir/$name.log" >&2
        exit 1
    fi
    if [ "$rejection_count" -ne 0 ] \
        && { [ "$dynamic_failed" -eq 0 ] \
             || [ "$undefined_count" -ne "$rejection_count" ]; }; then
        printf 'Unattributed Reddit promise rejection in %s\n' \
            "$output_dir/$name.log" >&2
        exit 1
    fi
done

grep -q 'url="https://www.reddit.com/search/?q=&q=PlayStation+Portable"' \
    "$output_dir/reddit-search.log"
grep -Eq '^interactive status=ok title="Reddit - The heart of the internet" .*scroll-y=0 ' \
    "$output_dir/reddit-search.log"
grep -q '^resources stylesheets=12/12 ' "$output_dir/reddit-search.log"
grep -q 'url="https://www.reddit.com/r/PSP/comments/' \
    "$output_dir/reddit-post.log"
grep -Eq '^interactive status=ok title="Reddit - The heart of the internet" .*scroll-y=[1-9][0-9]* ' \
    "$output_dir/reddit-post.log"
grep -q '^resources stylesheets=4/4 ' "$output_dir/reddit-post.log"

printf 'Wikipedia homepage/article and Reddit interaction qualification passed: %s (limit=%s MiB scripts=%s cache=%s KiB)\n' \
    "$output_dir" "$limit_mb" "$script_count" "$session_cache_kb"
