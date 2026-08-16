#!/bin/sh
set -eu

build_dir=${1:-build-bellard-clean-impersonate}
output_dir=${2:-session-stress-results}
cycles=${3:-100}
limit_mb=${4:-24}
maximum_growth=${SESSION_STRESS_MAX_GROWTH:-2097152}
interactive="$build_dir/psp-browser-interactive-lab"
commands="$output_dir/commands.txt"
log="$output_dir/session.log"

case "$cycles" in
  ''|*[!0-9]*) printf 'cycles must be a positive integer\n' >&2; exit 2 ;;
esac
case "$limit_mb" in
  16|24) ;;
  *) printf 'limit must be 16 or 24 MiB\n' >&2; exit 2 ;;
esac
if [ "$cycles" -lt 1 ] || [ "$cycles" -gt 1000 ]; then
  printf 'cycles must be between 1 and 1000\n' >&2
  exit 2
fi
if [ ! -x "$interactive" ]; then
  printf 'Build the interactive lab first: %s\n' "$interactive" >&2
  exit 2
fi

mkdir -p "$output_dir"
: > "$commands"
printf 'tick 2 16\nmark-steady\n' >> "$commands"
i=0
while [ "$i" -lt "$cycles" ]; do
  printf 'focus-id next\ncross\ncircle\ntop\nbottom\ntick 2 16\n' \
    >> "$commands"
  i=$((i + 1))
done
printf 'status\nquit\n' >> "$commands"

python3 fixtures/server.py > "$output_dir/server.log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT HUP INT TERM

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
  printf 'fixture server did not become ready\n' >&2
  exit 1
fi

"$interactive" --url http://127.0.0.1:8765/interactive.html \
  --fetch-scripts --commands "$commands" --no-loop-capture \
  --loop-output-dir "$output_dir/frames" \
  --output "$output_dir/final.ppm" --limit-mb "$limit_mb" > "$log" 2>&1

grep -q '^loop status=PASS' "$log"
grep -q 'interactive teardown=0 .* status=PASS' "$log"
growth=$(sed -n 's/^loop steady=yes .* growth=\([0-9][0-9]*\)$/\1/p' \
  "$log" | tail -n 1)
if [ -z "$growth" ] || [ "$growth" -gt "$maximum_growth" ]; then
  printf 'session plateau failed: growth=%s limit=%s; see %s\n' \
    "${growth:-missing}" "$maximum_growth" "$log" >&2
  exit 1
fi

printf 'Session stress passed: cycles=%s growth=%s ceiling=%s profile-limit-mb=%s log=%s\n' \
  "$cycles" "$growth" "$maximum_growth" "$limit_mb" "$log"
