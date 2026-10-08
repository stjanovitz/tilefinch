# Tilefinch Game Profile v1

The Tilefinch Game Profile is the authoring contract for small web games that
should run predictably on a Sony PSP. It is not a browser mode and does not
grant a page extra memory or APIs. It names the standards subset Tilefinch can
reliably provide, the hard ceilings enforced by the browser, and the patterns
that preserve input latency on a 333 MHz, 32-bit machine.

The machine-readable companion is
[`tilefinch-game-profile-v1.json`](tilefinch-game-profile-v1.json). Its integer
values are bytes, pixels, counts, or microseconds as named. Authors should use
feature detection and queried WebGL limits at runtime; the JSON is for build
tools, coding agents, and preflight checks, not page fingerprinting.

## What compatibility means

Game Profile v1 is a portable baseline, not a promise that every browser API
Tilefinch exposes is suitable for a game. A game may call itself **Tilefinch
Game Profile v1 compatible** when it:

- remains playable at 480×272 using only the reliable or bounded features
  documented here;
- handles every documented refusal and resource ceiling without hanging,
  retrying forever, or trapping browser controls;
- has a keyboard or focusable-menu path before Page controls are captured;
- can launch from an installed snapshot with networking unavailable; and
- passes the host and physical-device checks in the qualification checklist.

The words **reliable**, **bounded**, and **outside v1** are deliberate:

| Label | Author expectation |
|---|---|
| Reliable | The ordinary path is supported and should be used directly after feature detection. |
| Bounded | The API works only inside the stated count, byte, geometry, or lifetime ceiling; refusal is part of the contract. |
| Outside v1 | Do not make it necessary for play. A future Tilefinch build may support it, but a v1 game needs a fallback today. |

Tilefinch may add capabilities without changing this profile. Authors should
not infer profile support from the user agent or a Tilefinch-specific version
number. Detect the API, query WebGL limits, and keep the v1 fallback. A future
incompatible profile revision will use a new profile identifier and machine
file rather than silently changing these ceilings.

## Quick-start recipe

For a new game, this is the shortest path to a good PSP build:

1. Design the complete interface at 480×272 and make the initial Play, Help,
   and settings controls ordinary focusable HTML. Style focus with CSS
   outlines and add no focus-event listeners (see
   [Menus over a live canvas](#menus-over-a-live-canvas)).
2. Choose one primary renderer: Canvas 2D for sprite- or path-oriented games,
   or WebGL for retained 3D geometry. Keep a static help fallback.
3. Allocate entity pools, matrices, command records, particles, and audio
   voices once. Reuse them for the whole session.
4. Start audio, fullscreen, and Page controls only from the Play activation.
   Keep Start+Select reserved for Tilefinch.
5. Run one `requestAnimationFrame()` chain with at most one bounded simulation
   step per callback. Never replay a backlog of missed steps.
6. Keep steady WebGL frames structurally stable: upload static assets once,
   update changed prefixes with `bufferSubData()`, and batch repeated actors.
7. Put every install-critical asset on the same HTTPS origin, provide a Web
   App Manifest, and test the saved package with Wi-Fi unavailable.
8. Run the pressure profile and a physical-PSP action soak before describing
   the game as compatible.

As a safe first target, use a 320×180 backing surface, one texture atlas or no
textures, fewer than 32 active actors/effects, three or fewer steady WebGL
draws, short PCM effects, and no DOM mutation during gameplay. These are
starting recommendations rather than new hard limits.

### Choosing a renderer

| Game shape | Recommended path | Avoid in the frame loop |
|---|---|---|
| Brick-breaker, board game, simple sprites | Canvas 2D or small WebGL scene | pixel readback, export, DOM HUD updates |
| Charts, vector controls, drawing game | Canvas 2D retained paths/text | rebuilding long paths and shadows every frame |
| Top-down or modest 3D action | WebGL retained meshes plus bounded instancing | per-actor draw calls, buffer replacement, changing render state |
| Large maps, video processing, shader-heavy effects | Reduce the design or provide a static fallback | full-screen pixel processing, general GLSL, large streamed worlds |

The included [Prism Break 3D](../examples/prism-break-3d/) example is the
small-scene starting point. [Treadline Arena](../examples/treadline-arena/)
shows the upper end of the intended profile: retained arena geometry, a fixed
64-instance stream, an in-canvas HUD, pooled audio, installed-offline startup,
and a measured physical-device soak.

## Target and budgets

The target display is 480×272. The shipping PSP memory profile gives the whole
page 24 MiB and QuickJS 5 MiB; the engineering pressure profile lowers those
ceilings to 16 MiB and 4 MiB. DOM, script, styles, decoded images, Canvas,
WebGL resources, layout, render data, and session state share the page budget.
Reaching one subsystem's local limit does not imply that the remaining page
budget is available, and a budget refusal is a normal result a game must
survive.

### Installed-app heap

An installed app opened from the offline library starts with a larger
QuickJS heap floor: 9 MiB in the realistic profile and 6 MiB in the strict
one. On the PSP the floor is the `boot.cfg` key `app_heap_mb` (2-16, default
9, below `limit_mb`; an app never gets less than `heap_mb`). It is carved out
of the same page Budget, which does not change, and it reaches only the app's
top-level realm created by the library launch: frames, the next document in
the tab, a reload, or an ordinary navigation to the app's own URL all get the
ordinary heap again. Every page realm may already grow above its floor while
an eighth of the page Budget stays free, up to three quarters of it; that
growth still applies above the app floor. What the floor adds is a guarantee
under pressure: allocations up to it never wait for spare Budget, and garbage
collection paces with room above the live graph from the first frame.
QuickJS charges its arenas and large blocks to the page Budget's JavaScript
category; Budget categories attribute memory and are not sub-limits, so the
floor is enforced by QuickJS's own limit while every byte still counts
against the one page Budget. With that Budget held at its growth reserve,
the launched Treadline realm, about 5.6 MiB in use after its soak, can
still allocate about 3.3 MiB more (up to its floor, charged to the
JavaScript category) and is refused past it; at the ordinary floor the same
allocation is refused. `tilefinch-offline-app-launch-tests` sizes that probe
from what the game leaves and separately requires at least 2.5 MiB of the
floor to stay free, so the game cannot quietly grow into it.

The default comes from Treadline's qualification soak opened through the
offline route on the host (64-bit pointers, realistic 24 MiB profile,
1,800 frames): the QuickJS heap peaked at 5.09 MB (5.95 MB of arenas in the
Budget) and everything else at 5.60 MB (layout 2.11, resources 1.92, render
1.03, session 0.67 with script sources left on the Memory Stick, DOM 0.35),
11.52 MB for the whole page. At the ordinary 5 MiB floor the realm grew to
7.5 MiB for collection headroom. A 1 MiB game's restored code takes about
1.1 MB more heap than Treadline's. At 9 MiB the steady worst case is about
5.6 MB + 9 MiB of heap with arena overhead (~10.9 MB), 16.5 MB, and about
20.7 MB while the 4 MiB startup window is open,
leaving more than the eighth of 24 MiB the page reserve keeps. The strict
6 MiB floor gives a steady worst case of about 13 MB of its 16 MiB, again
above that profile's eighth. The PSP-1000
(32 MB) is unsupported; every supported model (PSP-2000, PSP-3000, PSP Go,
PSP-E1000) runs the same 64 MB memory mode and page Budget.

Keep each classic game script at or below the 512 KiB Game Profile admission
ceiling; the strict pressure profile retains its 256 KiB per-script limit.
On the PSP it is the `boot.cfg` key `file_kb`, which
`scripts/stage-psp-game.sh` sets to 512 for a staged game. Ordinary web pages
no longer have a fixed per-script limit: the app's default `file_kb` is 4096,
a sanity ceiling, and a page script is admitted by the memory its compile
needs (docs/engineering/PSP_ENVELOPE.md). A game should still stay within
512 KiB per script: that is what the profile promises a device can compile
next to a running game's heap. A
`file_kb` below a game's largest script quietly skips that script: its
element receives an `error` event and the player sees nothing. Listen for
script `error` and show a message rather than waiting forever on a loading
screen, as Treadline does; its `game.js` is about 400 KiB of its 512 KiB.
Installed apps may precompile at most eight classic scripts and 1 MiB of
admitted source in aggregate; the source remains packaged as the
engine-version-independent fallback. Splitting code can improve maintenance,
but it does not expand the aggregate JavaScript heap or installation budget.
The restored code itself occupies the script heap: measured on the 64-bit
host (the PSP's 32-bit pointers make it somewhat smaller), Treadline's ~690 KB
of source restores to roughly 0.79 MB of QuickJS heap before the game
allocates anything, and 1 MiB of dense synthetic code to roughly 1.8 MB, so a
1 MiB game should be designed around the remaining heap, not the package
limit.

For animation, treat 16.67 ms as the aspirational 60 Hz frame time and 33.33 ms
as the maximum steady 30 Hz frame time. Device qualification separately counts
frames above 34 ms. These are performance targets, not scheduler guarantees:
the browser may skip an animation callback rather than queue a backlog.

Page time is not wall time. On the PSP the page clock that drives
`requestAnimationFrame()` timestamps, `setTimeout()` and `setInterval()`
follows the wall clock but advances by at most one 16 ms tick per browser
loop, and never catches up. A long loop therefore slows page time: a menu
frame over a live canvas took about 50 ms on a PSP-3000 in October 2026, so
0.15 s of page time lasted about half a second. Gameplay can accept that
(a stall slows the game rather than replaying it), but UI rests, debounces
and key repeat should read `performance.now()` once per frame while they wait.

## Reliable API subset

### Canvas 2D

Tilefinch supports the common Canvas 2D path for sprites, charts, controls, and
small games:

- rectangles, paths, arcs, ellipses, Bézier curves, `arcTo()`, and
  `roundRect()`;
- fills, strokes, clipping, dashes, caps, joins, affine transforms, gradients,
  patterns, bounded shadows, and the practical Porter-Duff operations;
- real Tilefinch font metrics and glyph rendering through `fillText()`,
  `strokeText()`, and `measureText()`;
- canvas, decoded image, and `ImageBitmap` sources for `drawImage()`;
- `ImageData`, pixel readback/writeback, PNG export, `Path2D` SVG strings, and
  `Path2D.addPath()`.

One JavaScript RGBA backing is limited to 512 KiB: at most 131,072 pixels, with
authored dimensions proportionally bounded to 480×272. The renderer retains at
most four native presentation snapshots and 1 MiB across them. Eight
`ImageBitmap` objects may retain at most 1 MiB in aggregate. Paths retain at
most 256 commands. Native batches admit 64 rectangles, 16 images, or 16
path/text paint operations at once; excess work is flushed or refused without
unbounded growth.

The JavaScript RGBA backing and native presentation snapshot deliberately have
separate ownership. Do not assume that drawing-only canvases consume only
RGB565 memory. Avoid `getImageData()`, `putImageData()`, export, and frequent
canvas-to-canvas readback in the frame loop.

### WebGL 1

Tilefinch exposes WebGL 1 with a bounded fixed-function-compatible shader
translator. The practical path is indexed triangles or small point/line work,
vertex color or one 2D texture, and direct matrix transforms. The supported
extensions are `ANGLE_instanced_arrays` and `OES_vertex_array_object`.

Hard per-realm or per-submission limits are:

| Resource | Limit |
|---|---:|
| contexts | 2 |
| buffer objects / retained bytes | 24 / 512 KiB total |
| texture objects / retained pixel bytes | 8 / 416 KiB total |
| shader objects / source | 16 / 16 KiB each |
| program objects | 8 |
| vertex-array objects | 8 |
| framebuffer / renderbuffer objects | 4 / 4 |
| draws per native submission | 64 |
| effective vertices per draw | 4,096 |
| instances per draw | 64 |
| payload sources | 32 (24 buffers plus 8 textures) |
| texture size | 512×512, still subject to retained-byte limits |
| drawing buffer | 480×272 and 131,072 pixels |
| attributes / texture units | 8 / 1 |
| vertex / fragment uniform vectors | 16 / 4 |
| varying vectors | 2 |

Shader loops, derivatives, `discard`, arbitrary fragment programs, mipmaps,
stencil, general framebuffer attachments, and unrestricted blend/write-mask
state are outside v1. New textures intentionally use `LINEAR` and
`CLAMP_TO_EDGE`; query the context rather than assuming desktop WebGL defaults.
Unsupported shader shapes and excess resources report ordinary WebGL failure
or bounded context loss so the surrounding page can remain useful.

Shaders are translated to a fixed-function colour, texture × vertex colour ×
instance colour × one `vec4` uniform or constant, at link time. Control flow
that depends on per-pixel or per-vertex data, vertex texture lookups,
non-2D lookups, and a second sampled texture fail `linkProgram()`. Uniform-only
branches and colour or position math the GE cannot apply link with a
`WARNING:` program info log naming the construct and line, one
`console.warn` per program, and the `shaderWarnings` diagnostic counter. An
empty program info log means the PSP draws exactly what the shader computes.

See [WebGL authoring](WEBGL.md) for exact shader shapes, texture behavior,
instancing, antialiasing, context restoration, and the PSP GE qualification
path.

### Input, fullscreen, and visibility

The standard Gamepad API exposes one controller with the standard mapping. It
is disconnected until the user enters Page controls. A game can request entry
from its Play click with the Tilefinch extension below; Tilefinch admits it
only during a trusted top-level user activation and shows a native
**Hold Start + Select to exit** notice:

```js
await navigator.tilefinch?.requestPageControls?.(gameCanvas);
```

The optional element becomes fullscreen through the existing standards
lifecycle; omitting it uses the document root. Games must feature-detect the
extension and fall back to `Element.requestFullscreen()`.

The native notice lasts five seconds. A game that claims Page controls again
later in the same page load (each Deploy, or a resume after a pause menu) can
ask Tilefinch not to repeat it:

```js
navigator.tilefinch?.requestPageControls?.(gameCanvas, {notice: "once"});
```

`"once"` only quiets a repeat. Tilefinch always shows the notice on the first
entry of each page load, whatever the page asks, and on every manual
Start+Select entry; it never hides the **Page controls off** or **need an
active JS page** messages. The scope is the page load rather than the site or
the session: a reload or a navigation is a new context in which the player may
not remember the exit chord, and the state lives and dies with the document
instead of in browser storage. `"always"` (or no option) keeps the notice on
every entry. The accepted values are listed in
`navigator.tilefinch.pageControlsNotices`; other browsers and older Tilefinch
builds ignore the option, so the call needs no separate fallback. Holding **Start + Select** for 0.7
seconds remains the manual entry fallback and unconditional exit. Navigation, native media,
suspend, and page retirement also end capture. The firmware HOME callback is
outside page control and remains a system escape.

The page must remain navigable before capture. Use ordinary focusable buttons
for Play, Help, and settings; then accept Gamepad button 0 as the primary game
action. Tilefinch lets the user choose X or O as the physical primary button,
so never tell the player that a fixed face symbol is universal. Text that must
appear before optional page fonts are ready should use printable ASCII; do not
depend on arrows, controller symbols, or ornamental punctuation being present.

For an installed game, manifest `display: fullscreen` retracts Tilefinch's
browser chrome immediately when the user launches it from Library. Treat that
as presentation only, not as input authority: keep the initial menu focusable
and request Page-controls from the trusted Play/Deploy action. Triangle can
restore browser chrome before capture, and Start+Select always exits capture.

For a game with multiple control styles, translate them into one stable command
shape before simulation and networking. Treadline Arena's default Arcade layout
turns the nub into a camera-relative desired heading, while its Classic layout
retains independent treads; both feed the same bounded tread bits and aim
vector. This keeps physics, deterministic snapshots, and old peers independent
of a player's local preference. Treat directional face inputs as positions,
not confirm/cancel actions, and leave Start+Select unassigned so the native exit
chord remains unconditional.

Charge/release controls can use a held-fire bit without growing the packet, but
they change its meaning: version the simulation protocol and reject old peers.
Cancel a charge on pause, lost visibility, and local-player handoff. Never let
game shortcuts consume input intended for a replay-code text field.

For deterministic replays, record the post-input world-space command and bounded
step duration, plus initial seed/loadout and a simulation version. Verify a
final digest and report mismatches. Rendering must not consume the gameplay RNG.
Use fixed RAM rings for killcam frames and input logs; export/import can do
bounded menu-time work, but automatic capture must not write storage.

Aim assistance should be bounded and local. A fixed actor scan, angular cone,
and line-of-sight test are appropriate; allocation, spatial-index rebuilding,
or hidden network state is not. Send the resulting aim vector rather than a
target identity so peers do not need matching assistance settings. If an
analog control is translated to binary intent to preserve an existing packet,
say so explicitly; transmitting analog throttle is a protocol change.

The Fullscreen API is supported for one connected top-level element after a
trusted user activation. Start+Select remains the browser-owned page-control
escape chord, and HOME remains firmware-owned. `document.hidden`,
`document.visibilityState`, and `visibilitychange`
track inactive pages, native media, suspend, and resume. Stop simulation and
mute or suspend game audio while hidden; do not accumulate elapsed animation
steps to replay later.

### Web Audio

One user-activated `AudioContext` supports decoded PCM WAV effects and simple
generated waveforms. The useful nodes are buffer sources, gain, stereo panning,
and sine, square, sawtooth, or triangle oscillators. Buffer sources support
loop points, playback rate, scheduled start/stop, and `ended`/`onended`.

The native pool admits eight decoded buffers, four simultaneous voices, 16
audio nodes, and 512 KiB of decoded PCM. WAV input is PCM mono or stereo, 8- or
16-bit, from 8,000 through 48,000 Hz. Scheduling is limited to ten seconds
ahead and the mixer works in 512-frame blocks. The context starts suspended
and `resume()` requires trusted activation. Native media and PSP suspend
release the audio channel while retaining decoded effects. Compressed effects,
custom processing graphs, audio worklets, long-horizon scheduling, and PCM
readback are outside v1.

Gain and oscillator-frequency `AudioParam.setValueCurveAtTime()` copy 2–64
float32 samples into the native mixer. Linear interpolation and the final
held value continue without JavaScript ticks. The combined start delay and
duration must fit ten seconds; samples must be finite and within gain 0–4 or
frequency 1–20,000 Hz. Prepare and reuse effect curves rather than allocating
them on each shot. Use two samples for a linear pitch sweep; extra samples
only add setup work unless the effect genuinely changes shape. This bounded
subset requires a live source, a fixed gain/
panner graph, zero oscillator detune, and one curve per parameter. Cancel at
the current time before replacing a curve or switching to target envelopes;
future cancellation, overlapping curves, and general automation timelines are
not implemented. The older linear/exponential ramp methods remain immediate
value setters, not scheduled ramps.

`GainNode.gain.setTargetAtTime()` is the supported envelope: it publishes a
native target ramp for each source connected through that gain, and the mixer
thread applies it with no JavaScript ticks. `cancelScheduledValues()` works
only at the current time. Start the context from the trusted Play press:
`resume()` without user activation rejects with `NotAllowedError`, and an
automated run that never presses anything never starts game audio. Treat a
rejection as "silent until the next Play", not as a reason to retry.

### Storage

Each origin's `localStorage`, `sessionStorage`, and OPFS share a RAM allowance
that starts at 32 KiB and grows to at most 512 KiB while the device has memory
to spare. A write past it throws `QuotaExceededError`; the user may then move
the site to the Memory Stick (up to 4 MB), after which a retry succeeds.
Persistent storage is the user's choice, not an author guarantee; games must
tolerate a fresh store and a failed write. Use compact strings for settings,
unlocked levels, and scores. Do not serialize per-frame state, and batch
writes: on the Memory Stick every write appends to a log.

The bounded IndexedDB compatibility layer is useful for feature compatibility,
but v1 does not promise durable IndexedDB state across an app relaunch. An
offline game that needs durable preferences should use `localStorage` and
still provide safe defaults.

## Lifecycle contract

A game should implement these transitions explicitly:

1. **Load:** construct a small static shell first. Detect WebGL, Canvas, audio,
   and Gamepad support without starting a frame loop for a hidden page.
2. **Activate:** begin audio and fullscreen only from a user gesture. Page
   controls remain an independent user choice.
3. **Run:** maintain one `requestAnimationFrame()` chain, cap simulation deltas,
   and reuse frame-owned arrays and objects.
4. **Hide or suspend:** stop simulation advancement, suspend audio, and retain
   only bounded state needed to resume.
5. **Resume:** re-sample input and clocks. Never replay missed frames or assume
   the previous Gamepad connection survived.
6. **Context loss:** cancel `webglcontextlost` only if the game can rebuild all
   GPU resources on the same context object. Old resource objects become
   invalid after restoration.
7. **Navigate or close:** stop callbacks, release event listeners and audio,
   and allow page-owned memory to return to its baseline.

Detached canvases remain valid offscreen drawing and readback sources, but they
do not publish page damage until reconnected. A game must not poll or spin while
waiting for publication, audio activation, Gamepad capture, or restoration.

### Refusal and recovery contract

Every constrained operation needs a finite failure path. In particular:

- if WebGL creation, shader compilation, linking, allocation, or restoration
  fails, stop issuing WebGL commands and switch to Canvas 2D or the static help
  surface;
- if an optional effect exceeds an instance, particle, voice, or command pool,
  drop that effect before a player, projectile, input sample, or required UI;
- if audio activation is refused, continue silently and offer a later
  focusable **Enable audio** action rather than retrying every frame;
- if Page-controls or fullscreen is refused, preserve keyboard/focus controls
  and keep the browser escape instructions visible;
- if an installed resource is unavailable, show which optional feature is
  missing and keep the package launchable; and
- after context loss, rebuild from retained CPU-side assets only once. A
  failed restoration becomes the renderer fallback, not another loss loop.

Use a single game-owned status surface for these messages. Do not replace the
document with an error-only page: Play/Retry, Help, renderer fallback, and exit
must remain reachable.

## Performance recipes

- Prefer a 320×180 or 240×136 backing buffer scaled to the 480×272 display.
- Keep one animation chain and a fixed-size entity pool.
- Advance at most one bounded simulation step per animation callback. A
  catch-up accumulator that replays several missed fixed steps makes an
  already late PSP frame do several complete updates and can sustain its own
  latency. Cap the variable step at a value proven safe for the game's fastest
  collision sweep; let a longer stall slow game time instead of replaying it.
- Reuse typed arrays, matrices, command records, particles, and audio nodes.
- Upload static geometry and textures once; patch only changed buffer ranges.
- Keep stable vertex-array objects and draw shapes. Tilefinch validates a
  repeated VAO/program/index layout once and reuses a bounded packed-command
  template while buffer contents change through `bufferSubData()`. Replacing
  buffer storage, relinking a program, changing attribute layouts, or changing
  render state deliberately invalidates that work.
- A retained command list preserves command structure, not application
  intent. Before every replay, update every participating program's live
  uniforms—including the camera/view-projection matrix used by instanced
  geometry. Updating only the ordinary-scene program can leave actors and
  box-shaped scenery fixed in screen space while the floor moves. Qualify a
  retained renderer with a pixel-space test that follows one static landmark
  and one moving instance through a camera change.
- Batch by texture and render state. Retain one unit mesh and use
  `ANGLE_instanced_arrays` for repeated boxes, sprites, particles, or actors;
  split larger streams into the profile's bounded 64-instance draws.
- Temporal edge history is safe only when the prior transform of every
  contributing primitive is available. Use the bounded spatial AA path for
  moving instances unless the game retains and supplies their previous
  transforms; reprojecting current instances through an old camera creates
  ghost edges and apparent jumps.
- Render frequently changing HUD text, meters, and indicators as one retained
  in-canvas mesh. Keep ordinary HTML for menus, pause/help surfaces, and
  accessibility, but do not mutate DOM text, classes, or transforms every
  frame. Quantize meters and angles so their retained mesh changes only when
  the visible result changes.
- Draw small in-canvas text as whole-pixel geometry. A 320×180 canvas
  reaches the screen through the nearest 2:3 scale, so a one-pixel canvas
  stem is one or two screen pixels depending on its column, while a stem two
  canvas pixels wide is always three. Fragment shaders are translated to
  fixed function (the fragment colour is the vertex colour, or the texture
  times it), so per-fragment logic such as a shader-drawn text shadow cannot
  run on the PSP: a branch on a varying fails `linkProgram()` with the line
  named. Emit it as geometry or leave it out. Treadline's 5×7 HUD font is a
  worked example.
- Keep player input and the player-controlled actor at presentation cadence.
  If a bounded simulation is still expensive, distribute independent bot AI
  or background planning across frames; never delay local input to make that
  schedule work. Never advance collision or actor state while deliberately
  publishing the preceding world transform: the next complete frame reads as
  a teleport or wall crossing. Defer independent optional work such as a HUD
  text rebuild instead.
- Treat a third-person camera as a 3D line-of-sight problem. Clamp its focal
  point to the playable arena, test the low portion of the target-to-eye ray
  against bounded occluders, and apply immediate bounded retraction with
  smooth expansion. Testing only the ground projection can collapse an
  elevated camera; testing only the eye permits a nearby wall to fill the
  viewport.
- Give AI visibility tests a cheap segment/box broad phase before division.
  Fixed-position destructible cover can use a startup-owned candidate grid;
  keep live active flags and the exact narrow-phase test, include the largest
  admitted actor radius, and fall back for larger queries. Test cell edges,
  every arena, and cover destruction against the original full scan.
  Rank route candidates before testing their visibility, retain a checked
  waypoint while its grid cell is unchanged, and invalidate on scenery edits.
  Goal-based distance fields can share bounded storage across actors when
  their occupancy rules match; key them by goal and grid revision and check
  the key again after eviction. Keep actor-specific waypoint/visibility state
  separate. Preserve traversal order when simplifying a flood-fill loop.
  Expensive bank-shot strategy need not run at tread cadence: retain per-actor
  results, recheck on a bounded timer and before firing, and cancel telegraphs
  immediately when smoke breaks visibility. Never share mutable plan scratch
  between actors.
- Prepare a ray's direction, absolute direction and bounds once, not once
  per candidate. A bounded conservative mask can narrow candidates, but the
  exact predicate must still decide every intersection. Retest a remembered
  blocker against the current ray and active geometry *before* constructing
  the full candidate mask; never reuse the previous visibility answer.
  Give independent bank-shot faces/legs and waypoints separate blocker slots.
  Preserve original first-hit order for projectiles. Existing point grids may
  beat a more general interval-table lookup for small obstacle lists.
  Validate against identical captured queries, including destruction, gates,
  cell edges and near-axis arithmetic; compare each result/identity, not just
  a digest. Time setup separately from query batches and gather visit counts
  in a separate run so counting does not manufacture a speedup.
- For finite distances and radii, multiply a value by itself rather than
  using `value ** 2`: the PSP interpreter's exponent operation enters
  software `pow()`, unlike multiplication. Measure action frames as well as
  averages; a cheap-looking numeric expression can dominate a contact loop.
- Cache final scalar AI parameters when their wave/settings key changes,
  rather than caching only interpolation weights and repeating soft-float
  arithmetic for every actor. Preserve the original arithmetic at refresh
  time and check every parameter and difficulty against the uncached formula.
  In collision loops, reject on one squared axis before computing the second
  when it cannot change the exact contact result. Fixed-position crates can
  use the same candidate-grid recipe as cover; build after placement, retain
  live active flags, and use a full scan while placement is in progress.
  For bounded nonnegative grid coordinates, integer conversion can replace a
  native `Math.floor` call; reject out-of-range coordinates before conversion
  so truncation cannot turn an outside point into cell zero.
  For clamped grids, clamp the scaled coordinate before truncating. Preserve
  non-finite input behavior and verify cell boundaries against the original
  formula; a bitwise conversion alone wraps large coordinates.
- Prepack immutable instance tints at startup; keep animated flash/telegraph
  tints in per-entity scratch. Preserve intermediate float rounding when
  comparing the instanced and expanded fallback paths. Do not build meter
  geometry or colors for entities whose presentation does not include it.
- Skip updates for expired zero-valued timers. Retain stationary surface
  heights only when all placement, arena-change and network-correction paths
  initialize them; never reuse them across moving terrain or a new arena.
- Keep readback, image export, DOM layout queries and mutations, storage
  writes, and asset decoding outside the animation loop.
- Pre-create bounded audio voices and schedule supported native gain
  envelopes rather than ticking fades in JavaScript. Profile cancellation,
  gain reset, attack and release separately: these costs are nested inside
  shot/impact work, not additional frame phases. Keep isolated sound bursts
  outside displayed-gameplay measurements. Bootstrap automation should reuse
  private graph-membership records without per-segment callback closures.
- Preload only the bounded assets needed for the next screen. Show progress or
  a useful menu while optional resources remain unavailable.
- Measure on a physical PSP. PPSSPP is a correctness and lifecycle gate, not a
  performance oracle: at its normal clock it is faster than the device, and
  at a fixed 111 MHz (`TILEFINCH_PPSSPP_CPU_MHZ=111`) it is slower on
  CPU-bound work. Use 111 MHz as a deterministic pressure bracket; the
  physical device decides.
- For a fixed navigation grid, prepare exact cell/object membership when the
  arena is placed. Destruction and gate changes can then combine those
  retained memberships with live active-object bits instead of repeating
  geometric tests for every cell. Keep the original publication slices and
  revision boundary: faster preparation must not expose a partial field or
  change AI reaction timing. This does not replace fresh tank or projectile
  collision queries.

These are measured constraints rather than stylistic preferences. The earlier
Treadline Arena physical-PSP qualification, before the Daily/replay/boss and
expanded-AI additions, sustained 8,701 displayed gameplay
pipelines across a 5.5-minute repeated-action soak. Median/p95/worst pipeline
times were 33.221/33.247/33.280 ms, with no pipeline above 34 ms. Native WebGL
averaged 2.998 ms (3.650 ms p95, 3.874 ms maximum), canvas conversion averaged
about 3.9 ms, and the JavaScript callback averaged 11.474 ms with a 17.861 ms
maximum. Peak page ownership remained at the 9.88 MiB load-time high-water
mark, no allocation was refused, and post-warm-up retained growth was 45.6 KiB;
about 4.1 KiB of that was the bounded draw-template state. Authors should
repeat the measurement for their own scene rather than assume these timings.
The expanded game requires fresh hardware qualification; this historical
result is not a performance claim for its new modes. Later component runs on
a PSP-3000 (validation build, October 2026) are encouraging but
are not that qualification: an Onslaught boss wave with real input and audio
missed 3 of 2,176 two-vblank deadlines (readiness p95 28.2 ms, maximum
33.7 ms), and the long soak 20 of 2,046 (with the Enhanced lighting, now
Treadline's only look).

The sections below collect what building Treadline taught about the PSP.
Figures are dated physical PSP-3000 measurements. Several of
the browser paths involved are still being optimized, so treat them as the
current cost model rather than promises.

### Clocks and attribution

`performance.now()` costs 65-70 µs per call on the PSP (`Date.now()` 73 µs,
an ordinary JavaScript call about 2 µs, a soft-float add, multiply or compare
0.5-1.0 µs). A frame instrumented with phase timers ran about 10 ms slower,
so phase clocks cannot attribute a 33 ms frame's tail. Keep phase timers out
of shipping frames; spend a clock read only where a decision depends on it,
as Treadline does twice per frame to decide whether to defer optional HUD
work. Count events per frame (shots, hits, particles, route
searches) and join them to the browser's own frame record instead, or time
one isolated operation many times outside gameplay. Reading
`AudioContext.currentTime` costs about 0.1 ms; read it once per scheduling
decision.

### WebGL cost model

- **Publication.** An eligible canvas is published by the GE by default: one
  opaque WebGL canvas filling the viewport at the 2:3 scale (a 320×180
  backing shown at 480×270) with nothing composited above it — no HTML
  overlay, fixed or sticky element, scroll thumb, or focus outline. The GE
  scales it into the back buffer in about 0.7 ms instead of the CPU's ~5 ms of
  conversion and copy, pixel-exact. On Treadline's long soak this cut missed
  deadlines from 4.1% to 0.5%. A DOM HUD, menu or toast keeps that frame on
  the CPU compositor, so draw per-frame layers in the canvas and accept the
  CPU path only while a menu is open.
- **Instances.** Native WebGL work on a 64-instance boss wave is about
  2.1 ms per frame, the largest part (0.9 ms) colouring instance vertices.
  Instances of one draw whose tints are bit-identical share one coloured
  copy, so a small quantized tint palette is cheaper than per-instance colour
  variation; unchanged retained geometry drawn without instance colours is
  read by the GE in place. The JavaScript side is real too: writing eight changing matrix
  words and four tint words for 64 instances into `Float32Array`s took about
  1.2 ms. Write constant matrix words once, skip actors that did not move, and
  prefer fewer, larger draws.
- **Effects.** Measured per event: one sound start 0.84 ms, a six-particle
  burst 0.54 ms plus about 0.05 ms per live particle per frame, a decal
  0.14 ms. A hit frame stacks several of these, which is where the remaining
  late frames sit. Give optional effects a per-frame budget and drop them
  before actors, shells or input.

### Allocation and strings

QuickJS frees acyclic garbage by reference counting as soon as it is dropped,
but a cycle collection walks the whole live heap and can cost more than a
whole frame on the PSP. Treadline's measured gameplay frames allocate zero
JavaScript bytes: pools, typed arrays and retained closures are created at
startup and reused. Tilefinch's QuickJS appends `s += piece` in place when
`s` is a function-local variable; the same append to an object property,
closure variable or global copies the whole string every time, which turns
string building quadratic. Build strings in a local and assign once, and do
not build them per frame at all.

### Menus over a live canvas

HTML menus over a running WebGL canvas are retained as an overlay and blended
over every canvas frame. Measured on Treadline's menus (2026-10-04 and
2026-10-05):

- **No focus-event listeners.** Any `focus`, `blur`, `focusin` or `focusout`
  listener on the page, even one on `window`, makes every D-pad focus move
  dispatch all four events through script: 26-28 ms of script plus 3.6 ms of
  natives per move. With none, focus moves take the native path. Poll
  `document.activeElement` from the menu's frame loop when a screen must react
  to focus. Treadline's menu-layout gate fails when the page observes focus
  events.
- **Paint-only focus styles.** Style focus with `:focus`/`:focus-visible`
  CSS. An outline, a uniform border colour or a single zero-offset unblurred
  inset ring changes paint only: no relayout, and only the damaged rectangle
  of the overlay is repainted. A Treadline focus move is one 51 ms loop
  (press to first visible about 70 ms); a border-colour focus in a centred
  dialog over an animated canvas, 60 ms. Background, text colour, opacity,
  filter, transform or larger shadow changes take a relayout.
  `left: 50%; top: 50%; transform: translate(-50%, -50%)` centring stays on the
  paint-only path; a scaled, rotated, filtered or translucent ancestor does
  not.
- **Text changes.** A visible text change repaints only its damage but still
  costs a relayout: a Range Faults note write is a 114-117 ms loop, of which
  the relayout is 35-40 ms. Skip writes whose text is unchanged (they still
  relayout) and write a focus note once focus has rested 150 ms of wall time,
  not on every step of a held D-pad.
- **Screen switches.** A switch that changes the panel's box rebuilds the
  whole overlay in one loop: 241-318 ms of raster for Treadline's panel, a
  large share of it the 18 px glow shadow. Keep panel geometry stable between
  screens where you can, and keep large blurred shadows off panels that
  change size.

### Starting a match

Keep the Play/Deploy press light. On a PSP-3000 Treadline's first frame
without the menu arrives about 0.5 s after Deploy and steady 33 ms frames
begin about 0.58 s after it. Most of that first loop is the game's own click
turn, and about 180 ms of the turn is `music.js` building its segments: work
of exactly the kind that belongs before the press. Generated arenas are built in bounded
slices across frames behind a progress screen rather than inside the press.
Do one-time work such as level generation, geometry upload and pool warm-up
while the menu is idle or in such slices, start audio and Page controls from
the press itself, and avoid DOM changes in the same handler.

## Offline packaging

An installable game is an HTTPS page with a same-origin Web App Manifest. The
supported manifest presentation fields are `name`, `short_name`, `start_url`,
`scope`, `icons`, `theme_color`, and `display` (`browser`, `minimal-ui`,
`standalone`, or `fullscreen`). The manifest is limited to 64 KiB.

Tilefinch snapshots the committed document and same-origin response bodies the
page has already loaded. It does not crawl links, guess future assets, archive
cross-origin dependencies, or run a Service Worker. The installation preview
reports estimated size, captured resources, known unavailable resources,
display mode, theme color, and whether the operation is Install, Update, or
Reinstall. It also precompiles up to eight classic scripts from at most 1 MiB
of admitted source and stores at most 1 MiB of source-bound QuickJS bytecode.
Scripts beyond either bound are packaged source-only, as is a script whose
compilation is refused for memory. The packaged source remains the fallback
after an engine ABI change or failed bytecode restore, so authors do not ship
or depend on compiler artifacts; after an update **Library → Saved** marks an
installed game **RECOMPILE** and offers to recompile it from that source
before it is opened (see
[After a browser update](OFFLINE_LIBRARY.md#after-a-browser-update)). A precompiled script's source is not held in
memory after launch; it is read from the Memory Stick only when needed (see
[Script source on demand](OFFLINE_LIBRARY.md#script-source-on-demand)).

Hard snapshot limits are:

- 1 MiB serialized document;
- 1.5 MiB aggregate captured response bodies (about 1 MiB of script plus
  512 KiB of CSS, images, fonts and data) and no more than 32 resources;
- a 2,785,280-byte resource-pack envelope including compiler artifacts and
  bounded metadata;
- one decoded 16×16 RGBA icon in the native library;
- 12 total items across the offline library.

Keep HTML, CSS, JavaScript, fonts, images, and PCM effects same-origin and make
the game usable when an optional resource is listed as unavailable. The
snapshot can only capture responses still held by the live memory cache, so a
game larger than the **Memory cache** setting needs that setting raised
(for example to 2 MB) before it is loaded for installation. Opening an
installed app does not itself require or start a network connection. Network
work begins only when the page explicitly requests it, such as an opt-in
multiplayer action.

See [Offline library](OFFLINE_LIBRARY.md) for the storage and integrity model.

### Installation preflight

Before shipping an installable package, verify all of the following from the
installation preview rather than only from a live server:

- the name, icon, display mode, theme color, start URL, and scope are correct;
- every required HTML, CSS, script, image, font, and PCM effect is captured and
  same-origin;
- unavailable resources are genuinely optional and produce a useful in-game
  explanation;
- the estimated resource pack fits with headroom for compiled-script metadata;
- launch reaches a responsive menu before optional audio or networking work;
- Update and Reinstall preserve intended local preferences; and
- uninstall removes the package while a subsequent fresh install still boots.

Installed bytecode is an optimization, never the source of truth. Do not bind
your own package format to a Tilefinch version: Tilefinch validates its stored
compiler ABI and recompiles from the packaged source when necessary.

## Progressive fallback

Use this order:

1. WebGL when context creation, shader compilation, and link checks succeed.
2. Canvas 2D when the game has a practical lower-cost renderer.
3. A static, readable help screen explaining the unavailable feature.

A refusal must not become a retry loop. Keep Play, Help, fallback selection,
and the browser escape path usable even when graphics or audio initialization
fails.

## Qualification checklist

Before describing a game as Game Profile v1 compatible:

- test at 480×272 with the shipping 24 MiB / 5 MiB memory profile;
- run the strict 16 MiB / 4 MiB pressure profile and confirm clean degradation;
- record displayed present intervals as well as callback, WebGL, raster, and
  publication phases after warm-up. Target 16.67 ms and require both steady
  frames and interaction spikes to remain below 34 ms;
- run a multi-minute heavy scene with repeated collisions, effects, spawns,
  destruction, and input. Confirm no deadline-tail growth, budget refusals, or
  unbounded heap/allocation growth; a short average-only run is insufficient;
- during that run, track both simulation placement and rendered motion. Move
  the camera while a static landmark and an instanced actor remain visible,
  and confirm neither holds its old screen position or jumps on a later
  fallback frame;
- stage device-test packages with the repository's content-addressed game
  helper and retain its digest with the log. A document-only cache buster does
  not prove that external scripts, styles, or assets are current;
- verify Page-controls enter/exit, face-button preference, Gamepad disconnect,
  keyboard fallback, and a usable pre-capture menu;
- verify trusted audio start, voice completion, pause/resume, hidden-page mute,
  and native-media handoff;
- verify fullscreen entry and both Triangle and Start+Select exits;
- verify suspend/resume, navigation away/back, and WebGL loss/restoration;
- install from the preview, inspect missing resources and estimated size,
  launch with networking unavailable, update/reinstall, and uninstall;
- confirm teardown returns page-owned `Budget` memory to its baseline.

Record the tested Tilefinch commit or release, game package digest, memory
profile, PSP model, CPU clock, soak duration, peak page/QuickJS ownership,
median/p95/maximum presented-frame time, and every frame above 34 ms. A claim
based only on average FPS or PPSSPP is incomplete. Re-run the device soak after
changing renderer structure, asset size, collision/effects load, audio graph,
or installed-package startup.

Recommended compatibility statement for a game README:

> Targets Tilefinch Game Profile v1 at 480×272. Qualified with the strict host
> pressure profile and a physical-PSP action soak; see the project notes for
> the tested release, package digest, memory high-water mark, and frame tail.

The focused host lane and device procedure are in
[Development](DEVELOPMENT.md#canvas-and-webgl-game-qualification). The
repository's [Prism Break 3D](../examples/prism-break-3d/) and
[Treadline Arena](../examples/treadline-arena/) examples are the reference
packages.
