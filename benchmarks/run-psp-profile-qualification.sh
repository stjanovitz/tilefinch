#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
output_dir=${2:-psp-profile-results}
cycles=${3:-100}
lab="$build_dir/psp-browser-lab"
summary="$output_dir/summary.tsv"

if [ ! -x "$lab" ]; then
  printf 'Build the lab first: %s\n' "$lab" >&2
  exit 2
fi
case "$cycles" in
  ''|*[!0-9]*) printf 'cycles must be a positive integer\n' >&2; exit 2 ;;
esac
if [ "$cycles" -lt 1 ] || [ "$cycles" -gt 1000 ]; then
  printf 'cycles must be between 1 and 1000\n' >&2
  exit 2
fi

mkdir -p "$output_dir"
printf 'profile\tlimit_mb\tfixture_peak\tstress_growth\tasync_status\n' \
  > "$summary"

qualify() {
  profile=$1
  limit=$2
  profile_dir="$output_dir/$profile"
  mkdir -p "$profile_dir"

  fixture_log="$profile_dir/fixture.log"
  "$lab" --fixture fixtures/mobile-viewport.html \
    --psp-profile "$profile" --reader-profile none --scroll-all \
    --output-dir "$profile_dir/fixture-frames" > "$fixture_log" 2>&1
  grep -q 'benchmark status=ok' "$fixture_log"
  grep -q 'memory teardown       current=  0.00 MiB' "$fixture_log"
  fixture_peak=$(sed -n \
    's/.*benchmark status=ok .*peak_bytes=\([0-9][0-9]*\).*/\1/p' \
    "$fixture_log" | tail -n 1)
  if [ -z "$fixture_peak" ] || [ "$fixture_peak" -ge $((limit * 1024 * 1024)) ]; then
    printf '%s fixture exceeded its %s MiB profile\n' "$profile" "$limit" >&2
    exit 1
  fi

  lifecycle_log="$profile_dir/lifecycle.log"
  "$lab" --fixture fixtures/demo.html --psp-profile "$profile" \
    --reader-profile none --navigation-stress "$cycles" --no-render \
    > "$lifecycle_log" 2>&1
  grep -q 'navigation-stress .* status=PASS' "$lifecycle_log"

  sh benchmarks/run-session-stress.sh "$build_dir" \
    "$profile_dir/session" "$cycles" "$limit"
  stress_growth=$(sed -n \
    's/^loop steady=yes .* growth=\([0-9][0-9]*\)$/\1/p' \
    "$profile_dir/session/session.log" | tail -n 1)

  sh benchmarks/run-async-network.sh "$build_dir" \
    "$profile_dir/async" "$limit"
  printf '%s\t%s\t%s\t%s\tpass\n' \
    "$profile" "$limit" "$fixture_peak" "$stress_growth" >> "$summary"
}

qualify strict 16
qualify realistic 24
printf 'PSP profile qualification passed: %s\n' "$summary"
