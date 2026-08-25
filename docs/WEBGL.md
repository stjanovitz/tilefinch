# WebGL on Tilefinch

Tilefinch provides a bounded WebGL 1 path for small games, charts, and custom
controls. It translates a practical fixed-function-compatible subset to the
PSP graphics engine (GE); it is not a general GLSL interpreter. A scene that
fits the profile below can remain responsive on a 333 MHz PSP, while an
unrestricted desktop WebGL application usually cannot.

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
  antialias: false,
  depth: true
});
if (!gl) document.body.textContent = "WebGL is unavailable";
</script>
```

The drawing buffer is opaque. Antialiasing and stencil buffers are not
advertised. Check the returned context and queried limits instead of assuming
desktop defaults.

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
- Separate static geometry from dynamic geometry. Use `bufferSubData()` only
  for the range that changed.
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
programs. One native submission contains at most 64 draws, 4,096 vertices,
and exactly 32 distinct payload sources: the maximum 24 buffers plus eight
textures. Draw state is copied into a fixed 64-record per-context pool, so a
queued matrix or viewport never aliases later JavaScript state. A temporarily
detached page may retain an additional submission until its rendering bridge
returns, within that same 64-record total. Buffer storage is capped at
512 KiB and retained texture pixels at 416 KiB. Exceeding a true bound fails
soft; it never grants an unbounded page allocation.

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

The page does not receive PSP controls until the user holds **Start + Select**
to enter Page controls. Holding the chord again returns input to the browser.
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
offline-app patterns together. The focused qualification lane is described in
[Development](DEVELOPMENT.md#canvas-and-webgl-game-qualification).

## Preflight checklist

- The drawing buffer is deliberately PSP-sized.
- Shader compile and program link results are checked.
- The scene stays below the queried buffer, texture, draw, and vertex limits.
- No per-frame readback or object-graph allocation occurs.
- One animation chain pauses when the document is hidden.
- Gamepad disconnect and WebGL loss/restoration degrade cleanly.
- The game remains navigable before Page controls are entered.
- The offline install preview lists every required asset.
