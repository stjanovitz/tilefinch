# WebGL on Tilefinch

Tilefinch provides a bounded WebGL 1 path for small games, charts, and custom
controls. It translates a practical fixed-function-compatible subset to the
PSP graphics engine (GE); it is not a general GLSL interpreter. A scene that
fits the profile below can remain responsive on a 333 MHz PSP, while an
unrestricted desktop WebGL application usually cannot.

This is the detailed WebGL guide. The broader, versioned contract for Canvas,
input, audio, storage, lifecycle, offline packaging, and qualification is
[Tilefinch Game Profile v1](GAME_PROFILE.md), with a
[machine-readable companion](tilefinch-game-profile-v1.json).

## Start with the PSP-sized surface

Request the smallest backing resolution that preserves the game's look, then
scale the canvas with CSS. A 320×180 or 240×136 drawing buffer is a useful
starting point for a full-screen game. Tilefinch proportionally clamps larger
requests to 480×272 and 131,072 pixels, but authoring at the intended size
avoids hidden scaling assumptions and reduces fill work.

```html
<canvas id="game" width="320" height="180"></canvas>
<style>
  #game { width: 100vw; height: 100vh; image-rendering: pixelated; }
</style>
<script>
const canvas = document.querySelector("#game");
const gl = canvas.getContext("webgl", {
  alpha: false,
  antialias: true,
  depth: true
});
if (!gl) document.body.textContent = "WebGL is unavailable";
</script>
```

The drawing buffer is opaque and stencil buffers are not advertised. An
explicit `antialias:true` enables a bounded edge-coverage mode rather than a
second multisample framebuffer: the PSP GE adds hardware-smoothed line and
eligible triangle fringes, while the host fallback uses four fixed coverage
samples only for triangle rasterization. On PSP, full-clear opaque frames also
use a bounded temporal edge pass during small camera motion. It reprojects a
limited number of preceding fringe samples at low alpha, resets across large
camera, surface, or context changes, and never blends the filled scene. This
avoids full-frame history, long trails, and another framebuffer while making
camera shake more stable. It keeps all ordinary vertex and work ceilings.
Leave it off for deliberately pixelated art or when a measured scene needs
every last millisecond. Check the returned context attributes and queried
limits instead of assuming desktop defaults.

## Use shaders the GE can translate

Use a conventional position attribute, optional vertex color or one 2D
texture, and at most four `mat4` uniforms in the position expression. A
uniform color multiplier is also supported. Keep shaders short and direct:

```glsl
attribute vec3 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uProjection;
uniform mat4 uModel;
varying vec2 vTexCoord;

void main() {
  vTexCoord = aTexCoord;
  gl_Position = uProjection * uModel * vec4(aPosition, 1.0);
}
```

```glsl
precision mediump float;
uniform sampler2D uTexture;
varying vec2 vTexCoord;

void main() {
  gl_FragColor = texture2D(uTexture, vTexCoord);
}
```

Shader loops, `discard`, derivatives, arbitrary fragment effects, and general
shader control flow are outside this profile. Compile and link status must be
checked: unsupported shader shapes fail as ordinary WebGL errors so the page
can offer a Canvas 2D or static fallback. Source is bounded to 16 KiB per
shader; a larger shader is refused and is not retained for
`getShaderSource()`. Vertex and fragment uniform-vector limits are enforced
independently at the values returned by `getParameter()`.

Tilefinch intentionally substitutes PSP-practical texture defaults: a new
texture starts with `LINEAR` minification/magnification and
`CLAMP_TO_EDGE` wrapping. Mipmaps and the WebGL completeness rules that depend
on them are not implemented; `generateMipmap()` reports an error. Queries
return the effective substituted state, so a page never sees a spec default
that the GE silently ignores. Set the parameters explicitly when portable
behavior matters.

## Design for few submissions

- Prefer indexed triangles, atlases, and shared static vertex/index buffers.
- Use `ANGLE_instanced_arrays` for repeated transformed geometry. Tilefinch
  admits divisors 0 and 1, at most 64 instances in one draw, and a `mat4`
  per-instance transform occupying four consecutive attribute locations.
- Separate static geometry from dynamic geometry. Use `bufferSubData()` only
  for the range that changed.
- Keep a stable VAO, program, primitive shape, and index range for repeated
  draws. Tilefinch caches the validated draw plan and lazily retains one
  96-word packed template per active plan; each draw patches only live source
  indices/generations, matrices, colors, texture selection, and instance
  count. This is bounded internal acceleration, not a new API.
- Upload buffers and textures before issuing the frame's draws. A resource
  mutation after draws have queued must flush those draws first to preserve
  WebGL ordering.
- Group draws by texture and render state. Avoid toggling blend, depth, cull,
  viewport, and scissor state between small objects.
- Reuse typed arrays, matrices, vectors, and scene objects. Do not allocate a
  new object graph in every animation frame.
- Keep `readPixels()` out of the animation loop. It is an explicit
  synchronization and readback point.
- Create textures once and update only genuinely changing content. Unchanged
  texture generations remain cached by the PSP backend.

One realm admits at most two contexts, 24 buffers, eight textures, and eight
programs. One native submission contains at most 64 draws and 4,096 effective
vertices (base vertices multiplied by instance count),
and exactly 32 distinct payload sources: the maximum 24 buffers plus eight
textures. Draw state is copied into fixed per-context records, so a queued
matrix or viewport never aliases later JavaScript state. A temporarily
detached page may retain an additional ordered 64-record submission until its
rendering bridge returns, so the fixed command pool contains 128 records while
each native submission remains capped at 64. Buffer storage is capped at
512 KiB and retained texture pixels at 416 KiB. Exceeding a true bound fails
soft; it never grants an unbounded page allocation.

The bounded instancing shader shape is deliberately narrow: the position
expression may include one `mat4` attribute, with all four columns backed by
one interleaved float buffer and divisor 1. Other vertex attributes retain
divisor 0. The native path reuses the admitted unit geometry and applies each
matrix on the GE; the host fallback produces the same pixels. Divisors above
1 and arbitrary per-instance attribute layouts are refused rather than
silently expanded in QuickJS. Check for the extension and retain a normal
WebGL 1 fallback.

```js
const instancing = gl.getExtension("ANGLE_instanced_arrays");
for (let column = 0; instancing && column < 4; column++) {
  const location = instanceMatrixLocation + column;
  gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64, column * 16);
  gl.enableVertexAttribArray(location);
  instancing.vertexAttribDivisorANGLE(location, 1);
}
instancing?.drawElementsInstancedANGLE(
  gl.TRIANGLES, indexCount, gl.UNSIGNED_SHORT, 0, instanceCount);
```

### Canvas publication on PSP

The GE-rendered WebGL color surface lives in EDRAM, but Tilefinch's ordinary
page framebuffer is a budget-owned RGB565 surface in main RAM. The current
publication path converts and scales an eligible opaque canvas into that page
frame, applies retained HTML/browser overlays there, then copies the completed
page into the triple-buffered scanout surface. This keeps page rendering,
screenshots, focus damage, and the tear-free vblank owner on one authoritative
frame.

A direct GE-to-scanout canvas blit is therefore not a drop-in replacement for
the CPU conversion. It needs an explicit compositor handoff: exact canvas
geometry and EDRAM epoch must cross the platform-present boundary, overlays
must be composed after the GE fence, and every fallback/navigation path must
restore the main-RAM frame without exposing a partially written back buffer.
Until that ownership transition is qualified pixel-for-pixel on real hardware,
Tilefinch deliberately keeps the measured CPU fast path. Authors still benefit
most from a 320×180 opaque canvas and stable overlay geometry.

The Treadline device soak puts an upper bound on the opportunity: canvas
conversion averaged about 3.9 ms and the page-base copy was about 1.6 ms in
tail frames. Even removing both entirely would leave that scene around 27–28
ms, still in the 30 Hz rather than 60 Hz presentation band.

The physical-PSP validation probe now compares the exact 320x180-to-480x270
CPU kernel with a direct GE 8888-to-RGB565 back-buffer draw. Across 120
measured frames after 12 warm-ups, CPU conversion averaged 3.175 ms and its
following page copy 2.058 ms; the GE path averaged 4.315 ms end to end
(0.035 ms submission and 4.269 ms synchronization), with a 4.371 ms maximum.
All 129,600 displayed
pixels matched exactly. That proves the conversion is viable, but its isolated
saving is only about 0.9 ms before overlay/ownership integration, so it does
not justify changing the tear-free compositor by itself. Keep the CPU path
unless a later trace shows publication, rather than page JavaScript, is again
the limiting phase.

## Run a single bounded game loop

Use one `requestAnimationFrame()` chain, compute movement from its monotonic
timestamp, and cap an individual simulation step. Tilefinch runs at most one
callback per presented frame and skips elapsed frames under load rather than
building a callback backlog.

Pause simulation and audio on `visibilitychange`. Native media, suspend, and
leaving the page can all make the document hidden. On WebGL context loss,
`webglcontextlost` is dispatched and `getError()` reports
`CONTEXT_LOST_WEBGL` once. Retained JavaScript resources and native color/depth
storage are released deterministically. Calling `preventDefault()` on the loss
event requests one bounded restoration of the same context object; restoration
starts with default GL state, invalidates every old resource object, and
dispatches `webglcontextrestored`. Without cancellation the loss is terminal
and no longer occupies one of the realm's two slots. In either case, keep
compact CPU-side scene state so buffers and textures can be rebuilt.

```js
let previous = 0;
function frame(now) {
  if (!document.hidden) {
    const dt = Math.min(0.05, Math.max(0, (now - previous) / 1000));
    update(dt);
    draw();
  }
  previous = now;
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
```

## PSP controls

The page does not receive PSP controls until the user enters Page controls.
A trusted Play click may call `navigator.tilefinch.requestPageControls(canvas)`;
Tilefinch displays a Start+Select exit notice. Holding **Start + Select** for
0.7 seconds remains the manual entry fallback and returns input to the browser.
The D-pad and analog nub map to the standard Gamepad axes/buttons. Under
**Settings → Browsing & input → Game buttons**, the user can choose whether X
or O is the primary face button; games should use the standard Gamepad button
indices rather than displaying a hard-coded physical button.

Page fullscreen must start from a user activation. Triangle or Start+Select
always returns control to Tilefinch. Listen for disconnects and do not assume a
controller remains captured across navigation, native media, or suspend.

## Packaging and fallback

Installable games should keep their shell and assets same-origin, list the
bounded resources they need, and remain useful without service workers.
Tilefinch's offline-app preview reports missing resources and estimated size
before installation. Keep a static or Canvas 2D fallback for unsupported
shaders and display a clear message rather than retrying failed WebGL work in a
tight loop.

The repository's [Prism Break 3D](../examples/prism-break-3d/) example shows
the intended WebGL, Gamepad, Fullscreen, Page Visibility, Web Audio, and
offline-app patterns together. [Treadline Arena](../examples/treadline-arena/)
adds fixed-step third-person movement, a camera-following world, bounded entity
pools, one-bounce projectiles, swapped barrier meshes, three objective modes,
three tank classes, directional armor, class secondaries, an opt-in Command
meter, five gadgets, role-based bots, camera obstruction handling, persistent
setup preferences, keyboard fallback, and one compact input record per actor
suitable for future network transport. The focused qualification lane is
described in [Development](DEVELOPMENT.md#canvas-and-webgl-game-qualification).

## Preflight checklist

- The drawing buffer is deliberately PSP-sized.
- Shader compile and program link results are checked.
- The scene stays below the queried buffer, texture, draw, and vertex limits.
- No per-frame readback or object-graph allocation occurs.
- One animation chain pauses when the document is hidden.
- Gamepad disconnect and WebGL loss/restoration degrade cleanly.
- The game remains navigable before Page controls are entered.
- The offline install preview lists every required asset.
