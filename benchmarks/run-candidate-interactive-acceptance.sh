#!/bin/sh
set -eu

usage() {
  printf 'usage: %s BUILD_DIR REPLAY_ROOT [OUTPUT_DIR [MANIFEST]]\n' "$0" >&2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  usage
  exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$1
replay_root=$2
output=${3:-"${TMPDIR:-/tmp}/tilefinch-candidate-interactive"}
manifest=${4:-"$root/benchmarks/acceptance-sites-interactive-candidates.tsv"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-interactive-lab"
ledger_checker="$root/benchmarks/check-memory-ledger.sh"
site_filter=${CANDIDATE_SITE_FILTER:-}
summary="$output/summary.tsv"
failures=0
external_blocks=0
selected=0

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  else
    printf 'unavailable\n'
  fi
}

if [ ! -x "$lab" ]; then
  printf 'Build the interactive lab first; expected %s\n' "$lab" >&2
  exit 2
fi
if [ ! -d "$replay_root" ]; then
  printf 'Candidate replay root is missing: %s\n' "$replay_root" >&2
  exit 2
fi
if [ ! -f "$manifest" ]; then
  printf 'Candidate manifest is missing: %s\n' "$manifest" >&2
  exit 2
fi
if [ ! -x "$ledger_checker" ]; then
  printf 'Memory ledger checker is missing: %s\n' "$ledger_checker" >&2
  exit 2
fi

git_revision=unknown
git_state=unknown
if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git_revision=$(git -C "$root" rev-parse --verify HEAD 2>/dev/null || printf unknown)
  tracked=clean
  if ! git -C "$root" diff --quiet --ignore-submodules -- \
    || ! git -C "$root" diff --cached --quiet --ignore-submodules --; then
    tracked=modified
  fi
  untracked=clean
  if [ -n "$(git -C "$root" status --porcelain --untracked-files=normal 2>/dev/null \
      | sed -n '/^?? /{p;q;}')" ]; then
    untracked=present
  fi
  git_state="$tracked+$untracked"
fi
binary_sha256=$(sha256_file "$lab")
binary_bytes=$(wc -c < "$lab" | tr -d ' ')

mkdir -p "$output"
printf '%s\n' \
  'site	status	http	height	body_text	peak_bytes	allocator_failures	detail	scripts_loaded	script_failures	dom_mutations	network_completions	limit_bytes	budget_headroom_bytes	external_reserved_bytes	first_dom_us	first_paint_us	completion_us	engine_work_us	max_slice_us	max_slice_phase	max_compile_us	max_compile_bytes	max_callback_us	max_frame_us	git_revision	git_state	binary_sha256	binary_bytes' \
  > "$summary"
{
  printf 'git_revision=%s\n' "$git_revision"
  printf 'git_state=%s\n' "$git_state"
  printf 'binary_sha256=%s\n' "$binary_sha256"
  printf 'binary_bytes=%s\n' "$binary_bytes"
  printf 'binary_path=%s\n' "$lab"
  printf 'manifest=%s\n' "$manifest"
  printf 'replay_root=%s\n' "$replay_root"
} > "$output/run.meta"

numeric() {
  case $1 in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac
}

field() {
  pattern=$1
  file=$2
  sed -n "s/$pattern\([0-9][0-9]*\).*/\\1/p" "$file" | tail -n 1
}

word_field() {
  pattern=$1
  file=$2
  sed -n "s/$pattern\\([^ ]*\\).*/\\1/p" "$file" | tail -n 1
}

performance_field() {
  key=$1
  file=$2
  awk -v key="$key" '
    /^performance-us / {
      for (column = 2; column <= NF; column++) {
        split($column, pair, "=")
        if (pair[1] == key && pair[2] ~ /^[0-9]+$/) value = pair[2]
      }
    }
    END { if (value != "") print value }
  ' "$file"
}

reset_summary_metrics() {
  metric_scripts_loaded=-
  metric_script_failures=-
  metric_dom_mutations=-
  metric_network_completions=-
  metric_limit_bytes=-
  metric_budget_headroom=-
  metric_external_reserved=-
  metric_first_dom_us=-
  metric_first_paint_us=-
  metric_completion_us=-
  metric_engine_work_us=-
  metric_max_slice_us=-
  metric_max_slice_phase=-
  metric_max_compile_us=-
  metric_max_compile_bytes=-
  metric_max_callback_us=-
  metric_max_frame_us=-
}

emit_summary() {
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
    "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" >> "$summary"
  printf '\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$metric_scripts_loaded" "$metric_script_failures" \
    "$metric_dom_mutations" "$metric_network_completions" \
    "$metric_limit_bytes" "$metric_budget_headroom" \
    "$metric_external_reserved" "$metric_first_dom_us" \
    "$metric_first_paint_us" "$metric_completion_us" \
    "$metric_engine_work_us" "$metric_max_slice_us" \
    "$metric_max_slice_phase" "$metric_max_compile_us" \
    "$metric_max_compile_bytes" "$metric_max_callback_us" \
    "$metric_max_frame_us" "$git_revision" "$git_state" \
    "$binary_sha256" "$binary_bytes" >> "$summary"
}

populate_summary_metrics() {
  metrics_log=$1
  metrics_limit_mb=$2
  metrics_peak=$3
  reset_summary_metrics
  metric_limit_bytes=$((metrics_limit_mb * 1024 * 1024))
  if numeric "$metrics_peak"; then
    metric_budget_headroom=$((metric_limit_bytes - metrics_peak))
  fi

  value=$(field '^scripts discovered=.* loaded=' "$metrics_log")
  if numeric "$value"; then metric_scripts_loaded=$value; fi
  value=$(field '^scripts discovered=.* failed=' "$metrics_log")
  if numeric "$value"; then metric_script_failures=$value; fi
  value=$(field '^javascript-state mutations=' "$metrics_log")
  if numeric "$value"; then metric_dom_mutations=$value; fi
  async_completed=$(field '^javascript-network-async .* completed=' "$metrics_log")
  xhr_completed=$(field '^javascript-xhr-response responses=' "$metrics_log")
  if numeric "$async_completed"; then
    # The native async total includes fetch and XHR transports.  The XHR line
    # is a diagnostic subset, so adding it here would count those twice.
    metric_network_completions=$async_completed
  elif numeric "$xhr_completed"; then
    # Older logs predate the aggregate async metric; retain their XHR count as
    # a bounded compatibility fallback.
    metric_network_completions=$xhr_completed
  fi
  value=$(field '^memory-categories phase=interactive-stable .* external-reserved=' \
    "$metrics_log")
  if numeric "$value"; then metric_external_reserved=$value; fi
  value=$(field '^stream .* first-dom-us=' "$metrics_log")
  if numeric "$value"; then metric_first_dom_us=$value; fi
  value=$(field '^stream .* first-paint-us=' "$metrics_log")
  if numeric "$value"; then metric_first_paint_us=$value; fi
  value=$(field '^stream .* completion-us=' "$metrics_log")
  if numeric "$value"; then metric_completion_us=$value; fi
  value=$(field '^responsiveness max-slice-us=' "$metrics_log")
  if numeric "$value"; then metric_max_slice_us=$value; fi
  value=$(word_field '^responsiveness max-slice-us=[0-9][0-9]* phase=' \
    "$metrics_log")
  if [ -n "$value" ]; then metric_max_slice_phase=$value; fi
  value=$(field '^javascript-responsiveness .* max-compile-us=' "$metrics_log")
  if numeric "$value"; then metric_max_compile_us=$value; fi
  value=$(field '^javascript-responsiveness .* max-compile-bytes=' "$metrics_log")
  if numeric "$value"; then metric_max_compile_bytes=$value; fi
  value=$(field '^javascript-responsiveness .* max-callback-us=' "$metrics_log")
  if numeric "$value"; then metric_max_callback_us=$value; fi
  value=$(performance_field max-frame "$metrics_log")
  if numeric "$value"; then metric_max_frame_us=$value; fi

  if grep -q '^performance-us ' "$metrics_log"; then
    metric_engine_work_us=0
    for phase_key in network parse script style resource layout relayout runtime raster frame; do
      value=$(performance_field "$phase_key" "$metrics_log")
      if numeric "$value"; then
        metric_engine_work_us=$((metric_engine_work_us + value))
      fi
    done
  fi
}

corpus_contains() {
  directory=$1
  needle=$2
  for body_file in "$directory"/*.body; do
    if [ -f "$body_file" ] && grep -F -q -- "$needle" "$body_file"; then
      return 0
    fi
  done
  return 1
}

corpus_has_body() {
  directory=$1
  for body_file in "$directory"/*.body; do
    if [ -f "$body_file" ]; then
      return 0
    fi
  done
  return 1
}

runtime_body_contains() {
  log=$1
  needle=$2
  awk '
    /^javascript-state / { in_body = 1 }
    /^javascript-section-retention / { in_body = 0 }
    in_body { print }
  ' "$log" | grep -F -q -- "$needle"
}

record_failure() {
  name=$1
  http=$2
  height=$3
  body=$4
  peak=$5
  allocator_failures=$6
  detail=$7
  emit_summary "$name" FAIL "$http" "$height" "$body" "$peak" \
    "$allocator_failures" "$detail"
  printf 'Candidate acceptance failure for %s (%s); see %s/%s.log\n' \
    "$name" "$detail" "$output" "$name" >&2
  failures=$((failures + 1))
}

run_site() {
  name=$1
  url=$2
  replay_dir=$3
  limit_mb=$4
  ticks=$5
  tick_ms=$6
  max_download_kb=$7
  script_timeout_ms=$8
  script_heap_mb=$9
  shift 9
  script_total_mb=$1
  script_file_kb=$2
  script_count=$3
  minimum_height=$4
  minimum_body_text=$5
  maximum_peak_mb=$6
  maximum_allocator_failures=$7
  expected_http=$8
  required_title=$9
  shift 9
  required_runtime_text=$1
  required_corpus_text=$2
  block_policy=$3
  blocked_http=$4
  blocked_marker=$5
  shift 5
  minimum_scripts_loaded=$1
  minimum_dom_mutations=$2
  minimum_network_completions=$3
  maximum_script_failures=$4
  require_empty_js_error=$5
  forbidden_runtime_text=$6

  site_replay="$replay_root/$replay_dir"
  log="$output/$name.log"
  frame="$output/$name.ppm"
  selected=$((selected + 1))
  reset_summary_metrics
  metric_limit_bytes=$((limit_mb * 1024 * 1024))

  if [ ! -d "$site_replay" ] \
    || [ ! -f "$site_replay/trace.meta" ] \
    || ! corpus_has_body "$site_replay"; then
    record_failure "$name" - - - - - missing-replay-corpus
    return
  fi

  rm -f "$frame"
  set +e
  "$lab" --url "$url" \
    --replay-http "$site_replay" --deterministic-replay-seed 42 \
    --fetch-scripts --ticks "$ticks" --tick-ms "$tick_ms" \
    --limit-mb "$limit_mb" --max-download-kb "$max_download_kb" \
    --script-timeout-ms "$script_timeout_ms" \
    --script-heap-mb "$script_heap_mb" \
    --script-total-mb "$script_total_mb" \
    --script-file-kb "$script_file_kb" \
    --script-count "$script_count" \
    --output "$frame" > "$log" 2>&1
  code=$?
  set -e

  http=$(field '^network status=' "$log")
  height=$(field '^interactive status=ok .* height=' "$log")
  body=$(field '^document-memory .* body-text=' "$log")
  peak=$(field '^interactive teardown=.* peak=' "$log")
  allocator_failures=$(field '^interactive teardown=.* failures=' "$log")
  http=${http:--}
  height=${height:--}
  body=${body:--}
  peak=${peak:--}
  allocator_failures=${allocator_failures:--}
  populate_summary_metrics "$log" "$limit_mb" "$peak"

  if [ "$code" -ne 0 ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" engine-run
    return
  fi
  if ! grep -q '^interactive status=ok ' "$log" \
    || ! grep -q '^interactive teardown=0 active=0 largest=0 .* status=PASS$' \
      "$log"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" engine-status
    return
  fi
  if ! "$ledger_checker" "$log" interactive-stable interactive-teardown; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" memory-ledger
    return
  fi
  if [ ! -s "$frame" ] \
    || [ "$(LC_ALL=C sed -n '1p;1q' "$frame")" != P6 ] \
    || [ "$(LC_ALL=C sed -n '2p;2q' "$frame")" != '480 272' ] \
    || [ "$(LC_ALL=C sed -n '3p;3q' "$frame")" != 255 ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" viewport-frame
    return
  fi
  if ! numeric "$height" || ! numeric "$body" || ! numeric "$peak" \
    || ! numeric "$allocator_failures"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" missing-metrics
    return
  fi
  maximum_peak_bytes=$((maximum_peak_mb * 1024 * 1024))
  if [ "$peak" -gt "$maximum_peak_bytes" ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" peak-memory
    return
  fi
  if [ "$allocator_failures" -gt "$maximum_allocator_failures" ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" allocator-failures
    return
  fi

  marker_seen=0
  if [ "$blocked_marker" != - ]; then
    if grep -F -q -- "$blocked_marker" "$log"; then
      marker_seen=1
    fi
  fi
  if [ "$block_policy" = external ] \
    && [ "$http" = "$blocked_http" ] && [ "$marker_seen" -eq 1 ]; then
    emit_summary "$name" EXTERNAL_BLOCKED "$http" "$height" "$body" \
      "$peak" "$allocator_failures" "edge-$blocked_http:$blocked_marker"
    printf 'Candidate %s remains externally blocked (HTTP %s, marker %s); not counted as a pass.\n' \
      "$name" "$http" "$blocked_marker" >&2
    external_blocks=$((external_blocks + 1))
    return
  fi
  if [ "$marker_seen" -eq 1 ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" unexpected-block-marker
    return
  fi
  if [ "$http" != "$expected_http" ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" http-status
    return
  fi
  if [ "$height" -lt "$minimum_height" ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" minimum-height
    return
  fi
  if [ "$body" -lt "$minimum_body_text" ]; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" minimum-body-text
    return
  fi
  if ! grep '^interactive status=ok title=' "$log" \
    | grep -F -q -- "$required_title"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" required-title
    return
  fi
  if ! runtime_body_contains "$log" "$required_runtime_text"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" required-runtime-content
    return
  fi
  if ! corpus_contains "$site_replay" "$required_corpus_text"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" required-corpus-content
    return
  fi

  if [ "$minimum_scripts_loaded" -gt 0 ] \
    && { ! numeric "$metric_scripts_loaded" \
      || [ "$metric_scripts_loaded" -lt "$minimum_scripts_loaded" ]; }; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" minimum-scripts-loaded
    return
  fi
  if [ "$minimum_dom_mutations" -gt 0 ] \
    && { ! numeric "$metric_dom_mutations" \
      || [ "$metric_dom_mutations" -lt "$minimum_dom_mutations" ]; }; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" minimum-dom-mutations
    return
  fi
  if [ "$minimum_network_completions" -gt 0 ] \
    && { ! numeric "$metric_network_completions" \
      || [ "$metric_network_completions" -lt "$minimum_network_completions" ]; }; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" minimum-network-completions
    return
  fi
  if [ "$maximum_script_failures" != - ] \
    && { ! numeric "$metric_script_failures" \
      || [ "$metric_script_failures" -gt "$maximum_script_failures" ]; }; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" maximum-script-failures
    return
  fi
  if [ "$require_empty_js_error" = yes ] \
    && ! grep -q '^javascript-error="" source=' "$log"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" javascript-error
    return
  fi
  if [ "$forbidden_runtime_text" != - ] \
    && runtime_body_contains "$log" "$forbidden_runtime_text"; then
    record_failure "$name" "$http" "$height" "$body" "$peak" \
      "$allocator_failures" forbidden-runtime-content
    return
  fi

  emit_summary "$name" PASS "$http" "$height" "$body" "$peak" \
    "$allocator_failures" ok
}

tab=$(printf '\t')
while IFS="$tab" read -r name url replay_dir limit_mb ticks tick_ms \
  max_download_kb script_timeout_ms script_heap_mb script_total_mb \
  script_file_kb script_count minimum_height minimum_body_text \
  maximum_peak_mb maximum_allocator_failures expected_http required_title \
  required_runtime_text required_corpus_text block_policy blocked_http \
  blocked_marker purpose minimum_scripts_loaded minimum_dom_mutations \
  minimum_network_completions maximum_script_failures \
  require_empty_js_error forbidden_runtime_text; do
  case "$name" in ''|'#'*|name) continue ;; esac
  if [ -n "$site_filter" ] && [ "$name" != "$site_filter" ]; then
    continue
  fi
  minimum_scripts_loaded=${minimum_scripts_loaded:-0}
  minimum_dom_mutations=${minimum_dom_mutations:-0}
  minimum_network_completions=${minimum_network_completions:-0}
  maximum_script_failures=${maximum_script_failures:--}
  require_empty_js_error=${require_empty_js_error:-no}
  forbidden_runtime_text=${forbidden_runtime_text:--}
  for value in "$limit_mb" "$ticks" "$tick_ms" "$max_download_kb" \
    "$script_timeout_ms" "$script_heap_mb" "$script_total_mb" \
    "$script_file_kb" "$script_count" "$minimum_height" \
    "$minimum_body_text" "$maximum_peak_mb" \
    "$maximum_allocator_failures" "$expected_http" \
    "$minimum_scripts_loaded" "$minimum_dom_mutations" \
    "$minimum_network_completions"; do
    if ! numeric "$value"; then
      printf 'Invalid numeric field for candidate %s: %s\n' \
        "$name" "$value" >&2
      exit 2
    fi
  done
  if [ "$maximum_script_failures" != - ] \
    && ! numeric "$maximum_script_failures"; then
    printf 'Invalid maximum script failures for candidate %s: %s\n' \
      "$name" "$maximum_script_failures" >&2
    exit 2
  fi
  case "$require_empty_js_error" in
    yes|no) ;;
    *)
      printf 'Invalid require_empty_js_error for candidate %s: %s\n' \
        "$name" "$require_empty_js_error" >&2
      exit 2
      ;;
  esac
  case "$block_policy" in
    required)
      if [ "$blocked_http" != - ] || [ "$blocked_marker" != - ]; then
        printf 'Required candidate %s must not define an external block\n' \
          "$name" >&2
        exit 2
      fi
      ;;
    external)
      if ! numeric "$blocked_http" || [ "$blocked_marker" = - ]; then
        printf 'External candidate %s needs a status and marker\n' \
          "$name" >&2
        exit 2
      fi
      ;;
    *)
      printf 'Invalid block policy for candidate %s: %s\n' \
        "$name" "$block_policy" >&2
      exit 2
      ;;
  esac
  run_site "$name" "$url" "$replay_dir" "$limit_mb" "$ticks" \
    "$tick_ms" "$max_download_kb" "$script_timeout_ms" \
    "$script_heap_mb" "$script_total_mb" "$script_file_kb" \
    "$script_count" "$minimum_height" "$minimum_body_text" \
    "$maximum_peak_mb" "$maximum_allocator_failures" "$expected_http" \
    "$required_title" "$required_runtime_text" "$required_corpus_text" \
    "$block_policy" "$blocked_http" "$blocked_marker" \
    "$minimum_scripts_loaded" "$minimum_dom_mutations" \
    "$minimum_network_completions" "$maximum_script_failures" \
    "$require_empty_js_error" "$forbidden_runtime_text"
done < "$manifest"

if [ "$selected" -eq 0 ]; then
  printf 'No candidate rows selected (filter=%s)\n' "$site_filter" >&2
  exit 2
fi

cat "$summary"
if [ "$failures" -ne 0 ]; then
  printf 'Candidate replay has %s hard failure(s); qualification failed.\n' \
    "$failures" >&2
  exit 1
fi
if [ "$external_blocks" -ne 0 ]; then
  printf 'All runnable candidates passed, but %s candidate(s) remain externally blocked; qualification is incomplete.\n' \
    "$external_blocks" >&2
  exit 3
fi
printf 'All selected interactive candidates passed deterministic replay.\n'
