#!/bin/sh
set -eu

# A PSPLink `ld` only acknowledges module startup. Validation logging is
# buffered and the host0 file may not contain its final lines until teardown.
# Match a newly rotated report, then wait for its terminal record.

if [ "$#" -ne 4 ]; then
    echo "usage: $0 REPORT PREVIOUS_INODE TIMEOUT_SECONDS REQUIRE_SCRIPT" >&2
    exit 2
fi
report=$1
previous_inode=$2
timeout_seconds=$3
require_script=$4
for value in "$previous_inode" "$timeout_seconds" "$require_script"; do
    case "$value" in
        ''|*[!0-9]*)
            echo "invalid PSPLink report wait arguments" >&2
            exit 2 ;;
    esac
done
if [ "$timeout_seconds" -le 0 ] || [ "$timeout_seconds" -gt 3600 ] \
    || { [ "$require_script" -ne 0 ] && [ "$require_script" -ne 1 ]; }; then
    echo "invalid PSPLink report wait bounds" >&2
    exit 2
fi

elapsed=0
while [ "$elapsed" -le "$timeout_seconds" ]; do
    current_inode=$(ls -id "$report" 2>/dev/null | awk 'NR == 1 { print $1 }' || true)
    if [ -n "$current_inode" ] && [ "$current_inode" != "$previous_inode" ]; then
        if grep -q '^tilefinch-log: finish outcome=clean-exit healthy=1$' \
            "$report"; then
            if [ "$require_script" -eq 1 ] \
                && ! grep -Eq \
                    '^tilefinch-input-script: outcome=(complete|exit-action)( |$)' \
                    "$report"; then
                echo "PSPLink report ended cleanly but its input script did not complete" >&2
                grep 'tilefinch-input-script: outcome=' "$report" >&2 || true
                exit 1
            fi
            grep -E 'tilefinch-input-script: outcome=|tilefinch-validation: interactive-loops=|tilefinch-validation: outcome=clean-exit' \
                "$report" || true
            echo "PSPLink validation completed: $report"
            exit 0
        fi
        if grep -Eq \
            '^tilefinch-log: finish outcome=(halted|failed|crashed)' \
            "$report"; then
            echo "PSPLink validation ended unsuccessfully: $report" >&2
            tail -30 "$report" >&2
            exit 1
        fi
    fi
    if [ "$elapsed" -eq "$timeout_seconds" ]; then break; fi
    sleep 1
    elapsed=$((elapsed + 1))
done
echo "PSPLink validation did not publish a terminal report in ${timeout_seconds}s: $report" >&2
if [ -f "$report" ]; then tail -20 "$report" >&2; fi
exit 1
