#!/usr/bin/env python3
"""Recolor the Tilefinch XMB icon artwork into the Ember chrome palette.

The icon is hand-made artwork with no vector source: a faceted finch on a
rounded card. This tool maps a cool navy source image onto the Ember token
sheet without redrawing it: the facet structure, seams and shading survive
while the hues change.

Source of truth for the target colors: src/psp_ui_theme.h.

The transform is one-way: its input is the original cool-palette artwork,
not its own output. Keep that source outside the generated asset paths and
pass it explicitly if the accent changes.

    tools/recolor_psp_icon_ember.py IN.png OUT.png

Standard library only: the PSP tree has no Pillow dependency.
"""
import colorsys
import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# --- Ember tokens (src/psp_ui_theme.h) ---------------------------------------
GROUND = (0x10, 0x0E, 0x0D)          # PSP_THEME_GROUND
TEXT = (0xFB, 0xF6, 0xF2)            # PSP_THEME_TEXT

# The source card is a saturated deep navy; everything dark inside this hue
# window is card, not bird, and is rewritten to the warm ground with its own
# vignette preserved as a brightness ratio.
GROUND_HUE = (0.52, 0.78)            # 187 deg .. 280 deg
GROUND_MAX_LIGHTNESS = 0.20
GROUND_REFERENCE_LIGHTNESS = 0.082   # measured median of the source card

# Source hue bucket -> (target hue deg, lightness gain, minimum saturation).
# The artwork separates its facets by hue; a warm-only palette has to carry
# that separation on lightness instead, so each bucket also takes a step.
BUCKETS = (
    (300.0, 25.0, 11.0, 0.84, 0.80),    # coral head and legs -> ember red
    (25.0, 70.0, 38.0, 1.20, 0.82),     # yellow beak and flank -> amber
    (70.0, 150.0, 30.0, 1.10, 0.70),    # stray greens -> warm tan
    (150.0, 205.0, 28.0, 1.12, 0.70),   # cyan wing -> ember highlight
    (205.0, 248.0, 21.0, 0.90, 0.78),   # blue facets -> burnt orange
    (248.0, 300.0, 14.0, 0.64, 0.72),   # purple belly -> deep rust
)
NEUTRAL_SATURATION_FLOOR = 0.16      # below this a pixel is glass, not paint


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("%s is not a PNG" % path)
    position = 8
    compressed = bytearray()
    width = height = depth = color_type = None
    while position < len(data):
        (length,) = struct.unpack(">I", data[position:position + 4])
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            (width, height, depth, color_type, _compression, _filter,
             interlace) = struct.unpack(">IIBBBBB", payload)
            if interlace != 0:
                raise ValueError("interlaced PNG unsupported")
            if depth != 8 or color_type not in (2, 6):
                raise ValueError("only 8-bit RGB/RGBA PNGs are supported")
        elif kind == b"IDAT":
            compressed += payload
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(compressed))
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    pixels = bytearray(stride * height)
    previous = bytearray(stride)
    at = 0
    for y in range(height):
        filter_type = raw[at]
        at += 1
        line = bytearray(raw[at:at + stride])
        at += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    predictor = a
                elif pb <= pc:
                    predictor = b
                else:
                    predictor = c
                line[i] = (line[i] + predictor) & 0xFF
        elif filter_type != 0:
            raise ValueError("unsupported PNG filter %d" % filter_type)
        pixels[y * stride:(y + 1) * stride] = line
        previous = line
    return width, height, channels, pixels


def write_png(path, width, height, channels, pixels):
    stride = width * channels
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        rows += pixels[y * stride:(y + 1) * stride]

    def chunk(kind, payload):
        checksum = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", checksum))

    header = struct.pack(
        ">IIBBBBB", width, height, 8, 2 if channels == 3 else 6, 0, 0, 0)
    open(path, "wb").write(
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b""))


def bucket_for(hue_degrees):
    for low, high, hue, gain, saturation in BUCKETS:
        if low > high:
            if hue_degrees >= low or hue_degrees < high:
                return hue, gain, saturation
        elif low <= hue_degrees < high:
            return hue, gain, saturation
    return 24.0, 1.0, 0.70


def recolor_pixel(red, green, blue):
    hue, lightness, saturation = colorsys.rgb_to_hls(
        red / 255.0, green / 255.0, blue / 255.0)
    if lightness <= 0.012:
        return red, green, blue           # the outer frame stays black
    if saturation < NEUTRAL_SATURATION_FLOOR:
        # The browser-window glass on the wing: warm it toward TEXT.
        text_hue = colorsys.rgb_to_hls(*[c / 255.0 for c in TEXT])[0]
        warmed = colorsys.hls_to_rgb(text_hue, lightness, 0.30)
        return tuple(int(c * 255 + 0.5) for c in warmed)
    if (lightness < GROUND_MAX_LIGHTNESS
            and GROUND_HUE[0] <= hue <= GROUND_HUE[1]):
        scale = lightness / GROUND_REFERENCE_LIGHTNESS
        return tuple(min(255, int(c * scale + 0.5)) for c in GROUND)
    target_hue, gain, floor = bucket_for(hue * 360.0)
    warmed = colorsys.hls_to_rgb(
        target_hue / 360.0,
        min(0.94, lightness * gain),
        min(0.95, max(saturation * 0.85, floor)))
    return tuple(int(c * 255 + 0.5) for c in warmed)


def recolor(source, destination):
    width, height, channels, pixels = read_png(source)
    output = bytearray(width * height * channels)
    cache = {}
    for index in range(width * height):
        at = index * channels
        key = bytes(pixels[at:at + 3])
        mapped = cache.get(key)
        if mapped is None:
            mapped = bytes(recolor_pixel(key[0], key[1], key[2]))
            cache[key] = mapped
        output[at:at + 3] = mapped
        if channels == 4:
            output[at + 3] = pixels[at + 3]
    write_png(destination, width, height, channels, output)
    return width, height


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    size = recolor(sys.argv[1], sys.argv[2])
    print("recolored %s -> %s (%dx%d)" % (sys.argv[1], sys.argv[2], *size))
