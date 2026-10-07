#!/bin/sh
# Replay each target corpus once and summarize UBSan reports. They are
# non-fatal by default, and fork mode discards child output, so run this
# after a campaign.  usage: tests/fuzz/replay_ub.sh TARGET...
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "${TILEFINCH_FUZZ_BUILD:-$root/build-fuzz}" && mkdir -p logs
for t in "$@"; do
    UBSAN_OPTIONS=print_stacktrace=0:report_error_type=1 \
        tests/fuzz/fuzz_$t -runs=0 -rss_limit_mb=4096 fuzz-work/$t/corpus \
        > logs/ub-$t.log 2>&1
    echo "== $t: $(grep -c 'runtime error' logs/ub-$t.log)"
    grep 'runtime error' logs/ub-$t.log | sed "s|$root/||" \
        | sort | uniq -c | sort -rn | head -12
done
