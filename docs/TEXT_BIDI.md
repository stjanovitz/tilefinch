# Bidirectional page text

Tilefinch lays out mixed left-to-right and right-to-left page content without
rewriting DOM strings. Ordinary English and other LTR-only paragraphs retain
the existing text path; a parser census and stylesheet summary keep them out
of bidi analysis entirely.

This contract covers page content. Browser chrome localization, a general
text-selection UI, and complex-script shaping beyond the Arabic family are
not part of the current milestone.

## Pipeline

An inline formatting context enters the bidi path only when visible text,
`dir`, `direction`, or `unicode-bidi` can affect ordering. The bounded path is:

1. Flatten the inline paragraph into UTF-32 analysis units while recording
   source spans and representing atomic inline boxes with U+FFFC. The source
   DOM and UTF-8 remain in logical order.
2. Resolve paragraph levels with SheenBidi 3.0.0, a proven C implementation of
   Unicode Bidirectional Algorithm rules. Embeddings, overrides, isolates,
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
| UAX #9 visual runs per line | 256 | keep that line in logical order |
| inline source spans | 512 | keep the formatting context in logical order |
| mapped text commands | 1,024 | unmatched commands retain logical rendering |
| atomic inline boxes | 256 | keep the formatting context in logical order |
| retained shaped glyphs per layout | 4,096 | keep the affected line in logical order |
| SheenBidi scratch arena | 12 KiB | fail the analysis without partial state |

Paragraph analysis uses one page-budget allocation and a reusable aligned
scratch arena; it never allocates per codepoint. Temporary paragraph state is
released when its block flow completes. Only sparse visual glyph mappings
needed by the retained layout survive. Allocation refusal and hostile control
nesting do not reactivate the historical reverse-codepoint painter.

The current path deliberately omits text shadows for bidi-shaped commands.
That cosmetic degradation avoids duplicating shaped sidecars and never changes
text, links, or hit geometry.

## Glyph components

Arabic and Hebrew are optional signed language packs, alongside Japanese,
Chinese, Korean, Cyrillic, and Extended Latin. The default embedded Latin and
fallback glyphs remain available through missing, damaged, interrupted, or
removed components. Selecting a language keeps that pack attached; the parser
may lazily attach at most two other installed language packs when the page
actually uses their scripts. Color emoji remains independent, and the runtime
still admits no more than four packs total.

The Arabic pack includes U+0600–U+08FF base/mark coverage and the contextual
presentation forms emitted by the shaper. The Hebrew pack includes Hebrew
letters, marks, and presentation forms. Both use the existing TFGF/TFGM signed
component format and cause no pack enumeration or payload read at boot.

## Qualification

The focused host tests combine UAX #9-derived mixed-direction cases with
Arabic, Persian, Urdu, Hebrew, URLs, punctuation, digits, join controls,
over-depth controls, maximum admitted paragraphs, run-limit refusal, nested
HTML/CSS direction boundaries, wrapping, inline links and controls, hit tests,
visual caret stepping, logical selection/copying, and find geometry.
Signed-component proof opens the actual release packs and requires authored
pixels for Arabic presentation forms and Hebrew letters.

Performance qualification compares the same English Wikipedia capture and a
provider-results-shaped LTR fixture before and after. Both must stay within
normal timing variance and report zero admitted bidi paragraphs. An RTL
fixture records paragraph count, fallbacks, bidi microseconds, and peak
transient budget use. The PSP build separately gates executable text and hot
symbols.
