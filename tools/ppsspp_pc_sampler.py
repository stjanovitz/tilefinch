#!/usr/bin/env python3
"""Sample the emulated PSP program counter through PPSSPP's remote debugger.

PPSSPP has no cache model, but it runs the exact Allegrex binary, so where
the program counter sits over a journey is the device's instruction
footprint: the input a code-layout (I-cache) study needs. PPSSPP exposes a
WebSocket debugger (enable it with TILEFINCH_PPSSPP_DEBUGGER_PORT on
scripts/run-ppsspp-input-script.sh). Each sample briefly steps the CPU,
reads pc/ra/sp (and optionally the stack walk), and resumes it. Emulated
time does not advance while the CPU is stepped, so the app's own timings
stay valid; the sampling clock is the host's, so samples weight host time,
which tracks emulated time while guest code runs in the JIT.

Each sample also records `cpu.status` ticks (emulated cycles) and the
latest milestone line seen in the validation log (`--log`), so a report can
split the samples into windows (load, send, ...).

Usage:
  tools/ppsspp_pc_sampler.py --port 45123 --out samples.tsv \
      [--log PATH/tilefinch-validation.txt] [--hz 200] [--backtrace] \
      [--until-log 'tilefinch-validation: outcome=clean-exit'] [--max-seconds N]

Output TSV columns: host_ms ticks pc ra sp thread marker [bt_pc,...]
Symbolize and aggregate with tools/pc_profile_report.py.
Standard library only (Python 3.9+).
"""
import argparse
import base64
import json
import os
import re
import socket
import struct
import sys
import time


class WebSocket:
    def __init__(self, host, port, path="/debugger"):
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: debugger.ppsspp.org\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake closed")
            head += chunk
        header, _, rest = head.partition(b"\r\n\r\n")
        if b" 101 " not in header.split(b"\r\n", 1)[0]:
            raise ConnectionError("handshake refused: %r" % header[:200])
        self.buf = rest

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
        self.sock.sendall(hdr + mask + masked)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv(self):
        message = b""
        while True:
            self._need(2)
            b0, b1 = self.buf[0], self.buf[1]
            n = b1 & 0x7F
            off = 2
            if n == 126:
                self._need(4)
                n = struct.unpack("!H", self.buf[2:4])[0]
                off = 4
            elif n == 127:
                self._need(10)
                n = struct.unpack("!Q", self.buf[2:10])[0]
                off = 10
            if b1 & 0x80:
                off += 4
            self._need(off + n)
            payload = self.buf[off:off + n]
            self.buf = self.buf[off + n:]
            opcode = b0 & 0x0F
            if opcode == 0x8:
                raise ConnectionError("closed by peer")
            if opcode == 0x9:  # ping -> pong
                continue
            message += payload
            if b0 & 0x80:
                return json.loads(message.decode("utf-8", "replace"))

    def call(self, event, **kw):
        """Send a request and return its reply. cpu.stepping and cpu.resume
        answer with the untagged broadcast event of the same name."""
        ticket = "t%d" % time.monotonic_ns()
        req = {"event": event, "ticket": ticket}
        req.update(kw)
        self.send(req)
        broadcast = event in ("cpu.stepping", "cpu.resume")
        while True:
            msg = self.recv()
            if msg.get("ticket") == ticket:
                return msg
            if broadcast and msg.get("event") == event:
                return msg


MARKER_RE = re.compile(
    r"(tilefinch-input-script: (?:step|mark)[^\n]*|"
    r"tilefinch-load-timeline:[^\n]*|tilefinch-js-bench: kernel=\S+)")


class LogTail:
    def __init__(self, path):
        self.path = path
        self.pos = 0
        self.marker = "-"
        self.text_tail = ""

    def poll(self):
        if not self.path:
            return
        try:
            if os.path.getsize(self.path) < self.pos:
                self.pos = 0  # the app rotated its log after a reset
                self.text_tail = ""
            with open(self.path, "rb") as fh:
                fh.seek(self.pos)
                data = fh.read()
        except OSError:
            return
        if not data:
            return
        self.pos += len(data)
        text = data.decode("utf-8", "replace")
        self.text_tail = (self.text_tail + text)[-4096:]
        for m in MARKER_RE.finditer(text):
            self.marker = m.group(1).strip().replace("\t", " ")[:120]


def reconnect(args, ws):
    try:
        ws.sock.close()
    except OSError:
        pass
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            fresh = WebSocket(args.host, args.port)
            st = fresh.call("cpu.status")
            if st.get("stepping"):
                fresh.call("cpu.resume")
            return fresh
        except (ConnectionError, OSError, ValueError):
            time.sleep(0.5)
    return None


def gpr(regs_msg, name):
    for cat in regs_msg.get("categories", []):
        names = cat.get("registerNames", [])
        if name in names:
            return cat["uintValues"][names.index(name)]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--log")
    ap.add_argument("--hz", type=float, default=100.0)
    ap.add_argument("--min-cycles", type=int, default=100000,
                    help="back off when fewer emulated cycles ran between "
                    "samples")
    ap.add_argument("--backoff", type=float, default=0.03)
    ap.add_argument("--backtrace", action="store_true")
    ap.add_argument("--until-log")
    ap.add_argument("--max-seconds", type=float, default=900.0)
    ap.add_argument("--connect-timeout", type=float, default=300.0)
    ap.add_argument("--reset-dir",
                    help="app directory: reboot the game once connected")
    ap.add_argument("--reset-files", default=(
        "profile.cfg,profile.cfg.bak,local-storage.bin,local-storage.bin.bak,"
        "http-cache.bin,http-cache.bin.bak,site-storage,modcache"))
    args = ap.parse_args()

    deadline = time.monotonic() + args.connect_timeout
    ws = None
    while ws is None:
        try:
            ws = WebSocket(args.host, args.port)
        except OSError:
            if time.monotonic() > deadline:
                sys.exit("could not connect to PPSSPP debugger")
            time.sleep(0.5)
    stepped = False
    if args.reset_dir:
        # PPSSPP registers its debugger with report.ppsspp.org before it
        # serves the first request; without outbound network that blocks
        # the server for about a minute of the run. Stop the CPU, remove the
        # state the partial run wrote (as the device replay script does),
        # and reboot the game with the CPU held, so sampling covers boot.
        ws.call("cpu.stepping")
        for name in args.reset_files.split(","):
            path = os.path.join(args.reset_dir, name)
            if os.path.isdir(path):
                import shutil
                shutil.rmtree(path, ignore_errors=True)
            elif os.path.exists(path):
                os.remove(path)
        staged = os.path.join(args.reset_dir, "boot.cfg.after-reset")
        if os.path.exists(staged):
            os.replace(staged, os.path.join(args.reset_dir, "boot.cfg"))
        # The reset drops every debugger connection; reconnect, and the
        # rebooted game waits at its first instruction for cpu.resume.
        try:
            ws.call("game.reset", **{"break": True})
        except (ConnectionError, OSError, ValueError):
            pass
        ws = None
        wait_until = time.monotonic() + 120
        while time.monotonic() < wait_until:
            try:
                if ws is None:
                    time.sleep(0.2)
                    ws = WebSocket(args.host, args.port)
                st = ws.call("cpu.status")
                if st.get("stepping"):
                    break
                time.sleep(0.05)
            except (ConnectionError, OSError, ValueError):
                ws = None
        if ws is None:
            sys.exit("lost the PPSSPP debugger after game.reset")
        stepped = True
        print("reset: game rebooted with the CPU held", flush=True)
    tail = LogTail(args.log)
    if args.reset_dir and args.log:
        # Skip the partial run's log; the rebooted app rotates it.
        try:
            tail.pos = os.path.getsize(args.log)
        except OSError:
            pass
    period = 1.0 / args.hz
    start = time.monotonic()
    count = 0
    errors = 0
    last_ticks = 0
    with open(args.out, "w") as out:
        out.write("host_ms\tticks\tpc\tra\tsp\tthread\tmarker\tbacktrace\n")
        next_t = start
        while time.monotonic() - start < args.max_seconds:
            now = time.monotonic()
            if now < next_t:
                time.sleep(next_t - now)
            next_t += period
            tail.poll()
            if args.until_log and args.until_log in tail.text_tail:
                break
            try:
                if stepped:
                    st = ws.call("cpu.status")
                    stepped = False
                else:
                    st = ws.call("cpu.stepping")
                if st.get("event") == "error":
                    errors += 1
                    continue
                if not st.get("ticks"):
                    # The stepping broadcast may omit the emulated clock;
                    # the report weights samples by emulated cycles.
                    st = ws.call("cpu.status")
                regs = ws.call("cpu.getAllRegs")
                bt = ""
                thread = "-"
                if args.backtrace:
                    b = ws.call("hle.backtrace")
                    frames = b.get("frames", [])
                    bt = ",".join("%x" % f.get("pc", 0) for f in frames[:24])
                ws.call("cpu.resume")
            except (ConnectionError, OSError, ValueError):
                # A stalled or dropped connection: reconnect and make sure
                # the CPU runs; give up only once the emulator is gone.
                errors += 1
                ws = reconnect(args, ws)
                if ws is None:
                    break
                continue
            ticks = st.get("ticks", 0)
            if ticks and ticks - last_ticks < args.min_cycles:
                # The emulator barely ran since the last sample (an idle
                # frame wait): back off so sampling cannot starve it.
                next_t += args.backoff
            last_ticks = ticks or last_ticks
            pc = gpr(regs, "pc")
            ra = gpr(regs, "ra")
            sp = gpr(regs, "sp")
            if pc is None:
                errors += 1
                continue
            out.write("%d\t%d\t%x\t%x\t%x\t%s\t%s\t%s\n" % (
                int((time.monotonic() - start) * 1000), st.get("ticks", 0),
                pc, ra or 0, sp or 0, thread, tail.marker, bt))
            count += 1
            if count % 1000 == 0:
                out.flush()
    print("samples=%d errors=%d seconds=%.1f" % (
        count, errors, time.monotonic() - start))


if __name__ == "__main__":
    main()
