# Reader mode

Reader mode is a reversible presentation transform for ordinary HTTP(S)
pages. It keeps the parsed document, JavaScript realm, resources, controls,
and history entry intact, then applies bounded user-origin CSS through the
existing BrowserEngine seam. Turning Reader mode off restores the author
layout without refetching the page.

The transform is driven by document shape rather than a hostname allow-list.
On its first use for a loaded page, a bounded DOM pass classifies the page as
one of four forms:

- **Article:** one text-dense, low-link-density subtree dominates the page.
- **Listing:** at least eight repeated, evidenced media entries share a list
  container. A matching link alone is insufficient; each entry also needs a
  thumbnail or nearby duration/view metadata.
- **Watch:** the document declares primary media through `<video>`,
  `VideoObject`, or `og:type`. This check precedes listing detection so a
  related-items rail does not turn a watch page into a listing.
- **Raw:** no high-confidence shape was found. Manual Reader mode still
  applies the conservative generic reflow.

The pass visits at most 8,192 DOM nodes, tracks at most 128 levels of nesting,
and retains at most 64 listing entries. Its scratch table is charged to the
page budget and released immediately. Classification and the DOM markers it
produces are cached with the loaded page, so scrolling and repainting do not
repeat the work. Marker installation is journaled: an allocation failure,
internal-marker collision, or bound refusal removes every marker added by the
attempt and leaves the author DOM unchanged. Lazy image source promotion
recognizes only conventional one-pixel sentinels and remains subject to the
ordinary image count, byte, authorization, and offscreen-defer limits.

Reader mode is manual by default. Choose **Menu → Page tools → Reader
mode** to enable it for the current page. **Always use Reader mode** is a
bounded per-site preference. **Settings → Appearance → Auto Reader** is
an explicit global opt-in that engages only when the classifier reports a
high-confidence article, listing, or watch page.

Reader font is a persisted Sans/Serif choice. While Reader mode is active, the
normal Web pages scale control adjusts Reader text. `Remember size` is off by
default. When enabled, the profile records only the scale for the page's
registrable site in a 16-entry move-to-front table; sibling hosts such as
`en.wikipedia.org` and `m.wikipedia.org` share one value. Disabling the option
clears the table. Merely opening, closing, or navigating in Reader mode never
marks the profile dirty, so the default behavior adds no Memory Stick writes.

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

Reader mode is not a sanitizer, blocker, or separate browsing realm. Hidden
nodes remain live, and scripts keep the same authority they had before the
transform. The host renderer's `--reader-profile` options are older,
deterministic CSS fixtures for engineering comparisons; they are not the
device product's content-shape classifier.

**Library → Save article for later** is deliberately separate from this live
presentation transform. It serializes a bounded, text-only Reader document
rather than retaining the page realm or DOM. See
[Offline library](OFFLINE_LIBRARY.md).
