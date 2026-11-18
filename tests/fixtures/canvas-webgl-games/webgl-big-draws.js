(() => {
  "use strict";
  /* Draws over one native submission's 4,096-vertex budget are queued in
     parts. Each case draws once with one call and once as the page itself
     would split it (smaller calls at other boundaries); the pixels must be
     identical, with opaque overwrites (order), additive blending (each
     primitive exactly once) and depth testing. Shapes that cannot be split
     exactly (TRIANGLE_FAN, LINE_LOOP) and draws over 64 instances are
     refused with OUT_OF_MEMORY and one console warning per program and
     mode. */
  const W = 64, H = 48;
  const canvas = document.getElementById("game");
  canvas.width = W; canvas.height = H;
  const gl = canvas.getContext("webgl", { alpha: false, depth: true });
  const ext = gl && gl.getExtension("ANGLE_instanced_arrays");
  if (!ext) { globalThis.pocSummary = "WEBGL-BIG-DRAWS-NO-EXT"; return; }
  const failures = [];
  const warnings = [];
  /* The browser's own warnings do not pass through a page's console.warn;
     host and validation realms hand them to this sink. */
  globalThis.__tilefinchTestConsoleSink = function (level, ...args) {
    if (level === "warn") warnings.push(args.join(" "));
  };
  let seed = 0x1d872b41;
  const random = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
  };
  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source); gl.compileShader(shader); return shader;
  };
  const link = (vertex, fragment, locations) => {
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, vertex));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragment));
    locations.forEach((name, at) => gl.bindAttribLocation(program, at, name));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)
        || gl.getProgramInfoLog(program))
      failures.push("link:" + gl.getProgramInfoLog(program));
    return program;
  };
  /* uScale dims the additive runs so overlapping primitives never
     saturate: a primitive drawn twice or missed changes the sum. */
  const fragment = `varying lowp vec4 vColor; uniform lowp vec4 uScale;
    void main(void) { gl_FragColor = vColor * uScale; }`;
  const plain = link(`attribute vec3 aPosition; attribute vec4 aColor;
    uniform mat4 uP; varying lowp vec4 vColor;
    void main(void) { gl_Position = uP * vec4(aPosition, 1.0); vColor = aColor; }`,
    fragment, ["aPosition", "aColor"]);
  const compact = link(`attribute vec3 aPosition; attribute vec4 aColor;
    attribute vec4 aInstanceTransform; attribute vec4 aInstanceTint;
    uniform mat4 uP; varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uP * vec4(
        aPosition * aInstanceTransform.w + aInstanceTransform.xyz, 1.0);
      vColor = aColor * aInstanceTint;
    }`, fragment,
    ["aPosition", "aColor", "aInstanceTransform", "aInstanceTint"]);
  const matrixProgram = link(`attribute vec3 aPosition; attribute vec4 aColor;
    attribute mat4 aInstance; uniform mat4 uP; varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uP * aInstance * vec4(aPosition, 1.0); vColor = aColor;
    }`, fragment, ["aPosition", "aColor", "aInstance"]);
  const identity = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
  const scales = new Map();
  for (const program of [plain, compact, matrixProgram]) {
    gl.useProgram(program);
    gl.uniformMatrix4fv(gl.getUniformLocation(program, "uP"), false, identity);
    scales.set(program, gl.getUniformLocation(program, "uScale"));
  }
  const scale = (value) => {
    const program = gl.getParameter(gl.CURRENT_PROGRAM);
    gl.uniform4f(scales.get(program), value, value, value, 1);
  };
  const positions = gl.createBuffer(), colors = gl.createBuffer(),
    indices = gl.createBuffer(), instances = gl.createBuffer(),
    tints = gl.createBuffer();
  gl.viewport(0, 0, W, H);

  /* count vertices: small random triangles (or line/point positions) with
     random colours; `walk` keeps successive vertices close, as strips
     need, and `z` spreads depth for the depth-tested runs. */
  const geometry = (count, walk, size = .5) => {
    const p = new Float32Array(count * 3), c = new Float32Array(count * 4);
    let x = 0, y = 0;
    for (let v = 0; v < count; v++) {
      if (walk) {
        x = Math.max(-1, Math.min(1, x + (random() - .5) * size));
        y = Math.max(-1, Math.min(1, y + (random() - .5) * size));
      } else if (v % 3 === 0) {
        x = random() * 2 - 1; y = random() * 2 - 1;
      }
      p[v * 3] = walk ? x : x + (random() - .5) * size;
      p[v * 3 + 1] = walk ? y : y + (random() - .5) * size;
      p[v * 3 + 2] = random() * 1.8 - .9;
      c[v * 4] = random(); c[v * 4 + 1] = random();
      c[v * 4 + 2] = random(); c[v * 4 + 3] = 1;
    }
    return { p, c };
  };
  const bindGeometry = (program, data) => {
    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, positions);
    gl.bufferData(gl.ARRAY_BUFFER, data.p, gl.DYNAMIC_DRAW);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(0);
    gl.bindBuffer(gl.ARRAY_BUFFER, colors);
    gl.bufferData(gl.ARRAY_BUFFER, data.c, gl.DYNAMIC_DRAW);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(1);
  };
  const STATES = {
    opaque: () => {
      gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); scale(1);
    },
    additive: () => {
      gl.disable(gl.DEPTH_TEST); gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE); scale(1 / 32);
    },
    depth: () => {
      gl.disable(gl.BLEND); gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS);
      scale(1);
    },
  };
  const frame = (state, cull, draw) => {
    gl.clearColor(0, 0, 0, 1); gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    STATES[state]();
    if (cull) gl.enable(gl.CULL_FACE); else gl.disable(gl.CULL_FACE);
    draw();
    gl.finish();
    const pixels = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    return pixels;
  };
  /* The single draw must equal the page's own split, draw something, and
     leave no error. */
  const compare = (name, state, cull, single, reference) => {
    const one = frame(state, cull, single);
    const error = gl.getError();
    const parts = frame(state, cull, reference);
    let differing = 0, lit = 0;
    for (let at = 0; at < one.length; at += 4) {
      if (one[at] !== parts[at] || one[at + 1] !== parts[at + 1]
          || one[at + 2] !== parts[at + 2]) differing++;
      if (one[at] || one[at + 1] || one[at + 2]) lit++;
    }
    if (error !== gl.NO_ERROR || differing || lit < W * H / 8)
      failures.push(`${name}-${state}:error=${error}:differ=${differing}:lit=${lit}`);
  };
  const ranges = (total, size, overlap = 0) => {
    const out = [];
    for (let start = 0; start + overlap < total; start += size)
      out.push([start, Math.min(size + overlap, total - start)]);
    return out;
  };
  const before = { ...globalThis.__tilefinchWebGLDiagnostics };

  /* Independent primitives, from a non-zero first vertex. */
  for (const [mode, unit, name] of [[gl.TRIANGLES, 3, "triangles"],
      [gl.LINES, 2, "lines"], [gl.POINTS, 1, "points"]]) {
    const count = 6000 - 6000 % unit, data = geometry(count + 3, false);
    bindGeometry(plain, data);
    for (const state of ["opaque", "additive", "depth"])
      compare(name, state, false, () => gl.drawArrays(mode, 3, count),
        () => { for (const [s, n] of ranges(count, 333 * unit))
          gl.drawArrays(mode, 3 + s, n); });
  }
  /* Strips: the page splits at other even (triangle) or any (line)
     boundaries, overlapping by the shared vertices. Culling proves the
     TRIANGLE_STRIP parts keep the strip's alternating winding. */
  {
    const data = geometry(5001, true);
    bindGeometry(plain, data);
    for (const state of ["opaque", "additive", "depth"])
      for (const cull of [false, true])
        compare(`strip${cull ? "-cull" : ""}`, state, cull,
          () => gl.drawArrays(gl.TRIANGLE_STRIP, 0, 5001),
          () => { for (const [s, n] of ranges(5001, 1000, 2))
            gl.drawArrays(gl.TRIANGLE_STRIP, s, n); });
    for (const state of ["opaque", "additive"])
      compare("line-strip", state, false,
        () => gl.drawArrays(gl.LINE_STRIP, 0, 5001),
        () => { for (const [s, n] of ranges(5001, 1100, 1))
          gl.drawArrays(gl.LINE_STRIP, s, n); });
  }
  /* Indexed draws split by index range over the same vertices, with a
     non-zero byte offset, for both index widths. */
  for (const [type, Array, vertices, name] of [
      [gl.UNSIGNED_SHORT, Uint16Array, 1200, "indexed16"],
      [gl.UNSIGNED_BYTE, Uint8Array, 240, "indexed8"]]) {
    const data = geometry(vertices, false);
    bindGeometry(plain, data);
    const count = 7500, list = new Array(count + 6);
    for (let at = 0; at < list.length; at++)
      list[at] = Math.floor(random() * vertices);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indices);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, list, gl.DYNAMIC_DRAW);
    const bytes = list.BYTES_PER_ELEMENT;
    for (const state of ["opaque", "additive", "depth"])
      compare(name, state, false,
        () => gl.drawElements(gl.TRIANGLES, count, type, 6 * bytes),
        () => { for (const [s, n] of ranges(count, 1500))
          gl.drawElements(gl.TRIANGLES, n, type, (6 + s) * bytes); });
    const strip = 5000;
    for (const state of ["opaque", "additive"])
      compare(`${name}-strip`, state, true,
        () => gl.drawElements(gl.TRIANGLE_STRIP, strip, type, 6 * bytes),
        () => { for (const [s, n] of ranges(strip, 1200, 2))
          gl.drawElements(gl.TRIANGLE_STRIP, n, type, (6 + s) * bytes); });
  }
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, null);

  /* Instancing: a 66-vertex mesh times 64 instances (4,224 vertices) splits
     by instance range; a 4,500-vertex mesh times two instances draws each
     instance in vertex parts. The reference moves the per-instance
     attribute pointers itself. Instance order is kept. */
  const instanceData = (count, stride) => {
    const values = new Float32Array(count * stride);
    for (let at = 0; at < count; at++) {
      if (stride === 16) {
        values[at * 16] = values[at * 16 + 5] = .35;
        values[at * 16 + 10] = 1; values[at * 16 + 15] = 1;
        values[at * 16 + 12] = random() * 1.4 - .7;
        values[at * 16 + 13] = random() * 1.4 - .7;
      } else {
        values[at * 4] = random() * 1.4 - .7;
        values[at * 4 + 1] = random() * 1.4 - .7;
        values[at * 4 + 2] = random() * .5; values[at * 4 + 3] = .35;
      }
    }
    return values;
  };
  const tintData = (count) => {
    const values = new Float32Array(count * 4);
    for (let at = 0; at < values.length; at++)
      values[at] = at % 4 === 3 ? 1 : .3 + random() * .7;
    return values;
  };
  const compactPointers = (base) => {
    gl.bindBuffer(gl.ARRAY_BUFFER, instances);
    gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 16, base * 16);
    gl.bindBuffer(gl.ARRAY_BUFFER, tints);
    gl.vertexAttribPointer(3, 4, gl.FLOAT, false, 0, base * 16);
  };
  const prepareCompact = (mesh, count) => {
    bindGeometry(compact, mesh);
    gl.bindBuffer(gl.ARRAY_BUFFER, instances);
    gl.bufferData(gl.ARRAY_BUFFER, instanceData(count, 4), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(2); ext.vertexAttribDivisorANGLE(2, 1);
    gl.bindBuffer(gl.ARRAY_BUFFER, tints);
    gl.bufferData(gl.ARRAY_BUFFER, tintData(count), gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(3); ext.vertexAttribDivisorANGLE(3, 1);
  };
  {
    const mesh = geometry(66, false, .6);
    prepareCompact(mesh, 64);
    for (const [mode, name] of [[gl.TRIANGLES, "instanced"],
        [gl.TRIANGLE_FAN, "instanced-fan"], [gl.LINE_LOOP, "instanced-loop"]])
      for (const state of ["opaque", "additive", "depth"])
        compare(name, state, false, () => {
          compactPointers(0);
          ext.drawArraysInstancedANGLE(mode, 0, 66, 64);
        }, () => {
          compactPointers(0); ext.drawArraysInstancedANGLE(mode, 0, 66, 20);
          compactPointers(20); ext.drawArraysInstancedANGLE(mode, 0, 66, 44);
        });
    const big = geometry(4500, false, .2);
    prepareCompact(big, 2);
    for (const state of ["opaque", "additive", "depth"])
      compare("instanced-big", state, false, () => {
        compactPointers(0);
        ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 4500, 2);
      }, () => {
        for (let instance = 0; instance < 2; instance++) {
          compactPointers(instance);
          for (const [s, n] of ranges(4500, 1500))
            ext.drawArraysInstancedANGLE(gl.TRIANGLES, s, n, 1);
        }
      });
    compactPointers(0);
    for (const location of [2, 3]) {
      ext.vertexAttribDivisorANGLE(location, 0);
      gl.disableVertexAttribArray(location);
    }
  }
  {
    const mesh = geometry(66, false, 1.2);
    bindGeometry(matrixProgram, mesh);
    gl.bindBuffer(gl.ARRAY_BUFFER, instances);
    gl.bufferData(gl.ARRAY_BUFFER, instanceData(64, 16), gl.DYNAMIC_DRAW);
    const pointers = (base) => {
      gl.bindBuffer(gl.ARRAY_BUFFER, instances);
      for (let column = 0; column < 4; column++)
        gl.vertexAttribPointer(2 + column, 4, gl.FLOAT, false, 64,
          base * 64 + column * 16);
    };
    for (let column = 0; column < 4; column++) {
      gl.enableVertexAttribArray(2 + column);
      ext.vertexAttribDivisorANGLE(2 + column, 1);
    }
    for (const state of ["opaque", "additive"])
      compare("instanced-matrix", state, false, () => {
        pointers(0); ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 66, 64);
      }, () => {
        pointers(0); ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 66, 40);
        pointers(40); ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 66, 24);
      });
    pointers(0);
    for (let column = 0; column < 4; column++) {
      ext.vertexAttribDivisorANGLE(2 + column, 0);
      gl.disableVertexAttribArray(2 + column);
    }
  }
  const after = globalThis.__tilefinchWebGLDiagnostics;
  if (!(after.splitDraws > before.splitDraws
      && after.splitParts >= after.splitDraws * 2))
    failures.push(`diagnostics:${after.splitDraws}/${after.splitParts}`);

  /* Refusals: a fan or loop over the limit cannot be split exactly; more
     than 64 instances is over the instance limit. One warning per program
     and mode, naming the limit. */
  warnings.length = 0;
  {
    const data = geometry(5000, true);
    bindGeometry(plain, data);
    const expectRefusal = (name, draw) => {
      draw();
      const error = gl.getError();
      if (error !== gl.OUT_OF_MEMORY) failures.push(`${name}:error=${error}`);
    };
    expectRefusal("fan", () => gl.drawArrays(gl.TRIANGLE_FAN, 0, 5000));
    expectRefusal("fan-again", () => gl.drawArrays(gl.TRIANGLE_FAN, 0, 4500));
    expectRefusal("loop", () => gl.drawArrays(gl.LINE_LOOP, 0, 5000));
    expectRefusal("instances", () =>
      ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 3, 65));
    expectRefusal("instances-again", () =>
      ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 3, 100));
    gl.drawArrays(gl.TRIANGLE_FAN, 0, 4096);
    if (gl.getError() !== gl.NO_ERROR) failures.push("fan-at-limit");
    const expected = [
      / a TRIANGLE_FAN draw of 5000 vertices is over the PSP limit of 4096 vertices per submission, and a fan cannot be split into parts exactly, so it is not drawn; draw TRIANGLES or fans of at most 4096 vertices$/,
      / a LINE_LOOP draw of 5000 vertices is over the PSP limit of 4096 vertices per submission, and a loop cannot be split into parts exactly, so it is not drawn; draw LINE_STRIP or loops of at most 4096 vertices$/,
      / a draw of 65 instances is over the PSP limit of 64 instances per draw, so it is not drawn; draw at most 64 instances at a time$/,
    ];
    if (warnings.length !== expected.length
        || !warnings.every((text, at) => /^WebGL program \d+:/.test(text)
          && expected[at].test(text)))
      failures.push("warnings=" + JSON.stringify(warnings).slice(0, 600));
  }
  delete globalThis.__tilefinchTestConsoleSink;
  if (gl.isContextLost()) failures.push("lost");
  globalThis.pocSummary = failures.length === 0 ? "WEBGL-BIG-DRAWS-PASS"
    : `WEBGL-BIG-DRAWS-FAIL:${failures.length}:${failures.slice(0, 8).join(",")}`;
})();
