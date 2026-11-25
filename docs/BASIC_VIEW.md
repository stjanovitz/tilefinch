# Basic view

Basic view is Tilefinch's fallback for a page whose server-sent content is
useful but whose scripts failed. It shows a simple, script-free copy of the
page that keeps its links, tables and search forms working. This page is the
reference for PSP owners who want the details and for contributors; the
[user guide](USER_GUIDE.md#reader-mode-vs-basic-view) covers everyday use.

Basic view is deliberately narrower than a second browser engine: the page's
own DOM stays in place, and a compact copy extracted from it gives a
predictable presentation when you choose it.

Open **Menu → Page tools → Basic view** to switch between the original page
and Basic view. On a page where scripts stopped before leaving useful content
or actions, Tilefinch can also switch to Basic view, or offer it, by itself;
see [Automatic fallback](#automatic-fallback).

### How this differs from Reader mode

[Reader mode](READER_MODE.md) makes a page that works easier to read: it keeps
only the main article, listing or media and drops navigation. Basic view
rescues a page that does not work: it keeps nearly all of the page's real
content, including navigation links, tables and simple search forms. Both
drop the page's scripts and styling, and a page has only one of them at a
time. [Reader mode vs Basic view](USER_GUIDE.md#reader-mode-vs-basic-view)
compares them side by side.

The extracted copy keeps, in document order:

- headings, paragraphs, lists, links and common inline semantics;
- images, figures and captions;
- tables and code blocks;
- details/summary content;
- collapsed sections: a region hidden with `hidden=until-found`, or named by
  the `aria-controls` of an `aria-expanded="false"` control just before it.
  Basic view stops the page's scripts, so that control could never open it;
- explicitly authored, labeled `GET` or search forms.

Navigation and site-banner landmarks (`nav`, `role="navigation"` or
`role="banner"`) become closed, expandable groups in place, labeled with the
page's `aria-label` when supplied. Their links remain available without
JavaScript, while the main content and search form no longer sit behind
several screens of menu links. Ordinary inline word fragments stay joined;
adjacent independent links get a word boundary. Generic block wrappers such
as `div` are dropped but still end a line when text sits on either side, as
unstyled HTML would show them, so a story card's label, headline and summary
stay on separate lines instead of running into one paragraph; a paragraph
inside a link is flattened the same way, so a menu item stays on its
bullet's line. A select that is not kept is omitted rather than printing its
entire option list.

Tables used to arrange navigation or page columns become ordinary stacked
blocks, instead of squeezing stories into narrow equal-width cells. Tables
with data headers or captions keep their structure; nested data tables are
classified separately from an enclosing layout table.

The simplified presentation resets the site's sizing, text metrics and
generated decorations on extracted content. Image width hints remain
available so an illustration awaiting download keeps a usable layout target.
If an image is unavailable, its alternative text reserves space before the
caption or following paragraph instead of painting over it. Small images
keep their settled widths from the original layout when it still matches the
document. Small header images keep alternative text on one clipped line
rather than wrapping vertically; larger image descriptions may still wrap.
CSS-hidden alternate images are omitted, except inside recoverable
disclosures; hidden prose remains recoverable. Meaningful embedded images
remain visible. Lazy images use the same source selection and resource
permissions as the full page, including after a view switch; the browser
never guesses an alternative image URL.
On a page loaded without its scripts (such as a site that opens in Basic
view by default), the content a page provides in `<noscript>` for browsers
without JavaScript is shown by the page and kept by Basic view, often the
only full-size pictures of a lazy-loading front page. On a page that ran
scripts, `<noscript>` content stays hidden in both, as in any browser.

Basic view does not infer behavior from button text, classes, test IDs or
click-handler-shaped markup. `POST` forms, buttons outside a form, disabled
controls, downloads and opaque application commands never become new
actions. A kept search form is limited to authored text/search fields,
select/textarea values within the limits below, hidden GET parameters and
explicit submit controls. Multiple-selection controls are refused rather
than collapsed to a different single-value submission. Inherited disabled
state is honored when form wrappers are flattened, and authored read-only
fields remain read-only.

## Limits

Basic view is limited by what it shows, not by how much markup precedes the
content, and the limit grows with the memory the page has left:

- Every Basic view may show at least 512 nodes and 256 KiB of escaped
  markup.
- Beyond that it may show one more node for each KiB the page budget has
  free above a 4 MiB reserve, up to 8,192 nodes. A shown node costs about
  1 KiB once laid out, and the reserve covers what showing any view costs
  whatever its size, plus a margin.
- The markup limit follows the node limit at 256 bytes per node, from
  256 KiB up to 1 MiB, which it reaches at 4,096 nodes; above that the
  markup limit, not the node limit, caps the view. The markup buffer starts
  at 64 KiB and grows only as far as the page needs.

Long articles get the larger view only when there is free memory for it;
pages without that headroom get the same smaller, explicitly shortened view.
When you ask for Basic view on a page whose running scripts hold that
memory, and the view comes out shortened because of it, Tilefinch stops the
page's scripts first (Basic view stops them when it is shown anyway) and
prepares the view again with the memory they held.

Both limits are fixed when preparation starts, so the same page and memory
state always give the same view. Preparation stops walking the source as
soon as the view is full. The walk itself stops after 65,536 source nodes,
the limit of every other whole-document walk, so a news front page whose
first story sits behind thousands of layout wrappers still gets a Basic
view. Basic view keeps at most eight forms and 32 controls.

A page larger than the limit gets a balanced prefix that ends with
"Basic view shortened to fit this device." The cut never splits a form: a
form the limit reaches is withdrawn whole, because a form missing the
controls after the cut would submit a different query than the page's own.
Every other omission is a refusal: a form, control, select-option or anchor
limit installs nothing. The original page's DOM, form state and navigation
history are never replaced.

Preparing a Basic view does not change pixels. Tilefinch offers Basic view
only after the page's own work has settled and the current page's scripts
have provably failed. The original page must be blank or have no usable
link or control, unless memory limits prevented the page's scripts from
starting; in that case, surviving header links do not mean the application
works. Ordinary widget errors on a useful page leave it alone.

## Automatic fallback

**Menu → Settings → Browsing & input → Basic view fallback** decides what
Tilefinch does by itself when a page qualifies as above:

- **Automatic** (the default) switches to Basic view and says so.
- **Ask** leaves the original page on screen and opens the recovery sheet,
  with **Show Basic view** listed right after Retry. A declined offer is not
  repeated for that page.
- **Off** never switches to or offers Basic view by itself. The Page tools
  item still works, and the separate blank-page Reader fallback still
  applies.

A page qualifies when its scripts failed or were stopped and it is left blank
or without a usable action, or when a script-memory refusal prevented it from
starting even though navigation links survived. That covers:

- pages whose scripts were stopped by the parser time limit or by repeated
  failures, or ran out of memory, including the case where an anti-flicker
  style hid the page and the script that would reveal it never ran (hidden
  content paints nothing, so the page counts as blank even though its DOM
  holds links). Tilefinch never matches particular sites or vendors;
- scripts that fail after the page was shown: a page is examined when it
  first appears, and once more when a later script turn fails or its scripts
  are stopped after a failure;
- a script refused JavaScript memory after the page was shown while the
  page's scripts keep running (enough memory was left to keep them; how the
  page handled the error does not matter,
  [the rule](engineering/SCRIPT_MEMORY.md#exhaustion)): the page is examined
  once more if it is then blank or has no usable action. A page that keeps
  its content and links after such a refusal is left alone;
- script loading refused under memory pressure while the page was being
  built, or scripts stopped at their memory limit. This uses what the
  current page's memory accounting reports, not a site's name or its error
  text, and does not carry over to the next page;
- a script refused memory after the page was shown, such as an ad script a
  page adds once it has loaded: like the case above, the page is examined
  once more only if it is then blank or has no usable action. A page that
  keeps its content keeps its scripts that already ran, and the status says
  "A large page script was skipped: not enough memory";
- large pages, which get a shortened Basic view.

Pages that are still working are given a short time to settle first, so a
page that reveals itself late is left alone.

Heavy pages ([user guide](USER_GUIDE.md#heavy-pages)) meet this fallback in
two places. **X Stop page scripts** on a heavy page's status stops its scripts
like a page that ran out of memory, so a page left blank or without a usable
action then qualifies here. A page whose app cannot fit gets the recovery
sheet titled "Too heavy for the PSP", which has no Retry, so **Show Basic
view** comes first (under Off the sheet leaves it out). An app shell you
chose not to start is left as the server sent it: its scripts are intact, so
it does not qualify, and Basic view of an empty shell would show nothing
anyway.

When a page fails in a way the recovery sheet reports, its choices include
Basic view where it can help (not under Off):

- **Show Basic view** when the failed page is still loaded, its DOM exists
  and a Basic view is prepared or can be prepared for it.
- **Reload in Basic view** when a navigation failed after its whole document
  arrived (an HTTP 2xx response that could not be built into a page, for
  example under memory pressure). Tilefinch reloads that address once with
  page JavaScript off, presents the result in Basic view, and restores the
  site's JavaScript setting for later loads. Network, TLS, HTTP-error and
  cancelled loads do not offer it.

## Opening in Basic view by default

Under **Automatic**, a site whose page's scripts ran out of memory on an
earlier visit opens in Basic view. The signal is that site's own earlier
failure on this PSP, not a guess from a page's size or name. Tilefinch
remembers such a site by name only, the last 8, while **URL history** is on
(see the [user guide](USER_GUIDE.md#heavy-pages)). Turning URL history off,
forgetting a history address, or **Menu → Page tools → Site information →
Reset permissions** forgets it. **Ask** and **Off** use the **Load without
scripts?** offer instead.

The next load of any of the site's pages runs with page JavaScript off, so it
needs a fraction of the memory and time, and is shown in Basic view under the
note **BASIC VIEW: SCRIPTS RAN OUT OF MEMORY LAST TIME** with **X Try full
page  O Keep Basic view**. The note takes Cross and Circle only the first
time a site opens this way in a session; on its later pages it is a plain
status, and a press belongs to the page. **Menu → Page tools → Try full
page** (right after **Basic view**, only on such a page) does the same as
Cross at any time.

Every page load the browser starts is covered, including the page shown at
startup (the restored last page or a custom homepage) and a tab switch that
reloads its page. These are not:

- a form's POST result: it is the server's answer to the form, and neither
  **Try full page** nor the reload below could repeat it, so it loads as the
  site's JavaScript setting says;
- a site set to always open in Reader (**Menu → Page tools → Site
  information → Permissions & controls → Always Reader**), which opens in
  Reader instead;
- a page shown afterwards by another route (an offline page, a native page,
  a restored tab), which does not offer **Try full page**;
- a URL given on the command line, and the trace and validation boots.

Basic view is used only when it keeps the script-free page. The page loads
again at once with its scripts when:

- Basic view would leave out a visible form control (an amount box in a form
  it cannot keep, a POST form), including the fields of a form the view's
  size limit cut short or withdrew;
- the page has almost nothing to read without scripts (fewer than 512 bytes
  of text: an app shell that says to enable JavaScript);
- a redirect took the load to another site that has no note of its own.

The requested site then gets the **Load without scripts?** offer instead,
for the rest of the session and, while **URL history** is on, for later
sessions too. The profile remembers the last 8 such sites by name and
forgets them the same way as the notes above.

**Try full page** reloads that address once with scripts on. When that full
page loads, the site's note is forgotten and its pages open normally again;
if its scripts run out of memory again, the note returns. A redirect within
the site, or to another noted site, is shown in Basic view too, because the
load ran without scripts wherever it ended. Going to another page before the
full page loads drops the request. The site's JavaScript setting is never
changed: the load that opened in Basic view only ran without scripts, and the
next load follows the setting again, including after a load that could not
start or failed.

## Switching and fragments

Switching between the original page and Basic view keeps your place at the
nearest source position. Extracted IDs and legacy named anchors use a
`tilefinch-extracted-` prefix. Fragment URLs stay exactly as authored;
same-document navigation prefers the prefixed target while an extracted view
is active, then falls back to the original ID or legacy named anchor. The
original IDs remain unchanged for switching back.

Reader and Basic view share one extracted-view slot, so Tilefinch never
keeps two full copies of the same page. Reader remains the focused article
or listing view; Basic view is the broader fallback that keeps the page's
actions. Either view can be turned off to return to the original page. To
replace one kind with the other on the same document, reload first; this
keeps memory use in proportion to the page and prevents stale JavaScript
node references.

Returning from Basic view to the original page does not restart its scripts.
Reload the page to run them again. This is intentional: Basic view is the
script-free recovery view, not an alternate live view of an application.

## Structured data

Basic view uses only explicit DOM semantics. It does not turn arbitrary JSON
`contentUrl`, `SearchAction`, product metadata or application state into
controls.

## Engineering notes

- Preparation and presentation are a two-phase transaction. Tilefinch first
  retains the exact native root, then applies the Basic stylesheet. Only
  after that stylesheet and relayout succeed does it activate the view and
  retire the current page's author realms. This prevents delayed scripts
  from rewriting the trusted-looking Basic links or forms, while a failed
  presentation leaves the original page and its JavaScript realm untouched.
  The one exception is a requested view shortened by its memory bound while
  the page's realms hold at least 1 MiB of heap: the first extraction is
  discarded before anything is presented, the realms are retired as by Stop
  page scripts (not recorded as a memory stop), and the view is prepared
  again. A failure after that leaves the raw page, without its scripts.
  The configured JavaScript policy is unchanged for the next navigation.
- Admission builds one temporary extraction and commits that same buffer
  only after the limit checks pass. A failed DOM refresh removes the clone
  and its body marker before returning.
- Scratch and clone ownership are charged to the page's existing `Budget`.
  Source traversal and text copying have a cooperative checkpoint every 128
  nodes.
- Qualification reads current-page allocator and admission facts. Live
  author work gets a settle window before a page is examined.
