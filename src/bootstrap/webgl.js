(() => {
  "use strict";

  /* Surface retirement also advances the native cache incarnation.  Page
     code never reads, selects, or serializes that process-issued identity. */
  const releaseNativeWebGLSurface =
    globalThis.__tilefinchWebGLReleaseSurface;

  /* Tilefinch exposes the WebGL 1 object model, but deliberately translates
     only the fixed-function-compatible shader shapes the PSP GE can execute.
     Limits are part of the API contract: refusal loses this context softly
     rather than growing with page-controlled data. */
  const MAX_CONTEXTS = 2,
    MAX_BUFFER_BYTES = 512 * 1024,
    MAX_TEXTURE_BYTES = 416 * 1024,
    MAX_BUFFER_OBJECTS = 24,
    MAX_TEXTURE_OBJECTS = 8,
    MAX_SHADER_OBJECTS = 16,
    MAX_PROGRAM_OBJECTS = 8,
    MAX_FRAMEBUFFER_OBJECTS = 4,
    MAX_RENDERBUFFER_OBJECTS = 4,
    /* One payload per admitted buffer and texture is the exact native cap.
       Changing either object limit requires re-measuring the PSP bridge. */
    MAX_NATIVE_SOURCES = MAX_BUFFER_OBJECTS + MAX_TEXTURE_OBJECTS,
    MAX_SHADER_BYTES = 16 * 1024,
    MAX_INTERFACE_DECLARATIONS = 24,
    MAX_DRAWS = 64,
    MAX_VERTICES = 4096,
    MAX_DRAWING_WIDTH = 480,
    MAX_DRAWING_HEIGHT = 272,
    MAX_DRAWING_PIXELS = 131072,
    COMMAND_WORDS = 64,
    TEXTURE_WORDS = 9,
    WIRE_HEADER_WORDS = 4,
    COMMAND_WIRE_MAGIC = 0x54465743,
    TEXTURE_WIRE_MAGIC = 0x54465754,
    WIRE_VERSION = 1,
    FLUSH_FAILED = 0,
    FLUSH_OK = 1,
    FLUSH_DEFERRED = 2,
    contexts = new WeakMap(),
    liveContexts = [],
    diagnostics = {
      contexts: 0,
      lostContexts: 0,
      shaderRefusals: 0,
      drawCalls: 0,
      flushes: 0,
      vertices: 0,
      uploadedTextureBytes: 0,
      nativeFailures: 0,
      deferredFlushes: 0,
      forcedFlushes: 0,
      maximumForcedFlushesPerFrame: 0,
    },
    constants = {
      DEPTH_BUFFER_BIT: 0x00000100,
      STENCIL_BUFFER_BIT: 0x00000400,
      COLOR_BUFFER_BIT: 0x00004000,
      POINTS: 0x0000, LINES: 0x0001, LINE_LOOP: 0x0002,
      LINE_STRIP: 0x0003, TRIANGLES: 0x0004,
      TRIANGLE_STRIP: 0x0005, TRIANGLE_FAN: 0x0006,
      ZERO: 0, NONE: 0, ONE: 1, SRC_COLOR: 0x0300,
      ONE_MINUS_SRC_COLOR: 0x0301, SRC_ALPHA: 0x0302,
      ONE_MINUS_SRC_ALPHA: 0x0303, DST_ALPHA: 0x0304,
      ONE_MINUS_DST_ALPHA: 0x0305, DST_COLOR: 0x0306,
      ONE_MINUS_DST_COLOR: 0x0307, SRC_ALPHA_SATURATE: 0x0308,
      FUNC_ADD: 0x8006,
      ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893,
      ARRAY_BUFFER_BINDING: 0x8894, ELEMENT_ARRAY_BUFFER_BINDING: 0x8895,
      STREAM_DRAW: 0x88e0, STATIC_DRAW: 0x88e4, DYNAMIC_DRAW: 0x88e8,
      BUFFER_SIZE: 0x8764, BUFFER_USAGE: 0x8765,
      CURRENT_VERTEX_ATTRIB: 0x8626,
      VERTEX_ATTRIB_ARRAY_ENABLED: 0x8622,
      VERTEX_ATTRIB_ARRAY_SIZE: 0x8623,
      VERTEX_ATTRIB_ARRAY_STRIDE: 0x8624,
      VERTEX_ATTRIB_ARRAY_TYPE: 0x8625,
      VERTEX_ATTRIB_ARRAY_NORMALIZED: 0x886a,
      VERTEX_ATTRIB_ARRAY_POINTER: 0x8645,
      VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: 0x889f,
      BYTE: 0x1400, UNSIGNED_BYTE: 0x1401, SHORT: 0x1402,
      UNSIGNED_SHORT: 0x1403, INT: 0x1404, UNSIGNED_INT: 0x1405,
      FLOAT: 0x1406,
      CULL_FACE: 0x0b44, BLEND: 0x0be2, DITHER: 0x0bd0,
      STENCIL_TEST: 0x0b90, DEPTH_TEST: 0x0b71, SCISSOR_TEST: 0x0c11,
      POLYGON_OFFSET_FILL: 0x8037, SAMPLE_ALPHA_TO_COVERAGE: 0x809e,
      SAMPLE_COVERAGE: 0x80a0,
      NO_ERROR: 0, INVALID_ENUM: 0x0500, INVALID_VALUE: 0x0501,
      INVALID_OPERATION: 0x0502, OUT_OF_MEMORY: 0x0505,
      CONTEXT_LOST_WEBGL: 0x9242,
      CW: 0x0900, CCW: 0x0901, FRONT: 0x0404, BACK: 0x0405,
      FRONT_AND_BACK: 0x0408,
      NEVER: 0x0200, LESS: 0x0201, EQUAL: 0x0202, LEQUAL: 0x0203,
      GREATER: 0x0204, NOTEQUAL: 0x0205, GEQUAL: 0x0206, ALWAYS: 0x0207,
      TEXTURE_2D: 0x0de1, TEXTURE: 0x1702,
      TEXTURE0: 0x84c0, ACTIVE_TEXTURE: 0x84e0,
      TEXTURE_BINDING_2D: 0x8069,
      TEXTURE_MAG_FILTER: 0x2800, TEXTURE_MIN_FILTER: 0x2801,
      TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
      NEAREST: 0x2600, LINEAR: 0x2601,
      NEAREST_MIPMAP_NEAREST: 0x2700, LINEAR_MIPMAP_NEAREST: 0x2701,
      NEAREST_MIPMAP_LINEAR: 0x2702, LINEAR_MIPMAP_LINEAR: 0x2703,
      REPEAT: 0x2901, CLAMP_TO_EDGE: 0x812f, MIRRORED_REPEAT: 0x8370,
      ALPHA: 0x1906, RGB: 0x1907, RGBA: 0x1908,
      LUMINANCE: 0x1909, LUMINANCE_ALPHA: 0x190a,
      UNPACK_ALIGNMENT: 0x0cf5, PACK_ALIGNMENT: 0x0d05,
      UNPACK_FLIP_Y_WEBGL: 0x9240,
      UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241,
      UNPACK_COLORSPACE_CONVERSION_WEBGL: 0x9243,
      BROWSER_DEFAULT_WEBGL: 0x9244,
      VERTEX_SHADER: 0x8b31, FRAGMENT_SHADER: 0x8b30,
      COMPILE_STATUS: 0x8b81, LINK_STATUS: 0x8b82,
      VALIDATE_STATUS: 0x8b83, DELETE_STATUS: 0x8b80,
      SHADER_TYPE: 0x8b4f, ATTACHED_SHADERS: 0x8b85,
      ACTIVE_UNIFORMS: 0x8b86, ACTIVE_ATTRIBUTES: 0x8b89,
      CURRENT_PROGRAM: 0x8b8d,
      FLOAT_VEC2: 0x8b50, FLOAT_VEC3: 0x8b51, FLOAT_VEC4: 0x8b52,
      INT_VEC2: 0x8b53, INT_VEC3: 0x8b54, INT_VEC4: 0x8b55,
      BOOL: 0x8b56, BOOL_VEC2: 0x8b57, BOOL_VEC3: 0x8b58,
      BOOL_VEC4: 0x8b59, FLOAT_MAT2: 0x8b5a, FLOAT_MAT3: 0x8b5b,
      FLOAT_MAT4: 0x8b5c, SAMPLER_2D: 0x8b5e,
      VENDOR: 0x1f00, RENDERER: 0x1f01, VERSION: 0x1f02,
      SHADING_LANGUAGE_VERSION: 0x8b8c,
      VIEWPORT: 0x0ba2, SCISSOR_BOX: 0x0c10,
      COLOR_CLEAR_VALUE: 0x0c22, DEPTH_CLEAR_VALUE: 0x0b73,
      DEPTH_FUNC: 0x0b74, BLEND_SRC_RGB: 0x80c9,
      BLEND_DST_RGB: 0x80c8, CULL_FACE_MODE: 0x0b45,
      FRONT_FACE: 0x0b46, LINE_WIDTH: 0x0b21,
      MAX_TEXTURE_SIZE: 0x0d33, MAX_VIEWPORT_DIMS: 0x0d3a,
      MAX_VERTEX_ATTRIBS: 0x8869, MAX_TEXTURE_IMAGE_UNITS: 0x8872,
      MAX_COMBINED_TEXTURE_IMAGE_UNITS: 0x8b4d,
      MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8b4c,
      MAX_VARYING_VECTORS: 0x8dfc, MAX_VERTEX_UNIFORM_VECTORS: 0x8dfb,
      MAX_FRAGMENT_UNIFORM_VECTORS: 0x8dfd,
      FRAMEBUFFER: 0x8d40, RENDERBUFFER: 0x8d41,
      FRAMEBUFFER_COMPLETE: 0x8cd5,
    },
    typeBytes = (type) => ({
      [constants.BYTE]: 1, [constants.UNSIGNED_BYTE]: 1,
      [constants.SHORT]: 2, [constants.UNSIGNED_SHORT]: 2,
      [constants.FLOAT]: 4,
    })[type] || 0,
    toWebGLInt32 = (value) => Number(value) | 0,
    identityMatrix = () => new Float32Array([
      1, 0, 0, 0, 0, 1, 0, 0,
      0, 0, 1, 0, 0, 0, 0, 1,
    ]),
    boundedDrawingSize = (width, height) => {
      width = Number(width); height = Number(height);
      if (!Number.isInteger(width) || !Number.isInteger(height)
          || width < 0 || height < 0) return null;
      if (!width || !height) return {
        width: Math.min(width, MAX_DRAWING_WIDTH),
        height: Math.min(height, MAX_DRAWING_HEIGHT),
      };
      const scale = Math.min(
        1,
        MAX_DRAWING_WIDTH / width,
        MAX_DRAWING_HEIGHT / height,
        Math.sqrt(MAX_DRAWING_PIXELS / (width * height)),
      );
      return {
        width: Math.max(1, Math.floor(width * scale)),
        height: Math.max(1, Math.floor(height * scale)),
      };
    },
    multiplyMatrix4Into = (output, left, right) => {
      for (let column = 0; column < 4; column++) {
        for (let row = 0; row < 4; row++) {
          let value = 0;
          for (let inner = 0; inner < 4; inner++)
            value += left[inner * 4 + row] * right[column * 4 + inner];
          output[column * 4 + row] = value;
        }
      }
      return output;
    },
    combinedMatrix4Into = (output, scratch, entries, values) => {
      output.fill(0);
      output[0] = output[5] = output[10] = output[15] = 1;
      if (!entries.length) return output;
      let current = output, alternate = scratch;
      for (const entry of entries) {
        multiplyMatrix4Into(alternate, current, values.get(entry.name));
        const swap = current; current = alternate; alternate = swap;
      }
      if (current !== output) output.set(current);
      return output;
    },
    finiteArray = (value, count) => {
      if (!ArrayBuffer.isView(value) && !Array.isArray(value)) return null;
      if (value.length < count) return null;
      const output = new Float32Array(count);
      for (let i = 0; i < count; i++) {
        const number = Number(value[i]);
        if (!Number.isFinite(number)) return null;
        output[i] = number;
      }
      return output;
    },
    shaderText = (source) => String(source).replace(
      /\/\*[\s\S]*?\*\/|\/\/[^\n\r]*/g, " ",
    ),
    declarations = (source, keyword) => {
      const found = [], expression = new RegExp(
        "\\b" + keyword + "\\s+(?:(?:lowp|mediump|highp)\\s+)?" +
        "(float|vec[234]|mat[234]|sampler2D)\\s+" +
        "([A-Za-z_][A-Za-z0-9_]*)\\s*(?:\\[\\s*([0-9]+)\\s*\\])?\\s*;", "g",
      );
      let match, excess = false;
      while ((match = expression.exec(source))) {
        const size = match[3] === undefined ? 1 : Number(match[3]);
        if (!Number.isSafeInteger(size) || size < 1
            || found.length >= MAX_INTERFACE_DECLARATIONS) {
          excess = true;
          continue;
        }
        found.push({ type: match[1], name: match[2], size });
      }
      return { entries: found, excess };
    },
    uniformVectors = (entries) => {
      let vectors = 0;
      for (const entry of entries) {
        const columns = entry.type === "mat4" ? 4
          : entry.type === "mat3" ? 3 : entry.type === "mat2" ? 2 : 1;
        const consumed = columns * entry.size;
        if (!Number.isSafeInteger(consumed) || consumed > 16 - vectors)
          return 17;
        vectors += consumed;
      }
      return vectors;
    },
    glTypeFor = (name) => ({
      float: constants.FLOAT, vec2: constants.FLOAT_VEC2,
      vec3: constants.FLOAT_VEC3, vec4: constants.FLOAT_VEC4,
      mat2: constants.FLOAT_MAT2, mat3: constants.FLOAT_MAT3,
      mat4: constants.FLOAT_MAT4, sampler2D: constants.SAMPLER_2D,
    })[name] || constants.FLOAT,
    activeContexts = () => {
      const retained = [], active = [];
      for (const reference of liveContexts) {
        const context = typeof reference?.deref === "function"
          ? reference.deref() : reference;
        if (!context || !context.canvas) continue;
        retained.push(reference);
        if (context._nativeReleasePending)
          context._releaseNativeSurface();
        if (context._restorePending && context._restoreAuthorized
            && !context._lossDispatching) context._restoreAfterLoss();
        /* A canceled loss reserves its existing slot until restoration.  A
           terminally lost object remains reachable through its canvas but no
           longer consumes one of the realm's two active-context slots. */
        if (context._lost && !context._restorePending) continue;
        active.push(context);
      }
      liveContexts.length = 0;
      liveContexts.push(...retained);
      return active;
    },
    contextForCanvas = (canvas) => {
      const reference = contexts.get(canvas);
      return typeof reference?.deref === "function"
        ? reference.deref() || null : reference || null;
    };

  const makeCommandAttribute = () => ({
      data: null, size: 0, type: 0, normalized: false, stride: 0, offset: 0,
    }),
    makeCommandIndex = () => ({ data: null, type: 0, offset: 0, maximum: 0 }),
    makeCommandRecord = () => ({
      _pooled: true, _inUse: false, kind: "clear", mask: 0,
      clearColor: new Float32Array(4), clearDepth: 1,
      scissorEnabled: false, scissor: new Int32Array(4),
      mode: 0, first: 0, count: 0,
      _indexStorage: makeCommandIndex(), index: null,
      _positionStorage: makeCommandAttribute(), position: null,
      _colorStorage: makeCommandAttribute(), color: null,
      _texcoordStorage: makeCommandAttribute(), texcoord: null,
      texture: null, matrix: new Float32Array(16),
      uniformColor: new Float32Array(4), blend: false,
      depth: false, cull: false, depthFunc: 0,
      blendSrc: 0, blendDst: 0, cullFace: 0, frontFace: 0,
      viewport: new Int32Array(4),
    });

  class GLObject {
    constructor(owner, id) {
      this._owner = owner;
      this._id = id;
      this._deleted = false;
    }
  }
  class WebGLBuffer extends GLObject {
    constructor(owner, id) {
      super(owner, id); this._data = new Uint8Array();
      this._usage = constants.STATIC_DRAW;
      this._indexType = 0; this._indexOffset = -1;
      this._indexCount = -1; this._indexMaximum = 0;
    }
  }
  class WebGLTexture extends GLObject {
    constructor(owner, id) {
      super(owner, id); this._width = 0; this._height = 0;
      this._pixels = null; this._generation = 1;
      this._min = constants.LINEAR; this._mag = constants.LINEAR;
      this._wrapS = constants.CLAMP_TO_EDGE;
      this._wrapT = constants.CLAMP_TO_EDGE;
    }
  }
  class WebGLShader extends GLObject {
    constructor(owner, id, type) {
      super(owner, id); this._type = type; this._source = "";
      this._compiled = false; this._log = "";
    }
  }
  class WebGLProgram extends GLObject {
    constructor(owner, id) {
      super(owner, id); this._shaders = []; this._linked = false;
      this._log = ""; this._attributes = []; this._uniforms = [];
      this._uniformValues = new Map(); this._translation = null;
    }
  }
  class WebGLUniformLocation {
    constructor(program, entry) {
      this._program = program; this._name = entry.name;
      this._type = entry.type; this._size = entry.size;
    }
  }
  class WebGLActiveInfo {
    constructor(size, type, name) { this.size = size; this.type = type; this.name = name; }
  }
  class WebGLShaderPrecisionFormat {
    constructor(rangeMin, rangeMax, precision) {
      this.rangeMin = rangeMin; this.rangeMax = rangeMax;
      this.precision = precision;
    }
  }
  class WebGLFramebuffer extends GLObject {}
  class WebGLRenderbuffer extends GLObject {}

  const owned = (context, value, Type) =>
    value instanceof Type && value._owner === context && !value._deleted,
    attributeRole = (attributes, source, role) => {
      const pattern = role === "position" ? /position|vertex|coord/i
        : role === "color" ? /colou?r|tint/i : /tex|uv/i;
      const candidates = attributes.filter((entry) => pattern.test(entry.name));
      if (candidates.length) return candidates[0];
      if (role === "position") {
        const expression = /gl_Position\s*=([^;]+)/.exec(source)?.[1] || "";
        return attributes.find((entry) =>
          new RegExp("\\b" + entry.name + "\\b").test(expression)) || attributes[0];
      }
      return null;
    },
    translateProgram = (program) => {
      const vertex = program._shaders.find((shader) =>
          shader._type === constants.VERTEX_SHADER),
        fragment = program._shaders.find((shader) =>
          shader._type === constants.FRAGMENT_SHADER);
      if (!vertex || !fragment || !vertex._compiled || !fragment._compiled)
        return { error: "A compiled vertex and fragment shader are required" };
      const vs = shaderText(vertex._source), fs = shaderText(fragment._source),
        attributeScan = declarations(vs, "attribute"),
        vertexUniformScan = declarations(vs, "uniform"),
        fragmentUniformScan = declarations(fs, "uniform"),
        attributes = attributeScan.entries,
        uniforms = [...vertexUniformScan.entries, ...fragmentUniformScan.entries]
          .filter((entry, index, all) =>
            all.findIndex((candidate) => candidate.name === entry.name) === index);
      const conflictingUniform = [...vertexUniformScan.entries,
        ...fragmentUniformScan.entries].some((entry, index, all) => {
          const prior = all.findIndex((candidate) => candidate.name === entry.name);
          return prior !== index && (all[prior].type !== entry.type
            || all[prior].size !== entry.size);
        });
      if (attributeScan.excess || vertexUniformScan.excess
          || fragmentUniformScan.excess || attributes.length > 8
          || attributes.some((entry) => entry.size !== 1)
          || uniformVectors(vertexUniformScan.entries) > 16
          || uniformVectors(fragmentUniformScan.entries) > 4
          || conflictingUniform)
        return { error: "Shader interface exceeds the PSP WebGL limits" };
      const positionExpression = /\bgl_Position\s*=\s*([^;]+)/.exec(vs)?.[1] || "",
        position = attributeRole(attributes, vs, "position"),
        color = attributeRole(attributes, vs, "color"),
        texcoord = attributeRole(attributes, vs, "texture"),
        matrices = uniforms.filter((entry) => entry.type === "mat4" &&
          entry.size === 1 &&
          new RegExp("\\b" + entry.name + "\\b").test(positionExpression))
          .sort((left, right) => positionExpression.indexOf(left.name) -
            positionExpression.indexOf(right.name)),
        sampler = uniforms.find((entry) => entry.type === "sampler2D") || null,
        uniformColor = uniforms.find((entry) => entry.type === "vec4" &&
          /colou?r|tint/i.test(entry.name)) || null,
        usesTexture = /\btexture2D\s*\(/.test(fs);
      if (!position || !/\bgl_Position\s*=/.test(vs)
          || !/\bgl_FragColor\s*=/.test(fs))
        return { error: "Shader does not expose a fixed-function position/color output" };
      if (matrices.length > 4)
        return { error: "Position transform exceeds the four-matrix PSP limit" };
      if (/\b(discard|dFdx|dFdy|fwidth)\b/.test(fs)
          || /\b(for|while|do)\s*\(/.test(vs + "\n" + fs))
        return { error: "Dynamic shader control flow is not available on the PSP GE" };
      if (usesTexture && (!sampler || !texcoord))
        return { error: "Texture shaders require a sampler and texture-coordinate attribute" };
      return { attributes, uniforms, position, color, texcoord, matrices,
        sampler, uniformColor, usesTexture };
    };

  class WebGLRenderingContext {
    constructor(canvas, attributes = {}) {
      this.canvas = canvas;
      this._attributes = {
        /* PSP 8888 alpha is the GE stencil plane. Keep the page canvas
           explicitly opaque instead of exposing a transparent-buffer claim
           the fixed-function backend cannot preserve across every draw. */
        alpha: false,
        depth: attributes?.depth !== false,
        stencil: false,
        antialias: false,
        premultipliedAlpha: attributes?.premultipliedAlpha !== false,
        preserveDrawingBuffer: true,
        powerPreference: "low-power",
        failIfMajorPerformanceCaveat: false,
      };
      this._nextId = 1; this._errors = 0; this._contextLostPending = false;
      this._lost = false; this._restorePending = false;
      this._restoreAuthorized = false; this._lossDispatching = false;
      this._nativeReleasePending = false;
      this._bufferBytes = 0; this._textureBytes = 0;
      this._bufferCount = 0; this._textureCount = 0;
      this._shaderCount = 0; this._programCount = 0;
      this._framebufferCount = 0; this._renderbufferCount = 0;
      this._arrayBuffer = null; this._elementBuffer = null;
      this._texture = null; this._program = null;
      this._attributesState = Array.from({ length: 8 }, () => ({
        enabled: false, buffer: null, size: 4, type: constants.FLOAT,
        normalized: false, stride: 0, offset: 0, constant: [0, 0, 0, 1],
      }));
      this._enabled = new Set([constants.DITHER]);
      this._clearColor = [0, 0, 0, 0]; this._clearDepth = 1;
      this._viewport = [0, 0, this.drawingBufferWidth, this.drawingBufferHeight];
      this._scissor = [0, 0, this.drawingBufferWidth, this.drawingBufferHeight];
      this._depthFunc = constants.LESS;
      this._blendSrc = constants.ONE; this._blendDst = constants.ZERO;
      this._cullFace = constants.BACK; this._frontFace = constants.CCW;
      this._lineWidth = 1; this._flipY = false; this._premultiply = false;
      this._unpackAlignment = 4; this._packAlignment = 4;
      this._commands = []; this._queuedVertices = 0; this._flushQueued = false;
      this._forcedFlushesInFrame = 0;
      this._queuedSources = new Set(); this._queuedTextures = new Set();
      this._tailCommands = []; this._tailVertices = 0;
      this._tailSources = new Set(); this._tailTextures = new Set();
      /* Every queued draw owns one immutable record until publication.  The
         staging record and matrix scratch are reusable because JavaScript is
         single-threaded; only the copied pool record enters either queue. */
      this._commandPool = Array.from(
        { length: MAX_DRAWS }, () => makeCommandRecord());
      this._stagingCommand = makeCommandRecord();
      this._stagingCommand._pooled = false;
      this._matrixScratch = new Float32Array(16);
      this._drawIndex = makeCommandIndex();
      /* The packed 32-bit wire is retained per context. Flushes overwrite
         this bounded storage instead of allocating a Float64 command block. */
      this._commandWireBuffer = new ArrayBuffer(
        (WIRE_HEADER_WORDS + MAX_DRAWS * COMMAND_WORDS) * 4);
      this._commandWireU32 = new Uint32Array(this._commandWireBuffer);
      this._commandWireI32 = new Int32Array(this._commandWireBuffer);
      this._commandWireF32 = new Float32Array(this._commandWireBuffer);
      this._textureWireBuffer = new ArrayBuffer(
        (WIRE_HEADER_WORDS + MAX_TEXTURE_OBJECTS * TEXTURE_WORDS) * 4);
      this._textureWireU32 = new Uint32Array(this._textureWireBuffer);
      this._textureWireI32 = new Int32Array(this._textureWireBuffer);
      this._resources = new Set();
      this._surfaceReady = false;
      const drawingSize = boundedDrawingSize(canvas.width, canvas.height);
      this._drawingWidth = drawingSize?.width || 0;
      this._drawingHeight = drawingSize?.height || 0;
    }
    get drawingBufferWidth() { return this._drawingWidth; }
    get drawingBufferHeight() { return this._drawingHeight; }
    _setError(error) {
      if (this._lost) return;
      const index = [constants.INVALID_ENUM, constants.INVALID_VALUE,
        constants.INVALID_OPERATION, constants.OUT_OF_MEMORY].indexOf(error);
      if (index >= 0) this._errors |= 1 << index;
    }
    _object(value, Type) {
      if (this._lost) return false;
      if (value === null) return null;
      if (!owned(this, value, Type)) { this._setError(constants.INVALID_OPERATION); return false; }
      return value;
    }
    _lose(reason) {
      if (this._lost) return;
      this._lost = true; this._restorePending = true;
      this._restoreAuthorized = false; this._lossDispatching = true;
      this._resetCommandQueue();
      for (const resource of this._resources) {
        if (resource instanceof WebGLBuffer)
          resource._data = new Uint8Array();
        else if (resource instanceof WebGLTexture) {
          resource._pixels = null; resource._commandSnapshot = null;
        } else if (resource instanceof WebGLShader) {
          resource._source = ""; resource._compiled = false;
        } else if (resource instanceof WebGLProgram) {
          resource._shaders.length = 0; resource._uniformValues.clear();
          resource._translation = null; resource._linked = false;
        }
        resource._deleted = true;
      }
      this._resources.clear();
      this._bufferBytes = 0; this._textureBytes = 0;
      this._bufferCount = 0; this._textureCount = 0;
      this._shaderCount = 0; this._programCount = 0;
      this._framebufferCount = 0; this._renderbufferCount = 0;
      this._arrayBuffer = null; this._elementBuffer = null;
      this._texture = null; this._program = null;
      for (const attribute of this._attributesState) {
        attribute.enabled = false; attribute.buffer = null;
      }
      this._contextLostPending = true;
      diagnostics.lostContexts++; diagnostics.nativeFailures++;
      this._nativeReleasePending = !this._releaseNativeSurface();
      const event = new Event("webglcontextlost", { cancelable: true });
      Object.defineProperty(event, "statusMessage", { value: String(reason || "") });
      const canceled = this.canvas?.dispatchEvent(event) === false;
      this._lossDispatching = false;
      if (!canceled) {
        this._restorePending = false;
        return;
      }
      this._restoreAuthorized = true;
      queueMicrotask(() => this._restoreAfterLoss());
    }
    _releaseNativeSurface() {
      const canvas = this.canvas;
      if (!canvas || typeof releaseNativeWebGLSurface !== "function")
        return false;
      const released = releaseNativeWebGLSurface(canvas.__handle) === true;
      if (released) this._nativeReleasePending = false;
      return released;
    }
    _restoreAfterLoss() {
      if (!this._lost || !this._restorePending || !this.canvas) {
        this._restorePending = false; this._restoreAuthorized = false;
        return;
      }
      if (!this._restoreAuthorized || this._lossDispatching) return;
      if (this._nativeReleasePending && !this._releaseNativeSurface()) return;
      this._restorePending = false; this._restoreAuthorized = false;
      this._lost = false;
      this._nextId = 1; this._errors = 0;
      this._bufferBytes = 0; this._textureBytes = 0;
      this._bufferCount = 0; this._textureCount = 0;
      this._shaderCount = 0; this._programCount = 0;
      this._framebufferCount = 0; this._renderbufferCount = 0;
      this._arrayBuffer = null; this._elementBuffer = null;
      this._texture = null; this._program = null;
      for (const attribute of this._attributesState) {
        attribute.enabled = false; attribute.buffer = null;
        attribute.size = 4; attribute.type = constants.FLOAT;
        attribute.normalized = false; attribute.stride = 0;
        attribute.offset = 0; attribute.constant = [0, 0, 0, 1];
      }
      this._enabled = new Set([constants.DITHER]);
      this._clearColor = [0, 0, 0, 0]; this._clearDepth = 1;
      this._depthFunc = constants.LESS;
      this._blendSrc = constants.ONE; this._blendDst = constants.ZERO;
      this._cullFace = constants.BACK; this._frontFace = constants.CCW;
      this._lineWidth = 1; this._flipY = false; this._premultiply = false;
      this._unpackAlignment = 4; this._packAlignment = 4;
      this._surfaceReady = false;
      this._resize();
      if (!this._lost)
        this.canvas.dispatchEvent(new Event("webglcontextrestored"));
    }
    _schedule() {
      if (this._flushQueued || this._lost) return;
      this._flushQueued = true;
      queueMicrotask(() => {
        this._flushQueued = false; this._flush();
        this._forcedFlushesInFrame = 0;
      });
    }
    _resetCommandQueue() {
      for (const command of this._commands) this._releaseCommand(command);
      for (const command of this._tailCommands) this._releaseCommand(command);
      this._commands.length = 0; this._queuedVertices = 0;
      this._queuedSources.clear(); this._queuedTextures.clear();
      this._tailCommands.length = 0; this._tailVertices = 0;
      this._tailSources.clear(); this._tailTextures.clear();
    }
    _resetCurrentQueue() {
      for (const command of this._commands) this._releaseCommand(command);
      this._commands.length = 0; this._queuedVertices = 0;
      this._queuedSources.clear(); this._queuedTextures.clear();
    }
    _promoteTailQueue() {
      this._commands = this._tailCommands; this._tailCommands = [];
      this._queuedVertices = this._tailVertices; this._tailVertices = 0;
      this._queuedSources = this._tailSources; this._tailSources = new Set();
      this._queuedTextures = this._tailTextures; this._tailTextures = new Set();
    }
    _releaseCommand(command) {
      if (!command?._pooled) return;
      command._inUse = false;
      command.index = null; command.position = null;
      command.color = null; command.texcoord = null;
      command.texture = null;
      command._indexStorage.data = null;
      command._positionStorage.data = null;
      command._colorStorage.data = null;
      command._texcoordStorage.data = null;
    }
    _copyCommandAttribute(destination, source) {
      if (!source) { destination.data = null; return null; }
      destination.data = source.data; destination.size = source.size;
      destination.type = source.type; destination.normalized = !!source.normalized;
      destination.stride = source.stride; destination.offset = source.offset;
      return destination;
    }
    _acquireCommand(source) {
      const destination = this._commandPool.find((entry) => !entry._inUse);
      if (!destination) return null;
      destination._inUse = true; destination.kind = source.kind;
      destination.scissorEnabled = !!source.scissorEnabled;
      destination.scissor.set(source.scissor || [0, 0, 0, 0]);
      if (source.kind === "clear") {
        destination.mask = source.mask;
        destination.clearColor.set(source.clearColor || source.color);
        destination.clearDepth = source.clearDepth ?? source.depth;
        return destination;
      }
      destination.mode = source.mode; destination.first = source.first;
      destination.count = source.count;
      if (source.index) {
        destination._indexStorage.data = source.index.data;
        destination._indexStorage.type = source.index.type;
        destination._indexStorage.offset = source.index.offset;
        destination._indexStorage.maximum = source.index.maximum;
        destination.index = destination._indexStorage;
      } else destination.index = null;
      destination.position = this._copyCommandAttribute(
        destination._positionStorage, source.position);
      destination.color = this._copyCommandAttribute(
        destination._colorStorage, source.color);
      destination.texcoord = this._copyCommandAttribute(
        destination._texcoordStorage, source.texcoord);
      destination.texture = source.texture;
      destination.matrix.set(source.matrix);
      destination.uniformColor.set(source.uniformColor);
      destination.blend = !!source.blend; destination.depth = !!source.depth;
      destination.cull = !!source.cull; destination.depthFunc = source.depthFunc;
      destination.blendSrc = source.blendSrc;
      destination.blendDst = source.blendDst;
      destination.cullFace = source.cullFace;
      destination.frontFace = source.frontFace;
      destination.viewport.set(source.viewport);
      return destination;
    }
    _queueFits(commands, queuedVertices, queuedSources, queuedTextures,
               candidateSources, texture, vertices) {
      let addedSources = 0;
      for (let at = 0; at < candidateSources.length; at++) {
        const source = candidateSources[at];
        if (!source || queuedSources.has(source)) continue;
        let repeated = false;
        for (let prior = 0; prior < at; prior++)
          if (candidateSources[prior] === source) { repeated = true; break; }
        if (!repeated) addedSources++;
      }
      const addedTexture = texture && !queuedTextures.has(texture) ? 1 : 0;
      return commands.length < MAX_DRAWS
        && vertices <= MAX_VERTICES - queuedVertices
        && addedSources <= MAX_NATIVE_SOURCES - queuedSources.size
        && addedTexture <= MAX_TEXTURE_OBJECTS - queuedTextures.size;
    }
    _enqueueCommand(command, candidateSources = [], texture = null,
                    vertices = 0) {
      /* Once a tail exists, every newer command belongs behind it even when
         that command would fit in the older batch. Otherwise a source-heavy
         deferred draw followed by a clear could execute in reverse order. */
      let tail = this._tailCommands.length !== 0;
      if (!tail && !this._queueFits(
          this._commands, this._queuedVertices, this._queuedSources,
          this._queuedTextures, candidateSources, texture, vertices)) {
        const flushed = this._flush();
        if (flushed === FLUSH_FAILED) return false;
        tail = flushed === FLUSH_DEFERRED;
      }
      const commands = tail ? this._tailCommands : this._commands,
        queuedVertices = tail ? this._tailVertices : this._queuedVertices,
        queuedSources = tail ? this._tailSources : this._queuedSources,
        queuedTextures = tail ? this._tailTextures : this._queuedTextures;
      if (!this._queueFits(commands, queuedVertices, queuedSources,
          queuedTextures, candidateSources, texture, vertices)) {
        /* Both bounded batches are genuinely occupied. This is resource
           exhaustion, unlike the ordinary detached-bridge DEFERRED result. */
        this._setError(constants.OUT_OF_MEMORY); return false;
      }
      const queuedCommand = this._acquireCommand(command);
      if (!queuedCommand) {
        this._setError(constants.OUT_OF_MEMORY); return false;
      }
      commands.push(queuedCommand);
      for (const source of candidateSources) if (source) queuedSources.add(source);
      if (texture) queuedTextures.add(texture);
      if (tail) this._tailVertices += vertices;
      else this._queuedVertices += vertices;
      return true;
    }
    _sourceIndex(sources, sourceMap, bytes) {
      if (!bytes) return -1;
      let index = sourceMap.get(bytes);
      if (index === undefined) {
        index = sources.length; sourceMap.set(bytes, index); sources.push(bytes);
      }
      return index;
    }
    _flushCurrent() {
      if (this._lost) return FLUSH_FAILED;
      if (!this._commands.length) return FLUSH_OK;
      const width = this.drawingBufferWidth, height = this.drawingBufferHeight;
      if (!width || !height) {
        this._resetCommandQueue();
        this._surfaceReady = false;
        return FLUSH_OK;
      }
      if (width * height > MAX_DRAWING_PIXELS) {
        this._lose("drawing buffer exceeds the bounded PSP surface");
        return FLUSH_FAILED;
      }
      const sources = [], sourceMap = new Map(), textures = [], textureMap = new Map(),
        wireU32 = this._commandWireU32,
        wireI32 = this._commandWireI32,
        wireF32 = this._commandWireF32;
      wireU32[0] = COMMAND_WIRE_MAGIC; wireU32[1] = WIRE_VERSION;
      wireU32[2] = COMMAND_WORDS; wireU32[3] = this._commands.length;
      let commandAt = 0;
      for (const command of this._commands) {
        const base = WIRE_HEADER_WORDS + commandAt++ * COMMAND_WORDS;
        wireU32.fill(0, base, base + COMMAND_WORDS);
        if (command.kind === "clear") {
          wireI32[base] = 0; wireU32[base + 1] = command.mask;
          wireF32.set(command.clearColor || command.color, base + 2);
          wireF32[base + 6] = command.clearDepth ?? command.depth;
          wireI32.set(command.scissor, base + 40);
          wireU32[base + 39] = command.scissorEnabled ? 1 : 0;
          continue;
        }
        const draw = command, position = draw.position,
          color = draw.color, texcoord = draw.texcoord,
          index = draw.index;
        wireI32[base] = 1; wireI32[base + 1] = draw.mode;
        wireU32[base + 2] = draw.first; wireU32[base + 3] = draw.count;
        wireU32[base + 4] = index ? 1 : 0;
        wireI32[base + 5] = this._sourceIndex(sources, sourceMap, index?.data);
        wireI32[base + 6] = index?.type || 0;
        wireU32[base + 7] = index?.offset || 0;
        for (const [slot, attribute] of [[8, position], [14, color], [20, texcoord]]) {
          wireI32[base + slot] = this._sourceIndex(
            sources, sourceMap, attribute?.data);
          wireI32[base + slot + 1] = attribute?.size || 0;
          wireI32[base + slot + 2] = attribute?.type || 0;
          wireU32[base + slot + 3] = attribute?.normalized ? 1 : 0;
          wireU32[base + slot + 4] = attribute?.stride || 0;
          wireU32[base + slot + 5] = attribute?.offset || 0;
        }
        let textureIndex = -1;
        if (draw.texture) {
          textureIndex = textureMap.get(draw.texture);
          if (textureIndex === undefined) {
            textureIndex = textures.length; textureMap.set(draw.texture, textureIndex);
            textures.push(draw.texture);
          }
        }
        wireI32[base + 26] = textureIndex;
        wireU32[base + 27] = draw.blend ? 1 : 0;
        wireU32[base + 28] = draw.depth ? 1 : 0;
        wireU32[base + 29] = draw.cull ? 1 : 0;
        wireI32[base + 30] = draw.depthFunc;
        wireI32[base + 31] = draw.blendSrc;
        wireI32[base + 32] = draw.blendDst;
        wireI32[base + 33] = draw.cullFace;
        wireI32[base + 34] = draw.frontFace;
        wireI32.set(draw.viewport, base + 35);
        wireU32[base + 39] = draw.scissorEnabled ? 1 : 0;
        wireI32.set(draw.scissor, base + 40);
        wireF32.set(draw.matrix, base + 44);
        wireF32.set(draw.uniformColor, base + 60);
      }
      const textureWire = this._textureWireI32;
      this._textureWireU32[0] = TEXTURE_WIRE_MAGIC;
      this._textureWireU32[1] = WIRE_VERSION;
      this._textureWireU32[2] = TEXTURE_WORDS;
      this._textureWireU32[3] = textures.length;
      for (let index = 0; index < textures.length; index++) {
        const texture = textures[index],
          base = WIRE_HEADER_WORDS + index * TEXTURE_WORDS;
        textureWire[base] = texture._id;
        textureWire[base + 1] = texture._generation;
        textureWire[base + 2] = texture._width;
        textureWire[base + 3] = texture._height;
        textureWire[base + 4] = this._sourceIndex(
          sources, sourceMap, texture._pixels);
        textureWire[base + 5] = texture._min;
        textureWire[base + 6] = texture._mag;
        textureWire[base + 7] =
          texture._wrapS === constants.REPEAT ? 1 : 0;
        textureWire[base + 8] =
          texture._wrapT === constants.REPEAT ? 1 : 0;
      }
      const canvas = this.canvas;
      if (!canvas) {
        this._lose("canvas was collected");
        return FLUSH_FAILED;
      }
      const outcome = __tilefinchWebGLRender(
        canvas.__handle, width, height, wireU32, sources,
        this._textureWireU32,
      );
      if (outcome === 2) {
        diagnostics.deferredFlushes++;
        return FLUSH_DEFERRED;
      }
      this._resetCurrentQueue();
      if (outcome !== 1) {
        this._lose("native WebGL command admission failed");
        return FLUSH_FAILED;
      }
      diagnostics.flushes++; this._surfaceReady = true;
      return FLUSH_OK;
    }
    _flush() {
      if (!this._commands.length && this._tailCommands.length)
        this._promoteTailQueue();
      while (this._commands.length) {
        const outcome = this._flushCurrent();
        if (outcome !== FLUSH_OK) return outcome;
        if (this._tailCommands.length) this._promoteTailQueue();
      }
      return FLUSH_OK;
    }
    _flushForMutation() {
      if (!this._commands.length) return FLUSH_OK;
      diagnostics.forcedFlushes++;
      this._forcedFlushesInFrame++;
      diagnostics.maximumForcedFlushesPerFrame = Math.max(
        diagnostics.maximumForcedFlushesPerFrame,
        this._forcedFlushesInFrame);
      return this._flush();
    }
    _resize() {
      this._resetCommandQueue();
      const canvas = this.canvas,
        drawingSize = canvas ? boundedDrawingSize(canvas.width, canvas.height) : null;
      if (!drawingSize) {
        this._lose("invalid drawing buffer dimensions"); return;
      }
      this._drawingWidth = drawingSize.width;
      this._drawingHeight = drawingSize.height;
      this._viewport = [0, 0, this.drawingBufferWidth, this.drawingBufferHeight];
      this._scissor = [...this._viewport]; this._surfaceReady = false;
      this.clear(constants.COLOR_BUFFER_BIT |
        (this._attributes.depth ? constants.DEPTH_BUFFER_BIT : 0));
    }
    getContextAttributes() { return { ...this._attributes }; }
    isContextLost() { return this._lost; }
    getError() {
      if (this._contextLostPending) {
        this._contextLostPending = false;
        return constants.CONTEXT_LOST_WEBGL;
      }
      const order = [constants.INVALID_ENUM, constants.INVALID_VALUE,
        constants.INVALID_OPERATION, constants.OUT_OF_MEMORY];
      for (let index = 0; index < order.length; index++) {
        if (this._errors & (1 << index)) {
          this._errors &= ~(1 << index);
          return order[index];
        }
      }
      return constants.NO_ERROR;
    }
    createBuffer() {
      if (this._lost) return null;
      if (this._bufferCount >= MAX_BUFFER_OBJECTS) { this._setError(constants.OUT_OF_MEMORY); return null; }
      const buffer = new WebGLBuffer(this, this._nextId++);
      this._bufferCount++; this._resources.add(buffer); return buffer;
    }
    deleteBuffer(buffer) {
      if (!owned(this, buffer, WebGLBuffer)) return;
      if (this._flushForMutation() === FLUSH_FAILED) return;
      if (this._arrayBuffer === buffer) this._arrayBuffer = null;
      if (this._elementBuffer === buffer) this._elementBuffer = null;
      this._bufferBytes -= buffer._data.byteLength; buffer._data = new Uint8Array();
      buffer._deleted = true; this._bufferCount--; this._resources.delete(buffer);
    }
    isBuffer(buffer) { return owned(this, buffer, WebGLBuffer); }
    bindBuffer(target, buffer) {
      if (![constants.ARRAY_BUFFER, constants.ELEMENT_ARRAY_BUFFER].includes(target)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      const admitted = this._object(buffer, WebGLBuffer);
      if (admitted === false) return;
      if (target === constants.ARRAY_BUFFER) this._arrayBuffer = admitted;
      else this._elementBuffer = admitted;
    }
    bufferData(target, value, usage) {
      const buffer = target === constants.ARRAY_BUFFER ? this._arrayBuffer
        : target === constants.ELEMENT_ARRAY_BUFFER ? this._elementBuffer : false;
      if (buffer === false) { this._setError(constants.INVALID_ENUM); return; }
      if (!buffer) { this._setError(constants.INVALID_OPERATION); return; }
      if (![constants.STATIC_DRAW, constants.DYNAMIC_DRAW, constants.STREAM_DRAW].includes(usage)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      if (this._flushForMutation() === FLUSH_FAILED) return;
      let bytes;
      if (typeof value === "number") {
        const size = Number(value);
        if (!Number.isSafeInteger(size) || size < 0) { this._setError(constants.INVALID_VALUE); return; }
        bytes = new Uint8Array(size);
      } else if (ArrayBuffer.isView(value))
        bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
      else if (value instanceof ArrayBuffer) bytes = new Uint8Array(value).slice();
      else { this._setError(constants.INVALID_VALUE); return; }
      const retained = this._bufferBytes - buffer._data.byteLength;
      if (bytes.byteLength > MAX_BUFFER_BYTES - retained) {
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      this._bufferBytes = retained + bytes.byteLength;
      buffer._data = bytes; buffer._usage = usage;
      buffer._indexCount = -1;
    }
    bufferSubData(target, offset, value) {
      const buffer = target === constants.ARRAY_BUFFER ? this._arrayBuffer
        : target === constants.ELEMENT_ARRAY_BUFFER ? this._elementBuffer : false;
      if (!buffer) { this._setError(buffer === false ? constants.INVALID_ENUM : constants.INVALID_OPERATION); return; }
      if (!Number.isSafeInteger(offset) || offset < 0 || !ArrayBuffer.isView(value)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      if (offset > buffer._data.byteLength || bytes.byteLength > buffer._data.byteLength - offset) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (flushOutcome === FLUSH_DEFERRED) buffer._data = buffer._data.slice();
      buffer._data.set(bytes, offset);
      buffer._indexCount = -1;
    }
    getBufferParameter(target, parameter) {
      const buffer = target === constants.ARRAY_BUFFER ? this._arrayBuffer
        : target === constants.ELEMENT_ARRAY_BUFFER ? this._elementBuffer : null;
      if (!buffer) { this._setError(constants.INVALID_OPERATION); return null; }
      if (parameter === constants.BUFFER_SIZE) return buffer._data.byteLength;
      if (parameter === constants.BUFFER_USAGE) return buffer._usage;
      this._setError(constants.INVALID_ENUM); return null;
    }
    createShader(type) {
      if (this._lost) return null;
      if (![constants.VERTEX_SHADER, constants.FRAGMENT_SHADER].includes(type)) {
        this._setError(constants.INVALID_ENUM); return null;
      }
      if (this._shaderCount >= MAX_SHADER_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const shader = new WebGLShader(this, this._nextId++, type);
      this._shaderCount++; this._resources.add(shader); return shader;
    }
    shaderSource(shader, source) {
      if (!owned(this, shader, WebGLShader)) { this._setError(constants.INVALID_OPERATION); return; }
      source = String(source);
      shader._source = source.length <= MAX_SHADER_BYTES ? source : "";
      shader._compiled = false;
      shader._log = source.length <= MAX_SHADER_BYTES ? "" : "Shader source exceeds 16 KiB";
    }
    getShaderSource(shader) { return owned(this, shader, WebGLShader) ? shader._source : null; }
    compileShader(shader) {
      if (!owned(this, shader, WebGLShader)) { this._setError(constants.INVALID_OPERATION); return; }
      const source = shaderText(shader._source);
      shader._compiled = !!source && /\bvoid\s+main\s*\(/.test(source)
        && (shader._type === constants.VERTEX_SHADER
          ? /\bgl_Position\b/.test(source) : /\bgl_FragColor\b/.test(source));
      shader._log = shader._compiled ? "" : shader._log || "Missing bounded shader entry/output";
    }
    getShaderParameter(shader, parameter) {
      if (!owned(this, shader, WebGLShader)) return null;
      if (parameter === constants.COMPILE_STATUS) return shader._compiled;
      if (parameter === constants.DELETE_STATUS) return shader._deleted;
      if (parameter === constants.SHADER_TYPE) return shader._type;
      this._setError(constants.INVALID_ENUM); return null;
    }
    getShaderInfoLog(shader) { return owned(this, shader, WebGLShader) ? shader._log : null; }
    deleteShader(shader) {
      if (owned(this, shader, WebGLShader)) {
        shader._deleted = true; this._shaderCount--;
        this._resources.delete(shader);
      }
    }
    isShader(shader) { return owned(this, shader, WebGLShader); }
    createProgram() {
      if (this._lost) return null;
      if (this._programCount >= MAX_PROGRAM_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const program = new WebGLProgram(this, this._nextId++);
      this._programCount++; this._resources.add(program); return program;
    }
    attachShader(program, shader) {
      if (!owned(this, program, WebGLProgram) || !owned(this, shader, WebGLShader)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (program._shaders.includes(shader)) return;
      /* The translated profile needs exactly one shader of each kind. Keeping
         deleted-but-attached shaders alive is WebGL behavior, but it must not
         turn create/attach/delete into an unbounded program-owned array. */
      if (program._shaders.length >= 2
          || program._shaders.some((entry) => entry._type === shader._type)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      program._shaders.push(shader);
    }
    detachShader(program, shader) {
      if (!owned(this, program, WebGLProgram) || !owned(this, shader, WebGLShader)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      program._shaders = program._shaders.filter((item) => item !== shader);
    }
    getAttachedShaders(program) {
      return owned(this, program, WebGLProgram) ? [...program._shaders] : null;
    }
    bindAttribLocation(program, index, name) {
      if (!owned(this, program, WebGLProgram) || !Number.isInteger(index)
          || index < 0 || index >= 8) { this._setError(constants.INVALID_VALUE); return; }
      program._boundAttributes ||= new Map();
      const attributeName = String(name);
      if (!program._boundAttributes.has(attributeName) &&
          program._boundAttributes.size >= 8) {
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      program._boundAttributes.set(attributeName, index);
    }
    linkProgram(program) {
      if (!owned(this, program, WebGLProgram)) { this._setError(constants.INVALID_OPERATION); return; }
      const translation = translateProgram(program);
      if (translation.error) {
        program._linked = false; program._log = translation.error;
        diagnostics.shaderRefusals++; return;
      }
      const used = new Set();
      program._attributes = translation.attributes.map((entry, natural) => {
        let location = program._boundAttributes?.get(entry.name);
        if (!Number.isInteger(location) || location < 0 || location >= 8 || used.has(location)) {
          location = 0; while (used.has(location)) location++;
        }
        used.add(location); return { ...entry, location };
      });
      program._uniforms = translation.uniforms;
      program._translation = translation; program._linked = true; program._log = "";
      for (const uniform of program._uniforms) {
        if (uniform.type === "mat4") program._uniformValues.set(uniform.name, identityMatrix());
        else if (uniform.type === "vec4") program._uniformValues.set(uniform.name, new Float32Array([1, 1, 1, 1]));
        else program._uniformValues.set(uniform.name, 0);
      }
    }
    validateProgram(program) { if (owned(this, program, WebGLProgram)) program._validated = program._linked; }
    useProgram(program) {
      const admitted = this._object(program, WebGLProgram);
      if (admitted === false || (admitted && !admitted._linked)) {
        if (admitted) this._setError(constants.INVALID_OPERATION); return;
      }
      this._program = admitted;
    }
    getProgramParameter(program, parameter) {
      if (!owned(this, program, WebGLProgram)) return null;
      if (parameter === constants.LINK_STATUS) return program._linked;
      if (parameter === constants.VALIDATE_STATUS) return !!program._validated;
      if (parameter === constants.DELETE_STATUS) return program._deleted;
      if (parameter === constants.ATTACHED_SHADERS) return program._shaders.length;
      if (parameter === constants.ACTIVE_ATTRIBUTES) return program._attributes.length;
      if (parameter === constants.ACTIVE_UNIFORMS) return program._uniforms.length;
      this._setError(constants.INVALID_ENUM); return null;
    }
    getProgramInfoLog(program) { return owned(this, program, WebGLProgram) ? program._log : null; }
    deleteProgram(program) {
      if (!owned(this, program, WebGLProgram)) return;
      if (this._program === program) this._program = null;
      program._deleted = true; this._programCount--;
      this._resources.delete(program);
    }
    isProgram(program) { return owned(this, program, WebGLProgram); }
    getAttribLocation(program, name) {
      if (!owned(this, program, WebGLProgram) || !program._linked) return -1;
      return program._attributes.find((entry) => entry.name === String(name))?.location ?? -1;
    }
    getUniformLocation(program, name) {
      if (!owned(this, program, WebGLProgram) || !program._linked) return null;
      name = String(name).replace(/\[0\]$/, "");
      const entry = program._uniforms.find((candidate) => candidate.name === name);
      return entry ? new WebGLUniformLocation(program, entry) : null;
    }
    getActiveAttrib(program, index) {
      const entry = owned(this, program, WebGLProgram) ? program._attributes[index] : null;
      return entry ? new WebGLActiveInfo(entry.size, glTypeFor(entry.type), entry.name) : null;
    }
    getActiveUniform(program, index) {
      const entry = owned(this, program, WebGLProgram) ? program._uniforms[index] : null;
      return entry ? new WebGLActiveInfo(entry.size, glTypeFor(entry.type), entry.name) : null;
    }
    _uniform(location, value, acceptedTypes) {
      if (location === null || this._lost) return;
      if (!(location instanceof WebGLUniformLocation)
          || location._program !== this._program
          || !acceptedTypes.includes(location._type)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      location._program._uniformValues.set(location._name, value);
    }
    uniform1i(location, value) {
      this._uniform(location, toWebGLInt32(value), ["sampler2D"]);
    }
    uniform1f(location, value) { const v = Number(value); if (Number.isFinite(v)) this._uniform(location, v, ["float"]); else this._setError(constants.INVALID_VALUE); }
    uniform2f(location, x, y) { const v = finiteArray([x, y], 2); v ? this._uniform(location, v, ["vec2"]) : this._setError(constants.INVALID_VALUE); }
    uniform3f(location, x, y, z) { const v = finiteArray([x, y, z], 3); v ? this._uniform(location, v, ["vec3"]) : this._setError(constants.INVALID_VALUE); }
    uniform4f(location, x, y, z, w) { const v = finiteArray([x, y, z, w], 4); v ? this._uniform(location, v, ["vec4"]) : this._setError(constants.INVALID_VALUE); }
    uniform1fv(location, value) { const v = finiteArray(value, 1); v ? this._uniform(location, v, ["float"]) : this._setError(constants.INVALID_VALUE); }
    uniform2fv(location, value) { const v = finiteArray(value, 2); v ? this._uniform(location, v, ["vec2"]) : this._setError(constants.INVALID_VALUE); }
    uniform3fv(location, value) { const v = finiteArray(value, 3); v ? this._uniform(location, v, ["vec3"]) : this._setError(constants.INVALID_VALUE); }
    uniform4fv(location, value) { const v = finiteArray(value, 4); v ? this._uniform(location, v, ["vec4"]) : this._setError(constants.INVALID_VALUE); }
    uniformMatrix4fv(location, transpose, value) {
      if (transpose) { this._setError(constants.INVALID_VALUE); return; }
      const matrix = finiteArray(value, 16); matrix ? this._uniform(location, matrix, ["mat4"]) : this._setError(constants.INVALID_VALUE);
    }
    getUniform(program, location) {
      return owned(this, program, WebGLProgram) && location instanceof WebGLUniformLocation
        ? program._uniformValues.get(location._name) ?? null : null;
    }
    enableVertexAttribArray(index) {
      if (!Number.isInteger(index) || index < 0 || index >= 8) this._setError(constants.INVALID_VALUE);
      else this._attributesState[index].enabled = true;
    }
    disableVertexAttribArray(index) {
      if (!Number.isInteger(index) || index < 0 || index >= 8) this._setError(constants.INVALID_VALUE);
      else this._attributesState[index].enabled = false;
    }
    vertexAttribPointer(index, size, type, normalized, stride, offset) {
      const scalarBytes = typeBytes(type);
      if (!Number.isInteger(index) || index < 0 || index >= 8
          || !Number.isInteger(size) || size < 1 || size > 4
          || !scalarBytes || !Number.isInteger(stride) || stride < 0 || stride > 255
          || !Number.isInteger(offset) || offset < 0 || !this._arrayBuffer) {
        this._setError(!scalarBytes ? constants.INVALID_ENUM : constants.INVALID_VALUE); return;
      }
      if (offset % scalarBytes || (stride !== 0 && stride % scalarBytes)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      this._attributesState[index] = { ...this._attributesState[index],
        buffer: this._arrayBuffer, size, type, normalized: !!normalized, stride, offset };
    }
    vertexAttrib1f(index, x) { this.vertexAttrib4f(index, x, 0, 0, 1); }
    vertexAttrib2f(index, x, y) { this.vertexAttrib4f(index, x, y, 0, 1); }
    vertexAttrib3f(index, x, y, z) { this.vertexAttrib4f(index, x, y, z, 1); }
    vertexAttrib4f(index, x, y, z, w) {
      const value = finiteArray([x, y, z, w], 4);
      if (!value || !this._attributesState[index]) { this._setError(constants.INVALID_VALUE); return; }
      this._attributesState[index].constant = [...value];
    }
    getVertexAttrib(index, parameter) {
      const state = this._attributesState[index]; if (!state) { this._setError(constants.INVALID_VALUE); return null; }
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_ENABLED) return state.enabled;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_SIZE) return state.size;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_STRIDE) return state.stride;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_TYPE) return state.type;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_NORMALIZED) return state.normalized;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) return state.buffer;
      if (parameter === constants.CURRENT_VERTEX_ATTRIB) return new Float32Array(state.constant);
      this._setError(constants.INVALID_ENUM); return null;
    }
    getVertexAttribOffset(index, parameter) {
      if (parameter !== constants.VERTEX_ATTRIB_ARRAY_POINTER || !this._attributesState[index]) {
        this._setError(constants.INVALID_ENUM); return 0;
      }
      return this._attributesState[index].offset;
    }
    createTexture() {
      if (this._lost) return null;
      if (this._textureCount >= MAX_TEXTURE_OBJECTS) { this._setError(constants.OUT_OF_MEMORY); return null; }
      const texture = new WebGLTexture(this, this._nextId++);
      this._textureCount++; this._resources.add(texture); return texture;
    }
    deleteTexture(texture) {
      if (!owned(this, texture, WebGLTexture)) return;
      if (this._flushForMutation() === FLUSH_FAILED) return;
      if (this._texture === texture) this._texture = null;
      this._textureBytes -= texture._pixels?.byteLength || 0;
      texture._pixels = null; texture._commandSnapshot = null;
      texture._deleted = true; this._textureCount--;
      this._resources.delete(texture);
    }
    isTexture(texture) { return owned(this, texture, WebGLTexture); }
    activeTexture(unit) { if (unit !== constants.TEXTURE0) this._setError(constants.INVALID_ENUM); }
    bindTexture(target, texture) {
      if (target !== constants.TEXTURE_2D) { this._setError(constants.INVALID_ENUM); return; }
      const admitted = this._object(texture, WebGLTexture);
      if (admitted !== false) this._texture = admitted;
    }
    texParameteri(target, parameter, value) {
      if (target !== constants.TEXTURE_2D || !this._texture) { this._setError(constants.INVALID_OPERATION); return; }
      if (this._flushForMutation() === FLUSH_FAILED) return;
      if (parameter === constants.TEXTURE_MIN_FILTER) {
        if (![constants.NEAREST, constants.LINEAR].includes(value)) {
          this._setError(constants.INVALID_ENUM); return;
        }
        this._texture._min = value;
      } else if (parameter === constants.TEXTURE_MAG_FILTER) {
        if (![constants.NEAREST, constants.LINEAR].includes(value)) {
          this._setError(constants.INVALID_ENUM); return;
        }
        this._texture._mag = value;
      } else if (parameter === constants.TEXTURE_WRAP_S) {
        if (![constants.CLAMP_TO_EDGE, constants.REPEAT].includes(value)) {
          this._setError(constants.INVALID_ENUM); return;
        }
        this._texture._wrapS = value;
      } else if (parameter === constants.TEXTURE_WRAP_T) {
        if (![constants.CLAMP_TO_EDGE, constants.REPEAT].includes(value)) {
          this._setError(constants.INVALID_ENUM); return;
        }
        this._texture._wrapT = value;
      } else { this._setError(constants.INVALID_ENUM); return; }
      this._texture._generation++; this._texture._commandSnapshot = null;
    }
    texParameterf(target, parameter, value) { this.texParameteri(target, parameter, Number(value)); }
    getTexParameter(target, parameter) {
      if (target !== constants.TEXTURE_2D || !this._texture) { this._setError(constants.INVALID_OPERATION); return null; }
      if (parameter === constants.TEXTURE_MIN_FILTER) return this._texture._min;
      if (parameter === constants.TEXTURE_MAG_FILTER) return this._texture._mag;
      if (parameter === constants.TEXTURE_WRAP_S) return this._texture._wrapS;
      if (parameter === constants.TEXTURE_WRAP_T) return this._texture._wrapT;
      this._setError(constants.INVALID_ENUM); return null;
    }
    pixelStorei(parameter, value) {
      if (parameter === constants.UNPACK_FLIP_Y_WEBGL) this._flipY = !!value;
      else if (parameter === constants.UNPACK_PREMULTIPLY_ALPHA_WEBGL) this._premultiply = !!value;
      else if (parameter === constants.UNPACK_ALIGNMENT
               || parameter === constants.PACK_ALIGNMENT) {
        value = Number(value);
        if (![1, 2, 4, 8].includes(value)) {
          this._setError(constants.INVALID_VALUE);
          return;
        }
        if (parameter === constants.UNPACK_ALIGNMENT)
          this._unpackAlignment = value;
        else this._packAlignment = value;
      } else if (parameter === constants.UNPACK_COLORSPACE_CONVERSION_WEBGL) {
        if (![constants.NONE, constants.BROWSER_DEFAULT_WEBGL].includes(value))
          this._setError(constants.INVALID_VALUE);
      } else this._setError(constants.INVALID_ENUM);
    }
    _rgbaPixels(width, height, format, type, source, tightlyPacked = false) {
      if (type !== constants.UNSIGNED_BYTE
          || !Number.isInteger(width) || !Number.isInteger(height)
          || width < 0 || height < 0
          || width > 512 || height > 512 || width * height > 131072)
        return null;
      const components = format === constants.RGBA ? 4
        : format === constants.RGB ? 3
        : [constants.ALPHA, constants.LUMINANCE].includes(format) ? 1
        : format === constants.LUMINANCE_ALPHA ? 2 : 0;
      if (!components) return null;
      const rowBytes = width * components,
        rowStride = source == null || tightlyPacked ? rowBytes
          : Math.ceil(rowBytes / this._unpackAlignment) * this._unpackAlignment,
        requiredInput = height === 0 ? 0 : rowStride * (height - 1) + rowBytes;
      const input = source == null ? new Uint8Array(width * height * components)
        : ArrayBuffer.isView(source)
          ? new Uint8Array(source.buffer, source.byteOffset, source.byteLength) : null;
      if (!input || input.byteLength < requiredInput) return null;
      const outputBytes = width * height * 4;
      if (format === constants.RGBA && !this._premultiply) {
        if (rowStride === rowBytes && !this._flipY)
          return input.slice(0, outputBytes);
        const output = new Uint8Array(outputBytes);
        for (let y = 0; y < height; y++) {
          const sourceY = this._flipY ? height - 1 - y : y;
          output.set(input.subarray(sourceY * rowStride,
            sourceY * rowStride + rowBytes), y * rowBytes);
        }
        return output;
      }
      const output = new Uint8Array(outputBytes);
      for (let y = 0; y < height; y++) {
        const sourceY = this._flipY ? height - 1 - y : y,
          sourceRow = sourceY * rowStride, outputRow = y * width * 4;
        if (format === constants.RGB) {
          for (let x = 0; x < width; x++) {
            const inputAt = sourceRow + x * 3, outputAt = outputRow + x * 4;
            output[outputAt] = input[inputAt];
            output[outputAt + 1] = input[inputAt + 1];
            output[outputAt + 2] = input[inputAt + 2]; output[outputAt + 3] = 255;
          }
        } else if (format === constants.ALPHA) {
          for (let x = 0; x < width; x++) {
            const outputAt = outputRow + x * 4;
            output[outputAt + 3] = input[sourceRow + x];
          }
        } else if (format === constants.LUMINANCE) {
          for (let x = 0; x < width; x++) {
            const value = input[sourceRow + x], outputAt = outputRow + x * 4;
            output[outputAt] = value; output[outputAt + 1] = value;
            output[outputAt + 2] = value; output[outputAt + 3] = 255;
          }
        } else if (format === constants.LUMINANCE_ALPHA) {
          for (let x = 0; x < width; x++) {
            const inputAt = sourceRow + x * 2, outputAt = outputRow + x * 4,
              alpha = input[inputAt + 1], luminance = this._premultiply
                ? input[inputAt] * alpha / 255 : input[inputAt];
            output[outputAt] = luminance; output[outputAt + 1] = luminance;
            output[outputAt + 2] = luminance; output[outputAt + 3] = alpha;
          }
        } else {
          for (let x = 0; x < width; x++) {
            const inputAt = sourceRow + x * 4, outputAt = outputRow + x * 4,
              alpha = input[inputAt + 3];
            output[outputAt] = input[inputAt] * alpha / 255;
            output[outputAt + 1] = input[inputAt + 1] * alpha / 255;
            output[outputAt + 2] = input[inputAt + 2] * alpha / 255;
            output[outputAt + 3] = alpha;
          }
        }
      }
      return output;
    }
    texImage2D(target, level, internalFormat, ...args) {
      if (target !== constants.TEXTURE_2D || level !== 0 || !this._texture) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (this._flushForMutation() === FLUSH_FAILED) return;
      let width, height, border = 0, format, type, source;
      if (args.length === 6) [width, height, border, format, type, source] = args;
      else if (args.length === 3) {
        [format, type, source] = args;
        const snapshot = source instanceof HTMLCanvasElement
          ? globalThis.__tilefinchCanvasReadbackState?.(source)
          : source?.__handle ? __tilefinchCanvasImageSource(source.__handle) : null;
        if (!snapshot || snapshot.sameOrigin === false ||
            snapshot.originClean === false)
          throw new DOMException("The image is not origin-clean", "SecurityError");
        width = Number(snapshot.width); height = Number(snapshot.height);
        source = new Uint8Array(snapshot.pixels);
      } else { this._setError(constants.INVALID_VALUE); return; }
      width = Number(width); height = Number(height);
      if (border !== 0 || internalFormat !== format) { this._setError(constants.INVALID_VALUE); return; }
      const pixels = this._rgbaPixels(
        width, height, format, type, source, args.length === 3);
      if (!pixels) { this._setError(constants.INVALID_VALUE); return; }
      const retained = this._textureBytes - (this._texture._pixels?.byteLength || 0);
      if (pixels.byteLength > MAX_TEXTURE_BYTES - retained) { this._setError(constants.OUT_OF_MEMORY); return; }
      this._textureBytes = retained + pixels.byteLength;
      this._texture._width = width; this._texture._height = height;
      this._texture._pixels = pixels; this._texture._generation++;
      this._texture._commandSnapshot = null;
      diagnostics.uploadedTextureBytes += pixels.byteLength;
    }
    texSubImage2D(target, level, x, y, ...args) {
      if (target !== constants.TEXTURE_2D || level !== 0 || !this._texture?._pixels) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
      let width, height, format, type, source;
      if (args.length === 5) [width, height, format, type, source] = args;
      else { this._setError(constants.INVALID_VALUE); return; }
      x = Number(x); y = Number(y);
      width = Number(width); height = Number(height);
      const pixels = this._rgbaPixels(width, height, format, type, source);
      if (!Number.isInteger(x) || !Number.isInteger(y)
          || !pixels || x < 0 || y < 0 || x + width > this._texture._width
          || y + height > this._texture._height) { this._setError(constants.INVALID_VALUE); return; }
      if (flushOutcome === FLUSH_DEFERRED)
        this._texture._pixels = this._texture._pixels.slice();
      for (let row = 0; row < height; row++)
        this._texture._pixels.set(pixels.subarray(row * width * 4, (row + 1) * width * 4),
          ((y + row) * this._texture._width + x) * 4);
      this._texture._generation++; this._texture._commandSnapshot = null;
      diagnostics.uploadedTextureBytes += pixels.byteLength;
    }
    generateMipmap() { this._setError(constants.INVALID_OPERATION); }
    clearColor(r, g, b, a) {
      const values = finiteArray([r, g, b, a], 4);
      if (!values) { this._setError(constants.INVALID_VALUE); return; }
      this._clearColor = [...values].map((value) => Math.max(0, Math.min(1, value)));
    }
    clearDepth(value) { value = Number(value); Number.isFinite(value) ? this._clearDepth = Math.max(0, Math.min(1, value)) : this._setError(constants.INVALID_VALUE); }
    clear(mask) {
      if (mask & ~(constants.COLOR_BUFFER_BIT | constants.DEPTH_BUFFER_BIT | constants.STENCIL_BUFFER_BIT)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      const command = this._stagingCommand;
      command.kind = "clear"; command.mask = mask;
      command.clearColor.set(this._clearColor);
      command.clearDepth = this._clearDepth;
      command.scissorEnabled = this._enabled.has(constants.SCISSOR_TEST);
      command.scissor.set(this._scissor);
      if (!this._enqueueCommand(command)) return;
      this._schedule();
    }
    _capability(capability) {
      return [constants.BLEND, constants.CULL_FACE, constants.DEPTH_TEST,
        constants.DITHER, constants.SCISSOR_TEST].includes(capability);
    }
    enable(capability) {
      if (!this._capability(capability)) this._setError(constants.INVALID_ENUM);
      else this._enabled.add(capability);
    }
    disable(capability) {
      if (!this._capability(capability)) this._setError(constants.INVALID_ENUM);
      else this._enabled.delete(capability);
    }
    isEnabled(capability) {
      if (!this._capability(capability)) {
        this._setError(constants.INVALID_ENUM); return false;
      }
      return this._enabled.has(capability);
    }
    blendFunc(source, destination) {
      const factors = [constants.ZERO, constants.ONE, constants.SRC_ALPHA,
        constants.ONE_MINUS_SRC_ALPHA];
      if (!factors.includes(source) || !factors.includes(destination)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      this._blendSrc = source; this._blendDst = destination;
    }
    blendFuncSeparate(sourceRGB, destinationRGB, sourceAlpha,
                      destinationAlpha) {
      const factors = [constants.ZERO, constants.ONE, constants.SRC_ALPHA,
        constants.ONE_MINUS_SRC_ALPHA];
      if (![sourceRGB, destinationRGB, sourceAlpha, destinationAlpha]
          .every((factor) => factors.includes(factor))) {
        this._setError(constants.INVALID_ENUM); return;
      }
      /* The PSP GE has one factor pair for all four channels. Refuse the
         uncommon split-alpha shape instead of silently drawing different
         pixels from WebGL. */
      if (sourceRGB !== sourceAlpha || destinationRGB !== destinationAlpha) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      this._blendSrc = sourceRGB; this._blendDst = destinationRGB;
    }
    blendEquation(mode) { if (mode !== constants.FUNC_ADD) this._setError(constants.INVALID_ENUM); }
    blendEquationSeparate(rgb, alpha) { if (rgb !== constants.FUNC_ADD || alpha !== constants.FUNC_ADD) this._setError(constants.INVALID_ENUM); }
    depthFunc(value) {
      if (value < constants.NEVER || value > constants.ALWAYS)
        this._setError(constants.INVALID_ENUM);
      else this._depthFunc = value;
    }
    cullFace(value) {
      if (![constants.FRONT, constants.BACK,
            constants.FRONT_AND_BACK].includes(value))
        this._setError(constants.INVALID_ENUM);
      else this._cullFace = value;
    }
    frontFace(value) {
      if (![constants.CW, constants.CCW].includes(value))
        this._setError(constants.INVALID_ENUM);
      else this._frontFace = value;
    }
    lineWidth(value) { value = Number(value); value === 1 ? this._lineWidth = 1 : this._setError(constants.INVALID_VALUE); }
    viewport(x, y, width, height) {
      x = toWebGLInt32(x); y = toWebGLInt32(y);
      width = toWebGLInt32(width); height = toWebGLInt32(height);
      if (width < 0 || height < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      this._viewport = [x, y, Math.min(MAX_DRAWING_WIDTH, width),
        Math.min(MAX_DRAWING_HEIGHT, height)];
    }
    scissor(x, y, width, height) {
      x = toWebGLInt32(x); y = toWebGLInt32(y);
      width = toWebGLInt32(width); height = toWebGLInt32(height);
      if (width < 0 || height < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      this._scissor = [x, y, width, height];
    }
    colorMask(red, green, blue, alpha) {
      /* The initial bounded GE path has one opaque color plane. Refuse a
         non-default mask instead of silently drawing channels WebGL asked us
         to preserve. */
      if (![red, green, blue, alpha].every(Boolean))
        this._setError(constants.INVALID_OPERATION);
    }
    depthMask(value) {
      if (!Boolean(value)) this._setError(constants.INVALID_OPERATION);
    }
    stencilMask() { this._setError(constants.INVALID_OPERATION); }
    stencilMaskSeparate() { this._setError(constants.INVALID_OPERATION); }
    polygonOffset() {} sampleCoverage() {} hint() {} flush() { this._flush(); }
    finish() { this._flush(); }
    _attributeRangeAvailable(attribute, first, count) {
      if (!attribute?.enabled || !attribute.buffer) return false;
      if (!count) return true;
      const componentBytes = typeBytes(attribute.type),
        elementBytes = attribute.size * componentBytes,
        stride = attribute.stride || elementBytes,
        finalVertex = first + count - 1;
      if (!componentBytes || !Number.isSafeInteger(finalVertex)
          || finalVertex < first || attribute.offset > attribute.buffer._data.byteLength)
        return false;
      const available = attribute.buffer._data.byteLength - attribute.offset;
      return finalVertex <= Math.floor((available - elementBytes) / stride);
    }
    _draw(mode, first, count, index = null) {
      if (!this._program?._linked) { this._setError(constants.INVALID_OPERATION); return; }
      if (![constants.POINTS, constants.LINES, constants.LINE_STRIP,
            constants.LINE_LOOP, constants.TRIANGLES,
            constants.TRIANGLE_STRIP, constants.TRIANGLE_FAN].includes(mode)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      if (!Number.isInteger(first) || first < 0 || !Number.isInteger(count) || count < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (!count) return;
      if (count > MAX_VERTICES) {
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      const translation = this._program._translation,
        attr = (entry) => entry ? this._attributesState[
          this._program._attributes.find((candidate) => candidate.name === entry.name)?.location ?? -1] : null,
        position = attr(translation.position), color = attr(translation.color),
        texcoord = attr(translation.texcoord);
      if (!position?.enabled || !position.buffer) { this._setError(constants.INVALID_OPERATION); return; }
      if (translation.usesTexture && (!texcoord?.enabled || !texcoord.buffer || !this._texture?._pixels)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      const vertexFirst = index ? 0 : first,
        vertexCount = index ? index.maximum + 1 : count;
      if (!this._attributeRangeAvailable(position, vertexFirst, vertexCount)
          || (color?.enabled
              && !this._attributeRangeAvailable(color, vertexFirst, vertexCount))
          || (texcoord?.enabled
              && !this._attributeRangeAvailable(texcoord, vertexFirst, vertexCount))) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      const command = this._stagingCommand;
      command.kind = "draw";
      const matrix = combinedMatrix4Into(
        command.matrix, this._matrixScratch,
        translation.matrices, this._program._uniformValues);
      /* Finite authored uniforms can still overflow while matrices are
         multiplied. That invalidates this primitive's geometry; it is not a
         corrupt JS/native command contract and must not lose the context. */
      for (let component = 0; component < matrix.length; component++)
        if (!Number.isFinite(matrix[component])) return;
      const selectedColor = translation.uniformColor
        ? this._program._uniformValues.get(translation.uniformColor.name)
        : [1, 1, 1, 1];
      const uniformColor = command.uniformColor;
      uniformColor.set(selectedColor);
      if (translation.color && !color?.enabled) {
        for (let component = 0; component < 4; component++)
          uniformColor[component] *= color?.constant?.[component] ?? 1;
      }
      const snapshotAttribute = (storage, attribute) => {
          if (!attribute?.enabled) { storage.data = null; return null; }
          storage.data = attribute.buffer._data;
          storage.size = attribute.size; storage.type = attribute.type;
          storage.normalized = attribute.normalized;
          storage.stride = attribute.stride; storage.offset = attribute.offset;
          return storage;
        }, selectedTexture = translation.usesTexture ? this._texture : null;
      let textureSnapshot = null;
      if (selectedTexture) {
        if (selectedTexture._commandSnapshot?._generation
            !== selectedTexture._generation) {
          selectedTexture._commandSnapshot = {
            _id: selectedTexture._id,
            _generation: selectedTexture._generation,
            _width: selectedTexture._width,
            _height: selectedTexture._height,
            _pixels: selectedTexture._pixels,
            _min: selectedTexture._min,
            _mag: selectedTexture._mag,
            _wrapS: selectedTexture._wrapS,
            _wrapT: selectedTexture._wrapT,
          };
        }
        textureSnapshot = selectedTexture._commandSnapshot;
      }
      const positionSnapshot = snapshotAttribute(
          command._positionStorage, position),
        colorSnapshot = snapshotAttribute(command._colorStorage, color),
        texcoordSnapshot = snapshotAttribute(command._texcoordStorage, texcoord),
        candidateSources = [positionSnapshot?.data, colorSnapshot?.data,
          texcoordSnapshot?.data, index?.data, textureSnapshot?._pixels];
      command.mode = mode; command.first = first; command.count = count;
      command.index = index; command.position = positionSnapshot;
      command.color = colorSnapshot; command.texcoord = texcoordSnapshot;
      command.texture = textureSnapshot;
      command.blend = this._enabled.has(constants.BLEND);
      command.depth = this._enabled.has(constants.DEPTH_TEST);
      command.cull = this._enabled.has(constants.CULL_FACE);
      command.depthFunc = this._depthFunc;
      command.blendSrc = this._blendSrc; command.blendDst = this._blendDst;
      command.cullFace = this._cullFace; command.frontFace = this._frontFace;
      command.viewport.set(this._viewport);
      command.scissorEnabled = this._enabled.has(constants.SCISSOR_TEST);
      command.scissor.set(this._scissor);
      if (!this._enqueueCommand(
          command, candidateSources, textureSnapshot, count)) return;
      diagnostics.drawCalls++;
      diagnostics.vertices += count; this._schedule();
    }
    drawArrays(mode, first, count) { this._draw(mode, Number(first), Number(count)); }
    drawElements(mode, count, type, offset) {
      if (![constants.UNSIGNED_BYTE, constants.UNSIGNED_SHORT].includes(type)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      count = Number(count); offset = Number(offset);
      if (!Number.isInteger(count) || count < 0
          || !Number.isInteger(offset) || offset < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (!this._elementBuffer) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      const elementBytes = typeBytes(type), data = this._elementBuffer._data;
      if (offset % elementBytes || offset > data.byteLength
          || count > Math.floor((data.byteLength - offset) / elementBytes)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (!count) return;
      if (count > MAX_VERTICES) {
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      let maximum = this._elementBuffer._indexMaximum;
      if (this._elementBuffer._indexType !== type
          || this._elementBuffer._indexOffset !== offset
          || this._elementBuffer._indexCount !== count) {
        maximum = 0;
        if (type === constants.UNSIGNED_BYTE) {
          for (let at = 0; at < count; at++)
            maximum = Math.max(maximum, data[offset + at]);
        } else {
          const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
          for (let at = 0; at < count; at++)
            maximum = Math.max(maximum, view.getUint16(offset + at * 2, true));
        }
        this._elementBuffer._indexType = type;
        this._elementBuffer._indexOffset = offset;
        this._elementBuffer._indexCount = count;
        this._elementBuffer._indexMaximum = maximum;
      }
      this._drawIndex.data = data; this._drawIndex.type = type;
      this._drawIndex.offset = offset; this._drawIndex.maximum = maximum;
      this._draw(mode, 0, count, this._drawIndex);
    }
    readPixels(x, y, width, height, format, type, pixels) {
      if (format !== constants.RGBA || type !== constants.UNSIGNED_BYTE) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      x = Number(x); y = Number(y); width = Number(width); height = Number(height);
      if (![x, y, width, height].every(Number.isInteger)
          || x < -2147483648 || x > 2147483647
          || y < -2147483648 || y > 2147483647
          || width < 0 || height < 0 || width > 2147483647
          || height > 2147483647 || width > MAX_DRAWING_PIXELS
          || height > MAX_DRAWING_PIXELS
          || width * height > MAX_DRAWING_PIXELS) {
        this._setError(constants.INVALID_VALUE); return;
      }
      const rowBytes = width * 4,
        rowStride = Math.ceil(rowBytes / this._packAlignment) * this._packAlignment,
        required = height === 0 ? 0 : rowStride * (height - 1) + rowBytes;
      if (!ArrayBuffer.isView(pixels) || pixels.BYTES_PER_ELEMENT !== 1
          || pixels.byteLength < required) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (this._flush() !== FLUSH_OK) return;
      const canvas = this.canvas;
      if (!canvas || !__tilefinchWebGLReadPixels(canvas.__handle, x, y,
          width, height, pixels, this._packAlignment))
        this._setError(constants.INVALID_OPERATION);
    }
    getParameter(parameter) {
      const values = {
        [constants.VENDOR]: "Tilefinch",
        [constants.RENDERER]: "Tilefinch PSP GE (bounded)",
        [constants.VERSION]: "WebGL 1.0 Tilefinch",
        [constants.SHADING_LANGUAGE_VERSION]: "WebGL GLSL ES 1.0 (fixed-function subset)",
        [constants.MAX_TEXTURE_SIZE]: 512,
        [constants.MAX_VERTEX_ATTRIBS]: 8,
        [constants.MAX_TEXTURE_IMAGE_UNITS]: 1,
        [constants.MAX_COMBINED_TEXTURE_IMAGE_UNITS]: 1,
        [constants.MAX_VERTEX_TEXTURE_IMAGE_UNITS]: 0,
        [constants.MAX_VARYING_VECTORS]: 2,
        [constants.MAX_VERTEX_UNIFORM_VECTORS]: 16,
        [constants.MAX_FRAGMENT_UNIFORM_VECTORS]: 4,
        [constants.ARRAY_BUFFER_BINDING]: this._arrayBuffer,
        [constants.ELEMENT_ARRAY_BUFFER_BINDING]: this._elementBuffer,
        [constants.TEXTURE_BINDING_2D]: this._texture,
        [constants.ACTIVE_TEXTURE]: constants.TEXTURE0,
        [constants.CURRENT_PROGRAM]: this._program,
        [constants.COLOR_CLEAR_VALUE]: new Float32Array(this._clearColor),
        [constants.DEPTH_CLEAR_VALUE]: this._clearDepth,
        [constants.DEPTH_FUNC]: this._depthFunc,
        [constants.BLEND_SRC_RGB]: this._blendSrc,
        [constants.BLEND_DST_RGB]: this._blendDst,
        [constants.CULL_FACE_MODE]: this._cullFace,
        [constants.FRONT_FACE]: this._frontFace,
        [constants.LINE_WIDTH]: this._lineWidth,
        [constants.VIEWPORT]: new Int32Array(this._viewport),
        [constants.SCISSOR_BOX]: new Int32Array(this._scissor),
        [constants.UNPACK_ALIGNMENT]: this._unpackAlignment,
        [constants.PACK_ALIGNMENT]: this._packAlignment,
      };
      if (parameter === constants.MAX_VIEWPORT_DIMS) return new Int32Array([480, 272]);
      if (parameter in values) return values[parameter];
      if ([constants.BLEND, constants.CULL_FACE, constants.DEPTH_TEST,
           constants.DITHER, constants.SCISSOR_TEST].includes(parameter))
        return this._enabled.has(parameter);
      this._setError(constants.INVALID_ENUM); return null;
    }
    getShaderPrecisionFormat() { return new WebGLShaderPrecisionFormat(127, 127, 23); }
    getSupportedExtensions() { return []; }
    getExtension() { return null; }
    createFramebuffer() {
      if (this._lost) return null;
      if (this._framebufferCount >= MAX_FRAMEBUFFER_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const framebuffer = new WebGLFramebuffer(this, this._nextId++);
      this._framebufferCount++; this._resources.add(framebuffer);
      return framebuffer;
    }
    createRenderbuffer() {
      if (this._lost) return null;
      if (this._renderbufferCount >= MAX_RENDERBUFFER_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const renderbuffer = new WebGLRenderbuffer(this, this._nextId++);
      this._renderbufferCount++; this._resources.add(renderbuffer);
      return renderbuffer;
    }
    bindFramebuffer(target, value) {
      if (target !== constants.FRAMEBUFFER || value !== null) this._setError(constants.INVALID_OPERATION);
    }
    bindRenderbuffer(target, value) {
      if (target !== constants.RENDERBUFFER || value !== null) this._setError(constants.INVALID_OPERATION);
    }
    checkFramebufferStatus(target) {
      if (target !== constants.FRAMEBUFFER) { this._setError(constants.INVALID_ENUM); return 0; }
      return constants.FRAMEBUFFER_COMPLETE;
    }
    deleteFramebuffer(value) {
      if (owned(this, value, WebGLFramebuffer)) {
        value._deleted = true; this._framebufferCount--;
        this._resources.delete(value);
      }
    }
    deleteRenderbuffer(value) {
      if (owned(this, value, WebGLRenderbuffer)) {
        value._deleted = true; this._renderbufferCount--;
        this._resources.delete(value);
      }
    }
    isFramebuffer(value) { return owned(this, value, WebGLFramebuffer); }
    isRenderbuffer(value) { return owned(this, value, WebGLRenderbuffer); }
    framebufferTexture2D() { this._setError(constants.INVALID_OPERATION); }
    framebufferRenderbuffer() { this._setError(constants.INVALID_OPERATION); }
    renderbufferStorage() { this._setError(constants.INVALID_OPERATION); }
    copyTexImage2D() { this._setError(constants.INVALID_OPERATION); }
    copyTexSubImage2D() { this._setError(constants.INVALID_OPERATION); }
  }

  for (const [name, value] of Object.entries(constants))
    Object.defineProperty(WebGLRenderingContext.prototype, name,
      { configurable: false, enumerable: true, value, writable: false });

  globalThis.__tilefinchCreateWebGLContext = (canvas, attributes) => {
    let context = contextForCanvas(canvas);
    if (context) return context;
    if (activeContexts().length >= MAX_CONTEXTS) return null;
    if (!boundedDrawingSize(canvas.width, canvas.height)) return null;
    context = new WebGLRenderingContext(canvas, attributes || {});
    contexts.set(canvas,
      typeof WeakRef === "function" ? new WeakRef(context) : context);
    liveContexts.push(typeof WeakRef === "function" ? new WeakRef(context) : context);
    context._resize();
    diagnostics.contexts++; return context;
  };
  globalThis.__tilefinchWebGLResize = (canvas) => contextForCanvas(canvas)?._resize();
  globalThis.__tilefinchWebGLConnected = (canvas) => contextForCanvas(canvas)?._schedule();
  globalThis.__tilefinchFlushWebGLSurfaces = () => {
    for (const context of activeContexts()) context._flush();
  };
  globalThis.__tilefinchWebGLReadback = (canvas) => {
    const context = contextForCanvas(canvas);
    if (!context || context._lost) return null;
    if (context._flush() !== FLUSH_OK) return null;
    const snapshot = __tilefinchWebGLSnapshot(canvas.__handle);
    return snapshot ? { width: Number(snapshot.width), height: Number(snapshot.height),
      pixels: new Uint8ClampedArray(snapshot.pixels), originClean: true,
      surfaceUnavailable: false } : null;
  };
  Object.assign(globalThis, { WebGLRenderingContext, WebGLBuffer, WebGLTexture,
    WebGLShader, WebGLProgram, WebGLUniformLocation, WebGLActiveInfo,
    WebGLShaderPrecisionFormat, WebGLFramebuffer, WebGLRenderbuffer });
  Object.defineProperty(globalThis, "__tilefinchWebGLDiagnostics",
    { value: diagnostics, configurable: false, enumerable: false });
})();
