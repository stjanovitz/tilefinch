#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
output_dir=${2:-http-replay-results}
interactive="$build_dir/psp-browser-interactive-lab"
trace="$output_dir/trace"
capture_log="$output_dir/capture.log"
replay_log="$output_dir/replay.log"

if [ ! -x "$interactive" ]; then
  printf 'Build the interactive lab first: %s\n' "$interactive" >&2
  exit 2
fi
if [ -e "$trace" ]; then
  printf 'Trace output already exists: %s\n' "$trace" >&2
  exit 2
fi

mkdir -p "$output_dir"
python3 fixtures/server.py > "$output_dir/server.log" 2>&1 &
server_pid=$!
cleanup() {
  if [ -n "${server_pid:-}" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

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
  --capture-http "$trace" --deterministic-replay-seed 42 \
  --ticks 64 --tick-ms 16 --limit-mb 24 \
  --output "$output_dir/capture.ppm" > "$capture_log" 2>&1

kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=

"$interactive" --url http://127.0.0.1:8765/async-network.html \
  --replay-http "$trace" --deterministic-replay-seed 42 \
  --ticks 64 --tick-ms 16 --limit-mb 24 \
  --output "$output_dir/replay.ppm" > "$replay_log" 2>&1

expected='javascript summary="async:yes:visible,yes,yes,yes:visible:true:1.2.3.4:loadstart.progress.load.loadend,0-255-128-65:application/octet-stream,fetch-0-255-128-65:blob-0-255-128-65,body-TypeError:json-SyntaxError,stream-xy-TypeError,http://127.0.0.1:8765:origin-check,postbytes-0-255-128-65:AbortError,TimeoutError"'
for log in "$capture_log" "$replay_log"; do
  grep -q "$expected" "$log"
  grep -q 'javascript-network requests=10 failures=2 status=200' "$log"
  grep -q 'javascript-network-async queued=10 completed=10 rejected=0 cancelled=1 timed-out=1 peak-inflight=4' "$log"
  grep -q 'javascript-xhr sends=3 last-error=""' "$log"
  grep -q 'javascript-xhr-response responses=2 status=200 type="arraybuffer" text-units=0 bytes=4 states="1.2.3.4"' "$log"
  grep -q 'interactive teardown=0 active=0 largest=0 .* status=PASS' "$log"
done
grep -q '^success=0$' "$trace/0009.meta"
grep -q '^external-cancel=1$' "$trace/0009.meta"
grep -q '^success=0$' "$trace/0010.meta"
grep -q '^external-cancel=0$' "$trace/0010.meta"
grep -q '^psp-http-trace-clock=1$' "$trace/trace.meta"
grep -q '^origin-ms=[0-9][0-9]*$' "$trace/trace.meta"
for meta in "$trace"/[0-9][0-9][0-9][0-9].meta; do
  grep -q '^psp-http-trace=9$' "$meta"
  grep -q '^cookie-values=redacted$' "$meta"
  grep -Eq '^request-body-hash=[0-9a-f]{16}$' "$meta"
  grep -Eq '^response-body-hash=[0-9a-f]{16}$' "$meta"
  grep -q '^request-extra-header-shape=' "$meta"
  grep -Eq '^request-send-client-hints=[01]$' "$meta"
  grep -Eq '^request-send-low-client-hints=[01]$' "$meta"
done
printf 'Deterministic HTTP capture/replay passed: %s\n' "$output_dir"
