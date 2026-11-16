(() => {
  "use strict";
  /* The WebGL 1.0 interface objects (WebGLRenderingContext and the object
     interfaces), which pages reach for before, or instead of, a context:
     feature tests (`!!window.WebGLRenderingContext`), instanceof checks
     and constant reads (`WebGLRenderingContext.DEPTH_BUFFER_BIT`). Built
     the first time a page reads one of these names, a small module so
     that doing so does not load webgl.js; webgl.js loads this one first,
     constructs its contexts and objects as instances of these interfaces
     and puts the context's members on WebGLRenderingContext.prototype.
     WebGL 2 is not exposed (getContext("webgl2") answers null), so none
     of its interfaces is defined. Loaded after the page may have replaced
     globals: it uses what hardening.js captured. */
  const { defineProperty, getOwnPropertyDescriptor, freeze, apply, Event,
    TypeError, claimHooks } = globalThis.__tilefinchEventInterfaces;
  const publish = claimHooks?.()
    || ((name, value) => { globalThis[name] = value; });
  const illegal = () => { throw new TypeError("Illegal constructor"); };
  /* The WebGLRenderingContextBase constants, in IDL order. */
  const constants = {
    DEPTH_BUFFER_BIT: 0x100, STENCIL_BUFFER_BIT: 0x400,
    COLOR_BUFFER_BIT: 0x4000,
    POINTS: 0, LINES: 1, LINE_LOOP: 2, LINE_STRIP: 3, TRIANGLES: 4,
    TRIANGLE_STRIP: 5, TRIANGLE_FAN: 6,
    ZERO: 0, ONE: 1, SRC_COLOR: 0x300, ONE_MINUS_SRC_COLOR: 0x301,
    SRC_ALPHA: 0x302, ONE_MINUS_SRC_ALPHA: 0x303, DST_ALPHA: 0x304,
    ONE_MINUS_DST_ALPHA: 0x305, DST_COLOR: 0x306,
    ONE_MINUS_DST_COLOR: 0x307, SRC_ALPHA_SATURATE: 0x308,
    FUNC_ADD: 0x8006, BLEND_EQUATION: 0x8009, BLEND_EQUATION_RGB: 0x8009,
    BLEND_EQUATION_ALPHA: 0x883d, FUNC_SUBTRACT: 0x800a,
    FUNC_REVERSE_SUBTRACT: 0x800b,
    BLEND_DST_RGB: 0x80c8, BLEND_SRC_RGB: 0x80c9, BLEND_DST_ALPHA: 0x80ca,
    BLEND_SRC_ALPHA: 0x80cb, CONSTANT_COLOR: 0x8001,
    ONE_MINUS_CONSTANT_COLOR: 0x8002, CONSTANT_ALPHA: 0x8003,
    ONE_MINUS_CONSTANT_ALPHA: 0x8004, BLEND_COLOR: 0x8005,
    ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893,
    ARRAY_BUFFER_BINDING: 0x8894, ELEMENT_ARRAY_BUFFER_BINDING: 0x8895,
    STREAM_DRAW: 0x88e0, STATIC_DRAW: 0x88e4, DYNAMIC_DRAW: 0x88e8,
    BUFFER_SIZE: 0x8764, BUFFER_USAGE: 0x8765,
    CURRENT_VERTEX_ATTRIB: 0x8626,
    FRONT: 0x404, BACK: 0x405, FRONT_AND_BACK: 0x408,
    CULL_FACE: 0xb44, BLEND: 0xbe2, DITHER: 0xbd0, STENCIL_TEST: 0xb90,
    DEPTH_TEST: 0xb71, SCISSOR_TEST: 0xc11, POLYGON_OFFSET_FILL: 0x8037,
    SAMPLE_ALPHA_TO_COVERAGE: 0x809e, SAMPLE_COVERAGE: 0x80a0,
    NO_ERROR: 0, INVALID_ENUM: 0x500, INVALID_VALUE: 0x501,
    INVALID_OPERATION: 0x502, OUT_OF_MEMORY: 0x505,
    CW: 0x900, CCW: 0x901,
    LINE_WIDTH: 0xb21, ALIASED_POINT_SIZE_RANGE: 0x846d,
    ALIASED_LINE_WIDTH_RANGE: 0x846e, CULL_FACE_MODE: 0xb45,
    FRONT_FACE: 0xb46, DEPTH_RANGE: 0xb70, DEPTH_WRITEMASK: 0xb72,
    DEPTH_CLEAR_VALUE: 0xb73, DEPTH_FUNC: 0xb74,
    STENCIL_CLEAR_VALUE: 0xb91, STENCIL_FUNC: 0xb92, STENCIL_FAIL: 0xb94,
    STENCIL_PASS_DEPTH_FAIL: 0xb95, STENCIL_PASS_DEPTH_PASS: 0xb96,
    STENCIL_REF: 0xb97, STENCIL_VALUE_MASK: 0xb93,
    STENCIL_WRITEMASK: 0xb98, STENCIL_BACK_FUNC: 0x8800,
    STENCIL_BACK_FAIL: 0x8801, STENCIL_BACK_PASS_DEPTH_FAIL: 0x8802,
    STENCIL_BACK_PASS_DEPTH_PASS: 0x8803, STENCIL_BACK_REF: 0x8ca3,
    STENCIL_BACK_VALUE_MASK: 0x8ca4, STENCIL_BACK_WRITEMASK: 0x8ca5,
    VIEWPORT: 0xba2, SCISSOR_BOX: 0xc10, COLOR_CLEAR_VALUE: 0xc22,
    COLOR_WRITEMASK: 0xc23, UNPACK_ALIGNMENT: 0xcf5,
    PACK_ALIGNMENT: 0xd05, MAX_TEXTURE_SIZE: 0xd33,
    MAX_VIEWPORT_DIMS: 0xd3a, SUBPIXEL_BITS: 0xd50, RED_BITS: 0xd52,
    GREEN_BITS: 0xd53, BLUE_BITS: 0xd54, ALPHA_BITS: 0xd55,
    DEPTH_BITS: 0xd56, STENCIL_BITS: 0xd57, POLYGON_OFFSET_UNITS: 0x2a00,
    POLYGON_OFFSET_FACTOR: 0x8038, TEXTURE_BINDING_2D: 0x8069,
    SAMPLE_BUFFERS: 0x80a8, SAMPLES: 0x80a9,
    SAMPLE_COVERAGE_VALUE: 0x80aa, SAMPLE_COVERAGE_INVERT: 0x80ab,
    COMPRESSED_TEXTURE_FORMATS: 0x86a3,
    DONT_CARE: 0x1100, FASTEST: 0x1101, NICEST: 0x1102,
    GENERATE_MIPMAP_HINT: 0x8192,
    BYTE: 0x1400, UNSIGNED_BYTE: 0x1401, SHORT: 0x1402,
    UNSIGNED_SHORT: 0x1403, INT: 0x1404, UNSIGNED_INT: 0x1405,
    FLOAT: 0x1406,
    DEPTH_COMPONENT: 0x1902, ALPHA: 0x1906, RGB: 0x1907, RGBA: 0x1908,
    LUMINANCE: 0x1909, LUMINANCE_ALPHA: 0x190a,
    UNSIGNED_SHORT_4_4_4_4: 0x8033, UNSIGNED_SHORT_5_5_5_1: 0x8034,
    UNSIGNED_SHORT_5_6_5: 0x8363,
    FRAGMENT_SHADER: 0x8b30, VERTEX_SHADER: 0x8b31,
    MAX_VERTEX_ATTRIBS: 0x8869, MAX_VERTEX_UNIFORM_VECTORS: 0x8dfb,
    MAX_VARYING_VECTORS: 0x8dfc, MAX_COMBINED_TEXTURE_IMAGE_UNITS: 0x8b4d,
    MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8b4c,
    MAX_TEXTURE_IMAGE_UNITS: 0x8872, MAX_FRAGMENT_UNIFORM_VECTORS: 0x8dfd,
    SHADER_TYPE: 0x8b4f, DELETE_STATUS: 0x8b80, LINK_STATUS: 0x8b82,
    VALIDATE_STATUS: 0x8b83, ATTACHED_SHADERS: 0x8b85,
    ACTIVE_UNIFORMS: 0x8b86, ACTIVE_ATTRIBUTES: 0x8b89,
    SHADING_LANGUAGE_VERSION: 0x8b8c, CURRENT_PROGRAM: 0x8b8d,
    NEVER: 0x200, LESS: 0x201, EQUAL: 0x202, LEQUAL: 0x203,
    GREATER: 0x204, NOTEQUAL: 0x205, GEQUAL: 0x206, ALWAYS: 0x207,
    KEEP: 0x1e00, REPLACE: 0x1e01, INCR: 0x1e02, DECR: 0x1e03,
    INVERT: 0x150a, INCR_WRAP: 0x8507, DECR_WRAP: 0x8508,
    VENDOR: 0x1f00, RENDERER: 0x1f01, VERSION: 0x1f02,
    NEAREST: 0x2600, LINEAR: 0x2601, NEAREST_MIPMAP_NEAREST: 0x2700,
    LINEAR_MIPMAP_NEAREST: 0x2701, NEAREST_MIPMAP_LINEAR: 0x2702,
    LINEAR_MIPMAP_LINEAR: 0x2703,
    TEXTURE_MAG_FILTER: 0x2800, TEXTURE_MIN_FILTER: 0x2801,
    TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
    TEXTURE_2D: 0xde1, TEXTURE: 0x1702, TEXTURE_CUBE_MAP: 0x8513,
    TEXTURE_BINDING_CUBE_MAP: 0x8514,
    TEXTURE_CUBE_MAP_POSITIVE_X: 0x8515,
    TEXTURE_CUBE_MAP_NEGATIVE_X: 0x8516,
    TEXTURE_CUBE_MAP_POSITIVE_Y: 0x8517,
    TEXTURE_CUBE_MAP_NEGATIVE_Y: 0x8518,
    TEXTURE_CUBE_MAP_POSITIVE_Z: 0x8519,
    TEXTURE_CUBE_MAP_NEGATIVE_Z: 0x851a,
    MAX_CUBE_MAP_TEXTURE_SIZE: 0x851c,
  };
  /* TEXTURE0 to TEXTURE31, then ACTIVE_TEXTURE. */
  for (let unit = 0; unit < 32; unit++)
    constants["TEXTURE" + unit] = 0x84c0 + unit;
  const remaining = {
    ACTIVE_TEXTURE: 0x84e0,
    REPEAT: 0x2901, CLAMP_TO_EDGE: 0x812f, MIRRORED_REPEAT: 0x8370,
    FLOAT_VEC2: 0x8b50, FLOAT_VEC3: 0x8b51, FLOAT_VEC4: 0x8b52,
    INT_VEC2: 0x8b53, INT_VEC3: 0x8b54, INT_VEC4: 0x8b55, BOOL: 0x8b56,
    BOOL_VEC2: 0x8b57, BOOL_VEC3: 0x8b58, BOOL_VEC4: 0x8b59,
    FLOAT_MAT2: 0x8b5a, FLOAT_MAT3: 0x8b5b, FLOAT_MAT4: 0x8b5c,
    SAMPLER_2D: 0x8b5e, SAMPLER_CUBE: 0x8b60,
    VERTEX_ATTRIB_ARRAY_ENABLED: 0x8622, VERTEX_ATTRIB_ARRAY_SIZE: 0x8623,
    VERTEX_ATTRIB_ARRAY_STRIDE: 0x8624, VERTEX_ATTRIB_ARRAY_TYPE: 0x8625,
    VERTEX_ATTRIB_ARRAY_NORMALIZED: 0x886a,
    VERTEX_ATTRIB_ARRAY_POINTER: 0x8645,
    VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: 0x889f,
    IMPLEMENTATION_COLOR_READ_TYPE: 0x8b9a,
    IMPLEMENTATION_COLOR_READ_FORMAT: 0x8b9b,
    COMPILE_STATUS: 0x8b81,
    LOW_FLOAT: 0x8df0, MEDIUM_FLOAT: 0x8df1, HIGH_FLOAT: 0x8df2,
    LOW_INT: 0x8df3, MEDIUM_INT: 0x8df4, HIGH_INT: 0x8df5,
    FRAMEBUFFER: 0x8d40, RENDERBUFFER: 0x8d41,
    RGBA4: 0x8056, RGB5_A1: 0x8057, RGB565: 0x8d62,
    DEPTH_COMPONENT16: 0x81a5, STENCIL_INDEX8: 0x8d48,
    DEPTH_STENCIL: 0x84f9,
    RENDERBUFFER_WIDTH: 0x8d42, RENDERBUFFER_HEIGHT: 0x8d43,
    RENDERBUFFER_INTERNAL_FORMAT: 0x8d44, RENDERBUFFER_RED_SIZE: 0x8d50,
    RENDERBUFFER_GREEN_SIZE: 0x8d51, RENDERBUFFER_BLUE_SIZE: 0x8d52,
    RENDERBUFFER_ALPHA_SIZE: 0x8d53, RENDERBUFFER_DEPTH_SIZE: 0x8d54,
    RENDERBUFFER_STENCIL_SIZE: 0x8d55,
    FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: 0x8cd0,
    FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: 0x8cd1,
    FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: 0x8cd2,
    FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE: 0x8cd3,
    COLOR_ATTACHMENT0: 0x8ce0, DEPTH_ATTACHMENT: 0x8d00,
    STENCIL_ATTACHMENT: 0x8d20, DEPTH_STENCIL_ATTACHMENT: 0x821a,
    NONE: 0,
    FRAMEBUFFER_COMPLETE: 0x8cd5,
    FRAMEBUFFER_INCOMPLETE_ATTACHMENT: 0x8cd6,
    FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: 0x8cd7,
    FRAMEBUFFER_INCOMPLETE_DIMENSIONS: 0x8cd9,
    FRAMEBUFFER_UNSUPPORTED: 0x8cdd,
    FRAMEBUFFER_BINDING: 0x8ca6, RENDERBUFFER_BINDING: 0x8ca7,
    MAX_RENDERBUFFER_SIZE: 0x84e8,
    INVALID_FRAMEBUFFER_OPERATION: 0x506,
    UNPACK_FLIP_Y_WEBGL: 0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241,
    CONTEXT_LOST_WEBGL: 0x9242,
    UNPACK_COLORSPACE_CONVERSION_WEBGL: 0x9243,
    BROWSER_DEFAULT_WEBGL: 0x9244,
  };
  for (const name in remaining) constants[name] = remaining[name];

  /* WebIDL interface objects: constructors that throw (only webgl.js
     makes instances, with Reflect.construct and the interface as the new
     target), a non-writable prototype, a class string. */
  class WebGLObject { constructor() { illegal(); } }
  class WebGLBuffer extends WebGLObject { constructor() { illegal(); } }
  class WebGLFramebuffer extends WebGLObject {
    constructor() { illegal(); }
  }
  class WebGLProgram extends WebGLObject { constructor() { illegal(); } }
  class WebGLRenderbuffer extends WebGLObject {
    constructor() { illegal(); }
  }
  class WebGLShader extends WebGLObject { constructor() { illegal(); } }
  class WebGLTexture extends WebGLObject { constructor() { illegal(); } }
  class WebGLUniformLocation { constructor() { illegal(); } }
  class WebGLActiveInfo { constructor() { illegal(); } }
  class WebGLShaderPrecisionFormat { constructor() { illegal(); } }
  class WebGLRenderingContext { constructor() { illegal(); } }
  class WebGLContextEvent extends Event {
    #statusMessage;
    constructor(type, init = undefined) {
      super(type, init);
      const message = init?.statusMessage;
      this.#statusMessage = message === undefined ? "" : `${message}`;
    }
    get statusMessage() { return this.#statusMessage; }
  }
  defineProperty(WebGLContextEvent.prototype, "statusMessage",
    { __proto__: null, enumerable: true });
  const interfaces = { WebGLObject, WebGLBuffer, WebGLFramebuffer,
    WebGLProgram, WebGLRenderbuffer, WebGLShader, WebGLTexture,
    WebGLUniformLocation, WebGLActiveInfo, WebGLShaderPrecisionFormat,
    WebGLRenderingContext, WebGLContextEvent };
  for (const name in interfaces) {
    const Interface = interfaces[name];
    defineProperty(Interface.prototype, Symbol.toStringTag,
      { __proto__: null, configurable: true, value: name });
    defineProperty(globalThis, name, { __proto__: null, configurable: true,
      enumerable: false, writable: true, value: Interface });
  }
  /* Constants are enumerable, non-writable, non-configurable. */
  for (const name in constants) {
    const constant =
      { __proto__: null, enumerable: true, value: constants[name] };
    defineProperty(WebGLRenderingContext, name, constant);
    defineProperty(WebGLRenderingContext.prototype, name, constant);
  }
  /* The context's operations and attributes, with their lengths, on the
     prototype from the start: a page may read, wrap or replace one before
     it makes its first context, as in browsers. Each forwards to webgl.js's
     member of that name (published as __tilefinchWebGLMethods when it
     loads); webgl.js then puts its own members in place of these, leaving
     whatever a page put there itself, so a page's wrapper that kept one of
     these still reaches the implementation. Before webgl.js loads there is
     no context, so every receiver is the wrong one. Not enumerable, as
     webgl.js's own members are. */
  const members = [
    "getContextAttributes", 0, "isContextLost", 0, "getError", 0,
    "createBuffer", 0, "deleteBuffer", 1, "isBuffer", 1, "bindBuffer", 2,
    "bufferData", 3, "bufferSubData", 3, "getBufferParameter", 2,
    "createShader", 1, "shaderSource", 2, "getShaderSource", 1,
    "compileShader", 1, "getShaderParameter", 2, "getShaderInfoLog", 1,
    "deleteShader", 1, "isShader", 1, "createProgram", 0, "attachShader", 2,
    "detachShader", 2, "getAttachedShaders", 1, "bindAttribLocation", 3,
    "linkProgram", 1, "validateProgram", 1, "useProgram", 1,
    "getProgramParameter", 2, "getProgramInfoLog", 1, "deleteProgram", 1,
    "isProgram", 1, "getAttribLocation", 2, "getUniformLocation", 2,
    "getActiveAttrib", 2, "getActiveUniform", 2, "uniform1i", 2,
    "uniform1iv", 2, "uniform2i", 3, "uniform3i", 4, "uniform4i", 5,
    "uniform2iv", 2, "uniform3iv", 2, "uniform4iv", 2, "uniform1f", 2,
    "uniform2f", 3, "uniform3f", 4, "uniform4f", 5, "uniform1fv", 2,
    "uniform2fv", 2, "uniform3fv", 2, "uniform4fv", 2,
    "uniformMatrix4fv", 3, "uniformMatrix3fv", 3, "uniformMatrix2fv", 3,
    "getUniform", 2, "enableVertexAttribArray", 1,
    "disableVertexAttribArray", 1, "vertexAttribPointer", 6,
    "vertexAttrib1fv", 2, "vertexAttrib2fv", 2, "vertexAttrib3fv", 2,
    "vertexAttrib4fv", 2, "vertexAttrib1f", 2, "vertexAttrib2f", 3,
    "vertexAttrib3f", 4, "vertexAttrib4f", 5, "getVertexAttrib", 2,
    "getVertexAttribOffset", 2, "createTexture", 0, "deleteTexture", 1,
    "isTexture", 1, "activeTexture", 1, "bindTexture", 2,
    "texParameteri", 3, "texParameterf", 3, "getTexParameter", 2,
    "pixelStorei", 2, "texImage2D", 3, "texSubImage2D", 4,
    "generateMipmap", 1, "clearColor", 4, "clearDepth", 1, "clear", 1,
    "enable", 1, "disable", 1, "isEnabled", 1, "blendFunc", 2,
    "blendFuncSeparate", 4, "blendEquation", 1, "blendEquationSeparate", 2,
    "depthFunc", 1, "cullFace", 1, "frontFace", 1, "lineWidth", 1,
    "viewport", 4, "scissor", 4, "colorMask", 4, "depthMask", 1,
    "stencilMask", 1, "stencilMaskSeparate", 2, "stencilFunc", 0,
    "stencilFuncSeparate", 0, "stencilOp", 0, "stencilOpSeparate", 0,
    "clearStencil", 1, "blendColor", 4, "depthRange", 2, "polygonOffset", 0,
    "sampleCoverage", 0, "hint", 0, "flush", 0, "finish", 0,
    "drawArrays", 3, "drawElements", 4, "readPixels", 7, "getParameter", 1,
    "getShaderPrecisionFormat", 0, "getSupportedExtensions", 0,
    "getExtension", 1, "createFramebuffer", 0, "createRenderbuffer", 0,
    "bindFramebuffer", 2, "bindRenderbuffer", 2, "checkFramebufferStatus", 1,
    "deleteFramebuffer", 1, "deleteRenderbuffer", 1, "isFramebuffer", 1,
    "isRenderbuffer", 1, "framebufferTexture2D", 0,
    "framebufferRenderbuffer", 0, "renderbufferStorage", 0,
    "copyTexImage2D", 0, "copyTexSubImage2D", 0,
    /* attributes: -1 */
    "drawingBufferWidth", -1, "drawingBufferHeight", -1,
  ];
  const stubs = { __proto__: null },
    implementation = (receiver, name, args) => {
      const methods = globalThis.__tilefinchWebGLMethods;
      if (methods === undefined) throw new TypeError("Illegal invocation");
      return apply(methods[name], receiver, args);
    },
    contextPrototype = WebGLRenderingContext.prototype;
  for (let at = 0; at < members.length; at += 2) {
    const name = members[at], length = members[at + 1];
    let descriptor;
    if (length < 0) {
      const getter = getOwnPropertyDescriptor({
        get [name]() { return implementation(this, name, []); },
      }, name).get;
      stubs[name] = getter;
      descriptor = { __proto__: null, configurable: true, get: getter };
    } else {
      const method = { [name]() {
        return implementation(this, name, arguments);
      } }[name];
      defineProperty(method, "length", { __proto__: null, value: length });
      stubs[name] = method;
      descriptor = { __proto__: null, configurable: true, writable: true,
        value: method };
    }
    defineProperty(contextPrototype, name, descriptor);
  }
  /* Frozen: what webgl.js builds its contexts and objects from is not a
     page's to change (the page can read it). */
  publish("__tilefinchWebGLInterfaces",
    freeze({ __proto__: null, ...interfaces, stubs: freeze(stubs) }));
})();
