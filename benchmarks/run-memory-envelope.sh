#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
corpus_root=${2:-}
output_dir=${3:-memory-envelope-results}
lab="$build_dir/psp-browser-lab"
url=https://www.nytimes.com/

if [ ! -x "$lab" ]; then
  printf 'Build the static lab first; expected %s\n' "$lab" >&2
  exit 2
fi
if [ -z "$corpus_root" ] || [ ! -d "$corpus_root/nytimes" ]; then
  printf 'Usage: %s BUILD_DIR CAPTURE_ROOT [OUTPUT_DIR]\n' "$0" >&2
  exit 2
fi

mkdir -p "$output_dir/realistic" "$output_dir/strict-adaptive"

"$lab" --url "$url" --reader-profile none --fetch-css \
  --max-stylesheets 8 --max-css-kb 1536 --max-css-file-kb 512 \
  --fetch-images --max-images 64 --max-image-kb 768 \
  --max-image-file-kb 256 --max-decoded-image-kb 512 \
  --resource-stage-ms 3000 --psp-profile realistic --js-limit-mb 8 \
  --viewport-width 480 --viewport-height 272 --scroll-all \
  --output-dir "$output_dir/realistic" \
  --dump-links "$output_dir/realistic/links.tsv" \
  --dump-layout "$output_dir/realistic/layout.tsv" \
  --replay-http "$corpus_root/nytimes" --deterministic-replay-seed 42 \
  > "$output_dir/realistic.log" 2>&1

"$lab" --url "$url" --reader-profile none --fetch-css \
  --max-stylesheets 8 --max-css-kb 1536 --max-css-file-kb 512 \
  --fetch-images --max-images 64 --max-image-kb 768 \
  --max-image-file-kb 256 --max-decoded-image-kb 512 \
  --resource-stage-ms 3000 --psp-profile strict \
  --viewport-width 480 --viewport-height 272 --scroll-all \
  --output-dir "$output_dir/strict-adaptive" \
  --dump-links "$output_dir/strict-adaptive/links.tsv" \
  --dump-layout "$output_dir/strict-adaptive/layout.tsv" \
  --replay-http "$corpus_root/nytimes" --deterministic-replay-seed 42 \
  > "$output_dir/strict-adaptive.log" 2>&1

field() {
  key=$1
  file=$2
  sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p" "$file" | tail -n 1
}

realistic_peak=$(field peak_bytes "$output_dir/realistic.log")
strict_peak=$(field peak_bytes "$output_dir/strict-adaptive.log")
realistic_height=$(field page_height "$output_dir/realistic.log")
strict_height=$(field page_height "$output_dir/strict-adaptive.log")

grep -q '^benchmark status=ok ' "$output_dir/realistic.log"
grep -q '^scroll mode=full .* blank=0 revisit=match$' \
  "$output_dir/realistic.log"
grep -q '^memory teardown       current=  0.00 MiB .* failures=0$' \
  "$output_dir/realistic.log"
grep -q '^benchmark status=ok .* javascript=adaptive-skipped ' \
  "$output_dir/strict-adaptive.log"
grep -q '^javascript skipped by adaptive memory policy$' \
  "$output_dir/strict-adaptive.log"
grep -q '^external-css .* loaded=2 .*skipped-limit=' \
  "$output_dir/strict-adaptive.log"
grep -q '^adaptive-degradation enabled=yes active=yes javascript=skipped stylesheets=reduced images=reduced tiles=reduced$' \
  "$output_dir/strict-adaptive.log"
grep -q '^scroll mode=full .* blank=0 revisit=match$' \
  "$output_dir/strict-adaptive.log"
grep -q '^memory teardown       current=  0.00 MiB .* failures=0$' \
  "$output_dir/strict-adaptive.log"

if [ "$realistic_peak" -le $((16 * 1024 * 1024)) ] \
   || [ "$realistic_peak" -ge $((24 * 1024 * 1024)) ] \
   || [ "$strict_peak" -ge $((16 * 1024 * 1024)) ] \
   || [ "$realistic_height" -lt 1000 ] || [ "$strict_height" -lt 1000 ]; then
  printf 'Memory envelope invariant failed; see %s\n' "$output_dir" >&2
  exit 1
fi

printf 'mode\tlimit_mb\tpeak_bytes\tpage_height\n' > "$output_dir/summary.tsv"
printf 'realistic\t24\t%s\t%s\n' "$realistic_peak" "$realistic_height" \
  >> "$output_dir/summary.tsv"
printf 'strict-adaptive\t16\t%s\t%s\n' "$strict_peak" "$strict_height" \
  >> "$output_dir/summary.tsv"
printf 'Memory envelope qualification passed: %s\n' "$output_dir/summary.tsv"
