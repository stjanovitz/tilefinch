#!/bin/sh
set -eu

build_dir=${1:-build-dev}
output_dir=${2:-platform-session-results}
lab="$build_dir/psp-browser-interactive-lab"

if [ ! -x "$lab" ]; then
  printf 'Build the interactive lab first: %s\n' "$lab" >&2
  exit 2
fi
if [ -e "$output_dir" ]; then
  printf 'Output already exists: %s\n' "$output_dir" >&2
  exit 2
fi
mkdir -p "$output_dir"

python3 fixtures/server.py > "$output_dir/server.log" 2>&1 &
server_pid=$!
cleanup() {
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

ready=0
i=0
while [ "$i" -lt 50 ]; do
  if curl --silent --fail http://127.0.0.1:8765/interactive.html \
      > /dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
  i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
  printf 'Fixture server did not become ready\n' >&2
  exit 1
fi

started=$(date +%s)
"$lab" --url http://127.0.0.1:8765/interactive.html \
  --commands fixtures/platform-session.commands --platform-sim \
  --no-loop-capture --limit-mb 24 --script-heap-mb 4 \
  --loop-output-dir "$output_dir/unused-frames" \
  > "$output_dir/session.log" 2>&1
elapsed=$(( $(date +%s) - started ))

grep -q '^loop status=PASS ' "$output_dir/session.log"
grep -q '^platform-sim assets=[1-9][0-9]* input-polls=[1-9][0-9]* frames=[1-9][0-9]* .* status=PASS$' \
  "$output_dir/session.log"
grep -Eq '^navigation loads=([2-9][0-9]|[1-9][0-9][0-9]+) ' \
  "$output_dir/session.log"
grep -q '^interactive teardown=0 active=0 .* status=PASS$' \
  "$output_dir/session.log"
if [ "$elapsed" -ge 300 ]; then
  printf 'Accelerated session exceeded five minutes: %ss\n' "$elapsed" >&2
  exit 1
fi
printf 'Accelerated 30-minute platform session passed in %ss: %s\n' \
  "$elapsed" "$output_dir/session.log"
