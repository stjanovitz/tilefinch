# Chrome themes

**Settings → Appearance → Theme** controls Tilefinch's own chrome: native
Home, menus, Collections, diagnostics, and the native media player. It is
separate from **Night mode**, which affects web-page colors.

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
**Settings → Appearance → Theme**, press **X**, and choose one from the bounded
list. The first-install archive includes Forest and Violet as examples; files
downloaded from other people use the same format and can be copied alongside
them.

Tilefinch scans the folder only while this chooser is open, visits at most 64
directory entries, and lists at most 12 valid themes in alphabetical order.
The selected basename is saved in the profile. Later boots read only that one
file; an ordinary Midnight boot neither scans the directory nor opens a theme
file. A missing or invalid selected file falls back to Midnight without
blocking native Home. The former single-file `data/theme.tfth` location is
still accepted for profiles that selected Custom before the theme library was
introduced.

The format is line-oriented UTF-8/ASCII. The header and numeric `format`
version are mandatory; version 1 is the current schema. A newer Tilefinch can
therefore reject a future incompatible theme cleanly instead of guessing. Every
color is `#RRGGBB`. Omitted fields inherit Midnight, which makes a small
accent-only theme valid. The designer snaps every color to the exact RGB565
value the PSP will display; hand-authored 24-bit colors are accepted and
reduced once when loaded.

```text
tilefinch-theme
format=1
name=Harbor Blue
ground=#10151D
panel=#151D28
text=#F5F8FC
accent=#5F9DDB
accent_high=#91BDEA
```

`name` is an optional printable 31-character display label. Without it, the
chooser derives a label from the filename. The available color fields are:

| Group | Fields |
|---|---|
| Grounds | `ground`, `ground_reader`, `chrome_bar`, `hint_bar`, `panel` |
| Surfaces | `surface`, `surface_focus`, `line` |
| Text ramp | `text`, `text_body`, `text_muted`, `text_faint` |
| Accent | `accent`, `accent_high`, `on_accent` |
| Semantic | `ok`, `warn` |

Parsing of each candidate is bounded to 2 KiB, 24 lines, and 95 bytes per line,
uses no page-proportional allocation, and commits the palette only after the
complete file validates. Unknown versions or fields, duplicate names or fields,
malformed colors, and over-limit input reject the new palette rather than
partially applying it.
