(() => {
  "use strict";

  const canvas = document.getElementById("game") || document.body.appendChild(
    Object.assign(document.createElement("canvas"), { id: "game" }),
  );
  canvas.width = 320;
  canvas.height = 180;
  const gl = canvas.getContext("webgl", { alpha: false, depth: true });
  if (!gl) {
    globalThis.pocSummary = "WEBGL-GAME-NO-CONTEXT";
    return;
  }

  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
      throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, `
    attribute vec3 aPosition;
    attribute vec4 aColor;
    uniform mat4 uProjection;
    uniform mat4 uModel;
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uProjection * uModel * vec4(aPosition, 1.0);
      vColor = aColor;
    }
  `));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vColor;
    void main(void) { gl_FragColor = vColor; }
  `));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);

  const positions = new Float32Array([
    -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,
    -1,-1,-1, -1, 1,-1,  1, 1,-1,  1,-1,-1,
    -1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1,-1,
    -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,
     1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1, 1,
    -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,
  ]);
  const colors = new Float32Array(24 * 4);
  const faceColors = [
    [1,.3,.2,1], [.25,.7,1,1], [.4,1,.45,1],
    [1,.75,.2,1], [.8,.35,1,1], [.3,1,.9,1],
  ];
  for (let face = 0; face < 6; face++) {
    for (let vertex = 0; vertex < 4; vertex++)
      colors.set(faceColors[face], (face * 4 + vertex) * 4);
  }
  const indices = new Uint16Array([
     0, 1, 2,  0, 2, 3,  4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11, 12,13,14, 12,14,15,
    16,17,18, 16,18,19, 20,21,22, 20,22,23,
  ]);
  const bindAttribute = (name, values, size) => {
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, values, gl.STATIC_DRAW);
    const location = gl.getAttribLocation(program, name);
    gl.vertexAttribPointer(location, size, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(location);
  };
  bindAttribute("aPosition", positions, 3);
  bindAttribute("aColor", colors, 4);
  const indexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);

  const projection = new Float32Array([
    .56,0,0,0, 0,1,0,0, 0,0,-1,-1, 0,0,-1.2,0,
  ]);
  const projectionLocation = gl.getUniformLocation(program, "uProjection");
  const modelLocation = gl.getUniformLocation(program, "uModel");
  gl.uniformMatrix4fv(projectionLocation, false, projection);
  gl.enable(gl.DEPTH_TEST);
  gl.depthFunc(gl.LEQUAL);
  gl.enable(gl.CULL_FACE);
  gl.clearColor(.02, .03, .08, 1);
  globalThis.__webglGameFrames = 0;

  function frame() {
    const angle = ++globalThis.__webglGameFrames * .09;
    const c = Math.cos(angle), s = Math.sin(angle);
    const model = new Float32Array([
       c, s*.45,-s,0,
       0, .9, .4,0,
       s,-c*.45, c,0,
       0, 0,-4,1,
    ]);
    gl.uniformMatrix4fv(modelLocation, false, model);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.drawElements(gl.TRIANGLES, indices.length, gl.UNSIGNED_SHORT, 0);
    if (globalThis.__webglGameFrames < 10) {
      requestAnimationFrame(frame);
      return;
    }
    gl.finish();
    const pixel = new Uint8Array(4);
    gl.readPixels(gl.drawingBufferWidth >> 1, gl.drawingBufferHeight >> 1,
      1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
    const passed = gl.getError() === gl.NO_ERROR && pixel[3] === 255
      && (pixel[0] || pixel[1] || pixel[2]);
    globalThis.pocSummary = passed ? "WEBGL-GAME-PASS" :
      `WEBGL-GAME-FAIL:${pixel.join(",")}:${gl.getError()}`;
    document.documentElement.dataset.gameResult = passed ? "pass" : "fail";
  }
  frame();
})();
