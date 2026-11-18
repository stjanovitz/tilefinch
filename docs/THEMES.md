# Chrome themes

This page is for PSP owners who want to change Tilefinch's colors, and for
anyone making a theme file. **Menu → Settings → Appearance → Theme** controls
Tilefinch's own chrome: native Home, menus, the Library, diagnostics, and the
native media player. It is separate from **Night mode**, which affects
web-page colors.

Midnight is the default dark-blue theme. Cobalt, Slate, and the original warm
Ember palette remain built in, and Daylight provides complementary light
blue-grey chrome. Built-in themes require no storage read. The launcher and
first visible browser frame always use Midnight so an optional file cannot
delay or break startup.

## Downloaded themes

Custom themes change colors only. Tilefinch intentionally does not expose
font sizes, spacing, radii, animation timings, or control geometry: those are
part of its fixed 480×272 layout and input contract.

[Open the interactive theme designer](theme-designer.html) to adjust every
field with a color picker, preview it across Home, menus, Reader mode, the
native player, and diagnostics, then export a ready-to-copy `.tfth` file. If
you are reading this on GitHub, download the HTML file and open it locally to
use the interactive controls.

Copy `.tfth` files into Tilefinch's shared `data/themes/` directory. Open
**Menu → Settings → Appearance → Theme**, press Cross, and choose one from the
list. The first-install archive includes Forest and Violet as examples; files
downloaded from other people use the same format and can be copied alongside
them.

Tilefinch scans the folder only while this chooser is open, visits at most 64
directory entries, and lists at most 12 valid themes in alphabetical order.
The selected basename is saved in the profile. Later boots read only that one
file; an ordinary Midnight boot neither scans the directory nor opens a theme
file. A missing or invalid selected file falls back to Midnight without
blocking native Home. A Custom theme selection that names no file in
`data/themes/` reads the single file `data/theme.tfth` instead.

The format is line-oriented UTF-8/ASCII. The header and numeric `format`
version are mandatory; version 2 is the current schema, and Tilefinch still
reads version 1 files unchanged. A newer Tilefinch can therefore reject a
future incompatible theme cleanly instead of guessing. Every color is
`#RRGGBB`. Omitted fields inherit Midnight, which makes a small accent-only
theme valid. The designer snaps every color to the exact RGB565 value the PSP
will display; hand-authored 24-bit colors are accepted and reduced once when
loaded.

```text
tilefinch-theme
format=2
name=Harbor Blue
ground=#10151D
panel=#151D28
text=#F5F8FC
accent=#5F9DDB
accent_high=#91BDEA
danger=#F0707A
```

`name` is an optional printable 31-character display label. Without it, the
chooser derives a label from the filename. The available color fields are:

| Group | Fields |
|---|---|
| Grounds | `ground`, `ground_reader`, `chrome_bar`, `hint_bar`, `panel` |
| Surfaces | `surface`, `surface_focus`, `line` |
| Text ramp | `text`, `text_body`, `text_muted`, `text_faint` |
| Accent | `accent`, `accent_high`, `on_accent` |
| Semantic | `ok`, `warn`, `danger` |

`on_accent` is the ink on an accent fill: the highlighted menu row,
buttons, and the selected keyboard key. Give it at least 4.5:1 contrast
against both `accent` and `accent_high`; the built-in themes and the example
files meet that. The designer's suggested palettes use near-black or white,
whichever reads better on both, and move the accent slightly lighter or
darker when neither reaches 4.5:1. An accent-only file keeps Midnight's dark
`on_accent`, so give it an accent light enough for dark text.

`warn` marks warnings, such as a low battery or a certificate problem.
`danger` marks actions that delete or reset something: the Clear rows in
**Menu → Settings → Device & storage → Site data & storage**, a site's
**Delete its data**, **Clear data for this site** and **Reset permissions** in
**Menu → Page tools → Site information**, **Clear playback positions** in
**Menu → Settings → Video**, a Library item waiting for its second delete
press, and the warning lines when removing homebrew. It is drawn on the
`panel` and, for a focused row, on `surface_focus`; give it at least 4.5:1
contrast against both. The built-in values meet that: the dark themes use
`#EF6B75` and Daylight `#B3261E`.

Version 2 added `danger`. A version 1 file, or a version 2 file that leaves
it out, gets a derived red instead of Midnight's: the first of a short ramp
of reds that reaches 4.5:1 against the file's own `panel` and
`surface_focus`, starting from the dark themes' red on a dark panel and from
Daylight's on a light one. A file whose surfaces copy a built-in theme
therefore gets that theme's danger. When no red reaches 4.5:1 (mid-tone or
mismatched surfaces), the most legible of those reds and the theme's own
`text` is used. A version 1 file that names `danger` is rejected as an
unknown field, as earlier Tilefinch releases reject it; change its `format`
to 2. Earlier releases reject every version 2 file, so keep a version 1 copy
for them.

A theme file may be at most 2 KiB, 24 lines, and 95 bytes per line. Tilefinch
applies it only after the whole file checks out: unknown versions or fields,
duplicate names or fields, malformed colors, and over-limit input reject the
new palette rather than partly applying it.
