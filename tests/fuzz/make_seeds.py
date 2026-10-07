#!/usr/bin/env python3
"""Build the libFuzzer seed corpora for tests/fuzz from inline samples and
existing repository fixtures.

usage: make_seeds.py OUTPUT_DIR [TARGET ...]

Writes OUTPUT_DIR/<target>/seed-NNN files. Seeds are regenerated rather than
checked in so the corpus tracks the fixtures it is derived from.
"""
import hashlib
import os
import re
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def fixture_files(*suffixes, roots=("tests/fixtures", "fixtures"), limit=400,
                  max_bytes=64 * 1024):
    found = []
    for root in roots:
        base = os.path.join(ROOT, root)
        for directory, _, names in sorted(os.walk(base)):
            for name in sorted(names):
                if name.lower().endswith(suffixes):
                    path = os.path.join(directory, name)
                    try:
                        if os.path.getsize(path) <= max_bytes:
                            found.append(path)
                    except OSError:
                        pass
    return found[:limit]


def read(path, limit=None):
    with open(path, "rb") as handle:
        data = handle.read()
    return data if limit is None else data[:limit]


URLS = [
    b"https://a.example.com/p?q=1#f",
    b"http://[2001:db8::1]:8080/a/../b/%2e/c",
    b"https://127.0.0.1/",
    b"http://x.localhost/a//b/..",
    b"../up/./x?y",
    b"//cdn.example.org/s.js",
    b"data:text/plain;base64,SGVsbG8=",
    b"data:,a%20b%2",
    b"data:image/png;charset=utf-8;base64,iVBO Rw0K",
    b"?only",
    b"#frag",
    b"HTTPS://WWW.Example.CO.UK:443/%2E%2e/",
    b"http://[::ffff:1.2.3.4]/",
    b"https://a.b.c.d.e.blogspot.com/x",
]


def seeds_url():
    return [bytes([i]) + url for i, url in enumerate(URLS)]


CSS = [
    b"@media(max-width:600px){.x{display:flex;--tone:#123456;color:var(--tone)}}",
    b":root{--w:8197;--h:4611}.a{width:clamp(130px,39.4%,159px);"
    b"flex:0 0 calc((100% - (3 * 16px))/3.5);"
    b"padding-bottom:calc(100%*(var(--h)/var(--w)));height:calc(100% - 155px)}",
    b".a{width:calc(calc(calc(calc(1px))));height:min(1px,2px,3px);"
    b"max-width:calc(2px * 3px);min-width:calc(20px/0);padding:calc(1px + 2qu)}",
    b":root{--a:var(--b);--b:var(--a)}.x{width:calc(var(--a) + 1px)}",
    b"@layer a,b;@supports(display:grid){*{display:grid}}"
    b"@container (min-width: 300px){div>p:nth-child(2n+1 of .k){color:red}}",
    b"a[href^='http' i]:not(.b, #c)::before{content:\"\\201C\" attr(title);"
    b"counter-increment:x 2}",
    b"div:has(> img + p, ~ .z) .q:is(h1,h2):where(:hover){"
    b"background:url(data:image/png;base64,AAAA) no-repeat 10% 20%/cover,"
    b"linear-gradient(45deg,red 10%,rgba(0,0,0,.5) 90%)}",
    b".g{display:grid;grid-template-columns:repeat(auto-fill,minmax(10em,1fr)) "
    b"[a] 20px;grid-template-areas:\"a b\" \"c d\";grid-area:a/b/c/d}",
    b"@font-face{font-family:F;src:url(f.woff) format('woff');"
    b"unicode-range:U+0-7F}.f{font:italic 700 12px/1.5 F,serif}",
    b".t{transition:color 1s ease-in-out 0.5s,width 2s steps(4,end);"
    b"transform:translate(10px,-50%) rotate(45deg);"
    b"box-shadow:inset 0 0 0 1px #000,2px 2px 4px rgb(0 0 0/50%)}",
    b"@import url(x.css) screen;@namespace svg url(http://www.w3.org/2000/svg);"
    b"svg|a{color:hsl(120deg 50% 50%)}",
    b".m{mask:url(#m) center/contain no-repeat;clip-path:inset(1px 2px round 3px);"
    b"filter:blur(2px) drop-shadow(1px 1px red);"
    b"text-shadow:1px 1px 2px black,0 0 1em blue}",
    b"*{margin:0}html{font-size:62.5%}body{line-height:1.4;"
    b"font-family:system-ui,-apple-system,\"Segoe UI\",Roboto}",
    b"p::first-line{color:red}li::marker{content:'>'}"
    b"input:checked+label::after{content:counter(c, upper-roman)}",
]


def seeds_css():
    seeds = list(CSS)
    # Pair a few sheets with an inline style and a selector/query part.
    seeds.append(b".k{color:red}\0color:blue;width:calc(1px + 2em)\0"
                 b"div > p.k:not(:first-child)")
    seeds.append(b"\0display:grid;grid-template-columns:1fr 2fr\0"
                 b"(min-width: 300px) and (orientation: landscape)")
    seeds.append(b"\0--x:1px;margin:var(--x)\0rgb(10 20 30 / 40%)")
    for path in fixture_files(".css", roots=("tests", "fixtures", "examples"),
                              limit=60):
        seeds.append(read(path, 16 * 1024))
    pattern = re.compile(rb"<style[^>]*>(.*?)</style>", re.S | re.I)
    for path in fixture_files(".html", ".htm", limit=400):
        for match in pattern.finditer(read(path)):
            if match.group(1).strip():
                seeds.append(match.group(1)[:16 * 1024])
    return seeds


def seeds_html():
    seeds = [
        b"<!doctype html><style>@media(max-width:600px){.x{display:flex}}</style>"
        b"<body><div class=x style='color:red;width:calc(100% - 2px)'>ok",
        b"<script>const x='</not-script>';</script><!--unterminated-->"
        b"<textarea>&amp;<fake></textarea>",
        b"<table>text<tr><td colspan=999999>A<td>B</table>"
        b"<svg><foreignObject><p style='width:calc(100% - 2px)'>x",
        b"<html><head><style>@layer a,b;</style></head><body><template>"
        b"<select><option>one<option>two",
        b"<form><input type=checkbox checked><label>l</label><button>b</button>"
        b"<img srcset='a.png 1x, b.png 2x' sizes='100vw' alt=x></form>",
        b"<p dir=rtl>\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d abc <bdi>x</bdi></p>"
        b"<ol start=3 reversed><li value=9>a</ol>",
    ]
    for path in fixture_files(".html", ".htm", limit=150):
        seeds.append(read(path, 32 * 1024))
    # Byte 0 picks the streaming parser chunk size.
    return [bytes([i % 5]) + seed for i, seed in enumerate(seeds)]


def seeds_bidi():
    texts = [
        "abc \u05e9\u05dc\u05d5\u05dd 123 def",
        "\u0627\u0644\u0633\u0644\u0627\u0645 (hello) \u0661\u0662",
        "\u202bembedded \u202aLTR\u202c rtl\u202c \u2067iso\u2069",
        "a\u2029\u05d0\u2029b\u200f\u200e\u061c x",
        "\u0644\u0627 \u0644\u0623 combining e\u0301 \u05b0\u05d1",
    ]
    seeds = []
    for i, text in enumerate(texts):
        for base in range(4):
            seeds.append(bytes([base | (i << 2)]) + text.encode("utf-8"))
    seeds.append(b"\x02\xd7\xff\xfe\xc0\x80abc\xed\xa0\x80")
    return seeds


def chunk_seed(header, body):
    return header + body


def seeds_http():
    return [
        b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        b"Transfer-Encoding: chunked\r\nSet-Cookie: a=b; Path=/; Secure\r\n\r\n"
        b"5\r\nhello\r\n0\r\n\r\n",
        b"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-99/1000\r\n"
        b"Content-Length: 100\r\nAccept-Ranges: bytes\r\n\r\n",
        b"HTTP/1.1 302 Found\r\nLocation: /next?x=1\r\n"
        b"Content-Security-Policy: default-src 'self'; script-src 'nonce-abc' "
        b"https://cdn.example.com; upgrade-insecure-requests\r\n\r\n",
        b"HTTP/1.0 200 OK\r\nCache-Control: max-age=60, no-transform\r\n"
        b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n\r\n",
        b"Strict-Transport-Security: max-age=31536000; includeSubDomains; "
        b"preload\nAccess-Control-Allow-Origin: https://site.test\n"
        b"Access-Control-Allow-Credentials: true\n"
        b"Cross-Origin-Resource-Policy: same-site\n"
        b"X-Content-Type-Options: nosniff\n"
        b"Referrer-Policy: no-referrer, strict-origin-when-cross-origin\n"
        b"X-Frame-Options: SAMEORIGIN\n",
        b"Content-Security-Policy: script-src 'self' 'nonce-abc' "
        b"'sha256-bhHHL3z2vDgxUt0W3dWQOrprscmda2Y5pLsLg4GF+pI=' "
        b"https://*.site.test:* 'strict-dynamic'; style-src 'unsafe-inline'; "
        b"frame-ancestors 'self' https://a.test; base-uri 'none'; "
        b"form-action https:; worker-src blob:\n"
        b"Content-Security-Policy-Report-Only: default-src 'none'\n"
        b"Content-Security-Policy: img-src data: http://www.site.test/img.png, "
        b"connect-src wss://site.test\n",
        b"Set-Cookie: __Host-id=1; Path=/; Secure; HttpOnly; SameSite=Lax\n"
        b"Set-Cookie: a=b; Domain=.site.test; Path=/dir; "
        b"Expires=Wed, 21 Oct 2037 07:28:00 GMT; Max-Age=100\n"
        b"Set-Cookie: c=d; Partitioned; Secure; SameSite=None\n"
        b"Set-Cookie: __Secure-x=y; Secure; Expires=Sun, 06-Nov-94 08:49:37 GMT\n",
        b"Content-Range: bytes 0-99/100\nContent-Range: bytes 1000-5095/*\n"
        b"Accept-CH: Sec-CH-UA-Model, Sec-CH-UA-Platform-Version, DPR\n"
        b"Critical-CH: Sec-CH-UA-Model, Viewport-Width\n"
        b"Integrity: sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU= "
        b"sha512-z4PhNX7vuL3xVChQ1m2AB9Yg5AULVxXcg/SpIdNs6c5H0NE8XYXysP+DGNKHf"
        b"uwvY7kxvUdBeoGlODJ6+SfaPg==?opt sha384-abc md5-xyz\n"
        b"Location: ../other?x=1#frag\n"
        b"Request: X-Requested-With: XMLHttpRequest\\r\\nAccept: */*\n",
        b"\x03\xe8normal closure\n",
    ]


def seeds_m3u8():
    seeds = [
        b"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n"
        b"#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:9.009,\nseg0.ts\n"
        b"#EXTINF:9.009,\nseg1.ts\n#EXT-X-ENDLIST\n",
        b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=480x272,"
        b"CODECS=\"avc1.42e01e,mp4a.40.2\"\nlow.m3u8\n"
        b"#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",URI=\"a.m3u8\"\n"
        b"#EXT-X-STREAM-INF:BANDWIDTH=2560000,AUDIO=\"aud\"\nhigh.m3u8\n",
        b"#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MAP:URI=\"init.mp4\","
        b"BYTERANGE=\"720@0\"\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0x1234\n"
        b"#EXTINF:6.0,\n#EXT-X-BYTERANGE:1000@720\nmain.mp4\n"
        b"#EXT-X-DISCONTINUITY\n#EXTINF:6.0,\nnext.mp4\n",
    ]
    for path in fixture_files(".m3u8", limit=50):
        seeds.append(read(path, 32 * 1024))
    return seeds


def seeds_by_extension(*suffixes, limit=60, max_bytes=256 * 1024):
    return [read(path) for path in fixture_files(
        *suffixes, roots=("tests", "fixtures", "examples", "benchmarks"),
        limit=limit, max_bytes=max_bytes)]


def mp4_boxes(data):
    offset = 0
    while offset + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[offset:offset + 8])
        if size < 8 or offset + size > len(data):
            break
        yield kind, data[offset:offset + size]
        offset += size


MP4_CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts",
                  b"dinf", b"mvex", b"moof", b"traf"}


def mp4_shrink_tables(box, sample_size, chunk_offset):
    """Rewrite stsz/stco/co64 in place so every sample is sample_size bytes
    and every chunk starts at chunk_offset, keeping box sizes unchanged."""
    data = bytearray(box)
    def walk(start, end):
        offset = start
        while offset + 8 <= end:
            size, kind = struct.unpack(">I4s", data[offset:offset + 8])
            if size < 8 or offset + size > end:
                return
            payload = offset + 8
            if kind in MP4_CONTAINERS:
                walk(payload, offset + size)
            elif kind == b"stsz" and size >= 20:
                count = struct.unpack(">I", data[payload + 8:payload + 12])[0]
                struct.pack_into(">I", data, payload + 4, 0)
                for i in range(min(count, (size - 20) // 4)):
                    struct.pack_into(">I", data, payload + 12 + 4 * i,
                                     sample_size)
            elif kind in (b"stco", b"co64") and size >= 16:
                count = struct.unpack(">I", data[payload + 4:payload + 8])[0]
                width = 4 if kind == b"stco" else 8
                for i in range(min(count, (size - 16) // width)):
                    struct.pack_into(">I" if width == 4 else ">Q", data,
                                     payload + 8 + width * i, chunk_offset)
            offset += size
    walk(0, len(data))
    return bytes(data)


def mp4_find(data, path):
    """First box at the given container path, e.g. [b"moov", b"trak"]."""
    for kind, box in mp4_boxes(data):
        if kind == path[0]:
            return box if len(path) == 1 else mp4_find(box[8:], path[1:])
    return None


def box(kind, payload):
    return struct.pack(">I4s", len(payload) + 8, kind) + payload


def full_box(kind, version, flags, payload):
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)


def fragmented_mp4(stsd, samples=6):
    matrix = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0,
                         0x40000000)
    mvhd = full_box(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, 1000, 0)
                    + struct.pack(">IH", 0x10000, 0x100) + bytes(10)
                    + matrix + bytes(24) + struct.pack(">I", 2))
    tkhd = full_box(b"tkhd", 0, 3, struct.pack(">IIIII", 0, 0, 1, 0, 0)
                    + bytes(8) + struct.pack(">HHHH", 0, 0, 0, 0) + matrix
                    + struct.pack(">II", 320 << 16, 240 << 16))
    mdhd = full_box(b"mdhd", 0, 0, struct.pack(">IIIIHH", 0, 0, 90000, 0,
                                               0x55c4, 0))
    hdlr = full_box(b"hdlr", 0, 0, struct.pack(">I4s", 0, b"vide")
                    + bytes(12) + b"v\0")
    dinf = box(b"dinf", full_box(b"dref", 0, 0, struct.pack(">I", 1)
                                 + full_box(b"url ", 0, 1, b"")))
    stbl = box(b"stbl", stsd
               + full_box(b"stts", 0, 0, struct.pack(">I", 0))
               + full_box(b"stsc", 0, 0, struct.pack(">I", 0))
               + full_box(b"stsz", 0, 0, struct.pack(">II", 0, 0))
               + full_box(b"stco", 0, 0, struct.pack(">I", 0)))
    minf = box(b"minf", full_box(b"vmhd", 0, 1, bytes(8)) + dinf + stbl)
    trak = box(b"trak", tkhd + box(b"mdia", mdhd + hdlr + minf))
    mvex = box(b"mvex", full_box(b"trex", 0, 0, struct.pack(
        ">IIIII", 1, 1, 3000, 0, 0)))
    moov = box(b"moov", mvhd + trak + mvex)
    ftyp = box(b"ftyp", b"iso5\0\0\0\x01iso5avc1mp41")

    def moof(sequence, decode_time):
        trun_body = struct.pack(">Ii", samples, 0) + b"".join(
            struct.pack(">IIIi", 3000, 4, 0x02000000 if i else 0, 0)
            for i in range(samples))
        traf = box(b"traf", full_box(b"tfhd", 0, 0x020000,
                                     struct.pack(">I", 1))
                   + full_box(b"tfdt", 1, 0, struct.pack(">Q", decode_time))
                   + full_box(b"trun", 0, 0x000F01, trun_body))
        fragment = box(b"moof", full_box(b"mfhd", 0, 0,
                                         struct.pack(">I", sequence)) + traf)
        # Patch trun data_offset to point just past the moof header + mdat
        # header, relative to the moof start.
        at = fragment.index(b"trun") + 4 + 4 + 4
        fragment = fragment[:at] + struct.pack(">i", len(fragment) + 8) + \
            fragment[at + 4:]
        return fragment + box(b"mdat", b"\x00\x00\x00\x00" * samples)

    first = moof(1, 0)
    second = moof(2, 3000 * samples)
    sidx = full_box(b"sidx", 0, 0, struct.pack(">IIIIHH", 1, 90000, 0, 0, 0,
                                               2)
                    + struct.pack(">III", len(first), 3000 * samples,
                                  0x90000000)
                    + struct.pack(">III", len(second), 3000 * samples,
                                  0x90000000))
    return ftyp + moov + sidx + first + second


def seeds_mp4():
    seeds = []
    for path in fixture_files(".mp4", limit=2, max_bytes=4 << 20):
        trak = mp4_find(read(path), [b"moov", b"trak"])
        stsd = trak and mp4_find(trak[8:], [b"mdia", b"minf", b"stbl",
                                            b"stsd"])
        if stsd:
            seeds.append(fragmented_mp4(stsd))
            seeds.append(fragmented_mp4(stsd, 1))
    for path in fixture_files(".mp4", ".m4a", limit=10, max_bytes=4 << 20):
        data = read(path)
        boxes = list(mp4_boxes(data))
        ftyp = b"".join(box for kind, box in boxes if kind == b"ftyp")
        moov = b"".join(box for kind, box in boxes if kind == b"moov")
        mdat = [box for kind, box in boxes if kind == b"mdat"]
        if moov and mdat:
            # A few KiB that still opens: tiny samples, all chunks pointing
            # at the start of a short mdat (overlap is legal).
            body = mdat[0][8:8 + 2048]
            start = len(ftyp) + len(moov) + 8
            small = mp4_shrink_tables(moov, 4, start)
            seeds.append(ftyp + small + struct.pack(">I4s", len(body) + 8,
                                                    b"mdat") + body)
            start = len(ftyp) + 8
            seeds.append(ftyp + struct.pack(">I4s", len(body) + 8, b"mdat")
                         + body + mp4_shrink_tables(moov, 4, start))
        seeds.append(data[:3000])
    return seeds


def ts_packet(pid, payload, start=False, counter=0):
    header = struct.pack(">BHB", 0x47, (0x4000 if start else 0) | pid,
                         0x10 | (counter & 15))
    if len(payload) < 184:
        stuffing = 184 - len(payload) - 1
        adaptation = bytes([stuffing]) + (
            b"\x00" + b"\xff" * (stuffing - 1) if stuffing > 0 else b"")
        header = header[:3] + bytes([0x30 | (counter & 15)])
        return header + adaptation + payload
    return header + payload[:184]


def ts_section(table_id, body):
    length = len(body) + 4 + 5
    section = struct.pack(">BH", table_id, 0xB000 | length) + \
        b"\x00\x01\xc1\x00\x00" + body
    return b"\x00" + section + b"\x00\x00\x00\x00"


def pes(stream_id, pts, data):
    p = pts
    pts_bytes = bytes([0x21 | ((p >> 29) & 0x0E), (p >> 22) & 0xFF,
                       ((p >> 14) & 0xFE) | 1, (p >> 7) & 0xFF,
                       ((p << 1) & 0xFE) | 1])
    return b"\x00\x00\x01" + bytes([stream_id]) + b"\x00\x00\x80\x80\x05" + \
        pts_bytes + data


def seeds_ts():
    pat = ts_packet(0, ts_section(0, b"\x00\x01\xf0\x00"), True)
    pmt = ts_packet(0x1000, ts_section(
        2, b"\xe1\x00\xf0\x00" + b"\x1b\xe1\x00\xf0\x00"
        + b"\x0f\xe1\x01\xf0\x00"), True)
    sps = b"\x00\x00\x00\x01\x67\x42\xc0\x1e\xd9\x01\x40\x7b\x20"
    pps = b"\x00\x00\x00\x01\x68\xce\x3c\x80"
    idr = b"\x00\x00\x00\x01\x65\x88\x84\x00\x33\xff" + b"\x10" * 40
    aud = b"\x00\x00\x00\x01\x09\xf0"
    slice_ = b"\x00\x00\x00\x01\x41\x9a\x02" + b"\x22" * 30
    adts = b"\xff\xf1\x50\x80\x02\x1f\xfc" + b"\x21" * 9
    seeds = []
    video = [ts_packet(0x100, pes(0xE0, 9000 * i,
                                  (aud + sps + pps + idr) if i == 0
                                  else aud + slice_), True, i)
             for i in range(3)]
    audio = [ts_packet(0x101, pes(0xC0, 9000 * i, adts * 3), True, i)
             for i in range(2)]
    stream = pat + pmt + b"".join(video) + b"".join(audio)
    seeds.append(b"\x00" + stream)
    seeds.append(b"\x09" + stream[:188 * 3] + b"\x47\x00" + stream[188 * 3:])
    return seeds


def png(width, height, color_type, depth, rows, extra=b"", interlace=0):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(
            ">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0,
                         interlace)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + extra
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def seeds_image():
    build = os.environ.get("TILEFINCH_FUZZ_BUILD",
                           os.path.join(ROOT, "build-fuzz"))
    raster = []
    raster.append(png(4, 3, 2, 8, b"".join(
        b"\x00" + bytes(range(y * 12, y * 12 + 12)) for y in range(3))))
    raster.append(png(4, 2, 6, 8, b"\x01" + bytes(16) + b"\x04" + bytes(16)))
    plte = struct.pack(">I", 6) + b"PLTE" + b"\xff\x00\x00\x00\xff\x00"
    plte += struct.pack(">I", zlib.crc32(b"PLTE\xff\x00\x00\x00\xff\x00"))
    raster.append(png(8, 2, 3, 1, b"\x00\xa5\x00\x5a", plte))
    raster.append(png(3, 3, 0, 16, b"".join(b"\x02" + bytes(6)
                                             for _ in range(3)), b"", 1))
    raster.append(b"GIF89a\x02\x00\x02\x00\x80\x00\x00\x00\x00\x00\xff\xff"
                  b"\xff!\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x02"
                  b"\x00\x02\x00\x00\x02\x03\x84\x1d\x05\x00;")
    pixels = bytes(range(4 * 2 * 3)) + bytes(0)
    raster.append(b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
                  + struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 24, 0,
                                len(pixels), 2835, 2835, 0, 0) + pixels)
    raster.append(b"RIFF\x1a\x00\x00\x00WEBPVP8L\x0d\x00\x00\x00\x2f\x00\x00"
                  b"\x00\x10\x07\x10\x11\x11\x88\x88\xfe\x07\x00")
    candidates = fixture_files(".png", ".jpg", ".jpeg", ".gif", ".webp",
                               ".bmp", roots=("tests", "docs", "examples",
                                              "psp-assets"),
                               limit=40, max_bytes=40 * 1024)
    webp = os.path.join(build, "_deps/libwebp-src/examples/test.webp")
    if os.path.exists(webp):
        candidates.append(webp)
    for path in candidates:
        raster.append(read(path))
    seeds = []
    for index, image in enumerate(raster):
        seeds.append(bytes([(index % 4) << 1]) + image)
    svgs = [
        b"<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10' "
        b"viewBox='0 0 20 10'><rect x='1' y='1' width='8' height='8' "
        b"fill='#f00' stroke='blue' stroke-width='2' rx='2'/>"
        b"<circle cx='15' cy='5' r='4' fill='url(#g)'/><defs>"
        b"<linearGradient id='g'><stop offset='0' stop-color='red'/>"
        b"<stop offset='1' stop-color='blue' stop-opacity='.5'/>"
        b"</linearGradient></defs><path d='M1 1L19 9Q10 0 1 9z "
        b"m2 2c1 1 2 2 3 3a2 2 0 1 0 4 0'/></svg>",
        b"<svg width='8' height='8'><g transform='rotate(45 4 4) "
        b"scale(.5)'><polygon points='0,0 8,0 4,8'/><polyline "
        b"points='0 0 1 1 2 0' fill='none' stroke='#000'/></g></svg>",
    ]
    svgs += [read(path) for path in fixture_files(
        ".svg", roots=("fixtures", "examples", "tests"), limit=20)]
    for name in ("drawing.svg", "nano.svg"):
        path = os.path.join(build, "_deps/nanosvg-src/example", name)
        if os.path.exists(path) and os.path.getsize(path) < 64 * 1024:
            svgs.append(read(path))
    seeds += [b"\x01" + svg for svg in svgs]
    return seeds


def seeds_hls_source():
    seeds = []
    streams = [ts[1:] for ts in seeds_ts()]
    id3 = b"ID3\x04\x00\x00\x00\x00\x00\x04test"
    adts = b"\xff\xf1\x4c\x80\x00\xff\xfc"
    for mode in (0x00, 0x05, 0x46, 0x3c):
        seeds.append(bytes([mode]) + streams[0])
        seeds.append(bytes([mode]) + streams[0] + b"|SEG|" + streams[1])
    seeds.append(b"\x02" + id3 + adts * 3 + b"|SEG|" + adts + b"|SEG|"
                 + id3 + adts)
    return seeds


def woff_from_sfnt(sfnt):
    """Wrap a TrueType sfnt in WOFF 1.0, compressing every table."""
    count = struct.unpack(">H", sfnt[4:6])[0]
    tables = []
    for i in range(count):
        tag, checksum, offset, length = struct.unpack(
            ">4sIII", sfnt[12 + 16 * i:28 + 16 * i])
        raw = sfnt[offset:offset + length]
        packed = zlib.compress(raw)
        if len(packed) >= len(raw):
            packed = raw
        tables.append((tag, checksum, raw, packed))
    offset = 44 + 20 * count
    directory, body = b"", b""
    for tag, checksum, raw, packed in tables:
        directory += struct.pack(">4sIIII", tag, offset + len(body),
                                 len(packed), len(raw), checksum)
        body += packed + b"\0" * (-len(packed) % 4)
    total = 44 + len(directory) + len(body)
    sfnt_size = 12 + 16 * count + sum(
        len(raw) + (-len(raw) % 4) for _, _, raw, _ in tables)
    header = struct.pack(">4sIIHHIHHIIIII", b"wOFF", 0x00010000, total, count,
                         0, sfnt_size, 1, 0, 0, 0, 0, 0, 0)
    return header + directory + body


def seeds_font():
    seeds = []
    for path in fixture_files(".ttf", roots=("fonts",), limit=4,
                              max_bytes=128 * 1024):
        sfnt = read(path)
        seeds.append(sfnt)
        seeds.append(woff_from_sfnt(sfnt))
    return seeds


def seeds_misc():
    manifest = (b'{"name":"App","short_name":"A","start_url":"./start?x=1",'
                b'"scope":"/m/","display":"standalone","theme_color":"#336699",'
                b'"background_color":"rgb(1,2,3)","icons":[{"src":"i.png",'
                b'"sizes":"48x48 96x96","type":"image/png"}],"x":{"y":[1,2,'
                b'{"z":null}]},"description":"\\u00e9\\ud83d\\ude00"}')
    vtt = (b"WEBVTT\nKind: captions\n\n1\n00:00:01.000 --> 00:00:02.500 "
           b"align:start\n<c.colorE5E5E5>Hello</c> &amp; <i>world</i>\n\n"
           b"00:01:00.000 --> 00:01:01.000\nline one\nline two\n\n"
           b"NOTE comment\n\n99:59:59.999 --> 100:00:00.000\nlate\n")
    packet = (struct.pack(">IBBBBIIIHHI", 0x54464d50, 1, 1, 0, 0, 7, 3, 9, 1,
                          5, 42) + b"hello")
    player = (b'{"playabilityStatus":{"status":"OK"},"videoDetails":'
              b'{"title":"T","lengthSeconds":"60"},"streamingData":'
              b'{"formats":[{"itag":18,"width":640,"height":360,'
              b'"mimeType":"video/mp4; codecs=\\"avc1.42001E, mp4a.40.2\\"",'
              b'"url":"https://rr1.googlevideo.com/v.mp4?a=1","contentLength":'
              b'"1000"}],"adaptiveFormats":[{"itag":140,"bitrate":128000,'
              b'"mimeType":"audio/mp4; codecs=\\"mp4a.40.2\\"","url":'
              b'"https://rr3.googlevideo.com/a.m4a","audioTrack":{"id":"en.1",'
              b'"displayName":"English","audioIsDefault":true}}]},"captions":'
              b'{"playerCaptionsTracklistRenderer":{"captionTracks":[{"baseUrl":'
              b'"https://www.youtube.com/api/timedtext?v=x&lang=en","name":'
              b'{"simpleText":"English"},"vssId":".en","languageCode":"en"}]}}}')
    return [b"\x00" + manifest, b"\x01" + vtt, b"\x02" + vtt,
            b"\x07" + vtt, b"\x03" + packet, b"\x04" + player]


TARGETS = {
    "misc": seeds_misc,
    "bidi": seeds_bidi,
    "font": seeds_font,
    "hls_source": seeds_hls_source,
    "image": seeds_image,
    "mp4": seeds_mp4,
    "ts": seeds_ts,
    "url": seeds_url,
    "css": seeds_css,
    "html": seeds_html,
    "http": seeds_http,
    "hls": seeds_m3u8,
}


# libFuzzer dictionaries: string literals the parsers compare against,
# harvested from the sources so they stay current.
DICTIONARY_SOURCES = {
    "css": (["src/style*.c", "src/style_sheet/*.inc", "src/layout*.c"],
            rb'"([@:!a-z-][@:a-z0-9-]{1,39}\(?)"',
            [b"{", b"}", b";", b":", b"(", b")", b",", b"/*", b"*/", b"!important",
             b"calc(", b"var(--", b"px", b"em", b"rem", b"%", b"vw", b"vh",
             b"deg", b"fr", b"\\", b"@media", b"@supports", b"@container",
             b"@layer", b"@font-face", b"@keyframes", b"@import", b"::", b">",
             b"+", b"~", b"[", b"]", b"*", b"|=", b"^=", b"$=", b"*=", b"~="]),
    "html": (["src/document.c", "src/layout*.c", "src/style_resolve.c"],
             rb'"([a-z][a-z0-9-]{1,23})"',
             [b"<", b">", b"</", b"/>", b"<!--", b"-->", b"<!doctype html>",
              b"=\"", b"&amp;", b"&#x", b"<style>", b"</style>", b"<svg>",
              b"<table>", b"<template>", b"<math>", b"style=\""]),
}


def write_dictionary(output, target):
    import glob
    globs, pattern, extra = DICTIONARY_SOURCES[target]
    words = set(extra)
    for spec in globs:
        for path in glob.glob(os.path.join(ROOT, spec)):
            words.update(re.findall(pattern, read(path)))
    path = os.path.join(output, target + ".dict")
    with open(path, "wb") as handle:
        for word in sorted(words):
            escaped = word.replace(b"\\", b"\\\\").replace(b'"', b'\\"')
            handle.write(b'"' + escaped + b'"\n')
    print("%s.dict: %d entries" % (target, len(words)))


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    output = argv[1]
    targets = argv[2:] or sorted(TARGETS)
    for target in targets:
        directory = os.path.join(output, target)
        os.makedirs(directory, exist_ok=True)
        seen = set()
        for seed in TARGETS[target]():
            digest = hashlib.sha1(seed).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            with open(os.path.join(directory, "seed-" + digest[:16]), "wb") as f:
                f.write(seed)
        print("%s: %d seeds" % (target, len(seen)))
        if target in DICTIONARY_SOURCES:
            write_dictionary(output, target)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
