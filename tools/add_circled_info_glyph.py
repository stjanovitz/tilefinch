#!/usr/bin/env python3
"""Add CIRCLED LATIN SMALL LETTER I (U+24D8, "ⓘ") to Tilefinch's fonts.

The bundled Latin subsets lack the character, yet sites use it as their
"more information" button (for example Internet Archive's result tiles), so
it rendered as a box. The glyph is built from each face's own "i": a ring
whose stroke follows the face's stem weight, around a scaled copy of that
face's "i". Oblique faces get a ring slanted by the face's italic angle.

Usage: tools/add_circled_info_glyph.py fonts/*.ttf
A face that already maps U+24D8 is left unchanged. Requires fontTools.
"""

from __future__ import annotations

import argparse
import math
import sys

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

CODEPOINT = 0x24D8
GLYPH_NAME = "uni24D8"


def bounds(glyph_set, name):
    pen = BoundsPen(glyph_set)
    glyph_set[name].draw(pen)
    return pen.bounds


class _CrossingPen(BasePen):
    """Collects where the outline crosses the horizontal line y."""

    def __init__(self, glyph_set, y):
        super().__init__(glyph_set)
        self.y = y
        self.crossings = []
        self.last = None
        self.start = None

    def _segment(self, end):
        (x0, y0), (x1, y1) = self.last, end
        if (y0 <= self.y < y1) or (y1 <= self.y < y0):
            self.crossings.append(x0 + (self.y - y0) * (x1 - x0) / (y1 - y0))
        self.last = end

    def _moveTo(self, point):
        self.last = point
        self.start = point

    def _closePath(self):
        if self.last != self.start:
            self._segment(self.start)

    _endPath = _closePath

    def _lineTo(self, point):
        self._segment(point)

    def _qCurveToOne(self, control, point):
        start = self.last
        for k in range(1, 17):
            t = k / 16.0
            x = ((1 - t) ** 2 * start[0] + 2 * (1 - t) * t * control[0]
                 + t * t * point[0])
            y = ((1 - t) ** 2 * start[1] + 2 * (1 - t) * t * control[1]
                 + t * t * point[1])
            self._segment((x, y))

    def _curveToOne(self, c1, c2, point):
        start = self.last
        for k in range(1, 17):
            t = k / 16.0
            u = 1 - t
            x = (u ** 3 * start[0] + 3 * u * u * t * c1[0]
                 + 3 * u * t * t * c2[0] + t ** 3 * point[0])
            y = (u ** 3 * start[1] + 3 * u * u * t * c1[1]
                 + 3 * u * t * t * c2[1] + t ** 3 * point[1])
            self._segment((x, y))


def stem_width(glyph_set, name, y):
    """Horizontal thickness of the glyph's stem at height y, along the
    slant-free line: the widest pair of crossings."""
    pen = _CrossingPen(glyph_set, y)
    glyph_set[name].draw(pen)
    xs = sorted(pen.crossings)
    return max((xs[k + 1] - xs[k] for k in range(0, len(xs) - 1, 2)),
               default=0.0)


def draw_circle(pen, cx, cy, radius, clockwise, skew):
    """A closed circle of eight quadratic segments; skew slants x by y."""
    steps = 8
    control = radius / math.cos(math.pi / steps)

    def point(angle, r):
        x = cx + r * math.cos(angle)
        y = cy + r * math.sin(angle)
        return (round(x + (y - cy) * skew), round(y))

    sign = -1.0 if clockwise else 1.0
    step = sign * 2.0 * math.pi / steps
    pen.moveTo(point(0.0, radius))
    for k in range(steps):
        middle = (k + 0.5) * step
        end = (k + 1) * step
        if k == steps - 1:
            pen.qCurveTo(point(middle, control), point(0.0, radius))
        else:
            pen.qCurveTo(point(middle, control), point(end, radius))
    pen.closePath()


def add_glyph(path: str) -> bool:
    # Without recalculation, every existing glyph keeps its original bytes
    # and hhea/head/maxp keep their values: only the new glyph is encoded.
    font = TTFont(path, recalcBBoxes=False, recalcTimestamp=False)
    cmap = font.getBestCmap()
    if CODEPOINT in cmap:
        return False
    upm = font["head"].unitsPerEm
    glyph_set = font.getGlyphSet()
    i_name = cmap[ord("i")]
    h_name = cmap[ord("H")]
    i_box = bounds(glyph_set, i_name)
    h_box = bounds(glyph_set, h_name)
    cap_height = h_box[3]
    skew = math.tan(math.radians(-font["post"].italicAngle))
    # Measure the stem halfway up the x-height, clear of serifs and the dot.
    x_height = bounds(glyph_set, cmap[ord("x")])[3]
    stem = stem_width(glyph_set, i_name, 0.5 * x_height)

    # The ring sits on the cap height's centre and reaches a little past
    # the baseline and cap height, like other enclosed letters.
    outer = 0.58 * cap_height
    ring = max(0.55 * stem, 0.04 * upm)
    inner = outer - ring
    bearing = round(0.06 * upm)
    cx = bearing + outer + abs(skew) * outer
    cy = cap_height / 2.0

    pen = TTGlyphPen(glyph_set)
    draw_circle(pen, cx, cy, outer, True, skew)
    draw_circle(pen, cx, cy, inner, False, skew)
    i_height = i_box[3] - i_box[1]
    scale = (1.16 * inner) / i_height
    i_cx = (i_box[0] + i_box[2]) / 2.0
    i_cy = (i_box[1] + i_box[3]) / 2.0
    dx = cx - i_cx * scale
    dy = cy - i_cy * scale
    glyph_set[i_name].draw(TransformPen(pen, (scale, 0, 0, scale, dx, dy)))
    glyph = pen.glyph()

    order = font.getGlyphOrder() + [GLYPH_NAME]
    font.setGlyphOrder(order)
    font["glyf"].glyphs[GLYPH_NAME] = glyph
    glyph.recalcBounds(font["glyf"])
    advance = round(cx + outer + abs(skew) * outer + bearing)
    font["hmtx"].metrics[GLYPH_NAME] = (advance, glyph.xMin)
    maxp = font["maxp"]
    maxp.numGlyphs = len(order)
    maxp.maxPoints = max(maxp.maxPoints, len(glyph.coordinates))
    maxp.maxContours = max(maxp.maxContours, glyph.numberOfContours)
    font["hhea"].advanceWidthMax = max(font["hhea"].advanceWidthMax, advance)
    for table in font["cmap"].tables:
        if table.isUnicode():
            table.cmap[CODEPOINT] = GLYPH_NAME
    font.save(path)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("fonts", nargs="+")
    arguments = parser.parse_args()
    for path in arguments.fonts:
        changed = add_glyph(path)
        print(f"{path}: {'added' if changed else 'already present'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
