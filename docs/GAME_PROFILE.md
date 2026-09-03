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
   and settings controls ordinary focusable HTML.
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

Keep each classic game script at or below the 384 KiB Game Profile admission
ceiling. Installed apps may precompile at most eight classic scripts and 512
KiB of admitted source in aggregate; the source remains packaged as the
engine-version-independent fallback. Splitting code can improve maintenance,
but it does not expand the aggregate JavaScript heap or installation budget.

For animation, treat 16.67 ms as the aspirational 60 Hz frame time and 33.33 ms
as the maximum steady 30 Hz frame time. Device qualification separately counts
frames above 34 ms. These are performance targets, not scheduler guarantees:
the browser may skip an animation callback rather than queue a backlog.

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
extension and fall back to `Element.requestFullscreen()`. Holding **Start + Select** for 0.7
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

### Storage

`localStorage` and `sessionStorage` share 64 bounded entries and 16 KiB of value
storage in the browser session. Persistent `localStorage` is a user preference,
not an author guarantee; games must tolerate a fresh store. Use compact strings
for settings, unlocked levels, and scores. Do not serialize per-frame state.

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
- Keep readback, image export, DOM layout queries and mutations, storage
  writes, and asset decoding outside the animation loop.
- Preload only the bounded assets needed for the next screen. Show progress or
  a useful menu while optional resources remain unavailable.
- Measure on a physical PSP. PPSSPP is a correctness and lifecycle gate, not a
  performance oracle.

These are measured constraints rather than stylistic preferences. The current
Treadline Arena physical-PSP qualification sustained 8,701 displayed gameplay
pipelines across a 5.5-minute repeated-action soak. Median/p95/worst pipeline
times were 33.221/33.247/33.280 ms, with no pipeline above 34 ms. Native WebGL
averaged 2.998 ms (3.650 ms p95, 3.874 ms maximum), canvas conversion averaged
about 3.9 ms, and the JavaScript callback averaged 11.474 ms with a 17.861 ms
maximum. Peak page ownership remained at the 9.88 MiB load-time high-water
mark, no allocation was refused, and post-warm-up retained growth was 45.6 KiB;
about 4.1 KiB of that was the bounded draw-template state. Authors should
repeat the measurement for their own scene rather than assume these timings.
The separate validation-only GE publication probe was pixel-exact but saved
only about 0.9 ms over CPU conversion plus the page-buffer copy, so the
tear-free CPU compositor remains the default; reducing per-frame page work is
the larger opportunity for this workload.

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
Reinstall. It also precompiles up to eight classic scripts from at most 512 KiB
of admitted source and stores at most 512 KiB of source-bound QuickJS bytecode.
The packaged source remains the fallback after an engine ABI change or failed
bytecode restore, so authors do not ship or depend on compiler artifacts.

Hard snapshot limits are:

- 1 MiB serialized document;
- 1 MiB aggregate captured response bodies and no more than 32 resources;
- a 1,736,704-byte resource-pack envelope including compiler artifacts and
  bounded metadata;
- one decoded 16×16 RGBA icon in the native library;
- 12 total items across the offline library.

Keep HTML, CSS, JavaScript, fonts, images, and PCM effects same-origin and make
the game usable when an optional resource is listed as unavailable. Opening an
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
