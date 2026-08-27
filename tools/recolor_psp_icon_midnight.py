#!/usr/bin/env python3
"""Recolor Tilefinch's warm brand artwork into the Midnight blue palette.

The committed master, ICON0, and social-preview images share the same
faceted artwork at different raster sizes.  This transform changes only
saturated warm paint; the black card, pale browser window, seams, shading,
and alpha are retained exactly.  It is intentionally idempotent, so running
it on an already-blue output is a no-op.

    tools/recolor_psp_icon_midnight.py IN.png OUT.png

Standard library only: the PSP build has no image-library dependency.
"""

import colorsys
import sys

from recolor_psp_icon_ember import read_png, write_png


def midnight_pixel(red, green, blue):
    hue, lightness, saturation = colorsys.rgb_to_hls(
        red / 255.0, green / 255.0, blue / 255.0)
    degrees = hue * 360.0

    # Neutrals form the card, seams, eye, and browser-window inset.  Very
    # dark pixels are also card shadow even when compression gives them a
    # slight warm cast.
    if lightness <= 0.14 or saturation < 0.18:
        return red, green, blue

    # Only the Ember artwork's red/orange/yellow paint is transformed.
    # Existing blue artwork therefore survives a second invocation.
    if not (degrees < 82.0 or degrees >= 330.0):
        return red, green, blue

    if degrees >= 330.0 or degrees < 15.0:
        target_hue = 216.0
    elif degrees < 36.0:
        target_hue = 211.0
    elif degrees < 57.0:
        target_hue = 204.0
    else:
        target_hue = 198.0

    # Blue paint appears brighter than warm paint at equal HLS lightness on
    # the PSP LCD.  The small reduction keeps the main facets close to the
    # #3D78B6 Midnight token while retaining the authored highlight steps.
    target_lightness = max(0.10, min(0.88, lightness * 0.90))
    target_saturation = min(0.92, max(0.52, saturation * 0.88))
    recolored = colorsys.hls_to_rgb(
        target_hue / 360.0, target_lightness, target_saturation)
    return tuple(int(channel * 255.0 + 0.5) for channel in recolored)


def recolor(source, destination):
    width, height, channels, pixels = read_png(source)
    output = bytearray(pixels)
    cache = {}
    for index in range(width * height):
        at = index * channels
        key = bytes(pixels[at:at + 3])
        mapped = cache.get(key)
        if mapped is None:
            mapped = bytes(midnight_pixel(key[0], key[1], key[2]))
            cache[key] = mapped
        output[at:at + 3] = mapped
    write_png(destination, width, height, channels, output)
    return width, height


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    size = recolor(sys.argv[1], sys.argv[2])
    print("recolored %s -> %s (%dx%d)" % (sys.argv[1], sys.argv[2], *size))
