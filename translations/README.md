# Native interface translations

This page is for contributors who edit or publish Tilefinch's interface
translations. PSP owners install a language from **Menu → Settings →
Appearance → Language & emoji → Interface language**; the
[user guide](../docs/USER_GUIDE.md#other-languages-and-language-packs)
explains how.

The catalog offers Spanish, French, German, Japanese, Russian, Ukrainian,
Simplified Chinese, Korean, Hindi, Arabic, Portuguese, Bengali and Assamese
besides the built-in English. Each non-English language is a small,
data-only translation file downloaded from
[tilefinch-models](https://github.com/stjanovitz/tilefinch-models). Japanese,
Chinese and Korean also install their signed glyph packs; Russian and
Ukrainian install the Cyrillic pack, Hindi installs Devanagari, Arabic uses
the Arabic pack, and Bengali and Assamese share the Bengali–Assamese pack.
Spanish, French, German and Portuguese use the built-in font. Page fonts,
preferred video languages and HTTP language preferences are independent.

Common native navigation, settings labels, controls and status messages are
translated; advanced diagnostics and messages without a catalog entry stay
in English. Missing or damaged translation or font files fall back to
English without blocking startup. Arabic labels are contextually shaped and
ordered right-to-left once when the catalog loads; ASCII hints and digits
keep their order, and the browser's panel geometry is not mirrored. Hindi,
Bengali and Assamese labels use pre-shaped syllable clusters from their
packs, which cover every current interface label and common syllables, not
all possible complex-script shaping. There is no Hebrew interface
translation.

## Editing and publishing

The current catalog version is **v15**. All thirteen downloadable languages
use it, and `src/generated/ui_languages.inc` pins each language's file name,
SHA-256 and the signed glyph-pack revision its shaped labels need (Devanagari
revision 8 for Hindi, Bengali–Assamese revision 4 for Bengali and Assamese).

Edit the active catalog's `translations/ui/v15/source.tsv`, then run
`python3 tools/generate_ui_translations.py`.
Commit the authored sources, generated Hindi and Bengali–Assamese sequences
and `src/generated/ui_languages.inc` here, not the `.tful` downloads. Each
language spec carries a versioned `resource_key` relative to
tilefinch-models' `main` branch, together with the expected SHA-256 and
required glyph revision. Export the exact download bytes with:

```sh
python3 tools/generate_ui_translations.py --resources-only --resource-dir build-preset-release/ui-resources
```

Publish the exported `translations/ui/vN/*.tful` files only to
tilefinch-models. Sources and generation tools remain in the browser
repository. Host builds generate test resources in their build directory;
public-clone tests need no network or optional-download checkout. The
generated-file test checks their exact identity. The browser accepts only the
SHA-256-pinned bytes, at most 64 KiB and 2,048 rows per file. Values are
display text, not executable code or formatting programs.

**Published versions are immutable.** Released browsers download the exact
bytes they pin, so a file under a published `translations/ui/vN/` directory
is never changed or removed. A change to any translation, a new key, or a new
language needs a new catalog version, with matching browser catalog entries,
and its files (and any glyph-pack revision it requires) must be published to
tilefinch-models before a device can download them. Local-only changes cannot
reach a device. A new version must keep every key of the previous one except
those listed in the generator's `RETIRED_KEYS`, so every language still
translates everything the browser looks up.

### File format

A `.tful` file (`TFUI2`, described in `src/ui_language.c`) stores no English
keys. After its `TFUI2` header line come the big-endian row count, each
row's 32-bit FNV-1a hash of its English key's UTF-8 bytes, ASCII letters
lowercased, in increasing order, each key's byte length, then the values in
that order, one per line.
A lookup hashes the English string the same way, and a row is used only
when the key length also agrees, so an unrelated string whose hash happens
to collide stays English. The generator refuses a version whose keys
collide.

### Files on the Memory Stick

Each installed language keeps its current-version file
(`data/ui-language/<code>-vN.tful`) and, only while an install is writing,
one temporary file. Only explicit installation writes them. A successful
install also removes that language's files from older catalog versions,
which this build never reads (exact `<code>-vN.tful` names with a lower N,
regular files only, never through a symlink); other languages' files and
any other names stay. Startup reads just the selected file; painting
performs in-memory lookups, with no filesystem access.

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

The test loads the production Latin fonts and a synthetic pack containing
every translated display codepoint and Hindi cluster with the production 16×16
mono-pack metrics. This checks worst-case glyph extents without relying on
narrow missing-character boxes; missing glyphs fail the test. It checks
geometry, not translation quality or glyph legibility. Actual downloaded packs
still need visual device review. The test is part of the ordinary host suite
and needs no network or PSP.

`hi.sequences` and `bengali-assamese.sequences` are generated from the
active catalog's Hindi, Bengali and Assamese labels plus a common-syllable
inventory. The optional-components producer uses them with HarfBuzz/FreeType
offline shaping (`--shaped-mono`), so painting needs no runtime shaping
engine. When a new catalog adds labels, rebuild the Devanagari and
Bengali–Assamese packs from that version's sequence files, raise the
required glyph revision, and publish the packs before shipping a browser that
pins the catalog: older packs may not contain the new labels' syllable
clusters. Selecting such a language updates an older pack before reporting
installation complete.

### What the catalog covers

Status notices (toasts) are catalog lines too.
`tilefinch-ui-toast-catalog-tests` fails when a literal status has no key or
when the catalog keeps a key nothing looks up. A notice is translated a line
at a time, and a trailing progress count (" 45%") is kept after the translated
text, so composed notices are whole catalog lines: a pack's name above its
state, a message above its Settings path. A button shows its glyph beside a
shared word ("X" and the catalog's "Retry") rather than a key of its own.
Statuses built from runtime text (engine and download errors, names, counts, a
version or a release's notes) stay English, as do the version string, the
screen size and clock, and "MIT". On Site information the site name, the TLS
version and a server's certificate issuer stay as sent. Filenames, application
names, paths and links are never translated. Generated HTML uses logical
translations and escapes them; native chrome uses the pre-shaped display
labels. A new key should reuse an existing word where one fits, since the 64
KiB file limit binds before the row limit for the longest languages.
