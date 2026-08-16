#!/bin/sh
set -eu

build_dir=${1:-build-bellard-clean-impersonate}
output_dir=${2:-acceptance-results}
manifest=${3:-benchmarks/acceptance-sites.tsv}
lab="$build_dir/psp-browser-lab"
interactive="$build_dir/psp-browser-interactive-lab"
ledger_checker=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/check-memory-ledger.sh
summary="$output_dir/summary.tsv"
capture_root=${ACCEPTANCE_HTTP_CAPTURE_ROOT:-}
replay_root=${ACCEPTANCE_HTTP_REPLAY_ROOT:-}
site_filter=${ACCEPTANCE_SITE_FILTER:-}
psp_profile=${ACCEPTANCE_PSP_PROFILE:-}
failures=0

case "$psp_profile" in
  ''|strict|realistic) ;;
  *) printf 'ACCEPTANCE_PSP_PROFILE must be strict or realistic\n' >&2; exit 2 ;;
esac

if [ -n "$capture_root" ] && [ -n "$replay_root" ]; then
  printf 'Choose capture or replay, not both\n' >&2
  exit 2
fi
if [ -n "$capture_root" ]; then mkdir -p "$capture_root"; fi
if [ -n "$replay_root" ] && [ ! -d "$replay_root" ]; then
  printf 'Replay root is missing: %s\n' "$replay_root" >&2
  exit 2
fi

if [ ! -x "$lab" ] || [ ! -x "$interactive" ]; then
  printf 'Build the lab first; expected %s and %s\n' "$lab" "$interactive" >&2
  exit 2
fi
if [ ! -f "$manifest" ]; then
  printf 'Acceptance manifest is missing: %s\n' "$manifest" >&2
  exit 2
fi

mkdir -p "$output_dir"
printf 'site\tstatus\theight\tframes\tblank\trevisit\tpeak_bytes\n' > "$summary"

field() {
  key=$1
  file=$2
  sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p" "$file" | tail -n 1
}

run_site() {
  name=$1
  url=$2
  limit=$3
  profile=$4
  minimum_height=$5
  minimum_body_text=$6
  required_title=$7
  required_text=$8
  site_dir="$output_dir/$name"
  log="$output_dir/$name.log"
  if [ "$profile" != none ]; then
    printf 'Acceptance profiles must be none for %s; got %s\n' \
      "$name" "$profile" >&2
    exit 2
  fi
  mkdir -p "$site_dir"
  set -- "$lab" --url "$url" --reader-profile none --fetch-css \
    --max-stylesheets 8 --max-css-kb 1536 --max-css-file-kb 512 \
    --fetch-images --max-images 64 --max-image-kb 768 \
    --max-image-file-kb 256 --max-decoded-image-kb 512 \
    --resource-stage-ms 3000 \
    --limit-mb "$limit" --js-limit-mb 8 --tile-count 8 \
    --viewport-width 480 --viewport-height 272 --scroll-all \
    --output-dir "$site_dir" --dump-links "$site_dir/links.tsv" \
    --dump-layout "$site_dir/layout.tsv"
  if [ -n "$psp_profile" ]; then
    set -- "$@" --psp-profile "$psp_profile"
  fi
  if [ -n "$capture_root" ]; then
    set -- "$@" --capture-http "$capture_root/$name" \
      --deterministic-replay-seed 42
  elif [ -n "$replay_root" ]; then
    set -- "$@" --replay-http "$replay_root/$name" \
      --deterministic-replay-seed 42
  fi
  if ! "$@" > "$log" 2>&1; then
    failure_status=failed
    if grep -Eq 'returned error: (408|425|429|500|502|503|504)' "$log"; then
      failure_status=unavailable-http
    fi
    peak=$(field peak_bytes "$log")
    printf '%s\t%s\t-\t-\t-\t-\t%s\n' \
      "$name" "$failure_status" "${peak:--}" >> "$summary"
    printf 'Acceptance fetch/run failure for %s; see %s\n' \
      "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi

  if ! grep -q '^reader profile=none$' "$log"; then
    printf 'Reader profile unexpectedly active for %s; see %s\n' \
      "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi
  if ! "$ledger_checker" "$log" stable-page teardown; then
    printf 'Allocator ledger acceptance failure for %s; see %s\n' \
      "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi

  status=$(field status "$log")
  height=$(field page_height "$log")
  frames=$(field scroll_frames "$log")
  blank=$(field blank_frames "$log")
  revisit=$(field revisit "$log")
  peak=$(field peak_bytes "$log")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$status" "$height" "$frames" "$blank" "$revisit" "$peak" \
    >> "$summary"
  if [ "$status" != ok ] || [ "$blank" != 0 ] || [ "$revisit" != match ] \
    || [ "$height" -lt "$minimum_height" ]; then
    printf 'Acceptance failure for %s; see %s\n' "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi
  if [ -n "$minimum_body_text" ]; then
    body_text=$(field body-text "$log")
    if [ -z "$body_text" ] || [ "$body_text" -lt "$minimum_body_text" ]; then
      printf 'Acceptance content-size failure for %s; see %s\n' \
        "$name" "$log" >&2
      failures=$((failures + 1))
      return 0
    fi
  fi
  if [ -n "$required_title" ] \
    && ! grep '^document title=' "$log" | grep -F -q -- "$required_title"; then
    printf 'Acceptance title-content failure for %s; see %s\n' \
      "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi
  if [ -n "$required_text" ] \
    && ! grep -F -q -- "$required_text" "$site_dir/layout.tsv"; then
    printf 'Acceptance visible-content failure for %s; see %s\n' \
      "$name" "$log" >&2
    failures=$((failures + 1))
    return 0
  fi
}

tab=$(printf '\t')
while IFS="$tab" read -r name url limit profile minimum_height purpose \
  minimum_body_text required_title required_text; do
  case "$name" in
    ''|'#'*|name) continue ;;
  esac
  if [ -n "$site_filter" ] && [ "$name" != "$site_filter" ]; then
    continue
  fi
  if [ -z "$url" ] || [ -z "$limit" ] || [ -z "$profile" ] \
    || [ -z "$minimum_height" ]; then
    printf 'Invalid acceptance manifest row for %s\n' "$name" >&2
    exit 2
  fi
  case "$minimum_height" in
    *[!0-9]*|'')
      printf 'Invalid minimum height for %s: %s\n' \
        "$name" "$minimum_height" >&2
      exit 2
      ;;
  esac
  case "$minimum_body_text" in
    *[!0-9]*)
      printf 'Invalid minimum body text for %s: %s\n' \
        "$name" "$minimum_body_text" >&2
      exit 2
      ;;
  esac
  run_site "$name" "$url" "$limit" "$profile" "$minimum_height" \
    "$minimum_body_text" "$required_title" "$required_text"
done < "$manifest"

if [ "${ACCEPTANCE_CHATGPT_LIVE:-0}" = 1 ]; then
  chatgpt_log="$output_dir/chatgpt.log"
  TILEFINCH_TRACE_INTERACTION=1 "$interactive" --url https://chatgpt.com/ \
    --fetch-scripts --ticks 1000 --tick-ms 16 \
    --focus-id mobile-composer-prompt --type 'What can you do?' \
    --click-selector '[data-composer-submit]' --interaction-ticks 1000 \
    --post-click-from-top \
    --post-click-selector '.wm-app-scrollToBottomButton' \
    --limit-mb 64 \
    --script-timeout-ms 10000 --script-heap-mb 8 --script-total-mb 24 \
    --script-file-kb 512 --script-count 48 \
    --output "$output_dir/chatgpt-bottom.ppm" > "$chatgpt_log" 2>&1
  grep -q 'message-stream-complete' "$chatgpt_log"
  grep -q 'interactive teardown=.*status=PASS' "$chatgpt_log"
  "$ledger_checker" "$chatgpt_log" \
    interactive-stable interactive-teardown
  printf 'chatgpt\tok\t%s\t1\t0\tmatch\t%s\n' \
    "$(field height "$chatgpt_log")" "$(field peak "$chatgpt_log")" \
    >> "$summary"
else
  printf 'chatgpt\tskipped-live\t-\t-\t-\t-\t-\n' >> "$summary"
fi

if [ "$failures" -ne 0 ]; then
  printf 'Acceptance suite completed with %s failure(s). Summary: %s\n' \
    "$failures" "$summary" >&2
  exit 1
fi
printf 'Acceptance suite passed. Summary: %s\n' "$summary"
