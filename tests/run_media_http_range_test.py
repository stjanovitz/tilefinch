#!/usr/bin/env python3
"""Drive the bounded media range source against a googlevideo-shaped server.

src/media_http.c is the only consumer of FetchScheduler that issues a request,
polls it across frames and installs the response itself, and none of that was
covered end to end: the unit tests substitute a synchronous transport, which
skips the scheduler entirely. A rewrite of that path therefore shipped a defect
in which the window's bytes arrived, were copied into the cache, and were then
described as zero bytes long -- the read failed with nothing to report, and only
a device cycle found it.

This starts a loopback HTTP server that answers the same two shapes googlevideo
does and runs the real reader against it: real libcurl, real scheduler pump,
real completion detection, real admission checks.

  query200  `?range=A-B` answers 200 with exactly the requested slice, which is
            what the CDN does for the query form the media source uses.
  partial206  a Range: request answered 206 with Content-Range.
  short     a 200 whose body is one byte short of the request, which must be
            retried once and then reported rather than admitted.
  slow      the body dribbles out in small writes with pauses, so completion is
            reached across several pumps rather than inside the first one.
  fragment  a real indexed fragmented MP4 -- ftyp, a moov with mvex/trex, a
            sidx, and six moof+mdat segments -- so the demuxer's lazy fragment
            window advance runs over the real scheduler instead of a
            substituted synchronous transport. Its fragments are sized so
            reading them sequentially crosses both fragment boundaries and
            transport-window boundaries.
  fragment-slow  the same bytes dribbled out like `slow`, so a fragment
            boundary lands on a window that has not arrived and the window
            load itself has to answer WOULD_BLOCK and be retried.
  stall     headers with a Content-Length, and then nothing. The connection
            stays open and the body never arrives, which is the shape a device
            open sat in for over ten minutes: every layer looked healthy from
            the inside, the transfer simply never progressed. It is what the
            open's transaction budget and the cancel law are measured against.
  cadence   an optional real captured video, served with a byte-positioned
            burst/gap schedule. Unlike a live CDN, each run sees exactly the
            same slow regions. The C side consumes its real MP4 samples at
            their authored timestamps and reports contiguous visible holds.
            Optional profiles add repeatable CDN pathologies: bursty delivery,
            per-request setup latency, one truncated connection, or a transfer
            that trickles until the range source replaces its connection.
  stream-no-length  a valid close-delimited 200. It may use the streaming
            storage path, but no byte is exposed before terminal admission.
  stream-short  the first window is valid and each streamed successor is one
            byte short, pinning bounded rejection without a stalled stream.
  expire403  admits the initial window and returns persistent 403s afterwards,
            matching a signed media URL which expires during playback.
  bad206    returns a mismatched Content-Range and must never be admitted.
  ignore-range  ignores the range query and returns the full object.
  reset-always  closes every successor response after an admitted prefix.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from subprocess import run
from threading import Lock, Thread
import socket
import struct
import sys
import time
from urllib.parse import urlparse, parse_qs

TOTAL = 1024 * 1024
BODY = bytes((index * 31 + 7) & 0xFF for index in range(TOTAL))

# The fragmented fixture. Box layout is the one tests/test_media_mp4.c builds
# for its own sidx cases; what differs here is scale, and one deliberate
# alignment.
#
# Every segment after the first begins exactly on a 64 KiB boundary -- the
# transport window size the C side opens with. The last sample of a fragment
# therefore ends exactly where a window ends, and the next fragment's moof is
# the first byte of a window the source has not fetched. Served slowly, that is
# a fragment boundary landing on a window still in flight: the window load
# answers WOULD_BLOCK and has to be asked for again. Without the alignment the
# moof always shares a window with the sample before it and that path is never
# reached, however large the fixture is.
WINDOW = 64 * 1024
FRAGMENT_COUNT = 6
FRAGMENT_SAMPLES = 2
SAMPLE_DURATION = 500
FRAGMENT_DURATION = SAMPLE_DURATION * FRAGMENT_SAMPLES


def _u32(value):
    return struct.pack(">I", value)


def _box(kind, payload):
    return _u32(len(payload) + 8) + kind + payload


def _moov():
    trex = _box(b"trex", _u32(0) + _u32(1) + _u32(1)
                + _u32(SAMPLE_DURATION) + _u32(0) + _u32(0))
    tkhd = _box(b"tkhd", _u32(0) * 3 + _u32(1) + _u32(0))
    mdhd = _box(b"mdhd", _u32(0) * 3 + _u32(1000)
                + _u32(FRAGMENT_COUNT * FRAGMENT_DURATION) + _u32(0))
    hdlr = _box(b"hdlr", _u32(0) + _u32(0) + b"vide")
    avc1 = bytearray(78)
    avc1[32:36] = bytes((0x01, 0x40, 0x00, 0xF0))
    avcc = _box(b"avcC", bytes((1, 0x42, 0, 0x1E, 0xFF, 0xE1, 0)))
    stsd = _box(b"stsd", _u32(0) + _u32(1)
                + _box(b"avc1", bytes(avc1) + avcc))
    empty = b"".join(
        _box(kind, _u32(0) + _u32(0)) for kind in (b"stts", b"stsc", b"stco"))
    stsz = _box(b"stsz", _u32(0) * 3)
    stbl = _box(b"stbl", stsd + empty + stsz)
    mdia = _box(b"mdia", mdhd + hdlr + _box(b"minf", stbl))
    return _box(b"moov", _box(b"mvex", trex) + _box(b"trak", tkhd + mdia))


def _segment(decode_time, sizes):
    """One moof+mdat pair whose trun data offset points into its own mdat."""
    mfhd = _box(b"mfhd", _u32(0) + _u32(1))
    tfhd = _box(b"tfhd", _u32(0x0002000A) + _u32(1) + _u32(1)
                + _u32(SAMPLE_DURATION))
    tfdt = _box(b"tfdt", _u32(0) + _u32(decode_time))
    # Flags 0x701: data offset, per-sample duration, size and flags. Sample 0
    # of each fragment is a sync sample and the rest are not, which is what a
    # real fragment looks like and what the demuxer's seek tables index.
    runs = b"".join(
        _u32(SAMPLE_DURATION) + _u32(sizes[at])
        + _u32(0 if at == 0 else 0x00010000)
        for at in range(len(sizes)))
    # The offset is relative to the moof, so it is only known once the moof's
    # own length is: build it with a placeholder, then patch that word.
    trun = _box(b"trun", _u32(0x00000701) + _u32(len(sizes))
                + _u32(0) + runs)
    moof = _box(b"moof", mfhd + _box(b"traf", tfhd + tfdt + trun))
    payload = bytes((index * 31 + 7) & 0xFF for index in range(sum(sizes)))
    mdat = _box(b"mdat", payload)
    data_offset = len(moof) + 8
    at = moof.index(b"trun") + 4 + 4 + 4
    moof = moof[:at] + _u32(data_offset) + moof[at + 4:]
    return moof + mdat


def _sample_sizes(segment_bytes):
    """Split a segment's byte budget across its samples, moof and mdat aside."""
    payload = segment_bytes - len(_segment(0, (1,) * FRAGMENT_SAMPLES)) + (
        FRAGMENT_SAMPLES)
    each = payload // FRAGMENT_SAMPLES
    sizes = [each] * FRAGMENT_SAMPLES
    sizes[-1] += payload - each * FRAGMENT_SAMPLES
    return tuple(sizes)


def _fragmented():
    header = len(_box(b"ftyp", b"isom" + _u32(0))) + len(_moov()) + len(
        _box(b"sidx", _u32(0) * 6 + _u32(0) * 3 * FRAGMENT_COUNT))
    # The first segment absorbs the header's odd length so that the second one
    # starts at 2*WINDOW; every later segment is exactly one window long and so
    # starts on a boundary too.
    lengths = [2 * WINDOW - header] + [WINDOW] * (FRAGMENT_COUNT - 1)
    segments = [
        _segment(index * FRAGMENT_DURATION, _sample_sizes(length))
        for index, length in enumerate(lengths)]
    assert [len(segment) for segment in segments] == lengths
    sidx = _box(b"sidx", _u32(0) + _u32(1) + _u32(1000) + _u32(0) + _u32(0)
                + _u32(FRAGMENT_COUNT)
                + b"".join(_u32(len(segment)) + _u32(FRAGMENT_DURATION)
                           + _u32(0x90000000) for segment in segments))
    body = (_box(b"ftyp", b"isom" + _u32(0)) + _moov() + sidx
            + b"".join(segments))
    at = header
    for index, length in enumerate(lengths):
        assert index == 0 or at % WINDOW == 0, (
            f"segment {index} starts at {at}, not on a window boundary")
        at += length
    assert at == len(body)
    return body


FRAGMENTED = _fragmented()
CADENCE_BODY = None
CADENCE_PROFILE = "device-gaps"
CADENCE_ATTEMPTS = {}
CADENCE_ATTEMPTS_LOCK = Lock()
CADENCE_SERIAL_LOCK = Lock()


class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def _write_cadence(self, mode, first, payload, attempt):
        if (CADENCE_PROFILE == "drop-once" and first >= 256 * 1024
                and attempt == 1):
            # Headers promise the full bounded window, then the socket
            # disappears after a prefix has already been admitted. This
            # exercises retry after partial successor consumption.
            prefix = min(len(payload), 48 * 1024)
            self.wfile.write(payload[:prefix])
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if (CADENCE_PROFILE == "chaos-recovery"
                and mode == "cadence-video"
                and first >= 512 * 1024 and attempt == 1):
            # After the sustained stall/reconnect, lose one later response
            # after publishing a prefix. This is a distinct failure shape:
            # ordinary bounded retry, not the no-progress replacement path.
            prefix = min(len(payload), 32 * 1024)
            self.wfile.write(payload[:prefix])
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        step = 16 * 1024
        for at in range(0, len(payload), step):
            absolute = first + at
            self.wfile.write(payload[at:at + step])
            self.wfile.flush()
            if CADENCE_PROFILE in ("device-gaps", "psp-48k"):
                slow_region = ((384 * 1024 <= absolute < 512 * 1024)
                               or (896 * 1024 <= absolute < 1024 * 1024))
                time.sleep(0.58 if slow_region else 0.025)
            elif CADENCE_PROFILE == "bursty":
                # Good average throughput, but a Wi-Fi power-save-shaped
                # pause after every fourth publication.
                time.sleep(0.24 if (at // step) % 4 == 3 else 0.01)
            elif CADENCE_PROFILE == "live-cdn-prefetch":
                # The 2026-08-10 PSP run measured complete 256 KiB video
                # windows at roughly 0.6--1.2 s over a reused connection.
                # Preserve that average while delivering it in Wi-Fi-shaped
                # bursts. A correctly issued successor overlaps this whole
                # interval with decode; an on-demand request turns it into a
                # visible hold at every cache boundary.
                if at == 0 and first != 0:
                    time.sleep(0.18)
                elif (at // step) % 4 == 3:
                    time.sleep(0.08)
                else:
                    time.sleep(0.02)
            elif (CADENCE_PROFILE == "trickle-reconnect"
                  and first >= 256 * 1024 and attempt == 1):
                # About 2 KiB/s: below both captured stream demand and the
                # fixed safety floor. The client should use its shorter
                # already-starved verdict rather than waiting all eight sec.
                time.sleep(8.0 if at == 0 else 0.25)
            elif (CADENCE_PROFILE == "coupled-asymmetric"
                  and mode == "cadence-audio" and first >= 256 * 1024
                  and attempt == 1 and at == 0):
                # One serialized audio hop pauses for six seconds. Both source
                # buffers should absorb it without skew escaping the session's
                # 250 ms presentation-lead discipline.
                time.sleep(6.0)
            elif (CADENCE_PROFILE == "chaos-recovery"
                  and mode == "cadence-video"
                  and 256 * 1024 <= first < 512 * 1024
                  and attempt == 1 and at == 0):
                # A successor which was issued well ahead of demand stops
                # completely for longer than that runway and the bounded
                # request lifetime. Keeping the original handler asleep also
                # verifies that the replacement is independent of the
                # abandoned socket rather than sharing its state.
                time.sleep(16.0)
            elif CADENCE_PROFILE == "chaos-recovery":
                # Deterministic throughput shifts around the same healthy
                # average: fast bursts, a constrained region, then recovery.
                # These gaps are individually too short for the buffering UI;
                # only exhausting the bounded cache may make them visible.
                if 512 * 1024 <= absolute < 768 * 1024:
                    time.sleep(0.20 if (at // step) % 3 == 2 else 0.03)
                elif absolute >= 768 * 1024:
                    time.sleep(0.06 if (at // step) % 4 == 3 else 0.015)
                else:
                    time.sleep(0.012)
            else:
                time.sleep(0.01)

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        mode = parsed.path.strip("/").split("/")[0]
        if mode.startswith("cadence") and CADENCE_BODY is not None:
            body = CADENCE_BODY
        else:
            body = FRAGMENTED if mode.startswith("fragment") else BODY
        total = len(body)
        span = query.get("range", ["0-%d" % (total - 1)])[0]
        first, _, last = span.partition("-")
        first = int(first)
        last = min(int(last), total - 1)
        payload = body[first:last + 1]
        attempt = 1
        if mode.startswith("cadence") or mode == "drop-each-once":
            with CADENCE_ATTEMPTS_LOCK:
                # Audio and video deliberately request similarly aligned
                # windows in the coupled replay. Their retry histories are
                # independent CDN objects; sharing this counter could make a
                # video request consume audio's one-shot fault injection.
                key = (mode, first, last)
                attempt = CADENCE_ATTEMPTS.get(key, 0) + 1
                CADENCE_ATTEMPTS[key] = attempt
        if mode == "short" or (mode == "stream-short" and first != 0):
            payload = payload[:-1]
        if mode == "expire403" and first != 0:
            self.send_response(403)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if mode == "ignore-range":
            payload = body
        if (mode.startswith("cadence")
                and CADENCE_PROFILE == "setup-latency"):
            # The physical logs measured roughly 0.5 s for the initial TLS
            # setup. Applying it to every new range is deliberately harsher:
            # it proves readahead overlaps request setup rather than assuming
            # keep-alive always works.
            time.sleep(0.5)
        if mode == "partial206" or mode == "bad206":
            self.send_response(206)
            self.send_header(
                "Content-Range", "bytes %d-%d/%d" % (
                    first + (1 if mode == "bad206" else 0), last, total))
        else:
            self.send_response(200)
        self.send_header("Content-Type", "video/mp4")
        if mode != "stream-no-length":
            self.send_header("Content-Length", str(len(payload)))
        else:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        if mode == "stall":
            # Promised and never delivered. Long enough that the C side's own
            # bounds are what end every wait, short enough that a suite which
            # forgets to close one still finishes.
            time.sleep(25)
            return
        if mode == "reset-always" and first != 0:
            prefix = min(len(payload), 16 * 1024)
            self.wfile.write(payload[:prefix])
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if mode == "drop-each-once" and first != 0 and attempt == 1:
            # Each distinct range loses its first connection. This pins the
            # retry-scope boundary when a failed aligned prefetch is replaced
            # by a shifted request that contains one spanning MP4 sample.
            prefix = min(len(payload), 16 * 1024)
            self.wfile.write(payload[:prefix])
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if mode.startswith("cadence"):
            # Coupled mode models the PSP transport worker's one authored HTTP
            # hop at a time: video and audio have independent curl clients in
            # this host test, while the server-side lock serializes bodies.
            if CADENCE_PROFILE in ("coupled-asymmetric", "chaos-recovery"):
                with CADENCE_SERIAL_LOCK:
                    self._write_cadence(mode, first, payload, attempt)
            else:
                self._write_cadence(mode, first, payload, attempt)
        elif mode == "slow" or mode == "fragment-slow":
            step = 16 * 1024
            for at in range(0, len(payload), step):
                self.wfile.write(payload[at:at + step])
                self.wfile.flush()
                time.sleep(0.01)
        else:
            self.wfile.write(payload)


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass


def main():
    global CADENCE_BODY, CADENCE_PROFILE
    if (len(sys.argv) not in (2, 4, 6)
            or (len(sys.argv) >= 4 and sys.argv[2] != "--cadence")
            or (len(sys.argv) == 6 and sys.argv[4] != "--profile")):
        raise SystemExit(
            "usage: run_media_http_range_test.py TEST_EXECUTABLE "
            "[--cadence VIDEO.mp4 [--profile "
            "device-gaps|bursty|setup-latency|drop-once|trickle-reconnect|"
            "coupled-asymmetric|chaos-recovery|live-cdn-prefetch|psp-48k]]")
    cadence_path = None if len(sys.argv) == 2 else sys.argv[3]
    if len(sys.argv) == 6:
        CADENCE_PROFILE = sys.argv[5]
    profiles = {
        "device-gaps", "bursty", "setup-latency", "drop-once",
        "trickle-reconnect", "coupled-asymmetric", "chaos-recovery",
        "live-cdn-prefetch", "psp-48k",
    }
    if CADENCE_PROFILE not in profiles:
        raise SystemExit(f"unknown cadence profile: {CADENCE_PROFILE}")
    if cadence_path is not None:
        with open(cadence_path, "rb") as source:
            CADENCE_BODY = source.read()
    if len(FRAGMENTED) <= 4 * 64 * 1024:
        raise SystemExit(
            "the fragmented fixture must span several 64 KiB windows; "
            f"it is {len(FRAGMENTED)}B")
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", 0))
    except PermissionError as error:
        print(f"media range integration skipped: loopback unavailable: {error}")
        return 77
    finally:
        probe.close()
    server = QuietServer(("127.0.0.1", 0), RangeHandler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        arguments = [
            sys.argv[1], str(server.server_address[1]), str(TOTAL),
            str(len(FRAGMENTED)), str(FRAGMENT_COUNT),
            str(FRAGMENT_SAMPLES)]
        if CADENCE_BODY is not None:
            arguments.extend((
                "--cadence", str(len(CADENCE_BODY)), CADENCE_PROFILE))
        completed = run(
            arguments,
            check=False)
        if completed.returncode != 0:
            print(f"media range test executable exited "
                  f"{completed.returncode}", file=sys.stderr)
        return completed.returncode
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
