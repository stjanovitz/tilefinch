# WebGL on Tilefinch

For web and game developers. Tilefinch provides a bounded WebGL 1 path for
small games, charts, and custom controls. It translates a practical
fixed-function-compatible subset to the PSP graphics engine (GE); it is not a
general GLSL interpreter. A scene that fits the profile below can remain
responsive on a 333 MHz PSP, while an unrestricted desktop WebGL application
usually cannot.

This is the detailed WebGL guide. The broader, versioned contract for Canvas,
input, audio, storage, lifecycle, offline packaging, and qualification is
[Tilefinch Game Profile v1.2](GAME_PROFILE.md), with a
[machine-readable companion](tilefinch-game-profile-v1.2.json).

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

The drawing buffer is opaque and stencil buffers are not advertised. Blending
takes each WebGL 1 factor that the GE can weigh by exactly. The GE weighs the
fragment by the drawing buffer's colour and the drawing buffer by the
fragment's, but neither by its own colour, so which colour factors are taken
depends on the slot:

| Factor | Source factor | Destination factor |
|---|---|---|
| `ZERO`, `ONE`, `SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA` | drawn | drawn |
| `DST_COLOR`, `ONE_MINUS_DST_COLOR` | drawn | refused |
| `SRC_COLOR`, `ONE_MINUS_SRC_COLOR` | refused | drawn |
| `DST_ALPHA`, `ONE_MINUS_DST_ALPHA` | drawn as `ONE`, `ZERO` | drawn as `ONE`, `ZERO` |
| `SRC_ALPHA_SATURATE` | drawn as `ZERO` | invalid in WebGL 1 |
| `CONSTANT_COLOR`, `ONE_MINUS_CONSTANT_COLOR`, `CONSTANT_ALPHA`, `ONE_MINUS_CONSTANT_ALPHA` | drawn | drawn |

A refused factor sets `INVALID_ENUM` and leaves the factors as they were. The
drawing buffer's alpha is 1, so `DST_ALPHA` weighs as `ONE`, and
`ONE_MINUS_DST_ALPHA` and `SRC_ALPHA_SATURATE` (the smaller of the source
alpha and one minus the buffer's) as `ZERO`, exactly as WebGL draws them into
an opaque buffer. The constant factors weigh by the `blendColor()` colour,
clamped to [0, 1] as WebGL 1 does, which the GE holds as a fixed blend colour
of one byte per channel: each channel of the factor rounds to the nearest
1/255. `getParameter(BLEND_COLOR)` reports the colour as set; a browser that
blends with the exact value can differ by one step in 255 where a channel of
the factor is not a whole number of 255ths. As WebGL requires, a constant
colour factor and a constant alpha factor in one colour pair set
`INVALID_OPERATION`. `blendFuncSeparate()` applies the colour factors to all
four channels; its alpha factors may be any WebGL 1 factor, since the opaque
buffer never shows them, and `getParameter(BLEND_SRC_ALPHA)` and
`getParameter(BLEND_DST_ALPHA)` report them.

`blendEquation()` takes `FUNC_ADD`, `FUNC_SUBTRACT` (0x800A) and
`FUNC_REVERSE_SUBTRACT` (0x800B). The subtractions use the blend factors:
`FUNC_SUBTRACT` draws source × source factor − buffer × destination factor,
and `FUNC_REVERSE_SUBTRACT` the reverse. A result below zero is stored as
zero, as a WebGL unorm buffer clamps after the equation; with these factors
a subtraction never exceeds one. The GE draws them with its own subtract and
reverse-subtract operations and the same factor mapping as `FUNC_ADD`, and
the host renderer gives the same pixels, clamped the same way.

The GE truncates each weighted term where the host rounds the result once,
so with any factor other than `ZERO` and `ONE` (alpha, colour or constant) a
PSP channel can be one step in 255 away from the host's, as WebGL allows;
every factor pair and equation of the conformance fixture stays within that
step on a PSP.
With `EXT_blend_minmax` enabled, `blendEquation()` also takes `MIN_EXT`
(0x8007) and `MAX_EXT` (0x8008): each colour channel becomes the smaller or
larger of the fragment and the drawing buffer, and the blend factors are
ignored, as WebGL specifies. The GE draws them with its own minimum and
maximum blend operations, and the host renderer gives the same pixels. In
`blendEquationSeparate()` the RGB equation draws all channels; the alpha
equation is kept and reported by `getParameter(BLEND_EQUATION_ALPHA)` but,
like a separate alpha factor, never shows in the opaque buffer.

Only a draw with blending off, or with `FUNC_ADD` and factors that are
`ZERO`, `ONE`, `SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA` or drawn as one of them,
gets the antialiasing fringe. The fringe is a partial-coverage
blend over the buffer, which has no exact form for `FUNC_SUBTRACT`,
`FUNC_REVERSE_SUBTRACT`, `MIN_EXT` or `MAX_EXT` (a multisample buffer would
clamp each sample before averaging), nor for a colour or constant factor: on
the GE the fringe is an alpha blend, which would draw the edge of a multiply
as a plain mix. three.js r128's `MultiplyBlending` (`ZERO`, `SRC_COLOR`) and
`SubtractiveBlending` (`ZERO`, `ONE_MINUS_SRC_COLOR`), with or without
`premultipliedAlpha`, draw as WebGL does, and a `CustomBlending` material
draws with any equation and factors in the table above. Stencil and polygon
offset setters are accepted and have no effect. An
explicit `antialias:true` enables a bounded edge-coverage mode rather than a
second multisample framebuffer: the PSP GE adds hardware-smoothed line and
eligible triangle fringes, while the host fallback uses four fixed coverage
samples only for triangle rasterization. There the pixel centre still decides
which triangle owns a pixel (watertight, top-left fill rule), and the samples
only add a partial-coverage fringe where a triangle misses the centre, so
triangles that share an edge never let the clear colour through. On PSP, full-clear opaque frames also
use a bounded temporal edge pass during small camera motion. It reprojects a
limited number of preceding fringe samples at low alpha, resets across large
camera, surface, or context changes, and never blends the filled scene. This
avoids full-frame history, long trails, and another framebuffer while making
camera shake more stable. It keeps all ordinary vertex and work ceilings.
Leave it off for deliberately pixelated art or when a measured scene needs
every last millisecond. Check the returned context attributes and queried
limits instead of assuming desktop defaults.

`getSupportedExtensions()` lists `ANGLE_instanced_arrays`, `EXT_blend_minmax`,
`OES_vertex_array_object` and `TILEFINCH_repeated_frame_commands`. Enable
each with `getExtension()` before using its constants or functions.

## Use shaders the GE can translate

Use a position attribute, optional vertex color or one 2D texture, and at most
four `mat4` uniforms in the position expression. A `vec4` uniform or literal
color multiplier is also supported. Keep shaders short and direct:

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

### What the translator runs

The PSP does not execute GLSL. At `linkProgram()` Tilefinch reads the two
shaders and maps them onto the GE's fixed-function pipeline:

- **Position:** `gl_Position` is one product of up to four `mat4` uniforms,
  optionally one per-instance `mat4` attribute, and the position attribute
  itself, `vec4(position, 1.0)` or `vec4(position, 0.0, 1.0)`. The bounded
  instance form `vec4(aPosition * aInstanceTransform.w +
  aInstanceTransform.xyz, 1.0)` is also accepted.
- **Colour:** `gl_FragColor` is one product of at most these factors:
  `texture2D(sampler, varying)`, a varying, one `vec4` uniform, or one
  literal `vec4(r, g, b, a)` constant. A varying it reads must be written once
  in the vertex shader as a product of attributes and at most one `vec4`
  uniform. The GE applies one vertex colour attribute and one per-instance
  colour attribute. A texture coordinate varying must be a plain copy of one
  attribute. A local such as `vec4 texel = texture2D(...);` may stand for its
  value.
- **Inputs follow data flow.** The attribute that feeds the texture
  coordinate, the colour varyings, and `gl_Position` is the one used, whatever
  its name. When the colour is fully recognised, inputs the fragment colour
  never reads (an unused colour attribute or uniform) are not applied.

Everything else in `main()` is either refused or reported:

| Construct | Result |
|---|---|
| `for`, `while`, `do`, `dFdx`/`dFdy`/`fwidth` | link fails, except derivatives inside a built-in lit material's bump map, which is [left out](#built-in-materials-from-scene-libraries) |
| `discard` | link fails, except an alpha test `if (color.a < uniform) discard;` that the [material mapping](#built-in-materials-from-scene-libraries) draws as the GE alpha test |
| `if` or `?:` whose condition reads a varying, attribute, texture sample, `gl_FragCoord`, `gl_FrontFacing`, `gl_PointCoord`, a helper function, or a local derived from them | link fails: the GE cannot branch per pixel or per vertex |
| `texture2D` in the vertex shader; `textureCube`, `texture2DProj`, `texture2DLod` | link fails (`MAX_VERTEX_TEXTURE_IMAGE_UNITS` is 0; only 2D sampling exists), except a built-in lit material's environment map, [left out with a warning](#built-in-materials-from-scene-libraries) |
| two or more samplers sampled | link fails (`MAX_TEXTURE_IMAGE_UNITS` is 1), except the [extra maps of a built-in lit material](#built-in-materials-from-scene-libraries) |
| `if` or `?:` that reads only uniforms and constants | evaluated per draw when it chooses a uniform or constant factor (`vColor * (uMode > .5 ? 1. : .25)`); otherwise links with a warning and neither branch is evaluated |
| numeric and uniform colour factors (`vColor * 0.5`, `vec4(vColor.rgb * uTint, 1.0)`) | applied exactly, evaluated once per draw |
| other colour math (`vColor * vColor.g`, `vColor + vec4(0.25)`, `gl_FragCoord` effects) | links with a warning naming the factor; the PSP draws the recognised factors |
| other position math (offsets, `mat3` transforms, swizzles) | links with a warning; the PSP draws the recognised transform |
| `gl_FragColor` or `gl_Position` written in parts or more than once | links with a warning |

A refusal leaves `LINK_STATUS` false. A warning leaves it true. In both cases
`getProgramInfoLog()` names the construct and its line in the author's
source, for example
`ERROR: fragment shader line 5: 'if' depends on per-pixel data; the PSP GE
cannot branch per pixel` or
`WARNING: fragment shader line 2: '0.5' is not applied to gl_FragColor on the
PSP`. Each program also logs this text once with `console.warn`, and
`__tilefinchWebGLDiagnostics.shaderRefusals` and `.shaderWarnings` count
refused links and links with warnings. The check runs only at link time, so it
costs nothing per draw.

The split is deliberate. A per-pixel branch or a second texture has no
fixed-function meaning: the GE would draw one input's colour everywhere, so
the program fails like any WebGL program that exceeds the implementation, and
a page that checks `LINK_STATUS` can fall back. Dropped arithmetic still
draws the shader's base colour or texture, so it keeps working and the warning
tells the author what is missing. A game in development should treat any
program info log as a defect: on Tilefinch, a clean log means the PSP draws
exactly what the shader says.

Source is bounded to 64 KiB per shader; a larger shader is refused and is not
retained for `getShaderSource()`. The uniform limits are WebGL 1's minimums,
128 vertex and 16 fragment uniform vectors, counted per stage over the
uniforms the program actually uses, as WebGL counts them, and 8 varying
vectors pass from the vertex to the fragment shader. Uniform values are
evaluated in JavaScript once per draw rather than held in GE registers, so
declared but unused uniforms cost nothing.

Tilefinch intentionally substitutes PSP-practical texture defaults: a new
texture starts with `LINEAR` minification/magnification and
`CLAMP_TO_EDGE` wrapping. Mipmaps are not stored: a mipmap minification filter
samples the base level with its base filter (`LINEAR_MIPMAP_LINEAR` acts as
`LINEAR`), and `generateMipmap()` succeeds without building levels. Cube maps
are not available. Queries return the effective substituted state, so a page
never sees a spec default that the GE silently ignores. Set the parameters
explicitly when portable behavior matters.

### Built-in materials from scene libraries

Scene libraries such as three.js generate long shaders for their built-in
materials: preprocessor switches, uniform structs and arrays, helper
functions and `inout` accumulator structs. When the shape-based check above
cannot translate a program exactly, Tilefinch evaluates the whole of `main()`
symbolically (helpers inlined, struct values followed field by field,
uniform-only `if` and `?:` kept as per-draw selections) and maps what it
finds onto GE terms:

| Shader content | GE term |
|---|---|
| a chain of up to four `mat4` uniforms applied to the position | model-view and projection, split where the eye-space position is read |
| uniform and numeric colour factors, one 2D texture, one vertex colour | material colour evaluated per draw, texture, vertex colour |
| `mat3` uniform applied to the texture coordinate (`uvTransform`) | the full affine transform, applied to each vertex's texture coordinate |
| `gl_PointSize = size * (scale / -eye.z)`, `gl_PointCoord` sampling | point sprites with attenuated size and their own texture coordinates |
| `mix(color, fogColor, smoothstep(near, far, depth))`, exponential squared fog | GE linear fog (warned for `smoothstep` and `exp2`) |
| `if (alpha < uniform) discard;` | GE alpha test |
| lighting helpers in programs whose `SHADER_NAME` is `MeshLambertMaterial`, `MeshPhongMaterial`, `MeshStandardMaterial` or `MeshPhysicalMaterial` | GE per-vertex lighting: ambient, emissive, up to four directional or point lights, Blinn-Phong specular |
| other colour math that reads only attributes, uniforms and numbers, directly or through varyings (`0.5 + 0.5 * cos(uTime + vUv.xyx * 3.0)`) | evaluated per vertex in software (on the VFPU on a PSP) and interpolated by the GE |

Lighting is recognised by data flow, not by matching source text: a helper
that reads a light uniform (a struct uniform whose type ends in `Light`, or
`ambientLightColor`) or receives a light struct becomes one lighting term,
and the light-struct uniforms passed to it (`pointLights[0]`,
`directionalLights[1]`, ...) name the GE lights. A light probe is recognised
by what its helper computes, whatever the uniform, the helper and its
parameters are called: a helper that takes a `vec3[9]` uniform array and
evaluates three.js's spherical-harmonic irradiance on it (or only its band-0
term, the first coefficient times 0.886227), directly or through helpers that
pass the array on, adds the probe's band 0 to the GE ambient light. A helper
that does anything else with such an array is not a light probe. The material struct passed
beside them supplies the diffuse, specular and shininess inputs. `SHADER_NAME`
only selects the reflectance model.

The approximations link with a warning:

- per-pixel lighting (Phong, Standard) is evaluated per vertex;
- physically based shading uses GE Blinn-Phong: shininess
  `2 / roughness^4 - 2`, specular colour `mix(0.04, diffuse, metalness)`;
- point-light range falloff `(1 - d / distance)^decay` becomes GE
  `1 / (1 + k d)` matched at half the range;
- spot lights draw as point lights, hemisphere lights as ambient;
- more than four lights keep the first four;
- `smoothstep` and `exp2` fog draw as linear fog.
- colour math evaluated per vertex is interpolated across each triangle, so
  a term that is not linear in its inputs (a `cos` wave, a `step` edge) is
  only as fine as the mesh: subdivide the geometry where it matters.
- a lit material's texture maps other than its base colour map are left out,
  and the warning names each one (see below).

The GE samples one texture. A lit material may read several: the base
colour map, which multiplies the diffuse colour, is the one drawn, and every
other map is recognised by where its value flows, whatever its sampler is
called, and left out. Each map links with one warning,
`WARNING: fragment shader: texture 'normalMap' perturbs the normal; the GE lights with vertex normals`
for example, and the program draws exactly what the same material draws
without that map:

| The map's value | Example (three.js) | Drawn as | Warning ends |
|---|---|---|---|
| combined with the vertex normal into the normal the lighting reads | `normalMap`, `bumpMap` | vertex normals | `perturbs the normal; the GE lights with vertex normals` |
| added to light handed to the lighting | `lightMap` | the lights alone | `adds light; the PSP draws without it` |
| multiplying a lit term | `aoMap` | the lit term alone | `scales light; the PSP draws without it` |
| multiplying the emissive colour | `emissiveMap` | the emissive colour | `scales the emissive colour; the PSP draws the colour without it` |
| multiplying a material input (a uniform factor, or the specular strength) | `roughnessMap`, `metalnessMap`, `specularMap` | the uniform factor, `roughness` and `metalness` as uniforms | `scales a material input; the PSP draws the input without it` |
| a cube map, or a 2D map looked up by anything but a mesh texture coordinate, blended into or added to the colour | `envMap` | no reflections | `is an environment map; the PSP draws no reflections` |
| scaling only a dropped environment term | `specularMap` with `envMap` | no reflections | `scales the reflection; the PSP draws no reflections` |

A material whose only texture is such a map draws untextured, with the same
warning. A displacement map is never sampled: `MAX_VERTEX_TEXTURE_IMAGE_UNITS`
is 0, so three.js leaves it out of the vertex shader and draws the mesh
undisplaced, with no warning.

Per-vertex colour math is bounded. A program may use at most 32 steps (one
per operation or built-in call), 15 constant vectors (each number, uniform and
`mat3`/`mat4` column it reads) and 12 temporaries; a larger one is refused at
link time with the reason. A draw may run at most 65,536 steps in all
(vertices × instances × steps) in one submission, so a 32-step program draws
up to 2,048 vertices at once. A larger draw is made in several submissions,
as described under [Large draws](#large-draws); a fan or loop that does not
fit one submission is not drawn. Programs that read a texture,
`gl_FragCoord`, `gl_PointCoord` or the lights are not evaluated per vertex.

A lit program the mapping cannot represent is refused rather than drawn
unlit, for example a light type with no GE equivalent, lighting in a program
without a lit `SHADER_NAME`, or a second texture in the colour itself: an
alpha map, or two maps multiplying the diffuse colour. A program that is not
a built-in lit material and samples two textures (a `ShaderMaterial` mixing
two maps) is refused too. Custom `ShaderMaterial` programs go through the same
checks: shapes the mapping understands draw exactly, and anything else is
refused or warned as in the table above. Mapping happens at link time; each
draw only evaluates the program's uniform terms into a 256-word material
record. A program keeps that record for each of the last 16 sets of values
of the uniforms it reads, and a draw whose values match a kept set reuses
its record without evaluating anything: setting a uniform to the value it
already holds, as three.js does with its light-probe array before every
draw, keeps the record, and materials of one type that share a program
each find their own. A program whose values are new on almost every draw,
such as a shader driven by a time uniform, soon stops looking for a kept
set and evaluates its terms as each draw needs them. The position matrices
are copied into every draw.

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
matrix or viewport never aliases later JavaScript state. An ordinary command
is packed into the submission as it is queued and returns its pool record at
once; only a deferred ordered tail (commands queued behind a submission the
rendering bridge has not accepted yet, such as a temporarily detached page's)
keeps object snapshots. The fixed command pool therefore holds 65 records,
one more than a submission's 64 draws. Buffer storage is capped at
512 KiB and retained texture pixels at 416 KiB. Exceeding a true bound fails
soft; it never grants an unbounded page allocation.

### Large draws

The 4,096-vertex bound belongs to one native submission, not to a draw. It
exists because each submission's vertices are prepared in fixed scratch
memory: on the GE, 24 bytes per vertex and twice that for lit materials,
which also need room for their light terms; in the host renderer, one
expanded vertex per drawn vertex. Raising it would raise every page's peak
memory, and a failed allocation loses the context. The 64 KiB GE display list
is not the bound, because a full list is submitted and refilled. Indexed
draws are drawn index by index, so the bound counts indices, not the
vertices they reach; `UNSIGNED_BYTE` and `UNSIGNED_SHORT` indices may reach
any vertex in the buffer.

A draw with more vertices × instances than one submission takes (4,096, or
65,536 ÷ steps for a per-vertex colour program) is split into several
submissions that draw exactly what one would, in the same order, with the
same depth testing and blending:

- `TRIANGLES`, `LINES` and `POINTS` split on whole triangles, segments and
  points.
- `TRIANGLE_STRIP` and `LINE_STRIP` parts overlap by two vertices or one, and
  a triangle-strip part starts on an even triangle so the winding and culling
  stay those of the whole strip.
- `drawElements()` splits by index range; the indices stay where they are.
- An instanced draw is split into groups of whole instances when its base
  geometry fits a submission, and each instance's geometry into parts when
  it does not.

Each extra submission costs a GE list flush, so a split draw is slower than
the same geometry in fewer, smaller draws, but nothing is left out. A strip
part with a vertex that cannot be drawn (not finite, or past the clip range)
is dropped by itself, where before the whole strip was. Draws that fit one
submission are unchanged.

Two kinds of draw cannot be split exactly and are refused with
`OUT_OF_MEMORY` and one `console.warn` per program and draw shape:

- a `TRIANGLE_FAN` or `LINE_LOOP` with more vertices than one submission
  takes, for example `WebGL program 3: a TRIANGLE_FAN draw of 5000 vertices
  is over the PSP limit of 4096 vertices per submission, and a fan cannot be
  split into parts exactly, so it is not drawn; draw TRIANGLES or fans of at
  most 4096 vertices`;
- a draw of more than 64 instances, with `draw at most 64 instances at a
  time`.

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

The GE-rendered WebGL color surface lives in EDRAM, while Tilefinch's page
framebuffer is a budget-owned RGB565 surface in main RAM. By default an
eligible canvas frame skips that RAM frame: when one opaque WebGL canvas
fills the viewport at the 2:3 scale (a 320×180 backing shown at 480×270) and
nothing is composited above it (no HTML overlay, fixed or sticky element,
scroll thumb, or focus outline), the presenter has the GE scale the EDRAM
surface straight into the back buffer in 32-texel column strips, pixel-exact
with the CPU kernel. The RAM frame is produced only when something reads it
(screenshots, thumbnails, a fallback); WebGL readback is unaffected. Any
other frame, and any refused or failed GE submission, keeps the exact CPU
conversion and copy. The presenter's sprite list and vertices (86 KiB) are
taken from the heap by the first frame it publishes and freed once the page
has been left and no submission can still read them; if the heap cannot
supply them, that page's frames use the CPU conversion.

A canvas shows a frame only once it is complete. Draws are submitted when
the task that made them ends; a submission forced earlier inside the task (a
buffer or texture upload after a draw, more than 64 draws, `readPixels()`,
`flush()` or `finish()`) draws part of the frame into the drawing buffer, and
until the task's last submission the page, including the loading preview,
keeps showing the previous complete frame, or nothing before the first. The
previous frame is copied once per such frame (230 KB for a 320×180 canvas,
kept while the canvas lives); a frame submitted in one piece copies nothing.

The GE scale costs a fraction of the CPU conversion and copy, so authors
benefit most from a 320×180 opaque canvas with the HUD drawn inside it: a DOM
HUD, menu or toast over the canvas keeps those frames on the CPU path.

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
and frees one of the realm's two slots. In either case, keep
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
adds capped-step third-person movement, a camera-following world, bounded entity
pools, one-bounce projectiles, swapped barrier meshes, three objective modes,
three tank classes, directional armor, class secondaries, an opt-in Command
meter, five gadgets, role-based bots, camera obstruction handling, persistent
setup preferences, keyboard fallback, and one compact input record per actor
suitable for network transport. The focused qualification lane is described in
[Treadline qualification](../examples/treadline-arena/QUALIFICATION.md).

## How the PSP path is built

WebGL is a lazy, bounded command translator, not a software GLSL interpreter.
Its interface objects (`WebGLRenderingContext` with its constants,
`WebGLObject` and the object interfaces, `WebGLContextEvent`) come from a
separate small module, `webgl-interfaces.js`, that loads the first time a page
reads one of those names, so feature tests and constant reads do not load the
translator; the translator loads it first and makes its contexts and objects
instances of those interfaces. WebGL 2 is not exposed, so its interfaces are
not defined.
The JavaScript layer exposes the familiar object and state model, admits only
the shader shapes above, combines a bounded chain of position-matrix uniforms,
and snapshots each draw's buffers, attributes, transform, viewport, scissor,
depth, cull, and blend state into a bounded command batch. The native wire
format is versioned and uses fixed 32-bit integer and float words, which
avoids software double-precision conversion on Allegrex while keeping
draw-time snapshots. The native side reads possibly unaligned words with
`memcpy` and validates every scalar and byte span again before either the host
reference rasterizer or the GE sees it.

A draw from a [mapped material](#built-in-materials-from-scene-libraries)
carries one more record: 256 words with the model-view and projection
matrices, ambient, emissive, specular, fog, point-size, texture-transform and
alpha-test terms, and up to four lights. The batch sends these records as an
optional, separately versioned wire argument only when it holds such a draw.
The host reference rasterizer applies the GE lighting equation per vertex;
the GE path programs the same terms with `sceGuLight`, `sceGuFog` and
`sceGuAlphaFunc`, and draws points as screen-space sprites. Material draws
bypass the translated-geometry cache and do not combine with instancing: an
instanced material draw sets `INVALID_OPERATION`. A material draw with the
normal attribute disabled lights every vertex with normal (0, 0, 1).

Draw admission validates enabled-attribute spans and indexed-draw alignment,
count, offset, highest referenced vertex, and source-buffer bounds before a
command enters the batch, so malformed game geometry is an ordinary WebGL
error rather than a late native failure or context loss.

The PSP keeps one bounded EDRAM color target, depth target, and texture cache,
and shares GE command ownership with the media presenter rather than adding a
second graphics authority. Texture generations avoid re-uploading unchanged
pixels within one native `(realm epoch, canvas handle)` owner: textures a
frame keeps using stay resident, and a new one takes a free gap, evicting the
least recently used textures the frame does not use; fresh realms,
context restoration, canvas changes, and the video layout's reuse of the
EDRAM texture region clear that authority before lookup. Page-owned command,
buffer, texture, depth, and presentation memory is charged through `Budget`,
so a failed allocation or an oversized scene refuses the operation without
growing a hidden heap. The one fixed block outside `Budget` is the 64 KiB GE
display list, whose size does not depend on the scene: a realm's first frame
takes it from the heap, later frames reuse it, and it is freed when the realm
that last rendered ends (every frame waits for the GE before returning, so the
GE never reads it after that). If the heap cannot supply it, that flush fails
and the context is lost, as for any other allocation failure.

PSP draw translation first counts the exact bounded vertex requirement, then
charges only that much scratch for the submission. All emitted vertices occupy
one contiguous prefix, so a single cache writeback just before `sceGuFinish`
publishes the batch. GE synchronization remains the ownership boundary before
CPU readback; command submission is deliberately synchronous.

The PSP's 8888 alpha channel doubles as its stencil plane, which is why the
drawing buffer is opaque (`alpha: false`): GE output is resolved to an opaque
native surface while each completed row is copied, rather than claiming
transparent-buffer behavior the hardware cannot implement consistently.

When a canvas frame is not published by the GE path above, an opaque,
untransformed canvas update can still publish without rebuilding the
viewport's ordinary tiles. The renderer keeps the previous RGB565 frame,
overwrites only the canvas rectangle, then replays later document paint and
fixed or sticky chrome in order. Scaled nearest-neighbor publication converts
each source pixel once per run and copies repeated source rows, keeping the
fast path bounded without another canvas-sized allocation. Transparent,
filtered, rounded, multiply-blended, or ambiguously ordered canvases refuse
this path and use the normal retained-tile renderer. A 2D canvas
knows whether it is opaque without a per-frame scan: each commit reads the
alpha of the rectangle it changed, and the whole surface only when that
rectangle covers the last translucent pixel found.

The host rasterizer exercises pixels, clipping, depth, culling, blending,
texture wrapping, and line closure. PPSSPP's software renderer is used when
an end-to-end browser fixture must read GE pixels back into the page
renderer.

PPSSPP's hardware renderer does not always keep the WebGL surface in EDRAM in
step with what the GE drew into it. When the GE scales a full-screen canvas
straight from that surface, a recording from the emulator can show a frame
drawn in parts with its later parts missing (for example the first frame of
a three.js scene with one mesh absent), even though the browser presents it
only once complete. A PSP shows the complete frame, so judge such frames on
the device.

## Preflight checklist

- The drawing buffer is deliberately PSP-sized.
- Shader compile and program link results are checked, and every program
  info log is empty (a non-empty log names logic the PSP does not run).
- The scene stays below the queried buffer, texture, draw, and vertex limits.
- No per-frame readback or object-graph allocation occurs.
- One animation chain pauses when the document is hidden.
- Gamepad disconnect and WebGL loss/restoration degrade cleanly.
- The game remains navigable before Page controls are entered.
- The offline install preview lists every required asset.
