# Bidirectional page text

This page is for contributors and web developers who need to know how
Tilefinch handles right-to-left and complex-script text; PSP owners will
find the language packs themselves in the
[user guide](USER_GUIDE.md#other-languages-and-language-packs).

Tilefinch lays out mixed left-to-right and right-to-left page content without
rewriting DOM strings. Ordinary English and other LTR-only paragraphs use the
plain text path; a parser census and stylesheet summary keep them out of bidi
analysis entirely.

This contract covers page content. Arabic browser labels reuse the same
shaping and visual-order helpers once at translation load; they do not rewrite
the page DOM or mirror panel geometry. A general text-selection UI and general
complex-script shaping beyond the Arabic family are not part of this contract.

## Pipeline

An inline formatting context enters the bidi path only when visible text,
`dir`, `direction`, or `unicode-bidi` can affect ordering. The path is:

1. Flatten the inline paragraph into UTF-32 analysis units while recording
   source spans and representing atomic inline boxes with U+FFFC. The source
   DOM and UTF-8 remain in logical order.
2. Resolve paragraph levels with SheenBidi 3.0.0, a C implementation of the
   Unicode Bidirectional Algorithm. Embeddings, overrides, isolates,
   neutrals, paired punctuation, numbers, directional controls, `dir=auto`,
   and `unicode-bidi: plaintext` therefore share the same UAX #9 engine.
3. Apply the narrow Arabic-family shaper to Arabic, Persian, and Urdu joining
   forms. It consumes Unicode 17 joining/form tables and preserves one logical
   mapping unit per source codepoint. Hebrew uses UAX #9 ordering but no Arabic
   joining behavior.
4. Measure contextual glyphs before choosing line breaks. At each completed
   line, request UAX #9 line visual order and publish sparse shaped-glyph data
   beside the retained draw commands.
5. Paint and interaction consume that sidecar. Each visual glyph retains its
   logical UTF-8 offset, bidi level, and visual fixed-point x position;
   link/control boxes, atomic inline descendants, find highlights, focus hit
   testing, layout translation, scaling, and visual clones move with it.
   Allocation-free hit/visual-step helpers expose exact caret boundaries to a
   page-selection UI. JavaScript `Range`, selection text, clipboard writes,
   find indexing, and ordinary extraction still read the unchanged logical
   DOM order.

The shaped-glyph interface is intentionally narrower than HarfBuzz. It is
sufficient for Arabic-family contextual forms and leaves a compatible seam
for a future Indic shaper without linking a desktop shaping stack into the PSP
build.

## Bounds and degradation

| Resource | Bound | Failure behavior |
|---|---:|---|
| paragraph UTF-8 | 8,192 bytes | use readable logical-order layout |
| paragraph analysis units | 2,048 | use readable logical-order layout |
| Unicode paragraph segments in one inline flow | 32 | use readable logical-order layout |
| UAX #9 visual runs per line | 256 | keep that line in logical order |
| inline source spans | 512 | keep the formatting context in logical order |
| mapped text commands per inline flow | 1,024 | unmatched commands retain logical rendering |
| atomic inline boxes | 256 | keep the formatting context in logical order |
| retained shaped glyphs per layout | 65,536 | keep the affected line in logical order |
| retained mapped commands per layout | 16,384 | keep the affected line in logical order |
| SheenBidi scratch arena | 12 KiB | fail the analysis without partial state |

Paragraph analysis uses one page-budget allocation and a reusable aligned
scratch arena; it never allocates per codepoint. Temporary paragraph state is
released when its block flow completes. Only sparse visual glyph mappings
needed by the retained layout survive. Allocation refusal and hostile control
nesting fall back to logical order, never to reversing codepoints.

Text shadows are omitted for bidi-shaped commands.
That cosmetic degradation avoids duplicating shaped sidecars and never changes
text, links, or hit geometry.

## Glyph components

Arabic and Hebrew are optional signed language packs, alongside Japanese,
Simplified Chinese, Traditional Chinese, Korean, Cyrillic, Extended Latin,
Devanagari (Hindi) and Bengali–Assamese. The default embedded Latin and
fallback glyphs remain available through missing, damaged, interrupted, or
removed components. Selecting a language keeps that pack attached; the parser
may lazily attach at most two other installed language packs when the page
actually uses their scripts. Color emoji is independent, and no more than
four packs are attached at once.

The Arabic pack includes U+0600–U+08FF base/mark coverage and the contextual
presentation forms emitted by the shaper. The Hebrew pack includes Hebrew
letters, marks, and presentation forms. Both use the existing TFGF/TFGM signed
component format and cause no pack enumeration or payload read at boot.

Devanagari uses a fixed pre-shaped cluster inventory generated offline with
HarfBuzz/FreeType, consumed by longest-sequence matching in the existing pack
format. It covers current Hindi interface labels and common syllables, not
every possible Indic shaping sequence. No runtime shaping stack is linked. The
Bengali–Assamese pack uses the same sequence mechanism, including Assamese
ra/wa, shared Indic punctuation and complete interface clusters. Both
interface languages share a pack but retain separate catalog IDs. This does
not claim arbitrary runtime Bengali or Assamese shaping; the fixed inventory's
scalar fallback applies outside its coverage. There a pre-base vowel (U+093F,
U+09BF, U+09C7, U+09C8) still draws ahead of its consonants at the same total
width, while two-part Bengali vowels and lines of mixed direction keep stored
order. Monochrome pack cells are
resampled with an exact box filter at glyph creation, so small vowel marks and
thin strokes survive at small sizes and a stem keeps one weight wherever it
falls against the pixel grid; retained painting still uses the same cached glyph
bitmap.

A pack cell has no metrics of its own, so a pack built by fitting each glyph to
its cell draws lowercase as tall as capitals and shrinks its widest letters. That
suits CJK, whose glyphs fill the em, but not an alphabet. A metric pack instead
renders every glyph at one scale on a shared baseline, and declares that baseline
and each glyph's column box in a form older browsers ignore
(`include/tilefinch/glyph_component.h`). Its text is measured from the column
boxes, so letters keep their own widths without reading the pack during layout.
A combining mark in such a pack has no advance and its box ends at the pen, so it
draws back over the letter before it, which is the letter it follows in a
right-to-left cluster's stored order too. Marks are not positioned per letter:
the font's default placement is the only one a pack holds. That placement suits
a lowercase letter, so a pack can also list a capital and its accent (U+0300-036F)
as a shaped cluster, which draws the accent raised; a browser older than this
ignores those entries.

A pack stores one weight. Bold text reaches its monochrome glyphs through either
a synthetic bold or a real bold face, and both embolden the cell by one column
before it is scaled, so the extra weight stays in proportion to the size. A
metric glyph's advance gains that column, so layout and paint agree, and a color
glyph is drawn unchanged.

### In-page install offer

When a page uses a script that no installed pack covers, Tilefinch offers the
matching pack in a notice above the bottom bar. It never downloads on one
press: Cross opens a confirmation that shows the pack's verified download
size, and a second Cross installs it exactly as the menu does. The
[user guide](USER_GUIDE.md#other-languages-and-language-packs) describes the
notice, its buttons and its settings. The offer also appears over Reader mode
and Basic view, which draw with the same faces. When an install finishes, the
page is laid out again in place, because text measured against blank cells
has the wrong widths.

## Qualification

The focused host tests combine UAX #9-derived mixed-direction cases with
Arabic, Persian, Urdu, Hebrew, URLs, punctuation, digits, join controls,
over-depth controls, maximum admitted paragraphs, run-limit refusal, nested
HTML/CSS direction boundaries, wrapping, inline links and controls, hit tests,
visual caret stepping, logical selection/copying, and find geometry.
Signed-component proof opens the actual release packs and requires authored
pixels for Arabic presentation forms and Hebrew letters.

Performance qualification compares the same English encyclopedia-article
capture and a provider-results-shaped LTR fixture before and after a change.
Both must stay within normal timing variance and report zero admitted bidi
paragraphs. An RTL fixture records paragraph count, fallbacks, bidi
microseconds, and peak transient budget use. The PSP build separately gates
executable text and hot symbols.

## Engineering notes: the install offer

The parser's existing visible-text statistics pass also keeps a census for
each script: a saturating count and the page's own codepoints at the 1st,
4th, 16th and 64th occurrence (`DocumentGlyphCensus`). A script is in
meaningful use at 32 or more codepoints that are also at least one per 20
visible-text bytes, so a stray word or a long article's language-picker list
does not qualify. This adds one count per classified non-ASCII codepoint and
no DOM walk; the scan runs over the whole text, because the count needs it.

On a settled page with no other notice, the PSP app asks
`tilefinch_glyph_offer_poll()` once per page identity (URL plus census). It
offers the first catalog pack whose `page_scripts` lists a qualifying script
when no pack for that script is installed (the session mask, then the
store resolver's few `stat` calls), the pack is not in the profile's "Don't
ask again" mask, and the active sans face lacks at least half of the sampled
codepoints (`font_face_has_codepoint`, which also consults the attached
packs and the embedded CJK/emoji fallback). That last test is the trigger:
Polish (Latin Extended-A, carried by the embedded faces) is not offered the
Extended Latin pack that Vietnamese (Latin Extended Additional) needs, and
CJK is not offered at all while the embedded fallback draws it. A site is
offered at most once per session (16 hashed sites, oldest forgotten).

The offer takes only Cross and Triangle, once armed (0.6 s); Circle stays
Back and closes it. Cross opens a confirmation overlay
(`PSP_UI_SCREEN_GLYPH_OFFER`) and asks the session for the size:
`psp_glyph_component_session_prepare_size_check()` selects the pack's
update-client operation without the menu's auto-install, the app starts the
client's signed-metadata check over the network exactly as the menu does,
and the check stops at AVAILABLE. The confirmation shows the verified
manifest's `package_size`, or the check's own failure message, in the
interface language. Only a second Cross there raises the menu's
`glyph_component_primary_requested`, so the download, signature
verification, free-space preflight and Memory Stick promotion are the
menu's own. Triangle sets the pack's bit in the profile's "Don't ask again"
mask; **Menu → Settings → Appearance → Language & emoji → Offer language
packs** (Ask/Off) and **Reset declined offers** control both.

Every completed install, from the menu or the offer, sets the session's
`reattach_pending`. `psp_glyph_component_session_reattach()` then restores
the selected language and emoji packs that the install detached and attaches
installed packs for the page's scripts through the lazy path (the offered
pack's scripts for an offer install), and the app lays the page out again in
place through the user-stylesheet rebuild that page text size uses. If that
relayout cannot run, the notice asks for a reload; if the installed pack is
not one the page or the selection uses, **Font pack ready after restart**
applies. Scripted and replay runs (`trace` not `none`) and builds without
live networking never show the offer.
