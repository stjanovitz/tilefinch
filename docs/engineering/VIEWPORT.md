# Viewport and page zoom

This page is for engineers changing how a page's layout viewport is chosen or
mapped onto the 480x272 screen: meta viewport handling, the Page zoom
setting, and the code that converts between CSS and device pixels.

## One viewport value

Viewport resolution and CSS/device conversion are owned by one immutable
`ViewportContext` value (`include/tilefinch/viewport.h`). Style, layout,
JavaScript, navigation, controller input and rendering all receive the same
value. CSS media queries and viewport units, JavaScript viewport globals, root
CSSOM geometry, scrolling, hit testing, fixed and sticky overlays and tile
composition share its CSS coordinate space; only the renderer and the
controller's device-pixel input convert through its scale.

`viewport_context_resolve()` reads the page's meta viewport
(`document_mobile_viewport()`):

| Declaration | Layout width |
| --- | --- |
| `width=device-width` | the device width (480) |
| `width=N` | N, when it is 240 to 1024 |
| `initial-scale=S` with no usable width | device width / S, when that is 240 to 1024 |
| none | the 980 px legacy layout viewport |

Root horizontal overflow is retained, and a neutral fixture covers it. When
the layout and device widths differ, the renderer builds a bounded
device-scaled visual clone of the layout, the same one a desktop page uses.
Replacing a scaled tile-cache layout is transactional: if its visual clone or
a required overlay allocation fails, the previously renderable layout and
tiles stay active and the caller gets a failure result.

## Page zoom

Page zoom (**Settings → Appearance → Page zoom**) sets how large a CSS pixel
is drawn. Its steps are 50, 67, 75, 80, 90, 100, 110, 125 and 150%
(`viewport_page_zoom_steps`); 100% leaves the resolved viewport alone.
`viewport_apply_page_zoom()` runs after the meta viewport is resolved and
computes the zoomed width `device width x 100 / zoom` (960 CSS px at 50%, 640
at 75%, 384 at 125%, 320 at 150%):

- **Zoom below 100%** uses the larger of the page's own width and the zoomed
  width. A page that already asks for a wide viewport is never shrunk a
  second time: a page with no meta viewport (980 px) is unchanged at every
  step, and a `width=800` page stays 800 px until 50% asks for 960.
- **Zoom above 100%** narrows only a page whose width is the device width
  (`width=device-width`, `initial-scale=1`). A page with a fixed width keeps
  fitting the screen, because Tilefinch has no sideways scrolling.
  Explicit numeric widths retain that provenance even when equal to the
  device width (for example `width=480`).
- The CSS height follows the device aspect ratio, and the scale becomes
  `device width / layout width`, so `viewport_css_to_device()` maps the whole
  layout width onto the screen.
- A declared viewport (the lab's fidelity override) ignores zoom.

The zoomed width is never wider than 960 CSS px, so a zoomed page never needs
a larger layout or visual clone than a legacy desktop page already does.
Tiles are device pixels and their count does not change; text drawn above
100% rasterizes glyphs at up to 1.5 times their CSS size.

### Applying a change

The engine keeps the zoom in `NavigationSession.page_zoom_percent` (0 reads
as 100). Every navigation resolves its stream and committed viewport at that
zoom (`navigation_resolve_page_viewport()`), so the next page needs no second
layout. `browser_engine_set_page_zoom()` changes only the stored value.

`browser_engine_apply_presentation()` changes the open page in place. It runs
the same presentation transaction as a text-size change: the user sheet, the
stylesheet (media queries see the new width), the layout and the render shell
are rebuilt together, and the scroll anchor is carried across through device
pixels. The script runtime's viewport is updated, so `innerWidth` reads the
new width, and VisualViewport geometry follows it synchronously. A successful
viewport change queues coalesced Window and VisualViewport `resize` stages.
Each finishes its native listener microtask checkpoints before the next
stage, including `onresize` handlers; a rollback queues none. The diagnostic
Mobile Safari identity keeps reporting a VisualViewport scale of 1.
If any step is refused,
typically for memory, the transaction rolls back the viewport, the zoom, the
sheet and the layout together, and the page stays as it was. A zoom change
that resolves to the page's current viewport with an unchanged sheet (a
desktop page at most zoom-out steps) only stores the zoom; asking again for
the zoom the page already has always relays out.

On the PSP, a Left/Right press on the row updates the shown value at once,
and the page is relaid out when the choice has rested for 400 ms
(`psp_app_page_zoom_settle()`), so stepping through several values costs one
relayout. The apply uses the existing native-input cooperation scope, and a
choice that returns to the current viewport does no rebuild. Zoom-only changes
must retain all previously applied author sheets or refuse the replacement.
The setting is saved only once a relayout
succeeds; a refusal restores the previous value and shows **PAGE ZOOM
UNAVAILABLE**. The profile stores it in its own `PAGEZOOM` record; a profile
without one reads 100%.

Images chosen from `srcset` for a narrower layout keep that choice after
zooming in until the page is loaded again.

### Reader mode and Basic view

Reader mode and Basic view lay out at 100%: they are already one column at the
screen width, and their text size row (**Extracted text**) sizes them. A zoom
chosen while one is shown is saved without a relayout and takes effect when
the view is left.

## Tests

`tests/test_page_zoom.c` covers the steps, the zoom rule for device-width,
legacy, fixed-width and narrow pages, its composition with meta viewport
declarations, and a zoomed page through the engine: hit testing, the focus
rectangle, scrolling, page-down, the fixed layer, rollback when the render
shell is refused, and the next navigation inheriting the zoom.
It also covers synchronous script viewport reads, post-commit resize ordering
and coalescing, rollback without events, retained external-sheet admission,
cancellation and no-op zoom requests.
Native delivery tests include long promise chains, handler attributes,
captured event marking, one-shot listeners and script-dispatched scrolling.
`tests/test_browser_profile.c` covers the `PAGEZOOM` record and
`tests/test_psp_ui.c` the row's cycling and position.
