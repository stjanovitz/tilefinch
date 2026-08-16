# Reader mode

Reader mode is an internal presentation adapter for ordinary HTTP(S) pages.
It keeps the parsed document, JavaScript realm, resources, controls, and page
history intact, then applies a bounded user-origin stylesheet through the
existing BrowserEngine user-CSS seam. Turning it off restores the original
author layout without a refetch.

The generic transform hides top-level headers, footers, navigation, sidebars,
and complementary landmarks; prefers the branch containing `article` or
`main`; reflows the retained content to the full viewport width; uses a 1.55
line height; and bounds images, video, SVG, canvas, tables, and preformatted
text to the viewport. Small site adapters refine known Wikipedia, Reddit, and
NYTimes chrome without changing the DOM or adding site-specific engine paths.

Reader font is a persisted Sans/Serif choice. While Reader mode is active, the
normal Web pages scale control adjusts Reader text. `Remember size` is off by
default. When enabled, the profile records only the scale for the page's
registrable site in a 16-entry move-to-front table; sibling hosts such as
`en.wikipedia.org` and `m.wikipedia.org` share one value. Disabling the option
clears the table. Merely opening, closing, or navigating in Reader mode never
marks the profile dirty, so the default behavior adds no Memory Stick writes.

Limits are deliberate:

- generated Reader CSS is capped at 8 KiB and uses no page-owned heap before
  BrowserEngine transactionally copies it;
- site scale values are restricted to 80, 100, 125, and 150 percent;
- at most 16 registrable sites are retained, with the least-recently changed
  entry evicted;
- built-in `tilefinch.local` pages do not admit Reader mode;
- following a page link while Reader mode is active carries Reader mode into
  the destination and installs its bounded Reader sheet before the first
  layout; the loading chrome says `LOADING READER PAGE` while that candidate
  is pending;
- cancellation or failure restores the incumbent page's Reader adapter and
  scale, while address-bar, tab, internal-page, and explicit history
  navigations continue to leave Reader mode before admitting their target.

This feature is not a readability extractor, sanitizer, or blocker. Hidden
content remains live and scripts run exactly as they did before the transform.

**Library → Save article for later** is deliberately separate from this live
presentation transform. It serializes a bounded, text-only Reader document
rather than retaining the page realm or DOM. See
[Offline library](OFFLINE_LIBRARY.md).
