#!/usr/bin/env python3
"""Browser chrome takes its colors from the selected theme.

The runtime palette (psp_ui_theme_active_palette) is the only source of
chrome color. Three ways of bypassing it have shipped before, and each one
painted the same color under every theme:

- a built-in accent source token (PSP_THEME_ACCENT_EMBER and its siblings)
  used directly instead of the selected accent; src/psp_ui_theme.h defines
  them as compile-time constants, so outside the palette tables they are the
  fixed Ember orange (or another fixed hue);
- the Ember hex itself, written out;
- a literal rgb565(r, g, b) in the painter that should have been a token.

The destructive rows (Clear, Delete, Remove, Reset) used to be the last of
these: a fixed red (239, 107, 117) at about 2.6:1 on Daylight. They now use
the palette's danger color, and that red is only the dark themes' danger
source value in the token sheet.

The painter keeps a few literal colors on purpose. Each is listed below with
its reason; a new one fails this test until it is either routed through the
theme or added here with a reason.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 \
    and not sys.argv[1].startswith("-") else Path(__file__).resolve().parents[1]

BUILTIN_ACCENT = re.compile(
    r"\bPSP_THEME_ACCENT_(?:MIDNIGHT|EMBER|COBALT|SLATE)(?:_HI)?\b")

# Where a built-in accent source token may be named.
ACCENT_TOKEN_HOMES = {
    # The definitions.
    "src/psp_ui_theme.h",
    # The built-in palette tables.
    "src/psp_ui_theme.c",
    # The launcher runs before any profile is read, so it always draws the
    # default (Midnight) accent.
    "src/update_launcher_psp.c",
}

EMBER_HEX = re.compile(
    r"(?i)#?\bE8823C\b|#?\bF4A56F\b"
    r"|0xE8\s*,\s*0x82\s*,\s*0x3C|0xF4\s*,\s*0xA5\s*,\s*0x6F"
    r"|\b232\s*,\s*130\s*,\s*60\b|\b244\s*,\s*165\s*,\s*111\b")

# The dark themes' danger source value, in every spelling it has had.
DANGER_SOURCE = re.compile(
    r"(?i)#?\bEF6B75\b|0xEF\s*,\s*0x6B\s*,\s*0x75"
    r"|\b239\s*,\s*107\s*,\s*117\b")

LITERAL_COLOR = re.compile(r"\brgb565\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)")

# Literal colors src/psp_ui.c keeps on purpose, with the reason.
PAINTER_FIXED_COLORS = {
    # Drop shadows under panels and toasts, and the caption shadow and the
    # video badge's outer fringe: darkening, not a hue.
    (0, 0, 0): "shadow",
    # The microphone indicator's ring and glyph, and the Diagnostic QR's
    # light modules and the startup skeleton's cards.
    (255, 255, 255): "white on the red mark / QR modules / page skeleton",
    # The microphone-on mark is red in every theme: a privacy signal.
    (214, 32, 40): "microphone indicator",
    # The video play/pause badge and Buffering pill sit on moving video,
    # not on a theme ground, so they keep one legible grey pair.
    (37, 40, 43): "video badge ground",
    (228, 230, 232): "video badge glyph",
    # The startup homepage skeleton stands in for the page's own light
    # content before any profile, font or page is loaded.
    (247, 247, 248): "startup skeleton page",
    (215, 219, 223): "startup skeleton card border",
    (16, 20, 24): "startup skeleton heading",
    (92, 102, 112): "startup skeleton muted text",
}


def source_files():
    for folder in ("src", "include"):
        for path in sorted((ROOT / folder).rglob("*")):
            # Generated tables carry no chrome colors and are large.
            if "generated" in path.relative_to(ROOT).parts:
                continue
            if path.suffix in {".c", ".h", ".inc"} and path.is_file():
                yield path


def without_comments(text):
    """Blank C comments, keeping line numbers."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


class ThemeColorTests(unittest.TestCase):
    def test_builtin_accents_stay_in_the_palette_tables(self):
        misses = []
        for path in source_files():
            relative = path.relative_to(ROOT).as_posix()
            if relative in ACCENT_TOKEN_HOMES:
                continue
            text = without_comments(path.read_text(errors="replace"))
            for number, line in enumerate(text.splitlines(), 1):
                # src/psp_ui.c undefines them so any use fails to compile.
                if line.lstrip().startswith("#undef"):
                    continue
                if BUILTIN_ACCENT.search(line):
                    misses.append(f"{relative}:{number}: {line.strip()}")
        self.assertEqual(misses, [], "use the selected palette's accent, "
                         "not a built-in accent source token")

    def test_ember_hex_lives_only_in_the_token_sheet(self):
        misses = []
        for path in source_files():
            relative = path.relative_to(ROOT).as_posix()
            if relative == "src/psp_ui_theme.h":
                continue
            text = without_comments(path.read_text(errors="replace"))
            for number, line in enumerate(text.splitlines(), 1):
                if EMBER_HEX.search(line):
                    misses.append(f"{relative}:{number}: {line.strip()}")
        self.assertEqual(misses, [])

    def test_danger_red_lives_only_in_the_token_sheet(self):
        misses = []
        for path in source_files():
            relative = path.relative_to(ROOT).as_posix()
            if relative == "src/psp_ui_theme.h":
                continue
            text = without_comments(path.read_text(errors="replace"))
            for number, line in enumerate(text.splitlines(), 1):
                if DANGER_SOURCE.search(line):
                    misses.append(f"{relative}:{number}: {line.strip()}")
        self.assertEqual(misses, [], "destructive rows use the palette's "
                         "danger color (PSP_THEME_DANGER in src/psp_ui.c)")

    def test_painter_literal_colors_are_the_listed_ones(self):
        path = ROOT / "src" / "psp_ui.c"
        text = without_comments(path.read_text())
        misses = []
        for number, line in enumerate(text.splitlines(), 1):
            for match in LITERAL_COLOR.finditer(line):
                color = tuple(int(part) for part in match.groups())
                if color not in PAINTER_FIXED_COLORS:
                    misses.append(f"src/psp_ui.c:{number}: rgb565{color}")
        self.assertEqual(misses, [], "route the color through the theme "
                         "palette, or list it here with its reason")

    def test_the_allow_list_has_no_stale_entries(self):
        text = without_comments((ROOT / "src" / "psp_ui.c").read_text())
        used = {tuple(int(part) for part in match.groups())
                for match in LITERAL_COLOR.finditer(text)}
        self.assertEqual(sorted(set(PAINTER_FIXED_COLORS) - used), [])

    def test_example_themes_ink_reads_on_the_accent(self):
        """The example theme files and the designer's presets give on_accent
        4.5:1 on accent and accent_high, as the built-in themes do
        (test_builtin_on_accent_reads in tests/test_psp_ui.c)."""
        pairs = []
        for path in sorted((ROOT / "psp-assets" / "themes").glob("*.tfth")):
            fields = dict(line.split("=", 1) for line in
                          path.read_text().splitlines() if "=" in line)
            pairs.append((path.name, fields["accent"],
                          fields["accent_high"], fields["on_accent"]))
        designer = (ROOT / "docs" / "theme-designer.html").read_text()
        presets = re.findall(
            r'(\w+): \{[^}]*?accent:"(#\w{6})", accent_high:"(#\w{6})", '
            r'on_accent:"(#\w{6})"', designer)
        self.assertGreaterEqual(len(presets), 4)
        pairs += presets
        misses = []
        for name, accent, high, ink in pairs:
            for fill in (accent, high):
                ratio = contrast(ink, fill)
                if ratio < 4.5:
                    misses.append(f"{name}: {ink} on {fill} {ratio:.2f}:1")
        self.assertEqual(misses, [])


def rgb565_luminance(color):
    """WCAG luminance of #RRGGBB after the PSP's RGB565 reduction, with the
    expansion identities of include/tilefinch/pixel_math.h."""
    red, green, blue = (int(color[at:at + 2], 16) for at in (1, 3, 5))
    channels = ((red >> 3) * 1053 >> 7, ((green >> 2) * 259 + 3) >> 6,
                (blue >> 3) * 1053 >> 7)
    linear = [value / 255 / 12.92 if value / 255 <= 0.04045
              else ((value / 255 + 0.055) / 1.055) ** 2.4
              for value in channels]
    return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


def contrast(first, second):
    a = rgb565_luminance(first) + 0.05
    b = rgb565_luminance(second) + 0.05
    return max(a, b) / min(a, b)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + [
        argument for argument in sys.argv[1:] if argument.startswith("-")])
