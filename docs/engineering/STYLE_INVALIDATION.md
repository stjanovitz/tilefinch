# Style invalidation and layout reuse

This page is for engineers changing what a DOM change restyles or relays out:
the mutation journal, invalidation scoping, the retained style table, the
hidden-content skip, container-query probes, retained focus paint, and how
stylesheet input reaches the cascade. The rules are summarized in
[Architecture](../ARCHITECTURE.md#dom-mutations-and-style-invalidation); this
page holds the mechanism. Streaming parse and provisional layout are in
[Streaming navigation](STREAMING_NAVIGATION.md).

## Mutation journal

Script DOM writes are recorded in a bounded, allocation-free journal per realm
(`SCRIPT_MUTATION_JOURNAL_LIMIT` records a turn, with a shared pool for the
class tokens a write changes; `include/tilefinch/js_runtime.h` gives the sizes
and their cost). When it fills, the engine keeps up to 16 subtree roots
(`SCRIPT_MUTATION_OVERFLOW_ROOT_LIMIT`) that hold the unrecorded changes and
restyles those, falling back to a full reset only when a root cannot be named.

## Invalidation scope

A class, id, or attribute change restyles the element plus the elements that
carry the subject key of a rule reading it from elsewhere (siblings
included); focus moves and inline custom-property writes are scoped by keyed
`:focus-within` rules and by which retained styles read which custom
properties. A changed child list restyles the parent and its children unless
a sibling test reaches their descendants, and an append after the last
sibling (or removal of the last element child) restyles only the parent and
that subtree when no sibling test counting from the end can see it.

Each record also says whether it can move a `:has()` answer: the sheet's
`:has()` rules are classified once (where the anchor lies relative to the
subject, which region the argument reads, which keys each must carry, at most
2048 occurrences), so a relational change drops only the cached styles those
answers can reach (`src/style_has_invalidation.c`).

## Retained style table

Computed styles are retained in a table that starts at 1,024 entries and grows
by powers of two up to 16,384 (`LAYOUT_REUSE_STYLE_CAPACITY` and `_LIMIT` in
`src/layout_internal.h`) when a full walk is about to cover more elements than
it holds, or a build evicted live entries, by more than a sixteenth of its
size. It grows as far as the budget allows while keeping 2 MiB to spare
(`LAYOUT_REUSE_STYLE_GROWTH_RESERVE_BYTES`). The table has to cover the walk:
a document-order walk through a smaller LRU table evicts each entry before the
next build reaches it (`layout_reuse_fit_styles` in `src/layout_reuse.c`).

## Hidden-only changes

A journal whose every change stays inside content that renders nothing (a
consent stub's `display:none` locator `<iframe>`, a script appended to
`<body>`, writes inside a closed menu) skips the relayout altogether
(`navigation_mutations_are_hidden_only` in `src/navigation/relayout.inc`).
Each record must lie inside an element that computes `display:none` at that
point and rendered nothing before, and must not be able to restyle anything
outside it through `:empty`, sibling position, or `:has()`. Paths through
`<head>`, inline SVG, form option lists, media and picture sources, plugins,
and image maps never skip, nor does a hidden list item, because layout
numbers list items whether or not they render. A head `<script>` is inert
only when no `:has()` anchored at `<head>` or `<html>` can see it
(`stylesheet_head_change_reaches_outside`).

## Container queries

Container queries get at most one measured probe followed by one
authoritative rebuild. The probe records each container's content box as soon
as block layout knows it and logs every container-query evaluation; when
every logged result still holds against the measured geometry, the probe is
exactly what the rebuild would produce and the rebuild is skipped. Early
answers (a flex item measured before its box is final), a dropped log,
container-relative units, or a DOM change take the rebuild, and while a probe
logs, nothing that consulted a container query is taken from the reuse cache.

The bounded retained-selector cache can serve container-query sheets because
it rechecks each rule's container condition against current geometry on every
replay; allocation refusal declines it and leaves ordinary style resolution.

## Retained focus paint

Simple focus changes update retained paint instead of laying the page out: up
to 64 rounded controls reserve one inset-ring command during layout
(`src/layout_block_decoration.c`) whose color, opacity, and thickness focus
then changes in place with bounded damage, after a dry run proves every old
and new focus target and with layer order and geometry fixed. Complex or
changing shadows, background clipping, inherited or relational effects, and
exhausted slots fall back to authoritative layout, and expanding a section is
always a layout operation.

## Stylesheet input

Stylesheet suffixes preserve the selector compiler's and index's state, and
`@font-face` rules are discovered during the main CSS parse rather than in a
second walk. A sheet larger than its per-file cap is not discarded: the
buffered bytes are cut back to the last complete top-level rule
(`stylesheet_complete_rules_prefix` in `include/tilefinch/resources.h`, an
allocation-free scan that skips strings, comments, escapes, and `url()`
bodies), so no half rule is applied, and the truncation stays visible in the
resource counters.
