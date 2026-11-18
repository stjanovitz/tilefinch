# UI during page work

This page is for engineers changing how the PSP app keeps native chrome and
input live while page work runs: the owner-thread cooperative scope during
long scripts, and the callback-thread loading-UI supervisor that draws chrome
during loads and media opens. The rules are summarized in
[Architecture](../ARCHITECTURE.md#input-during-long-scripts) and under
[the mutation-owner rule](../ARCHITECTURE.md#2-the-browser-graph-has-one-mutation-owner);
this page holds the mechanism. Input-script timing records are in
[Input script reference](INPUT_SCRIPT_REFERENCE.md#input-during-page-work).

## Owner-thread cooperative scope

Post-load PSP runtime work arms a lazy, **owner-thread-only** cooperative UI
scope. At a checkpoint after 8 ms, the scope keeps the last completed engine
frame and copies the UI state; it then samples input at most once every
16 ms. Direction and activation presses enter the existing four-entry queue,
with visible queued-input feedback, and replay once the task returns; Circle
requests cancellation. Neither the callback-thread presenter nor
resource-worker hooks can enter the scope, native WebGL synchronizes its GE
list before returning to a checkpoint, no controller or DOM action runs inside
a suspended mutation, and no framebuffer ownership changes hands.
Page-controls capture bypasses this path so a game keeps its own input loop.

This is cooperation, not preemption: a native call with no checkpoints can
still block feedback, and page actions wait for a safe task boundary.
Validation reports the first checkpoint gap, input-to-presentation latency,
and every call of 100 ms or more that serviced no checkpoint; a queued press
alone is not a time-to-interactive pass.

## The loading-UI supervisor

The PSP app reuses its mandatory firmware callback thread as the loading-UI
supervisor (`src/psp_app/psp_app_runtime.c`). While a supervised scope is
active (a navigation load or a media open), it presents the immutable page
frame the scope published and draws chrome from a private copy of the UI state
taken when the scope began. It owns no engine state and never reads the
engine. The interface is in `src/psp_app/psp_app_internal.h`.

### The presentation fence

The supervisor's presentation fence (`psp_presentation_fence_acquire` and
`_release`) is the one lock between the browser thread and the supervisor.
Held, the supervisor neither presents nor samples the pad. Acquires nest on the
thread that holds it, and each true acquire is released once.

- **Glyphs.** The chrome glyph cache is filled on the owning thread when faces
  are bound (every printable ASCII glyph at each UI scale in use); a miss on
  any other thread draws the built-in bitmap for that tick. Binding and
  filling hold the fence.
- **Modal loops.** Every modal loop on the browser thread that samples the pad
  and presents by itself (a page's alert/confirm/prompt, the keyboard for the
  address bar, Find, page fields and the developer URL, a screenshot's
  capture) holds the fence for its life, so the supervisor neither draws over
  it nor reads its presses.

### Data the copy borrows

The copy still points at browser-thread data: the HOME, Library and Find
views, and the Library item and profile strings its rows borrow. Each write to
that data while a scope may be active holds the fence for the write alone (a
view rebuild, a Library replace, rename or removal, a bookmark or history
edit), and any Memory Stick I/O it needs (an icon read, a fingerprint probe,
the index save) runs outside it. The library takes the fence through its
`fence_acquire`/`fence_release` hooks (`include/tilefinch/offline_library.h`).

Appending a Library item needs no fence, since no row points past the count
its list was built with. The Tabs view needs none either: no scope begins
while Tabs is the screen, because it pauses page work and frame pumps and an
action chosen there closes it first.

### Preview sheets

A preview sheet (an offline-app preview or recompile offer, a file offer, a
ZIP install) is freed when it is answered, and the copy can still show it: a
pending load's navigation pump begins its scope again over whatever screen is
up once another scope (the sheet's own preparation, say) has retired it. The
offline store calls its `release_view` hook just before freeing a preview, and
the PSP browser's hook (`psp_work_cooperate_release_view`) takes the fence and
drops the sheet from the copy, which then shows the page until the scope ends.

### Media opens

A media open's scope copies the player state the same way. How the session's
presentation is fenced, and how presses the supervisor takes reach the player
exactly once, is in
[PSP media session](PSP_MEDIA_SESSION_STATE.md#supervised-media-opens).

### One receiver per press

The supervisor samples the same pad as the browser loop, so each press has one
receiver. `psp_input_loop_edges` (`include/tilefinch/psp_input_route.h`)
decides where the loop's button edges come from on each frame: its own sample
when no supervisor runs, only what the supervisor forwarded during a
supervised load, and only the stick and held buttons during a media open whose
scope spans loop frames, because the supervisor queues every edge for the loop
to replay.
