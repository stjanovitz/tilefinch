# Native interface translations

Settings → Appearance → Language & emoji → Interface language offers English,
Spanish, French, German, Japanese, Russian, Ukrainian, Simplified Chinese and
Korean, Hindi and Arabic. Left/Right selects; X installs. A second X during installation cancels.
Restart to apply the saved selection.

Non-English selections download a small data-only translation file from
[tilefinch-models](https://github.com/stjanovitz/tilefinch-models).
Japanese, Chinese and Korean also install their existing signed
glyph packs; Russian and Ukrainian install the Cyrillic pack, Hindi installs
Devanagari, and Arabic uses the Arabic pack. Spanish, French
and German use the built-in font. Page fonts, preferred video languages and
HTTP language preferences remain independent.

Common native navigation, settings labels and controls are translated. Advanced
diagnostics and messages not yet translated remain in English. Missing or
damaged translation/font files fall back to English without blocking startup.
Arabic labels are contextually shaped and ordered right-to-left once when the
catalog loads; ASCII hints and digits retain their order. This does not mirror
the browser's panel geometry. Hindi consumes pre-shaped syllable clusters from
the Devanagari pack. Its bounded inventory covers every current interface label
and common Hindi syllables, not all possible complex-script shaping. Hebrew
interface translation is not yet offered.

## Editing and publishing

Edit the unpublished catalog's `source.tsv`, then run
`python3 tools/generate_ui_translations.py`.
Commit the authored sources, generated Hindi sequences and
`src/generated/ui_languages.inc` here, not the `.tful` downloads. Each language
spec carries a versioned `resource_key` relative to tilefinch-models' `main`
branch, together with the expected SHA-256 and required glyph revision.
Export the exact download bytes with:

```sh
python3 tools/generate_ui_translations.py --resources-only --resource-dir build-preset-release/ui-resources
```

Publish the exported `translations/ui/vN/*.tful` files only to tilefinch-models.
Sources and generation tools remain in the browser repository. Host builds
generate test resources in their build directory; public-clone tests need no
network or optional-download checkout. The generated-file test checks their
exact identity. The browser accepts only
the SHA-256-pinned bytes, with a 32 KiB / 320-row limit. Values are display text,
not executable code or formatting programs.

Downloads use tilefinch-models' `main` branch and versioned
`translations/ui/vN/` path.
The original languages remain at v1; Russian, Ukrainian, Chinese and Korean
use v2; Hindi and Arabic use v3. The current catalog uses v5 for all ten
downloadable languages, adding YouTube provider controls and notices to the
navigation instructions introduced in v4. V1–v4
remain immutable for existing releases. The file
format is unchanged, and the browser catalog pins each language's version.
They become available when those files are published; local-only changes cannot
be downloaded by a device. Existing releases reject changed files rather than
loading incompatible translations. Released browsers use the models repository;
keep its published version directories immutable.
New translations require a new version and matching browser catalog/URLs.

One active file and one temporary file per language live in `data/ui-language/`.
Only explicit installation writes them. Startup reads just the selected file;
painting performs bounded in-memory lookups, with no filesystem access.

## Layout regression tests

`tilefinch-ui-language-layout-tests` exercises the real native painter at
480×272 in every interface language, in light and dark themes and both menu
text sizes. It covers settings, language choices, installation/cancellation
states, themes, tabs, page tools, site controls and help. Every translated
label must be complete, stay onscreen, and have at least five pixels of space
from adjacent text. Frame and row-padding canaries catch out-of-bounds writes.
It also renders the YouTube provider's signed-out and signed-in home pages,
Account panel and video actions in every language, checking text extents and
the reserved widths of all feed and rating notices. Author video titles are
kept unchanged, even when they coincide with a translated control key.

The test loads the production Latin fonts and a synthetic pack containing every
translated display codepoint and Hindi cluster with the production 16×16
mono-pack metrics. This checks
worst-case glyph extents without relying on narrow missing-character boxes;
missing glyphs fail the test. It checks geometry, not translation quality or
glyph legibility. Actual downloaded packs still need visual device review.
The test is part of the ordinary host suite and needs no network or PSP.

`hi.sequences` is generated from the Hindi labels plus a bounded common-syllable
inventory. The optional-components producer uses it with HarfBuzz/FreeType
offline shaping (`--shaped-mono`), so painting needs no runtime shaping engine.
The Hindi pack must be built, signed and published through the normal optional
component process before a released browser can download it. Translation
files likewise become downloadable only when their versioned paths are public.
For a new catalog, rebuild the Hindi pack from the active version's
`hi.sequences` before publishing the catalog: older packs may not contain the
new navigation instructions' syllable clusters.
The v5 Hindi catalog requires signed Devanagari revision 3; selecting Hindi
updates an older pack before reporting installation complete.
