#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
nytimes_corpus=${1:-}
overflow_corpus=${2:-}
output_dir=${3:-"${TMPDIR:-/tmp}/tilefinch-port-readiness"}
release_build=${PSP_BROWSER_RELEASE_BUILD_DIR:-"$root/build-release"}

if [ -z "$nytimes_corpus" ] || [ ! -d "$nytimes_corpus/nytimes" ] \
    || [ -z "$overflow_corpus" ] || [ ! -d "$overflow_corpus/wikipedia" ]; then
    printf 'Usage: %s NYTIMES_CAPTURE_ROOT OVERFLOW_CAPTURE_ROOT [OUTPUT_DIR]\n' \
        "$0" >&2
    exit 2
fi
if [ -e "$output_dir" ]; then
    printf 'Readiness output already exists: %s\n' "$output_dir" >&2
    exit 2
fi
mkdir -p "$output_dir"

printf 'gate\tstatus\n' > "$output_dir/summary.tsv"
run_gate() {
    name=$1
    shift
    printf 'readiness gate=%s status=running\n' "$name"
    "$@"
    printf '%s\tpass\n' "$name" >> "$output_dir/summary.tsv"
}

run_gate release-tests "$root/scripts/dev.sh" test-release
run_gate portability "$root/scripts/audit-portability.sh" "$release_build" \
    "$output_dir/portability"
run_gate interaction "$root/benchmarks/run-interactive-acceptance.sh" \
    "$release_build" "$output_dir/interaction"
run_gate async-network "$root/benchmarks/run-async-network.sh" \
    "$release_build" "$output_dir/async-network" 24
run_gate http-replay "$root/benchmarks/run-http-replay.sh" \
    "$release_build" "$output_dir/http-replay"
run_gate client-hints "$root/benchmarks/run-client-hint-replay.sh" \
    "$release_build" "$output_dir/client-hints"
run_gate profiles sh "$root/benchmarks/run-psp-profile-qualification.sh" \
    "$release_build" "$output_dir/profiles" 100
run_gate memory-envelope "$root/benchmarks/run-memory-envelope.sh" \
    "$release_build" "$nytimes_corpus" "$output_dir/memory-envelope"

printf 'readiness gate=overflow-replay status=running\n'
ACCEPTANCE_HTTP_REPLAY_ROOT="$overflow_corpus" \
    "$root/benchmarks/run-acceptance.sh" "$release_build" \
    "$output_dir/overflow-replay" \
    "$root/benchmarks/acceptance-sites-portability.tsv"
printf 'overflow-replay\tpass\n' >> "$output_dir/summary.tsv"

run_gate sanitizers "$root/scripts/dev.sh" sanitize
printf 'Pre-cross-build port readiness passed: %s\n' "$output_dir/summary.tsv"
