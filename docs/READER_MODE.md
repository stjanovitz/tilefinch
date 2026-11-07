# Reader mode

Reader mode shows an easier-to-read copy of the page you are on: the main
article, listing or video with the surrounding navigation, sidebars and site
styling removed, set in Tilefinch's own type at full screen width. This page
is the reference for PSP owners who want to know exactly how Reader decides
what to show, and for contributors; the
[user guide](USER_GUIDE.md#reader-mode) covers everyday use.

Reader keeps the page you loaded, its resources and its history entry, and
builds the simplified copy beside it. Once the Reader view is shown,
Tilefinch stops the page's JavaScript, so later scripts cannot rewrite or
intercept it. Turning Reader mode off restores the original layout without
fetching anything again, but the page's scripts do not resume; reload the
page to run its JavaScript again.

Reader's presentation resets the site's sizing, text metrics and generated
decorations, while keeping headings, inline emphasis, code, links and image
size hints. This keeps a site's application styles from adding oversized gaps
or overlapping labels to the reading view.

## How a page is classified

Reader decides by the document's shape, not by a list of sites. The first
time it is used on a loaded page, one pass over the DOM classifies the page
as one of four forms:

- **Article:** one text-dense subtree with few links dominates the page.
- **Listing:** at least eight repeated media entries share a list
  container. A matching link alone is not enough; each entry also needs a
  thumbnail or nearby duration or view-count metadata.
- **Media:** the document has a visible, playable `<video>` or `<audio>`
  element, or a high-confidence discovered video next to its main title.
  Schema.org and `og:type` media hints alone do not make a page Media,
  because promotional metadata would misclassify ordinary landing pages.
  Those hints do rule out a Listing, so a page whose player is added by
  script, next to a rail of related videos, becomes an Article or Raw rather
  than hiding its description as a listing.
- **Raw:** no safe shape was found. Reader mode is unavailable and the page
  is shown unchanged.

A substantial authored article takes precedence over a containing page
wrapper whose extra prose comes from recommendations. Within that article,
Reader puts its heading, author link and opening prose before lead media,
keeping each source node once and the rest of the article in source order.
Content beyond Reader's limits is never presented as complete: a view you
open yourself shows a shortening notice, and Auto Reader engages only when
the whole article fits.

Reader recognizes both `<main>` and `role="main"` content landmarks. A
heading immediately before the selected prose in its main landmark is kept as
the reading title. Small linked-logo mastheads do not displace the content,
even when nested inside that landmark; Basic view keeps those links in a
collapsed navigation section.

When the page's own layout is available, extracted images keep its useful
inline size rather than enlarging small header graphics. Reader also
recovers illustrations alongside prose from pages whose scripts failed to
reveal them.

Collapsed sections count as content. A region the author hid with the
`hidden` attribute until it is opened, either `hidden=until-found` or a
region named by the `aria-controls` of an `aria-expanded="false"` control just
before it (the WAI-ARIA disclosure pattern, which some encyclopedia sites use
for every section after the lead), is extracted like visible content. Reader
and Basic view stop the page's scripts, so the control could never open it
there. Content hidden any other way stays out.

## Turning it on

Reader mode is manual by default:

- **Menu → Page tools → Reader mode** turns it on for the current page;
- **Menu → Page tools → Site information → Permissions & controls → Always
  Reader** turns it on for every page of one site;
- **Menu → Settings → Appearance → Auto Reader** is a global opt-in that
  engages only when the classifier reports a high-confidence article,
  listing, or media page. It is checked once, when a page finishes loading,
  and switches the page to Reader straight away without asking.

Auto Reader decides page by page, and its Reader does not carry over to the
next link the way Reader you turned on yourself does; the
[user guide](USER_GUIDE.md#reader-mode) describes how that feels in use. The
blank-page Reader fallback counts as a choice you made, so its Reader carries
over too.

High confidence needs the whole page: if the classifier or the extraction
reached a limit, Auto Reader leaves the page alone, because the unseen rest
could change the page's kind and a shortened Reader view would hide the rest
of the article. An article is high-confidence only when its extracted root
looks like one clear article:

- it has a page title (an `h1`), or the root is an authored `<article>` with
  a heading;
- it is mostly prose: at least five paragraphs with 120 bytes or more of
  their own text and at most a quarter of it in links, at least a third of
  the root's paragraphs, and, with preformatted text, at least half of the
  root's non-link text;
- it has at most two headline links: `h2` to `h4` headings whose text is
  mostly a link to another page. A heading's link to its own fragment, as
  on documentation pages, is not a headline link.

An article made of lists rather than paragraphs stays manual.

## Font and size

**Reader font** is a saved Sans or Serif choice. While Reader mode is active,
the normal page text-size control adjusts Reader text. Reader mode lays out
at 100% whatever the Page zoom setting. **Remember size** is
off by default; when it is on, the profile records only the size for the
page's registrable site, so sibling hosts such as `en.example.org` and
`m.example.org` share one value. Turning the option off clears the saved
sizes. Opening, closing, or navigating in Reader mode never writes the
profile, so the default behavior adds no Memory Stick writes.

## Limits

- The classifier keeps a scratch record for at most 8,192 elements, visits
  at most 65,536 nodes, follows at most 128 levels of nesting, and keeps at
  most 64 listing entries. Paragraphs, like text, count only outside hidden
  and excluded regions (navigation, headers, footers, asides and forms).
- Reader walks at most 32,768 nodes of its article or listing root.
- The extracted copy shares
  [Basic view's limits](BASIC_VIEW.md#limits): at least 512
  nodes and 256 KiB of markup, and up to 8,192 nodes and 1 MiB when the page
  has the memory to show them.
- Remembered sizes are 80, 100, 125, or 150 percent, for at most 16 sites;
  the least recently changed site is forgotten first.
- Built-in `tilefinch.local` pages do not offer Reader mode.
- Lazy images are promoted only from the conventional one-pixel
  placeholders, and stay subject to the ordinary image count, size,
  permission, and offscreen-deferral limits.

Reader mode is not a sanitizer, content blocker, or separate browsing
context. **Menu → Page tools → Save article** is separate from this live view:
it saves a text-only Reader copy instead of keeping the page; see
[Offline library](OFFLINE_LIBRARY.md).

## How this differs from Basic view

Reader mode is for pages that work: it keeps only the main article, listing
or media, and Auto Reader is an opt-in that engages on high-confidence pages
even when nothing failed. [Basic view](BASIC_VIEW.md) is for pages that do
not work: it keeps nearly all of the page's real content, including
navigation links, tables and simple GET and search forms, and its automatic
fallback (on by default) engages only when scripts failed and left the page
blank, hidden or unusable. Both stop the page's scripts until a reload, and a
page has one or the other: Basic is tried first on failure, and neither
replaces the page's own DOM.
[Reader mode vs Basic view](USER_GUIDE.md#reader-mode-vs-basic-view)
compares them side by side.

## Engineering notes

- Reader styles the extracted tree with user-origin CSS through the
  `BrowserEngine` presentation seam. The generated Reader CSS is capped at
  8 KiB and copied by `BrowserEngine` in one step, so a refused rebuild
  leaves the previous view intact. The retained author DOM stays page-owned,
  and the page's author realms are retired only after presentation succeeds.
- The classification and the DOM markers it produces are cached with the
  loaded page, so scrolling and repainting do not repeat the work. Marker
  installation is journaled: an allocation failure, a collision with an
  internal marker, or a limit refusal removes every marker the attempt added
  and leaves the author DOM unchanged. The classifier's scratch table is
  charged to the page budget and released straight away.
- The internal page-kind name for Media is `watch`.
- Switching views reuses the current page's retained stylesheet responses,
  including their redirect and referrer provenance; it does not depend on
  the optional HTTP cache still holding them. The presentation shares
  response bytes while keeping its own budget ledger.
- Following a page link while Reader you turned on is active carries the
  base Reader sheet into the destination, prepares its content-shape markers
  after parsing, and performs one Reader layout; the loading chrome says
  `LOADING READER PAGE` meanwhile. Cancellation or failure restores the
  previous page's Reader adapter and scale, while address-bar, tab,
  internal-page, and explicit history navigations leave Reader mode before
  loading their target.
- Back and Forward leave Reader and classify the page again, because history
  entries do not record which view they were shown in.
- The host renderer's `--reader-profile` options are deterministic CSS
  fixtures for engineering comparisons, not the device's content-shape
  classifier.
