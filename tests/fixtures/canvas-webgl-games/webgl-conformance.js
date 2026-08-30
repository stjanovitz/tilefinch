(() => {
  "use strict";
  const canvas = document.createElement("canvas");
  canvas.width = 32;
  canvas.height = 32;
  document.body.appendChild(canvas);
  const gl = canvas.getContext("webgl");
  const defaultDrawingBufferDiscard =
    gl.getContextAttributes().preserveDrawingBuffer === false;
  const bad = gl.createShader(gl.VERTEX_SHADER);
  gl.shaderSource(bad, "attribute vec2 p;");
  gl.compileShader(bad);
  const badRejected = !gl.getShaderParameter(bad, gl.COMPILE_STATUS)
    && gl.getShaderInfoLog(bad).length > 0;

  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    return shader;
  };
  const link = (vertexSource, fragmentSource) => {
    const linked = gl.createProgram();
    gl.attachShader(linked, compile(gl.VERTEX_SHADER, vertexSource));
    gl.attachShader(linked, compile(gl.FRAGMENT_SHADER, fragmentSource));
    gl.linkProgram(linked);
    return linked;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER,
    "attribute vec2 aPosition; void main(){gl_Position=vec4(aPosition,0.,1.);}"));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER,
    "uniform vec4 uColor; void main(){gl_FragColor=uColor;}"));
  gl.linkProgram(program);
  gl.useProgram(program);
  const buffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1, 1,-1, 0,1]),
    gl.DYNAMIC_DRAW);
  gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([-.9,-.9]));
  const position = gl.getAttribLocation(program, "aPosition");
  gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(position);
  gl.uniform4f(gl.getUniformLocation(program, "uColor"), .2, .8, .3, 1);
  gl.viewport(0, 0, 32, 32);
  gl.clearColor(0, 0, 0, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  gl.finish();
  const pixel = new Uint8Array(4);
  gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
  gl.bufferSubData(gl.ARRAY_BUFFER, 999, new Uint8Array([1]));
  const boundedError = gl.getError() === gl.INVALID_VALUE;
  const indexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2]),
    gl.STATIC_DRAW);
  gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 1);
  const alignedIndexError = gl.getError() === gl.INVALID_OPERATION;
  gl.drawElements(gl.TRIANGLES, 4, gl.UNSIGNED_SHORT, 0);
  const boundedIndexError = gl.getError() === gl.INVALID_OPERATION;

  const alignedBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, alignedBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, 32, gl.STATIC_DRAW);
  gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 3, 0);
  const alignedStrideError = gl.getError() === gl.INVALID_OPERATION;
  gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 2);
  const alignedOffsetError = gl.getError() === gl.INVALID_OPERATION;

  const signedBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, signedBuffer);
  gl.bufferData(gl.ARRAY_BUFFER,
    new Int8Array([-1,-1, 1,-1, 0,1]), gl.STATIC_DRAW);
  gl.vertexAttribPointer(position, 2, gl.BYTE, false, 0, 0);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  gl.finish();
  const signedRawPixel = new Uint8Array(4);
  gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, signedRawPixel);
  gl.bufferData(gl.ARRAY_BUFFER,
    new Int8Array([-128,-128, 127,-128, 0,127]), gl.STATIC_DRAW);
  gl.vertexAttribPointer(position, 2, gl.BYTE, true, 0, 0);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  gl.finish();
  const signedNormalizedPixel = new Uint8Array(4);
  gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE,
    signedNormalizedPixel);
  const signedByteWorks = signedRawPixel[1] > signedRawPixel[0]
    && signedNormalizedPixel[1] > signedNormalizedPixel[0];

  const geometryProgram = link(
    "attribute vec4 aPosition;void main(){gl_Position=aPosition;}",
    "void main(){gl_FragColor=vec4(1.,0.,0.,1.);}");
  gl.useProgram(geometryProgram);
  const geometryPosition = gl.getAttribLocation(geometryProgram, "aPosition");
  const geometryBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, geometryBuffer);
  gl.enableVertexAttribArray(geometryPosition);
  gl.vertexAttribPointer(geometryPosition, 4, gl.FLOAT, false, 0, 0);
  const geometryCases = [
    [NaN,-1,0,1, 1,-1,0,1, 0,1,0,1],
    [-1,-1,0,0, 1,-1,0,1, 0,1,0,1],
    [-1,-1,-2,1, 1,-1,0,1, 0,1,0,1],
    [100,-1,0,1, 102,-1,0,1, 101,1,0,1],
  ];
  let invalidGeometrySoft = true;
  for (const values of geometryCases) {
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(values), gl.STATIC_DRAW);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.finish();
    invalidGeometrySoft &&= !gl.isContextLost() && gl.getError() === gl.NO_ERROR;
  }

  const unpackCases = [
    [gl.RGBA, 4, [11,22,33,44], [55,66,77,88],
      [11,22,33,44,55,66,77,88]],
    [gl.RGB, 3, [11,22,33], [55,66,77],
      [11,22,33,255,55,66,77,255]],
    [gl.ALPHA, 1, [44], [88], [0,0,0,44,0,0,0,88]],
    [gl.LUMINANCE, 1, [11], [55], [11,11,11,255,55,55,55,255]],
    [gl.LUMINANCE_ALPHA, 2, [11,44], [55,88],
      [11,11,11,44,55,55,55,88]],
  ];
  let unpackRowsCorrect = true;
  for (const alignment of [1,2,4,8]) {
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, alignment);
    for (const [format, components, firstRow, secondRow, expected] of unpackCases) {
      const stride = Math.ceil(components / alignment) * alignment;
      const source = new Uint8Array(stride + components).fill(0xee);
      source.set(firstRow, 0); source.set(secondRow, stride);
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texImage2D(gl.TEXTURE_2D, 0, format, 1, 2, 0,
        format, gl.UNSIGNED_BYTE, source);
      unpackRowsCorrect &&= texture._pixels.length === expected.length
        && expected.every((value, index) => texture._pixels[index] === value);
      gl.deleteTexture(texture);
    }
  }
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 3);
  const invalidUnpackError = gl.getError() === gl.INVALID_VALUE
    && gl.getParameter(gl.UNPACK_ALIGNMENT) === 8;
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);

  const vertexUniforms = Array.from({length:16}, (_, i) =>
    `uniform vec4 v${i};`).join("");
  const fragmentUniforms = Array.from({length:4}, (_, i) =>
    `uniform float f${i};`).join("");
  const exactUniformProgram = link(
    `attribute vec2 aPosition;${vertexUniforms}`+
      "void main(){gl_Position=vec4(aPosition,0.,1.);}",
    `${fragmentUniforms}void main(){gl_FragColor=vec4(1.);}`);
  const vertexOverflowProgram = link(
    "attribute vec2 aPosition;uniform mat4 tooMany[5];"+
      "void main(){gl_Position=vec4(aPosition,0.,1.);}",
    "void main(){gl_FragColor=vec4(1.);}");
  const fragmentOverflowProgram = link(
    "attribute vec2 aPosition;void main(){gl_Position=vec4(aPosition,0.,1.);}",
    "uniform vec4 tooMany[5];void main(){gl_FragColor=vec4(1.);}");
  const excessDeclarations = Array.from({length:25}, (_, i) =>
    `uniform float q${i};`).join("");
  const declarationOverflowProgram = link(
    "attribute vec2 aPosition;void main(){gl_Position=vec4(aPosition,0.,1.);}",
    `${excessDeclarations}void main(){gl_FragColor=vec4(1.);}`);
  const uniformLimitsWork = gl.getProgramParameter(
      exactUniformProgram, gl.LINK_STATUS)
    && !gl.getProgramParameter(vertexOverflowProgram, gl.LINK_STATUS)
    && !gl.getProgramParameter(fragmentOverflowProgram, gl.LINK_STATUS)
    && !gl.getProgramParameter(declarationOverflowProgram, gl.LINK_STATUS);
  for (const testedProgram of [exactUniformProgram, vertexOverflowProgram,
    fragmentOverflowProgram, declarationOverflowProgram]) {
    for (const testedShader of gl.getAttachedShaders(testedProgram) || [])
      gl.deleteShader(testedShader);
  }

  const perspectiveProgram = link(
    "attribute vec3 aPosition;attribute vec2 aTexCoord;uniform mat4 uP;"+
      "varying vec2 vTexCoord;void main(){vTexCoord=aTexCoord;"+
      "gl_Position=uP*vec4(aPosition,1.);}",
    "uniform sampler2D uTexture;varying vec2 vTexCoord;"+
      "void main(){gl_FragColor=texture2D(uTexture,vTexCoord);}");
  gl.useProgram(perspectiveProgram);
  const perspectivePositions = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, perspectivePositions);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
    -.8,-.8,0, .8,-.8,0, 0,.2,-1]), gl.STATIC_DRAW);
  const perspectivePosition = gl.getAttribLocation(perspectiveProgram, "aPosition");
  gl.vertexAttribPointer(perspectivePosition, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(perspectivePosition);
  const perspectiveTexcoords = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, perspectiveTexcoords);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,0, 1,0, .5,1]),
    gl.STATIC_DRAW);
  const perspectiveTexcoord = gl.getAttribLocation(perspectiveProgram, "aTexCoord");
  gl.vertexAttribPointer(perspectiveTexcoord, 2, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(perspectiveTexcoord);
  const projection = new Float32Array([
    1,0,0,0, 0,1,0,0, 0,0,0,.75, 0,0,0,1]);
  const projectionLocation = gl.getUniformLocation(perspectiveProgram, "uP");
  const projectionBeforeWrongSetter = Array.from(
    gl.getUniform(perspectiveProgram, projectionLocation));
  gl.uniform1i(projectionLocation, 7);
  const wrongUniformSetterRejected = gl.getError() === gl.INVALID_OPERATION
    && Array.from(gl.getUniform(perspectiveProgram, projectionLocation)).join()
      === projectionBeforeWrongSetter.join()
    && !gl.isContextLost();
  gl.uniformMatrix4fv(projectionLocation, false, projection);
  const uniformSnapshot = gl.getUniform(perspectiveProgram, projectionLocation);
  uniformSnapshot[0] = 99;
  const uniformSnapshotWorks =
    gl.getUniform(perspectiveProgram, projectionLocation)[0] === 1;
  const perspectiveTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, perspectiveTexture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 4, 0, gl.RGBA,
    gl.UNSIGNED_BYTE, new Uint8Array([
      240,10,30,255, 20,220,40,255, 10,30,240,255, 220,200,20,255]));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  gl.finish();
  const perspectivePixel = new Uint8Array(4);
  gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, perspectivePixel);
  const perspectiveCorrect = perspectivePixel[0] > 180
    && perspectivePixel[1] < 80 && perspectivePixel[2] < 80;

  const partial = new Uint8Array(16).fill(0x7d);
  gl.readPixels(-1, -1, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, partial);
  const partialReadWorks = partial.slice(0, 12).every((value) => value === 0x7d)
    && partial[15] !== 0x7d;

  gl.disable(gl.SCISSOR_TEST);
  gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);
  gl.enable(gl.SCISSOR_TEST); gl.scissor(8, 6, 7, 5);
  gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);
  gl.scissor(9, 7, 0, 0);
  gl.clearColor(1, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);
  gl.finish();
  const scissorInside = new Uint8Array(4), scissorOutside = new Uint8Array(4);
  gl.readPixels(10, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, scissorInside);
  gl.readPixels(7, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, scissorOutside);
  const offsetAndEmptyScissorWork = scissorInside[1] > 240
    && scissorInside[0] < 10 && scissorOutside[0] < 10
    && scissorOutside[1] < 10;
  gl.disable(gl.SCISSOR_TEST);

  while (gl.getError() !== gl.NO_ERROR) {}
  gl.viewport(2147483647, -2147483648, 2147483647, 2147483647);
  const viewportMax = Array.from(gl.getParameter(gl.VIEWPORT));
  gl.viewport(0, 0, 2147483648, 1);
  const wrappedViewportRejected = gl.getError() === gl.INVALID_VALUE;
  gl.viewport(0, 0, NaN, Infinity);
  const convertedViewport = Array.from(gl.getParameter(gl.VIEWPORT));
  gl.scissor(2147483647, -2147483648, 2147483647, 2147483647);
  const scissorMax = Array.from(gl.getParameter(gl.SCISSOR_BOX));
  gl.scissor(0, 0, 2147483648, 1);
  const wrappedScissorRejected = gl.getError() === gl.INVALID_VALUE;
  gl.scissor(0, 0, NaN, Infinity);
  const convertedScissor = Array.from(gl.getParameter(gl.SCISSOR_BOX));
  const integerConversionWorks = viewportMax.join() ===
      "2147483647,-2147483648,480,272"
    && wrappedViewportRejected && convertedViewport.join() === "0,0,0,0"
    && scissorMax.join() ===
      "2147483647,-2147483648,2147483647,2147483647"
    && wrappedScissorRejected && convertedScissor.join() === "0,0,0,0";
  gl.viewport(0, 0, 32, 32); gl.scissor(0, 0, 32, 32);

  let nativeExceptionPropagated = false;
  try {
    const throwingSources = new Proxy([], {
      get(target, key) {
        if (key === "length") throw new Error("source getter");
        return Reflect.get(target, key);
      },
    });
    __tilefinchWebGLRender(canvas.__handle, 32, 32,
      gl._commandWireU32, throwingSources, gl._textureWireU32);
  } catch (error) {
    nativeExceptionPropagated = error?.message === "source getter";
  }

  const incompatibleWire = gl._commandWireU32.slice();
  incompatibleWire[1]++;
  const incompatibleWireRejected = __tilefinchWebGLRender(
    canvas.__handle, 32, 32, incompatibleWire, [], gl._textureWireU32) === 0;
  const trailingWire = new Uint32Array([
    gl._commandWireU32[0], gl._commandWireU32[1],
    gl._commandWireU32[2], 0, 0xdeadbeef,
  ]);
  const trailingWireRejected = __tilefinchWebGLRender(
    canvas.__handle, 32, 32, trailingWire, [], gl._textureWireU32) === 0;
  const wrongWireElementSizeRejected = __tilefinchWebGLRender(
    canvas.__handle, 32, 32, new Uint16Array(trailingWire.buffer), [],
    gl._textureWireU32) === 0;

  const depthProgram = link(
    "attribute vec3 aPosition;void main(){gl_Position=vec4(aPosition,1.);}",
    "uniform vec4 uColor;void main(){gl_FragColor=uColor;}");
  gl.useProgram(depthProgram);
  const depthPosition = gl.getAttribLocation(depthProgram, "aPosition");
  const depthColor = gl.getUniformLocation(depthProgram, "uColor");
  const depthBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, depthBuffer);
  gl.vertexAttribPointer(depthPosition, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(depthPosition);
  gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.clearDepth(1);
  const depthCase = (mode, near, far) => {
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(near), gl.DYNAMIC_DRAW);
    gl.uniform4f(depthColor, 0, 1, 0, 1);
    gl.drawArrays(mode, 0, near.length / 3);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(far), gl.DYNAMIC_DRAW);
    gl.uniform4f(depthColor, 1, 0, 0, 1);
    gl.drawArrays(mode, 0, far.length / 3);
    gl.finish();
    const samples = new Uint8Array(32 * 32 * 4);
    gl.readPixels(0, 0, 32, 32, gl.RGBA, gl.UNSIGNED_BYTE, samples);
    for (let at = 0; at < samples.length; at += 4)
      if (samples[at + 1] > 240 && samples[at] < 10) return true;
    return false;
  };
  const pointDepthWorks = depthCase(
    gl.POINTS, [0, 0, -.5], [0, 0, .5]);
  const lineDepthWorks = depthCase(
    gl.LINES, [-.8, 0, -.5, .8, 0, -.5],
    [-.8, 0, .5, .8, 0, .5]);
  const triangleDepthWorks = depthCase(
    gl.TRIANGLES, [-.5,-.5,-.5, .5,-.5,-.5, 0,.5,-.5],
    [-.5,-.5,.5, .5,-.5,.5, 0,.5,.5]);
  const primitiveDepthParity =
    pointDepthWorks && lineDepthWorks && triangleDepthWorks;
  gl.disable(gl.DEPTH_TEST);

  const forcedFlushesBefore = __tilefinchWebGLDiagnostics.forcedFlushes;
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.bindBuffer(gl.ARRAY_BUFFER, geometryBuffer);
  gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([-.8, -.8]));
  const forcedFlushCounted =
    __tilefinchWebGLDiagnostics.forcedFlushes === forcedFlushesBefore + 1
    && __tilefinchWebGLDiagnostics.maximumForcedFlushesPerFrame >= 1;

  while (gl.getError() !== gl.NO_ERROR) {}
  gl.enable(0x7fffffff);
  gl.viewport(0, 0, -1, 1);
  const pendingErrors = new Set([gl.getError(), gl.getError()]);
  const distinctErrorsRetained = pendingErrors.has(gl.INVALID_ENUM)
    && pendingErrors.has(gl.INVALID_VALUE) && gl.getError() === gl.NO_ERROR;
  gl.deleteBuffer(buffer);

  const sourceCanvas = document.createElement("canvas");
  sourceCanvas.width = 4; sourceCanvas.height = 4;
  document.body.appendChild(sourceCanvas);
  const sourceContext = sourceCanvas.getContext("webgl");
  const sourceCompile = (type, source) => {
    const shader = sourceContext.createShader(type);
    sourceContext.shaderSource(shader, source); sourceContext.compileShader(shader);
    return shader;
  };
  const sourceProgram = sourceContext.createProgram();
  sourceContext.attachShader(sourceProgram, sourceCompile(sourceContext.VERTEX_SHADER,
    "attribute vec2 aPosition;attribute vec4 aColor;attribute vec2 aTexCoord;"+
    "varying vec4 vColor;varying vec2 vTexCoord;void main(){vColor=aColor;"+
    "vTexCoord=aTexCoord;gl_Position=vec4(aPosition,0.,1.);}"));
  sourceContext.attachShader(sourceProgram, sourceCompile(sourceContext.FRAGMENT_SHADER,
    "uniform sampler2D uTexture;varying vec4 vColor;varying vec2 vTexCoord;"+
    "void main(){gl_FragColor=vColor*texture2D(uTexture,vTexCoord);}"));
  sourceContext.linkProgram(sourceProgram); sourceContext.useProgram(sourceProgram);
  sourceContext.uniform1i(sourceContext.getUniformLocation(sourceProgram, "uTexture"), 0);
  const sourceAttributes = ["aPosition", "aColor", "aTexCoord"].map((name) =>
    sourceContext.getAttribLocation(sourceProgram, name));
  const sourceBuffers = [], sourceTextures = [];
  for (let draw = 0; draw < 8; draw++) {
    const arrays = [
      new Float32Array([-1,-1, 1,-1, 0,1]),
      new Float32Array([1,1,1,1, 1,1,1,1, 1,1,1,1]),
      new Float32Array([0,0, 1,0, .5,1]),
    ];
    const drawBuffers = [];
    for (let attribute = 0; attribute < 3; attribute++) {
      const sourceBuffer = sourceContext.createBuffer();
      sourceContext.bindBuffer(sourceContext.ARRAY_BUFFER, sourceBuffer);
      sourceContext.bufferData(sourceContext.ARRAY_BUFFER, arrays[attribute],
        sourceContext.STATIC_DRAW);
      drawBuffers.push(sourceBuffer);
    }
    sourceBuffers.push(drawBuffers);
    const sourceTexture = sourceContext.createTexture();
    sourceContext.bindTexture(sourceContext.TEXTURE_2D, sourceTexture);
    sourceContext.texImage2D(sourceContext.TEXTURE_2D, 0, sourceContext.RGBA,
      1, 1, 0, sourceContext.RGBA, sourceContext.UNSIGNED_BYTE,
      new Uint8Array([draw + 1, 255, 255, 255]));
    sourceTextures.push(sourceTexture);
  }
  for (let draw = 0; draw < 8; draw++) {
    for (let attribute = 0; attribute < 3; attribute++) {
      sourceContext.bindBuffer(sourceContext.ARRAY_BUFFER,
        sourceBuffers[draw][attribute]);
      sourceContext.vertexAttribPointer(sourceAttributes[attribute],
        attribute === 1 ? 4 : 2, sourceContext.FLOAT, false, 0, 0);
      sourceContext.enableVertexAttribArray(sourceAttributes[attribute]);
    }
    sourceContext.bindTexture(sourceContext.TEXTURE_2D, sourceTextures[draw]);
    sourceContext.drawArrays(sourceContext.TRIANGLES, 0, 3);
  }
  sourceContext.finish();
  const exactSourceLimitWorks = !sourceContext.isContextLost()
    && sourceContext.getError() === sourceContext.NO_ERROR;
  const retainedSourceBuffer = sourceBuffers[0][0];
  const retainedSourceTexture = sourceTextures[0];
  sourceContext._lose("source-limit test complete");
  const lostResourcesReleased = retainedSourceBuffer._data.byteLength === 0
    && retainedSourceTexture._pixels === null
    && sourceContext._bufferBytes === 0 && sourceContext._textureBytes === 0
    && sourceContext.createBuffer() === null;

  const aaCanvas = document.createElement("canvas");
  aaCanvas.width = 8; aaCanvas.height = 8;
  document.body.appendChild(aaCanvas);
  const aa = aaCanvas.getContext("webgl", {antialias:true, depth:false});
  const aaCompile = (type, source) => {
    const shader = aa.createShader(type);
    aa.shaderSource(shader, source); aa.compileShader(shader); return shader;
  };
  const aaProgram = aa.createProgram();
  aa.attachShader(aaProgram, aaCompile(aa.VERTEX_SHADER,
    "attribute vec2 aPosition;void main(){gl_Position=vec4(aPosition,0.,1.);}"));
  aa.attachShader(aaProgram, aaCompile(aa.FRAGMENT_SHADER,
    "void main(){gl_FragColor=vec4(1.,0.,0.,1.);}"));
  aa.linkProgram(aaProgram); aa.useProgram(aaProgram);
  const aaBuffer = aa.createBuffer();
  aa.bindBuffer(aa.ARRAY_BUFFER, aaBuffer);
  aa.bufferData(aa.ARRAY_BUFFER,
    new Float32Array([-1,-1, 1,-1, -1,1]), aa.STATIC_DRAW);
  const aaPosition = aa.getAttribLocation(aaProgram, "aPosition");
  aa.vertexAttribPointer(aaPosition, 2, aa.FLOAT, false, 0, 0);
  aa.enableVertexAttribArray(aaPosition);
  aa.clearColor(0, 0, 0, 1); aa.clear(aa.COLOR_BUFFER_BIT);
  aa.drawArrays(aa.TRIANGLES, 0, 3); aa.finish();
  const aaPixels = new Uint8Array(8 * 8 * 4);
  aa.readPixels(0, 0, 8, 8, aa.RGBA, aa.UNSIGNED_BYTE, aaPixels);
  let partialCoverage = false;
  for (let at = 0; at < aaPixels.length; at += 4)
    partialCoverage ||= aaPixels[at] > 0 && aaPixels[at] < 255;
  const boundedAntialiasWorks = aa.getContextAttributes().antialias
    && partialCoverage && !aa.isContextLost();
  aa._lose("antialias test complete");

  const large = document.createElement("canvas");
  large.width = 640;
  large.height = 480;
  document.body.appendChild(large);
  const largeContext = large.getContext("webgl");
  const third = document.createElement("canvas").getContext("webgl");
  let lostEvents = 0;
  large.addEventListener("webglcontextlost", () => lostEvents++);
  largeContext._commands.push({
    kind:"draw", mode:0x7fffffff, first:0, count:0, index:null,
    position:null, color:null, texcoord:null, texture:null,
    matrix:new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]),
    uniformColor:new Float32Array([1,1,1,1]),
    blend:false, depth:false, cull:false, depthFunc:largeContext.LESS,
    blendSrc:largeContext.ONE, blendDst:largeContext.ZERO,
    cullFace:largeContext.BACK, frontFace:largeContext.CCW,
    viewport:[NaN,0,1,1], scissorEnabled:false, scissor:[0,0,1,1],
  });
  largeContext.finish();
  const terminalLoss = largeContext.isContextLost() && lostEvents === 1
    && largeContext.getError() === largeContext.CONTEXT_LOST_WEBGL
    && largeContext.getError() === largeContext.NO_ERROR
    && largeContext.createBuffer() === null;
  const replacementContext = document.createElement("canvas").getContext("webgl");
  const passed = defaultDrawingBufferDiscard && badRejected
    && gl.getProgramParameter(program, gl.LINK_STATUS)
    && pixel[1] > pixel[0] && pixel[3] === 255
    && boundedError && alignedIndexError && boundedIndexError
    && alignedStrideError && alignedOffsetError && signedByteWorks
    && invalidGeometrySoft && unpackRowsCorrect && invalidUnpackError
    && uniformLimitsWork && wrongUniformSetterRejected && uniformSnapshotWorks
    && perspectiveCorrect && partialReadWorks && offsetAndEmptyScissorWork
    && integerConversionWorks && nativeExceptionPropagated
    && incompatibleWireRejected && trailingWireRejected
    && wrongWireElementSizeRejected && primitiveDepthParity
    && forcedFlushCounted
    && exactSourceLimitWorks && lostResourcesReleased && boundedAntialiasWorks
    && distinctErrorsRetained
    && !gl.isBuffer(buffer)
    && largeContext.drawingBufferWidth === 362
    && largeContext.drawingBufferHeight === 272 && third === null
    && terminalLoss && replacementContext;
  globalThis.pocSummary = passed ? "WEBGL-CONFORMANCE-PASS" :
    `WEBGL-CONFORMANCE-FAIL:${defaultDrawingBufferDiscard}:${badRejected}:${pixel}:${boundedError}:` +
      `${alignedIndexError}:${boundedIndexError}:` +
      `${alignedStrideError}:${alignedOffsetError}:${signedByteWorks}:`+
      `${invalidGeometrySoft}:${unpackRowsCorrect}:${invalidUnpackError}:`+
      `${uniformLimitsWork}:${wrongUniformSetterRejected}:`+
      `${uniformSnapshotWorks}:${perspectivePixel}:`+
      `${partialReadWorks}:${offsetAndEmptyScissorWork}:`+
      `${integerConversionWorks}:${nativeExceptionPropagated}:`+
      `${incompatibleWireRejected}:${trailingWireRejected}:`+
      `${wrongWireElementSizeRejected}:${primitiveDepthParity}:`+
      `${pointDepthWorks}:${lineDepthWorks}:${triangleDepthWorks}:`+
      `${forcedFlushCounted}:`+
      `${exactSourceLimitWorks}:${lostResourcesReleased}:`+
      `${boundedAntialiasWorks}:`+
      `${distinctErrorsRetained}:${terminalLoss}:${!!replacementContext}:`+
      `${largeContext.drawingBufferWidth}x${largeContext.drawingBufferHeight}:` +
      `${third === null}`;
})();
