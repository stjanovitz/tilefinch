(() => {
  "use strict";
  /* Watertight triangle rasterization. Adjacent triangles that share an edge
     must cover every pixel exactly once: additive blending counts the
     coverage, so a gap reads 0 and a double hit reads 32. Meshes are
     jittered grids, fans and strips at many sub-pixel offsets and angles
     (including vertices on exact pixel centres and corners), plus a floor
     that crosses the near plane, where the clipper must give both triangles
     of a shared edge the same new vertex. With antialiasing on, an opaque
     mesh must not let the clear colour show through along its shared edges
     (the dotted seams Treadline's floor and backdrop showed). */
  const W = 64, H = 48;
  let seed = 0x2545f491;
  const random = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed / 4294967296;
  };
  const failures = [];
  const context = (antialias) => {
    const canvas = document.createElement("canvas");
    canvas.width = W; canvas.height = H;
    document.body.appendChild(canvas);
    const gl = canvas.getContext("webgl", {antialias, depth: true});
    const compile = (type, source) => {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source); gl.compileShader(shader);
      return shader;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER,
      "attribute vec3 aPosition;uniform mat4 uP;" +
      "void main(){gl_Position=uP*vec4(aPosition,1.);}"));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER,
      "uniform vec4 uColor;void main(){gl_FragColor=uColor;}"));
    gl.linkProgram(program); gl.useProgram(program);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    const position = gl.getAttribLocation(program, "aPosition");
    gl.vertexAttribPointer(position, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(position);
    gl.viewport(0, 0, W, H);
    return {
      gl, color: gl.getUniformLocation(program, "uColor"),
      matrix: gl.getUniformLocation(program, "uP"),
    };
  };
  const identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
  // Column-major perspective: 90 degree vertical field, near .5, far 100.
  const near = .5, far = 100, aspect = W / H;
  const perspective = [
    1 / aspect,0,0,0, 0,1,0,0,
    0,0,(far + near) / (near - far),-1,
    0,0,2 * far * near / (near - far),0];
  const draw = (target, mode, vertices, matrix, opaque) => {
    const gl = target.gl;
    gl.clearColor(0, 0, 0, 1); gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (opaque) {
      gl.disable(gl.BLEND);
      gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS);
      gl.uniform4f(target.color, 0, 200 / 255, 0, 1);
    } else {
      gl.disable(gl.DEPTH_TEST);
      gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE);
      gl.uniform4f(target.color, 16 / 255, 0, 0, 1);
    }
    gl.uniformMatrix4fv(target.matrix, false, new Float32Array(matrix));
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.DYNAMIC_DRAW);
    gl.drawArrays(mode, 0, vertices.length / 3);
    gl.finish();
    const pixels = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    return pixels;
  };
  // Every pixel must be hit exactly once (additive) or show only the mesh
  // colour (opaque). rows(y) limits the check to rows the mesh must cover.
  const check = (name, pixels, opaque, covered = () => true) => {
    let gaps = 0, doubles = 0, leaks = 0;
    for (let y = 0; y < H; y++) {
      if (!covered(y)) continue;
      for (let x = 0; x < W; x++) {
        const at = (y * W + x) * 4;
        if (opaque) {
          if (pixels[at] !== 0 || pixels[at + 2] !== 0
              || pixels[at + 1] < 196) leaks++;
        } else if (pixels[at] < 16) gaps++;
        else if (pixels[at] > 16) doubles++;
      }
    }
    if (gaps || doubles || leaks)
      failures.push(`${name}:gaps=${gaps}:doubles=${doubles}:leaks=${leaks}`);
  };
  // Sub-pixel quantum for a case: free floats, exact pixel centres/corners
  // (half pixels) or 1/16 pixel steps.
  const quantize = (value, size, step) => {
    if (step === 0) return value;
    const pixel = (value + 1) * .5 * size;
    return Math.round(pixel / step) * step / size * 2 - 1;
  };
  const grid = (columns, rows, step, plane) => {
    const points = [];
    for (let row = 0; row <= rows; row++) {
      for (let column = 0; column <= columns; column++) {
        let x = -1.3 + 2.6 * column / columns;
        let y = -1.3 + 2.6 * row / rows;
        if (row > 0 && row < rows && column > 0 && column < columns) {
          x += (random() - .5) * .7 * 2.6 / columns;
          y += (random() - .5) * .7 * 2.6 / rows;
        }
        x = quantize(x, W, step); y = quantize(y, H, step);
        points.push([x, y, plane ? .3 * x - .2 * y : 0]);
      }
    }
    const vertices = [];
    const at = (row, column) => points[row * (columns + 1) + column];
    for (let row = 0; row < rows; row++) {
      for (let column = 0; column < columns; column++) {
        const a = at(row, column), b = at(row, column + 1);
        const c = at(row + 1, column + 1), d = at(row + 1, column);
        const quad = random() < .5 ? [a, b, c, a, c, d] : [a, b, d, b, c, d];
        for (const point of quad) vertices.push(...point);
      }
    }
    return {vertices, points, at};
  };
  const strips = (mesh, columns, rows) => {
    const runs = [];
    for (let row = 0; row < rows; row++) {
      const vertices = [];
      for (let column = 0; column <= columns; column++)
        vertices.push(...mesh.at(row, column), ...mesh.at(row + 1, column));
      runs.push(vertices);
    }
    return runs;
  };
  const fan = (step) => {
    const cx = quantize((random() - .5) * 1.4, W, step);
    const cy = quantize((random() - .5) * 1.4, H, step);
    const spokes = 7 + Math.floor(random() * 40);
    const start = random() * Math.PI * 2;
    const vertices = [cx, cy, 0];
    for (let spoke = 0; spoke <= spokes; spoke++) {
      const angle = start + Math.PI * 2 * (spoke % spokes) / spokes;
      vertices.push(Math.cos(angle) * 3, Math.sin(angle) * 3, 0);
    }
    return vertices;
  };
  // A floor at y = -1 from behind the camera (z = 6) to z = -60, wide enough
  // that its sides never reach the screen, jittered in its own plane. Rows of
  // triangles cross the near plane at z = -.5 and must be clipped, not
  // dropped; its far edge is one horizontal screen line.
  const floor = () => {
    const columns = 9, rows = 11, points = [];
    for (let row = 0; row <= rows; row++) {
      for (let column = 0; column <= columns; column++) {
        let x = -400 + 800 * column / columns;
        let z = 6 - 66 * Math.pow(row / rows, 1.6);
        if (row > 0 && row < rows && column > 0 && column < columns) {
          x += (random() - .5) * 30;
          z += (random() - .5) * (row < 3 ? .6 : 2);
        }
        points.push([x, -1, z]);
      }
    }
    const vertices = [];
    const at = (row, column) => points[row * (columns + 1) + column];
    for (let row = 0; row < rows; row++) {
      for (let column = 0; column < columns; column++) {
        const a = at(row, column), b = at(row, column + 1);
        const c = at(row + 1, column + 1), d = at(row + 1, column);
        const quad = random() < .5 ? [a, b, c, a, c, d] : [a, b, d, b, c, d];
        for (const point of quad) vertices.push(...point);
      }
    }
    // readPixels row 0 is the bottom. The far edge (z = -60) projects to
    // NDC y = -1/60; rows more than a pixel below it must be covered.
    const edge = (1 - 1 / 60) * .5 * H;
    return {vertices, covered: (y) => y + .5 < edge - 1};
  };
  const additive = context(false), smooth = context(true);
  const steps = [0, .5, 1 / 16];
  for (let round = 0; round < 12; round++) {
    const step = steps[round % steps.length];
    const columns = 3 + (round % 5), rows = 2 + (round % 4);
    const mesh = grid(columns, rows, step, round % 2 === 1);
    check(`grid${round}`, draw(additive, additive.gl.TRIANGLES,
      mesh.vertices, identity, false), false);
    check(`aa-grid${round}`, draw(smooth, smooth.gl.TRIANGLES,
      mesh.vertices, identity, true), true);
    {
      // One strip per band of the same mesh; together they tile the canvas.
      const gl = additive.gl;
      gl.clearColor(0, 0, 0, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.disable(gl.DEPTH_TEST);
      gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE);
      gl.uniform4f(additive.color, 16 / 255, 0, 0, 1);
      for (const run of strips(mesh, columns, rows)) {
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(run), gl.DYNAMIC_DRAW);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, run.length / 3);
      }
      gl.finish();
      const pixels = new Uint8Array(W * H * 4);
      gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
      check(`strip${round}`, pixels, false);
    }
    const fanVertices = fan(step);
    check(`fan${round}`, draw(additive, additive.gl.TRIANGLE_FAN,
      fanVertices, identity, false), false);
    check(`aa-fan${round}`, draw(smooth, smooth.gl.TRIANGLE_FAN,
      fanVertices, identity, true), true);
  }
  for (let round = 0; round < 4; round++) {
    const ground = floor();
    check(`near${round}`, draw(additive, additive.gl.TRIANGLES,
      ground.vertices, perspective, false), false, ground.covered);
    check(`aa-near${round}`, draw(smooth, smooth.gl.TRIANGLES,
      ground.vertices, perspective, true), true, ground.covered);
  }
  // Name every failing case (bounded summary), then the first few counts.
  globalThis.pocSummary = failures.length === 0 ? "WEBGL-SEAMS-PASS"
    : `WEBGL-SEAMS-FAIL:failed=${failures.length}:`
      + failures.map((failure) => failure.split(":")[0]).join(",")
      + `;${failures.slice(0, 4).join(",")}`;
})();
