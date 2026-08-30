(() => {
  "use strict";
  const canvas = document.getElementById("game");
  canvas.width = 8; canvas.height = 8;
  const gl = canvas.getContext("webgl", { alpha: false });
  const ext = gl && gl.getExtension("ANGLE_instanced_arrays");
  if (!ext) { globalThis.pocSummary = "WEBGL-INSTANCING-NO-EXT"; return; }
  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source); gl.compileShader(shader); return shader;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, `
    attribute vec3 aPosition;
    attribute vec4 aColor;
    attribute mat4 aInstance;
    attribute vec4 aInstanceTint;
    uniform mat4 uProjection;
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uProjection * aInstance * vec4(aPosition, 1.0);
      vColor = aColor * aInstanceTint;
    }
  `));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vColor;
    void main(void) { gl_FragColor = vColor; }
  `));
  gl.linkProgram(program); gl.useProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    globalThis.pocSummary = `WEBGL-INSTANCING-LINK:${gl.getProgramInfoLog(program)}`;
    return;
  }
  const position = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, position);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    -.22, -.34, 0, .22, -.34, 0, 0, .34, 0,
  ]), gl.STATIC_DRAW);
  const positionLocation = gl.getAttribLocation(program, "aPosition");
  gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(positionLocation);
  const baseColors = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, baseColors);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  ]), gl.STATIC_DRAW);
  const colorLocation = gl.getAttribLocation(program, "aColor");
  gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(colorLocation);

  const matrices = new Float32Array(32);
  for (let instance = 0; instance < 2; instance++) {
    const at = instance * 16;
    matrices[at] = matrices[at + 5] = matrices[at + 10] = matrices[at + 15] = 1;
    matrices[at + 12] = instance ? .52 : -.52;
  }
  const instances = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, instances);
  gl.bufferData(gl.ARRAY_BUFFER, matrices, gl.STATIC_DRAW);
  const matrixLocation = gl.getAttribLocation(program, "aInstance");
  for (let column = 0; column < 4; column++) {
    const location = matrixLocation + column;
    gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64, column * 16);
    gl.enableVertexAttribArray(location);
    ext.vertexAttribDivisorANGLE(location, 1);
  }
  const tints = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, tints);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    1, .1, .1, 1, .1, 1, .1, 1,
  ]), gl.STATIC_DRAW);
  const tintLocation = gl.getAttribLocation(program, "aInstanceTint");
  gl.vertexAttribPointer(tintLocation, 4, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(tintLocation);
  ext.vertexAttribDivisorANGLE(tintLocation, 1);
  const identity = new Float32Array(16);
  identity[0] = identity[5] = identity[10] = identity[15] = 1;
  gl.uniformMatrix4fv(gl.getUniformLocation(program, "uProjection"), false, identity);
  gl.viewport(0, 0, 8, 8); gl.clearColor(0, 0, 0, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 3, 2);
  gl.finish();
  const drawError = gl.getError();
  const left = new Uint8Array(4), right = new Uint8Array(4);
  gl.readPixels(2, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, left);
  gl.readPixels(6, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, right);
  ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 65, 64);
  const expandedWorkBounded = gl.getError() === gl.OUT_OF_MEMORY;
  ext.vertexAttribDivisorANGLE(matrixLocation, 2);
  const bounded = gl.getError() === gl.INVALID_OPERATION;
  const painted = drawError === gl.NO_ERROR
    && left[0] > 100 && left[1] < 80
    && right[1] > 100 && right[0] < 80
    && left[3] === 255 && right[3] === 255;

  const compactProgram = gl.createProgram();
  gl.attachShader(compactProgram, compile(gl.VERTEX_SHADER, `
    attribute vec3 aPosition;
    attribute vec4 aColor;
    attribute vec4 aInstanceTransform;
    attribute vec4 aInstanceTint;
    uniform mat4 uProjection;
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uProjection * vec4(
        aPosition * aInstanceTransform.w + aInstanceTransform.xyz, 1.0);
      vColor = aColor * aInstanceTint;
    }
  `));
  gl.attachShader(compactProgram, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vColor;
    void main(void) { gl_FragColor = vColor; }
  `));
  gl.bindAttribLocation(compactProgram, 0, "aPosition");
  gl.bindAttribLocation(compactProgram, 1, "aColor");
  gl.bindAttribLocation(compactProgram, 2, "aInstanceTransform");
  gl.bindAttribLocation(compactProgram, 3, "aInstanceTint");
  gl.linkProgram(compactProgram); gl.useProgram(compactProgram);
  const compactLinked = gl.getProgramParameter(compactProgram, gl.LINK_STATUS);
  const transforms = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, transforms);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    -.52, 0, 0, 1, .52, 0, 0, 1,
  ]), gl.STATIC_DRAW);
  gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 16, 0);
  gl.enableVertexAttribArray(2); ext.vertexAttribDivisorANGLE(2, 1);
  gl.bindBuffer(gl.ARRAY_BUFFER, tints);
  gl.vertexAttribPointer(3, 4, gl.FLOAT, false, 16, 0);
  gl.enableVertexAttribArray(3); ext.vertexAttribDivisorANGLE(3, 1);
  gl.bindBuffer(gl.ARRAY_BUFFER, position);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, baseColors);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
  gl.uniformMatrix4fv(
    gl.getUniformLocation(compactProgram, "uProjection"), false, identity);
  gl.clear(gl.COLOR_BUFFER_BIT);
  ext.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 3, 2);
  gl.finish();
  const compactError = gl.getError();
  const compactLeft = new Uint8Array(4), compactRight = new Uint8Array(4);
  gl.readPixels(2, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, compactLeft);
  gl.readPixels(6, 3, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, compactRight);
  const compactPainted = compactLinked && compactError === gl.NO_ERROR
    && compactLeft[0] > 100 && compactLeft[1] < 80
    && compactRight[1] > 100 && compactRight[0] < 80;
  const vaos = gl.getExtension("OES_vertex_array_object");
  const vao = vaos?.createVertexArrayOES();
  const unboundIsFalse = vaos && !vaos.isVertexArrayOES(vao);
  vaos?.bindVertexArrayOES(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, position);
  gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(0);
  const boundMatches = gl.getParameter(vaos.VERTEX_ARRAY_BINDING_OES) === vao
    && vaos.isVertexArrayOES(vao);
  vaos?.bindVertexArrayOES(null);
  gl.disableVertexAttribArray(0);
  const defaultDisabled = !gl.getVertexAttrib(
    0, gl.VERTEX_ATTRIB_ARRAY_ENABLED);
  vaos?.bindVertexArrayOES(vao);
  const restored = gl.getVertexAttrib(0, gl.VERTEX_ATTRIB_ARRAY_ENABLED)
    && gl.getVertexAttrib(0, gl.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) === position;
  vaos?.deleteVertexArrayOES(vao);
  const vaoPass = unboundIsFalse && boundMatches && defaultDisabled
    && restored && !vaos.isVertexArrayOES(vao)
    && gl.getParameter(vaos.VERTEX_ARRAY_BINDING_OES) === null;
  globalThis.pocSummary = painted && compactPainted
      && bounded && expandedWorkBounded && vaoPass
    ? "WEBGL-INSTANCING-PASS"
    : `WEBGL-INSTANCING-FAIL:${left.join(",")}:${right.join(",")}:${compactLeft.join(",")}:${compactRight.join(",")}:${bounded}:vao=${vaoPass}:err=${drawError}/${compactError}:lost=${gl.isContextLost()}`;
})();
