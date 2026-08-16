#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${TILEFINCH_UPDATE_E2E_BUILD_DIR:-"$root/build-preset-psp-update-e2e"}
timeout_seconds=180

usage() {
    printf '%s\n' \
        "usage: scripts/run-ppsspp-update-e2e.sh [--timeout SECONDS]" \
        "" \
        "Builds an ephemeral signed release, serves it from a private local" \
        "TLS origin, and boots an isolated PPSSPP launcher install through" \
        "check, download, install, trial, health confirmation, and exit."
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --timeout)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            timeout_seconds=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

case "$timeout_seconds" in
    ''|*[!0-9]*)
        printf 'timeout must be an integer number of seconds\n' >&2
        exit 2
        ;;
esac
[ "$timeout_seconds" -ge 30 ] && [ "$timeout_seconds" -le 300 ] || {
    printf 'timeout must be between 30 and 300 seconds\n' >&2
    exit 2
}
[ -n "${PSPDEV:-}" ] || {
    printf '%s\n' 'export PSPDEV before running the update qualification' >&2
    exit 2
}
for tool in cmake openssl python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'required tool is unavailable: %s\n' "$tool" >&2
        exit 2
    }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-update-e2e.XXXXXX")
server_pid=
cleanup() {
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$work/origin"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
    -out "$work/root-private.pem" >/dev/null 2>&1
openssl pkey -in "$work/root-private.pem" -pubout \
    -out "$work/root-public.pem" >/dev/null 2>&1
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
    -out "$work/release-private.pem" >/dev/null 2>&1
openssl pkey -in "$work/release-private.pem" -pubout \
    -out "$work/release-public.pem" >/dev/null 2>&1
python3 "$root/tools/tilefinch_update_tool.py" root \
    --version 1 --expires 2000000000 \
    --root-threshold 1 --release-threshold 1 \
    --root-key "$work/root-public.pem" \
    --release-key "$work/release-public.pem" \
    --output "$work/root-v1.tfur"

# The endpoint certificate is unrelated to update signing.  A throwaway CA
# is appended to the validation build's ordinary Mozilla bundle, proving the
# real certificate verifier while keeping the exercise completely private.
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
    -keyout "$work/tls-ca-key.pem" -out "$work/tls-ca.pem" \
    -subj '/CN=Tilefinch local update test CA' >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -sha256 \
    -keyout "$work/tls-server-key.pem" -out "$work/tls-server.csr" \
    -subj '/CN=127.0.0.1' >/dev/null 2>&1
printf '%s\n' \
    'subjectAltName=IP:127.0.0.1' \
    'extendedKeyUsage=serverAuth' \
    'keyUsage=digitalSignature,keyEncipherment' >"$work/tls-server.ext"
openssl x509 -req -sha256 -days 1 \
    -in "$work/tls-server.csr" \
    -CA "$work/tls-ca.pem" -CAkey "$work/tls-ca-key.pem" \
    -CAcreateserial -extfile "$work/tls-server.ext" \
    -out "$work/tls-server.pem" >/dev/null 2>&1
cp "$root/certs/roots.pem" "$work/roots.pem"
printf '\n' >>"$work/roots.pem"
cat "$work/tls-ca.pem" >>"$work/roots.pem"

cmake --preset psp -B "$build_dir" \
    -DTILEFINCH_PSP_VALIDATION_LOG=ON \
    -DTILEFINCH_RELEASE_SEQUENCE=1 \
    -DTILEFINCH_UPDATE_ROOT_V1="$work/root-v1.tfur" \
    -DPSP_BROWSER_PSP_CA_BUNDLE="$work/roots.pem"
cmake --build "$build_dir" --target tilefinch-psp-install-tree -j8

python3 "$root/tools/tilefinch_update_tool.py" pack \
    --directory "$build_dir/tilefinch-install/Tilefinch/slot-a" \
    --output "$work/origin/tilefinch-update-v1.tfup"
printf '%s' 'Private local end-to-end update qualification.' \
    >"$work/notes.txt"
python3 "$root/tools/tilefinch_update_tool.py" manifest \
    --package "$work/origin/tilefinch-update-v1.tfup" \
    --root-version 1 --sequence 2 --expires 2000000000 \
    --version 0.1.1-e2e --tag v0.1.1-e2e \
    --asset tilefinch-update-v1.tfup --notes "$work/notes.txt" \
    --output "$work/manifest.bin"
python3 "$root/tools/tilefinch_update_tool.py" envelope \
    --manifest "$work/manifest.bin" \
    --release-key "$work/release-private.pem" \
    --output "$work/origin/tilefinch-update-v1.tfum"

python3 "$root/tools/local_update_server.py" \
    --directory "$work/origin" \
    --certificate "$work/tls-server.pem" \
    --private-key "$work/tls-server-key.pem" \
    --port-file "$work/port" --log "$work/server.jsonl" \
    >"$work/server-console.log" 2>&1 &
server_pid=$!
attempt=0
while [ ! -s "$work/port" ]; do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "$work/server-console.log" >&2
        printf '%s\n' 'local update origin exited before becoming ready' >&2
        exit 1
    fi
    [ "$attempt" -lt 100 ] || {
        printf '%s\n' 'local update origin did not become ready' >&2
        exit 1
    }
    sleep 0.05
    attempt=$((attempt + 1))
done
port=$(sed -n '1p' "$work/port")

PSP_BROWSER_PSP_BUILD_DIR="$build_dir" \
    "$root/scripts/run-ppsspp-network.sh" \
        --update-e2e \
        "https://127.0.0.1:$port/tilefinch-update-v1.tfum" \
        --timeout "$timeout_seconds"

if ! grep -q '"kind":"metadata".*"outcome":"ok"' \
        "$work/server.jsonl"; then
    printf '%s\n' 'the emulator never fetched signed metadata' >&2
    exit 1
fi
if ! grep -q '"kind":"package".*"outcome":"ok"' \
        "$work/server.jsonl"; then
    printf '%s\n' 'the emulator never fetched the update package' >&2
    exit 1
fi
cp "$work/server.jsonl" \
    "$build_dir/ppsspp-network-latest/update-server.jsonl"
printf 'PASS: private local signed-update qualification; artifacts: %s\n' \
    "$build_dir/ppsspp-network-latest"
