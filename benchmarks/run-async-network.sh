#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
output_dir=${2:-async-network-results}
limit_mb=${3:-24}
interactive="$build_dir/psp-browser-interactive-lab"
lab="$build_dir/psp-browser-lab"
log="$output_dir/session.log"

if [ ! -x "$interactive" ] || [ ! -x "$lab" ]; then
  printf 'Build both labs first: %s and %s\n' "$interactive" "$lab" >&2
  exit 2
fi
case "$limit_mb" in
  16|24) ;;
  *) printf 'limit must be 16 or 24 MiB\n' >&2; exit 2 ;;
esac

mkdir -p "$output_dir"
python3 fixtures/server.py > "$output_dir/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT HUP INT TERM

ready=0
i=0
while [ "$i" -lt 50 ]; do
  if curl --silent --fail http://127.0.0.1:8765/async-network.html \
      > /dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
  i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
  printf 'fixture server did not become ready\n' >&2
  exit 1
fi

"$interactive" --url http://127.0.0.1:8765/async-network.html \
  --ticks 64 --tick-ms 16 --limit-mb "$limit_mb" \
  --output "$output_dir/final.ppm" > "$log" 2>&1

grep -q 'javascript summary="async:yes:visible,yes,yes,yes:visible:true:1.2.3.4:loadstart.progress.load.loadend,0-255-128-65:application/octet-stream,fetch-0-255-128-65:blob-0-255-128-65,body-TypeError:json-SyntaxError,stream-xy-TypeError,http://127.0.0.1:8765:origin-check,postbytes-0-255-128-65:AbortError,TimeoutError"' "$log"
grep -q 'javascript-network requests=10 failures=2 status=200' "$log"
grep -q 'javascript-network-async queued=10 completed=10 rejected=0 cancelled=1 timed-out=1 peak-inflight=4' "$log"
grep -q 'javascript-xhr sends=3 last-error=""' "$log"
grep -q 'javascript-xhr-response responses=2 status=200 type="arraybuffer" text-units=0 bytes=4 states="1.2.3.4"' "$log"
grep -q 'javascript-promises unhandled=0 last=""' "$log"
grep -q 'interactive teardown=0 active=0 largest=0 .* status=PASS' "$log"

resource_log="$output_dir/resources.log"
"$interactive" --url http://127.0.0.1:8765/unified-resources.html \
  --ticks 8 --tick-ms 16 --limit-mb "$limit_mb" \
  --output "$output_dir/resources.ppm" > "$resource_log" 2>&1
grep -q 'javascript summary="imports:rgb(36, 104, 19):rgb(220, 233, 247):yes"' \
  "$resource_log"
grep -q 'resources stylesheets=3/5 css-imports=2/4 css-import-skips=2/0' \
  "$resource_log"
grep -q 'interactive teardown=0 active=0 largest=0 .* status=PASS' \
  "$resource_log"

phase_log="$output_dir/resource-phase.log"
"$lab" --url http://127.0.0.1:8765/slow-resources.html \
  --fetch-css --max-stylesheets 4 --resource-stage-ms 100 \
  --reader-profile none --skip-js --no-render --limit-mb "$limit_mb" \
  > "$phase_log" 2>&1
grep -Eq 'external-css .*batches=1 .*deadline-exceeded=yes' \
  "$phase_log"
grep -q 'benchmark status=ok' "$phase_log"
grep -q 'memory teardown       current=  0.00 MiB' "$phase_log"
printf 'Async network scheduler passed: limit-mb=%s log=%s\n' \
  "$limit_mb" "$log"
