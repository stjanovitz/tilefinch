#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-dev}
output=${2:-/tmp/tilefinch-failure-recovery}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
mkdir -p "$output"

port_file="$output/port"
python3 - "$root/fixtures" "$port_file" <<'PY' &
import http.server, pathlib, socketserver, sys
root = pathlib.Path(sys.argv[1])
port_file = pathlib.Path(sys.argv[2])
class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def end_headers(self):
        self.send_header("Connection", "close")
        super().end_headers()
with socketserver.TCPServer(("127.0.0.1", 0), Handler) as server:
    port_file.write_text(str(server.server_address[1]))
    import os
    os.chdir(root)
    server.serve_forever()
PY
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT INT TERM

i=0
while [ ! -s "$port_file" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { echo "fixture server did not start" >&2; exit 1; }
    sleep 0.02
done
port=$(cat "$port_file")
"$build/psp-browser-failure-recovery" \
    "http://127.0.0.1:$port/demo.html" | tee "$output/run.log"
grep -q 'status=PASS' "$output/run.log"
