# Reader mode

Reader's extracted presentation neutralizes author sizing, text metrics and
generated decorations, while preserving semantic headings, inline emphasis,
code, links and image sizing hints. This prevents application styles from
introducing oversized gaps or overlapping labels into the reading surface.

Reader mode is a bounded native presentation of an ordinary HTTP(S) page. It
keeps the parsed author document, its resources, and its history entry, adds
one extracted semantic tree beside them, and styles that tree with user-origin
CSS through the existing `BrowserEngine` seam. Once the Reader view is shown,
Tilefinch retires the page's author JavaScript realms, so later scripts cannot
rewrite or intercept it. Turning Reader mode off restores the retained author
layout without fetching anything again, but script state does not resume;
reload the page to run its JavaScript again.

The transform is driven by the document's shape, not a list of hostnames. The
first time it is used on a loaded page, a bounded DOM pass classifies the page
as one of four forms:

- **Article:** one text-dense, low-link-density subtree dominates the page.
- **Listing:** at least eight repeated, evidenced media entries share a list
  container. A matching link alone is insufficient; each entry also needs a
  thumbnail or nearby duration/view metadata.
- **Media:** the document has a visible, playable `<video>` or `<audio>`
  element, or a high-confidence discovered candidate next to its primary
  title. Schema.org and `og:type` media hints alone do not make a page Media,
  because promotional metadata would misclassify ordinary landing pages. Those
  hints do veto a high-confidence Listing, however, so a script-injected
  player with a related-items rail degrades to Article or Raw instead of
  hiding its description as a listing. The internal page-kind name is still
  `watch`, for stylesheet and telemetry compatibility.
- **Raw:** no safe semantic shape was found. Reader mode is unavailable and
  the author presentation remains unchanged.

A substantial authored article takes precedence over a containing page
wrapper whose extra prose comes from recommendations. Within that article,
Reader puts its heading, author link and opening prose before lead media,
retaining each source node once and preserving the rest of the article in
source order. Content beyond an extraction bound is never presented as
complete: manual views show the shortening notice, while automatic admission
still requires a complete extraction.

The pass keeps a scratch record for at most 8,192 elements, visits at most
65,536 nodes (text nodes need no record, and an encyclopedia article has
about one per element), tracks at most 128 levels of nesting, and keeps at
most 64 listing entries. Its scratch table is charged to the page budget and
released straight away. Paragraphs, like text, count only outside hidden and
excluded regions (navigation, headers, footers, asides and forms).

Collapsed sections count as content. A region the author hid with the
`hidden` attribute until it is opened, either `hidden=until-found` or a
region named by the `aria-controls` of an `aria-expanded="false"` control just
before it (the WAI-ARIA disclosure pattern, which mobile Wikipedia uses for
every section after the lead), is extracted like visible content. Reader and
Basic view retire the page's scripts, so the control could never open it
there. Content hidden any other way stays out.

The extracted tree shares [Basic view's emitted bounds](BASIC_VIEW.md#bounds-and-transactions):
at least 512 nodes and 256 KiB of markup, and up to 4,096 nodes and 1 MiB
when the page has the memory to show them. Reader walks at most 16,384 nodes
of its article or listing root. The classification and the DOM
markers it produces are cached with the loaded page, so scrolling and
repainting do not repeat the work. Marker installation is journaled: an
allocation failure, a collision with an internal marker, or a bound refusal
removes every marker the attempt added and leaves the author DOM unchanged.
Promoting lazy image sources recognizes only the conventional one-pixel
placeholders and stays subject to the ordinary image count, byte,
authorization, and offscreen-deferral limits.

Reader mode is manual by default:

- **Menu → Page tools → Reader mode** turns it on for the current page;
- **Page tools → Site information → Permissions & controls → Always Reader**
  is a bounded per-site preference;
- **Settings → Appearance → Auto Reader** is an explicit global opt-in that
  engages only when the classifier reports a high-confidence article,
  listing, or media page. It is checked once, when a page finishes loading,
  and switches the page to Reader straight away (it does not ask first).

Auto Reader decides page by page:

- When it switches a page to Reader, a short note says so: "Reader view
  (Auto Reader)" and "Select, Page tools, Reader mode: full page". It clears
  by itself like other status notes. Circle stays Back, as on any page.
- Reader that Auto Reader turned on belongs to that page. Following a link
  leaves it, and the destination gets its own check: a front page reached
  from an automatically shown article appears as it is.
- Reader you turn on yourself (Page tools, Always Reader, or the recovery
  sheet's Reader choice) stays on as you follow links, as before. Turning an
  automatic Reader off and on again in Page tools makes it yours.
- Back and Forward leave Reader and check the page again, as history
  entries do not record which view they were shown in. A page Auto Reader
  showed in Reader is shown in Reader again; one you switched yourself is
  not.
- The blank-page Reader fallback is not Auto Reader: like a choice you
  make, its Reader carries over to the next link.

High confidence needs the whole page: if the classifier walk or the
extraction reached a bound, Auto Reader leaves the page alone, because the
unseen rest could change the page's kind and a shortened Reader view would
hide the rest of the article. An article is high-confidence only when its
extracted root looks like one clear article:

- it has a page title (an `h1`), or the root is an authored `<article>` with
  a heading;
- it is mostly prose: at least five paragraphs with 120 bytes or more of
  their own text and at most a quarter of it in links, at least a third of
  the root's paragraphs, and, with preformatted text, at least half of the
  root's non-link text;
- it has at most two headline links: `h2` to `h4` headings whose text is
  mostly a link to another page. A heading's link to its own fragment, as
  on documentation pages, is not a headline link.

On the October 2026 census of 55 popular pages, the BBC and Guardian
articles and an MDN reference page qualify, and no front page, product page,
store, forecast or application page does. Wikipedia's PlayStation Portable
article would qualify by shape, but its Reader view needs about 5,100 nodes,
more than the 4,096-node bound, so it stays manual. A Wiktionary entry is
made of lists rather than paragraphs and stays manual too.

**Reader font** is a saved Sans or Serif choice. While Reader mode is active,
the normal page text-size control adjusts Reader text. **Remember size** is
off by default; when it is on, the profile records only the size for the
page's registrable site, in a 16-entry move-to-front table, so sibling hosts
such as `en.wikipedia.org` and `m.wikipedia.org` share one value. Turning the
option off clears the table. Opening, closing, or navigating in Reader mode
never marks the profile dirty, so the default behavior adds no Memory Stick
writes.

Limits are deliberate:

- generated Reader CSS is capped at 8 KiB and is transactionally copied by
  BrowserEngine;
- site scale values are restricted to 80, 100, 125, and 150 percent;
- at most 16 registrable sites are retained, with the least-recently changed
  entry evicted;
- built-in `tilefinch.local` pages do not admit Reader mode;
- following a page link while Reader you turned on is active carries the
  bounded base Reader sheet into the destination, prepares its content-shape markers after
  parsing, and performs one authoritative Reader layout. The loading chrome
  says `LOADING READER PAGE` while the candidate is pending;
- cancellation or failure restores the incumbent page's Reader adapter and
  scale, while address-bar, tab, internal-page, and explicit history
  navigations continue to leave Reader mode before admitting their target.

Reader mode is not a sanitizer, content blocker, or separate browsing realm.
Switching views reuses the current page's retained stylesheet responses,
including their redirect and referrer provenance. It does not depend on those
responses still being present in the optional HTTP cache. The presentation
transaction shares response bytes while keeping its own ledger, so a refused
rebuild leaves the previous view intact.

The retained author DOM remains page-owned, while the connected extracted tree
has exact native provenance and the current page's author realms are retired
after presentation succeeds. The host renderer's `--reader-profile` options
are older, deterministic CSS fixtures for engineering comparisons; they are
not the device product's content-shape classifier.

**Page tools → Save article** is deliberately separate from this live
presentation. It serializes a bounded, text-only Reader document instead of
keeping the page's realm or DOM; see [Offline library](OFFLINE_LIBRARY.md).

## How this differs from Basic view

Reader mode is for pages that work: it keeps only the main article, listing
or media, and Auto Reader is an opt-in that engages on high-confidence pages
even when nothing failed. [Basic view](BASIC_VIEW.md) is for pages that do
not work: it keeps nearly all of the page's real content, including
navigation links, tables and bounded GET and search forms, and its automatic
fallback (on by default) engages only when scripts failed and left the page
blank, hidden or unusable. Both retire the page's scripts until a reload, and
they share one extracted-tree slot: a page has one or the other, Basic is
tried first on failure, and neither replaces the authored DOM.
[Reader mode vs Basic view](USER_GUIDE.md#reader-mode-vs-basic-view)
compares them side by side.
