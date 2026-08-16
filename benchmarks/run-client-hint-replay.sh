#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
output_dir=${2:-client-hint-replay-results}
interactive="$build_dir/psp-browser-interactive-lab"
trace="$output_dir/trace"
frame_trace="$output_dir/frame-trace"

if [ ! -x "$interactive" ]; then
  printf 'Build the interactive lab first: %s\n' "$interactive" >&2
  exit 2
fi
if [ -e "$trace" ] || [ -e "$frame_trace" ]; then
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
  if curl --silent --fail http://127.0.0.1:8765/critical-hints.html \
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

"$interactive" --url http://127.0.0.1:8765/critical-hints.html \
  --capture-http "$trace" --deterministic-replay-seed 42 \
  --ticks 4 --tick-ms 16 --limit-mb 24 \
  --output "$output_dir/capture.ppm" > "$output_dir/capture.log" 2>&1

"$interactive" --url http://localhost:8765/critical-frame-parent.html \
  --fetch-scripts \
  --capture-http "$frame_trace" --deterministic-replay-seed 42 \
  --ticks 8 --tick-ms 16 --limit-mb 24 \
  --output "$output_dir/frame-capture.ppm" \
  > "$output_dir/frame-capture.log" 2>&1

kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=

"$interactive" --url http://127.0.0.1:8765/critical-hints.html \
  --replay-http "$trace" --deterministic-replay-seed 42 \
  --ticks 4 --tick-ms 16 --limit-mb 24 --trace-page \
  --output "$output_dir/replay.ppm" > "$output_dir/replay.log" 2>&1

"$interactive" --url http://localhost:8765/critical-frame-parent.html \
  --fetch-scripts \
  --replay-http "$frame_trace" --deterministic-replay-seed 42 \
  --ticks 8 --tick-ms 16 --limit-mb 24 \
  --output "$output_dir/frame-replay.ppm" \
  > "$output_dir/frame-replay.log" 2>&1

for log in "$output_dir/capture.log" "$output_dir/replay.log"; do
  grep -q 'javascript summary="critical-hints:post:cookie:navigation"' "$log"
  grep -q 'client-hint-retries=1' "$log"
  if grep -q 'critical-hints:pre' "$log"; then
    printf 'pre-hint document was executed: %s\n' "$log" >&2
    exit 1
  fi
done

for log in "$output_dir/frame-capture.log" \
           "$output_dir/frame-replay.log"; do
  grep -q 'javascript summary="critical-frame:post:cookie:language:same-origin-referrer:frame-navigation:clean-script-metadata"' "$log"
  grep -q 'client-hint-retries=1' "$log"
  grep -q 'frames discovered=1 loaded=1 failed=0' "$log"
  if grep -q 'critical-frame:pre' "$log"; then
    printf 'pre-hint frame was executed: %s\n' "$log" >&2
    exit 1
  fi
done

grep -q 'navigation loads=1 destroys=0 history=1 pruned=0 reloads=0' \
  "$output_dir/replay.log"
grep -q 'page-capability-trace=' "$output_dir/replay.log"

for meta in "$trace"/000*.meta "$frame_trace"/000*.meta; do
  grep -q '^psp-http-trace=9$' "$meta"
  grep -q '^cookie-values=redacted$' "$meta"
done

grep -q '^request-send-client-hints=0$' "$trace/0000.meta"
grep -q '^request-client-hint-tokens=$' "$trace/0000.meta"
grep -q '^request-client-hint-origin=$' "$trace/0000.meta"
grep -q '^request-send-low-client-hints=1$' "$trace/0000.meta"
grep -q '^request-sec-fetch-user=1$' "$trace/0000.meta"
grep -q '^request-upgrade-insecure=1$' "$trace/0000.meta"
grep -q '^critical-ch=Sec-CH-UA-Arch$' "$trace/0000.meta"
grep -q '^request-send-client-hints=1$' "$trace/0001.meta"
grep -q '^request-client-hint-tokens=Sec-CH-UA-Arch$' "$trace/0001.meta"
grep -q '^request-client-hint-origin=http://127.0.0.1:8765$' "$trace/0001.meta"
grep -q '^request-send-low-client-hints=1$' "$trace/0001.meta"
grep -q '^request-sec-fetch-user=1$' "$trace/0001.meta"
grep -q '^request-upgrade-insecure=1$' "$trace/0001.meta"
grep -Eq '^request-cookie-bytes=[1-9][0-9]*$' "$trace/0001.meta"
grep -q '^critical-ch=$' "$trace/0001.meta"

grep -q '^request-send-client-hints=0$' "$frame_trace/0002.meta"
grep -q '^request-client-hint-tokens=$' "$frame_trace/0002.meta"
grep -q '^request-client-hint-origin=$' "$frame_trace/0002.meta"
grep -q '^request-send-low-client-hints=1$' "$frame_trace/0002.meta"
grep -q '^request-sec-fetch-user=0$' "$frame_trace/0002.meta"
grep -q '^request-upgrade-insecure=1$' "$frame_trace/0002.meta"
grep -q '^critical-ch=Sec-CH-UA-Arch$' "$frame_trace/0002.meta"
grep -q '^set-cookie-0=frame_hint_retry=xxxx; Path=/$' \
  "$frame_trace/0002.meta"
grep -q '^set-cookie-url-0=http://localhost:8765/critical-frame.html$' \
  "$frame_trace/0002.meta"
grep -q '^request-send-client-hints=1$' "$frame_trace/0003.meta"
grep -q '^request-client-hint-tokens=Sec-CH-UA-Arch$' \
  "$frame_trace/0003.meta"
grep -q '^request-client-hint-origin=http://localhost:8765$' \
  "$frame_trace/0003.meta"
grep -q '^request-send-low-client-hints=1$' "$frame_trace/0003.meta"
grep -q '^request-sec-fetch-user=0$' "$frame_trace/0003.meta"
grep -q '^request-upgrade-insecure=1$' "$frame_trace/0003.meta"
grep -Eq '^request-cookie-bytes=[1-9][0-9]*$' \
  "$frame_trace/0003.meta"
grep -q '^critical-ch=$' "$frame_trace/0003.meta"
for meta in "$frame_trace/0002.meta" "$frame_trace/0003.meta"; do
  grep -q '^request-credential-origin=http://localhost:8765/critical-frame-parent.html$' \
    "$meta"
  grep -q '^request-initiator-url=http://localhost:8765/critical-frame-parent.html$' \
    "$meta"
  grep -q '^request-referrer-source=http://localhost:8765/critical-frame-parent.html$' \
    "$meta"
  grep -q '^request-referrer-policy=same-origin$' "$meta"
done

printf 'Critical-CH capture/replay passed: %s\n' "$output_dir"
