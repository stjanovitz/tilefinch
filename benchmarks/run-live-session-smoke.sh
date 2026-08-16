#!/bin/sh
set -eu

build_dir=${1:-build-bellard-clean-impersonate}
output_dir=${2:-live-session-results}
rounds=${3:-3}
interactive="$build_dir/psp-browser-interactive-lab"
commands="$output_dir/commands.txt"
log="$output_dir/session.log"

case "$rounds" in
  ''|*[!0-9]*) printf 'rounds must be a positive integer\n' >&2; exit 2 ;;
esac
if [ "$rounds" -lt 1 ] || [ "$rounds" -gt 20 ]; then
  printf 'rounds must be between 1 and 20\n' >&2
  exit 2
fi
if [ ! -x "$interactive" ]; then
  printf 'Build the interactive lab first: %s\n' "$interactive" >&2
  exit 2
fi

mkdir -p "$output_dir"
: > "$commands"
i=0
while [ "$i" -lt "$rounds" ]; do
  printf '%s\n' \
    'go https://news.ycombinator.com/' bottom top \
    'go https://www.gov.uk/' bottom top \
    'go https://en.wikipedia.org/wiki/PlayStation_Portable' bottom top \
    'go https://old.reddit.com/r/PSP/' bottom top \
    'go https://www.nytimes.com/' bottom top >> "$commands"
  if [ "$i" -eq 0 ]; then printf 'mark-steady\n' >> "$commands"; fi
  i=$((i + 1))
done
printf 'status\nquit\n' >> "$commands"

"$interactive" --url https://example.com/ --commands "$commands" \
  --no-loop-capture --loop-output-dir "$output_dir/frames" \
  --output "$output_dir/final.ppm" --limit-mb 32 > "$log" 2>&1

grep -q '^loop status=PASS' "$log"
grep -q 'interactive teardown=0 .* status=PASS' "$log"
printf 'Live session smoke passed: rounds=%s log=%s\n' "$rounds" "$log"

