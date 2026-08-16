#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
output=${2:-/tmp/psp-envelope-local}
work_root=${3:-"$root/../../work"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-lab"
interactive="$build/psp-browser-interactive-lab"
manifest="$root/benchmarks/psp-envelope-local.tsv"

[ -x "$lab" ] && [ -x "$interactive" ] && [ -f "$manifest" ] \
  && [ -d "$work_root" ] || {
  printf 'local envelope inputs are missing\n' >&2
  exit 2
}
mkdir -p "$output"
summary="$output/summary.tsv"
interactive_summary="$output/interactive-summary.tsv"
printf 'site\tprofile\tstatus\theight\tcommands\tframes\tblank\trevisit\tpeak_bytes\tmax_layout_slice_us\tpressure_decisions\n' \
  > "$summary"
printf 'site\tstatus\theight\tpeak_bytes\tmax_slice_us\tmax_slice_phase\tnonpreemptible_compiles\tmax_compile_us\tmax_compile_bytes\n' \
  > "$interactive_summary"

field() {
  key=$1
  file=$2
  sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p" "$file" | tail -n 1
}

tab=$(printf '\t')
while IFS="$tab" read -r name fixture minimum_height; do
  case "$name" in ''|'#'*|name) continue ;; esac
  input="$work_root/$fixture"
  [ -f "$input" ] || {
    printf 'local snapshot missing: %s\n' "$input" >&2
    exit 2
  }
  for profile in realistic strict; do
    run="$output/$name-$profile"
    log="$run.log"
    mkdir -p "$run"
    "$lab" --fixture "$input" --psp-profile "$profile" \
      --reader-profile none --viewport-width 480 --viewport-height 272 \
      --scroll-all --output-dir "$run" > "$log" 2>&1
    grep -q '^reader profile=none$' "$log"
    grep -q '^benchmark status=ok ' "$log"
    grep -q '^scroll mode=full .* blank=0 revisit=match$' "$log"
    grep -q '^memory teardown       current=  0.00 MiB ' "$log"
    status=$(field status "$log")
    height=$(field page_height "$log")
    commands=$(field commands "$log")
    frames=$(field scroll_frames "$log")
    blank=$(field blank_frames "$log")
    revisit=$(field revisit "$log")
    peak=$(field peak_bytes "$log")
    max_slice=$(sed -n \
      's/^layout-responsiveness .* max-slice-us=\([0-9][0-9]*\).*/\1/p' \
      "$log" | tail -n 1)
    decisions=$(sed -n \
      's/^pressure-summary decisions=\([0-9][0-9]*\).*/\1/p' \
      "$log" | tail -n 1)
    [ "$height" -ge "$minimum_height" ]
    [ "$max_slice" -le 16000 ]
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$profile" "$status" "$height" "$commands" "$frames" \
      "$blank" "$revisit" "$peak" "$max_slice" "$decisions" \
      >> "$summary"
  done

  interactive_log="$output/$name-interactive.log"
  "$interactive" --fixture "$input" --psp-profile realistic --ticks 4 \
    --output "$output/$name-interactive.ppm" \
    > "$interactive_log" 2>&1
  grep -q '^interactive status=ok ' "$interactive_log"
  grep -q '^interactive teardown=0 .* status=PASS$' "$interactive_log"
  interactive_height=$(sed -n \
    's/^interactive status=ok .* height=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  interactive_peak=$(sed -n \
    's/^interactive teardown=0 .* peak=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  interactive_max=$(sed -n \
    's/^responsiveness max-slice-us=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  interactive_phase=$(sed -n \
    's/^responsiveness max-slice-us=[0-9][0-9]* phase=\([^ ]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  compile_count=$(sed -n \
    's/^javascript-responsiveness .* nonpreemptible-compiles=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  compile_us=$(sed -n \
    's/^javascript-responsiveness .* max-compile-us=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  compile_bytes=$(sed -n \
    's/^javascript-responsiveness .* max-compile-bytes=\([0-9][0-9]*\).*/\1/p' \
    "$interactive_log" | tail -n 1)
  [ "$interactive_peak" -lt $((24 * 1024 * 1024)) ]
  if [ "$interactive_max" -gt 16000 ]; then
    [ "$interactive_phase" = script ]
    [ "$compile_count" -gt 0 ]
    [ "$compile_us" -gt 16000 ]
  fi
  printf '%s\tok\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" \
    "$interactive_height" "$interactive_peak" "$interactive_max" \
    "$interactive_phase" "$compile_count" "$compile_us" "$compile_bytes" \
    >> "$interactive_summary"
done < "$manifest"

cat "$summary"
cat "$interactive_summary"
