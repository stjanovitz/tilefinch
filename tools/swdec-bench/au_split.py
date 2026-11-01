#!/usr/bin/env python3
"""Split an Annex-B H.264 stream into access units; write [u32 LE len][AU]..."""
import struct, sys

def nals(data):
    i, n, starts = 0, len(data), []
    while True:
        j = data.find(b"\x00\x00\x01", i)
        if j < 0: break
        starts.append(j + 3); i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        while e > s and data[e - 1] == 0: e -= 1
        yield data[s:e]

def first_mb_zero(nal):
    return len(nal) > 1 and (nal[1] & 0x80) != 0   # ue(v) == 0 is a single '1' bit

data = open(sys.argv[1], "rb").read()
aus, cur, have_vcl = [], [], False
for nal in nals(data):
    t = nal[0] & 0x1F
    starts_au = False
    if t in (6, 7, 8, 9) and have_vcl: starts_au = True
    if t in (1, 5) and have_vcl and first_mb_zero(nal): starts_au = True
    if starts_au:
        aus.append(cur); cur = []; have_vcl = False
    cur.append(nal)
    if t in (1, 5): have_vcl = True
if cur: aus.append(cur)
with open(sys.argv[2], "wb") as f:
    for au in aus:
        b = b"".join(b"\x00\x00\x00\x01" + x for x in au)
        f.write(struct.pack("<I", len(b))); f.write(b)
print(len(aus), "access units")
