#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
  printf 'usage: %s LOG [STABLE_PHASE [TEARDOWN_PHASE]]\n' "$0" >&2
  exit 2
fi

log=$1
stable_phase=${2:-stable-page}
teardown_phase=${3:-teardown}
if [ ! -f "$log" ]; then
  printf 'memory ledger log is missing: %s\n' "$log" >&2
  exit 2
fi

awk -v stable="$stable_phase" -v teardown="$teardown_phase" '
BEGIN {
    expected_name["uncategorized"] = 1
    expected_name["dom"] = 1
    expected_name["javascript"] = 1
    expected_name["style"] = 1
    expected_name["resource"] = 1
    expected_name["layout"] = 1
    expected_name["render"] = 1
    expected_name["session"] = 1
    expected_name["navigation"] = 1
}
function value(name,    i, pair) {
    for (i = 1; i <= NF; i++) {
        split($i, pair, "=")
        if (pair[1] == name) return pair[2]
    }
    return ""
}
function reject(message) {
    print "memory ledger failure: " message > "/dev/stderr"
    bad = 1
}
function unsigned_value(text) {
    return text ~ /^[0-9]+$/
}
$1 == "memory-categories" {
    phase = value("phase")
    selected = ""
    if (phase == stable) {
        stable_headers++
        selected = "stable"
        if (value("reconcile") != "yes")
            reject(stable " does not reconcile")
        if (!unsigned_value(value("current")) ||
            !unsigned_value(value("expected")) ||
            !unsigned_value(value("external-reserved")))
            reject(stable " header has missing/non-numeric fields")
        if (value("current") != value("expected"))
            reject(stable " current/category total differs")
    } else if (phase == teardown) {
        teardown_headers++
        selected = "teardown"
        if (value("reconcile") != "yes")
            reject(teardown " does not reconcile")
        if (!unsigned_value(value("current")) ||
            !unsigned_value(value("expected")) ||
            !unsigned_value(value("external-reserved")))
            reject(teardown " header has missing/non-numeric fields")
        if (value("current") + 0 != 0 || value("expected") + 0 != 0 ||
            value("external-reserved") + 0 != 0)
            reject(teardown " retains owned bytes")
    }
    next
}
$1 == "memory-category" && selected != "" {
    current_text = value("current")
    active_text = value("active")
    allocations_text = value("allocs")
    frees_text = value("frees")
    name = value("name")
    if (!(name in expected_name))
        reject(selected " reports unknown category " name)
    category_key = selected SUBSEP name
    category_seen[category_key]++
    if (category_seen[category_key] != 1)
        reject(selected " reports category " name " more than once")
    if (selected == "stable") stable_categories++
    else teardown_categories++
    if (!unsigned_value(current_text) || !unsigned_value(active_text) ||
        !unsigned_value(allocations_text) || !unsigned_value(frees_text)) {
        reject(selected " category " name " has missing/non-numeric fields")
        next
    }
    current = current_text + 0
    active = active_text + 0
    allocations = allocations_text + 0
    frees = frees_text + 0
    if (allocations < frees || allocations - frees != active)
        reject(selected " category " name " allocation/free imbalance")
    if (selected == "teardown" && (current != 0 || active != 0))
        reject("teardown category " name " remains active")
    next
}
END {
    if (stable_headers != 1)
        reject("expected one " stable " category header")
    if (teardown_headers != 1)
        reject("expected one " teardown " category header")
    if (stable_categories != 9)
        reject(stable " did not report all nine categories")
    if (teardown_categories != 9)
        reject(teardown " did not report all nine categories")
    for (name in expected_name) {
        if (category_seen["stable" SUBSEP name] != 1)
            reject(stable " is missing category " name)
        if (category_seen["teardown" SUBSEP name] != 1)
            reject(teardown " is missing category " name)
    }
    exit bad ? 1 : 0
}
' "$log"
