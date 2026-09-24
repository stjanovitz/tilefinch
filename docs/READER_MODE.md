# Reader mode

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

The pass visits at most 8,192 DOM nodes, tracks at most 128 levels of
nesting, and keeps at most 64 listing entries. Its scratch table is charged to
the page budget and released straight away. The classification and the DOM
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
  listing, or media page.

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
- following a page link while Reader mode is active carries the bounded base
  Reader sheet into the destination, prepares its content-shape markers after
  parsing, and performs one authoritative Reader layout. The loading chrome
  says `LOADING READER PAGE` while the candidate is pending;
- cancellation or failure restores the incumbent page's Reader adapter and
  scale, while address-bar, tab, internal-page, and explicit history
  navigations continue to leave Reader mode before admitting their target.

Reader mode is not a sanitizer, content blocker, or separate browsing realm.
The retained author DOM remains page-owned, while the connected extracted tree
has exact native provenance and the current page's author realms are retired
after presentation succeeds. The host renderer's `--reader-profile` options
are older, deterministic CSS fixtures for engineering comparisons; they are
not the device product's content-shape classifier.

**Page tools → Save article** is deliberately separate from this live
presentation. It serializes a bounded, text-only Reader document instead of
keeping the page's realm or DOM; see [Offline library](OFFLINE_LIBRARY.md).

[Basic view](BASIC_VIEW.md) is a broader fallback that keeps bounded, explicit
GET and search forms and navigation instead of selecting only an article or
listing. Reader and Basic share one extracted-tree slot, and neither replaces
the authored DOM.
