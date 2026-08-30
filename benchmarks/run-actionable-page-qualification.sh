#!/bin/sh
set -eu

build_dir=${1:-build-preset-release}
output_dir=${2:-"$build_dir/actionable-page-qualification"}
lab="$build_dir/psp-browser-interactive-lab"
log="$output_dir/run.log"

test -x "$lab"
mkdir -p "$output_dir"

"$lab" \
  --fixture fixtures/actionable-page.html \
  --commands fixtures/actionable-page.commands \
  --fetch-scripts --ticks 2 --no-loop-capture \
  --output "$output_dir/final.ppm" >"$log" 2>&1

# The inert javascript: URL and the throwing optional handler must not turn
# either Cross press into a failed command.
if grep -q '^loop command=.* status=FAIL' "$log"; then
  cat "$log" >&2
  exit 1
fi

# Fragment activation, Back, and Forward stay inside the committed document:
# the URL changes in both directions while the load count remains one.
grep -Eq '^loop frame=.*url="https://fixture\.test/#details".*scroll=[1-9][0-9]*/.*loads=1 ' "$log"
grep -Eq '^loop frame=.*url="https://fixture\.test/".*loads=1 ' "$log"
test "$(grep -Ec '^loop frame=.*url="https://fixture\.test/#details".*loads=1 ' "$log")" -ge 2
grep -q '^experimental-node id="details-copy" text="The fragment target is still reachable\."$' "$log"
grep -q '^interactive teardown=0 active=0 .*status=PASS$' "$log"

printf 'Actionable page qualification passed: %s\n' "$log"
