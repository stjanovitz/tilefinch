# Basic view

Basic view is Tilefinch's bounded, reversible fallback for a page whose
server-rendered content is useful but whose application enhancement failed.
It is intentionally narrower than a second browser engine: the authored DOM
stays connected, while a compact native extraction supplies a predictable
presentation when the user chooses it.

Open **Menu → Page tools → Basic view** to switch between Raw and Basic. On a
page where scripts stopped before leaving useful content or actions,
Tilefinch can also switch to Basic, or offer it, by itself; see
[Automatic fallback](#automatic-fallback).

### How this differs from Reader mode

[Reader mode](READER_MODE.md) makes a page that works easier to read: it keeps
only the main article, listing or media and drops navigation. Basic view
rescues a page that does not work: it keeps nearly all of the page's real
content, including navigation links, tables and simple search forms. Both
drop the page's scripts and styling, and a page has only one of them at a
time. [Reader mode vs Basic view](USER_GUIDE.md#reader-mode-vs-basic-view)
compares them side by side.

The extracted tree retains, in document order:

- headings, paragraphs, lists, links and common inline semantics;
- images, figures and captions;
- tables and code blocks;
- details/summary content;
- collapsed sections: a region hidden with `hidden=until-found`, or named by
  the `aria-controls` of an `aria-expanded="false"` control just before it.
  Basic view retires scripts, so that control could never open it;
- explicitly authored, labeled `GET` or search forms.

Navigation landmarks (`nav` or `role="navigation"`) become closed, expandable
groups in place, labeled with the page's `aria-label` when supplied. Their
links remain available without JavaScript, while the main content and search
form no longer sit behind several screens of menu links. Ordinary inline
word fragments stay joined; adjacent independent links get a word boundary.
An unretained select is omitted rather than printing its entire option list.

Tables used to arrange navigation or page columns become ordinary stacked
blocks, instead of squeezing stories into narrow equal-width cells. Tables
with data headers or captions retain their structure; nested data tables are
classified separately from an enclosing layout table.

The simplified presentation resets author sizing, text metrics and generated
decorations on extracted content. Image width hints remain available so an
illustration awaiting download keeps a usable layout target.
If an image is unavailable, its alternative text reserves space before the
caption or following paragraph instead of painting over it.
Meaningful embedded images remain visible. Lazy sources use the same source
selection and resource authorization as the full page, including after a
view switch; the browser never guesses an alternative image URL.

Basic view does not infer behavior from button text, classes, test IDs or
click-handler-shaped markup. `POST` forms, unowned buttons, disabled controls,
downloads and opaque application commands never become new actions. An
admitted search form is limited to authored text/search fields, bounded
select/textarea values, hidden GET parameters and explicit submit controls.
Multiple-selection controls are refused rather than collapsed to a different
single-value submission contract.
Inherited disabled state is honored when form wrappers are flattened, and
authored read-only fields remain read-only.

## Bounds and transactions

Basic view is bounded by what it emits, not by how much markup precedes the
content, and the bound scales with the memory the page has left:

- Every preparation may emit at least 512 semantic nodes and 256 KiB of
  escaped markup (the fixed bound before October 2026).
- Beyond that it may emit one more node for each KiB the page budget has
  free above a 4 MiB reserve, up to 4,096 nodes. An emitted node costs about
  1 KiB once shown: about 0.6 KiB for the parsed copy and 0.4 KiB for its
  layout, measured on the 55-page site census. The reserve covers what
  showing any view costs whatever its size (restyling the raw page and
  laying out the view before the page's scripts are retired: about 2 MiB on
  Wikipedia, up to 3.3 MiB on the census) plus a margin.
- The markup bound follows the node bound at 256 bytes per node, between
  256 KiB and 1 MiB. The markup buffer starts at 64 KiB and grows only as
  far as the page needs.

Both bounds are fixed when preparation starts, so the same page and memory
state always give the same view. With the fixed 512-node bound, 15 of the 55
census pages were cut short (Hacker News lost its last stories and npr.org
four fifths of its front page). With the scaled bound 13 of them are
extracted whole; the other two (a GitHub repository page and an MDN page)
already peak within 1.8 MiB of the 32 MiB ceiling when shown at 512 nodes,
so they keep the 512-node view they had before.
Preparation stops walking the source as soon as the emitted prefix is full.
The walk itself stops after 65,536 source nodes (the bound of every other
whole-document walk), with a cooperative checkpoint every 128 nodes. A news
front page reaches its first story only after thousands of layout wrappers
(apnews.com has 24,585 nodes); an earlier 4,096-node inspection bound and the
rule that refused any truncated extraction left such pages without Basic view.
It retains at most eight forms and 32 controls. Scratch and clone ownership
are charged to the page's existing `Budget`.

A page larger than the emitted bound gets a balanced prefix that ends with
"Basic view shortened to fit this device." The cut never splits a form: a form
the bound reaches is withdrawn whole, because a form missing the controls
after the cut would submit a different query than the page's own. Every other
omission is a refusal: a form, control, select-option or anchor bound installs
nothing. Admission builds one bounded temporary extraction and commits that
same buffer only after these checks pass. A failed DOM refresh removes the
clone and its body marker before returning. Raw DOM, form state and
navigation history are never replaced.

Preparation and presentation are a two-phase transaction. Tilefinch first
retains the exact native root, then applies the Basic stylesheet. Only after
that stylesheet and relayout succeed does it activate the view and retire the
current page's author realms. This prevents delayed scripts from rewriting the
trusted-looking Basic links or forms, while a failed presentation leaves the
original raw page and JavaScript realm untouched. The configured JavaScript
policy is unchanged for the next navigation.

Preparation itself does not change pixels. Tilefinch offers Basic view only
after author work has settled, current-page script degradation is proven, and
the raw result is blank or has no positive-size action target. A useful raw
page therefore stays visible until the user explicitly switches views.

## Automatic fallback

**Settings → Browsing & input → Basic view fallback** decides what Tilefinch
does by itself when a page qualifies as above:

- **Automatic** (the default) switches to Basic view and says so.
- **Ask** leaves the raw page on screen and opens the recovery sheet with
  **Show Basic view** as its first choice after Retry. A declined offer is not
  repeated for that page.
- **Off** never switches to or offers Basic view by itself. The Page tools
  item still works, and the separate blank-page Reader fallback is unchanged.

A page qualifies when its scripts failed or were stopped and it is left blank
or without a usable action. That covers:

- pages whose scripts were stopped by the parser time limit or by repeated
  failures, or whose realm ran out of memory, including the case where an
  anti-flicker style hid the page and the script that would reveal it never
  ran (hidden content paints nothing, so the page counts as blank even though
  its DOM holds links). Tilefinch never matches particular sites or vendors;
- scripts that fail after the page was shown: a page is examined at commit,
  and once more when a later runtime turn fails or its realm is retired
  after a script failure;
- large pages, which now get a shortened Basic view instead of none.

Live author work is given a bounded settle window first, so a page that
reveals itself late is left alone.

Heavy pages ([user guide](USER_GUIDE.md#heavy-pages)) meet this fallback in
two places. **X Stop page scripts** on a heavy page's status retires its
realms like a realm that ran out of memory, so a page left blank or without
a usable action then qualifies here. A page whose app cannot fit gets the
recovery sheet titled "Too heavy for the PSP", with **Show Basic view** as
its first choice (it has no Retry; under Off it offers Reader and Return
only). An app shell the user chose not to start
is left as the server sent it: its realm is intact, so it does not qualify,
and Basic view of an empty shell would show nothing anyway.

When a page fails in a way the recovery sheet reports, its choices include
Basic view where it can help (not under Off):

- **Show Basic view** when the failed page is still loaded, its DOM exists
  and a Basic tree is prepared or can be prepared for it.
- **Reload in Basic view** when a navigation failed after its whole document
  arrived (an HTTP 2xx response that could not be built into a page, for
  example under memory pressure). Tilefinch reloads
  that address once with page JavaScript off, presents the result in Basic
  view, and restores the site's JavaScript setting for later loads. Network,
  TLS, HTTP-error and cancelled loads do not offer it.

## Switching and fragments

The normal text-anchor handoff preserves the nearest source position when
switching between Raw and Basic. Extracted IDs and legacy named anchors use a
bounded `tilefinch-extracted-` prefix. Fragment URLs stay exactly as authored;
same-document navigation prefers the painted prefixed target while an
extracted view is active, then falls back to the raw source ID or legacy named
anchor. The raw source IDs remain unchanged for switching back.

Reader and Basic share one extracted-presentation slot. Tilefinch never keeps
two proportional clones of the same page. Reader remains the focused article
or listing view; Basic is the broader action-preserving fallback. Either view
can be turned off to return to Raw. To replace one extracted kind with the
other on the same document, reload first; this keeps clone ownership
proportional and prevents stale JavaScript node references.

Returning from Basic to Raw does not resurrect the retired author realm. Reload
the page to run its scripts again. This is intentional: Basic is the bounded,
script-free recovery surface, not an alternate live view of an application.

## Structured data

Basic view uses only explicit DOM semantics. It does not turn
arbitrary JSON `contentUrl`, `SearchAction`, product metadata or application
state into controls. A later structured-data extension should reuse the
bounded token scanner and admit only explicit Schema.org contexts after each
shape has a task-based fixture and a clear, non-guessing action contract.
