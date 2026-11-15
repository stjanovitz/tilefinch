(() => {
  "use strict";
  /* FUNC_SUBTRACT and FUNC_REVERSE_SUBTRACT (core WebGL 1) weight the
     fragment and the drawing buffer by the blend factors, then subtract:
     source * sfactor - destination * dfactor, or the reverse, clamped to
     [0, 1] as a unorm buffer stores it. Every pixel is checked against
     that reference computed here. The opaque drawing buffer shows no
     alpha, so the RGB equation decides the pixels and the alpha equation
     is only reported. */
  const failures = [];
  const W = 16, H = 16;
  const make = (canvas, antialias) => {
    canvas.width = W; canvas.height = H;
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
    const positions = gl.createBuffer(), colors = gl.createBuffer();
    gl.viewport(0, 0, W, H);
    gl.uniform4f(gl.getUniformLocation(program, "uTint"), 1, 1, 1, 1);
    return { gl, positions, colors };
  };
  const target = make(document.getElementById("game"), false);
  const gl = target.gl;
  const SUBTRACT = 0x800a, REVERSE = 0x800b;

  /* The constants and queries, with no extension. */
  if (gl.FUNC_SUBTRACT !== SUBTRACT || gl.FUNC_REVERSE_SUBTRACT !== REVERSE)
    failures.push("constants");
  if (gl.getParameter(gl.BLEND_EQUATION) !== gl.FUNC_ADD
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== gl.FUNC_ADD)
    failures.push("default-equation");
  gl.blendEquation(SUBTRACT);
  if (gl.getError() !== gl.NO_ERROR
      || gl.getParameter(gl.BLEND_EQUATION) !== SUBTRACT
      || gl.getParameter(gl.BLEND_EQUATION_RGB) !== SUBTRACT
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== SUBTRACT)
    failures.push("query-subtract");
  gl.blendEquationSeparate(REVERSE, SUBTRACT);
  if (gl.getError() !== gl.NO_ERROR
      || gl.getParameter(gl.BLEND_EQUATION_RGB) !== REVERSE
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== SUBTRACT)
    failures.push("query-separate");
  /* MIN_EXT is still an extension enum; an invalid equation keeps both. */
  gl.blendEquation(0x8007);
  if (gl.getError() !== gl.INVALID_ENUM
      || gl.getParameter(gl.BLEND_EQUATION_RGB) !== REVERSE)
    failures.push("min-without-extension");
  gl.blendEquationSeparate(SUBTRACT, 0x800c);
  if (gl.getError() !== gl.INVALID_ENUM
      || gl.getParameter(gl.BLEND_EQUATION_RGB) !== REVERSE
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== SUBTRACT)
    failures.push("invalid-keeps-state");

  const byte = (value) => value / 255;
  const clearColor = [100, 150, 200];
  const alphaByte = 102; /* 0.4 */
  const quad = (target, left, right, alpha = byte(alphaByte)) => {
    const g = target.gl;
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array([
      -1, -1, 1, -1, -1, 1,   1, -1, 1, 1, -1, 1]), g.DYNAMIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    const c = [];
    for (let v = 0; v < 6; v++)
      c.push(...(v < 3 ? left : right).map(byte), alpha);
    g.bindBuffer(g.ARRAY_BUFFER, target.colors);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array(c), g.DYNAMIC_DRAW);
    g.vertexAttribPointer(1, 4, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(1);
  };
  const read = (g) => {
    const pixels = new Uint8Array(W * H * 4);
    g.readPixels(0, 0, W, H, g.RGBA, g.UNSIGNED_BYTE, pixels);
    return pixels;
  };
  /* The lower-left triangle owns the pixels below the diagonal from
     (-1, 1) to (1, -1); pixels on the diagonal are skipped. */
  const owner = (x, y) => {
    const s = x + y + 1 - W;
    return s < 0 ? 0 : s > 0 ? 1 : -1;
  };
  /* The WebGL reference: weight, subtract, clamp to [0, 255], round. */
  const weight = (factor, alpha) => factor === gl.ZERO ? 0
    : factor === gl.ONE ? 1 : factor === gl.SRC_ALPHA ? alpha : 1 - alpha;
  const blend = (equation, source, destination, sfactor, dfactor,
                 alpha = alphaByte / 255) =>
    source.map((value, k) => {
      const s = value * weight(sfactor, alpha),
        d = destination[k] * weight(dfactor, alpha),
        v = equation === SUBTRACT ? s - d
          : equation === REVERSE ? d - s : s + d;
      return Math.round(Math.min(255, Math.max(0, v)));
    });
  /* The host renderer rounds each blended channel once, as the
     reference does. The PSP GE truncates each weighted term, so a page run
     of this fixture on the GE (not part of the host suite) may set
     __webglBlendTolerance to 1 and gets the off-by-one channels listed in
     __webglBlendNearMisses. */
  const tolerance = globalThis.__webglBlendTolerance || 0;
  const nearMisses = globalThis.__webglBlendNearMisses = [];
  const check = (name, pixels, expect) => {
    let wrong = 0, first = "", near = 0;
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++) {
        const side = owner(x, y);
        if (side < 0) continue;
        const want = expect(side), at = (y * W + x) * 4;
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
    const error = gl.getError();
    if (error !== gl.NO_ERROR || wrong)
      failures.push(`${name}:error=${error}:wrong=${wrong}:${first}`);
  };
  const clear = () => {
    gl.clearColor(...clearColor.map(byte), 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
  };
  /* Each side has channels below, near and above the clear colour, so
     every pair has results that clamp at zero and results that do not. */
  const left = [50, 200, 230], right = [255, 20, 120];
  const sides = [left, right];
  gl.enable(gl.BLEND);
  const factors = [gl.ZERO, gl.ONE, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA];
  const names = ["0", "1", "a", "1-a"];
  for (const equation of [SUBTRACT, REVERSE])
    for (let s = 0; s < factors.length; s++)
      for (let d = 0; d < factors.length; d++) {
        clear();
        gl.blendEquation(equation);
        gl.blendFunc(factors[s], factors[d]);
        quad(target, left, right);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        check(`${equation === SUBTRACT ? "sub" : "rsub"}-${names[s]}-${names[d]}`,
          read(gl), (side) => blend(equation, sides[side], clearColor,
            factors[s], factors[d]));
      }

  /* blendFuncSeparate: the colour factors draw all channels. */
  clear();
  gl.blendEquation(REVERSE);
  gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE, gl.ONE, gl.ZERO);
  quad(target, left, right);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  check("rsub-separate-factors", read(gl), (side) => blend(REVERSE,
    sides[side], clearColor, gl.SRC_ALPHA, gl.ONE));

  /* Separate equations: the RGB equation draws; the alpha one is kept. */
  clear();
  gl.blendFunc(gl.ONE, gl.ONE);
  gl.blendEquationSeparate(SUBTRACT, gl.FUNC_ADD);
  quad(target, left, right);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  check("separate-sub-add", read(gl), (side) => blend(SUBTRACT,
    sides[side], clearColor, gl.ONE, gl.ONE));
  clear();
  gl.blendEquationSeparate(gl.FUNC_ADD, REVERSE);
  quad(target, left, right);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  check("separate-add-rsub", read(gl), (side) => blend(gl.FUNC_ADD,
    sides[side], clearColor, gl.ONE, gl.ONE));

  /* Blending off ignores the equation. */
  clear();
  gl.blendEquation(SUBTRACT);
  gl.disable(gl.BLEND);
  quad(target, left, right);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  check("disabled", read(gl), (side) => sides[side]);
  gl.enable(gl.BLEND);

  /* Sequences in one frame mix ADD, MIN, MAX and both subtractions; each
     draw combines with what the one before it left. The same draw is
     queued with each equation, so the queued commands must keep each
     draw's own equation and factors. */
  const ext = gl.getExtension("EXT_blend_minmax");
  if (!ext) failures.push("no-minmax");
  else {
    const steps = [
      [gl.FUNC_ADD, gl.SRC_ALPHA, gl.ONE, [60, 10, 40]],
      [SUBTRACT, gl.ONE, gl.ONE_MINUS_SRC_ALPHA, [200, 90, 255]],
      [ext.MAX_EXT, gl.ZERO, gl.ZERO, [70, 30, 90]],
      [REVERSE, gl.SRC_ALPHA, gl.ONE, [255, 40, 100]],
      [ext.MIN_EXT, gl.ONE, gl.ONE, [120, 250, 25]],
      [gl.FUNC_ADD, gl.ONE, gl.ONE, [30, 30, 30]],
      [REVERSE, gl.ONE, gl.ONE, [40, 255, 10]],
      [SUBTRACT, gl.ONE_MINUS_SRC_ALPHA, gl.SRC_ALPHA, [255, 255, 0]],
    ];
    const run = (name, draws) => {
      clear();
      let want = clearColor;
      for (const [equation, sfactor, dfactor, color] of draws) {
        gl.blendEquation(equation);
        gl.blendFunc(sfactor, dfactor);
        quad(target, color, color);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        want = equation === ext.MIN_EXT
          ? color.map((v, k) => Math.min(v, want[k]))
          : equation === ext.MAX_EXT
            ? color.map((v, k) => Math.max(v, want[k]))
            : blend(equation, color, want, sfactor, dfactor);
      }
      check(name, read(gl), () => want);
    };
    run("sequence", steps);
    run("sequence-reversed", steps.slice().reverse());
    /* One draw, repeated with each equation, in one batch. */
    clear();
    quad(target, left, right);
    gl.blendFunc(gl.ONE, gl.ONE);
    let want = [clearColor, clearColor];
    for (const equation of [SUBTRACT, gl.FUNC_ADD, REVERSE, ext.MAX_EXT,
        REVERSE, ext.MIN_EXT, gl.FUNC_ADD]) {
      gl.blendEquation(equation);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      want = want.map((retained, side) => equation === ext.MIN_EXT
        ? sides[side].map((v, k) => Math.min(v, retained[k]))
        : equation === ext.MAX_EXT
          ? sides[side].map((v, k) => Math.max(v, retained[k]))
          : blend(equation, sides[side], retained, gl.ONE, gl.ONE));
    }
    check("repeated-draw", read(gl), (side) => want[side]);
    /* With EXT_blend_minmax, the RGB equation still decides the pixels. */
    clear();
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    gl.blendEquationSeparate(REVERSE, ext.MIN_EXT);
    quad(target, left, right);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    check("separate-rsub-min", read(gl), (side) => blend(REVERSE,
      sides[side], clearColor, gl.SRC_ALPHA, gl.ONE));
    if (gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== ext.MIN_EXT)
      failures.push("separate-min-query");
  }

  /* With antialiasing, a subtract triangle gets no partial-coverage
     fringe: it draws exactly the pixels it draws without antialiasing. */
  const canvas = document.createElement("canvas");
  document.body.appendChild(canvas);
  const smooth = make(canvas, true);
  const sliver = (target, equation) => {
    const g = target.gl;
    g.clearColor(...clearColor.map(byte), 1);
    g.clear(g.COLOR_BUFFER_BIT);
    g.enable(g.BLEND); g.blendEquation(equation);
    g.blendFunc(g.ONE, g.ONE);
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER,
      new Float32Array([-.83, -.71, .77, -.42, -.12, .91]), g.STATIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    g.disableVertexAttribArray(1);
    g.vertexAttrib4f(1, byte(right[0]), byte(right[1]), byte(right[2]), 1);
    g.drawArrays(g.TRIANGLES, 0, 3);
    g.finish();
    return read(g);
  };
  for (const equation of [SUBTRACT, REVERSE]) {
    const sharp = sliver(target, equation),
      smoothed = sliver(smooth, equation);
    let differing = 0, covered = 0;
    for (let at = 0; at < sharp.length; at += 4) {
      if (sharp[at] !== smoothed[at] || sharp[at + 1] !== smoothed[at + 1]
          || sharp[at + 2] !== smoothed[at + 2]) differing++;
      if (sharp[at + 1] !== clearColor[1]) covered++;
    }
    if (differing || covered < 40 || smooth.gl.getError() !== gl.NO_ERROR)
      failures.push(`antialias-${equation}:differ=${differing}:covered=${covered}`);
  }
  globalThis.pocSummary = failures.length === 0 ? "WEBGL-BLEND-SUBTRACT-PASS"
    : `WEBGL-BLEND-SUBTRACT-FAIL:${failures.length}:${failures.slice(0, 8).join(",")}`;
})();
