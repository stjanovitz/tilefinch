(() => {
  "use strict";
  /* EXT_blend_minmax: MIN_EXT and MAX_EXT take the per-channel minimum or
     maximum of the fragment and the drawing buffer and ignore the blend
     factors, as WebGL does. Every pixel is checked against that reference
     computed here. The opaque drawing buffer shows no alpha, so the RGB
     equation decides the pixels and the alpha equation is only reported. */
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
    return { gl, positions, colors,
      tint: gl.getUniformLocation(program, "uTint") };
  };
  const target = make(document.getElementById("game"), false);
  const gl = target.gl;
  const constants = { MIN_EXT: 0x8007, MAX_EXT: 0x8008 };

  /* Before the extension is enabled its enums are invalid. */
  gl.blendEquation(constants.MIN_EXT);
  if (gl.getError() !== gl.INVALID_ENUM) failures.push("enum-before-enable");
  if (gl.getParameter(gl.BLEND_EQUATION_RGB) !== gl.FUNC_ADD
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== gl.FUNC_ADD
      || gl.getParameter(gl.BLEND_EQUATION) !== gl.FUNC_ADD)
    failures.push("default-equation");
  if (!gl.getSupportedExtensions().includes("EXT_blend_minmax"))
    failures.push("not-listed");
  const ext = gl.getExtension("EXT_blend_minmax");
  if (!ext || ext.MIN_EXT !== constants.MIN_EXT
      || ext.MAX_EXT !== constants.MAX_EXT
      || gl.getExtension("ext_BLEND_minmax") !== ext)
    failures.push("extension-object");
  if (!ext) { globalThis.pocSummary = "WEBGL-BLEND-MINMAX-NO-EXT"; return; }

  const byte = (value) => value / 255;
  const clearColor = [100, 150, 200];
  /* Two triangles covering the canvas; per-vertex colours constant per
     triangle so every pixel's expected value is exact. */
  const quad = (target, left, right) => {
    const g = target.gl;
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array([
      -1, -1, 1, -1, -1, 1,   1, -1, 1, 1, -1, 1]), g.DYNAMIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    const c = [];
    for (let v = 0; v < 6; v++) c.push(...(v < 3 ? left : right).map(byte), .5);
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
  /* Which triangle owns pixel (x, y): the lower-left one below the
     diagonal from (-1, 1) to (1, -1). Pixels on the diagonal are skipped. */
  const owner = (x, y) => {
    const s = x + y + 1 - W;
    return s < 0 ? 0 : s > 0 ? 1 : -1;
  };
  const run = (name, setup, left, right, expect) => {
    gl.clearColor(...clearColor.map(byte), 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    setup();
    quad(target, left, right);
    gl.uniform4f(target.tint, 1, 1, 1, 1);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    gl.finish();
    const error = gl.getError();
    const pixels = read(gl);
    let wrong = 0, first = "";
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++) {
        const side = owner(x, y);
        if (side < 0) continue;
        const want = expect(side === 0 ? left : right),
          at = (y * W + x) * 4;
        for (let k = 0; k < 3; k++)
          if (pixels[at + k] !== want[k]) {
            if (!wrong) first = `${x},${y}:${pixels.slice(at, at + 3)}/${want}`;
            wrong++;
          }
        if (pixels[at + 3] !== 255) wrong++;
      }
    if (error !== gl.NO_ERROR || wrong)
      failures.push(`${name}:error=${error}:wrong=${wrong}:${first}`);
  };
  const minimum = (color) => color.map((v, k) => Math.min(v, clearColor[k]));
  const maximum = (color) => color.map((v, k) => Math.max(v, clearColor[k]));
  const left = [50, 200, 100], right = [180, 20, 240];
  gl.enable(gl.BLEND);
  /* The factors are ignored, whatever they are. */
  for (const [source, destination] of [
      [gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA], [gl.ZERO, gl.ZERO],
      [gl.ONE, gl.ONE], [gl.ONE_MINUS_SRC_ALPHA, gl.SRC_ALPHA]]) {
    run(`min-${source}-${destination}`, () => {
      gl.blendFunc(source, destination); gl.blendEquation(ext.MIN_EXT);
    }, left, right, minimum);
    run(`max-${source}-${destination}`, () => {
      gl.blendFunc(source, destination); gl.blendEquation(ext.MAX_EXT);
    }, left, right, maximum);
  }
  /* A second draw over the first combines with what the first left. */
  gl.clearColor(...clearColor.map(byte), 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.blendEquation(ext.MAX_EXT);
  quad(target, left, left);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.blendEquation(ext.MIN_EXT);
  quad(target, right, right);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
  gl.finish();
  {
    const pixels = read(gl), want = [100, 20, 200];
    for (let k = 0; k < 3; k++)
      if (pixels[(8 * W + 3) * 4 + k] !== want[k]) failures.push("max-then-min");
  }
  /* Separate equations: the RGB equation draws; both are reported. */
  run("separate-min-add", () => {
    gl.blendFunc(gl.ONE, gl.ONE);
    gl.blendEquationSeparate(ext.MIN_EXT, gl.FUNC_ADD);
  }, left, right, minimum);
  if (gl.getParameter(gl.BLEND_EQUATION_RGB) !== ext.MIN_EXT
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== gl.FUNC_ADD)
    failures.push("separate-query");
  run("separate-add-max", () => {
    gl.blendFunc(gl.ONE, gl.ONE);
    gl.blendEquationSeparate(gl.FUNC_ADD, ext.MAX_EXT);
  }, left, right, (color) => color.map((v, k) => Math.min(255, v + clearColor[k])));
  /* An invalid equation stays an error and keeps the state. Subtraction
     is core WebGL and is covered by webgl-blend-subtract.js. */
  gl.blendEquation(ext.MAX_EXT);
  gl.blendEquation(0x800c);
  if (gl.getError() !== gl.INVALID_ENUM
      || gl.getParameter(gl.BLEND_EQUATION_RGB) !== ext.MAX_EXT)
    failures.push("invalid-refused");
  gl.blendEquationSeparate(ext.MIN_EXT, 0x800c);
  if (gl.getError() !== gl.INVALID_ENUM
      || gl.getParameter(gl.BLEND_EQUATION_ALPHA) !== ext.MAX_EXT)
    failures.push("separate-refused");
  /* Blending off ignores the equation. */
  run("disabled", () => {
    gl.blendEquation(ext.MIN_EXT); gl.disable(gl.BLEND);
  }, left, right, (color) => color);

  /* With antialiasing, a MIN/MAX triangle gets no partial-coverage fringe:
     it draws exactly the pixels it draws without antialiasing. */
  const canvas = document.createElement("canvas");
  document.body.appendChild(canvas);
  const smooth = make(canvas, true);
  smooth.gl.getExtension("EXT_blend_minmax");
  const sliver = (target) => {
    const g = target.gl;
    g.clearColor(...clearColor.map(byte), 1);
    g.clear(g.COLOR_BUFFER_BIT);
    g.enable(g.BLEND); g.blendEquation(ext.MIN_EXT);
    g.bindBuffer(g.ARRAY_BUFFER, target.positions);
    g.bufferData(g.ARRAY_BUFFER,
      new Float32Array([-.83, -.71, .77, -.42, -.12, .91]), g.STATIC_DRAW);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
    g.enableVertexAttribArray(0);
    g.disableVertexAttribArray(1);
    g.vertexAttrib4f(1, byte(left[0]), byte(left[1]), byte(left[2]), 1);
    g.uniform4f(target.tint, 1, 1, 1, 1);
    g.drawArrays(g.TRIANGLES, 0, 3);
    g.finish();
    return read(g);
  };
  {
    const sharp = sliver(target), smoothed = sliver(smooth);
    let differing = 0, covered = 0;
    for (let at = 0; at < sharp.length; at += 4) {
      if (sharp[at] !== smoothed[at] || sharp[at + 1] !== smoothed[at + 1]
          || sharp[at + 2] !== smoothed[at + 2]) differing++;
      if (sharp[at] !== clearColor[0]) covered++;
    }
    if (differing || covered < 40 || smooth.gl.getError() !== gl.NO_ERROR)
      failures.push(`antialias:differ=${differing}:covered=${covered}`);
  }
  globalThis.pocSummary = failures.length === 0 ? "WEBGL-BLEND-MINMAX-PASS"
    : `WEBGL-BLEND-MINMAX-FAIL:${failures.length}:${failures.slice(0, 8).join(",")}`;
})();
