#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
replay=${2:-"$root/../tilefinch-corpora/acceptance-psp-envelope-20260716"}
output=${3:-/tmp/psp-envelope-expanded-interactive}
manifest="$root/benchmarks/psp-envelope-expanded.tsv"
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-interactive-lab"

[ -x "$lab" ] && [ -d "$replay" ] && [ -f "$manifest" ] || {
  printf 'expanded interactive envelope inputs are missing\n' >&2
  exit 2
}
mkdir -p "$output"
summary="$output/summary.tsv"
printf 'site\tstatus\theight\tpeak_bytes\tmax_slice_us\tmax_slice_phase\tnonpreemptible_compiles\tmax_compile_us\tmax_compile_bytes\tnonpreemptible_callbacks\tmax_callback_us\n' \
  > "$summary"

tab=$(printf '\t')
while IFS="$tab" read -r name url limit profile minimum_height purpose; do
  case "$name" in ''|'#'*|name) continue ;; esac
  log="$output/$name.log"
  if [ -f "$replay/$name/fixture.html" ]; then
    set -- "$lab" --fixture "$replay/$name/fixture.html"
  else
    set -- "$lab" --url "$url" --replay-http "$replay/$name" \
      --deterministic-replay-seed 42
  fi
  "$@" --psp-profile realistic --ticks 4 \
    --output "$output/$name.ppm" > "$log" 2>&1
  grep -q '^interactive status=ok ' "$log"
  grep -q '^interactive teardown=0 .* status=PASS$' "$log"
  height=$(sed -n \
    's/^interactive status=ok .* height=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  peak=$(sed -n \
    's/^interactive teardown=0 .* peak=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  maximum=$(sed -n \
    's/^responsiveness max-slice-us=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  phase=$(sed -n \
    's/^responsiveness max-slice-us=[0-9][0-9]* phase=\([^ ]*\).*/\1/p' \
    "$log" | tail -n 1)
  compile_count=$(sed -n \
    's/^javascript-responsiveness .* nonpreemptible-compiles=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  compile_us=$(sed -n \
    's/^javascript-responsiveness .* max-compile-us=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  compile_bytes=$(sed -n \
    's/^javascript-responsiveness .* max-compile-bytes=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  callback_count=$(sed -n \
    's/^javascript-responsiveness .* nonpreemptible-callbacks=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  callback_us=$(sed -n \
    's/^javascript-responsiveness .* max-callback-us=\([0-9][0-9]*\).*/\1/p' \
    "$log" | tail -n 1)
  [ "$height" -ge "$minimum_height" ]
  [ "$peak" -lt $((24 * 1024 * 1024)) ]
  if [ "$maximum" -gt 16000 ]; then
    if [ "$phase" = network ]; then
      : # Synchronous host transport/replay call; separately bounded on PSP.
    elif [ "$phase" = script ]; then
      [ "$compile_count" -gt 0 ]
      [ "$compile_us" -gt 16000 ]
    else
      [ "$phase" = runtime ]
      [ "$callback_count" -gt 0 ]
      [ "$callback_us" -gt 16000 ]
    fi
  fi
  printf '%s\tok\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$height" "$peak" "$maximum" "$phase" "$compile_count" \
    "$compile_us" "$compile_bytes" "$callback_count" "$callback_us" \
    >> "$summary"
done < "$manifest"

cat "$summary"
