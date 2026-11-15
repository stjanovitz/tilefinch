(() => {
  "use strict";
  /* Every accepted blend factor, in the source and the destination slot,
     with FUNC_ADD, FUNC_SUBTRACT and FUNC_REVERSE_SUBTRACT, checked pixel
     by pixel against the WebGL reference computed here: weigh each channel
     of the fragment and the drawing buffer, combine, clamp to [0, 255],
     round. The drawing buffer is opaque, so its alpha is 1: DST_ALPHA
     weighs as ONE, ONE_MINUS_DST_ALPHA and SRC_ALPHA_SATURATE (min(As,
     1 - Ad)) as ZERO. Constant factors weigh by blendColor() rounded to a
     byte per channel, the GE's fixed colours; the main grid uses a
     blendColor that is whole bytes, so that rounding is exact there. The
     GE weighs its source by the drawing buffer's colour and its
     destination by the fragment's, so SRC_COLOR and ONE_MINUS_SRC_COLOR
     as source colour factors, and DST_COLOR and ONE_MINUS_DST_COLOR as
     destination ones, are refused with INVALID_ENUM. */
  const failures = [];
  const TILE = 8;
  const make = (canvas, width, height, antialias) => {
    canvas.width = width; canvas.height = height;
    const gl = canvas.getContext("webgl", { alpha: false, antialias });
    const compile = (type, source) => {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source); gl.compileShader(shader); return shader;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, `
      attribute vec2 aPosition; attribute vec4 aColor;
      varying lowp vec4 vColor;
      void main(void) { gl_Position = vec4(aPosition, 0.0, 1.0); vColor = aColor; }`));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, `
      varying lowp vec4 vColor; uniform lowp vec4 uTint;
      void main(void) { gl_FragColor = vColor * uTint; }`));
    gl.bindAttribLocation(program, 0, "aPosition");
    gl.bindAttribLocation(program, 1, "aColor");
    gl.linkProgram(program); gl.useProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)
        || gl.getProgramInfoLog(program))
      failures.push("link:" + gl.getProgramInfoLog(program));
    gl.uniform4f(gl.getUniformLocation(program, "uTint"), 1, 1, 1, 1);
    return { gl, positions: gl.createBuffer(), colors: gl.createBuffer() };
  };
  const byte = (value) => value / 255;
  const alphaByte = 102; /* 0.4 */
  /* Two triangles over the viewport: the lower-left one LEFT, the other
     RIGHT. */
  const quad = (target, left, right, alpha = byte(alphaByte)) => {
    const g = target.gl;
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array([
      -1, -1, 1, -1, -1, 1,   1, -1, 1, 1, -1, 1]), g.STATIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    const c = [];
    for (let v = 0; v < 6; v++)
      c.push(...(v < 3 ? left : right).map(byte), alpha);
    g.bindBuffer(g.ARRAY_BUFFER, target.colors);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array(c), g.STATIC_DRAW);
    g.vertexAttribPointer(1, 4, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(1);
  };
  /* Three grids of 13 x 12 tiles, one per equation. */
  const sources = ["ZERO", "ONE", "SRC_ALPHA", "ONE_MINUS_SRC_ALPHA",
    "DST_COLOR", "ONE_MINUS_DST_COLOR", "DST_ALPHA", "ONE_MINUS_DST_ALPHA",
    "SRC_ALPHA_SATURATE", "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR",
    "CONSTANT_ALPHA", "ONE_MINUS_CONSTANT_ALPHA"];
  const destinations = ["ZERO", "ONE", "SRC_ALPHA", "ONE_MINUS_SRC_ALPHA",
    "SRC_COLOR", "ONE_MINUS_SRC_COLOR", "DST_ALPHA", "ONE_MINUS_DST_ALPHA",
    "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR", "CONSTANT_ALPHA",
    "ONE_MINUS_CONSTANT_ALPHA"];
  const W = 3 * sources.length * TILE, H = destinations.length * TILE;
  const target = make(document.getElementById("game"), W, H, false);
  const gl = target.gl;
  const SUBTRACT = gl.FUNC_SUBTRACT, REVERSE = gl.FUNC_REVERSE_SUBTRACT;
  const equations = [gl.FUNC_ADD, SUBTRACT, REVERSE];
  const equationNames = ["add", "sub", "rsub"];

  /* Constants and default queries. */
  const enums = { CONSTANT_COLOR: 0x8001, ONE_MINUS_CONSTANT_COLOR: 0x8002,
    CONSTANT_ALPHA: 0x8003, ONE_MINUS_CONSTANT_ALPHA: 0x8004,
    BLEND_COLOR: 0x8005, BLEND_DST_ALPHA: 0x80ca, BLEND_SRC_ALPHA: 0x80cb };
  for (const name in enums)
    if (gl[name] !== enums[name]) failures.push("constant-" + name);
  const factorsNow = () => [gl.BLEND_SRC_RGB, gl.BLEND_DST_RGB,
    gl.BLEND_SRC_ALPHA, gl.BLEND_DST_ALPHA].map((p) => gl.getParameter(p));
  const same = (a, b) => a.length === b.length && a.every((v, k) => v === b[k]);
  if (!same(factorsNow(), [gl.ONE, gl.ZERO, gl.ONE, gl.ZERO]))
    failures.push("default-factors:" + factorsNow());
  const color0 = gl.getParameter(gl.BLEND_COLOR);
  if (!(color0 instanceof Float32Array) || !same(color0, [0, 0, 0, 0]))
    failures.push("default-blend-color");
  /* blendColor() is clamped to [0, 1] and kept as float32. */
  gl.blendColor(-0.5, 0.3, 2, 0.25);
  const color1 = gl.getParameter(gl.BLEND_COLOR);
  if (gl.getError() !== gl.NO_ERROR
      || !same(color1, [0, Math.fround(0.3), 1, 0.25]))
    failures.push("blend-color-clamp:" + Array.from(color1));
  color1[0] = 1;
  if (gl.getParameter(gl.BLEND_COLOR)[0] !== 0) failures.push("blend-color-copy");

  /* Separate factors: the alpha factors may be any WebGL 1 factor, since
     the opaque buffer never shows them; they are kept and reported. */
  gl.blendFuncSeparate(gl.DST_COLOR, gl.SRC_COLOR, gl.SRC_COLOR,
    gl.DST_COLOR);
  if (gl.getError() !== gl.NO_ERROR || !same(factorsNow(),
      [gl.DST_COLOR, gl.SRC_COLOR, gl.SRC_COLOR, gl.DST_COLOR]))
    failures.push("query-separate:" + factorsNow());
  gl.blendFuncSeparate(gl.ONE, gl.ZERO, gl.SRC_ALPHA_SATURATE,
    enums.ONE_MINUS_CONSTANT_ALPHA);
  if (gl.getError() !== gl.NO_ERROR || !same(factorsNow(),
      [gl.ONE, gl.ZERO, gl.SRC_ALPHA_SATURATE, enums.ONE_MINUS_CONSTANT_ALPHA]))
    failures.push("query-separate-alpha:" + factorsNow());
  gl.blendFunc(gl.ONE_MINUS_DST_COLOR, gl.ONE_MINUS_SRC_COLOR);
  if (gl.getError() !== gl.NO_ERROR || !same(factorsNow(),
      [gl.ONE_MINUS_DST_COLOR, gl.ONE_MINUS_SRC_COLOR,
        gl.ONE_MINUS_DST_COLOR, gl.ONE_MINUS_SRC_COLOR]))
    failures.push("query-blend-func:" + factorsNow());
  /* Refused factors leave all four as they were. */
  const kept = factorsNow();
  for (const [args, error, name] of [
      [[gl.SRC_COLOR, gl.ZERO], gl.INVALID_ENUM, "src-color-source"],
      [[gl.ONE_MINUS_SRC_COLOR, gl.ONE], gl.INVALID_ENUM, "1-src-color-source"],
      [[gl.ZERO, gl.DST_COLOR], gl.INVALID_ENUM, "dst-color-destination"],
      [[gl.ONE, gl.ONE_MINUS_DST_COLOR], gl.INVALID_ENUM,
        "1-dst-color-destination"],
      [[gl.ONE, gl.SRC_ALPHA_SATURATE], gl.INVALID_ENUM, "saturate-destination"],
      [[0x0309, gl.ZERO], gl.INVALID_ENUM, "0x309"],
      [[gl.ONE, enums.BLEND_COLOR], gl.INVALID_ENUM, "0x8005"],
      [[2, gl.ONE], gl.INVALID_ENUM, "2"],
      [[gl.ONE, gl.ZERO, gl.ONE, gl.SRC_ALPHA_SATURATE], gl.INVALID_ENUM,
        "saturate-destination-alpha"],
      [[gl.ONE, gl.ZERO, gl.FUNC_ADD, gl.ZERO], gl.INVALID_ENUM,
        "equation-as-alpha-factor"],
      [[enums.CONSTANT_COLOR, enums.CONSTANT_ALPHA], gl.INVALID_OPERATION,
        "constant-colour-with-alpha"],
      [[enums.ONE_MINUS_CONSTANT_ALPHA, enums.ONE_MINUS_CONSTANT_COLOR],
        gl.INVALID_OPERATION, "constant-alpha-with-colour"],
      [[enums.CONSTANT_ALPHA, enums.CONSTANT_COLOR, gl.ONE, gl.ONE],
        gl.INVALID_OPERATION, "separate-constant-mix"]]) {
    if (args.length === 2) gl.blendFunc(...args);
    else gl.blendFuncSeparate(...args);
    const got = gl.getError();
    if (got !== error || !same(factorsNow(), kept))
      failures.push(`refused-${name}:error=${got}:${factorsNow()}`);
  }
  /* Only the colour pair may not mix constant colour and alpha. */
  gl.blendFuncSeparate(gl.ONE, gl.ZERO, enums.CONSTANT_COLOR,
    enums.CONSTANT_ALPHA);
  gl.blendFunc(enums.CONSTANT_COLOR, enums.ONE_MINUS_CONSTANT_COLOR);
  gl.blendFunc(enums.CONSTANT_ALPHA, enums.ONE_MINUS_CONSTANT_ALPHA);
  if (gl.getError() !== gl.NO_ERROR) failures.push("constant-pairs");

  /* The WebGL reference for one channel K. BLEND is blendColor(). */
  const fixed = (value) => Math.round(value * 255) / 255;
  const weight = (name, k, source, destination, alpha, blend) => {
    switch (name) {
      case "ZERO": case "ONE_MINUS_DST_ALPHA": case "SRC_ALPHA_SATURATE":
        return 0;
      case "ONE": case "DST_ALPHA": return 1;
      case "SRC_ALPHA": return alpha;
      case "ONE_MINUS_SRC_ALPHA": return 1 - alpha;
      case "SRC_COLOR": return source[k] / 255;
      case "ONE_MINUS_SRC_COLOR": return 1 - source[k] / 255;
      case "DST_COLOR": return destination[k] / 255;
      case "ONE_MINUS_DST_COLOR": return 1 - destination[k] / 255;
      case "CONSTANT_COLOR": return fixed(blend[k]);
      case "ONE_MINUS_CONSTANT_COLOR": return fixed(1 - blend[k]);
      case "CONSTANT_ALPHA": return fixed(blend[3]);
      case "ONE_MINUS_CONSTANT_ALPHA": return fixed(1 - blend[3]);
    }
    throw new Error("factor " + name);
  };
  const blend = (equation, source, destination, sfactor, dfactor,
                 blendColor, alpha = alphaByte / 255) =>
    source.map((value, k) => {
      const s = value * weight(sfactor, k, source, destination, alpha,
          blendColor),
        d = destination[k] * weight(dfactor, k, source, destination, alpha,
          blendColor),
        v = equation === SUBTRACT ? s - d
          : equation === REVERSE ? d - s : s + d;
      return Math.round(Math.min(255, Math.max(0, v)));
    });
  const factor = (name) => enums[name] || gl[name];

  /* The host renderer rounds each blended channel once, as the reference
     does. The PSP GE truncates each weighted term, so a page run of this
     fixture on the GE (not part of the host suite) may set
     __webglBlendTolerance to 1 and gets the off-by-one channels listed in
     __webglBlendNearMisses. */
  const tolerance = globalThis.__webglBlendTolerance || 0;
  const nearMisses = globalThis.__webglBlendNearMisses = [];
  const read = (g, width, height) => {
    const pixels = new Uint8Array(width * height * 4);
    g.readPixels(0, 0, width, height, g.RGBA, g.UNSIGNED_BYTE, pixels);
    return pixels;
  };
  /* The lower-left triangle owns the pixels below the tile's diagonal;
     pixels on the diagonal are skipped. */
  const owner = (x, y) => {
    const s = x + y + 1 - TILE;
    return s < 0 ? 0 : s > 0 ? 1 : -1;
  };
  const checkTile = (name, pixels, width, column, row, expect) => {
    let wrong = 0, first = "", near = 0;
    for (let y = 0; y < TILE; y++)
      for (let x = 0; x < TILE; x++) {
        const side = owner(x, y);
        if (side < 0) continue;
        const want = expect(side),
          at = ((row * TILE + y) * width + column * TILE + x) * 4;
        for (let k = 0; k < 3; k++)
          if (pixels[at + k] !== want[k]) {
            if (Math.abs(pixels[at + k] - want[k]) <= tolerance) {
              near++; continue;
            }
            if (!wrong) first = `${x},${y}:${pixels.slice(at, at + 3)}/${want}`;
            wrong++;
          }
        if (pixels[at + 3] !== 255) wrong++;
      }
    if (near) nearMisses.push(`${name}:${near}`);
    if (wrong) failures.push(`${name}:wrong=${wrong}:${first}`);
  };
  const clearColor = [100, 150, 200];
  const clear = () => {
    gl.viewport(0, 0, W, H);
    gl.clearColor(...clearColor.map(byte), 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
  };
  const tileViewport = (column, row) =>
    gl.viewport(column * TILE, row * TILE, TILE, TILE);
  /* Each side has channels below, near and above the clear colour, so
     every pair has results that clamp and results that do not. */
  const left = [50, 200, 230], right = [255, 20, 120];
  const sides = [left, right];
  /* Whole bytes, so the GE's byte-wide fixed colours are exact. */
  const blendBytes = [60, 140, 220, 90];
  gl.blendColor(...blendBytes.map(byte));
  const blendColor = Array.from(gl.getParameter(gl.BLEND_COLOR));
  /* WebGL refuses a constant colour factor with a constant alpha one
     (checked above); those tiles keep the clear colour. */
  const constantKind = (name) => name.endsWith("CONSTANT_COLOR") ? 1
    : name.endsWith("CONSTANT_ALPHA") ? 2 : 0;
  const mixed = (s, d) => constantKind(s) && constantKind(d)
    && constantKind(s) !== constantKind(d);
  gl.enable(gl.BLEND);
  clear();
  quad(target, left, right);
  equations.forEach((equation, e) => {
    gl.blendEquation(equation);
    sources.forEach((s, i) => destinations.forEach((d, j) => {
      if (mixed(s, d)) return;
      gl.blendFunc(factor(s), factor(d));
      tileViewport(e * sources.length + i, j);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
    }));
  });
  if (gl.getError() !== gl.NO_ERROR) failures.push("grid-error");
  const grid = read(gl, W, H);
  equations.forEach((equation, e) => sources.forEach((s, i) =>
    destinations.forEach((d, j) => checkTile(
      `${equationNames[e]}-${s}-${d}`, grid, W, e * sources.length + i, j,
      (side) => mixed(s, d) ? clearColor
        : blend(equation, sides[side], clearColor, s, d, blendColor)))));

  /* Later checks draw into tiles of the same canvas and read it once. */
  const tiles = [];
  let tileAt = 0;
  const nextTile = (name, expect) => {
    const column = tileAt % (W / TILE), row = Math.floor(tileAt / (W / TILE));
    tileAt++;
    tiles.push([name, column, row, expect]);
    tileViewport(column, row);
  };
  const readTiles = () => {
    const pixels = read(gl, W, H);
    for (const [name, column, row, expect] of tiles)
      checkTile(name, pixels, W, column, row, expect);
    tiles.length = 0; tileAt = 0;
  };
  clear();
  gl.blendEquation(gl.FUNC_ADD);

  /* three.js r128 setBlending(), as its minified source calls it, with
     blendEquation(FUNC_ADD): MultiplyBlending and SubtractiveBlending with
     and without premultipliedAlpha. The premultiplied SubtractiveBlending
     colour pair is ZERO, ZERO (black); its ONE_MINUS_SRC_COLOR source alpha
     factor is accepted, as the alpha factors never show. */
  for (const [name, args, s, d] of [
      ["three-multiply", [0, 768], "ZERO", "SRC_COLOR"],
      ["three-subtractive", [0, 769], "ZERO", "ONE_MINUS_SRC_COLOR"],
      ["three-multiply-premultiplied", [0, 768, 0, 770], "ZERO", "SRC_COLOR"],
      ["three-subtractive-premultiplied", [0, 0, 769, 771], "ZERO", "ZERO"]]) {
    gl.blendEquation(32774);
    if (args.length === 2) gl.blendFunc(...args);
    else gl.blendFuncSeparate(...args);
    if (gl.getError() !== gl.NO_ERROR) failures.push(name + "-error");
    nextTile(name, (side) => blend(gl.FUNC_ADD, sides[side], clearColor, s, d,
      blendColor));
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }

  /* blendFuncSeparate: the colour factors draw all channels. */
  gl.blendEquation(REVERSE);
  gl.blendFuncSeparate(gl.DST_COLOR, gl.SRC_COLOR, gl.ONE, gl.ZERO);
  nextTile("rsub-separate-colour", (side) => blend(REVERSE, sides[side],
    clearColor, "DST_COLOR", "SRC_COLOR", blendColor));
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.blendEquation(gl.FUNC_ADD);
  gl.blendFuncSeparate(enums.CONSTANT_ALPHA, enums.ONE_MINUS_CONSTANT_ALPHA,
    gl.SRC_COLOR, gl.ONE_MINUS_DST_COLOR);
  nextTile("add-separate-constant", (side) => blend(gl.FUNC_ADD, sides[side],
    clearColor, "CONSTANT_ALPHA", "ONE_MINUS_CONSTANT_ALPHA", blendColor));
  gl.drawArrays(gl.TRIANGLES, 0, 6);

  /* A blendColor that is not whole bytes rounds to the nearest byte per
     channel, for each factor. */
  gl.blendColor(0.3, 0.55, 0.81, 0.47);
  const roundedColor = Array.from(gl.getParameter(gl.BLEND_COLOR));
  for (const s of ["CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR",
      "CONSTANT_ALPHA", "ONE_MINUS_CONSTANT_ALPHA"]) {
    gl.blendFunc(factor(s), gl.ONE_MINUS_SRC_ALPHA);
    nextTile("rounded-" + s, (side) => blend(gl.FUNC_ADD, sides[side],
      clearColor, s, "ONE_MINUS_SRC_ALPHA", roundedColor));
    gl.drawArrays(gl.TRIANGLES, 0, 6);
  }

  /* One draw repeated in one batch while blendColor() changes between the
     draws: each queued draw keeps the colour it was drawn with. */
  {
    const colours = [[255, 0, 128, 255], [10, 200, 60, 51], [120, 120, 255, 0],
      [0, 255, 0, 204]];
    gl.blendEquation(gl.FUNC_ADD);
    gl.blendFunc(enums.CONSTANT_COLOR, enums.ONE_MINUS_CONSTANT_COLOR);
    let want = [clearColor, clearColor];
    nextTile("repeated-blend-color", (side) => want[side]);
    for (const colour of colours) {
      gl.blendColor(...colour.map(byte));
      const current = Array.from(gl.getParameter(gl.BLEND_COLOR));
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      want = want.map((retained, side) => blend(gl.FUNC_ADD, sides[side],
        retained, "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR", current));
    }
  }

  /* A sequence in one frame mixes the equations with colour, constant and
     destination-alpha factors; each draw combines with what the one before
     it left. */
  {
    gl.blendColor(...blendBytes.map(byte));
    const steps = [
      [gl.FUNC_ADD, "DST_COLOR", "ONE", [60, 10, 40]],
      [SUBTRACT, "ONE", "ONE_MINUS_SRC_COLOR", [200, 90, 255]],
      [REVERSE, "ONE_MINUS_DST_COLOR", "SRC_COLOR", [255, 40, 100]],
      [gl.FUNC_ADD, "CONSTANT_COLOR", "ONE_MINUS_CONSTANT_COLOR",
        [120, 250, 25]],
      [gl.FUNC_ADD, "ZERO", "SRC_COLOR", [130, 200, 255]],
      [SUBTRACT, "DST_ALPHA", "ONE_MINUS_DST_ALPHA", [30, 30, 30]],
      [REVERSE, "SRC_ALPHA_SATURATE", "CONSTANT_ALPHA", [40, 255, 10]],
      [gl.FUNC_ADD, "ONE_MINUS_CONSTANT_ALPHA", "DST_ALPHA", [255, 255, 0]],
    ];
    const run = (name, draws) => {
      let want = clearColor;
      nextTile(name, () => want);
      for (const [equation, s, d, colour] of draws) {
        gl.blendEquation(equation);
        gl.blendFunc(factor(s), factor(d));
        quad(target, colour, colour);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        want = blend(equation, colour, want, s, d, blendColor);
      }
    };
    run("sequence", steps);
    run("sequence-reversed", steps.slice().reverse());
    quad(target, left, right);
  }

  /* MIN_EXT and MAX_EXT ignore colour factors too. */
  const minmax = gl.getExtension("EXT_blend_minmax");
  gl.blendFunc(gl.DST_COLOR, gl.SRC_COLOR);
  gl.blendEquation(minmax.MIN_EXT);
  nextTile("min-colour-factors", (side) =>
    sides[side].map((v, k) => Math.min(v, clearColor[k])));
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.blendEquation(minmax.MAX_EXT);
  nextTile("max-colour-factors", (side) =>
    sides[side].map((v, k) => Math.max(v, clearColor[k])));
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.blendEquation(gl.FUNC_ADD);

  /* Blending off ignores the factors. */
  gl.blendFunc(gl.ZERO, gl.SRC_COLOR);
  gl.disable(gl.BLEND);
  nextTile("disabled", (side) => sides[side]);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.enable(gl.BLEND);
  if (gl.getError() !== gl.NO_ERROR) failures.push("tiles-error");
  readTiles();

  /* With antialiasing, a draw with colour or constant factors gets no
     partial-coverage fringe: it draws exactly the pixels it draws without
     antialiasing. A destination-alpha factor draws as ONE, fringe
     included. */
  const canvas = document.createElement("canvas");
  document.body.appendChild(canvas);
  const smooth = make(canvas, 16, 16, true);
  const sliver = (target, sfactor, dfactor) => {
    const g = target.gl;
    g.viewport(0, 0, 16, 16);
    g.clearColor(...clearColor.map(byte), 1);
    g.clear(g.COLOR_BUFFER_BIT);
    g.enable(g.BLEND); g.blendEquation(g.FUNC_ADD);
    g.blendColor(...blendBytes.map(byte));
    g.blendFunc(sfactor, dfactor);
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER,
      new Float32Array([-.83, -.71, .77, -.42, -.12, .91]), g.STATIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    g.disableVertexAttribArray(1);
    g.vertexAttrib4f(1, byte(right[0]), byte(right[1]), byte(right[2]),
      byte(alphaByte));
    g.drawArrays(g.TRIANGLES, 0, 3);
    g.finish();
    return read(g, 16, 16);
  };
  const differ = (a, b) => {
    let count = 0;
    for (let at = 0; at < a.length; at += 4)
      if (a[at] !== b[at] || a[at + 1] !== b[at + 1] || a[at + 2] !== b[at + 2])
        count++;
    return count;
  };
  for (const [s, d] of [["ZERO", "SRC_COLOR"], ["DST_COLOR", "ZERO"],
      ["ZERO", "ONE_MINUS_SRC_COLOR"], ["ONE_MINUS_DST_COLOR", "ONE"],
      ["CONSTANT_ALPHA", "ONE_MINUS_CONSTANT_ALPHA"],
      ["SRC_ALPHA", "ONE_MINUS_CONSTANT_COLOR"]]) {
    const sharp = sliver(target, factor(s), factor(d)),
      smoothed = sliver(smooth, factor(s), factor(d));
    let covered = 0;
    for (let at = 0; at < sharp.length; at += 4)
      if (sharp[at + 1] !== clearColor[1]) covered++;
    const differing = differ(sharp, smoothed);
    if (differing || covered < 40 || smooth.gl.getError() !== gl.NO_ERROR)
      failures.push(`antialias-${s}-${d}:differ=${differing}:covered=${covered}`);
  }
  {
    const one = sliver(smooth, gl.SRC_ALPHA, gl.ONE),
      dstAlpha = sliver(smooth, gl.SRC_ALPHA, gl.DST_ALPHA),
      sharp = sliver(target, gl.SRC_ALPHA, gl.DST_ALPHA);
    if (differ(one, dstAlpha) || !differ(sharp, dstAlpha))
      failures.push(`antialias-dst-alpha:${differ(one, dstAlpha)}:`
        + differ(sharp, dstAlpha));
  }
  globalThis.pocSummary = failures.length === 0 ? "WEBGL-BLEND-FACTORS-PASS"
    : `WEBGL-BLEND-FACTORS-FAIL:${failures.length}:${failures.slice(0, 8).join(",")}`;
})();
