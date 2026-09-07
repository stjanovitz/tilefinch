(() => {
  "use strict";

  /* Surface retirement also advances the native cache incarnation.  Page
     code never reads, selects, or serializes that process-issued identity. */
  const releaseNativeWebGLSurface =
    globalThis.__tilefinchWebGLReleaseSurface,
    nativeIndexMaximum = globalThis.__tilefinchWebGLIndexMaximum,
    nativeFiniteFloat32 = globalThis.__tilefinchWebGLFiniteFloat32,
    nativeCombineMatrix4 = globalThis.__tilefinchWebGLCombineMatrix4;

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
    MAX_VERTEX_ARRAY_OBJECTS = 8,
    MAX_FRAMEBUFFER_OBJECTS = 4,
    MAX_RENDERBUFFER_OBJECTS = 4,
    /* One payload per admitted buffer and texture is the exact native cap.
       Changing either object limit requires re-measuring the PSP bridge. */
    MAX_NATIVE_SOURCES = MAX_BUFFER_OBJECTS + MAX_TEXTURE_OBJECTS,
    MAX_SHADER_BYTES = 16 * 1024,
    MAX_INTERFACE_DECLARATIONS = 24,
    MAX_DRAWS = 64,
    MAX_INSTANCES = 64,
    /* Ordinary commands are packed and released as they enter the current
       batch. Only a deferred ordered tail retains object snapshots. */
    COMMAND_POOL_LIMIT = MAX_DRAWS + 1,
    MAX_VERTICES = 4096,
    MAX_DRAWING_WIDTH = 480,
    MAX_DRAWING_HEIGHT = 272,
    MAX_DRAWING_PIXELS = 131072,
    COMMAND_WORDS = 96,
    TEXTURE_WORDS = 9,
    WIRE_HEADER_WORDS = 4,
    COMMAND_WIRE_MAGIC = 0x54465743,
    TEXTURE_WIRE_MAGIC = 0x54465754,
    WIRE_VERSION = 5,
    FLUSH_FAILED = 0,
    FLUSH_OK = 1,
    FLUSH_DEFERRED = 2,
    CAP_BLEND = 1 << 0,
    CAP_CULL_FACE = 1 << 1,
    CAP_DEPTH_TEST = 1 << 2,
    CAP_DITHER = 1 << 3,
    CAP_SCISSOR_TEST = 1 << 4,
    DEFAULT_UNIFORM_COLOR = new Float32Array([1, 1, 1, 1]),
    contexts = new WeakMap(),
    repeatedCommandLists = new WeakMap(),
    liveContexts = [],
    diagnostics = {
      contexts: 0,
      lostContexts: 0,
      shaderRefusals: 0,
      drawCalls: 0,
      instancedDrawCalls: 0,
      instances: 0,
      flushes: 0,
      vertices: 0,
      uploadedTextureBytes: 0,
      nativeFailures: 0,
      deferredFlushes: 0,
      forcedFlushes: 0,
      maximumForcedFlushesPerFrame: 0,
      drawPlanHits: 0,
      drawPlanMisses: 0,
      commandTemplateHits: 0,
      commandTemplateMisses: 0,
      commandSlotTemplateHits: 0,
      commandSlotTemplateMisses: 0,
      sourcePacketHits: 0,
      sourcePacketMisses: 0,
      sourcePacketIndexHits: 0,
      sourcePacketIndexMisses: 0,
      profileDrawPhases: false,
      profileDraws: 0,
      profileBasicMs: 0,
      profileInstancesMs: 0,
      profileRangesMs: 0,
      profilePrepareMs: 0,
      profileEnqueueMs: 0,
      profileFinishMs: 0,
      profileQueueAdmissionMs: 0,
      profileWirePackMs: 0,
      profileWireSourcesMs: 0,
      profileWireStateMs: 0,
      profileWireInstancesMs: 0,
      profileWireRetainMs: 0,
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
      VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE: 0x88fe,
      VERTEX_ARRAY_BINDING_OES: 0x85b5,
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
    capabilityBit = (capability) => {
      if (capability === constants.BLEND) return CAP_BLEND;
      if (capability === constants.CULL_FACE) return CAP_CULL_FACE;
      if (capability === constants.DEPTH_TEST) return CAP_DEPTH_TEST;
      if (capability === constants.DITHER) return CAP_DITHER;
      if (capability === constants.SCISSOR_TEST) return CAP_SCISSOR_TEST;
      return 0;
    },
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
        const at = column * 4,
          right0 = right[at], right1 = right[at + 1],
          right2 = right[at + 2], right3 = right[at + 3];
        output[at] = left[0] * right0 + left[4] * right1
          + left[8] * right2 + left[12] * right3;
        output[at + 1] = left[1] * right0 + left[5] * right1
          + left[9] * right2 + left[13] * right3;
        output[at + 2] = left[2] * right0 + left[6] * right1
          + left[10] * right2 + left[14] * right3;
        output[at + 3] = left[3] * right0 + left[7] * right1
          + left[11] * right2 + left[15] * right3;
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
      data: null, id: 0, generation: 0, usage: 0,
      size: 0, type: 0, normalized: false, stride: 0, offset: 0,
    }),
    makeVertexAttributeState = () => ({
      enabled: false, buffer: null, size: 4, type: constants.FLOAT,
      normalized: false, stride: 0, offset: 0,
      constant: new Float32Array([0, 0, 0, 1]),
      capacityGeneration: 0, capacity: 0, divisor: 0,
    }),
    makeVertexArrayState = () => ({
      attributes: Array.from({ length: 8 }, makeVertexAttributeState),
      elementBuffer: null,
      revision: 1,
      drawPlan: null,
    }),
    makeCommandIndex = () => ({
      data: null, id: 0, generation: 0, usage: 0,
      type: 0, offset: 0, maximum: 0,
    }),
    makeCommandRecord = (poolIndex = -1) => ({
      _pooled: true, _poolIndex: poolIndex, _inUse: false,
      kind: "clear", mask: 0,
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
      instanceCount: 1,
      _instanceMatrixStorage: makeCommandAttribute(), instanceMatrix: null,
      _instanceColorStorage: makeCommandAttribute(), instanceColor: null,
      _instanceTransformStorage: makeCommandAttribute(), instanceTransform: null,
    }),
    writeCommandSourceIdentity = (wire, base, slot, source) => {
      wire[base + slot] = source?.id || 0;
      wire[base + slot + 1] = source?.generation || 0;
      wire[base + slot + 2] = source?.usage || 0;
    },
    writeCommandAttribute = (context, wireI32, wireU32, base, slot,
                             attribute, sources, sourceMap) => {
      wireI32[base + slot] = context._sourceIndex(
        sources, sourceMap, attribute?.data);
      wireI32[base + slot + 1] = attribute?.size || 0;
      wireI32[base + slot + 2] = attribute?.type || 0;
      wireU32[base + slot + 3] = attribute?.normalized ? 1 : 0;
      wireU32[base + slot + 4] = attribute?.stride || 0;
      wireU32[base + slot + 5] = attribute?.offset || 0;
    },
    writeLiveSourceIdentity = (wire, base, slot, attribute) => {
      const buffer = attribute?.enabled ? attribute.buffer : null;
      wire[base + slot] = buffer?._id || 0;
      wire[base + slot + 1] = buffer?._generation || 0;
      wire[base + slot + 2] = buffer?._usage || 0;
    },
    writeTemplateAttribute = (wireI32, wireU32, slot, attribute) => {
      const buffer = attribute?.enabled ? attribute.buffer : null;
      wireI32[slot] = -1;
      wireI32[slot + 1] = buffer ? attribute.size : 0;
      wireI32[slot + 2] = buffer ? attribute.type : 0;
      wireU32[slot + 3] = buffer && attribute.normalized ? 1 : 0;
      wireU32[slot + 4] = buffer ? attribute.stride : 0;
      wireU32[slot + 5] = buffer ? attribute.offset : 0;
    };

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
      this._generation = 1;
      this._storageGeneration = 1;
      this._usage = constants.STATIC_DRAW;
      this._indexType = 0; this._indexOffset = -1;
      this._indexCount = -1; this._indexMaximum = 0;
      /* Element draws reuse this bounded per-buffer descriptor. Keeping one
         on the context forced seven property writes every time consecutive
         draw calls selected different index buffers. */
      this._commandIndex = makeCommandIndex();
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
      this._linkRevision = 0;
      this._uniformStateRevision = 1;
      this._matrixRevision = 1; this._combinedMatrixRevision = 0;
      this._combinedMatrixStorage = new Float32Array(16);
      this._combinedMatrix = this._combinedMatrixStorage;
      this._matrixPack = new Float32Array(64);
      this._matrixCount = 0;
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
  class WebGLVertexArrayOES extends GLObject {
    constructor(owner, id) {
      super(owner, id);
      this._state = makeVertexArrayState();
      this._everBound = false;
    }
  }
  class AngleInstancedArrays {
    constructor(context) {
      this._context = context;
      this.VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE =
        constants.VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE;
    }
    vertexAttribDivisorANGLE(index, divisor) {
      const context = this._context;
      index = Number(index); divisor = Number(divisor);
      if (!Number.isInteger(index) || index < 0 || index >= 8
          || !Number.isInteger(divisor) || divisor < 0 || divisor > 1) {
        context._setError(divisor > 1
          ? constants.INVALID_OPERATION : constants.INVALID_VALUE);
        return;
      }
      const state = context._attributesState[index];
      if (state.divisor === divisor) return;
      state.divisor = divisor;
      (context._vertexArray?._state || context._defaultVertexArrayState)
        .revision++;
      context._touchRepeatedCommandState();
    }
    drawArraysInstancedANGLE(mode, first, count, instances) {
      this._context._draw(
        mode, Number(first), Number(count), null, Number(instances));
    }
    drawElementsInstancedANGLE(mode, count, type, offset, instances) {
      this._context._drawElements(
        mode, count, type, offset, Number(instances));
    }
  }
  class OesVertexArrayObject {
    constructor(context) {
      this._context = context;
      this.VERTEX_ARRAY_BINDING_OES = constants.VERTEX_ARRAY_BINDING_OES;
    }
    createVertexArrayOES() { return this._context._createVertexArray(); }
    deleteVertexArrayOES(array) { this._context._deleteVertexArray(array); }
    isVertexArrayOES(array) {
      return owned(this._context, array, WebGLVertexArrayOES)
        && array._everBound;
    }
    bindVertexArrayOES(array) { this._context._bindVertexArray(array); }
  }
  class TilefinchRepeatedFrameCommands {
    constructor(context) { this._context = context; }
    begin() { return this._context._beginRepeatedCommandCapture(); }
    end(drawLimits) {
      return this._context._endRepeatedCommandCapture(drawLimits);
    }
    execute(commandList, drawCounts) {
      return this._context._executeRepeatedCommandList(
        commandList, drawCounts);
    }
  }

  const owned = (context, value, Type) =>
    value instanceof Type && value._owner === context && !value._deleted,
    attributeRole = (attributes, source, role) => {
      const pattern = role === "position" ? /position|vertex|coord/i
        : role === "color" ? /colou?r|tint/i : /tex|uv/i;
      const candidates = attributes.filter((entry) => entry.type !== "mat4"
        && pattern.test(entry.name));
      if (candidates.length) return candidates[0];
      if (role === "position") {
        const expression = /gl_Position\s*=([^;]+)/.exec(source)?.[1] || "";
        return attributes.find((entry) => entry.type !== "mat4" &&
          new RegExp("\\b" + entry.name + "\\b").test(expression))
          || attributes.find((entry) => entry.type !== "mat4") || null;
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
          || fragmentUniformScan.excess
          || attributes.reduce((slots, entry) =>
            slots + (entry.type === "mat4" ? 4 : 1), 0) > 8
          || attributes.some((entry) => entry.size !== 1)
          || uniformVectors(vertexUniformScan.entries) > 16
          || uniformVectors(fragmentUniformScan.entries) > 4
          || conflictingUniform)
        return { error: "Shader interface exceeds the PSP WebGL limits" };
      const positionExpression = /\bgl_Position\s*=\s*([^;]+)/.exec(vs)?.[1] || "",
        position = attributeRole(attributes, vs, "position"),
        color = attributeRole(attributes, vs, "color"),
        texcoord = attributeRole(attributes, vs, "texture"),
        instanceMatrix = attributes.find((entry) => entry.type === "mat4"
          && new RegExp("\\b" + entry.name + "\\b").test(positionExpression)) || null,
        instanceColor = attributes.find((entry) => entry !== color
          && entry.type === "vec4"
          && /instance.*(colou?r|tint)|(colou?r|tint).*instance/i
            .test(entry.name)) || null,
        instanceTransform = attributes.find((entry) => entry !== color
          && entry !== instanceColor && entry.type === "vec4"
          && /instance.*(transform|offset|position)|(transform|offset|position).*instance/i
            .test(entry.name)
          && new RegExp("\\b" + entry.name + "\\s*\\.\\s*xyz\\b")
            .test(positionExpression)
          && new RegExp("\\b" + entry.name + "\\s*\\.\\s*w\\b")
            .test(positionExpression)) || null,
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
      return { attributes, uniforms, position, color, texcoord,
        instanceMatrix, instanceColor, instanceTransform, matrices,
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
        /* Full framebuffer multisampling does not fit the PSP budget.  An
           explicit request enables Tilefinch's bounded edge-coverage path;
           keep the default off so existing pages retain their measured cost. */
        antialias: attributes?.antialias === true,
        premultipliedAlpha: attributes?.premultipliedAlpha !== false,
        preserveDrawingBuffer: attributes?.preserveDrawingBuffer === true,
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
      this._vertexArrayCount = 0;
      this._arrayBuffer = null;
      this._defaultVertexArrayState = makeVertexArrayState();
      this._vertexArray = null;
      this._attributesState = this._defaultVertexArrayState.attributes;
      this._elementBuffer = null;
      this._texture = null; this._program = null;
      this._enabledMask = CAP_DITHER;
      this._clearColor = new Float32Array(4); this._clearDepth = 1;
      this._viewport = new Int32Array(
        [0, 0, this.drawingBufferWidth, this.drawingBufferHeight]);
      this._scissor = new Int32Array(
        [0, 0, this.drawingBufferWidth, this.drawingBufferHeight]);
      this._depthFunc = constants.LESS;
      this._blendSrc = constants.ONE; this._blendDst = constants.ZERO;
      this._cullFace = constants.BACK; this._frontFace = constants.CCW;
      this._rasterStateKey = this._computeRasterStateKey();
      this._viewportRevision = 1; this._scissorRevision = 1;
      this._lineWidth = 1; this._flipY = false; this._premultiply = false;
      this._unpackAlignment = 4; this._packAlignment = 4;
      this._commands = []; this._queuedVertices = 0; this._flushQueued = false;
      /* One retained microtask job: the frame loop schedules a flush every
         presented frame, and a fresh closure per frame would be steady
         QuickJS garbage pacing full collections onto gameplay. */
      this._flushMicrotask = () => {
        this._flushQueued = false; this._flush();
        this._forcedFlushesInFrame = 0;
      };
      /* bufferSubData preserves storage and therefore every cached attribute
         range. bufferData replaces storage; one context-wide incarnation
         makes the common draw-plan check constant-sized instead of walking
         six buffer objects through QuickJS on every draw. */
      this._bufferStorageRevision = 1;
      this._forcedFlushesInFrame = 0;
      this._queuedSources = new Set(); this._queuedTextures = new Set();
      this._tailCommands = []; this._tailVertices = 0;
      this._tailSources = new Set(); this._tailTextures = new Set();
      /* Every queued draw owns one immutable record until publication. Draws
         are populated directly in this bounded pool: constructing a second
         staging record and copying its matrices/attributes made each WebGL
         call pay twice for the same state snapshot on the PSP interpreter. */
      this._commandPool = Array.from(
        { length: COMMAND_POOL_LIMIT }, (_, index) => makeCommandRecord(index));
      this._freeCommandIndices = Array.from(
        { length: COMMAND_POOL_LIMIT },
        (_, index) => COMMAND_POOL_LIMIT - index - 1);
      this._matrixScratch = new Float32Array(16);
      this._drawUniformColorScratch = new Float32Array(4);
      this._instancedExtension = null;
      this._vertexArrayExtension = null;
      this._repeatedFrameExtension = null;
      this._repeatedCommandCapture = null;
      this._repeatedCommandStateRevision = 1;
      this._flushSources = [];
      this._flushTextures = [];
      this._flushSourceUsedMask = 0;
      this._flushTextureUsedMask = 0;
      this._sourcePacketRetained = false;
      this._sourcePacketStable = true;
      this._sourcePacketChecked = false;
      this._sourcePacketEpoch = 1;
      /* The packed 32-bit wire is retained per context. Flushes overwrite
         this bounded storage instead of allocating a Float64 command block. */
      this._commandWireBuffer = new ArrayBuffer(
        (WIRE_HEADER_WORDS + MAX_DRAWS * COMMAND_WORDS) * 4);
      this._commandWireU32 = new Uint32Array(this._commandWireBuffer);
      this._commandWireI32 = new Int32Array(this._commandWireBuffer);
      this._commandWireF32 = new Float32Array(this._commandWireBuffer);
      /* A stable animation normally emits the same draw plans into the same
         bounded wire slots every frame. Keep per-context authority over the
         invariant template in each slot, then patch only live fields. */
      this._nextCommandTemplateId = 1;
      this._commandWireTemplateIds = new Uint32Array(MAX_DRAWS);
      this._commandWireTemplateRevisions = new Uint32Array(MAX_DRAWS);
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
      this._invalidateCommandWireTemplates();
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
      this._vertexArrayCount = 0;
      this._arrayBuffer = null; this._elementBuffer = null;
      this._vertexArray = null;
      this._defaultVertexArrayState = makeVertexArrayState();
      this._attributesState = this._defaultVertexArrayState.attributes;
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
      this._vertexArrayCount = 0;
      this._arrayBuffer = null;
      this._defaultVertexArrayState = makeVertexArrayState();
      this._vertexArray = null;
      this._attributesState = this._defaultVertexArrayState.attributes;
      this._elementBuffer = null;
      this._texture = null; this._program = null;
      this._enabledMask = CAP_DITHER;
      this._nextCommandTemplateId = 1;
      this._invalidateCommandWireTemplates();
      this._clearColor.fill(0); this._clearDepth = 1;
      this._depthFunc = constants.LESS;
      this._blendSrc = constants.ONE; this._blendDst = constants.ZERO;
      this._cullFace = constants.BACK; this._frontFace = constants.CCW;
      this._rasterStateKey = this._computeRasterStateKey();
      this._viewportRevision = 1; this._scissorRevision = 1;
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
      queueMicrotask(this._flushMicrotask);
    }
    _beginRepeatedCommandCapture() {
      if (this._lost || this._repeatedCommandCapture
          || this._commands.length || this._tailCommands.length) return false;
      this._repeatedCommandCapture = { records: [], failed: false };
      return true;
    }
    _touchRepeatedCommandState() {
      this._repeatedCommandStateRevision =
        (this._repeatedCommandStateRevision + 1) >>> 0;
      if (this._repeatedCommandStateRevision !== 0) return;
      this._repeatedCommandStateRevision = 1;
      /* Make equality impossible across the theoretical 32-bit wrap. */
      this._advanceSourcePacketEpoch();
    }
    _advanceBufferStorageRevision() {
      this._bufferStorageRevision =
        (this._bufferStorageRevision + 1) >>> 0;
      if (this._bufferStorageRevision !== 0) return;
      this._bufferStorageRevision = 1;
      for (const resource of this._resources)
        if (resource instanceof WebGLVertexArrayOES)
          resource._state.drawPlan = null;
      this._defaultVertexArrayState.drawPlan = null;
    }
    _captureRepeatedCommand(record) {
      const capture = this._repeatedCommandCapture;
      if (!capture || capture.failed) return;
      if (capture.records.length >= MAX_DRAWS) {
        capture.failed = true;
        return;
      }
      capture.records.push(record);
    }
    _repeatedWireTemplate(slot) {
      const base = WIRE_HEADER_WORDS + slot * COMMAND_WORDS,
        template = new Uint32Array(COMMAND_WORDS);
      template.set(this._commandWireU32.subarray(
        base, base + COMMAND_WORDS));
      return template;
    }
    _repeatedSourceBindings(record, plan) {
      const base = WIRE_HEADER_WORDS + record.slot * COMMAND_WORDS,
        bindings = [];
      const retain = (offset, buffer) => {
        const source = this._commandWireI32[base + offset];
        if (source < 0) return true;
        if (!buffer || source >= MAX_NATIVE_SOURCES) return false;
        for (let at = 0; at < bindings.length; at += 2) {
          if (bindings[at] !== source) continue;
          return bindings[at + 1] === buffer;
        }
        bindings.push(source, buffer);
        return true;
      };
      if (!retain(5, record.indexBuffer)
          || !retain(8, plan.position?.enabled
            ? plan.position.buffer : null)
          || !retain(14, plan.color?.enabled
            ? plan.color.buffer : null)
          || !retain(20, plan.texcoord?.enabled
            ? plan.texcoord.buffer : null)
          || !retain(77, plan.instanceMatrix?.enabled
            ? plan.instanceMatrix.buffer : null)
          || !retain(83, plan.instanceColor?.enabled
            ? plan.instanceColor.buffer : null)
          || !retain(89, plan.instanceTransform?.enabled
            ? plan.instanceTransform.buffer : null)) return null;
      return bindings;
    }
    _repeatedGenerationPatches(record, plan) {
      const patches = [];
      const retainGeneration = (offset, buffer) => {
        if (buffer) patches.push(offset, buffer);
      };
      /* STATIC_DRAW describes the author's expected update frequency; it
         does not make bufferSubData a structural mutation.  The retained
         record already binds the live buffer object and a subdata write
         cannot alter its capacity.  Patch every content generation so an
         occasional static scenery refresh reuses the validated list rather
         than abandoning a large object graph for the whole-realm collector. */
      retainGeneration(65, record.indexBuffer);
      retainGeneration(68, plan.position?.enabled
        ? plan.position.buffer : null);
      retainGeneration(71, plan.color?.enabled ? plan.color.buffer : null);
      retainGeneration(74, plan.texcoord?.enabled
        ? plan.texcoord.buffer : null);
      return patches;
    }
    _endRepeatedCommandCapture(drawLimits) {
      const capture = this._repeatedCommandCapture;
      this._repeatedCommandCapture = null;
      if (!capture || capture.failed || !capture.records.length
          || capture.records.length !== this._commands.length) return null;
      let drawCount = 0;
      for (const record of capture.records)
        if (record.kind === 1) drawCount++;
      /* Replay reads one live uniform value per program. Refuse a capture
         whose draws observed different uniform states for the same program
         rather than silently collapsing those draws to the final value. */
      for (let recordAt = 0; recordAt < capture.records.length; recordAt++) {
        const record = capture.records[recordAt];
        if (record.kind !== 1) continue;
        for (let priorAt = 0; priorAt < recordAt; priorAt++) {
          const prior = capture.records[priorAt];
          if (prior.kind === 1 && prior.plan.program === record.plan.program
              && prior.uniformStateRevision !== record.uniformStateRevision)
            return null;
        }
      }
      /* A game often draws the same retained buffers with a changing count
         of instances or HUD indices.  Capturing only the first frame's
         counts forced it to discard and rebuild the list at every new high
         water mark, leaving several retained typed-array snapshots for the
         collector.  An optional pair of (count, instances) limits lets the
         caller declare the fixed buffer capacity once.  Validate that
         capacity against the live vertex/index storage here; execution still
         validates each frame's actual count and the aggregate 4096-vertex
         batch ceiling. */
      if (drawLimits !== undefined
          && (!(drawLimits instanceof Uint16Array)
              || drawLimits.length < drawCount * 2)) return null;
      let drawAt = 0;
      for (const record of capture.records) {
        if (record.kind !== 1) continue;
        const maximumCount = drawLimits
          ? drawLimits[drawAt * 2] : record.maximumCount;
        const maximumInstances = drawLimits
          ? drawLimits[drawAt * 2 + 1] : record.maximumInstances;
        drawAt++;
        if (maximumCount < record.maximumCount
            || maximumInstances < record.maximumInstances
            || maximumInstances > MAX_INSTANCES
            || maximumCount > MAX_VERTICES
            || maximumCount > Math.floor(MAX_VERTICES / maximumInstances))
          return null;
        const plan = record.plan;
        let vertexFirst = record.wireTemplate[2];
        let vertexCount = maximumCount;
        if (record.indexBuffer) {
          const data = record.indexBuffer._data;
          const type = record.wireTemplate[6];
          const offset = record.wireTemplate[7];
          const elementBytes = typeBytes(type);
          if (!(data instanceof Uint8Array) || !elementBytes
              || offset > data.byteLength
              || maximumCount
                > Math.floor((data.byteLength - offset) / elementBytes))
            return null;
          const maximum = typeof nativeIndexMaximum === "function"
            ? nativeIndexMaximum(data, type, offset, maximumCount) : -1;
          if (!Number.isInteger(maximum) || maximum < 0) return null;
          vertexFirst = 0;
          vertexCount = maximum + 1;
        }
        if (!this._attributeRangeAvailable(
              plan.position, vertexFirst, vertexCount)
            || (plan.color?.enabled && !this._attributeRangeAvailable(
              plan.color, vertexFirst, vertexCount))
            || (plan.texcoord?.enabled && !this._attributeRangeAvailable(
              plan.texcoord, vertexFirst, vertexCount))) return null;
        const hasInstanceAttribute = !!(plan.instanceMatrix
          || plan.instanceColor || plan.instanceTransform);
        if (maximumInstances > 1 && !hasInstanceAttribute) return null;
        if ((plan.instanceMatrix?.enabled
              && !this._attributeRangeAvailable(
                plan.instanceMatrix, 0, maximumInstances))
            || (plan.instanceColor?.enabled
              && !this._attributeRangeAvailable(
                plan.instanceColor, 0, maximumInstances))
            || (plan.instanceTransform?.enabled
              && !this._attributeRangeAvailable(
                plan.instanceTransform, 0, maximumInstances))) return null;
        if (plan.instanceMatrix?.enabled) {
          const base = plan.program._translation.instanceMatrixLocation;
          for (let column = 1; column < 4; column++)
            if (!this._attributeRangeAvailable(
                plan.ownerState.attributes[base + column],
                0, maximumInstances)) return null;
        }
        record.maximumCount = maximumCount;
        record.maximumInstances = maximumInstances;
      }
      const commandList = Object.freeze({ drawCount });
      repeatedCommandLists.set(commandList, {
        context: this,
        records: capture.records,
        commandCount: this._commands.length,
        bufferStorageRevision: this._bufferStorageRevision,
        sourcePacketEpoch: this._sourcePacketEpoch,
        stateRevision: this._repeatedCommandStateRevision,
      });
      return commandList;
    }
    _executeRepeatedCommandList(commandList, drawCounts) {
      const list = repeatedCommandLists.get(commandList);
      if (!list || list.context !== this || this._lost
          || this._repeatedCommandCapture || this._commands.length
          || this._tailCommands.length || !this._sourcePacketRetained
          || list.bufferStorageRevision !== this._bufferStorageRevision
          || list.sourcePacketEpoch !== this._sourcePacketEpoch
          || list.stateRevision !== this._repeatedCommandStateRevision
          || !(drawCounts instanceof Uint16Array)
          || drawCounts.length < commandList.drawCount * 2) return false;
      let drawAt = 0, queuedVertices = 0;
      let instancedDraws = 0, instancesDrawn = 0;
      let sourceMask = 0;
      const wireI32 = this._commandWireI32,
        wireU32 = this._commandWireU32,
        wireF32 = this._commandWireF32;
      /* Validate and patch each bounded record in one traversal. A refusal
         leaves the queue empty; partially patched inactive wire slots are
         harmless and the ordinary fallback overwrites them before use. This
         avoids a second property-heavy JS pass over every repeated draw. */
      for (let recordAt = 0; recordAt < list.records.length; recordAt++) {
        const record = list.records[recordAt];
        const base = WIRE_HEADER_WORDS + record.slot * COMMAND_WORDS;
        wireU32.set(record.wireTemplate, base);
        if (record.kind === 0) continue;
        const count = drawCounts[drawAt * 2];
        const instances = drawCounts[drawAt * 2 + 1];
        drawAt++;
        const plan = record.plan;
        if (!count || !instances || count > record.maximumCount
            || instances > record.maximumInstances
            || !plan
            || count > Math.floor(MAX_VERTICES / instances)
            || count * instances > MAX_VERTICES - queuedVertices)
          return false;
        queuedVertices += count * instances;
        if (!this._prepareProgramCombinedMatrix(plan.program)) return false;
        wireU32[base + 3] = count;
        wireU32[base + 76] = instances;
        if (instances > 1) {
          instancedDraws++;
          instancesDrawn += instances;
        }
        wireF32.set(plan.program._combinedMatrix, base + 44);
        if (record.constantColor) {
          wireF32[base + 60] =
            plan.uniformColorValue[0] * record.constantColor[0];
          wireF32[base + 61] =
            plan.uniformColorValue[1] * record.constantColor[1];
          wireF32[base + 62] =
            plan.uniformColorValue[2] * record.constantColor[2];
          wireF32[base + 63] =
            plan.uniformColorValue[3] * record.constantColor[3];
        } else {
          wireF32.set(plan.uniformColorValue, base + 60);
        }
        const sourceBindings = record.sourceBindings;
        for (let at = 0; at < sourceBindings.length; at += 2) {
          const buffer = sourceBindings[at + 1];
          if (buffer._deleted || !(buffer._data instanceof Uint8Array))
            return false;
          this._flushSources[sourceBindings[at]] = buffer._data;
        }
        const generationPatches = record.generationPatches;
        for (let at = 0; at < generationPatches.length; at += 2)
          wireI32[base + generationPatches[at]] =
            generationPatches[at + 1]._generation;
        sourceMask = (sourceMask | record.sourceMask) >>> 0;
      }
      this._commands.length = list.commandCount;
      this._commands.fill(null);
      this._queuedVertices = queuedVertices;
      this._flushSourceUsedMask = sourceMask;
      this._sourcePacketChecked = true;
      diagnostics.drawCalls += commandList.drawCount;
      diagnostics.vertices += queuedVertices;
      diagnostics.instancedDrawCalls += instancedDraws;
      diagnostics.instances += instancesDrawn;
      diagnostics.repeatedCommandListExecutions =
        (diagnostics.repeatedCommandListExecutions || 0) + 1;
      this._schedule();
      return true;
    }
    _resetCommandQueue() {
      this._repeatedCommandCapture = null;
      for (const command of this._commands) this._releaseCommand(command);
      for (const command of this._tailCommands) this._releaseCommand(command);
      this._commands.length = 0; this._queuedVertices = 0;
      this._queuedSources.clear(); this._queuedTextures.clear();
      this._tailCommands.length = 0; this._tailVertices = 0;
      this._tailSources.clear(); this._tailTextures.clear();
      this._flushSources.length = 0;
      this._flushTextures.length = 0;
      this._flushSourceUsedMask = 0;
      this._flushTextureUsedMask = 0;
      this._sourcePacketRetained = false;
      this._sourcePacketStable = true;
      this._sourcePacketChecked = false;
      this._advanceSourcePacketEpoch();
    }
    _invalidateCommandWireTemplates() {
      this._commandWireTemplateIds.fill(0);
      this._commandWireTemplateRevisions.fill(0);
    }
    _invalidateCommandWireTemplate(commandAt) {
      this._commandWireTemplateIds[commandAt] = 0;
      this._commandWireTemplateRevisions[commandAt] = 0;
    }
    _resetCurrentQueue(preserveSourcePacket = false) {
      for (const command of this._commands) this._releaseCommand(command);
      this._commands.length = 0; this._queuedVertices = 0;
      this._queuedSources.clear(); this._queuedTextures.clear();
      if (!preserveSourcePacket) {
        this._flushSources.length = 0;
        this._flushTextures.length = 0;
        this._advanceSourcePacketEpoch();
      }
      this._flushSourceUsedMask = 0;
      this._flushTextureUsedMask = 0;
      this._sourcePacketRetained = preserveSourcePacket;
      this._sourcePacketStable = true;
      this._sourcePacketChecked = false;
    }
    _invalidateSourcePacket() {
      this._flushSources.length = 0;
      this._flushTextures.length = 0;
      this._flushSourceUsedMask = 0;
      this._flushTextureUsedMask = 0;
      this._sourcePacketRetained = false;
      this._sourcePacketStable = true;
      this._sourcePacketChecked = false;
      this._advanceSourcePacketEpoch();
    }
    _advanceSourcePacketEpoch() {
      this._sourcePacketEpoch = (this._sourcePacketEpoch + 1) >>> 0;
      if (this._sourcePacketEpoch !== 0) return;
      this._sourcePacketEpoch = 1;
      /* A four-billion-packet wrap is not a practical page lifetime, but
         clearing the bounded plans keeps this an identity, not a hint. */
      for (const resource of this._resources)
        if (resource instanceof WebGLVertexArrayOES)
          resource._state.drawPlan = null;
      this._defaultVertexArrayState.drawPlan = null;
    }
    _promoteTailQueue() {
      this._commands = this._tailCommands; this._tailCommands = [];
      this._queuedVertices = this._tailVertices; this._tailVertices = 0;
      this._queuedSources = this._tailSources; this._tailSources = new Set();
      this._queuedTextures = this._tailTextures; this._tailTextures = new Set();
    }
    _releaseCommand(command) {
      if (!command?._pooled) return;
      if (!command._inUse) return;
      command._inUse = false;
      command.index = null; command.position = null;
      command.color = null; command.texcoord = null;
      command.instanceMatrix = null;
      command.instanceColor = null;
      command.instanceTransform = null;
      command.texture = null;
      command._indexStorage.data = null;
      command._positionStorage.data = null;
      command._colorStorage.data = null;
      command._texcoordStorage.data = null;
      command._instanceMatrixStorage.data = null;
      command._instanceColorStorage.data = null;
      command._instanceTransformStorage.data = null;
      this._freeCommandIndices.push(command._poolIndex);
    }
    _snapshotLiveAttribute(destination, attribute) {
      if (!attribute?.enabled) { destination.data = null; return null; }
      const buffer = attribute.buffer;
      destination.data = buffer._data;
      destination.id = buffer._id;
      destination.generation = buffer._generation;
      destination.usage = buffer._usage;
      destination.size = attribute.size;
      destination.type = attribute.type;
      destination.normalized = attribute.normalized;
      destination.stride = attribute.stride;
      destination.offset = attribute.offset;
      return destination;
    }
    _takeCommand(kind) {
      const poolIndex = this._freeCommandIndices.pop();
      const destination = poolIndex === undefined
        ? null : this._commandPool[poolIndex];
      if (!destination) return null;
      destination._inUse = true;
      destination.kind = kind;
      return destination;
    }
    _queueFits(commands, queuedVertices, queuedSources, queuedTextures,
               candidateSources, texture, vertices) {
      /* The complete context already admits at most 24 buffer payloads and
         eight texture payloads. Buffer/texture mutations force an ordered
         flush before replacing their storage, so one batch cannot observe
         more identities than that exact 32-source native boundary. Avoid a
         second Set walk for every draw; _sourceIndex still deduplicates the
         actual wire sources and the native bridge validates the final cap.
         A non-null source list is reserved for the validation/deferred seam,
         which can inject identities that are not retained WebGL objects. */
      let addedSources = 0;
      if (candidateSources !== null) {
        for (let at = 0; at < candidateSources.length; at++) {
          const source = candidateSources[at];
          if (!source || queuedSources.has(source)) continue;
          let repeated = false;
          for (let prior = 0; prior < at; prior++)
            if (candidateSources[prior] === source) {
              repeated = true; break;
            }
          if (!repeated) addedSources++;
        }
      }
      const addedTexture = candidateSources !== null && texture
        && !queuedTextures.has(texture) ? 1 : 0;
      return commands.length < MAX_DRAWS
        && vertices <= MAX_VERTICES - queuedVertices
        && addedSources <= MAX_NATIVE_SOURCES - queuedSources.size
        && addedTexture <= MAX_TEXTURE_OBJECTS - queuedTextures.size;
    }
    _selectCommandTail(candidateSources, texture, vertices) {
      let tail = this._tailCommands.length !== 0;
      if (!tail && this._queueFits(
          this._commands, this._queuedVertices, this._queuedSources,
          this._queuedTextures, candidateSources, texture, vertices)) {
        /* The ordinary frame stays in the current packed batch. Avoid
           repeating the same bounded capacity walk below for every draw. */
        return false;
      }
      if (!tail) {
        const flushed = this._flush();
        if (flushed === FLUSH_FAILED) return null;
        tail = flushed === FLUSH_DEFERRED;
      }
      const commands = tail ? this._tailCommands : this._commands,
        queuedVertices = tail ? this._tailVertices : this._queuedVertices,
        queuedSources = tail ? this._tailSources : this._queuedSources,
        queuedTextures = tail ? this._tailTextures : this._queuedTextures;
      if (!this._queueFits(commands, queuedVertices, queuedSources,
          queuedTextures, candidateSources, texture, vertices)) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      return tail;
    }
    _retainCommandInputs(tail, candidateSources, texture, vertices) {
      if (candidateSources !== null) {
        const queuedSources = tail ? this._tailSources : this._queuedSources,
          queuedTextures = tail ? this._tailTextures : this._queuedTextures;
        for (const source of candidateSources)
          if (source) queuedSources.add(source);
        if (texture) queuedTextures.add(texture);
      }
      if (tail) this._tailVertices += vertices;
      else this._queuedVertices += vertices;
    }
    _enqueueCommand(command, candidateSources = [], texture = null,
                    vertices = 0) {
      /* Once a tail exists, every newer command belongs behind it even when
         that command would fit in the older batch. Otherwise a source-heavy
         deferred draw followed by a clear could execute in reverse order. */
      const tail = this._selectCommandTail(
        candidateSources, texture, vertices);
      if (tail === null) return false;
      const commands = tail ? this._tailCommands : this._commands;
      let retainedCommand = command;
      /* Validation injects inert clears directly to force the deferred-tail
         boundary. Keep that bounded seam without putting production draws
         back through the former full staging-copy path. */
      if (!retainedCommand?._pooled || !retainedCommand._inUse) {
        if (retainedCommand?.kind !== "clear") {
          this._setError(constants.INVALID_OPERATION); return false;
        }
        retainedCommand = this._takeCommand("clear");
        if (!retainedCommand) {
          this._setError(constants.OUT_OF_MEMORY); return false;
        }
        retainedCommand.mask = command.mask;
        retainedCommand.clearColor.set(command.clearColor || command.color);
        retainedCommand.clearDepth = command.clearDepth ?? command.depth;
        retainedCommand.scissorEnabled = !!command.scissorEnabled;
        retainedCommand.scissor.set(command.scissor || [0, 0, 0, 0]);
      }
      if (tail) {
        commands.push(retainedCommand);
      } else {
        const commandAt = commands.length;
        commands.push(null);
        this._packCommand(retainedCommand, commandAt);
        this._releaseCommand(retainedCommand);
      }
      this._retainCommandInputs(tail, candidateSources, texture, vertices);
      return true;
    }
    _enqueueClear(mask) {
      const tail = this._selectCommandTail([], null, 0);
      if (tail === null) return false;
      this._touchRepeatedCommandState();
      if (tail) {
        if (this._repeatedCommandCapture)
          this._repeatedCommandCapture.failed = true;
        const command = this._takeCommand("clear");
        if (!command) { this._setError(constants.OUT_OF_MEMORY); return false; }
        command.mask = mask;
        command.clearColor.set(this._clearColor);
        command.clearDepth = this._clearDepth;
        command.scissorEnabled =
          (this._enabledMask & CAP_SCISSOR_TEST) !== 0;
        command.scissor.set(this._scissor);
        this._tailCommands.push(command);
        return true;
      }
      const commandAt = this._commands.length,
        base = WIRE_HEADER_WORDS + commandAt * COMMAND_WORDS,
        wireU32 = this._commandWireU32,
        wireI32 = this._commandWireI32,
        wireF32 = this._commandWireF32;
      this._commands.push(null);
      this._invalidateCommandWireTemplate(commandAt);
      wireI32[base] = 0;
      wireU32[base + 1] = mask;
      wireF32.set(this._clearColor, base + 2);
      wireF32[base + 6] = this._clearDepth;
      wireU32[base + 39] =
        (this._enabledMask & CAP_SCISSOR_TEST) !== 0 ? 1 : 0;
      wireI32.set(this._scissor, base + 40);
      this._captureRepeatedCommand({
        kind: 0, slot: commandAt,
        wireTemplate: this._repeatedWireTemplate(commandAt),
      });
      return true;
    }
    _sourceIndex(sources, sourceMap, bytes) {
      if (!bytes) return -1;
      /* PSP WebGL batches admit at most 32 source views, and ordinary game
         frames use far fewer. Array#indexOf performs the pointer comparison
         in native code; maintaining a QuickJS Map here cost more than the
         bounded linear lookup on every draw admission. Keep the parameter so
         deferred validation records retain the same helper contract. */
      let index = sources.indexOf(bytes);
      if (index < 0) {
        if (sources.length < MAX_NATIVE_SOURCES) {
          index = sources.length;
          sources.push(bytes);
        } else {
          /* A retained packet can contain a source which disappeared from
             this frame. Recycle only a slot no command has referenced; this
             keeps source indices stable without letting stale identities
             consume the bounded 32-source native contract forever. */
          index = -1;
          for (let at = 0; at < MAX_NATIVE_SOURCES; at++)
            if (!(this._flushSourceUsedMask & (1 << at))) {
              index = at; break;
            }
          if (index < 0) return MAX_NATIVE_SOURCES;
          this._advanceSourcePacketEpoch();
          sources[index] = bytes;
        }
      }
      this._flushSourceUsedMask =
        (this._flushSourceUsedMask | (1 << index)) >>> 0;
      return index;
    }
    _textureIndex(textures, texture) {
      if (!texture) return -1;
      let index = textures.indexOf(texture);
      if (index < 0) {
        if (textures.length < MAX_TEXTURE_OBJECTS) {
          index = textures.length;
          textures.push(texture);
        } else {
          index = -1;
          for (let at = 0; at < MAX_TEXTURE_OBJECTS; at++)
            if (!(this._flushTextureUsedMask & (1 << at))) {
              index = at; break;
            }
          if (index < 0) return MAX_TEXTURE_OBJECTS;
          textures[index] = texture;
        }
      }
      this._flushTextureUsedMask |= 1 << index;
      return index;
    }
    _packCommand(command, commandAt) {
      this._invalidateCommandWireTemplate(commandAt);
      const sources = this._flushSources,
        textures = this._flushTextures,
        wireU32 = this._commandWireU32,
        wireI32 = this._commandWireI32,
        wireF32 = this._commandWireF32,
        base = WIRE_HEADER_WORDS + commandAt * COMMAND_WORDS;
      if (command.kind === "clear") {
        wireI32[base] = 0; wireU32[base + 1] = command.mask;
        wireF32.set(command.clearColor || command.color, base + 2);
        wireF32[base + 6] = command.clearDepth ?? command.depth;
        wireI32.set(command.scissor, base + 40);
        wireU32[base + 39] = command.scissorEnabled ? 1 : 0;
        return;
      }
      const draw = command, position = draw.position,
        color = draw.color, texcoord = draw.texcoord,
        index = draw.index;
      wireI32[base] = 1; wireI32[base + 1] = draw.mode;
      wireU32[base + 2] = draw.first; wireU32[base + 3] = draw.count;
      wireU32[base + 4] = index ? 1 : 0;
      wireI32[base + 5] = this._sourceIndex(sources, null, index?.data);
      wireI32[base + 6] = index?.type || 0;
      wireU32[base + 7] = index?.offset || 0;
      writeCommandAttribute(
        this, wireI32, wireU32, base, 8,
        position, sources, null);
      writeCommandAttribute(
        this, wireI32, wireU32, base, 14,
        color, sources, null);
      writeCommandAttribute(
        this, wireI32, wireU32, base, 20,
        texcoord, sources, null);
      const textureIndex = this._textureIndex(textures, draw.texture);
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
      writeCommandSourceIdentity(wireI32, base, 64, index);
      writeCommandSourceIdentity(wireI32, base, 67, position);
      writeCommandSourceIdentity(wireI32, base, 70, color);
      writeCommandSourceIdentity(wireI32, base, 73, texcoord);
      wireU32[base + 76] = draw.instanceCount || 1;
      writeCommandAttribute(
        this, wireI32, wireU32, base, 77,
        draw.instanceMatrix, sources, null);
      writeCommandAttribute(
        this, wireI32, wireU32, base, 83,
        draw.instanceColor, sources, null);
      writeCommandAttribute(
        this, wireI32, wireU32, base, 89,
        draw.instanceTransform, sources, null);
      wireU32[base + 95] = 0;
    }
    _computeRasterStateKey() {
      const sourceBlendKey = this._blendSrc === constants.ZERO ? 0
        : this._blendSrc === constants.ONE ? 1
        : this._blendSrc === constants.SRC_ALPHA ? 2 : 3;
      const destinationBlendKey = this._blendDst === constants.ZERO ? 0
        : this._blendDst === constants.ONE ? 1
        : this._blendDst === constants.SRC_ALPHA ? 2 : 3;
      const cullKey = this._cullFace === constants.FRONT ? 0
        : this._cullFace === constants.BACK ? 1 : 2;
      return ((this._enabledMask & (CAP_BLEND | CAP_CULL_FACE
          | CAP_DEPTH_TEST | CAP_SCISSOR_TEST))
        | ((this._depthFunc - constants.NEVER) << 5)
        | (sourceBlendKey << 8)
        | (destinationBlendKey << 10)
        | (cullKey << 12)
        | ((this._frontFace - constants.CW) << 14)) >>> 0;
    }
    _drawTemplateStateMatches(plan) {
      return !!plan.commandTemplateU32
        && plan.commandTemplateRasterKey === this._rasterStateKey
        && plan.commandTemplateViewportRevision === this._viewportRevision
        && plan.commandTemplateScissorRevision === this._scissorRevision;
    }
    _prepareDrawTemplate(plan, mode, first, count, index,
                         position, color, texcoord,
                         instanceMatrix, instanceColor, instanceTransform) {
      if (!plan.commandTemplateU32) {
        plan.commandTemplateU32 = new Uint32Array(COMMAND_WORDS);
        plan.commandTemplateI32 = new Int32Array(
          plan.commandTemplateU32.buffer);
      }
      const wireU32 = plan.commandTemplateU32,
        wireI32 = plan.commandTemplateI32;
      wireU32.fill(0);
      wireI32[0] = 1; wireI32[1] = mode;
      wireU32[2] = first; wireU32[3] = count;
      wireU32[4] = index ? 1 : 0;
      wireI32[5] = -1; wireI32[6] = index?.type || 0;
      wireU32[7] = index?.offset || 0;
      writeTemplateAttribute(wireI32, wireU32, 8, position);
      writeTemplateAttribute(wireI32, wireU32, 14, color);
      writeTemplateAttribute(wireI32, wireU32, 20, texcoord);
      wireI32[26] = -1;
      wireU32[27] = (this._enabledMask & CAP_BLEND) !== 0 ? 1 : 0;
      wireU32[28] = (this._enabledMask & CAP_DEPTH_TEST) !== 0 ? 1 : 0;
      wireU32[29] = (this._enabledMask & CAP_CULL_FACE) !== 0 ? 1 : 0;
      wireI32[30] = this._depthFunc;
      wireI32[31] = this._blendSrc; wireI32[32] = this._blendDst;
      wireI32[33] = this._cullFace; wireI32[34] = this._frontFace;
      wireI32.set(this._viewport, 35);
      wireU32[39] = (this._enabledMask & CAP_SCISSOR_TEST) !== 0 ? 1 : 0;
      wireI32.set(this._scissor, 40);
      writeCommandSourceIdentity(wireI32, 0, 64, index);
      writeLiveSourceIdentity(wireI32, 0, 67, position);
      writeLiveSourceIdentity(wireI32, 0, 70, color);
      writeLiveSourceIdentity(wireI32, 0, 73, texcoord);
      writeTemplateAttribute(wireI32, wireU32, 77, instanceMatrix);
      writeTemplateAttribute(wireI32, wireU32, 83, instanceColor);
      writeTemplateAttribute(wireI32, wireU32, 89, instanceTransform);
      plan.commandTemplateRasterKey = this._rasterStateKey;
      plan.commandTemplateViewportRevision = this._viewportRevision;
      plan.commandTemplateScissorRevision = this._scissorRevision;
      plan.commandTemplateRevision =
        ((plan.commandTemplateRevision || 0) + 1) >>> 0;
      if (plan.commandTemplateRevision === 0)
        plan.commandTemplateRevision = 1;
      if (!plan.commandTemplateId) {
        plan.commandTemplateId = this._nextCommandTemplateId;
        this._nextCommandTemplateId =
          (this._nextCommandTemplateId + 1) >>> 0;
        if (this._nextCommandTemplateId === 0) {
          this._nextCommandTemplateId = 1;
          this._invalidateCommandWireTemplates();
        }
      }
    }
    _enqueueDraw(mode, first, count, index, position, color, texcoord,
                 textureSnapshot, instanceCount, instanceMatrix,
                 instanceColor, instanceTransform, matrix, uniformColor,
                 candidateSources, vertices, drawPlan) {
      const profile = diagnostics.profileDrawPhases;
      let profileAt = profile ? performance.now() : 0,
        profilePackStarted = profileAt;
      /* Ordinary WebGL draws carry resources through retained buffer
         objects, so candidateSources is null. Keep their overwhelmingly
         common in-budget admission in this frame instead of paying two
         nested JS calls for the same bounded arithmetic on every draw. */
      const directCurrent = candidateSources === null
        && this._tailCommands.length === 0
        && this._commands.length < MAX_DRAWS
        && vertices <= MAX_VERTICES - this._queuedVertices;
      const tail = directCurrent ? false : this._selectCommandTail(
        candidateSources, textureSnapshot, vertices);
      if (profile) {
        const now = performance.now();
        diagnostics.profileQueueAdmissionMs += now - profileAt;
        profileAt = now;
        profilePackStarted = now;
      }
      if (tail === null) return false;
      this._touchRepeatedCommandState();
      if (tail) {
        if (this._repeatedCommandCapture)
          this._repeatedCommandCapture.failed = true;
        const command = this._takeCommand("draw");
        if (!command) { this._setError(constants.OUT_OF_MEMORY); return false; }
        command.matrix.set(matrix); command.uniformColor.set(uniformColor);
        if (index) {
          const retainedIndex = command._indexStorage;
          retainedIndex.data = index.data;
          retainedIndex.id = index.id;
          retainedIndex.generation = index.generation;
          retainedIndex.usage = index.usage;
          retainedIndex.type = index.type;
          retainedIndex.offset = index.offset;
          retainedIndex.maximum = index.maximum;
          command.index = retainedIndex;
        } else command.index = null;
        command.mode = mode; command.first = first; command.count = count;
        command.position = this._snapshotLiveAttribute(
          command._positionStorage, position);
        command.color = this._snapshotLiveAttribute(
          command._colorStorage, color);
        command.texcoord = this._snapshotLiveAttribute(
          command._texcoordStorage, texcoord);
        command.texture = textureSnapshot;
        command.instanceCount = instanceCount;
        command.instanceMatrix = this._snapshotLiveAttribute(
          command._instanceMatrixStorage, instanceMatrix);
        command.instanceColor = this._snapshotLiveAttribute(
          command._instanceColorStorage, instanceColor);
        command.instanceTransform = this._snapshotLiveAttribute(
          command._instanceTransformStorage, instanceTransform);
        command.blend = (this._enabledMask & CAP_BLEND) !== 0;
        command.depth = (this._enabledMask & CAP_DEPTH_TEST) !== 0;
        command.cull = (this._enabledMask & CAP_CULL_FACE) !== 0;
        command.depthFunc = this._depthFunc;
        command.blendSrc = this._blendSrc; command.blendDst = this._blendDst;
        command.cullFace = this._cullFace; command.frontFace = this._frontFace;
        command.viewport.set(this._viewport);
        command.scissorEnabled =
          (this._enabledMask & CAP_SCISSOR_TEST) !== 0;
        command.scissor.set(this._scissor);
        this._tailCommands.push(command);
        this._retainCommandInputs(
          true, candidateSources, textureSnapshot, vertices);
        if (profile)
          diagnostics.profileWirePackMs += performance.now() - profilePackStarted;
        return true;
      }
      const commandAt = this._commands.length,
        base = WIRE_HEADER_WORDS + commandAt * COMMAND_WORDS,
        sources = this._flushSources,
        textures = this._flushTextures,
        wireU32 = this._commandWireU32,
        wireI32 = this._commandWireI32,
        wireF32 = this._commandWireF32;
      this._commands.push(null);
      if (!this._drawTemplateStateMatches(drawPlan)) {
        diagnostics.commandTemplateMisses++;
        this._prepareDrawTemplate(
          drawPlan, mode, first, count, index, position, color, texcoord,
          instanceMatrix, instanceColor, instanceTransform);
      } else diagnostics.commandTemplateHits++;
      const slotTemplateCurrent =
        this._commandWireTemplateIds[commandAt]
             === drawPlan.commandTemplateId
        && this._commandWireTemplateRevisions[commandAt]
             === drawPlan.commandTemplateRevision;
      const firstPacketDraw = this._sourcePacketRetained
        && !this._sourcePacketChecked;
      if (firstPacketDraw) {
        this._sourcePacketChecked = true;
        if (slotTemplateCurrent) diagnostics.sourcePacketHits++;
        else {
          diagnostics.sourcePacketMisses++;
          /* No command can reference the old packet yet, so discard it now
             instead of carrying irrelevant sources through this frame. */
          this._invalidateSourcePacket();
          this._sourcePacketChecked = true;
        }
      }
      if (slotTemplateCurrent) diagnostics.commandSlotTemplateHits++;
      else {
        if (!firstPacketDraw && commandAt !== 0)
          this._sourcePacketStable = false;
        diagnostics.commandSlotTemplateMisses++;
        wireU32.set(drawPlan.commandTemplateU32, base);
        this._commandWireTemplateIds[commandAt] =
          drawPlan.commandTemplateId;
        this._commandWireTemplateRevisions[commandAt] =
          drawPlan.commandTemplateRevision;
      }
      /* Retained bufferData storage and an unchanged draw plan guarantee the
         same source objects. When the previous frame preserved this packet
         and this exact template slot, its bounded source indices remain
         authoritative; only generations and live state need patching. */
      const packetIndexHit = this._sourcePacketRetained
        && slotTemplateCurrent && textureSnapshot === null
        && drawPlan.commandPacketEpoch === this._sourcePacketEpoch
        && drawPlan.commandPacketSlot === commandAt;
      if (packetIndexHit) {
        diagnostics.sourcePacketIndexHits++;
        this._flushSourceUsedMask = (this._flushSourceUsedMask
          | drawPlan.commandPacketMask) >>> 0;
      } else {
        diagnostics.sourcePacketIndexMisses++;
        wireI32[base + 5] = this._sourceIndex(
          sources, null, index?.data);
        wireI32[base + 8] = this._sourceIndex(
          sources, null, position?.enabled ? position.buffer?._data : null);
        wireI32[base + 14] = this._sourceIndex(
          sources, null, color?.enabled ? color.buffer?._data : null);
        wireI32[base + 20] = this._sourceIndex(
          sources, null, texcoord?.enabled ? texcoord.buffer?._data : null);
        wireI32[base + 26] = this._textureIndex(textures, textureSnapshot);
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileWireSourcesMs += now - profileAt;
        profileAt = now;
      }
      wireF32.set(matrix, base + 44);
      wireF32.set(uniformColor, base + 60);
      if (profile) {
        const now = performance.now();
        diagnostics.profileWireStateMs += now - profileAt;
        profileAt = now;
      }
      wireI32[base + 65] = index?.generation || 0;
      wireI32[base + 68] = position?.enabled
        ? position.buffer?._generation || 0 : 0;
      wireI32[base + 71] = color?.enabled
        ? color.buffer?._generation || 0 : 0;
      wireI32[base + 74] = texcoord?.enabled
        ? texcoord.buffer?._generation || 0 : 0;
      /* Repeated-list execution patches the live slot's bounded draw count.
         An ordinary frame may then hit the same retained plan/template slot,
         so restore this per-draw value just as we do the instance count. */
      wireU32[base + 3] = count;
      wireU32[base + 76] = instanceCount;
      if (!packetIndexHit) {
        wireI32[base + 77] = this._sourceIndex(sources, null,
          instanceMatrix?.enabled ? instanceMatrix.buffer?._data : null);
        wireI32[base + 83] = this._sourceIndex(sources, null,
          instanceColor?.enabled ? instanceColor.buffer?._data : null);
        wireI32[base + 89] = this._sourceIndex(sources, null,
          instanceTransform?.enabled ? instanceTransform.buffer?._data : null);
        if (textureSnapshot === null) {
          let packetMask = 0, packetIndex = wireI32[base + 5];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 8];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 14];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 20];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 77];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 83];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          packetIndex = wireI32[base + 89];
          if (packetIndex >= 0 && packetIndex < MAX_NATIVE_SOURCES)
            packetMask |= 1 << packetIndex;
          drawPlan.commandPacketEpoch = this._sourcePacketEpoch;
          drawPlan.commandPacketSlot = commandAt;
          drawPlan.commandPacketMask = packetMask >>> 0;
        }
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileWireInstancesMs += now - profileAt;
        profileAt = now;
      }
      if (candidateSources === null) this._queuedVertices += vertices;
      else this._retainCommandInputs(
        false, candidateSources, textureSnapshot, vertices);
      if (this._repeatedCommandCapture) {
        if (textureSnapshot !== null
            || drawPlan.commandPacketEpoch !== this._sourcePacketEpoch) {
          this._repeatedCommandCapture.failed = true;
        } else {
          const record = {
            kind: 1,
            slot: commandAt,
            plan: drawPlan,
            indexBuffer: drawPlan.ownerState.elementBuffer,
            maximumCount: count,
            maximumInstances: instanceCount,
            uniformStateRevision:
              drawPlan.program._uniformStateRevision,
            constantColor: drawPlan.program._translation.color
                && !drawPlan.color?.enabled
              ? new Float32Array(drawPlan.color?.constant
                || DEFAULT_UNIFORM_COLOR)
              : null,
            sourceMask: drawPlan.commandPacketMask,
            templateId: this._commandWireTemplateIds[commandAt],
            templateRevision:
              this._commandWireTemplateRevisions[commandAt],
            wireTemplate: this._repeatedWireTemplate(commandAt),
          };
          record.sourceBindings =
            this._repeatedSourceBindings(record, drawPlan);
          if (record.sourceBindings === null) {
            this._repeatedCommandCapture.failed = true;
            return true;
          }
          record.generationPatches =
            this._repeatedGenerationPatches(record, drawPlan);
          this._captureRepeatedCommand(record);
        }
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileWireRetainMs += now - profileAt;
        diagnostics.profileWirePackMs += now - profilePackStarted;
      }
      return true;
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
      const sources = this._flushSources,
        textures = this._flushTextures,
        wireU32 = this._commandWireU32;
      wireU32[0] = COMMAND_WIRE_MAGIC; wireU32[1] = WIRE_VERSION;
      wireU32[2] = COMMAND_WORDS; wireU32[3] = this._commands.length;
      let commandAt = 0;
      for (const command of this._commands) {
        if (command !== null) this._packCommand(command, commandAt);
        commandAt++;
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
          sources, null, texture._pixels);
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
        this._textureWireU32, this._attributes.antialias,
        this._attributes.preserveDrawingBuffer,
      );
      if (outcome === 2) {
        diagnostics.deferredFlushes++;
        return FLUSH_DEFERRED;
      }
      const preserveSourcePacket = this._sourcePacketStable
        && this._tailCommands.length === 0;
      this._resetCurrentQueue(preserveSourcePacket);
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
      this._viewport[0] = 0; this._viewport[1] = 0;
      this._viewport[2] = this.drawingBufferWidth;
      this._viewport[3] = this.drawingBufferHeight;
      this._scissor.set(this._viewport);
      this._surfaceReady = false;
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
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
      if (this._arrayBuffer === buffer) this._arrayBuffer = null;
      const vertexArrays = [this._defaultVertexArrayState];
      for (const resource of this._resources)
        if (resource instanceof WebGLVertexArrayOES)
          vertexArrays.push(resource._state);
      for (const state of vertexArrays) {
        let changed = false;
        if (state.elementBuffer === buffer) {
          state.elementBuffer = null;
          changed = true;
        }
        for (const attribute of state.attributes)
          if (attribute.buffer === buffer) {
            attribute.buffer = null;
            attribute.capacityGeneration = 0;
            attribute.capacity = 0;
            changed = true;
          }
        if (changed) state.revision++;
      }
      this._touchRepeatedCommandState();
      if (this._elementBuffer === buffer) this._elementBuffer = null;
      this._bufferBytes -= buffer._data.byteLength;
      if (flushOutcome === FLUSH_OK) this._invalidateSourcePacket();
      buffer._data = new Uint8Array();
      buffer._deleted = true; this._bufferCount--; this._resources.delete(buffer);
    }
    isBuffer(buffer) { return owned(this, buffer, WebGLBuffer); }
    bindBuffer(target, buffer) {
      if (![constants.ARRAY_BUFFER, constants.ELEMENT_ARRAY_BUFFER].includes(target)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      if ((target === constants.ARRAY_BUFFER && buffer === this._arrayBuffer)
          || (target === constants.ELEMENT_ARRAY_BUFFER
              && buffer === this._elementBuffer)) return;
      const admitted = this._object(buffer, WebGLBuffer);
      if (admitted === false) return;
      if (target === constants.ARRAY_BUFFER) this._arrayBuffer = admitted;
      else {
        this._elementBuffer = admitted;
        const state = this._vertexArray?._state
          || this._defaultVertexArrayState;
        state.elementBuffer = admitted;
        state.revision++;
        this._touchRepeatedCommandState();
      }
    }

    _createVertexArray() {
      if (this._lost) return null;
      if (this._vertexArrayCount >= MAX_VERTEX_ARRAY_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const array = new WebGLVertexArrayOES(this, this._nextId++);
      this._vertexArrayCount++;
      this._resources.add(array);
      return array;
    }
    _deleteVertexArray(array) {
      if (array === null || array === undefined) return;
      if (!owned(this, array, WebGLVertexArrayOES)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (this._vertexArray === array) this._bindVertexArray(null);
      array._state.revision++;
      this._touchRepeatedCommandState();
      array._deleted = true;
      array._state.elementBuffer = null;
      for (const attribute of array._state.attributes) attribute.buffer = null;
      this._vertexArrayCount--;
      this._resources.delete(array);
    }
    _bindVertexArray(array) {
      if (array === this._vertexArray) return;
      const admitted = this._object(array, WebGLVertexArrayOES);
      if (admitted === false) return;
      this._vertexArray = admitted;
      const state = admitted?._state || this._defaultVertexArrayState;
      this._attributesState = state.attributes;
      this._elementBuffer = state.elementBuffer;
      if (admitted) admitted._everBound = true;
    }
    bufferData(target, value, usage) {
      const buffer = target === constants.ARRAY_BUFFER ? this._arrayBuffer
        : target === constants.ELEMENT_ARRAY_BUFFER ? this._elementBuffer : false;
      if (buffer === false) { this._setError(constants.INVALID_ENUM); return; }
      if (!buffer) { this._setError(constants.INVALID_OPERATION); return; }
      if (![constants.STATIC_DRAW, constants.DYNAMIC_DRAW, constants.STREAM_DRAW].includes(usage)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
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
      if (flushOutcome === FLUSH_OK) this._invalidateSourcePacket();
      buffer._data = bytes; buffer._usage = usage;
      buffer._generation++;
      buffer._storageGeneration++;
      this._advanceBufferStorageRevision();
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
      /* Uint8Array already is the exact byte view bufferSubData needs. Games
         commonly retain one such view over a changing Float32 instance
         prefix; wrapping it again every frame only creates QuickJS garbage
         and can turn an otherwise bounded frame into a collection spike. */
      const bytes = value instanceof Uint8Array ? value
        : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      if (offset > buffer._data.byteLength || bytes.byteLength > buffer._data.byteLength - offset) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (flushOutcome === FLUSH_DEFERRED) {
        buffer._data = buffer._data.slice();
        /* Commands already handed to the detached bridge retain the old
           backing object. Future draws must rebuild their packet against the
           copy instead of treating an equal-sized buffer as the same source. */
        buffer._storageGeneration++;
        this._advanceBufferStorageRevision();
      }
      buffer._data.set(bytes, offset);
      buffer._generation++;
      /* Repeated lists patch the generation of every retained buffer. A
         subdata write preserves storage and attribute capacity regardless of
         its usage hint, so it does not invalidate their structural proof. */
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
      this._touchRepeatedCommandState();
      const translation = translateProgram(program);
      if (translation.error) {
        program._linked = false; program._log = translation.error;
        diagnostics.shaderRefusals++; return;
      }
      const used = new Set();
      program._attributes = translation.attributes.map((entry, natural) => {
        const slots = entry.type === "mat4" ? 4 : 1;
        let location = program._boundAttributes?.get(entry.name);
        const runAvailable = (base) => Number.isInteger(base) && base >= 0
          && base <= 8 - slots
          && Array.from({ length: slots }, (_, at) => base + at)
            .every((candidate) => !used.has(candidate));
        if (!runAvailable(location)) {
          location = 0;
          while (location < 8 && !runAvailable(location)) location++;
        }
        if (!runAvailable(location)) return { ...entry, location: -1 };
        for (let at = 0; at < slots; at++) used.add(location + at);
        return { ...entry, location };
      });
      if (program._attributes.some((entry) => entry.location < 0)) {
        program._linked = false;
        program._log = "Attribute locations exceed the PSP WebGL limits";
        diagnostics.shaderRefusals++;
        return;
      }
      program._uniforms = translation.uniforms;
      translation.positionLocation = translation.position
        ? program._attributes.find((entry) =>
          entry.name === translation.position.name)?.location ?? -1
        : -1;
      translation.colorLocation = translation.color
        ? program._attributes.find((entry) =>
          entry.name === translation.color.name)?.location ?? -1
        : -1;
      translation.texcoordLocation = translation.texcoord
        ? program._attributes.find((entry) =>
          entry.name === translation.texcoord.name)?.location ?? -1
        : -1;
      translation.instanceMatrixLocation = translation.instanceMatrix
        ? program._attributes.find((entry) =>
          entry.name === translation.instanceMatrix.name)?.location ?? -1
        : -1;
      translation.instanceColorLocation = translation.instanceColor
        ? program._attributes.find((entry) =>
          entry.name === translation.instanceColor.name)?.location ?? -1
        : -1;
      translation.instanceTransformLocation = translation.instanceTransform
        ? program._attributes.find((entry) =>
          entry.name === translation.instanceTransform.name)?.location ?? -1
        : -1;
      program._translation = translation; program._linked = true; program._log = "";
      program._linkRevision++;
      program._matrixRevision = 1;
      program._combinedMatrixRevision = 0;
      program._combinedMatrix = program._combinedMatrixStorage;
      program._combinedMatrix.fill(0);
      program._combinedMatrix[0] = program._combinedMatrix[5]
        = program._combinedMatrix[10] = program._combinedMatrix[15] = 1;
      program._matrixPack.fill(0);
      program._matrixCount = translation.matrices.length;
      const matrixSlots = new Map();
      for (let index = 0; index < translation.matrices.length; index++)
        matrixSlots.set(translation.matrices[index].name, index);
      for (const uniform of program._uniforms) {
        if (uniform.type === "mat4") {
          const slot = matrixSlots.get(uniform.name);
          const matrix = slot === undefined ? identityMatrix()
            : program._matrixPack.subarray(slot * 16, slot * 16 + 16);
          matrix.fill(0);
          matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
          program._uniformValues.set(uniform.name, matrix);
          if (translation.matrices.length === 1 && slot === 0)
            program._combinedMatrix = matrix;
        }
        else if (uniform.type === "vec4") program._uniformValues.set(uniform.name, new Float32Array([1, 1, 1, 1]));
        else program._uniformValues.set(uniform.name, 0);
      }
    }
    validateProgram(program) { if (owned(this, program, WebGLProgram)) program._validated = program._linked; }
    useProgram(program) {
      if (program === this._program) return;
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
      this._touchRepeatedCommandState();
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
    _touchProgramUniformState(program) {
      program._uniformStateRevision =
        (program._uniformStateRevision + 1) >>> 0;
      if (program._uniformStateRevision === 0)
        program._uniformStateRevision = 1;
    }
    _uniform(location, value, acceptedType) {
      if (location === null || this._lost) return;
      if (!(location instanceof WebGLUniformLocation)
          || location._program !== this._program
          || location._type !== acceptedType) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      location._program._uniformValues.set(location._name, value);
      this._touchProgramUniformState(location._program);
      if (location._type === "mat4") location._program._matrixRevision++;
    }
    _uniformScalars(location, acceptedType, count, x, y, z, w) {
      if (location === null || this._lost) return;
      if (!(location instanceof WebGLUniformLocation)
          || location._program !== this._program
          || location._type !== acceptedType) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      x = Number(x); y = Number(y); z = Number(z); w = Number(w);
      if (!Number.isFinite(x) || (count > 1 && !Number.isFinite(y))
          || (count > 2 && !Number.isFinite(z))
          || (count > 3 && !Number.isFinite(w))) {
        this._setError(constants.INVALID_VALUE); return;
      }
      let retained = location._program._uniformValues.get(location._name);
      if (!(retained instanceof Float32Array) || retained.length !== count) {
        retained = new Float32Array(count);
        location._program._uniformValues.set(location._name, retained);
      }
      retained[0] = x;
      if (count > 1) retained[1] = y;
      if (count > 2) retained[2] = z;
      if (count > 3) retained[3] = w;
      this._touchProgramUniformState(location._program);
    }
    _uniformFloatArray(location, value, count, acceptedType) {
      if (location === null || this._lost) return;
      if (!(location instanceof WebGLUniformLocation)
          || location._program !== this._program
          || location._type !== acceptedType) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if ((!ArrayBuffer.isView(value) && !Array.isArray(value))
          || value.length < count) {
        this._setError(constants.INVALID_VALUE); return;
      }
      const nativeCopy = value instanceof Float32Array
        && value.length === count
        && typeof nativeFiniteFloat32 === "function";
      if (nativeCopy) {
        if (!nativeFiniteFloat32(value, count)) {
          this._setError(constants.INVALID_VALUE); return;
        }
      } else {
        for (let index = 0; index < count; index++)
          if (!Number.isFinite(Number(value[index]))) {
            this._setError(constants.INVALID_VALUE); return;
          }
      }
      let retained = location._program._uniformValues.get(location._name);
      if (!(retained instanceof Float32Array) || retained.length !== count) {
        retained = new Float32Array(count);
        location._program._uniformValues.set(location._name, retained);
      }
      if (nativeCopy) retained.set(value);
      else for (let index = 0; index < count; index++)
        retained[index] = Number(value[index]);
      this._touchProgramUniformState(location._program);
      if (acceptedType === "mat4") location._program._matrixRevision++;
    }
    uniform1i(location, value) {
      this._uniform(location, toWebGLInt32(value), "sampler2D");
    }
    uniform1f(location, value) { const v = Number(value); if (Number.isFinite(v)) this._uniform(location, v, "float"); else this._setError(constants.INVALID_VALUE); }
    uniform2f(location, x, y) { this._uniformScalars(location, "vec2", 2, x, y, 0, 0); }
    uniform3f(location, x, y, z) { this._uniformScalars(location, "vec3", 3, x, y, z, 0); }
    uniform4f(location, x, y, z, w) { this._uniformScalars(location, "vec4", 4, x, y, z, w); }
    uniform1fv(location, value) { this._uniformFloatArray(location, value, 1, "float"); }
    uniform2fv(location, value) { this._uniformFloatArray(location, value, 2, "vec2"); }
    uniform3fv(location, value) { this._uniformFloatArray(location, value, 3, "vec3"); }
    uniform4fv(location, value) { this._uniformFloatArray(location, value, 4, "vec4"); }
    uniformMatrix4fv(location, transpose, value) {
      if (transpose) { this._setError(constants.INVALID_VALUE); return; }
      this._uniformFloatArray(location, value, 16, "mat4");
    }
    getUniform(program, location) {
      if (!owned(this, program, WebGLProgram)
          || !(location instanceof WebGLUniformLocation)
          || location._program !== program) {
        this._setError(constants.INVALID_OPERATION);
        return null;
      }
      const value = program._uniformValues.get(location._name);
      /* WebGL returns a value snapshot. Exposing the retained typed array
         would let page code mutate a matrix without a uniform setter and
         bypass the draw-time state/accounting boundary. */
      return ArrayBuffer.isView(value) ? new value.constructor(value)
        : Array.isArray(value) ? value.slice()
        : value ?? null;
    }
    enableVertexAttribArray(index) {
      if (!Number.isInteger(index) || index < 0 || index >= 8) this._setError(constants.INVALID_VALUE);
      else if (!this._attributesState[index].enabled) {
        this._attributesState[index].enabled = true;
        (this._vertexArray?._state || this._defaultVertexArrayState)
          .revision++;
        this._touchRepeatedCommandState();
      }
    }
    disableVertexAttribArray(index) {
      if (!Number.isInteger(index) || index < 0 || index >= 8) this._setError(constants.INVALID_VALUE);
      else if (this._attributesState[index].enabled) {
        this._attributesState[index].enabled = false;
        (this._vertexArray?._state || this._defaultVertexArrayState)
          .revision++;
        this._touchRepeatedCommandState();
      }
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
      const state = this._attributesState[index];
      if (state.buffer === this._arrayBuffer && state.size === size
          && state.type === type && state.normalized === !!normalized
          && state.stride === stride && state.offset === offset) return;
      state.buffer = this._arrayBuffer; state.size = size; state.type = type;
      state.normalized = !!normalized; state.stride = stride;
      state.offset = offset; state.capacityGeneration = 0;
      (this._vertexArray?._state || this._defaultVertexArrayState)
        .revision++;
      this._touchRepeatedCommandState();
    }
    vertexAttrib1f(index, x) { this.vertexAttrib4f(index, x, 0, 0, 1); }
    vertexAttrib2f(index, x, y) { this.vertexAttrib4f(index, x, y, 0, 1); }
    vertexAttrib3f(index, x, y, z) { this.vertexAttrib4f(index, x, y, z, 1); }
    vertexAttrib4f(index, x, y, z, w) {
      const state = this._attributesState[index];
      x = Number(x); y = Number(y); z = Number(z); w = Number(w);
      if (!state || !Number.isFinite(x) || !Number.isFinite(y)
          || !Number.isFinite(z) || !Number.isFinite(w)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      state.constant[0] = x; state.constant[1] = y;
      state.constant[2] = z; state.constant[3] = w;
      this._touchRepeatedCommandState();
    }
    getVertexAttrib(index, parameter) {
      const state = this._attributesState[index]; if (!state) { this._setError(constants.INVALID_VALUE); return null; }
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_ENABLED) return state.enabled;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_SIZE) return state.size;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_STRIDE) return state.stride;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_TYPE) return state.type;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_NORMALIZED) return state.normalized;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) return state.buffer;
      if (parameter === constants.VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE)
        return state.divisor;
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
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
      if (this._texture === texture) this._texture = null;
      this._textureBytes -= texture._pixels?.byteLength || 0;
      if (flushOutcome === FLUSH_OK) this._invalidateSourcePacket();
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
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
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
      if (flushOutcome === FLUSH_OK) this._invalidateSourcePacket();
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
      const flushOutcome = this._flushForMutation();
      if (flushOutcome === FLUSH_FAILED) return;
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
      if (flushOutcome === FLUSH_OK) this._invalidateSourcePacket();
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
      if (flushOutcome === FLUSH_DEFERRED) {
        this._texture._pixels = this._texture._pixels.slice();
      }
      for (let row = 0; row < height; row++)
        this._texture._pixels.set(pixels.subarray(row * width * 4, (row + 1) * width * 4),
          ((y + row) * this._texture._width + x) * 4);
      this._texture._generation++; this._texture._commandSnapshot = null;
      diagnostics.uploadedTextureBytes += pixels.byteLength;
    }
    generateMipmap() { this._setError(constants.INVALID_OPERATION); }
    clearColor(r, g, b, a) {
      r = Number(r); g = Number(g); b = Number(b); a = Number(a);
      if (!Number.isFinite(r) || !Number.isFinite(g)
          || !Number.isFinite(b) || !Number.isFinite(a)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      r = Math.max(0, Math.min(1, r));
      g = Math.max(0, Math.min(1, g));
      b = Math.max(0, Math.min(1, b));
      a = Math.max(0, Math.min(1, a));
      if (this._clearColor[0] === r && this._clearColor[1] === g
          && this._clearColor[2] === b && this._clearColor[3] === a) return;
      this._clearColor[0] = r; this._clearColor[1] = g;
      this._clearColor[2] = b; this._clearColor[3] = a;
      this._touchRepeatedCommandState();
    }
    clearDepth(value) {
      value = Number(value);
      if (!Number.isFinite(value)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      value = Math.max(0, Math.min(1, value));
      if (this._clearDepth === value) return;
      this._clearDepth = value;
      this._touchRepeatedCommandState();
    }
    clear(mask) {
      if (mask & ~(constants.COLOR_BUFFER_BIT | constants.DEPTH_BUFFER_BIT | constants.STENCIL_BUFFER_BIT)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (!this._enqueueClear(mask)) return;
      this._schedule();
    }
    _capability(capability) {
      return capabilityBit(capability);
    }
    enable(capability) {
      const bit = this._capability(capability);
      if (!bit) this._setError(constants.INVALID_ENUM);
      else if (!(this._enabledMask & bit)) {
        this._enabledMask |= bit;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    disable(capability) {
      const bit = this._capability(capability);
      if (!bit) this._setError(constants.INVALID_ENUM);
      else if (this._enabledMask & bit) {
        this._enabledMask &= ~bit;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    isEnabled(capability) {
      const bit = this._capability(capability);
      if (!bit) {
        this._setError(constants.INVALID_ENUM); return false;
      }
      return (this._enabledMask & bit) !== 0;
    }
    blendFunc(source, destination) {
      const factors = [constants.ZERO, constants.ONE, constants.SRC_ALPHA,
        constants.ONE_MINUS_SRC_ALPHA];
      if (!factors.includes(source) || !factors.includes(destination)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      if (this._blendSrc !== source || this._blendDst !== destination) {
        this._blendSrc = source; this._blendDst = destination;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
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
      if (this._blendSrc !== sourceRGB || this._blendDst !== destinationRGB) {
        this._blendSrc = sourceRGB; this._blendDst = destinationRGB;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    blendEquation(mode) { if (mode !== constants.FUNC_ADD) this._setError(constants.INVALID_ENUM); }
    blendEquationSeparate(rgb, alpha) { if (rgb !== constants.FUNC_ADD || alpha !== constants.FUNC_ADD) this._setError(constants.INVALID_ENUM); }
    depthFunc(value) {
      if (value < constants.NEVER || value > constants.ALWAYS)
        this._setError(constants.INVALID_ENUM);
      else if (this._depthFunc !== value) {
        this._depthFunc = value;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    cullFace(value) {
      if (![constants.FRONT, constants.BACK,
            constants.FRONT_AND_BACK].includes(value))
        this._setError(constants.INVALID_ENUM);
      else if (this._cullFace !== value) {
        this._cullFace = value;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    frontFace(value) {
      if (![constants.CW, constants.CCW].includes(value))
        this._setError(constants.INVALID_ENUM);
      else if (this._frontFace !== value) {
        this._frontFace = value;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
    lineWidth(value) { value = Number(value); value === 1 ? this._lineWidth = 1 : this._setError(constants.INVALID_VALUE); }
    viewport(x, y, width, height) {
      x = toWebGLInt32(x); y = toWebGLInt32(y);
      width = toWebGLInt32(width); height = toWebGLInt32(height);
      if (width < 0 || height < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      width = Math.min(MAX_DRAWING_WIDTH, width);
      height = Math.min(MAX_DRAWING_HEIGHT, height);
      if (this._viewport[0] !== x || this._viewport[1] !== y
          || this._viewport[2] !== width || this._viewport[3] !== height) {
        this._viewport[0] = x; this._viewport[1] = y;
        this._viewport[2] = width; this._viewport[3] = height;
        this._viewportRevision++;
        this._touchRepeatedCommandState();
      }
    }
    scissor(x, y, width, height) {
      x = toWebGLInt32(x); y = toWebGLInt32(y);
      width = toWebGLInt32(width); height = toWebGLInt32(height);
      if (width < 0 || height < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (this._scissor[0] !== x || this._scissor[1] !== y
          || this._scissor[2] !== width || this._scissor[3] !== height) {
        this._scissor[0] = x; this._scissor[1] = y;
        this._scissor[2] = width; this._scissor[3] = height;
        this._scissorRevision++;
        this._touchRepeatedCommandState();
      }
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
      const finalVertex = first + count - 1;
      if (!Number.isSafeInteger(finalVertex) || finalVertex < first)
        return false;
      const buffer = attribute.buffer;
      if (attribute.capacityGeneration !== buffer._storageGeneration) {
        const componentBytes = typeBytes(attribute.type),
          elementBytes = attribute.size * componentBytes,
          stride = attribute.stride || elementBytes,
          length = buffer._data.byteLength;
        attribute.capacity = componentBytes && attribute.offset <= length
          && elementBytes <= length - attribute.offset
          ? Math.floor((length - attribute.offset - elementBytes) / stride) + 1
          : 0;
        attribute.capacityGeneration = buffer._storageGeneration;
      }
      return finalVertex < attribute.capacity;
    }
    _instanceMatrixAttribute(translation, instanceCount) {
      const base = translation.instanceMatrixLocation;
      if (base < 0) return null;
      const first = this._attributesState[base];
      if (!first?.enabled || !first.buffer || first.divisor !== 1
          || first.size !== 4 || first.type !== constants.FLOAT
          || first.normalized || first.stride < 64) return false;
      for (let column = 1; column < 4; column++) {
        const state = this._attributesState[base + column];
        if (!state?.enabled || state.buffer !== first.buffer
            || state.divisor !== 1 || state.size !== 4
            || state.type !== constants.FLOAT || state.normalized
            || state.stride !== first.stride
            || state.offset !== first.offset + column * 16) return false;
      }
      return this._attributeRangeAvailable(first, 0, instanceCount)
        && this._attributeRangeAvailable(
          this._attributesState[base + 3], 0, instanceCount)
        ? first : false;
    }
    _instanceColorAttribute(translation, instanceCount) {
      const location = translation.instanceColorLocation;
      if (location < 0) return null;
      const state = this._attributesState[location];
      if (!state?.enabled || !state.buffer || state.divisor !== 1
          || state.size !== 4) return false;
      return this._attributeRangeAvailable(state, 0, instanceCount)
        ? state : false;
    }
    _instanceTransformAttribute(translation, instanceCount) {
      const location = translation.instanceTransformLocation;
      if (location < 0) return null;
      const state = this._attributesState[location];
      if (!state?.enabled || !state.buffer || state.divisor !== 1
          || state.size !== 4 || state.type !== constants.FLOAT
          || state.normalized) return false;
      return this._attributeRangeAvailable(state, 0, instanceCount)
        ? state : false;
    }
    _prepareProgramCombinedMatrix(program) {
      if (program._combinedMatrixRevision === program._matrixRevision)
        return true;
      if (program._matrixCount === 1) {
        /* A single matrix is already retained in the bounded matrix pack.
           Point the combined view at it during link instead of crossing the
           JS/native bridge merely to copy sixteen unchanged float words. */
        program._combinedMatrixRevision = program._matrixRevision;
        return true;
      }
      const translation = program._translation,
        nativeCombined = typeof nativeCombineMatrix4 === "function"
          && nativeCombineMatrix4(
            program._combinedMatrix, program._matrixPack,
            program._matrixCount),
        matrix = nativeCombined ? program._combinedMatrix
          : combinedMatrix4Into(
            program._combinedMatrix, this._matrixScratch,
            translation.matrices, program._uniformValues);
      for (let component = 0; component < matrix.length; component++)
        if (!Number.isFinite(matrix[component])) return false;
      program._combinedMatrixRevision = program._matrixRevision;
      return true;
    }
    _draw(mode, first, count, index = null, instanceCount = 1) {
      const profile = diagnostics.profileDrawPhases;
      let profileAt = profile ? performance.now() : 0;
      if (!this._program?._linked) { this._setError(constants.INVALID_OPERATION); return; }
      if (!Number.isInteger(mode) || mode < constants.POINTS
          || mode > constants.TRIANGLE_FAN) {
        this._setError(constants.INVALID_ENUM); return;
      }
      if (!Number.isInteger(first) || first < 0 || !Number.isInteger(count) || count < 0) {
        this._setError(constants.INVALID_VALUE); return;
      }
      if (!Number.isInteger(instanceCount) || instanceCount < 0
          || instanceCount > MAX_INSTANCES) {
        this._setError(instanceCount > MAX_INSTANCES
          ? constants.OUT_OF_MEMORY : constants.INVALID_VALUE); return;
      }
      if (!count || !instanceCount) return;
      if (count > MAX_VERTICES
          || instanceCount > Math.floor(MAX_VERTICES / count)) {
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      const program = this._program,
        translation = program._translation,
        vertexState = this._vertexArray?._state
          || this._defaultVertexArrayState,
        previousPlan = vertexState.drawPlan;
      let cachedPlan = previousPlan;
      let position, color, texcoord, instanceMatrix, instanceColor,
        instanceTransform;
      const cacheHit = cachedPlan
        && cachedPlan.program === program
        && cachedPlan.linkRevision === program._linkRevision
        && cachedPlan.revision === vertexState.revision
        && cachedPlan.mode === mode && cachedPlan.first === first
        && cachedPlan.count === count
        && cachedPlan.indexId === (index?.id || 0)
        && cachedPlan.indexType === (index?.type || 0)
        && cachedPlan.indexOffset === (index?.offset || 0)
        && cachedPlan.indexMaximum === (index?.maximum || 0)
        && cachedPlan.bufferStorageRevision === this._bufferStorageRevision
        && instanceCount <= cachedPlan.maximumInstanceCount;
      if (cacheHit) {
        diagnostics.drawPlanHits++;
        position = cachedPlan.position; color = cachedPlan.color;
        texcoord = cachedPlan.texcoord;
        instanceMatrix = cachedPlan.instanceMatrix;
        instanceColor = cachedPlan.instanceColor;
        instanceTransform = cachedPlan.instanceTransform;
      } else {
        diagnostics.drawPlanMisses++;
        position = translation.positionLocation >= 0
          ? this._attributesState[translation.positionLocation] : null;
        color = translation.colorLocation >= 0
          ? this._attributesState[translation.colorLocation] : null;
        texcoord = translation.texcoordLocation >= 0
          ? this._attributesState[translation.texcoordLocation] : null;
        if (!position?.enabled || !position.buffer) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        if (position.divisor !== 0
            || (color?.enabled && color.divisor !== 0)
            || (texcoord?.enabled && texcoord.divisor !== 0)) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        if (translation.usesTexture
            && (!texcoord?.enabled || !texcoord.buffer
                || !this._texture?._pixels)) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        instanceMatrix = this._instanceMatrixAttribute(
          translation, instanceCount);
        instanceColor = this._instanceColorAttribute(
          translation, instanceCount);
        instanceTransform = this._instanceTransformAttribute(
          translation, instanceCount);
        if (instanceMatrix === false || instanceColor === false
            || instanceTransform === false
            || (instanceMatrix && instanceTransform)) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        const vertexFirst = index ? 0 : first,
          vertexCount = index ? index.maximum + 1 : count;
        if (!this._attributeRangeAvailable(position, vertexFirst, vertexCount)
            || (color?.enabled
                && !this._attributeRangeAvailable(
                  color, vertexFirst, vertexCount))
            || (texcoord?.enabled
                && !this._attributeRangeAvailable(
                  texcoord, vertexFirst, vertexCount))) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        cachedPlan = vertexState.drawPlan = {
          program, linkRevision: program._linkRevision,
          ownerState: vertexState,
          revision: vertexState.revision, mode, first, count,
          indexId: index?.id || 0, indexType: index?.type || 0,
          indexOffset: index?.offset || 0,
          indexMaximum: index?.maximum || 0,
          bufferStorageRevision: this._bufferStorageRevision,
          position, color, texcoord, instanceMatrix, instanceColor,
          instanceTransform, maximumInstanceCount: instanceCount,
          uniformColorValue: translation.uniformColor
            ? program._uniformValues.get(translation.uniformColor.name)
            : DEFAULT_UNIFORM_COLOR,
          commandTemplateU32: null,
          commandTemplateI32: null,
          commandTemplateRevision: 0,
          commandTemplateId: 0,
        };
      }
      /* Texture binding remains live state rather than part of the geometry
         admission plan. A cached attribute/range result must not turn an
         unbound or deleted texture into a valid textured draw. */
      if (translation.usesTexture && !this._texture?._pixels) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileBasicMs += now - profileAt;
        profileAt = now;
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileInstancesMs += now - profileAt;
        profileAt = now;
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profileRangesMs += now - profileAt;
        profileAt = now;
      }
      /* Finite authored uniforms can still overflow while matrices are
         multiplied. That invalidates this primitive's geometry; it is not a
         corrupt JS/native command contract and must not lose context. */
      if (!this._prepareProgramCombinedMatrix(program)) return;
      const selectedTexture = translation.usesTexture ? this._texture : null;
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
      const selectedColor = cachedPlan.uniformColorValue;
      let uniformColor = selectedColor;
      if (translation.color && !color?.enabled) {
        uniformColor = this._drawUniformColorScratch;
        uniformColor.set(selectedColor);
        for (let component = 0; component < 4; component++)
          uniformColor[component] *= color?.constant?.[component] ?? 1;
      }
      if (profile) {
        const now = performance.now();
        diagnostics.profilePrepareMs += now - profileAt;
        profileAt = now;
      }
      if (!this._enqueueDraw(
          mode, first, count, index, position, color, texcoord,
          textureSnapshot, instanceCount, instanceMatrix, instanceColor,
          instanceTransform, program._combinedMatrix, uniformColor,
          null, count * instanceCount, cachedPlan)) return;
      if (profile) {
        const now = performance.now();
        diagnostics.profileEnqueueMs += now - profileAt;
        profileAt = now;
      }
      diagnostics.drawCalls++;
      diagnostics.vertices += count * instanceCount; this._schedule();
      if (instanceCount > 1) {
        diagnostics.instancedDrawCalls++;
        diagnostics.instances += instanceCount;
      }
      if (profile) {
        diagnostics.profileFinishMs += performance.now() - profileAt;
        diagnostics.profileDraws++;
      }
    }
    drawArrays(mode, first, count) { this._draw(mode, Number(first), Number(count)); }
    _drawElements(mode, count, type, offset, instanceCount = 1) {
      if (type !== constants.UNSIGNED_BYTE
          && type !== constants.UNSIGNED_SHORT) {
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
        maximum = typeof nativeIndexMaximum === "function"
          ? nativeIndexMaximum(data, type, offset, count) : -1;
        if (!Number.isInteger(maximum) || maximum < 0) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        this._elementBuffer._indexType = type;
        this._elementBuffer._indexOffset = offset;
        this._elementBuffer._indexCount = count;
        this._elementBuffer._indexMaximum = maximum;
      }
      const commandIndex = this._elementBuffer._commandIndex;
      if (commandIndex.generation !== this._elementBuffer._generation
          || commandIndex.type !== type || commandIndex.offset !== offset
          || commandIndex.maximum !== maximum) {
        commandIndex.data = data; commandIndex.type = type;
        commandIndex.id = this._elementBuffer._id;
        commandIndex.generation = this._elementBuffer._generation;
        commandIndex.usage = this._elementBuffer._usage;
        commandIndex.offset = offset; commandIndex.maximum = maximum;
      }
      this._draw(mode, 0, count, commandIndex, instanceCount);
    }
    drawElements(mode, count, type, offset) {
      this._drawElements(mode, count, type, offset);
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
        [constants.VERTEX_ARRAY_BINDING_OES]: this._vertexArray,
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
      const capability = capabilityBit(parameter);
      if (capability) return (this._enabledMask & capability) !== 0;
      this._setError(constants.INVALID_ENUM); return null;
    }
    getShaderPrecisionFormat() { return new WebGLShaderPrecisionFormat(127, 127, 23); }
    getSupportedExtensions() {
      return ["ANGLE_instanced_arrays", "OES_vertex_array_object",
        "TILEFINCH_repeated_frame_commands"];
    }
    getExtension(name) {
      name = String(name).toLowerCase();
      if (name === "angle_instanced_arrays") {
        if (!this._instancedExtension)
          this._instancedExtension = new AngleInstancedArrays(this);
        return this._instancedExtension;
      }
      if (name === "oes_vertex_array_object") {
        if (!this._vertexArrayExtension)
          this._vertexArrayExtension = new OesVertexArrayObject(this);
        return this._vertexArrayExtension;
      }
      if (name === "tilefinch_repeated_frame_commands") {
        if (!this._repeatedFrameExtension)
          this._repeatedFrameExtension =
            new TilefinchRepeatedFrameCommands(this);
        return this._repeatedFrameExtension;
      }
      return null;
    }
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
    WebGLShaderPrecisionFormat, WebGLFramebuffer, WebGLRenderbuffer,
    WebGLVertexArrayOES });
  Object.defineProperty(globalThis, "__tilefinchWebGLDiagnostics",
    { value: diagnostics, configurable: false, enumerable: false });
})();
