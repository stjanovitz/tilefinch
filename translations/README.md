# Native interface translations

Settings → Appearance → Language & emoji → Interface language offers English,
Spanish, French, German, Japanese, Russian, Ukrainian, Simplified Chinese and
Korean, Hindi, Arabic, Portuguese, Bengali and Assamese. Left/Right selects;
X installs. A second
X during installation cancels.
Restart to apply the saved selection.

Non-English selections download a small data-only translation file from
[tilefinch-models](https://github.com/stjanovitz/tilefinch-models).
Japanese, Chinese and Korean also install their existing signed
glyph packs; Russian and Ukrainian install the Cyrillic pack, Hindi installs
Devanagari, Arabic uses the Arabic pack, and Bengali and Assamese share the
Bengali-Assamese pack. Spanish, French, German and Portuguese use the built-in font. Page fonts, preferred video
languages and HTTP language preferences remain independent.

Common native navigation, settings labels, controls and status messages are
translated. Advanced diagnostics and messages not yet translated remain in
English. Missing or
damaged translation/font files fall back to English without blocking startup.
Once per boot, when Home or a page is idle, an English notice then names the
language and what it needs (its interface text, or a font pack update); X
opens Interface language on that language, where one more X downloads it,
and O dismisses the notice until the next boot.
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
the SHA-256-pinned bytes, with a 64 KiB / 2,048-row limit (v1–v11 files stay
within the earlier 32 KiB / 400 rows). Values are display text,
not executable code or formatting programs.

V1–v11 files are text: a `TFUI1` header line, then one `English\tvalue` row
per line. From v12 a file stores no English keys (`TFUI2`, described in
`src/ui_language.c`): after its header line come the big-endian row count,
each row's 32-bit FNV-1a hash of its ASCII-lowercased English key in
increasing order, each key's byte length, then the values in that order, one
per line. A lookup hashes the English string the same way, so it matches
exactly what the earlier case-insensitive key search matched, and a row is
used only when the key length also agrees, so an unrelated string whose hash
happens to collide stays English. The generator refuses a version whose keys
collide.

Downloads use tilefinch-models' `main` branch and versioned
`translations/ui/vN/` path.
The original languages remain at v1; Russian, Ukrainian, Chinese and Korean
use v2; Hindi and Arabic use v3. The current catalog retains v7 for those ten
downloadable languages, including native player controls, and adds Portuguese
in v8. Portuguese uses European Portuguese wording and the built-in Latin
font; it needs no additional glyph pack. V9 adds Bengali and Assamese, using a
shared bounded pre-shaped inventory for both interfaces. V1–v8 remain immutable
for existing releases. New translation files and required signed packs must be
published to tilefinch-models before their in-app downloads are available. The file
format is unchanged, and the browser catalog pins each language's version.
They become available when those files are published; local-only changes cannot
be downloaded by a device. Existing releases reject changed files rather than
loading incompatible translations. Released browsers use the models repository;
keep its published version directories immutable.
New translations require a new version and matching browser catalog/URLs.

Each installed language keeps its current-version file
(`data/ui-language/<code>-vN.tful`) and, only while an install is writing,
one temporary file. Only explicit installation writes them. A successful
install also removes that language's files from older catalog versions,
which this build never reads (exact `<code>-vN.tful` names with a lower N,
regular files only, never through a symlink); other languages' files and
any other names stay. Files an older build left for a language that is not
reinstalled stay until it is. Startup reads just the selected file;
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
The v7 Hindi catalog requires signed Devanagari revision 4; selecting Hindi
updates an older pack before reporting installation complete.

V11 adds file-download and homebrew-management controls and warnings in every
interface language. V1–v10 downloads and saved language IDs remain unchanged.
V11 requires Devanagari revision 6 and Bengali-Assamese revision 2 for its new
shaped labels. These resources must be built and published before shipping an
app that pins v11; generating the catalog does not publish the downloads.
The native layout gate covers Save, Install, Remove, Downloads, and the ZIP
deletion confirmation. Filenames, application names, paths and links remain
unchanged. Generated HTML uses logical translations and escapes them; native
chrome uses the pre-shaped display labels.

V12 adds the status notices (toasts). `tilefinch-ui-toast-catalog-tests`
fails when a literal status has no key or when the catalog keeps a key nothing
looks up. It sees literals passed to the status calls, returned by status
helpers (including the acknowledgement shown when an action starts), written
to a status buffer, set as a font-pack install's failure message, or routed
to any of these through a local variable. The same lookup translates text the
painter draws from state, and the test sees those sources too: a setting's
reloading, saved and reload-failed notices, a theme file's errors, the video
player's headline, the Update screen's status, notes and button (with its
download and install messages), the font-pack offer's confirmation box, and
the static tables and section headings of the Site data & storage screens.
The failed-video panel, the Help details, both Site data & storage
screens and Site information are translated too. On Site information the
site name, the TLS version and a server's certificate issuer stay as sent;
"RAM" and "KB" are the same in every language. A button shows its glyph beside a shared word
("X" and the catalog's "Retry") rather than a key of its own, and the Retry
chip grows to fit the word. Statuses built from runtime text (engine and
download errors, names, counts, a version or a release's notes) stay English,
as do the version string, the screen size and clock, and "MIT". A notice is translated a line
at a time, and a trailing progress count (" 45%") is kept after the
translated text, so composed notices are whole catalog lines: a pack's name
above its state, a message above its Settings path. V12 retires the keys
listed in `RETIRED_KEYS` of the generator. Its hashed format drops the 19 KB
of English keys every file used to repeat (Bengali, the longest, is 59,823
of the 65,535 bytes, with 1,030 rows). The runtime accepts 2,048 rows and
sizes a loaded catalog to the rows it has, so the byte cap binds first; a
new key should still reuse an existing word where one fits. Arabic's shaped
display copy has 16-bit offsets too and is 53,661 of its 65,535 bytes. V12 requires
Devanagari revision 7 and Bengali-Assamese revision 3, rebuilt from the v12
`hi.sequences` and `bengali-assamese.sequences`; publish those packs and the
v12 `.tful` files before shipping an app that pins v12.
