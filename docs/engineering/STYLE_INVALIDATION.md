# Style invalidation and layout reuse

This page is for engineers changing what a DOM change restyles or relays out:
the mutation journal, invalidation scoping, the retained style table, the
hidden-content skip, geometry read proofs, container-query probes, retained focus paint, and how
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

## Geometry reads within a turn

A geometry read in the middle of a script turn applies the pending journal to
the reuse cache and builds a complete layout as script's scratch snapshot
(`script_runtime_flush_geometry_layout_reuse`). Content inserted from a
detached tree and removed again in the same turn (a measurement probe) is
transient: relayout skips its records (`navigation_mutation_is_transient`),
and a journal holding nothing else keeps the published layout. What a build
saw with the probe present must not outlive it. The snapshot is dropped when
the DOM changed after it was built, so later reads answer from the published
layout again (`script_runtime_keep_published_layout`). A record a build
applied while its node was in the document is marked (`reuse_seen`); once it
turns transient, the parent the content left is restyled once, for every
`:has()` entry, since the content the build saw may differ from what the
removal's own probe found (`navigation_reuse_settle_transient`).

Each build in the turn applies only the records it has not applied yet. A
record the cache took is marked (`reuse_applied`), and the journal names the
cache and its epoch (`layout_reuse_cache_epoch`) it was applied to. Records
merge in place (a later write to the same node and kind, or a removal's
scope), so the bridge clears a merged record's mark rather than the journal
keeping a count of records applied. The marks are dropped, and every record
is applied again, when the cache is a different one, its epoch moved (a
reset, a changed sheet, fonts or viewport, a rebind), the journal overflowed,
or the turn's `:has()` plans mixed. A marked record whose content has since
turned transient still gets its settle. The journal and its marks are zeroed
when the turn's relayout consumes it.

A write recorded while its node was inside content the turn inserted
(`inside_insert`), whose node has since left the document with that insertion
transient, changed nothing a connected element can see. The cache drops only
that subtree's styles, match lists and measurements
(`layout_reuse_cache_invalidate_detached`); a `:has()` note from a node with
no connected ancestors could place no anchor and would drop every
`:has()`-dependent style. What a build saw of the insertion, its settle
restyles. A subtree that left any other way (a removal from a connected
parent, an `innerHTML` on a connected node) keeps the ordinary path, since
that removal's probe saw the content after the write, not necessarily what the
cache holds.

### Reads during page load

A committing page keeps deferred scripts' feature probes under an image-only
rule: their geometry reads build no layout. `DOMContentLoaded` and `load`
listeners lift that rule (`script_runtime_finish_loading`), within a work
allowance (`loading_build_admitted`):

- **Only the page's own reads build.** A read builds only while page code is
  on the stack (`JS_IsAuthorCodeRunning`): a listener's read, directly or
  through browser code it called, whatever brackets lie between (the JS
  profiler's trampolines bracket every native). The browser's own work during
  load (the dispatcher between listeners, observer passes, focus fixups,
  chrome, any read from outside script) answers from the estimate. A listener
  that measures nothing costs no layout.
- **The first build is admitted by an estimate.** Before it runs, its work is
  estimated from the live tree: every element tests the rules that no tag, id
  or class selects (`stylesheet_rules_tested_per_element`, which builds the
  rule index first when a sheet change dropped it, as the build would), plus
  16 units per node (`LOADING_LAYOUT_UNITS_PER_NODE`; ordinary text content
  counts 15 to 19, a hidden subtree less). It builds only when that is at most
  16,384 units (`LOADING_LAYOUT_WORK_FLOOR`), which is a page of roughly 1,000
  nodes without such rules. A news page with 175 of those rules and 3,000
  elements is estimated at about 660,000 units and builds nothing; its one
  build was about 20 s under PPSSPP.
  - The tree is counted, not read from the document's totals, which are the
    parser's and lag a tree a script built before its read. The count stops
    once the nodes alone pass the floor, and is kept until a script changes
    the tree, so a loop of style writes and reads walks it once.
  - The estimate leaves out the candidates an element's tag, id and classes
    add and how much text a node holds: it is a lower bound of what the
    build counts, and a page under the floor may still build several times
    the estimate.
- **Later builds are paid from what the first set**: half its work, or the
  floor when that is more (`LOADING_LAYOUT_SHARE_PERCENT`).

Work is counted, not timed, so a replay spends the allowance the same way on
every run. A build's work is the selector candidates its style resolution
tested plus its layout work units (nodes and text laid out). On the host both
cost about 0.1 µs, so the sum follows a build's time.

- **A later build without the reuse cache starts cold.** It costs about what
  the first build did, so it starts only when the allowance still covers the
  first build's work.
- **A later build with the reuse cache often costs a fraction.** It may start
  while any allowance is left, so the last one can overrun the allowance by
  its own work.
- **A read that proves the snapshot still holds builds nothing** and spends
  nothing, whoever reads.

When no build is admitted, a read whose inputs changed since the snapshot
drops it (`geometry_snapshot_release`). That read, and every later one, then
answers from the layoutless estimate (`geometry.js`). The estimate follows the
page's style writes, so a page never reads a snapshot it has since changed: a
width written and read back matches, and a loop that shrinks a box until it
fits ends.

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

## Parser growth

The streaming preview's cache serves styles across preview builds while the
parser keeps appending, and the parser writes no journal. Tree construction
inserts at its current node or one of the open elements above it, so before
each preview build navigation hands the cache the parser's current node
(`document_parser_current_node`), and the build records the path from the
Document to it, with each element's last child. That path is not always the
document's last-child chain: an element foster-parented out of a table sits
before the table, and after `</body>` comments follow `<body>`. The next
build restyles what the content added since can move, as a script append
after the last sibling would: a parent's `:empty`, the earlier siblings'
tests counting from the end and what reads them through `+`, `~` or a
descendant combinator, the `:has()` answers the new children reach (with an
anchor's remembered "no `::before` or `::after`"), and the measurements of
every element whose content grew, text the parser merged into the node
before a table included. Anything else on the path (an attribute merged
into `<html>` or `<body>`, foster parenting before a recorded node, the
adoption agency, a subtree a script freed, a path past 128 levels) restyles
that element's subtree. Once the parse ends, the page's first build (or a
style read before it) applies the same step one last time, against the sheet
the page has by then, and the cache stops following the parse
(`src/layout_reuse_parser.inc`). The commit often adds rules first (body
`<style>` elements after the last preview); their new sheet generation
empties the cache, and the step then has nothing to restyle.

The step reads the sheet's trailing and `:has()` argument keys and, for a
`:has()` note, its `:has()` classification, all rebuilt for each new sheet
generation. Each rule keeps what its selector text holds (`:has(`, a test
counting from the end, `:focus-within`; `STYLE_SELECTOR_FACTS_*` in
`src/style_cache_internal.h`), filled the first time a scan asks and moved
with the rule when the sheet re-sorts, so a rebuild reads the text of only
the rules added since. A custom-property rule also keeps whether it has a
sibling or positional test, filled apart because only the structural-name
scan reads it, and takes its facts from the declaration before it when both
hold the same selector text.

Testing whether an element can carry a trailing or `:has()` argument key
enumerates the element's keys once against the whole key table, and a walk
over the earlier siblings of one parent asks about the parent and its
ancestors once. When only the siblings' own keys place the tests counting
from the end (no key on the parent or above), the step restyles the
siblings from the first one carrying such a key onward, with their
subtrees, and keeps the styles of the siblings before it.

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

A journal that left the tree as it was skips the relayout too
(`navigation_mutations_left_tree_unchanged`): every record is transient, or
names a node that left the document by way of transient content, with no
connected scope (a probe built with `innerHTML`, cells styled inside it, read
and removed again). A conservative resource scan does not stop it; the
resource fingerprint must be unchanged instead. Records retired by a free,
resource or sheet rebuilds, image scans, a sheet a read installed during the
turn, and pages with shadow trees (whose content parents do not reach the
document) take the ordinary path. The skip settles the reuse cache and drops
the turn's geometry snapshot like the other skips.

## Geometry read proofs

A geometry read after DOM writes keeps the turn's current snapshot when the
writes it has not seen moved nothing (`navigation_prepare_geometry_proof` in
`src/navigation/geometry_proof.inc`). Each record the snapshot has not seen
(`reuse_applied` clear when the read begins) must be transient, inside content
that renders nothing (the hidden-only test above), or an attribute or inline
style write that layout sees only through computed styles: `class`, `style`,
`data-*` (not `data-tilefinch*` or `data-placeholder`) and `aria-*`, with no
attribute selector naming it outside its element's own compound, and no
`attr()` reading it unless the proof re-checks that element's style. A record
that a build already saw and that has since turned transient, a relational
record, an overflowed journal, a retirement the journal did not observe, a
queued resource or stylesheet rebuild, shadow trees and container queries all
refuse. So does a journal with no record the snapshot has not seen, since the
document then changed without one, and an image table that differs from the
one the snapshot was laid out with (`LayoutDocument.image_fingerprint`,
`images_layout_fingerprint`): an image arrived or changed size meanwhile.

The style checks come from the reuse cache (`layout_reuse_proof_*` in
`src/layout_reuse.c`). A build records whether every element style it resolved
is still in the table when it finishes; the layout carries that build's
serial (`LayoutDocument.reuse_serial`), and the cache vouches for that layout
until it loses an entry the layout used. While the journal is applied for the
read, nothing new is stored, and each entry an invalidation drops is kept as
a row with its payload, up to 32 (`LAYOUT_REUSE_PROOF_ROW_LIMIT`; more refuse).
Each row is then resolved again, root first: it holds when the element is
detached or under a `display:none` ancestor, when it is `display:none` itself
and the row was too, or when its style and parent style hash are byte-equal
to the row and each `::before`/`::after` a build resolved generated nothing
then and generates nothing on the second resolution (`pseudo_resolved`,
`pseudo_absent`). Equal rows go back into
the table. A style the table could not keep (a full probe window at the size
limit) is held apart instead, up to 16 (`LAYOUT_REUSE_PROOF_HELD_LIMIT`), and
every proof re-checks it.

A proof that holds keeps the snapshot and marks its paint stale, as the focus
proof does. One that is refused, or whose rows differ, builds a snapshot.
`TILEFINCH_VERIFY_GEOMETRY_PROOF` builds anyway and compares every box and
the command list; `TILEFINCH_DISABLE_GEOMETRY_PROOF` turns proofs off.

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
