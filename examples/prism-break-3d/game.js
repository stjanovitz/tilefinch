(() => {
  "use strict";

  const canvas = document.getElementById("game");
  const gl = canvas && canvas.getContext("webgl", {
    alpha: false, depth: true, antialias: true,
  });
  const ui = {
    panel: document.getElementById("panel"),
    heading: document.querySelector("#panel h1"),
    message: document.getElementById("message"),
    play: document.getElementById("play"),
    score: document.getElementById("score"),
    level: document.getElementById("level"),
    lives: document.getElementById("lives"),
    power: document.getElementById("power"),
    toast: document.getElementById("toast"),
    controls: document.getElementById("controls"),
  };
  if (!gl || !ui.panel || !ui.play) {
    if (ui.message) ui.message.textContent = "WebGL is unavailable.";
    globalThis.pocSummary = "PRISM-BREAK-NO-WEBGL";
    return;
  }
  const instancing = gl.getExtension("ANGLE_instanced_arrays");

  const MAX_VERTICES = 3072;
  const MAX_INDICES = 4608;
  const positions = new Float32Array(MAX_VERTICES * 3);
  const colors = new Float32Array(MAX_VERTICES * 4);
  const indices = new Uint16Array(MAX_INDICES);
  let vertexCount = 0, indexCount = 0;
  const HUD_GLYPH_LIMIT = 48;
  const HUD_RECT_LIMIT = HUD_GLYPH_LIMIT * 10;
  const hudVertices = new Float32Array(HUD_RECT_LIMIT * 4 * 6);
  const hudIndices = new Uint16Array(HUD_RECT_LIMIT * 6);
  let hudVertexCount = 0, hudIndexCount = 0, hudCharacterCount = 0;
  let hudMeshDirty = true;

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
  gl.bindAttribLocation(program, 0, "aPosition");
  gl.bindAttribLocation(program, 1, "aColor");
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);

  const positionBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, positions.byteLength, gl.DYNAMIC_DRAW);
  const positionLocation = gl.getAttribLocation(program, "aPosition");
  gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(positionLocation);
  const colorBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, colors.byteLength, gl.DYNAMIC_DRAW);
  const colorLocation = gl.getAttribLocation(program, "aColor");
  gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(colorLocation);
  const indexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices.byteLength, gl.DYNAMIC_DRAW);
  /* The non-instanced fallback allocates these after extension admission.
     Tilefinch's retained brick stream needs two compact instance buffers
     instead of three maximum-sized CPU meshes. */
  let staticPositionBuffer = null, staticColorBuffer = null;
  let staticIndexBuffer = null, staticVertexArray = null;
  /* Scenery never changes after construction. Keep it in an exact-sized
     immutable stream so a brick update does not invalidate and rebuild the
     native bridge's decoded background geometry. */
  const backgroundPositionBuffer = gl.createBuffer();
  const backgroundColorBuffer = gl.createBuffer();
  const backgroundIndexBuffer = gl.createBuffer();
  const vertexArrays = gl.getExtension("OES_vertex_array_object");
  function createMeshVertexArray(position, color, index) {
    if (!vertexArrays) return null;
    const array = vertexArrays.createVertexArrayOES();
    vertexArrays.bindVertexArrayOES(array);
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(positionLocation);
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(colorLocation);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    return array;
  }
  const dynamicVertexArray = createMeshVertexArray(
    positionBuffer, colorBuffer, indexBuffer);
  const backgroundVertexArray = createMeshVertexArray(
    backgroundPositionBuffer, backgroundColorBuffer, backgroundIndexBuffer);
  if (vertexArrays) vertexArrays.bindVertexArrayOES(null);

  const projection = new Float32Array(16);
  const f = 1 / Math.tan(52 * Math.PI / 360), aspect = 320 / 180,
    near = .5, far = 30;
  projection[0] = f / aspect;
  projection[5] = f;
  projection[10] = (far + near) / (near - far);
  projection[11] = -1;
  projection[14] = 2 * far * near / (near - far);
  gl.uniformMatrix4fv(
    gl.getUniformLocation(program, "uProjection"), false, projection);
  const modelLocation = gl.getUniformLocation(program, "uModel");
  const model = new Float32Array(16);
  gl.enable(gl.DEPTH_TEST);
  gl.depthFunc(gl.LEQUAL);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  gl.clearColor(.008, .016, .045, 1);
  gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);

  /* Gameplay text is a fixed 3x5 rectangle mesh rendered in screen space. Updating
     this retained mesh is much cheaper on the PSP than mutating text nodes,
     which would force style/layout and rebuild the post-canvas overlay. */
  const HUD_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-";
  const HUD_PATTERNS = {
    " ": "000000000000000", "0": "111101101101111",
    "1": "010110010010111", "2": "111001111100111",
    "3": "111001111001111", "4": "101101111001001",
    "5": "111100111001111", "6": "111100111101111",
    "7": "111001001001001", "8": "111101111101111",
    "9": "111101111001111", "A": "010101111101101",
    "B": "110101110101110", "C": "111100100100111",
    "D": "110101101101110", "E": "111100110100111",
    "F": "111100110100100", "G": "111100101101111",
    "H": "101101111101101", "I": "111010010010111",
    "J": "001001001101111", "K": "101101110101101",
    "L": "100100100100111", "M": "101111111101101",
    "N": "101111111111101", "O": "111101101101111",
    "P": "111101111100100", "Q": "111101101111001",
    "R": "111101111110101", "S": "111100111001111",
    "T": "111010010010010", "U": "101101101101111",
    "V": "101101101101010", "W": "101101111111101",
    "X": "101101010101101", "Y": "101101010010010",
    "Z": "111001010100111", "-": "000000111000000",
  };
  const HUD_SCORE_TINT = [.87, .97, 1, 1];
  const HUD_POWER_TINT = [.51, 1, .86, 1];
  const HUD_TOAST_TINT = [1, .97, .66, 1];
  const hudProgram = gl.createProgram();
  gl.attachShader(hudProgram, compile(gl.VERTEX_SHADER, `
    attribute vec2 aPosition;
    attribute vec4 aTint;
    varying lowp vec4 vTint;
    void main(void) {
      gl_Position = vec4(aPosition, 0.0, 1.0);
      vTint = aTint;
    }
  `));
  gl.attachShader(hudProgram, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vTint;
    void main(void) { gl_FragColor = vTint; }
  `));
  gl.bindAttribLocation(hudProgram, 0, "aPosition");
  gl.bindAttribLocation(hudProgram, 1, "aTint");
  gl.linkProgram(hudProgram);
  if (!gl.getProgramParameter(hudProgram, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(hudProgram));
  const hudVertexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, hudVertices.byteLength, gl.DYNAMIC_DRAW);
  const hudIndexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, hudIndices.byteLength,
    gl.DYNAMIC_DRAW);
  let hudVertexArray = null;
  if (vertexArrays) {
    hudVertexArray = vertexArrays.createVertexArrayOES();
    vertexArrays.bindVertexArrayOES(hudVertexArray);
  }
  gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
  gl.enableVertexAttribArray(0);
  gl.enableVertexAttribArray(1);
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
  if (vertexArrays) vertexArrays.bindVertexArrayOES(null);

  const BOX_VERTICES = new Int8Array([
    -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,
     1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,
    -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,
    -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,
     1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,
    -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,
  ]);
  const BOX_INDICES = new Uint8Array([
     0, 1, 2,  0, 2, 3,  4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11, 12,13,14, 12,14,15,
    16,17,18, 16,18,19, 20,21,22, 20,22,23,
  ]);
  const OCTA_VERTICES = new Int8Array([
     1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1,
  ]);
  const OCTA_INDICES = new Uint8Array([
    2,0,4, 2,4,1, 2,1,5, 2,5,0,
    3,4,0, 3,1,4, 3,5,1, 3,0,5,
  ]);
  const FACE_SHADE = new Float32Array([1, .56, 1.18, .46, .82, .68]);
  const rotationX = -.17, rotationY = .09;
  const cosX = Math.cos(rotationX), sinX = Math.sin(rotationX);
  const cosY = Math.cos(rotationY), sinY = Math.sin(rotationY);

  /* Dynamic effects retain one octahedron and submit only bounded transform
     records when instancing is available.  The fallback below remains the
     ordinary WebGL 1 mesh builder, so the example still runs elsewhere. */
  const MAX_OCTA_INSTANCES = 128, MAX_INSTANCES_PER_DRAW = 64;
  const octaTransforms = new Float32Array(MAX_OCTA_INSTANCES * 4);
  const octaTints = new Float32Array(MAX_OCTA_INSTANCES * 4);
  let octaInstanceCount = 0;
  let collectInstancedOcta = false;
  let instanceProgram = null, instancePosition = null, instanceShade = null;
  let instanceIndex = null, instanceTransform = null, instanceTint = null;
  let instanceVertexArray = null;
  let instancePositionLocation = -1, instanceShadeLocation = -1;
  let instanceTransformLocation = -1, instanceModelLocation = null;
  let instanceTintLocation = -1;
  let instancePointerStart = -1;

  const BRICK_COLUMNS = 8, BRICK_ROWS = 6;
  const MAX_BRICK_INSTANCES = BRICK_COLUMNS * BRICK_ROWS;
  const MAX_BOX_INSTANCES = 16;
  const boxInstanceMatrices = new Float32Array(MAX_BOX_INSTANCES * 16);
  const boxInstanceTints = new Float32Array(MAX_BOX_INSTANCES * 4);
  let boxInstanceCount = 0, collectInstancedBoxes = false;
  let boxInstanceProgram = null, boxInstancePosition = null;
  let boxInstanceShade = null, boxInstanceIndex = null;
  let boxInstanceMatrix = null, boxInstanceTint = null;
  let boxInstanceVertexArray = null, boxInstanceModelLocation = null;
  let boxInstanceMatrixLocation = -1, boxInstanceTintLocation = -1;
  const brickInstanceMatrices = new Float32Array(
    MAX_BRICK_INSTANCES * 16);
  const brickInstanceTints = new Float32Array(MAX_BRICK_INSTANCES * 4);
  let brickInstanceCount = 0, brickInstanceMatrix = null;
  let brickInstanceTint = null, brickInstanceVertexArray = null;
  let brickMatrixDirtyFirst = MAX_BRICK_INSTANCES, brickMatrixDirtyLast = -1;
  let brickTintDirtyFirst = MAX_BRICK_INSTANCES, brickTintDirtyLast = -1;
  let brickInstanceUploads = 0, brickInstanceFloatsUploaded = 0;

  function resetOctaInstances() {
    octaInstanceCount = 0;
  }

  function addOctaInstance(x, y, z, radius, color, alpha) {
    if (octaInstanceCount >= MAX_OCTA_INSTANCES) return false;
    let alphaStep = (alpha * 7 + .5) | 0;
    if (alphaStep < 0) alphaStep = 0;
    else if (alphaStep > 7) alphaStep = 7;
    if (alphaStep === 0) return true;
    const instance = octaInstanceCount++;
    const tintAt = instance * 4;
    octaTints[tintAt] = color[0];
    octaTints[tintAt + 1] = color[1];
    octaTints[tintAt + 2] = color[2];
    octaTints[tintAt + 3] = alphaStep / 7;
    const at = instance * 4;
    octaTransforms[at] = x;
    octaTransforms[at + 1] = y;
    octaTransforms[at + 2] = z;
    octaTransforms[at + 3] = radius;
    return true;
  }

  function writeVertex(x, y, z, color, shade, alpha = 1,
                       brightness = 1) {
    if (vertexCount >= MAX_VERTICES) return false;
    const rotatedX = x * cosY + z * sinY;
    const rotatedZ = -x * sinY + z * cosY;
    const rotatedY = y * cosX - rotatedZ * sinX;
    const viewZ = y * sinX + rotatedZ * cosX - 7.7;
    const p = vertexCount * 3, c = vertexCount * 4;
    positions[p] = rotatedX;
    positions[p + 1] = rotatedY;
    positions[p + 2] = viewZ;
    colors[c] = Math.min(1, color[0] * shade * brightness);
    colors[c + 1] = Math.min(1, color[1] * shade * brightness);
    colors[c + 2] = Math.min(1, color[2] * shade * brightness);
    colors[c + 3] = alpha;
    vertexCount++;
    return true;
  }

  function addBox(x, y, z, width, height, depth, color, alpha = 1,
                  brightness = 1) {
    if (collectInstancedBoxes && boxInstanceProgram) {
      if (boxInstanceCount >= MAX_BOX_INSTANCES) return false;
      const instance = boxInstanceCount++;
      const matrixAt = instance * 16;
      /* Off-diagonal entries never change from the typed array's zero-filled
         initialization; overwrite only the seven authored components. */
      boxInstanceMatrices[matrixAt] = width * .5;
      boxInstanceMatrices[matrixAt + 5] = height * .5;
      boxInstanceMatrices[matrixAt + 10] = depth * .5;
      boxInstanceMatrices[matrixAt + 12] = x;
      boxInstanceMatrices[matrixAt + 13] = y;
      boxInstanceMatrices[matrixAt + 14] = z;
      boxInstanceMatrices[matrixAt + 15] = 1;
      const tintAt = instance * 4;
      boxInstanceTints[tintAt] = color[0] * brightness;
      boxInstanceTints[tintAt + 1] = color[1] * brightness;
      boxInstanceTints[tintAt + 2] = color[2] * brightness;
      boxInstanceTints[tintAt + 3] = alpha;
      return true;
    }
    if (vertexCount + 24 > MAX_VERTICES || indexCount + 36 > MAX_INDICES)
      return false;
    const base = vertexCount;
    for (let at = 0; at < 24; at++) {
      const source = at * 3;
      writeVertex(
        x + BOX_VERTICES[source] * width * .5,
        y + BOX_VERTICES[source + 1] * height * .5,
        z + BOX_VERTICES[source + 2] * depth * .5,
        color, FACE_SHADE[(at / 4) | 0], alpha, brightness);
    }
    for (let at = 0; at < 36; at++) indices[indexCount++] = base + BOX_INDICES[at];
    return true;
  }

  function addOctahedron(x, y, z, radius, color, alpha = 1) {
    if (collectInstancedOcta)
      return addOctaInstance(x, y, z, radius, color, alpha);
    if (vertexCount + 6 > MAX_VERTICES || indexCount + 24 > MAX_INDICES)
      return false;
    const base = vertexCount;
    for (let at = 0; at < 6; at++) {
      const source = at * 3;
      writeVertex(
        x + OCTA_VERTICES[source] * radius,
        y + OCTA_VERTICES[source + 1] * radius,
        z + OCTA_VERTICES[source + 2] * radius,
        color, at < 2 ? .82 : at < 4 ? 1.08 : 1, alpha);
    }
    for (let at = 0; at < 24; at++)
      indices[indexCount++] = base + OCTA_INDICES[at];
    return true;
  }

  function initializeInstancing() {
    if (!instancing) return;
    instanceProgram = gl.createProgram();
    gl.attachShader(instanceProgram, compile(gl.VERTEX_SHADER, `
      attribute vec3 aPosition;
      attribute vec4 aColor;
      attribute vec4 aInstanceTransform;
      attribute vec4 aInstanceTint;
      uniform mat4 uProjection;
      uniform mat4 uModel;
      uniform mat4 uView;
      varying lowp vec4 vColor;
      void main(void) {
        gl_Position = uProjection * uModel * uView
          * vec4(aPosition * aInstanceTransform.w
              + aInstanceTransform.xyz, 1.0);
        vColor = aColor * aInstanceTint;
      }
    `));
    gl.attachShader(instanceProgram, compile(gl.FRAGMENT_SHADER, `
      varying lowp vec4 vColor;
      void main(void) { gl_FragColor = vColor; }
    `));
    gl.bindAttribLocation(instanceProgram, 0, "aPosition");
    gl.bindAttribLocation(instanceProgram, 1, "aColor");
    gl.bindAttribLocation(instanceProgram, 2, "aInstanceTransform");
    gl.bindAttribLocation(instanceProgram, 3, "aInstanceTint");
    gl.linkProgram(instanceProgram);
    if (!gl.getProgramParameter(instanceProgram, gl.LINK_STATUS)) {
      instanceProgram = null; return;
    }
    instancePositionLocation = gl.getAttribLocation(
      instanceProgram, "aPosition");
    instanceShadeLocation = gl.getAttribLocation(instanceProgram, "aColor");
    instanceTransformLocation = gl.getAttribLocation(
      instanceProgram, "aInstanceTransform");
    instanceTintLocation = gl.getAttribLocation(
      instanceProgram, "aInstanceTint");
    instanceModelLocation = gl.getUniformLocation(instanceProgram, "uModel");

    instancePosition = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instancePosition);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(OCTA_VERTICES),
      gl.STATIC_DRAW);
    instanceShade = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceShade);
    const shades = new Float32Array(6 * 4);
    for (let at = 0; at < 6; at++) {
      const shade = at < 2 ? .82 : at < 4 ? 1.08 : 1;
      shades.set([shade, shade, shade, 1], at * 4);
    }
    gl.bufferData(gl.ARRAY_BUFFER, shades, gl.STATIC_DRAW);
    instanceIndex = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, instanceIndex);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,
      new Uint16Array(OCTA_INDICES), gl.STATIC_DRAW);
    instanceTransform = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTransform);
    gl.bufferData(gl.ARRAY_BUFFER, octaTransforms.byteLength, gl.DYNAMIC_DRAW);
    instanceTint = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTint);
    gl.bufferData(gl.ARRAY_BUFFER, octaTints.byteLength, gl.DYNAMIC_DRAW);
    if (vertexArrays) {
      instanceVertexArray = vertexArrays.createVertexArrayOES();
      vertexArrays.bindVertexArrayOES(instanceVertexArray);
      gl.bindBuffer(gl.ARRAY_BUFFER, instancePosition);
      gl.vertexAttribPointer(
        instancePositionLocation, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(instancePositionLocation);
      gl.bindBuffer(gl.ARRAY_BUFFER, instanceShade);
      gl.vertexAttribPointer(
        instanceShadeLocation, 4, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(instanceShadeLocation);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, instanceIndex);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTransform);
    gl.vertexAttribPointer(
      instanceTransformLocation, 4, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(instanceTransformLocation);
    instancing.vertexAttribDivisorANGLE(instanceTransformLocation, 1);
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTint);
    gl.vertexAttribPointer(instanceTintLocation, 4, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(instanceTintLocation);
    instancing.vertexAttribDivisorANGLE(instanceTintLocation, 1);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);
    instancePointerStart = 0;
    gl.useProgram(instanceProgram);
    gl.uniformMatrix4fv(
      gl.getUniformLocation(instanceProgram, "uProjection"), false, projection);
    gl.uniformMatrix4fv(
      gl.getUniformLocation(instanceProgram, "uView"), false,
      new Float32Array([
        cosY, sinX * sinY, -cosX * sinY, 0,
        0, cosX, sinX, 0,
        sinY, -sinX * cosY, cosX * cosY, 0,
        0, 0, -7.7, 1,
      ]));
    gl.useProgram(program);
  }
  initializeInstancing();

  function initializeBoxInstancing() {
    if (!instancing) return;
    boxInstanceProgram = gl.createProgram();
    gl.attachShader(boxInstanceProgram, compile(gl.VERTEX_SHADER, `
      attribute vec3 aPosition;
      attribute vec4 aColor;
      attribute mat4 aInstanceModel;
      attribute vec4 aInstanceTint;
      uniform mat4 uProjection;
      uniform mat4 uModel;
      uniform mat4 uView;
      varying lowp vec4 vColor;
      void main(void) {
        gl_Position = uProjection * uModel * uView * aInstanceModel
          * vec4(aPosition, 1.0);
        vColor = aColor * aInstanceTint;
      }
    `));
    gl.attachShader(boxInstanceProgram, compile(gl.FRAGMENT_SHADER, `
      varying lowp vec4 vColor;
      void main(void) { gl_FragColor = vColor; }
    `));
    gl.bindAttribLocation(boxInstanceProgram, 0, "aPosition");
    gl.bindAttribLocation(boxInstanceProgram, 1, "aColor");
    gl.bindAttribLocation(boxInstanceProgram, 2, "aInstanceModel");
    gl.bindAttribLocation(boxInstanceProgram, 6, "aInstanceTint");
    gl.linkProgram(boxInstanceProgram);
    if (!gl.getProgramParameter(boxInstanceProgram, gl.LINK_STATUS)) {
      boxInstanceProgram = null;
      return;
    }
    boxInstanceMatrixLocation = gl.getAttribLocation(
      boxInstanceProgram, "aInstanceModel");
    boxInstanceTintLocation = gl.getAttribLocation(
      boxInstanceProgram, "aInstanceTint");
    boxInstanceModelLocation = gl.getUniformLocation(
      boxInstanceProgram, "uModel");
    boxInstancePosition = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(BOX_VERTICES),
      gl.STATIC_DRAW);
    boxInstanceShade = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
    const shades = new Float32Array(24 * 4);
    for (let vertex = 0; vertex < 24; vertex++) {
      const shade = FACE_SHADE[(vertex / 4) | 0];
      shades.set([shade, shade, shade, 1], vertex * 4);
    }
    gl.bufferData(gl.ARRAY_BUFFER, shades, gl.STATIC_DRAW);
    boxInstanceIndex = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(BOX_INDICES),
      gl.STATIC_DRAW);
    boxInstanceMatrix = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
    gl.bufferData(gl.ARRAY_BUFFER, boxInstanceMatrices.byteLength,
      gl.DYNAMIC_DRAW);
    boxInstanceTint = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
    gl.bufferData(gl.ARRAY_BUFFER, boxInstanceTints.byteLength,
      gl.DYNAMIC_DRAW);
    if (vertexArrays) {
      boxInstanceVertexArray = vertexArrays.createVertexArrayOES();
      vertexArrays.bindVertexArrayOES(boxInstanceVertexArray);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(0);
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(1);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
    for (let column = 0; column < 4; column++) {
      const location = boxInstanceMatrixLocation + column;
      gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64, column * 16);
      gl.enableVertexAttribArray(location);
      instancing.vertexAttribDivisorANGLE(location, 1);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
    gl.vertexAttribPointer(boxInstanceTintLocation, 4,
      gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(boxInstanceTintLocation);
    instancing.vertexAttribDivisorANGLE(boxInstanceTintLocation, 1);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);

    /* Bricks use the same immutable unit box and shader, but retain their
       own compact transforms and tints.  A collision can therefore patch
       one 16-byte tint (or one 64-byte transform when a brick disappears)
       without rebuilding the complete field. */
    brickInstanceMatrix = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceMatrix);
    gl.bufferData(gl.ARRAY_BUFFER, brickInstanceMatrices.byteLength,
      gl.DYNAMIC_DRAW);
    brickInstanceTint = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceTint);
    gl.bufferData(gl.ARRAY_BUFFER, brickInstanceTints.byteLength,
      gl.DYNAMIC_DRAW);
    if (vertexArrays) {
      brickInstanceVertexArray = vertexArrays.createVertexArrayOES();
      vertexArrays.bindVertexArrayOES(brickInstanceVertexArray);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(0);
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
    gl.enableVertexAttribArray(1);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceMatrix);
    for (let column = 0; column < 4; column++) {
      const location = boxInstanceMatrixLocation + column;
      gl.vertexAttribPointer(location, 4, gl.FLOAT, false, 64, column * 16);
      gl.enableVertexAttribArray(location);
      instancing.vertexAttribDivisorANGLE(location, 1);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceTint);
    gl.vertexAttribPointer(boxInstanceTintLocation, 4,
      gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(boxInstanceTintLocation);
    instancing.vertexAttribDivisorANGLE(boxInstanceTintLocation, 1);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);

    gl.useProgram(boxInstanceProgram);
    gl.uniformMatrix4fv(gl.getUniformLocation(
      boxInstanceProgram, "uProjection"), false, projection);
    gl.uniformMatrix4fv(gl.getUniformLocation(
      boxInstanceProgram, "uView"), false, new Float32Array([
        cosY, sinX * sinY, -cosX * sinY, 0,
        0, cosX, sinX, 0,
        sinY, -sinX * cosY, cosX * cosY, 0,
        0, 0, -7.7, 1,
      ]));
    gl.useProgram(program);
  }
  initializeBoxInstancing();

  if (!boxInstanceProgram) {
    staticPositionBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, positions.byteLength, gl.DYNAMIC_DRAW);
    staticColorBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, colors.byteLength, gl.DYNAMIC_DRAW);
    staticIndexBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, staticIndexBuffer);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices.byteLength,
      gl.DYNAMIC_DRAW);
    staticVertexArray = createMeshVertexArray(
      staticPositionBuffer, staticColorBuffer, staticIndexBuffer);
  }

  const LEVELS = [
    ["11111111", "11111111", "11111111", "11111111", "11111111"],
    [".122221.", "121..121", "21211212", "12122121", ".211112."],
    ["33333333", "3.2222.3", "32.11.23", "321..123", "22222222"],
    ["4.4.4.4.", ".333333.", "22222222", "2.1111.2", "11111111", ".1.1.1.1"],
  ];
  const PALETTE = [
    [0,0,0], [.18,.88,1], [.78,.3,1], [1,.36,.25], [1,.84,.24],
  ];
  const POWER_COLORS = {
    wide: [.2,1,.64], multi: [1,.45,.98], slow: [.35,.72,1],
    shield: [1,.9,.25], laser: [1,.25,.2],
  };
  const POWER_NAMES = {
    wide: "WIDE PADDLE", multi: "MULTIBALL", slow: "SLOW FIELD",
    shield: "PRISM SHIELD", laser: "LASER CORE",
  };
  const powerCycle = ["wide", "multi", "slow", "shield", "laser"];
  const brickGrid = new Array(BRICK_COLUMNS * BRICK_ROWS).fill(null);
  const SCENE_COLORS = {
    star: [.28,.55,1], floor: [.05,.16,.32], rail: [.12,.52,.88],
    paddle: [.16,.72,1], paddleWide: [.2,1,.65], paddleLaser: [1,.28,.22],
    shield: [1,.88,.22], trail: [.24,.85,1], ball: [1,.97,.7],
    shot: [1,.25,.18],
  };
  const stars = [];
  let randomState = 0x51f15e1d;
  function random() {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return (randomState >>> 0) / 4294967296;
  }
  for (let at = 0; at < 24; at++) {
    stars.push({
      x: random() * 10 - 5, y: random() * 6 - 3,
      z: -.7 - random() * .8, size: .012 + random() * .022,
    });
  }

  const state = {
    mode: "title", score: 0, lives: 3, level: 0, combo: 0,
    bricks: [], balls: [], powerups: [], shots: [],
    paddle: { x: 0, width: 1.55 },
    shield: 0, laser: 0, wideUntil: 0, slowUntil: 0,
    time: 0, transition: 0,
    destroyed: 0, frames: 0, meshDrops: 0, running: true,
  };
  const input = {
    left: false, right: false, launch: false, pause: false,
    axis: 0, lastPrimary: false, lastPause: false,
    source: "keyboard", gamepadConnected: false,
  };
  let lastTimestamp = 0, toastUntil = 0;
  let hudCache = "", hudScore = "", hudLevel = "", hudLives = "",
    hudPower = "", hudToast = "", hudDirty = true,
    hudSecond = -1, hudTick = -1,
    hudScoreTick = -1;
  let backgroundDirty = true, backgroundVertexCount = 0;
  let backgroundIndexCount = 0;
  let staticDirty = true, staticVertexCount = 0, staticIndexCount = 0;
  let qualificationHeavy = false, qualificationProfile = false;
  let qualificationBridgeProfile = false;
  let qualificationBurstAt = 0;
  const qualificationTiming = {
    samples: 0, update: 0, build: 0, upload: 0, commands: 0, hud: 0,
    model: 0, clear: 0, staticDraw: 0, dynamicDraw: 0, instanceDraw: 0,
    frame: 0, maxUpdate: 0, maxBuild: 0, maxUpload: 0, maxCommands: 0,
    maxHud: 0, maxFrame: 0,
  };
  /* Particle bursts are deliberately frequent in the qualification scene.
     Keep the fixed-capacity effect in compact parallel arrays: this avoids
     both allocation/collection stalls and dozens of interpreted object-
     property walks per update/build on the PSP. Direction vectors are also
     computed once rather than paying trigonometry in the burst frame. */
  const PARTICLE_LIMIT = 48;
  const particleX = new Float32Array(PARTICLE_LIMIT);
  const particleY = new Float32Array(PARTICLE_LIMIT);
  const particleVx = new Float32Array(PARTICLE_LIMIT);
  const particleVy = new Float32Array(PARTICLE_LIMIT);
  const particleLife = new Float32Array(PARTICLE_LIMIT);
  const particleColors = new Array(PARTICLE_LIMIT);
  let particleCount = 0;
  const particleDirections = new Float32Array(32 * 2);
  for (let at = 0; at < 32; at++) {
    const angle = at * Math.PI * 2 / 32;
    particleDirections[at * 2] = Math.cos(angle);
    particleDirections[at * 2 + 1] = Math.sin(angle);
  }
  const BALL_LIMIT = 3;
  const ballPool = new Array(BALL_LIMIT);
  for (let at = 0; at < BALL_LIMIT; at++) {
    ballPool[at] = {
      x: 0, y: 0, vx: 0, vy: 0, radius: .12,
      stuck: true, alive: false,
      trailX: new Float32Array(7), trailY: new Float32Array(7),
      trailHead: 0, trailCount: 0,
    };
  }
  const POWERUP_LIMIT = 4;
  const powerupPool = new Array(POWERUP_LIMIT);
  for (let at = 0; at < POWERUP_LIMIT; at++)
    powerupPool[at] = { x: 0, y: 0, type: "wide", phase: 0 };
  const SHOT_LIMIT = 8;
  const shotPool = new Array(SHOT_LIMIT);
  for (let at = 0; at < SHOT_LIMIT; at++) shotPool[at] = { x: 0, y: 0 };

  function resetParticles() {
    particleCount = 0;
  }

  function resetBall(ball, x = state.paddle.x, y = -2.45,
                     vx = 1.8, vy = 3.2) {
    ball.x = x; ball.y = y; ball.vx = vx; ball.vy = vy;
    ball.radius = .12; ball.stuck = true; ball.alive = true;
    ball.trailHead = ball.trailCount = 0;
    return ball;
  }

  function makeBall(x = state.paddle.x, y = -2.45, vx = 1.8, vy = 3.2) {
    for (let at = 0; at < BALL_LIMIT; at++) {
      if (state.balls.indexOf(ballPool[at]) < 0)
        return resetBall(ballPool[at], x, y, vx, vy);
    }
    return null;
  }

  function claimPowerup(x, y, type) {
    for (let at = 0; at < POWERUP_LIMIT; at++) {
      const power = powerupPool[at];
      if (state.powerups.indexOf(power) >= 0) continue;
      power.x = x; power.y = y; power.type = type; power.phase = 0;
      return power;
    }
    return null;
  }

  function claimShot(x, y) {
    for (let at = 0; at < SHOT_LIMIT; at++) {
      const shot = shotPool[at];
      if (state.shots.indexOf(shot) >= 0) continue;
      shot.x = x; shot.y = y;
      return shot;
    }
    return null;
  }

  function markBrickTintDirty(brick) {
    staticDirty = true;
    if (!boxInstanceProgram || !brick) return;
    brickTintDirtyFirst = Math.min(brickTintDirtyFirst, brick.instance);
    brickTintDirtyLast = Math.max(brickTintDirtyLast, brick.instance);
  }

  function markBrickMatrixDirty(brick) {
    staticDirty = true;
    if (!boxInstanceProgram || !brick) return;
    brickMatrixDirtyFirst = Math.min(brickMatrixDirtyFirst, brick.instance);
    brickMatrixDirtyLast = Math.max(brickMatrixDirtyLast, brick.instance);
  }

  function markAllBricksDirty() {
    staticDirty = true;
    brickInstanceCount = state.bricks.length;
    if (!boxInstanceProgram || !brickInstanceCount) return;
    brickMatrixDirtyFirst = brickTintDirtyFirst = 0;
    brickMatrixDirtyLast = brickTintDirtyLast = brickInstanceCount - 1;
  }

  function buildLevel(index) {
    state.bricks.length = 0;
    brickGrid.fill(null);
    state.powerups.length = 0;
    state.shots.length = 0;
    const rows = LEVELS[index % LEVELS.length];
    for (let row = 0; row < rows.length; row++) {
      for (let column = 0; column < rows[row].length; column++) {
        const hp = Number(rows[row][column]);
        if (!hp) continue;
        const brick = {
          x: (column - 3.5) * 1.08,
          y: 2.42 - row * .52,
          width: .98, height: .36,
          hp, maximum: hp, alive: true,
          color: PALETTE[Math.min(4, hp)],
          instance: state.bricks.length,
        };
        state.bricks.push(brick);
        brickGrid[row * BRICK_COLUMNS + column] = brick;
      }
    }
    state.balls.length = 0;
    const ball = makeBall();
    if (ball) state.balls.push(ball);
    state.paddle.x = 0;
    state.paddle.width = state.time < state.wideUntil ? 2.35 : 1.55;
    state.combo = 0;
    state.transition = 0;
    markAllBricksDirty();
    hudDirty = true;
    showToast(`LEVEL ${index + 1}`, 1.2);
  }

  function resetGame() {
    state.score = 0;
    state.lives = 3;
    state.level = 0;
    state.destroyed = 0;
    state.shield = 0;
    state.laser = 0;
    state.wideUntil = 0;
    state.slowUntil = 0;
    resetParticles();
    randomState = 0x51f15e1d;
    buildLevel(0);
    hudDirty = true;
  }

  function showPanel(title, message, label) {
    ui.heading.textContent = title;
    ui.message.textContent = message;
    ui.play.textContent = label;
    ui.panel.hidden = false;
  }

  function hidePanel() { ui.panel.hidden = true; }

  function showToast(text, seconds = .8) {
    hudToast = String(text).toUpperCase().slice(0, 20);
    hudMeshDirty = true;
    hudDirty = true;
    toastUntil = state.time + seconds;
  }

  function setMode(mode) {
    state.mode = mode;
    if (mode === "playing") hidePanel();
    else if (mode === "paused")
      showPanel("PAUSED", "The prism field is frozen.", "Resume");
    else if (mode === "game-over")
      showPanel("GAME OVER", `Final score ${state.score}.`, "Play again");
    else if (mode === "victory")
      showPanel("PRISM MASTER", `All levels clear - ${state.score} points.`, "Play again");
  }

  class SoundBank {
    constructor() {
      this.context = null;
      this.voices = [];
      this.next = 0;
    }
    start() {
      if (this.context || typeof AudioContext !== "function") return;
      try {
        this.context = new AudioContext();
        this.context.resume().then(() => {
          for (let at = 0; at < 2; at++) {
            const gain = this.context.createGain();
            const oscillator = this.context.createOscillator();
            gain.gain.value = 0;
            oscillator.type = at ? "triangle" : "square";
            oscillator.connect(gain).connect(this.context.destination);
            oscillator.start();
            this.voices.push({ oscillator, gain, stopAt: 0 });
          }
        }).catch(() => {});
      } catch (_) { this.context = null; }
    }
    play(frequency, duration = .045, volume = .12) {
      if (!this.voices.length) return;
      const voice = this.voices[this.next++ % this.voices.length];
      voice.oscillator.frequency.value = frequency;
      voice.gain.gain.value = volume;
      voice.stopAt = state.time + duration;
    }
    tick(now) {
      for (let at = 0; at < this.voices.length; at++) {
        const voice = this.voices[at];
        if (voice.stopAt && now >= voice.stopAt) {
          voice.gain.gain.value = 0;
          voice.stopAt = 0;
        }
      }
    }
  }
  const sounds = new SoundBank();

  function requestPresentation() {
    sounds.start();
    const shell = document.getElementById("game-shell");
    if (shell && navigator.tilefinch?.requestPageControls) {
      navigator.tilefinch.requestPageControls(shell).catch(() => {});
    } else if (shell && shell.requestFullscreen) {
      shell.requestFullscreen().catch(() => {});
    }
    canvas.focus();
  }

  function startOrResume() {
    requestPresentation();
    if (state.mode === "paused") {
      setMode("playing");
      return;
    }
    resetGame();
    setMode("playing");
  }
  ui.play.addEventListener("click", startOrResume);

  function togglePause() {
    if (state.mode === "playing") setMode("paused");
    else if (state.mode === "paused") setMode("playing");
  }

  function spawnParticles(x, y, color, count = 6) {
    for (let at = 0; at < count && particleCount < PARTICLE_LIMIT; at++) {
      const particle = particleCount++;
      const direction = ((random() * 32) | 0) * 2;
      const speed = .7 + random() * 1.8;
      particleX[particle] = x;
      particleY[particle] = y;
      particleVx[particle] = particleDirections[direction] * speed;
      particleVy[particle] = particleDirections[direction + 1] * speed;
      particleLife[particle] = .35 + random() * .5;
      particleColors[particle] = color;
    }
  }

  function maybeDropPower(brick) {
    if (state.powerups.length >= 4 || state.destroyed % 7 !== 0) return;
    const type = powerCycle[((state.destroyed / 7) | 0) % powerCycle.length];
    const power = claimPowerup(brick.x, brick.y, type);
    if (power) state.powerups.push(power);
  }

  function destroyBrick(brick) {
    brick.alive = false;
    markBrickMatrixDirty(brick);
    state.destroyed++;
    state.combo = Math.min(12, state.combo + 1);
    state.score += 100 * state.combo * brick.maximum;
    hudDirty = true;
    const color = PALETTE[Math.min(4, brick.maximum)];
    spawnParticles(brick.x, brick.y, color, 7);
    maybeDropPower(brick);
    sounds.play(240 + state.combo * 18, .035, .1);
    if (!state.bricks.some((candidate) => candidate.alive)) {
      state.mode = "level-clear";
      state.transition = 1.25;
      showToast("FIELD CLEAR", 1.1);
      sounds.play(660, .18, .16);
    }
  }

  function hitBrick(brick) {
    if (!brick.alive) return;
    brick.hp--;
    if (brick.hp <= 0) destroyBrick(brick);
    else {
      markBrickTintDirty(brick);
      state.score += 35;
      hudDirty = true;
      spawnParticles(brick.x, brick.y, PALETTE[Math.min(4, brick.maximum)], 3);
      sounds.play(170 + brick.hp * 35, .025, .07);
    }
  }

  function applyPower(type) {
    const duration = state.time + 9;
    if (type === "wide") {
      state.wideUntil = duration;
      state.paddle.width = 2.35;
    } else if (type === "slow") {
      state.slowUntil = duration;
      for (const ball of state.balls) { ball.vx *= .78; ball.vy *= .78; }
    } else if (type === "shield") state.shield = 1;
    else if (type === "laser") state.laser = 12;
    else if (type === "multi" && state.balls.length < 3) {
      const source = state.balls.find((ball) => ball.alive)
        || state.balls[0] || ballPool[0];
      while (state.balls.length < 3) {
        const direction = state.balls.length & 1 ? -1 : 1;
        const ball = makeBall(
          source.x, source.y,
          direction * Math.max(1.8, Math.abs(source.vx)),
          Math.max(2.5, Math.abs(source.vy)));
        if (!ball) break;
        ball.stuck = false;
        state.balls.push(ball);
      }
    }
    showToast(POWER_NAMES[type], 1);
    hudDirty = true;
    sounds.play(520, .12, .13);
  }

  function launchOrFire() {
    const stuck = state.balls.find((ball) => ball.alive && ball.stuck);
    if (stuck) {
      stuck.stuck = false;
      const slowScale = state.time < state.slowUntil ? .78 : 1;
      const speed = (3.55 + state.level * .18) * slowScale;
      stuck.vx = input.axis * 1.4 || 1.25;
      stuck.vy = Math.sqrt(Math.max(2, speed * speed - stuck.vx * stuck.vx));
      sounds.play(330, .06, .1);
      return;
    }
    if (state.laser > 0 && state.shots.length <= 6) {
      const left = claimShot(
        state.paddle.x - state.paddle.width * .34, -2.22);
      if (left) state.shots.push(left);
      const right = claimShot(
        state.paddle.x + state.paddle.width * .34, -2.22);
      if (right) state.shots.push(right);
      state.laser--;
      hudDirty = true;
      sounds.play(760, .04, .08);
    }
  }

  function connectedGamepad() {
    if (typeof navigator.getGamepads !== "function") return null;
    const pads = navigator.getGamepads();
    if (!pads) return null;
    for (let at = 0; at < pads.length; at++) {
      if (pads[at] && pads[at].connected) return pads[at];
    }
    return null;
  }

  function setInputSource(source) {
    if (input.source === source) return;
    input.source = source;
    /* Edge state belongs to the previous device. A first press on the newly
       active device must never inherit a held button from another source. */
    input.lastPrimary = false;
    input.lastPause = false;
  }

  function updateControlHint() {
    const pad = connectedGamepad();
    input.gamepadConnected = !!pad;
    if (!ui.controls) return;
    if (pad) {
      ui.controls.textContent =
        "Controller: stick/D-pad moves | A/X acts | Y/Triangle pauses";
    } else if (navigator.platform === "PSP") {
      ui.controls.textContent =
        "Press Play for game controls | START+SELECT exits | Nub/D-pad moves | X acts";
    } else {
      ui.controls.textContent =
        "Keyboard: Left/Right or A/D moves | Space/Enter acts | P/Esc pauses";
    }
  }

  function pollInput() {
    const keyboardAxis = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    const pad = connectedGamepad();
    if (!!pad !== input.gamepadConnected) updateControlHint();
    let gamepadAxis = 0, gamepadPrimary = false, gamepadPause = false;
    if (pad) {
      const analog = Number(pad.axes[0]) || 0;
      const digital = (pad.buttons[15]?.pressed ? 1 : 0)
        - (pad.buttons[14]?.pressed ? 1 : 0);
      gamepadAxis = Math.abs(analog) > .12 ? analog : digital;
      gamepadPrimary = !!pad.buttons[0]?.pressed;
      gamepadPause = !!pad.buttons[3]?.pressed;
      /* Merely connecting a controller must not disable desktop keys. The
         controller becomes authoritative only after it produces input. */
      if (gamepadAxis || gamepadPrimary || gamepadPause)
        setInputSource("gamepad");
    }
    if (!pad && input.source === "gamepad") setInputSource("keyboard");
    const usingGamepad = input.source === "gamepad" && !!pad;
    const axis = usingGamepad ? gamepadAxis : keyboardAxis;
    const primary = usingGamepad ? gamepadPrimary : input.launch;
    const pause = usingGamepad ? gamepadPause : input.pause;
    input.axis = Math.max(-1, Math.min(1, axis));
    if (primary && !input.lastPrimary) {
      if (state.mode === "playing") launchOrFire();
      else if (state.mode === "paused") setMode("playing");
      else if (["title", "game-over", "victory"].includes(state.mode)) {
        resetGame();
        setMode("playing");
      }
    }
    if (pause && !input.lastPause) togglePause();
    input.lastPrimary = primary;
    input.lastPause = pause;
  }

  function keyboardAction(event) {
    const key = String(event.key || "");
    const code = String(event.code || "");
    const lower = key.length === 1 ? key.toLowerCase() : key;
    if (key === "ArrowLeft" || lower === "a" || code === "KeyA")
      return "left";
    if (key === "ArrowRight" || lower === "d" || code === "KeyD")
      return "right";
    if (key === " " || key === "Spacebar" || key === "Enter"
        || code === "Space" || code === "Enter") return "launch";
    if (lower === "p" || key === "Escape" || code === "KeyP")
      return "pause";
    return "";
  }

  addEventListener("keydown", (event) => {
    const action = keyboardAction(event);
    if (!action) return;
    input[action] = true;
    setInputSource("keyboard");
    event.preventDefault();
  });
  addEventListener("keyup", (event) => {
    const action = keyboardAction(event);
    if (!action) return;
    input[action] = false;
    event.preventDefault();
  });
  addEventListener("blur", () => {
    input.left = input.right = input.launch = input.pause = false;
    input.lastPrimary = input.lastPause = false;
  });
  addEventListener("gamepadconnected", updateControlHint);
  addEventListener("gamepaddisconnected", updateControlHint);
  canvas.addEventListener("pointermove", (event) => {
    if (!canvas.clientWidth) return;
    setInputSource("pointer");
    const relative = (event.clientX - canvas.getBoundingClientRect().left)
      / canvas.clientWidth;
    const bound = 4.42 - state.paddle.width * .5;
    state.paddle.x = Math.max(-bound, Math.min(bound, relative * 9 - 4.5));
  });
  canvas.addEventListener("pointerdown", () => {
    setInputSource("pointer");
    if (state.mode === "playing") launchOrFire();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden && state.mode === "playing") {
      state.mode = "hidden";
    } else if (!document.hidden && state.mode === "hidden") {
      state.mode = "playing";
      lastTimestamp = 0;
    }
  });

  function ballBrickCollision(ball, brick) {
    const dx = ball.x - brick.x, dy = ball.y - brick.y;
    const reachX = brick.width * .5 + ball.radius;
    const reachY = brick.height * .5 + ball.radius;
    if (Math.abs(dx) > reachX || Math.abs(dy) > reachY) return false;
    const penetrationX = reachX - Math.abs(dx);
    const penetrationY = reachY - Math.abs(dy);
    if (penetrationX < penetrationY) {
      ball.vx = dx < 0 ? -Math.abs(ball.vx) : Math.abs(ball.vx);
      ball.x += dx < 0 ? -penetrationX : penetrationX;
    } else {
      ball.vy = dy < 0 ? -Math.abs(ball.vy) : Math.abs(ball.vy);
      ball.y += dy < 0 ? -penetrationY : penetrationY;
    }
    hitBrick(brick);
    return true;
  }

  function loseBall(ball) {
    if (state.shield > 0) {
      state.shield = 0;
      ball.y = -3;
      ball.vy = Math.abs(ball.vy);
      sounds.play(140, .1, .14);
      return;
    }
    ball.alive = false;
    ball.trailCount = 0;
    if (state.balls.some((candidate) => candidate.alive)) return;
    state.lives--;
    hudDirty = true;
    state.combo = 0;
    if (state.lives <= 0) {
      setMode("game-over");
      sounds.play(90, .3, .14);
    } else {
      state.balls.length = 0;
      const replacement = makeBall();
      if (replacement) state.balls.push(replacement);
      showToast("BALL LOST", .9);
    }
  }

  function updateBall(ball, dt) {
    if (!ball.alive) return;
    if (ball.stuck) {
      ball.x = state.paddle.x;
      ball.y = -2.18;
      return;
    }
    ball.trailHead = (ball.trailHead + 6) % 7;
    ball.trailX[ball.trailHead] = ball.x;
    ball.trailY[ball.trailHead] = ball.y;
    if (ball.trailCount < 7) ball.trailCount++;
    ball.x += ball.vx * dt;
    ball.y += ball.vy * dt;
    if (ball.x < -4.42 + ball.radius) {
      ball.x = -4.42 + ball.radius;
      ball.vx = Math.abs(ball.vx);
      sounds.play(150, .02, .045);
    } else if (ball.x > 4.42 - ball.radius) {
      ball.x = 4.42 - ball.radius;
      ball.vx = -Math.abs(ball.vx);
      sounds.play(150, .02, .045);
    }
    if (ball.y > 3.05 - ball.radius) {
      ball.y = 3.05 - ball.radius;
      ball.vy = -Math.abs(ball.vy);
      sounds.play(180, .02, .045);
    }
    const paddleTop = -2.35;
    if (ball.vy < 0 && ball.y - ball.radius <= paddleTop
        && ball.y > paddleTop - .28
        && Math.abs(ball.x - state.paddle.x)
           <= state.paddle.width * .5 + ball.radius) {
      ball.y = paddleTop + ball.radius;
      const hit = (ball.x - state.paddle.x) / (state.paddle.width * .5);
      const speed = Math.min(5.25, Math.hypot(ball.vx, ball.vy) * 1.015);
      ball.vx = Math.max(-3.7, Math.min(3.7, hit * 3.3));
      ball.vy = Math.sqrt(Math.max(2.2, speed * speed - ball.vx * ball.vx));
      state.combo = 0;
      sounds.play(290, .035, .07);
    }
    /* Bricks occupy the authored 8x6 level lattice. Adjacent-cell probing
       preserves boundary collisions (the ball can overlap two cells) while
       replacing three full 40-48 brick scans in the multiball scene with at
       most nine bounded checks per ball. */
    const centerColumn = Math.floor(ball.x / 1.08 + 4);
    const centerRow = Math.floor((2.42 - ball.y) / .52 + .5);
    let brickHit = false;
    for (let row = Math.max(0, centerRow - 1);
         row <= Math.min(BRICK_ROWS - 1, centerRow + 1) && !brickHit; row++) {
      for (let column = Math.max(0, centerColumn - 1);
           column <= Math.min(BRICK_COLUMNS - 1, centerColumn + 1); column++) {
        const brick = brickGrid[row * BRICK_COLUMNS + column];
        if (brick?.alive && ballBrickCollision(ball, brick)) {
          brickHit = true;
          break;
        }
      }
    }
    if (ball.y < -3.38) loseBall(ball);
  }

  function updatePowerups(dt) {
    for (let at = state.powerups.length - 1; at >= 0; at--) {
      const power = state.powerups[at];
      power.y -= 1.15 * dt;
      power.phase += dt * 5;
      if (power.y < -2.15 && power.y > -2.75
          && Math.abs(power.x - state.paddle.x)
             < state.paddle.width * .5 + .18) {
        applyPower(power.type);
        state.powerups[at] = state.powerups[state.powerups.length - 1];
        state.powerups.pop();
      } else if (power.y < -3.35) {
        state.powerups[at] = state.powerups[state.powerups.length - 1];
        state.powerups.pop();
      }
    }
  }

  function updateShots(dt) {
    for (let at = state.shots.length - 1; at >= 0; at--) {
      const shot = state.shots[at];
      shot.y += 5.6 * dt;
      let removed = shot.y > 3.3;
      if (!removed) {
        const centerColumn = Math.floor(shot.x / 1.08 + 4);
        const centerRow = Math.floor((2.42 - shot.y) / .52 + .5);
        for (let row = Math.max(0, centerRow - 1);
             row <= Math.min(BRICK_ROWS - 1, centerRow + 1) && !removed;
             row++) {
          for (let column = Math.max(0, centerColumn - 1);
               column <= Math.min(BRICK_COLUMNS - 1, centerColumn + 1);
               column++) {
            const brick = brickGrid[row * BRICK_COLUMNS + column];
            if (brick?.alive
                && Math.abs(shot.x - brick.x) < brick.width * .5
                && Math.abs(shot.y - brick.y) < brick.height * .5 + .12) {
              hitBrick(brick);
              removed = true;
              break;
            }
          }
        }
      }
      if (removed) {
        state.shots[at] = state.shots[state.shots.length - 1];
        state.shots.pop();
      }
    }
  }

  function updateEffects(dt) {
    sounds.tick(state.time);
    for (let at = particleCount - 1; at >= 0; at--) {
      particleLife[at] -= dt;
      if (particleLife[at] <= 0) {
        const last = --particleCount;
        if (at != last) {
          particleX[at] = particleX[last];
          particleY[at] = particleY[last];
          particleVx[at] = particleVx[last];
          particleVy[at] = particleVy[last];
          particleLife[at] = particleLife[last];
          particleColors[at] = particleColors[last];
        }
        continue;
      }
      particleX[at] += particleVx[at] * dt;
      particleY[at] += particleVy[at] * dt;
      particleVy[at] -= 1.8 * dt;
    }
    if (toastUntil && state.time >= toastUntil) {
      hudToast = "";
      hudMeshDirty = true;
      hudDirty = true;
      toastUntil = 0;
    }
  }

  function update(dt) {
    state.time += dt;
    pollInput();
    updateEffects(dt);
    /* Explicit query/debug qualification keeps the worst bounded effects
       resident long enough for a real device cadence run. Normal gameplay
       never enters this path. */
    if (qualificationHeavy && state.time >= qualificationBurstAt) {
      spawnParticles(0, 0, SCENE_COLORS.ball, 48);
      qualificationBurstAt = state.time + .45;
    }
    if (state.mode === "level-clear") {
      state.transition -= dt;
      if (state.transition <= 0) {
        state.level++;
        if (state.level >= LEVELS.length) setMode("victory");
        else {
          buildLevel(state.level);
          setMode("playing");
        }
      }
      return;
    }
    if (state.mode !== "playing") return;
    if (state.time >= state.wideUntil) state.paddle.width = 1.55;
    const movement = input.axis * 5.4 * dt;
    state.paddle.x = Math.max(
      -4.42 + state.paddle.width * .5,
      Math.min(4.42 - state.paddle.width * .5, state.paddle.x + movement));
    for (let ballAt = 0; ballAt < state.balls.length; ballAt++)
      updateBall(state.balls[ballAt], dt);
    updatePowerups(dt);
    updateShots(dt);
  }

  function buildBackgroundScene() {
    vertexCount = indexCount = 0;
    for (const star of stars)
      addOctahedron(star.x, star.y, star.z, star.size,
        SCENE_COLORS.star, .48);
    addBox(0, -3.18, .16, 9.35, .08, .42, SCENE_COLORS.floor);
    addBox(-4.49, 0, .08, .18, 6.35, .42, SCENE_COLORS.rail);
    addBox(4.49, 0, .08, .18, 6.35, .42, SCENE_COLORS.rail);
    addBox(0, 3.18, .06, 9.25, .12, .35, SCENE_COLORS.rail);

    backgroundVertexCount = vertexCount;
    backgroundIndexCount = indexCount;
  }

  function buildStaticScene() {
    vertexCount = indexCount = 0;

    for (const brick of state.bricks) {
      if (!brick.alive) continue;
      const damage = brick.hp / brick.maximum;
      if (!addBox(brick.x, brick.y, 0, brick.width, brick.height, .34,
                  brick.color, 1, .58 + .42 * damage))
        state.meshDrops++;
    }
    staticVertexCount = vertexCount;
    staticIndexCount = indexCount;
  }

  function writeBrickMatrix(instance) {
    const brick = state.bricks[instance], at = instance * 16;
    brickInstanceMatrices.fill(0, at, at + 16);
    if (brick?.alive) {
      brickInstanceMatrices[at] = brick.width * .5;
      brickInstanceMatrices[at + 5] = brick.height * .5;
      brickInstanceMatrices[at + 10] = .17;
      brickInstanceMatrices[at + 12] = brick.x;
      brickInstanceMatrices[at + 13] = brick.y;
      brickInstanceMatrices[at + 14] = 0;
    } else {
      /* Keep the retained slot structurally valid but wholly off-clip. */
      brickInstanceMatrices[at + 12] = 32;
      brickInstanceMatrices[at + 13] = 32;
    }
    brickInstanceMatrices[at + 15] = 1;
  }

  function writeBrickTint(instance) {
    const brick = state.bricks[instance], at = instance * 4;
    if (!brick) {
      brickInstanceTints.fill(0, at, at + 4);
      return;
    }
    const brightness = .58 + .42 * brick.hp / brick.maximum;
    brickInstanceTints[at] = brick.color[0] * brightness;
    brickInstanceTints[at + 1] = brick.color[1] * brightness;
    brickInstanceTints[at + 2] = brick.color[2] * brightness;
    brickInstanceTints[at + 3] = 1;
  }

  function uploadBrickInstances() {
    if (!boxInstanceProgram) return;
    let uploaded = 0;
    if (brickMatrixDirtyLast >= brickMatrixDirtyFirst) {
      const first = brickMatrixDirtyFirst, last = brickMatrixDirtyLast;
      for (let instance = first; instance <= last; instance++)
        writeBrickMatrix(instance);
      const values = brickInstanceMatrices.subarray(
        first * 16, (last + 1) * 16);
      gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceMatrix);
      gl.bufferSubData(gl.ARRAY_BUFFER, first * 64, values);
      uploaded += values.length;
    }
    if (brickTintDirtyLast >= brickTintDirtyFirst) {
      const first = brickTintDirtyFirst, last = brickTintDirtyLast;
      for (let instance = first; instance <= last; instance++)
        writeBrickTint(instance);
      const values = brickInstanceTints.subarray(
        first * 4, (last + 1) * 4);
      gl.bindBuffer(gl.ARRAY_BUFFER, brickInstanceTint);
      gl.bufferSubData(gl.ARRAY_BUFFER, first * 16, values);
      uploaded += values.length;
    }
    brickMatrixDirtyFirst = brickTintDirtyFirst = MAX_BRICK_INSTANCES;
    brickMatrixDirtyLast = brickTintDirtyLast = -1;
    if (uploaded) {
      brickInstanceUploads++;
      brickInstanceFloatsUploaded += uploaded;
    }
    staticVertexCount = staticIndexCount = 0;
  }

  function buildDynamicScene() {
    vertexCount = indexCount = 0;
    resetOctaInstances();
    boxInstanceCount = 0;
    collectInstancedOcta = instanceProgram !== null;
    collectInstancedBoxes = boxInstanceProgram !== null;
    const paddleColor = state.laser ? SCENE_COLORS.paddleLaser
      : state.time < state.wideUntil
        ? SCENE_COLORS.paddleWide : SCENE_COLORS.paddle;
    addBox(state.paddle.x, -2.52, .04,
      state.paddle.width, .25, .44, paddleColor);
    if (state.shield)
      addBox(0, -3.02, .02, 8.6, .06, .16, SCENE_COLORS.shield, .8);

    for (let ballAt = 0; ballAt < state.balls.length; ballAt++) {
      const ball = state.balls[ballAt];
      if (!ball.alive) continue;
      for (let at = ball.trailCount - 1; at >= 0; at--) {
        const slot = (ball.trailHead + at) % 7;
        const strength = 1 - at / ball.trailCount;
        addOctahedron(ball.trailX[slot], ball.trailY[slot], .08,
          ball.radius * (.3 + strength * .45),
          SCENE_COLORS.trail, strength * .24);
      }
      addOctahedron(ball.x, ball.y, .1, ball.radius, SCENE_COLORS.ball);
    }
    for (let powerAt = 0; powerAt < state.powerups.length; powerAt++) {
      const power = state.powerups[powerAt];
      const bob = Math.sin(power.phase) * .045;
      addOctahedron(power.x, power.y + bob, .14, .19,
        POWER_COLORS[power.type]);
    }
    for (let shotAt = 0; shotAt < state.shots.length; shotAt++) {
      const shot = state.shots[shotAt];
      addBox(shot.x, shot.y, .1, .055, .28, .09, SCENE_COLORS.shot);
    }
    for (let particleAt = 0; particleAt < particleCount; particleAt++) {
      let alpha = particleLife[particleAt] / .85;
      if (alpha > 1) alpha = 1;
      addOctahedron(
        particleX[particleAt], particleY[particleAt], .22,
        .035 + .035 * alpha, particleColors[particleAt], alpha);
    }
    collectInstancedOcta = false;
    collectInstancedBoxes = false;
  }

  function uploadMesh(position, color, index) {
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      positions.subarray(0, vertexCount * 3));
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      colors.subarray(0, vertexCount * 4));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0,
      indices.subarray(0, indexCount));
  }

  function uploadImmutableMesh(position, color, index) {
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.bufferData(gl.ARRAY_BUFFER,
      positions.slice(0, vertexCount * 3), gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.bufferData(gl.ARRAY_BUFFER,
      colors.slice(0, vertexCount * 4), gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,
      indices.slice(0, indexCount), gl.STATIC_DRAW);
  }

  function drawMesh(vertexArray, position, color, index, count) {
    if (vertexArrays) vertexArrays.bindVertexArrayOES(vertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, position);
      gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, color);
      gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    }
    gl.drawElements(gl.TRIANGLES, count, gl.UNSIGNED_SHORT, 0);
  }

  function uploadOctaInstances() {
    if (!instanceProgram || !octaInstanceCount) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTransform);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      octaTransforms.subarray(0, octaInstanceCount * 4));
    gl.bindBuffer(gl.ARRAY_BUFFER, instanceTint);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      octaTints.subarray(0, octaInstanceCount * 4));
  }

  function uploadBoxInstances() {
    if (!boxInstanceProgram || !boxInstanceCount) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceMatrix);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      boxInstanceMatrices.subarray(0, boxInstanceCount * 16));
    gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceTint);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      boxInstanceTints.subarray(0, boxInstanceCount * 4));
  }

  function drawBoxInstanceStream(vertexArray, matrixBuffer, tintBuffer,
                                 count) {
    if (!boxInstanceProgram || !count) return;
    gl.useProgram(boxInstanceProgram);
    gl.uniformMatrix4fv(boxInstanceModelLocation, false, model);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(vertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstancePosition);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, boxInstanceShade);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, matrixBuffer);
      for (let column = 0; column < 4; column++)
        gl.vertexAttribPointer(boxInstanceMatrixLocation + column, 4,
          gl.FLOAT, false, 64, column * 16);
      gl.bindBuffer(gl.ARRAY_BUFFER, tintBuffer);
      gl.vertexAttribPointer(boxInstanceTintLocation, 4,
        gl.FLOAT, false, 16, 0);
    }
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, boxInstanceIndex);
    instancing.drawElementsInstancedANGLE(
      gl.TRIANGLES, BOX_INDICES.length, gl.UNSIGNED_SHORT, 0,
      count);
  }

  function drawBrickInstances() {
    drawBoxInstanceStream(
      brickInstanceVertexArray, brickInstanceMatrix, brickInstanceTint,
      brickInstanceCount);
    gl.useProgram(program);
  }

  function drawBoxInstances() {
    drawBoxInstanceStream(
      boxInstanceVertexArray, boxInstanceMatrix, boxInstanceTint,
      boxInstanceCount);
  }

  function drawOctaInstances() {
    if (!instanceProgram || !octaInstanceCount) return;
    gl.useProgram(instanceProgram);
    gl.uniformMatrix4fv(instanceModelLocation, false, model);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(instanceVertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, instancePosition);
      gl.vertexAttribPointer(
        instancePositionLocation, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, instanceShade);
      gl.vertexAttribPointer(
        instanceShadeLocation, 4, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, instanceIndex);
    }
    for (let first = 0; first < octaInstanceCount;
         first += MAX_INSTANCES_PER_DRAW) {
      const count = Math.min(MAX_INSTANCES_PER_DRAW,
        octaInstanceCount - first);
      if (instancePointerStart !== first) {
        gl.bindBuffer(gl.ARRAY_BUFFER, instanceTransform);
        gl.vertexAttribPointer(instanceTransformLocation, 4,
          gl.FLOAT, false, 16, first * 16);
        gl.bindBuffer(gl.ARRAY_BUFFER, instanceTint);
        gl.vertexAttribPointer(instanceTintLocation, 4,
          gl.FLOAT, false, 16, first * 16);
        instancePointerStart = first;
      }
      instancing.drawElementsInstancedANGLE(
        gl.TRIANGLES, OCTA_INDICES.length, gl.UNSIGNED_SHORT, 0, count);
    }
    gl.useProgram(program);
  }

  function updateModel() {
    /* Keep the title field visibly alive without starting game physics. The
       same model-uniform path used by play animates the whole star/rail field,
       so the title adds no geometry uploads, particles, or DOM mutations. */
    const title = state.mode === "title" ? 1 : 0;
    const titleX = title * Math.sin(state.time * .72) * .075;
    const titleY = title * Math.cos(state.time * .54) * .045;
    const roll = title * Math.sin(state.time * .38) * .012;
    const cosine = Math.cos(roll), sine = Math.sin(roll);
    model.fill(0);
    model[0] = cosine;
    model[1] = sine;
    model[4] = -sine;
    model[5] = cosine;
    model[10] = 1;
    model[12] = titleX;
    model[13] = titleY;
    model[15] = 1;
    gl.uniformMatrix4fv(modelLocation, false, model);
  }

  function writeHudVertex(pixelX, pixelY, tint) {
    const at = hudVertexCount * 6;
    hudVertices[at] = pixelX / 160 - 1;
    hudVertices[at + 1] = 1 - pixelY / 90;
    hudVertices[at + 2] = tint[0];
    hudVertices[at + 3] = tint[1];
    hudVertices[at + 4] = tint[2];
    hudVertices[at + 5] = tint[3];
    hudVertexCount++;
  }

  function addHudRect(pixelX, pixelY, width, height, tint) {
    if (hudVertexCount / 4 >= HUD_RECT_LIMIT) return false;
    const base = hudVertexCount;
    writeHudVertex(pixelX, pixelY, tint);
    writeHudVertex(pixelX + width, pixelY, tint);
    writeHudVertex(pixelX + width, pixelY + height, tint);
    writeHudVertex(pixelX, pixelY + height, tint);
    hudIndices[hudIndexCount++] = base;
    hudIndices[hudIndexCount++] = base + 1;
    hudIndices[hudIndexCount++] = base + 2;
    hudIndices[hudIndexCount++] = base;
    hudIndices[hudIndexCount++] = base + 2;
    hudIndices[hudIndexCount++] = base + 3;
    return true;
  }

  function addHudGlyph(character, pixelX, pixelY, scale, tint) {
    if (hudCharacterCount >= HUD_GLYPH_LIMIT
        || hudVertexCount / 4 > HUD_RECT_LIMIT - 10) return false;
    const pattern = HUD_PATTERNS[HUD_CHARS.indexOf(character) >= 0
      ? character : " "];
    hudCharacterCount++;
    for (let row = 0; row < 5; row++) {
      let column = 0;
      while (column < 3) {
        if (pattern[row * 3 + column] !== "1") { column++; continue; }
        const first = column;
        while (column < 3 && pattern[row * 3 + column] === "1") column++;
        if (!addHudRect(pixelX + first * scale, pixelY + row * scale,
                        (column - first) * scale, scale, tint)) return false;
      }
    }
    return true;
  }

  function addHudText(text, pixelX, pixelY, scale, tint) {
    text = String(text).toUpperCase();
    for (let at = 0; at < text.length; at++) {
      if (!addHudGlyph(text[at], pixelX, pixelY, scale, tint)) break;
      pixelX += 4 * scale;
    }
  }

  function rebuildHudMesh() {
    hudVertexCount = hudIndexCount = hudCharacterCount = 0;
    addHudText(hudScore || "000000", 7, 5, 2, HUD_SCORE_TINT);
    if (hudPower)
      addHudText(hudPower, 7, 164, 2, HUD_POWER_TINT);
    if (hudToast) {
      const width = hudToast.length * 8;
      addHudText(hudToast, Math.max(7, (320 - width) * .5), 27, 2,
        HUD_TOAST_TINT);
    }
    if (vertexArrays) vertexArrays.bindVertexArrayOES(hudVertexArray);
    gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      hudVertices.subarray(0, hudVertexCount * 6));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0,
      hudIndices.subarray(0, hudIndexCount));
    if (vertexArrays) vertexArrays.bindVertexArrayOES(null);
    hudMeshDirty = false;
  }

  function drawHud() {
    if (!hudIndexCount) return;
    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(hudProgram);
    if (vertexArrays) vertexArrays.bindVertexArrayOES(hudVertexArray);
    else {
      gl.bindBuffer(gl.ARRAY_BUFFER, hudVertexBuffer);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, hudIndexBuffer);
    }
    gl.drawElements(gl.TRIANGLES, hudIndexCount, gl.UNSIGNED_SHORT, 0);
    gl.enable(gl.DEPTH_TEST);
    gl.useProgram(program);
  }

  function updateHud() {
    const second = state.time | 0;
    const scoreTick = (state.time * 4) | 0;
    if (!hudDirty && second === hudSecond && scoreTick === hudTick
        && !hudMeshDirty) return;
    hudDirty = false;
    hudSecond = second;
    hudTick = scoreTick;
    let powerText = "";
    if (state.time < state.wideUntil) powerText = "WIDE";
    if (state.time < state.slowUntil)
      powerText += `${powerText ? " " : ""}SLOW`;
    if (state.shield) powerText += `${powerText ? " " : ""}SHIELD`;
    if (state.laser)
      powerText += `${powerText ? " " : ""}LASER`;
    const scoreText = String(state.score).padStart(6, "0"),
      levelText = `LEVEL ${Math.min(LEVELS.length, state.level + 1)}`,
      livesText = `LIVES ${state.lives}`;
    const next = `${state.score}|${state.level}|${state.lives}|${powerText}`;
    if (next === hudCache && scoreText === hudScore && !hudMeshDirty) return;
    hudCache = next;
    /* A score change is the common case. Avoid sending the other three
       unchanged strings through the DOM bridge: on the PSP each authored
       text mutation has to validate, journal and mark layout damage. */
    if (scoreText !== hudScore
        && (hudScore === "" || scoreTick !== hudScoreTick
            || state.mode !== "playing")) {
      hudScore = scoreText;
      hudScoreTick = scoreTick;
      hudMeshDirty = true;
    }
    if (levelText !== hudLevel) {
      hudLevel = levelText;
      ui.level.textContent = levelText;
    }
    if (livesText !== hudLives) {
      hudLives = livesText;
      ui.lives.textContent = livesText;
    }
    if (powerText !== hudPower) {
      hudPower = powerText;
      hudMeshDirty = true;
    }
    if (hudMeshDirty) rebuildHudMesh();
  }

  function render(profileFrame = false) {
    const hudStarted = profileFrame ? performance.now() : 0;
    updateHud();
    if (profileFrame) {
      const hudElapsed = performance.now() - hudStarted;
      qualificationTiming.hud += hudElapsed;
      qualificationTiming.maxHud = Math.max(
        qualificationTiming.maxHud, hudElapsed);
    }
    if (backgroundDirty) {
      buildBackgroundScene();
      uploadImmutableMesh(
        backgroundPositionBuffer, backgroundColorBuffer,
        backgroundIndexBuffer);
      backgroundDirty = false;
    }
    if (staticDirty) {
      if (boxInstanceProgram) uploadBrickInstances();
      else {
        buildStaticScene();
        uploadMesh(staticPositionBuffer, staticColorBuffer, staticIndexBuffer);
      }
      staticDirty = false;
    }
    let phaseStarted = profileFrame ? performance.now() : 0;
    buildDynamicScene();
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - phaseStarted;
      qualificationTiming.build += elapsed;
      qualificationTiming.maxBuild = Math.max(
        qualificationTiming.maxBuild, elapsed);
      phaseStarted = now;
    }
    if (indexCount) uploadMesh(positionBuffer, colorBuffer, indexBuffer);
    uploadBoxInstances();
    uploadOctaInstances();
    if (profileFrame) {
      const now = performance.now();
      const elapsed = now - phaseStarted;
      qualificationTiming.upload += elapsed;
      qualificationTiming.maxUpload = Math.max(
        qualificationTiming.maxUpload, elapsed);
      phaseStarted = now;
    }
    const commandsStarted = profileFrame ? phaseStarted : 0;
    updateModel();
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.model += now - phaseStarted;
      phaseStarted = now;
    }
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.clear += now - phaseStarted;
      phaseStarted = now;
    }
    if (backgroundIndexCount)
      drawMesh(backgroundVertexArray, backgroundPositionBuffer,
        backgroundColorBuffer, backgroundIndexBuffer,
        backgroundIndexCount);
    if (boxInstanceProgram) drawBrickInstances();
    else if (staticIndexCount)
      drawMesh(staticVertexArray, staticPositionBuffer, staticColorBuffer,
        staticIndexBuffer, staticIndexCount);
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.staticDraw += now - phaseStarted;
      phaseStarted = now;
    }
    if (indexCount)
      drawMesh(dynamicVertexArray, positionBuffer, colorBuffer,
        indexBuffer, indexCount);
    drawBoxInstances();
    if (profileFrame) {
      const now = performance.now();
      qualificationTiming.dynamicDraw += now - phaseStarted;
      phaseStarted = now;
    }
    drawOctaInstances();
    drawHud();
    if (profileFrame) {
      const now = performance.now();
      const instanceElapsed = now - phaseStarted;
      const commandElapsed = now - commandsStarted;
      qualificationTiming.instanceDraw += instanceElapsed;
      qualificationTiming.commands += commandElapsed;
      qualificationTiming.maxCommands = Math.max(
        qualificationTiming.maxCommands, commandElapsed);
      phaseStarted = now;
    }
    state.frames++;
    if (profileFrame) {
      qualificationTiming.samples++;
      if (qualificationTiming.samples === 80) {
        const count = qualificationTiming.samples;
        const bridge = globalThis.__tilefinchWebGLDiagnostics,
          bridgeDraws = Math.max(1, Number(bridge?.profileDraws) || 0);
        const summary = [
          "PRISM-JS-PROFILE",
          `samples=${count}`,
          `update=${(qualificationTiming.update / count).toFixed(3)}ms`,
          `build=${(qualificationTiming.build / count).toFixed(3)}ms`,
          `upload=${(qualificationTiming.upload / count).toFixed(3)}ms`,
          `commands=${(qualificationTiming.commands / count).toFixed(3)}ms`,
          `model=${(qualificationTiming.model / count).toFixed(3)}ms`,
          `clear=${(qualificationTiming.clear / count).toFixed(3)}ms`,
          `static=${(qualificationTiming.staticDraw / count).toFixed(3)}ms`,
          `dynamic=${(qualificationTiming.dynamicDraw / count).toFixed(3)}ms`,
          `instances=${(qualificationTiming.instanceDraw / count).toFixed(3)}ms`,
          `hud=${(qualificationTiming.hud / count).toFixed(3)}ms`,
          `max-update=${qualificationTiming.maxUpdate.toFixed(3)}ms`,
          `max-build=${qualificationTiming.maxBuild.toFixed(3)}ms`,
          `max-upload=${qualificationTiming.maxUpload.toFixed(3)}ms`,
          `max-commands=${qualificationTiming.maxCommands.toFixed(3)}ms`,
          `max-hud=${qualificationTiming.maxHud.toFixed(3)}ms`,
          `max-frame=${qualificationTiming.maxFrame.toFixed(3)}ms`,
          `bridge-draws=${bridgeDraws}`,
          `bridge-basic=${(Number(bridge?.profileBasicMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-instance=${(Number(bridge?.profileInstancesMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-range=${(Number(bridge?.profileRangesMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-prepare=${(Number(bridge?.profilePrepareMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-enqueue=${(Number(bridge?.profileEnqueueMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-finish=${(Number(bridge?.profileFinishMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-admit=${(Number(bridge?.profileQueueAdmissionMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `bridge-pack=${(Number(bridge?.profileWirePackMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `wire-sources=${(Number(bridge?.profileWireSourcesMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `wire-state=${(Number(bridge?.profileWireStateMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `wire-instances=${(Number(bridge?.profileWireInstancesMs || 0) / bridgeDraws).toFixed(3)}ms`,
          `wire-retain=${(Number(bridge?.profileWireRetainMs || 0) / bridgeDraws).toFixed(3)}ms`,
        ].join(" ");
        globalThis.pocSummary = summary;
        console.log(summary);
        if (bridge) bridge.profileDrawPhases = false;
      }
    }
  }

  function frame(timestamp) {
    if (!state.running) return;
    /* A catch-up accumulator makes a slow device perform two complete game
       updates in an already late frame, which creates a self-sustaining
       latency spike. The fastest authored objects move less than one brick
       thickness in 1/30 s, so one capped variable step remains collision-safe
       and lets a long stall degrade by slowing time instead of compounding. */
    const elapsed = lastTimestamp
      ? Math.min(1 / 30, (timestamp - lastTimestamp) / 1000) : 0;
    lastTimestamp = timestamp;
    const profileFrame = qualificationProfile
      && qualificationTiming.samples < 80 && state.frames >= 8;
    const frameStarted = profileFrame ? performance.now() : 0;
    const updateStarted = profileFrame ? performance.now() : 0;
    if (elapsed > 0) update(elapsed);
    if (profileFrame) {
      const updateElapsed = performance.now() - updateStarted;
      qualificationTiming.update += updateElapsed;
      qualificationTiming.maxUpdate = Math.max(
        qualificationTiming.maxUpdate, updateElapsed);
    }
    render(profileFrame);
    if (profileFrame) {
      const frameElapsed = performance.now() - frameStarted;
      qualificationTiming.frame += frameElapsed;
      qualificationTiming.maxFrame = Math.max(
        qualificationTiming.maxFrame, frameElapsed);
    }
    requestAnimationFrame(frame);
  }

  function beginQualification(mode) {
    qualificationHeavy = mode === "heavy" || mode === "profile"
      || mode === "tail";
    qualificationProfile = mode === "profile" || mode === "tail";
    qualificationBridgeProfile = mode === "profile";
    for (const key of Object.keys(qualificationTiming))
      qualificationTiming[key] = 0;
    const bridge = globalThis.__tilefinchWebGLDiagnostics;
    if (bridge) {
      bridge.profileDrawPhases = qualificationBridgeProfile;
      for (const key of ["profileDraws", "profileBasicMs",
        "profileInstancesMs", "profileRangesMs", "profilePrepareMs",
        "profileEnqueueMs", "profileFinishMs", "profileQueueAdmissionMs",
        "profileWirePackMs", "profileWireSourcesMs", "profileWireStateMs",
        "profileWireInstancesMs", "profileWireRetainMs"]) bridge[key] = 0;
    }
    qualificationBurstAt = 0;
    resetGame();
    setMode("playing");
    launchOrFire();
    if (qualificationHeavy) {
      applyPower("multi");
      applyPower("wide");
      applyPower("shield");
      applyPower("laser");
      spawnParticles(0, 0, SCENE_COLORS.ball, 48);
      qualificationBurstAt = .45;
    }
  }

  const debug = Object.freeze({
    start() { resetGame(); setMode("playing"); },
    step(frames = 1) {
      frames = Math.max(0, Math.min(600, Number(frames) | 0));
      for (let at = 0; at < frames; at++) update(1 / 60);
      render();
    },
    clearLevel() {
      for (const brick of state.bricks) {
        brick.alive = false;
        markBrickMatrixDirty(brick);
      }
      state.mode = "level-clear";
      state.transition = 0;
    },
    strikeBrick(index = 0) {
      index = Math.max(0, Math.min(state.bricks.length - 1,
        Number(index) | 0));
      const brick = state.bricks[index];
      if (brick?.alive) hitBrick(brick);
      render();
    },
    setScore(value) {
      state.score = Math.max(0, Number(value) | 0);
      hudDirty = true;
      hudScoreTick = -1;
      updateHud();
    },
    injectPower(type) {
      if (Object.prototype.hasOwnProperty.call(POWER_NAMES, type)) applyPower(type);
    },
    burst() {
      spawnParticles(0, 0, SCENE_COLORS.ball, 48);
      render();
    },
    launch() { launchOrFire(); },
    qualify(mode) { beginQualification(String(mode)); },
    snapshot() {
      return {
        mode: state.mode, score: state.score, lives: state.lives,
        level: state.level, bricks: state.bricks.filter((brick) => brick.alive).length,
        balls: state.balls.filter((ball) => ball.alive).length,
        movingBalls: state.balls.filter((ball) => ball.alive && !ball.stuck).length,
        particles: particleCount, powerups: state.powerups.length,
        shots: state.shots.length, vertices: vertexCount, indices: indexCount,
        backgroundVertices: backgroundVertexCount,
        backgroundIndices: backgroundIndexCount,
        staticVertices: staticVertexCount, staticIndices: staticIndexCount,
        brickInstances: boxInstanceProgram ? brickInstanceCount : 0,
        hudGlyphs: hudCharacterCount,
        brickInstanceUploads, brickInstanceFloatsUploaded,
        paddleX: state.paddle.x, paddleWidth: state.paddle.width,
        shield: state.shield, laser: state.laser,
        meshDrops: state.meshDrops, frames: state.frames,
        titleMotionX: model[12], titleMotionY: model[13],
        inputSource: input.source,
        gamepadConnected: input.gamepadConnected,
      };
    },
    stop() { state.running = false; },
  });
  Object.defineProperty(globalThis, "__prismBreakDebug", {
    value: debug, configurable: false, writable: false,
  });
  globalThis.pocSummary = "PRISM-BREAK-READY";
  if (location.search.includes("qualification=profile")) {
    beginQualification("profile");
  } else if (location.search.includes("qualification=tail")) {
    beginQualification("tail");
  } else if (location.search.includes("qualification=heavy")) {
    beginQualification("heavy");
  } else if (location.search.includes("qualification=ordinary")
             || location.search.includes("demo=1")) {
    beginQualification("ordinary");
  } else {
    /* Offline installation serializes the live document. Re-establish the
       authored entry state in case the snapshot was taken while the game or
       pause overlay had hidden/relabelled this panel. */
    showPanel(
      "PRISM BREAK 3D",
      "Break every prism. Catch falling power cores.",
      "Play");
  }
  updateControlHint();
  render();
  requestAnimationFrame(frame);
})();
