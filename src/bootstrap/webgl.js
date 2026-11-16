(() => {
  "use strict";
  /* Loaded after the page may have replaced these globals: build
     errors, promises and platform objects with the constructors
     hardening.js captured, and publish private hooks with its
     defineProperty. What runs while this module loads, and what makes
     contexts and objects, uses its reflection too: a page's
     Reflect.construct must never receive the implementation classes. */
  const { defineProperty, getOwnPropertyDescriptor, ownKeys,
    construct: reflectConstruct,
    freeze, Error, DOMException, Map, Set, WeakMap, WeakRef,
    Array, ArrayBuffer, Uint8Array, Uint8ClampedArray, Int32Array,
    Uint32Array, Float32Array, Float64Array, RegExp, claimHooks } =
    globalThis.__tilefinchEventInterfaces;
  /* This module's hook publisher, claimed first (hardening.js); a realm
     without reserved hooks takes plain globals. */
  const publish = claimHooks?.()
    || ((name, value) => { globalThis[name] = value; });
  /* The interface objects, constants included, come from
     webgl-interfaces.js, which the host loads first: page-visible names
     exist before WebGL is used, and contexts and objects made here are
     instances of those same interfaces. */
  const { WebGLRenderingContext, WebGLBuffer, WebGLTexture, WebGLShader,
    WebGLProgram, WebGLUniformLocation, WebGLActiveInfo,
    WebGLShaderPrecisionFormat, WebGLFramebuffer, WebGLRenderbuffer,
    WebGLContextEvent, stubs } = globalThis.__tilefinchWebGLInterfaces;

  /* Surface retirement also advances the native cache incarnation.  Page
     code never reads, selects, or serializes that process-issued identity. */
  const releaseNativeWebGLSurface =
    globalThis.__tilefinchWebGLReleaseSurface,
    publishNativeWebGLFrame = globalThis.__tilefinchWebGLPublishFrame,
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
    /* three.js r128's expanded built-in shaders reach ~52 KiB. */
    MAX_SHADER_BYTES = 64 * 1024,
    MAX_INTERFACE_DECLARATIONS = 24,
    /* WebGL 1's minimum limits. Uniform values are evaluated in JS per
       draw, not held in GE registers, so the PSP needs no lower bound. */
    MAX_VERTEX_UNIFORM_VECTORS = 128,
    MAX_FRAGMENT_UNIFORM_VECTORS = 16,
    /* WebGL 1's minimum. Varyings are followed symbolically, not stored. */
    MAX_VARYING_VECTORS = 8,
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
    MATERIAL_WIRE_MAGIC = 0x5446574d,
    /* One material record per command slot (word 95 = slot + 1). */
    MATERIAL_WORDS = 256,
    /* Material records kept per program, one per recent input value set
       (about 1.5 KiB each, allocated as value sets appear), and the miss
       count after which draws look for one less and less often. */
    MATERIAL_KEPT_RECORDS = 16, MATERIAL_MISS_STREAK = 8,
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
    /* Command word 27 carries the blend equation: 0 blending off, then
       FUNC_ADD, EXT_blend_minmax's MIN_EXT and MAX_EXT, FUNC_SUBTRACT and
       FUNC_REVERSE_SUBTRACT. */
    MIN_EXT = 0x8007, MAX_EXT = 0x8008,
    BLEND_WIRE = { [0x8006]: 1, [MIN_EXT]: 2, [MAX_EXT]: 3, [0x800a]: 4,
      [0x800b]: 5 },
    /* The WebGL 1 blend factors. Command words 31 and 32 carry a factor
       enum, or BLEND_FIXED | 0xBBGGRR for a fixed colour (the GE's GU_FIX
       layout), which _blendFactorWire resolves from blendColor(). */
    BLEND_FACTORS = [0, 1, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305,
      0x0306, 0x0307, 0x0308, 0x8001, 0x8002, 0x8003, 0x8004],
    BLEND_FIXED = 0x01000000,
    DRAW_MODE_NAMES = ["POINTS", "LINES", "LINE_LOOP", "LINE_STRIP",
      "TRIANGLES", "TRIANGLE_STRIP", "TRIANGLE_FAN"],
    DEFAULT_UNIFORM_COLOR = new Float32Array([1, 1, 1, 1]),
    contexts = new WeakMap(),
    repeatedCommandLists = new WeakMap(),
    liveContexts = [],
    diagnostics = {
      contexts: 0,
      lostContexts: 0,
      shaderRefusals: 0,
      shaderWarnings: 0,
      drawCalls: 0,
      instancedDrawCalls: 0,
      instances: 0,
      flushes: 0,
      vertices: 0,
      uploadedTextureBytes: 0,
      nativeFailures: 0,
      splitDraws: 0,
      splitParts: 0,
      deferredFlushes: 0,
      forcedFlushes: 0,
      maximumForcedFlushesPerFrame: 0,
      drawPlanHits: 0,
      drawPlanMisses: 0,
      materialRecordHits: 0,
      materialRecordBuilds: 0,
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
    /* WebGL 1.0's constants are the interface object's; the two extension
       values (ANGLE_instanced_arrays, OES_vertex_array_object) are not. */
    constants = WebGLRenderingContext,
    VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE = 0x88fe,
    VERTEX_ARRAY_BINDING_OES = 0x85b5,
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
    /* Exactly the same retained uniform value: 0 and -0 differ. */
    sameMaterialValue = (a, b) => a === b && (a !== 0 || 1 / a === 1 / b),
    /* Whether a material record key holds exactly `value` at `at`. */
    materialKeyPartMatches = (key, at, value) => {
      if (value instanceof Float32Array) {
        const length = value.length;
        if (key[at++] !== length) return false;
        for (let k = 0; k < length; k++) {
          const a = key[at++], b = value[k];
          if (a !== b || (a === 0 && 1 / a !== 1 / b)) return false;
        }
        return true;
      }
      if (typeof value !== "number" || key[at++] !== -1) return false;
      const a = key[at];
      return a === value && (a !== 0 || 1 / a === 1 / value);
    },
    identityMatrix = () => new Float32Array([
      1, 0, 0, 0, 0, 1, 0, 0,
      0, 0, 1, 0, 0, 0, 0, 1,
    ]),
    /* canvas.width reads back the bounded size; the page's request is the
       attribute (HTML: a valid non-negative integer, else the default). */
    requestedCanvasSize = (canvas, name, fallback) => {
      const text = canvas.getAttribute?.(name);
      if (typeof text !== "string" || !/^(?:0|[1-9][0-9]*)$/.test(text)) return fallback;
      const value = Number(text);
      return value <= 0x7fffffff ? value : fallback;
    },
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
    /* Comments become spaces of the same length, so offsets into the
       stripped text still give the author's line numbers. */
    shaderText = (source) => String(source).replace(
      /\/\*[\s\S]*?\*\/|\/\/[^\n\r]*/g,
      (comment) => comment.replace(/[^\n]/g, " "),
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
    uniformVectors = (entries, limit) => {
      let vectors = 0;
      for (const entry of entries) {
        const columns = entry.type === "mat4" ? 4
          : entry.type === "mat3" ? 3 : entry.type === "mat2" ? 2 : 1;
        const consumed = columns * entry.size;
        if (!Number.isSafeInteger(consumed) || consumed > limit - vectors)
          return limit + 1;
        vectors += consumed;
      }
      return vectors;
    },
    /* Link-time shader check. translateProgram picks the GE inputs from
       declarations and names; scanShader and inspectProgram read what the
       shader computes, so logic the GE cannot run is refused or reported
       instead of silently drawing the vertex colour. Both run once per link,
       never on the draw path, and every scan is bounded by SCAN_TOKENS. */
    SCAN_TOKENS = 256,
    scanShader = (text, perData) => {
      text = text.replace(/^[ \t]*#.*$/gm, "");
      const tokens = [], at = [], pattern =
        /[A-Za-z_]\w*|(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|&&|\|\||[-+*\/<>=!]=|\S/g,
        tainted = new Set(perData), functions = new Set(),
        assigned = new Map(), flow = [];
      for (let match; (match = pattern.exec(text));) {
        tokens.push(match[0]); at.push(match.index);
      }
      for (const match of text.matchAll(
        /\b(?:void|float|int|bool|[bi]?vec[234]|mat[234])\s+(\w+)\s*\(/g))
        if (match[1] !== "main") functions.add(match[1]);
      /* Helper results are treated as per-pixel data: a helper may read a
         varying without receiving it as an argument. */
      const perToken = (token) => tainted.has(token) || functions.has(token)
        || /^texture/.test(token);
      let depth = 0;
      for (let i = 0; i < tokens.length; i++) {
        const token = tokens[i];
        if (token === "{") depth++;
        else if (token === "}") depth--;
        else if (/^(for|while|do|discard|dFdx|dFdy|fwidth)$/.test(token))
          flow.push({ keyword: token, index: at[i], refused: true });
        else if (token === "if" || token === "?") {
          let from = i + 1, to = from, open = 0;
          if (token === "if") {
            for (; to < tokens.length && to - i < SCAN_TOKENS; to++)
              if (tokens[to] === "(") open++;
              else if (tokens[to] === ")" && --open === 0) break;
          } else {
            for (to = from = i; from > 0 && i - from < SCAN_TOKENS; from--) {
              const previous = tokens[from - 1];
              if (previous === ")") open++;
              else if (previous === "(") { if (!open) break; open--; }
              else if (!open && /^([;{},=]|return|[-+*\/]=)$/.test(previous))
                break;
            }
          }
          const condition = tokens.slice(from, to);
          flow.push({ keyword: token === "?" ? "?:" : "if", index: at[i],
            perData: condition.length >= SCAN_TOKENS - 1
              || condition.some(perToken) });
        } else if (functions.has(token) && !depth && tokens[i + 1] === "(") {
          for (let j = i + 2; j < tokens.length && tokens[j] !== ")"
              && j - i < SCAN_TOKENS; j++)
            if (/^[,)[]$/.test(tokens[j + 1])) tainted.add(tokens[j]);
        } else if (/^[A-Za-z_]/.test(token) && tokens[i - 1] !== ".") {
          let j = i + 1, partial = false;
          if (tokens[j] === ".") { j += 2; partial = true; }
          else if (tokens[j] === "[") {
            while (j < tokens.length && tokens[j] !== "]" && j - i < SCAN_TOKENS)
              j++;
            j++; partial = true;
          }
          if (!/^[-+*\/]?=$/.test(tokens[j] || "")) continue;
          let end = j + 1;
          for (let open = 0; end < tokens.length && end - j < SCAN_TOKENS; end++) {
            const next = tokens[end];
            if (next === "(") open++;
            else if (next === ")") open--;
            else if (!open && (next === ";" || next === ",")) break;
          }
          const rhs = tokens.slice(j + 1, end),
            truncated = end - j >= SCAN_TOKENS;
          if (!assigned.has(token)) assigned.set(token, []);
          assigned.get(token).push({ index: at[i], rhs,
            partial: partial || truncated || tokens[j] !== "=" });
          if (truncated || rhs.some(perToken)) tainted.add(token);
        }
      }
      return { text, tokens, at, flow, assigned,
        line: (index) => text.slice(0, index).split("\n").length };
    },
    /* Splits an expression at top-level `*`, unwrapping whole-factor
       parentheses, so `(uA * uB) * p` and `uA * uB * p` read alike. */
    shaderFactors = (tokens, depth = 0) => {
      const factors = [];
      let open = 0, start = 0;
      for (let i = 0; i <= tokens.length; i++) {
        const token = tokens[i];
        if (token === "(") open++;
        else if (token === ")") open--;
        else if (i === tokens.length || (token === "*" && !open)) {
          const factor = tokens.slice(start, i);
          let closes = 0, whole = factor[0] === "(" && depth < 4;
          for (let k = 0; whole && k < factor.length; k++) {
            closes += factor[k] === "(" ? 1 : factor[k] === ")" ? -1 : 0;
            if (!closes && k < factor.length - 1) whole = false;
          }
          factors.push(...(whole
            ? shaderFactors(factor.slice(1, -1), depth + 1) : [factor]));
          start = i + 1;
        }
      }
      return factors;
    },
    inspectProgram = (vs, fs, t) => {
      const errors = [], warnings = [],
        varyings = declarations(fs, "varying").entries.map((e) => e.name),
        samplers = t.uniforms.filter((e) => e.type === "sampler2D")
          .map((e) => e.name),
        vertex = scanShader(vs, t.attributes.map((e) => e.name)),
        fragment = scanShader(fs,
          [...varyings, "gl_FragCoord", "gl_FrontFacing", "gl_PointCoord"]),
        report = (list, scan, index, text) => list.push(
          (list === errors ? "ERROR: " : "WARNING: ")
          + (scan === vertex ? "vertex" : "fragment") + " shader line "
          + scan.line(index) + ": " + text),
        quote = (tokens) => {
          const text = tokens.join("");
          return "'" + (text.length > 48 ? text.slice(0, 45) + "..." : text) + "'";
        },
        number = (token) => /^\.?\d/.test(token || "") ? Number(token) : NaN,
        sampled = new Set();
      let constant = null, exact = true, sampleIndex = 0;
      for (const scan of [vertex, fragment]) {
        const unit = scan === vertex ? "vertex" : "pixel";
        for (const entry of scan.flow) {
          if (entry.refused)
            report(errors, scan, entry.index,
              `'${entry.keyword}' is not available on the PSP GE`);
          else if (entry.perData)
            report(errors, scan, entry.index, `'${entry.keyword}' depends on `
              + `per-${unit} data; the PSP GE cannot branch per ${unit}`);
          else {
            report(warnings, scan, entry.index, `'${entry.keyword}' on `
              + "uniforms is not evaluated on the PSP; its branches are "
              + "ignored");
            exact = false;
          }
        }
        scan.tokens.forEach((token, i) => {
          if (!/^texture/.test(token) || scan.tokens[i + 1] !== "(") return;
          if (scan === vertex || token !== "texture2D")
            report(errors, scan, scan.at[i], `'${token}' is not available; `
              + "only texture2D in the fragment shader is");
          else {
            sampled.add(scan.tokens[i + 2]);
            sampleIndex = scan.at[i];
          }
        });
      }
      if (sampled.size > 1)
        report(errors, fragment, sampleIndex,
          `${sampled.size} textures are sampled; MAX_TEXTURE_IMAGE_UNITS is 1`);
      if (errors.length) return { errors, warnings };

      /* The GE colour is texture * vertex colour * instance colour * one
         colour uniform or constant. Every factor of gl_FragColor, and of the
         varyings it reads, must be one of those. The factors also choose
         the inputs: names only guess (aVertexPosition contains "tex"). */
      const output = fragment.assigned.get("gl_FragColor") || [],
        attribute = (name) => t.attributes.find((entry) =>
          entry.name === name && entry.type !== "mat4"),
        colorUniform = (name) => t.uniforms.find((entry) =>
          entry.name === name && entry.type === "vec4"),
        colors = new Set(), uniformColors = new Set(), varyingUses = [],
        unapplied = (scan, index, factor, target) => {
          report(warnings, scan, index,
            `${quote(factor)} is not applied to ${target} on the PSP`);
          exact = false;
        },
        readFragment = (tokens, index, depth) => {
          for (const factor of shaderFactors(tokens)) {
            const [name] = factor, single = factor.length === 1,
              local = single && fragment.assigned.get(name);
            if (single && varyings.includes(name))
              varyingUses.push({ name, index });
            else if (single && colorUniform(name)) uniformColors.add(name);
            else if (local?.length === 1 && !local[0].partial && depth < 4)
              readFragment(local[0].rhs, local[0].index, depth + 1);
            else if (name === "texture2D" && factor.length === 6
                && samplers.includes(factor[2]) && factor[3] === ","
                && varyings.includes(factor[4]))
              varyingUses.push({ name: factor[4], index, texture: true });
            else if (name === "vec4" && !constant
                && (factor.length === 4 || factor.length === 10)
                && factor.every((token, k) => k < 2 || k === factor.length - 1
                  || (k % 2 ? token === "," : number(token) >= 0))) {
              const values = factor.filter((_, k) => k > 1 && !(k % 2))
                .map((token) => Math.min(1, number(token)));
              constant = new Float32Array(values.length === 1
                ? [values[0], values[0], values[0], values[0]] : values);
            } else unapplied(fragment, index, factor, "gl_FragColor");
          }
        };
      let instanceRead = false, textureRead = false;
      if (output.length !== 1 || output[0].partial) {
        report(warnings, fragment, output[0]?.index ?? 0, "gl_FragColor is "
          + "written in parts or more than once; only one whole write is "
          + "translated");
        exact = false;
      } else readFragment(output[0].rhs, output[0].index, 0);
      for (const use of varyingUses) {
        const writes = vertex.assigned.get(use.name),
          write = writes?.length === 1 && !writes[0].partial && writes[0];
        if (!write) {
          report(warnings, writes ? vertex : fragment,
            writes?.[0].index ?? use.index, `varying ${use.name} needs one `
              + "whole write in the vertex shader");
          exact = false;
        } else if (use.texture) {
          textureRead = true;
          const entry = attribute(write.rhs.join(""));
          if (entry) t.texcoord = entry;
          else unapplied(vertex, write.index, write.rhs, "texture coordinates");
        } else for (const factor of shaderFactors(write.rhs)) {
          const name = factor.join(""), entry = attribute(name);
          if (entry && entry === t.instanceColor) instanceRead = true;
          else if (entry) colors.add(entry);
          else if (colorUniform(name)) uniformColors.add(name);
          else unapplied(vertex, write.index, factor, `varying ${use.name}`);
        }
      }
      if (colors.size > 1 || uniformColors.size + !!constant > 1) {
        report(warnings, fragment, output[0].index, "the PSP applies one "
          + "colour attribute and one colour uniform or constant; the rest "
          + "are not applied");
        exact = false;
      }
      if (colors.size) t.color = [...colors][0];
      if (uniformColors.size)
        t.uniformColor = colorUniform([...uniformColors][0]);
      /* A fully recognised colour drops the inputs it never reads. */
      if (exact) {
        if (!colors.size) t.color = null;
        if (!instanceRead) t.instanceColor = null;
        if (!uniformColors.size) t.uniformColor = null;
        t.usesTexture = textureRead;
      }
      t.constantColor = t.uniformColor ? null : constant;

      /* gl_Position must be a matrix chain ending in the position attribute,
         vec4(position, 0, 1), or the documented instance-transform form. */
      const position = vertex.assigned.get("gl_Position") || [],
        p = t.position.name, m = t.instanceTransform?.name;
      if (position.length !== 1 || position[0].partial)
        report(warnings, vertex, position[0]?.index ?? 0, "gl_Position is "
          + "written in parts or more than once; only one whole write is "
          + "translated");
      else for (const factor of shaderFactors(position[0].rhs)) {
        const name = factor.join(""), close = factor.lastIndexOf(","),
          inner = factor.slice(2, factor.indexOf(",")).join(""),
          tail = factor.slice(factor.indexOf(",") + 1, -1)
            .filter((token) => token !== ",").map(number);
        if (t.matrices.some((entry) => entry.name === name)
            || name === t.instanceMatrix?.name || name === p
            || (factor[0] === "vec4" && close > 0
              && (inner === p || (m && inner === `${p}*${m}.w+${m}.xyz`))
              && tail.every((value, k) => value === (k < tail.length - 1
                ? 0 : 1)))) continue;
        report(warnings, vertex, position[0].index, quote(factor)
          + " is not applied to gl_Position on the PSP");
      }
      return { errors, warnings };
    },
    glTypeFor = (name) => ({
      float: constants.FLOAT, vec2: constants.FLOAT_VEC2,
      vec3: constants.FLOAT_VEC3, vec4: constants.FLOAT_VEC4,
      mat2: constants.FLOAT_MAT2, mat3: constants.FLOAT_MAT3,
      mat4: constants.FLOAT_MAT4, sampler2D: constants.SAMPLER_2D,
      bool: 0x8b56, int: 0x1404, samplerCube: 0x8b60,
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
      uniformColor: new Float32Array(4), blend: 0,
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
    },
    /* [start, length] vertex ranges of one instance's split draw (see
       _enqueueSplitDraw); n is over limit and the mode is not a loop or fan. */
    splitVertexRanges = (mode, n, limit) => {
      const ranges = [];
      if (mode === constants.LINE_STRIP || mode === constants.TRIANGLE_STRIP) {
        const shared = mode === constants.LINE_STRIP ? 1 : 2,
          primitives = n - shared,
          most = limit - shared - (mode === constants.LINE_STRIP ? 0
            : (limit - shared) % 2),
          parts = Math.ceil(primitives / most);
        let size = Math.ceil(primitives / parts);
        if (shared === 2) size += size % 2;
        for (let start = 0; start < primitives; start += size)
          ranges.push(start, Math.min(size, primitives - start) + shared);
        return ranges;
      }
      const unit = mode === constants.TRIANGLES ? 3
          : mode === constants.LINES ? 2 : 1,
        usable = n - n % unit, most = limit - limit % unit,
        parts = Math.ceil(usable / most),
        size = Math.ceil(usable / parts / unit) * unit;
      for (let start = 0; start < usable; start += size)
        ranges.push(start, Math.min(size, usable - start));
      return ranges;
    },
    /* The instance attribute as read from instance `base` onwards. */
    shiftedInstanceAttribute = (attribute, base) => {
      if (!attribute || !base) return attribute;
      const stride = attribute.stride
        || attribute.size * typeBytes(attribute.type);
      return {
        enabled: attribute.enabled, buffer: attribute.buffer,
        size: attribute.size, type: attribute.type,
        normalized: attribute.normalized, stride: attribute.stride,
        offset: attribute.offset + base * stride, divisor: attribute.divisor,
      };
    },
    /* The parts of a split draw, each with its own command template
       (a part plan that inherits the draw plan's admission). */
    splitDrawParts = (plan, mode, first, count, index, instances,
                      instanceMatrix, instanceColor, instanceTransform,
                      limit) => {
      const parts = [],
        add = (start, length, base, partInstances) => {
          const partPlan = Object.create(plan);
          partPlan.commandTemplateU32 = null;
          partPlan.commandTemplateI32 = null;
          partPlan.commandTemplateRevision = 0;
          partPlan.commandTemplateId = 0;
          partPlan.commandPacketEpoch = -1;
          partPlan.commandPacketSlot = -1;
          partPlan.commandPacketMask = 0;
          parts.push({
            start, first: index ? 0 : first + start, count: length,
            instances: partInstances, plan: partPlan,
            index: index ? makeCommandIndex() : null,
            instanceMatrix: shiftedInstanceAttribute(instanceMatrix, base),
            instanceColor: shiftedInstanceAttribute(instanceColor, base),
            instanceTransform:
              shiftedInstanceAttribute(instanceTransform, base),
          });
        };
      if (count <= limit) {
        const groups = Math.ceil(instances / Math.floor(limit / count)),
          size = Math.ceil(instances / groups);
        for (let base = 0; base < instances; base += size)
          add(0, count, base, Math.min(size, instances - base));
        return parts;
      }
      const ranges = splitVertexRanges(mode, count, limit);
      for (let base = 0; base < instances; base++)
        for (let at = 0; at < ranges.length; at += 2)
          add(ranges[at], ranges[at + 1], base, 1);
      return parts;
    };

  class GLObject {
    constructor(owner, id) {
      this._owner = owner;
      this._id = id;
      this._deleted = false;
    }
  }
  class WebGLBufferImpl extends GLObject {
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
  class WebGLTextureImpl extends GLObject {
    constructor(owner, id) {
      super(owner, id); this._width = 0; this._height = 0;
      this._pixels = null; this._generation = 1;
      this._min = constants.LINEAR; this._mag = constants.LINEAR;
      this._wrapS = constants.CLAMP_TO_EDGE;
      this._wrapT = constants.CLAMP_TO_EDGE;
    }
  }
  class WebGLShaderImpl extends GLObject {
    constructor(owner, id, type) {
      super(owner, id); this._type = type; this._source = "";
      this._compiled = false; this._log = "";
    }
  }
  class WebGLProgramImpl extends GLObject {
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
  class WebGLUniformLocationImpl {
    constructor(program, entry, offset = 0) {
      this._program = program; this._name = entry.name;
      this._type = entry.type; this._size = entry.size;
      this._offset = offset;
    }
  }
  class WebGLActiveInfoImpl {
    constructor(size, type, name) { this.size = size; this.type = type; this.name = name; }
  }
  class WebGLShaderPrecisionFormatImpl {
    constructor(rangeMin, rangeMax, precision) {
      this.rangeMin = rangeMin; this.rangeMax = rangeMax;
      this.precision = precision;
    }
  }
  class WebGLFramebufferImpl extends GLObject {}
  class WebGLRenderbufferImpl extends GLObject {}
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
        VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE;
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
      this.VERTEX_ARRAY_BINDING_OES = VERTEX_ARRAY_BINDING_OES;
    }
    createVertexArrayOES() { return this._context._createVertexArray(); }
    deleteVertexArrayOES(array) { this._context._deleteVertexArray(array); }
    isVertexArrayOES(array) {
      return owned(this._context, array, WebGLVertexArrayOES)
        && array._everBound;
    }
    bindVertexArrayOES(array) { this._context._bindVertexArray(array); }
  }
  class ExtBlendMinmax {
    constructor() { this.MIN_EXT = MIN_EXT; this.MAX_EXT = MAX_EXT; }
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

  /* BEGIN shader semantics */
  /* Link-time GLSL semantics: a bounded preprocessor, a symbolic
     evaluator of main() and the mapping of its outputs onto GE
     fixed-function terms. Every walk is bounded; any construct
     outside the mapping throws SemError and the caller keeps the
     older inspector's refusal or warnings. */
  const shaderSemantics = (() => {
    /* Bounded GLSL ES 1.0 preprocessor. Input must already have comments
       blanked.
       Inactive lines become empty lines so line numbers are preserved. */
    const PP_EXPANSIONS = 20000, PP_OUTPUT = 256 * 1024, PP_DEPTH = 32;
    const preprocessShader = (text) => {
      const defines = new Map(), lines = text.split("\n"), out = [],
        stack = [], fail = (message, line) => ({ error:
          `preprocessor line ${line + 1}: ${message}` });
      let expansions = 0, bytes = 0;
      const identifier = /[A-Za-z_]\w*/y;
      const expand = (source, hidden, depth) => {
        if (depth > PP_DEPTH) throw new Error("macro nesting is too deep");
        let result = "", at = 0;
        while (at < source.length) {
          const c = source[at];
          if (/[0-9.]/.test(c)) {
            const number = /(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?[A-Za-z_\w]*/y;
            number.lastIndex = at;
            const m = number.exec(source);
            if (m) { result += m[0]; at += m[0].length; continue; }
          }
          identifier.lastIndex = at;
          const m = /[A-Za-z_]/.test(c) ? identifier.exec(source) : null;
          if (!m) { result += c; at++; continue; }
          const name = m[0], macro = defines.get(name);
          at += name.length;
          if (!macro || hidden.has(name)) { result += name; continue; }
          if (++expansions > PP_EXPANSIONS) throw new Error("too many macro expansions");
          if (!macro.params) {
            hidden.add(name);
            result += expand(macro.body, hidden, depth + 1);
            hidden.delete(name);
            continue;
          }
          let scan = at;
          while (scan < source.length && /\s/.test(source[scan])) scan++;
          if (source[scan] !== "(") { result += name; continue; }
          const args = [];
          let open = 0, start = scan + 1, end = -1;
          for (let k = scan; k < source.length; k++) {
            const t = source[k];
            if (t === "(") open++;
            else if (t === ")") {
              if (--open === 0) { args.push(source.slice(start, k)); end = k; break; }
            } else if (t === "," && open === 1) { args.push(source.slice(start, k)); start = k + 1; }
          }
          if (end < 0) throw new Error(`unterminated call of macro ${name}`);
          if (args.length === 1 && !args[0].trim() && !macro.params.length) args.length = 0;
          if (args.length !== macro.params.length)
            throw new Error(`macro ${name} takes ${macro.params.length} arguments`);
          const values = args.map((arg) => expand(arg.trim(), hidden, depth + 1));
          const body = macro.body.replace(/[A-Za-z_]\w*/g, (word) => {
            const index = macro.params.indexOf(word);
            return index < 0 ? word : values[index];
          });
          hidden.add(name);
          result += expand(body, hidden, depth + 1);
          hidden.delete(name);
          at = end + 1;
        }
        return result;
      };
      /* #if arithmetic: integers, defined(), unary ! ~ - +, binary C operators. */
      const evaluate = (expression) => {
        const resolved = expression.replace(
          /\bdefined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|([A-Za-z_]\w*))/g,
          (_, a, b) => defines.has(a || b) ? " 1 " : " 0 ");
        const tokens = expand(resolved, new Set(), 0).match(
          /0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?|[A-Za-z_]\w*|<<|>>|<=|>=|==|!=|&&|\|\||\S/g) || [];
        let i = 0;
        const precedence = { "||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6,
          "<": 7, ">": 7, "<=": 7, ">=": 7, "<<": 8, ">>": 8, "+": 9, "-": 9,
          "*": 10, "/": 10, "%": 10 };
        const unary = () => {
          const t = tokens[i++];
          if (t === "(") { const v = binary(0); if (tokens[i++] !== ")") throw new Error("expected )"); return v; }
          if (t === "!") return unary() ? 0 : 1;
          if (t === "-") return -unary();
          if (t === "+") return +unary();
          if (t === "~") return ~unary();
          if (t === undefined) throw new Error("incomplete #if expression");
          if (/^\d/.test(t)) return Number.parseInt(t, /^0[xX]/.test(t) ? 16 : /^0\d/.test(t) ? 8 : 10);
          if (/^[A-Za-z_]/.test(t)) return 0;
          throw new Error(`unexpected '${t}' in #if`);
        };
        const binary = (minimum) => {
          let left = unary();
          for (;;) {
            const op = tokens[i], level = precedence[op];
            if (!level || level <= minimum - 1 || level < minimum) break;
            i++;
            const right = binary(level + 1);
            left = op === "||" ? (left || right ? 1 : 0) : op === "&&" ? (left && right ? 1 : 0)
              : op === "|" ? left | right : op === "^" ? left ^ right : op === "&" ? left & right
              : op === "==" ? +(left === right) : op === "!=" ? +(left !== right)
              : op === "<" ? +(left < right) : op === ">" ? +(left > right)
              : op === "<=" ? +(left <= right) : op === ">=" ? +(left >= right)
              : op === "<<" ? left << right : op === ">>" ? left >> right
              : op === "+" ? left + right : op === "-" ? left - right
              : op === "*" ? left * right
              : right === 0 ? (() => { throw new Error("division by zero in #if"); })()
              : op === "/" ? Math.trunc(left / right) : left % right;
          }
          return left;
        };
        const value = binary(0);
        if (i !== tokens.length) throw new Error("trailing tokens in #if");
        return value !== 0;
      };
      const active = () => stack.every((entry) => entry.active);
      for (let line = 0; line < lines.length; line++) {
        let text = lines[line];
        const directive = /^\s*#\s*(\w*)\s*(.*)$/.exec(text);
        try {
          if (directive) {
            const [, keyword, rest] = directive, live = active();
            if (keyword === "ifdef" || keyword === "ifndef") {
              const name = /^[A-Za-z_]\w*/.exec(rest)?.[0];
              if (!name) return fail(`#${keyword} needs a macro name`, line);
              const condition = live && (defines.has(name) === (keyword === "ifdef"));
              stack.push({ active: condition, taken: condition, parent: live });
            } else if (keyword === "if") {
              const condition = live && evaluate(rest);
              stack.push({ active: condition, taken: condition, parent: live });
            } else if (keyword === "elif" || keyword === "else") {
              const top = stack[stack.length - 1];
              if (!top || top.sawElse) return fail(`unexpected #${keyword}`, line);
              if (keyword === "else") top.sawElse = true;
              const condition = top.parent && !top.taken
                && (keyword === "else" || evaluate(rest));
              top.active = condition; top.taken ||= condition;
            } else if (keyword === "endif") {
              if (!stack.pop()) return fail("unexpected #endif", line);
            } else if (!live) {
              /* skipped */
            } else if (keyword === "define") {
              const m = /^([A-Za-z_]\w*)(\(([^)]*)\))?\s*(.*)$/.exec(rest);
              if (!m) return fail("malformed #define", line);
              if (/^(GL_|__)/.test(m[1])) return fail(`#define of reserved ${m[1]}`, line);
              if (defines.size >= 512 && !defines.has(m[1]))
                return fail("too many macros", line);
              defines.set(m[1], { params: m[2] === undefined ? null
                : m[3].split(",").map((p) => p.trim()).filter(Boolean), body: m[4].trim() });
            } else if (keyword === "undef") {
              defines.delete(/^[A-Za-z_]\w*/.exec(rest)?.[0]);
            } else if (keyword === "error") {
              return fail(`#error ${rest}`, line);
            } else if (!/^(version|extension|pragma|line|)$/.test(keyword)) {
              return fail(`unknown directive #${keyword}`, line);
            }
            out.push("");
            continue;
          }
          out.push(active() ? expand(text, new Set(), 0) : "");
        } catch (error) {
          return fail(error.message, line);
        }
        bytes += out[out.length - 1].length + 1;
        if (bytes > PP_OUTPUT) return fail("expanded shader is too large", line);
      }
      if (stack.length) return fail("missing #endif", lines.length - 1);
      return { text: out.join("\n"), defines };
    };
    /* The link-time semantic pass.
       Symbolically evaluates the preprocessed vertex and fragment main() into
       expression DAGs, inlining helper functions, then maps the outputs onto
       the GE's fixed-function terms. Every walk is bounded. */

    const SEM_TOKENS = 60000, SEM_NODES = 40000, SEM_STATEMENTS = 6000,
      SEM_DEPTH = 24;

    const tokenize = (text) => {
      const tokens = [], at = [];
      const pattern = /[A-Za-z_]\w*|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?|\+\+|--|[-+*\/]=|==|!=|<=|>=|&&|\|\||\^\^|\S/g;
      for (let m; (m = pattern.exec(text));) {
        if (tokens.length >= SEM_TOKENS) throw new SemError("shader is too long to analyse");
        tokens.push(m[0]); at.push(m.index);
      }
      return { tokens, at };
    };

    /* A token run with the identifier `keep` as P, every other identifier
       (locals, parameters, fields) as _, and numbers in one spelling. */
    const shapeTokens = (tokens, start, end, keep) => {
      let shape = "";
      for (let k = start; k < end; k++) {
        const t = tokens[k];
        shape += (t === keep ? "P" : /^\d|^\.\d/.test(t) ? String(Number(t))
          : /^[A-Za-z_]/.test(t) && !TYPES.has(t) && t !== "return" ? "_" : t)
          + " ";
      }
      return shape;
    };
    /* Light-probe bodies (probeParameter): band 0 of the probe alone, and
       three.js r128's shGetIrradianceAt. */
    const SH_PROBE_BODIES = ["{return P[0]*0.886227;}",
      "{float x=n.x,y=n.y,z=n.z;vec3 r=P[0]*0.886227;"
      + "r+=P[1]*2.0*0.511664*y;r+=P[2]*2.0*0.511664*z;"
      + "r+=P[3]*2.0*0.511664*x;r+=P[4]*2.0*0.429043*x*y;"
      + "r+=P[5]*2.0*0.429043*y*z;r+=P[6]*(0.743125*z*z-0.247708);"
      + "r+=P[7]*2.0*0.429043*x*z;r+=P[8]*0.429043*(x*x-y*y);return r;}"];
    let shProbeShapeList = null;
    const shProbeShapes = () => shProbeShapeList ??= SH_PROBE_BODIES.map(
      (body) => { const { tokens } = tokenize(body);
        return shapeTokens(tokens, 0, tokens.length, "P"); });

    class SemError extends Error {}
    const fail = (message) => { throw new SemError(message); };

    const TYPES = new Set(["void", "bool", "int", "float", "vec2", "vec3", "vec4",
      "bvec2", "bvec3", "bvec4", "ivec2", "ivec3", "ivec4", "mat2", "mat3", "mat4",
      "sampler2D", "samplerCube"]);
    const QUALIFIERS = new Set(["const", "in", "out", "inout", "highp", "mediump",
      "lowp", "precision", "invariant", "uniform", "attribute", "varying"]);
    const size = (type) => type === "float" || type === "int" || type === "bool" ? 1
      : /^[bi]?vec([234])$/.test(type) ? Number(type.slice(-1))
      : type === "mat2" ? 4 : type === "mat3" ? 9 : type === "mat4" ? 16 : 0;
    const vecType = (n) => n === 1 ? "float" : "vec" + n;

    /* ---- parsing ------------------------------------------------------- */
    const parseProgram = (text) => {
      const { tokens, at } = tokenize(text), structs = new Map(),
        globals = new Map(), functions = new Map();
      let i = 0;
      const peek = (k = 0) => tokens[i + k], next = () => tokens[i++],
        expect = (t) => { if (tokens[i] !== t) fail(`expected '${t}' near '${tokens[i]}'`); i++; };
      const isType = (t) => TYPES.has(t) || structs.has(t);
      const skipQualifiers = () => {
        const found = [];
        while (QUALIFIERS.has(peek())) found.push(next());
        return found;
      };
      const parseStruct = () => {
        expect("struct"); const name = next(); expect("{");
        const fields = [];
        while (peek() !== "}") {
          skipQualifiers(); const type = next();
          for (;;) {
            const field = next(); let count = 0;
            if (peek() === "[") { next(); count = Number(next()); expect("]"); }
            fields.push({ name: field, type, count });
            if (peek() === ",") { next(); continue; }
            expect(";"); break;
          }
          if (i >= tokens.length) fail("unterminated struct");
        }
        expect("}");
        structs.set(name, fields);
        return name;
      };
      const skipBlock = () => {
        const start = i; let depth = 0;
        do {
          if (tokens[i] === "{") depth++;
          else if (tokens[i] === "}") depth--;
          i++;
        } while (depth > 0 && i < tokens.length);
        return [start, i];
      };
      while (i < tokens.length) {
        if (peek() === "precision") { while (next() !== ";"); continue; }
        if (peek() === ";") { next(); continue; }
        const qualifiers = skipQualifiers();
        let type;
        if (peek() === "struct") {
          type = parseStruct();
          if (peek() === ";") { next(); continue; }
        } else type = next();
        if (!isType(type)) fail(`unknown type '${type}'`);
        const name = next();
        if (peek() === "(") {
          next(); const params = [];
          while (peek() !== ")") {
            const pq = skipQualifiers(), ptype = next();
            if (ptype === "void" && peek() === ")") break;
            const pname = next(); let count = 0;
            if (peek() === "[") { next(); count = Number(next()); expect("]"); }
            params.push({ name: pname, type: ptype, count,
              out: pq.includes("out") || pq.includes("inout") });
            if (peek() === ",") next();
          }
          expect(")");
          if (peek() === ";") { next(); continue; }
          const [start, end] = skipBlock();
          functions.set(name, { name, type, params, start, end });
          continue;
        }
        for (let declared = name;;) {
          let count = 0, init = null;
          if (peek() === "[") { next(); count = Number(next()); expect("]"); }
          if (peek() === "=") {
            next(); const s = i; let depth = 0;
            while (i < tokens.length && !(depth === 0 && (peek() === "," || peek() === ";"))) {
              if (peek() === "(") depth++; else if (peek() === ")") depth--; i++;
            }
            init = [s, i];
          }
          const storage = qualifiers.find((q) => q === "uniform" || q === "attribute"
            || q === "varying") || (qualifiers.includes("const") ? "const" : "global");
          globals.set(declared, { name: declared, type, count, storage, init, at: at[i - 1] });
          if (peek() === ",") { next(); declared = next(); continue; }
          expect(";"); break;
        }
      }
      return { tokens, at, structs, globals, functions, text };
    };

    /* ---- expression nodes ---------------------------------------------- */
    const F_UNIFORM = 1, F_ATTRIBUTE = 2, F_VARYING = 4, F_PIXEL = 8,
      F_TEXTURE = 16, F_LIGHT = 32, F_UNKNOWN = 64;
    /* A texture read: only texture2D is drawn, but a built-in lit material's
       cube environment map is recognised so that it can be left out. */
    const SAMPLE_CALL = /^texture(2D|Cube)$/;

    const makeEvaluator = (program, stage, options) => {
      let nodes = 0, statements = 0;
      const { tokens, at, structs, globals, functions } = program;
      const node = (props) => {
        if (++nodes > SEM_NODES) fail("shader is too large to analyse");
        props.flags ??= 0; return props;
      };
      const num = (v, t = "float") => node({ k: "num", t, v });
      const isNum = (n, v) => n.k === "num" && (v === undefined || n.v === v);
      const lightUniform = (g) => /Light$/.test(g.type) || g.name === "ambientLightColor";
      /* The parameter at which a helper takes a light probe's nine
         spherical-harmonic coefficients, or -1. Recognised by what the
         helper computes, never by a name: a vec3[9] parameter whose body is
         one of SH_PROBE_BODIES with every other identifier renamed (the
         band-0 term alone, or three.js's whole irradiance evaluation), or
         one used only as that argument of such a helper. */
      const probeParameters = new Map();
      const probeParameter = (fn, depth = 0) => {
        if (probeParameters.has(fn.name)) return probeParameters.get(fn.name);
        probeParameters.set(fn.name, -1);
        let found = -1;
        for (let k = 0; k < fn.params.length && found < 0; k++) {
          const p = fn.params[k];
          if (p.type === "vec3" && p.count === 9 && !p.out
              && probeBody(fn, p.name, depth)) found = k;
        }
        probeParameters.set(fn.name, found);
        return found;
      };
      const probeBody = (fn, name, depth) => {
        if (shProbeShapes().includes(shapeTokens(tokens, fn.start, fn.end, name)))
          return true;
        let passed = false;
        for (let k = fn.start; k < fn.end; k++) {
          if (tokens[k] !== name) continue;
          if (!/^[(,]$/.test(tokens[k - 1]) || !/^[),]$/.test(tokens[k + 1])
              || depth >= SEM_DEPTH) return false;
          let position = 0, nest = 0, open = -1;
          for (let j = k - 1; j > fn.start && open < 0; j--) {
            const t = tokens[j];
            if (t === ")" || t === "]") nest++;
            else if (t === "(" || t === "[") {
              if (nest === 0) open = t === "(" ? j : 0; else nest--;
            } else if (t === "," && nest === 0) position++;
          }
          const callee = open > 0 ? functions.get(tokens[open - 1]) : null;
          if (!callee || callee === fn
              || probeParameter(callee, depth + 1) !== position) return false;
          passed = true;
        }
        return passed;
      };
      /* Globals referenced (transitively) by each function, for light opacity. */
      const reads = new Map();
      const functionReads = (fn, depth = 0) => {
        if (reads.has(fn.name)) return reads.get(fn.name);
        const found = new Set(); reads.set(fn.name, found);
        for (let k = fn.start; k < fn.end; k++) {
          const t = tokens[k];
          if (globals.has(t)) found.add(t);
          else if (functions.has(t) && t !== fn.name && depth < SEM_DEPTH)
            for (const g of functionReads(functions.get(t), depth + 1)) found.add(g);
        }
        return found;
      };
      const readsLight = (fn) => [...functionReads(fn)].some((name) =>
        lightUniform(globals.get(name)));

      const typeOfMember = (type, field) => {
        const fields = structs.get(type);
        if (fields) {
          const f = fields.find((x) => x.name === field);
          if (!f) fail(`no field ${field} in ${type}`);
          return f.type;
        }
        const n = size(type);
        if (/^[bi]?vec/.test(type) || type === "float") {
          if (!/^[xyzwrgbastpq]{1,4}$/.test(field)) fail(`bad swizzle .${field}`);
          return (type[0] === "b" ? "bvec" : type[0] === "i" ? "ivec" : "vec")
            .replace(/vec$/, field.length === 1 ? "" : "vec")
            + (field.length === 1 ? (type[0] === "b" ? "bool" : type[0] === "i" ? "int" : "float") : field.length);
        }
        fail(`member .${field} of ${type}`);
      };
      const normSwizzle = (s) => s.replace(/[rs]/g, "x").replace(/[gt]/g, "y")
        .replace(/[bp]/g, "z").replace(/[aq]/g, "w");
      const opaqueOf = (origin, type, flags) => {
        if (structs.has(type)) {
          const fields = new Map();
          for (const f of structs.get(type))
            fields.set(f.name, opaqueOf({ ...origin, field: f.name }, f.type, flags));
          return { k: "struct", t: type, fields, origin, flags };
        }
        return node({ k: "opaque", t: type, origin, flags });
      };

      /* -- constructors / folding -- */
      const swizzle = (a, s, t) => {
        s = normSwizzle(s);
        if (a.k === "num") return num(a.v, t);
        if (a.k === "ctor" && a.args.every((x) => size(x.t) === 1)
            && a.args.length === size(a.t)) {
          const picked = [...s].map((c) => a.args["xyzw".indexOf(c)]);
          return picked.length === 1 ? picked[0] : ctor(t, picked);
        }
        if (a.k === "ctor" && a.args.length === 1 && size(a.args[0].t) === 1)
          return s.length === 1 ? a.args[0] : ctor(t, [a.args[0]]);
        if (s === "xyzw".slice(0, size(a.t)) && s.length === size(a.t)) return a;
        if (a.k === "swz") {
          const inner = [...s].map((c) => a.s["xyzw".indexOf(c)]).join("");
          return swizzle(a.a, inner, t);
        }
        if (a.k === "ctor" && a.args.length >= 1) {
          /* vec4(v3, f).xyz -> v3 ; .w -> f */
          let offset = 0; const parts = [];
          for (const arg of a.args) { parts.push({ arg, from: offset, n: size(arg.t) }); offset += size(arg.t); }
          const idx = [...s].map((c) => "xyzw".indexOf(c));
          const part = parts.find((p) => idx[0] >= p.from && idx[0] < p.from + p.n);
          if (part && idx.every((x, k) => x === idx[0] + k) && idx[idx.length - 1] < part.from + part.n) {
            const local = idx.map((x) => "xyzw"[x - part.from]).join("");
            return local === "xyzw".slice(0, part.n) ? part.arg : swizzle(part.arg, local, t);
          }
        }
        if (a.k === "merge") {
          /* reading back the components a partial write replaced */
          const idx = [...s].map((c) => "xyzw".indexOf(c)),
            written = [...a.s].map((c) => "xyzw".indexOf(c));
          if (idx.every((x) => written.includes(x))) {
            const local = idx.map((x) => "xyzw"[written.indexOf(x)]).join("");
            return local === "xyzw".slice(0, size(a.v.t)) && local.length === size(a.v.t)
              ? a.v : swizzle(a.v, local, t);
          }
          if (idx.every((x) => !written.includes(x))) return swizzle(a.a, s, t);
        }
        return node({ k: "swz", t, a, s, flags: a.flags });
      };
      const ctor = (t, args) => {
        if (args.length === 1 && args[0].t === t) return args[0];
        if (args.length === 1 && args[0].k === "num" && size(t) <= 4)
          return node({ k: "ctor", t, args: [args[0]], flags: 0 });
        if (t === "float" && args.length === 1) return args[0].t === "float" ? args[0]
          : swizzle(args[0], "x", "float");
        return node({ k: "ctor", t, args, flags: args.reduce((f, a) => f | a.flags, 0) });
      };
      const binary = (o, a, b) => {
        const t = resultType(o, a.t, b.t);
        if (a.k === "num" && b.k === "num") {
          const v = o === "+" ? a.v + b.v : o === "-" ? a.v - b.v : o === "*" ? a.v * b.v
            : o === "/" ? a.v / b.v : null;
          if (v !== null) return num(v, t);
        }
        if (o === "*" && isNum(a, 1) && size(b.t) >= size(a.t)) return b;
        if (o === "*" && isNum(b, 1) && size(a.t) >= size(b.t)) return a;
        if (o === "*" && a.k === "ctor" && a.args.length === 1 && isNum(a.args[0], 1)
            && size(b.t) === size(a.t)) return b;
        if (o === "*" && b.k === "ctor" && b.args.length === 1 && isNum(b.args[0], 1)
            && size(a.t) === size(b.t)) return a;
        if (o === "+" && (isNum(a, 0) || (a.k === "ctor" && a.args.length === 1 && isNum(a.args[0], 0)))
            && size(b.t) >= size(a.t)) return b;
        if (o === "+" && (isNum(b, 0) || (b.k === "ctor" && b.args.length === 1 && isNum(b.args[0], 0)))
            && size(a.t) >= size(b.t)) return a;
        return node({ k: "op", o, t, a, b, flags: a.flags | b.flags });
      };
      const resultType = (o, ta, tb) => {
        if (["<", ">", "<=", ">=", "==", "!=", "&&", "||", "^^"].includes(o)) return "bool";
        if (ta === tb) return ta;
        if (o === "*" && /^mat/.test(ta) && /vec/.test(tb)) return tb;
        if (o === "*" && /vec/.test(ta) && /^mat/.test(tb)) return ta;
        return size(ta) >= size(tb) ? ta : tb;
      };
      const BUILTIN_TYPES = { dot: "float", length: "float", distance: "float",
        any: "bool", all: "bool" };
      const call = (f, args) => {
        const t = BUILTIN_TYPES[f] || (SAMPLE_CALL.test(f) ? "vec4"
          : f === "cross" ? "vec3" : args[0]?.t || "float");
        if (args.every((a) => a.k === "num")) {
          const v = args.map((a) => a.v), M = Math;
          const folded = { abs: M.abs, sqrt: M.sqrt, exp: M.exp, exp2: (x) => 2 ** x,
            log: M.log, log2: M.log2, pow: M.pow, floor: M.floor, ceil: M.ceil, sin: M.sin,
            cos: M.cos, min: M.min, max: M.max, sign: M.sign,
            clamp: (x, lo, hi) => M.min(M.max(x, lo), hi), fract: (x) => x - M.floor(x),
            mod: (x, y) => x - y * M.floor(x / y), inversesqrt: (x) => 1 / M.sqrt(x),
            radians: (x) => x * M.PI / 180, degrees: (x) => x * 180 / M.PI }[f];
          if (folded && t === "float") return num(folded(...v));
        }
        let flags = args.reduce((acc, a) => acc | a.flags, 0);
        if (SAMPLE_CALL.test(f)) flags |= F_TEXTURE;
        if (/^(dFdx|dFdy|fwidth)$/.test(f)) flags |= F_PIXEL;
        return node({ k: "call", f, t, args, flags });
      };

      /* -- values for globals -- */
      const globalCache = new Map();
      const globalValue = (name) => {
        if (globalCache.has(name)) return globalCache.get(name);
        const g = globals.get(name);
        let value;
        const leaf = (path, type, flags, kind) => {
          if (structs.has(type)) {
            const fields = new Map();
            for (const f of structs.get(type)) {
              if (f.count) {
                const items = [];
                for (let k = 0; k < f.count; k++) items.push(leaf(`${path}.${f.name}[${k}]`, f.type, flags, kind));
                fields.set(f.name, { k: "array", t: f.type, items });
              } else fields.set(f.name, leaf(`${path}.${f.name}`, f.type, flags, kind));
            }
            return { k: "struct", t: type, fields, flags };
          }
          return node({ k: kind, t: type, name: path, flags });
        };
        if (g.storage === "uniform") {
          const flags = F_UNIFORM | (lightUniform(g) ? F_LIGHT : 0);
          if (g.count) {
            const items = [];
            for (let k = 0; k < g.count; k++) items.push(leaf(`${name}[${k}]`, g.type, flags, "unif"));
            value = { k: "array", t: g.type, items };
          } else value = leaf(name, g.type, flags, "unif");
        } else if (g.storage === "attribute") value = node({ k: "attr", t: g.type, name, flags: F_ATTRIBUTE });
        else if (g.storage === "varying") {
          value = stage === "fragment"
            ? node({ k: "vary", t: g.type, name, flags: F_VARYING | (options.varyingFlags?.get(name) || 0) })
            : { k: "undef", t: g.type };
        } else if (g.init) {
          value = evalTokens(g.init[0], g.init[1], new Map());
        } else value = { k: "undef", t: g.type };
        globalCache.set(name, value);
        return value;
      };
      const BUILTIN_INPUTS = { gl_FragCoord: "vec4", gl_PointCoord: "vec2", gl_FrontFacing: "bool" };

      /* -- expression parser + evaluator over a token range -- */
      const scopeLookup = (scopes, name) => {
        for (let s = scopes.length - 1; s >= 0; s--) if (scopes[s].has(name)) return scopes[s];
        return null;
      };
      let evalTokens; // assigned below
      const parseExpression = (ctx) => {
        const { scopes } = ctx;
        const PREC = { "||": 1, "^^": 2, "&&": 3, "==": 4, "!=": 4, "<": 5, ">": 5,
          "<=": 5, ">=": 5, "+": 6, "-": 6, "*": 7, "/": 7 };
        const primary = () => {
          const t = tokens[ctx.i++];
          if (t === "(") { const v = ternary(); if (tokens[ctx.i++] !== ")") fail("expected )"); return postfix(v); }
          if (t === "-") return unaryMinus(prefixed());
          if (t === "+") return prefixed();
          if (t === "!") { const v = prefixed(); return node({ k: "op", o: "!", t: "bool", a: v, b: num(0), flags: v.flags }); }
          if (/^(\d|\.\d)/.test(t)) return postfix(num(Number(t), /[.eE]/.test(t) ? "float" : "int"));
          if (t === "true" || t === "false") return num(t === "true" ? 1 : 0, "bool");
          if (TYPES.has(t) || structs.has(t)) {
            const args = callArgs();
            if (structs.has(t)) {
              const fields = new Map();
              structs.get(t).forEach((f, k) => fields.set(f.name, args[k]));
              return postfix({ k: "struct", t, fields, flags: args.reduce((a, x) => a | (x.flags || 0), 0) });
            }
            return postfix(construct(t, args));
          }
          if (/^[A-Za-z_]/.test(t)) {
            if (tokens[ctx.i] === "(") {
              const args = callArgs(true);
              return postfix(invoke(t, args, ctx));
            }
            const scope = scopeLookup(scopes, t);
            if (scope) return postfix(scope.get(t));
            if (outputs.has(t)) return postfix(outputs.get(t));
            if (globals.has(t)) return postfix(globalValue(t));
            if (BUILTIN_INPUTS[t]) return postfix(node({ k: "input", t: BUILTIN_INPUTS[t], name: t, flags: F_PIXEL }));
            fail(`unknown identifier '${t}'`);
          }
          fail(`unexpected '${t}'`);
        };
        const prefixed = () => {
          if (tokens[ctx.i] === "++" || tokens[ctx.i] === "--") fail("'++' and '--' are not translated");
          return primary();
        };
        const unaryMinus = (v) => v.k === "num" ? num(-v.v, v.t)
          : node({ k: "op", o: "neg", t: v.t, a: v, b: num(0), flags: v.flags });
        const callArgs = (rawLvalues = false) => {
          if (tokens[ctx.i++] !== "(") fail("expected (");
          const args = [];
          if (tokens[ctx.i] === ")") { ctx.i++; return args; }
          for (;;) {
            const start = ctx.i;
            args.push(ternary());
            args[args.length - 1].__start = start; args[args.length - 1].__end = ctx.i;
            const t = tokens[ctx.i++];
            if (t === ")") break;
            if (t !== ",") fail("expected , or )");
          }
          return args;
        };
        const postfix = (v) => {
          for (;;) {
            const t = tokens[ctx.i];
            if (t === ".") {
              ctx.i++; const field = tokens[ctx.i++];
              if (v.k === "struct") {
                if (!v.fields.has(field)) fail(`no field ${field}`);
                v = v.fields.get(field);
              } else if (v.k === "opaque" && structs.has(v.t)) {
                v = opaqueOf({ ...v.origin, field }, typeOfMember(v.t, field), v.flags);
              } else v = swizzle(v, field, typeOfMember(v.t, field));
              if (v === undefined) fail(`undefined field ${field}`);
            } else if (t === "[") {
              ctx.i++; const index = ternary();
              if (tokens[ctx.i++] !== "]") fail("expected ]");
              if (index.k !== "num") fail("non-constant index");
              if (v.k === "array") {
                if (!v.items[index.v]) fail("index out of range");
                v = v.items[index.v];
              } else if (/^mat/.test(v.t)) {
                v = node({ k: "col", t: vecType(Math.sqrt(size(v.t))), a: v, i: index.v, flags: v.flags });
              } else v = swizzle(v, "xyzw"[index.v], v.t[0] === "i" ? "int" : "float");
            } else if (t === "++" || t === "--") fail("'++' and '--' are not translated");
            else return v;
          }
        };
        const binaryLevel = (minimum) => {
          let left = prefixed();
          for (;;) {
            const o = tokens[ctx.i], level = PREC[o];
            if (!level || level < minimum) return left;
            ctx.i++;
            const right = binaryLevel(level + 1);
            left = binary(o, left, right);
          }
        };
        const ternary = () => {
          const c = binaryLevel(1);
          if (tokens[ctx.i] !== "?") return c;
          ctx.i++; const a = ternary();
          if (tokens[ctx.i++] !== ":") fail("expected :");
          const b = ternary();
          return select(c, a, b);
        };
        return { ternary, postfix };
      };
      const select = (c, a, b) => {
        if (c.k === "num") return c.v ? a : b;
        if (a === b) return a;
        if (a.k === "struct" || b.k === "struct") {
          const fields = new Map();
          for (const [name, value] of a.fields) fields.set(name, select(c, value, b.fields.get(name)));
          return { k: "struct", t: a.t, fields, flags: c.flags | a.flags | b.flags };
        }
        if (a.k === "undef") return b;
        if (b.k === "undef") return a;
        return node({ k: "sel", t: a.t, c, a, b, flags: c.flags | a.flags | b.flags });
      };
      const construct = (t, args) => {
        const flat = args.flatMap((a) => a.k === "struct" ? fail("struct in vector constructor") : [a]);
        if (/^mat/.test(t) && flat.length === 1 && /^mat/.test(flat[0].t))
          return node({ k: "ctor", t, args: flat, flags: flat[0].flags });
        return ctor(t, flat);
      };
      const invoke = (name, args, ctx) => {
        const fn = functions.get(name);
        if (!fn) return call(name, args);
        if (ctx.depth >= SEM_DEPTH) fail("helper functions nest too deeply");
        const argFlags = args.reduce((f, a) => f | (a.flags || 0) | (a.k === "struct" ? structFlags(a) : 0), 0);
        /* A helper is a light term when it reads light uniforms or is handed a
           light struct; plain math on lit values (tone mapping) still inlines. */
        const lightStruct = args.some((a) => a.k === "struct" && (structFlags(a) & F_LIGHT));
        const probeAt = probeParameter(fn), probe = probeAt >= 0
          && args[probeAt]?.k === "array" && args[probeAt].items.length === 9
          && args[probeAt].items.every((item) => item.k === "unif")
          ? args[probeAt] : null;
        if (lightStruct || probe || readsLight(fn)) {
          /* Lighting math is not translated; it becomes one opaque, recorded
             term whose arguments the GE mapping reads. */
          const origin = { fn: name, args, probe };
          if (lightCalls.length < 64) lightCalls.push(origin);
          fn.params.forEach((p, k) => {
            if (p.out) assign(ctx, args[k], opaqueOf({ ...origin, param: p.name }, p.type, F_LIGHT));
          });
          return fn.type === "void" ? { k: "void", t: "void" }
            : opaqueOf(origin, fn.type, F_LIGHT | argFlags);
        }
        const scope = new Map();
        fn.params.forEach((p, k) => scope.set(p.name, args[k]));
        const inner = { scopes: [scope], depth: ctx.depth + 1, ret: null, guards: [] };
        runBlock(fn.start + 1, fn.end - 1, inner);
        fn.params.forEach((p, k) => { if (p.out) assign(ctx, args[k], scope.get(p.name)); });
        if (fn.type === "void") return { k: "void", t: "void" };
        if (!inner.ret) fail(`helper ${name} has no return`);
        return inner.ret;
      };
      const structFlags = (s) => {
        let f = s.flags || 0;
        for (const v of s.fields.values()) f |= v.k === "struct" ? structFlags(v) : (v.flags || 0);
        return f;
      };
      /* Assign through an argument expression captured as a token range. */
      const assign = (ctx, arg, value) => {
        if (arg.__start === undefined) fail("out argument is not an lvalue");
        writeLvalue(ctx, arg.__start, arg.__end, "=", value);
      };
      const writeLvalue = (ctx, start, end, op, value) => {
        const name = tokens[start];
        const scope = scopeLookup(ctx.scopes, name);
        let root;
        if (scope) root = scope.get(name);
        else if (name === "gl_FragColor" || name === "gl_Position" || name === "gl_PointSize"
            || (globals.has(name) && globals.get(name).storage === "varying" && stage === "vertex")
            || (globals.has(name) && globals.get(name).storage === "global")) {
          root = outputs.has(name) ? outputs.get(name)
            : globals.has(name) ? globalValue(name) : { k: "undef", t: name === "gl_PointSize" ? "float" : "vec4" };
        } else fail(`cannot assign to '${name}'`);
        const path = [];
        for (let k = start + 1; k < end;) {
          if (tokens[k] === ".") { path.push({ field: tokens[k + 1] }); k += 2; }
          else if (tokens[k] === "[") {
            const sub = { i: k + 1, scopes: ctx.scopes, depth: ctx.depth };
            const index = parseExpression(sub).ternary();
            if (index.k !== "num") fail("non-constant index in assignment");
            path.push({ index: index.v }); k = sub.i + 1;
          } else fail("unsupported assignment target");
        }
        const update = (current, steps) => {
          if (!steps.length) {
            if (op === "=") return value;
            if (current.k === "undef") fail("compound assignment to an unset value");
            return binary(op[0], current, value);
          }
          const [step, ...rest] = steps;
          if (step.index !== undefined) {
            if (current.k === "array") {
              const items = current.items.slice();
              items[step.index] = update(items[step.index], rest);
              return { ...current, items };
            }
            fail("indexed write into a vector or matrix");
          }
          if (current.k === "struct" || (current.k === "undef" && structs.has(current.t))) {
            const fields = new Map(current.k === "struct" ? current.fields : []);
            if (current.k === "undef")
              for (const f of structs.get(current.t)) fields.set(f.name, { k: "undef", t: f.type });
            fields.set(step.field, update(fields.get(step.field), rest));
            return { k: "struct", t: current.t, fields, flags: 0 };
          }
          if (current.k === "opaque" && structs.has(current.t)) {
            const expanded = opaqueOf(current.origin, current.t, current.flags);
            return update(expanded, steps);
          }
          if (rest.length) fail("nested swizzle write");
          const s = normSwizzle(step.field), t = typeOfMember(current.t, step.field);
          const old = current.k === "undef" ? null : swizzle(current, s, t);
          const v = op === "=" ? value : binary(op[0], old ?? fail("partial write of unset"), value);
          if (current.k === "undef") fail("partial write of an unset vector");
          return node({ k: "merge", t: current.t, a: current, s, v, flags: current.flags | v.flags });
        };
        let next = update(root, path);
        for (const guard of ctx.guards) next = select(guard, next, root);
        if (scope) scope.set(name, next);
        else outputs.set(name, next);
      };

      const outputs = new Map(), discards = [], lightCalls = [];
      const runStatement = (ctx) => {
        if (++statements > SEM_STATEMENTS) fail("shader has too many statements to analyse");
        const t = tokens[ctx.i];
        if (t === "{") {
          ctx.i++; ctx.scopes.push(new Map());
          while (tokens[ctx.i] !== "}") { if (ctx.i >= tokens.length) fail("unterminated block"); runStatement(ctx); }
          ctx.i++; ctx.scopes.pop(); return;
        }
        if (t === ";") { ctx.i++; return; }
        if (/^(for|while|do)$/.test(t)) fail(`'${t}' is not available on the PSP GE`);
        if (t === "discard") {
          ctx.i++; if (tokens[ctx.i++] !== ";") fail("expected ;");
          discards.push({ guards: ctx.guards.slice() });
          return;
        }
        if (t === "return") {
          ctx.i++;
          let value = { k: "void", t: "void" };
          if (tokens[ctx.i] !== ";") value = parseExpression(ctx).ternary();
          if (tokens[ctx.i++] !== ";") fail("expected ;");
          let result = value;
          for (let g = ctx.guards.length - 1; g >= 0; g--) result = { pendingGuard: ctx.guards[g], value: result };
          if (!ctx.guards.length) {
            ctx.ret = ctx.pending ? ctx.pending(value) : value;
            ctx.done = true;
          } else {
            const guard = ctx.guards.reduce((a, b) => binary("&&", a, b));
            const prior = ctx.pending;
            ctx.pending = (rest) => {
              const merged = select(guard, value, rest);
              return prior ? prior(merged) : merged;
            };
            ctx.guards.push(node({ k: "op", o: "!", t: "bool", a: guard, b: num(0), flags: guard.flags }));
            ctx.returnGuards = (ctx.returnGuards || 0) + 1;
            ctx.skipRest = true;
          }
          return;
        }
        if (t === "if") {
          ctx.i++; if (tokens[ctx.i++] !== "(") fail("expected (");
          const c = parseExpression(ctx).ternary();
          if (tokens[ctx.i++] !== ")") fail("expected )");
          const thenStart = ctx.i; skipStatement(ctx); const thenEnd = ctx.i;
          if (c.flags & (F_ATTRIBUTE | F_VARYING | F_PIXEL | F_TEXTURE)) {
            /* The one per-pixel branch the GE has: 'if (alpha < ref) discard;'. */
            const lone = (thenEnd - thenStart === 2 && tokens[thenStart] === "discard")
              || (thenEnd - thenStart === 4 && tokens[thenStart] === "{" && tokens[thenStart + 1] === "discard");
            if (stage === "fragment" && lone && tokens[ctx.i] !== "else") {
              discards.push({ guards: ctx.guards.slice(), cond: c });
              return;
            }
            fail(`'if' depends on per-${stage === "vertex" ? "vertex" : "pixel"} data`);
          }
          let elseStart = -1, elseEnd = -1;
          if (tokens[ctx.i] === "else") { ctx.i++; elseStart = ctx.i; skipStatement(ctx); elseEnd = ctx.i; }
          const resume = ctx.i;
          if (c.k === "num") {
            if (c.v) { ctx.i = thenStart; runStatement(ctx); }
            else if (elseStart >= 0) { ctx.i = elseStart; runStatement(ctx); }
            ctx.i = resume; return;
          }
          const notC = node({ k: "op", o: "!", t: "bool", a: c, b: num(0), flags: c.flags });
          ctx.guards.push(c); ctx.i = thenStart; runStatement(ctx); ctx.guards.pop();
          if (elseStart >= 0) { ctx.guards.push(notC); ctx.i = elseStart; runStatement(ctx); ctx.guards.pop(); }
          ctx.i = resume; return;
        }
        /* declaration */
        let k = ctx.i;
        while (QUALIFIERS.has(tokens[k])) k++;
        if ((TYPES.has(tokens[k]) || structs.has(tokens[k])) && /^[A-Za-z_]/.test(tokens[k + 1] || "")
            && tokens[k + 1] !== "(") {
          const type = tokens[k]; ctx.i = k + 1;
          for (;;) {
            const name = tokens[ctx.i++];
            let count = 0;
            if (tokens[ctx.i] === "[") { ctx.i++; count = Number(tokens[ctx.i++]); ctx.i++; }
            let value = count ? { k: "array", t: type, items: Array.from({ length: count }, () => ({ k: "undef", t: type })) }
              : { k: "undef", t: type };
            if (tokens[ctx.i] === "=") { ctx.i++; value = parseExpression(ctx).ternary(); }
            ctx.scopes[ctx.scopes.length - 1].set(name, value);
            const sep = tokens[ctx.i++];
            if (sep === ";") break;
            if (sep !== ",") fail("expected , or ;");
          }
          return;
        }
        /* assignment or call */
        const start = ctx.i;
        let depth = 0, opAt = -1;
        for (let j = ctx.i; j < tokens.length; j++) {
          const tok = tokens[j];
          if (tok === "(" || tok === "[") depth++;
          else if (tok === ")" || tok === "]") depth--;
          else if (depth === 0 && /^([-+*\/]?=)$/.test(tok)) { opAt = j; break; }
          else if (depth === 0 && tok === ";") break;
        }
        if (opAt >= 0) {
          ctx.i = opAt + 1;
          const value = parseExpression(ctx).ternary();
          if (tokens[ctx.i++] !== ";") fail("expected ;");
          writeLvalue(ctx, start, opAt, tokens[opAt], value);
          return;
        }
        parseExpression(ctx).ternary();
        if (tokens[ctx.i++] !== ";") fail("expected ;");
      };
      const skipStatement = (ctx) => {
        if (tokens[ctx.i] === "{") {
          let depth = 0;
          do { if (tokens[ctx.i] === "{") depth++; else if (tokens[ctx.i] === "}") depth--; ctx.i++; }
          while (depth > 0 && ctx.i < tokens.length);
          return;
        }
        if (tokens[ctx.i] === "if") {
          ctx.i++; let depth = 0;
          do { if (tokens[ctx.i] === "(") depth++; else if (tokens[ctx.i] === ")") depth--; ctx.i++; }
          while (depth > 0);
          skipStatement(ctx);
          if (tokens[ctx.i] === "else") { ctx.i++; skipStatement(ctx); }
          return;
        }
        let depth = 0;
        while (ctx.i < tokens.length) {
          const tok = tokens[ctx.i++];
          if (tok === "(") depth++; else if (tok === ")") depth--;
          else if (tok === ";" && depth === 0) return;
        }
      };
      const runBlock = (start, end, ctx) => {
        ctx.i = start;
        while (ctx.i < end && !ctx.done) {
          runStatement(ctx);
        }
        if (ctx.pending && !ctx.done) fail("helper does not return on every path");
      };
      evalTokens = (start, end, scope) => {
        const ctx = { i: start, scopes: [scope], depth: 0, guards: [] };
        const v = parseExpression(ctx).ternary();
        return v;
      };
      const runMain = () => {
        const main = functions.get("main");
        if (!main) fail("no main()");
        const ctx = { scopes: [new Map()], depth: 0, ret: null, guards: [] };
        runBlock(main.start + 1, main.end - 1, ctx);
        return { outputs, discards, lightCalls };
      };
      return { runMain, line: (index) => program.text.slice(0, index).split("\n").length };
    };

    const analyse = (vsText, fsText) => {
      const vs = parseProgram(vsText), fs = parseProgram(fsText);
      const vertex = makeEvaluator(vs, "vertex", {}).runMain();
      const varyingFlags = new Map();
      for (const [name, value] of vertex.outputs) varyingFlags.set(name, value.flags || 0);
      const fragment = makeEvaluator(fs, "fragment", { varyingFlags }).runMain();
      return { vs, fs, vertex, fragment };
    };

    /* ---- printing for inspection -------------------------------------- */
    const show = (n, depth = 0) => {
      if (!n) return "?";
      if (depth > 12) return "…";
      switch (n.k) {
        case "num": return String(+n.v.toFixed(6));
        case "unif": case "attr": case "vary": case "input": return n.name;
        case "op": return n.o === "neg" ? `-(${show(n.a, depth + 1)})` : n.o === "!" ? `!(${show(n.a, depth + 1)})`
          : `(${show(n.a, depth + 1)} ${n.o} ${show(n.b, depth + 1)})`;
        case "call": return `${n.f}(${n.args.map((a) => show(a, depth + 1)).join(", ")})`;
        case "ctor": return `${n.t}(${n.args.map((a) => show(a, depth + 1)).join(", ")})`;
        case "swz": return `${show(n.a, depth + 1)}.${n.s}`;
        case "col": return `${show(n.a, depth + 1)}[${n.i}]`;
        case "sel": return `(${show(n.c, depth + 1)} ? ${show(n.a, depth + 1)} : ${show(n.b, depth + 1)})`;
        case "merge": return `merge(${show(n.a, depth + 1)}.${n.s} = ${show(n.v, depth + 1)})`;
        case "opaque": return `LIGHT<${n.origin.fn}${n.origin.param ? ":" + n.origin.param : ""}${n.origin.field ? "." + n.origin.field : ""}>`;
        case "struct": return `{${[...n.fields].map(([k, v]) => k + ":" + show(v, depth + 1)).join(", ")}}`;
        case "undef": return "undef";
        default: return n.k;
      }
    };

    /* Prototype: map the semantic DAGs onto GE fixed-function terms. */

    const LIT_MODELS = { MeshLambertMaterial: "lambert", MeshPhongMaterial: "phong",
      MeshStandardMaterial: "standard", MeshPhysicalMaterial: "standard" };

    const uniformOnly = (n) => n && !(n.flags & ~F_UNIFORM) && n.k !== "opaque"
      && n.k !== "struct" && n.k !== "array" && n.k !== "undef" && !hasKind(n, "input");
    const hasKind = (n, kind, depth = 0) => {
      if (!n || depth > 64) return false;
      if (n.k === kind) return true;
      for (const c of [n.a, n.b, n.c, n.v, ...(n.args || [])])
        if (c && typeof c === "object" && hasKind(c, kind, depth + 1)) return true;
      return false;
    };
    const isNum = (n, v) => n?.k === "num" && (v === undefined || n.v === v);

    const buildPlan = (analysis, shaderName) => {
      const warnings = [], vout = analysis.vertex.outputs, fout = analysis.fragment.outputs;
      const vglobals = analysis.vs.globals, fglobals = analysis.fs.globals;
      const position = vout.get("gl_Position"), fragColor = fout.get("gl_FragColor");
      if (!position || position.k === "undef") fail("gl_Position is not written");
      if (!fragColor || fragColor.k === "undef") fail("gl_FragColor is not written");
      const used = { attributes: new Set(), uniforms: new Map() };
      const noteUniforms = (n, depth = 0) => {
        if (!n || typeof n !== "object" || depth > 200) return;
        if (n.k === "unif") { used.uniforms.set(n.name, n.t); return; }
        if (n.k === "struct") { for (const v of n.fields.values()) noteUniforms(v, depth + 1); return; }
        if (n.k === "array") { for (const v of n.items) noteUniforms(v, depth + 1); return; }
        if (n.k === "opaque") { for (const a of n.origin.args || []) noteUniforms(a, depth + 1); return; }
        for (const c of [n.a, n.b, n.c, n.v, ...(n.args || [])]) noteUniforms(c, depth + 1);
      };
      /* -- position: a chain of mat4 uniforms applied to the position attribute -- */
      const matrices = (n) => n.k === "unif" && n.t === "mat4" ? [n.name]
        : n.k === "op" && n.o === "*" && n.t === "mat4" ? (() => {
          const a = matrices(n.a), b = a && matrices(n.b); return b ? [...a, ...b] : null; })()
        : null;
      const base = (n) => {
        if (n.k === "attr" && n.t === "vec4") return n.name;
        if (n.k !== "ctor" || n.t !== "vec4") return null;
        const [p, ...rest] = n.args;
        if (p?.k !== "attr") return null;
        const tail = rest.map((x) => x.k === "num" ? x.v : NaN);
        if (p.t === "vec3" && tail.length === 1 && tail[0] === 1) return p.name;
        if (p.t === "vec2" && tail.length === 2 && tail[0] === 0 && tail[1] === 1) return p.name;
        return null;
      };
      const chain = (n) => {
        const b = base(n);
        if (b) return { matrices: [], attribute: b };
        if (n.k !== "op" || n.o !== "*" || n.a.t !== "mat4") return null;
        const m = matrices(n.a), rest = m && chain(n.b);
        return rest ? { matrices: [...m, ...rest.matrices], attribute: rest.attribute } : null;
      };
      const clip = chain(position);
      if (!clip) fail("gl_Position is not a matrix chain applied to the position attribute");
      if (clip.matrices.length > 4) fail("position transform exceeds four matrices");
      used.attributes.add(clip.attribute);
      /* The eye-space split comes from any other output that reads the same
         chain without its leading matrices (fog depth, point size, lighting). */
      let eyeSplit = -1;
      const suffix = (m) => m.length < clip.matrices.length
        && m.every((name, k) => name === clip.matrices[clip.matrices.length - m.length + k])
        && m.length > 0;
      const findEye = (n, depth = 0) => {
        if (!n || typeof n !== "object" || depth > 200) return;
        if (n.k === "op" && n.o === "*" && n.a?.t === "mat4") {
          const c = chain(n);
          if (c && c.attribute === clip.attribute && suffix(c.matrices))
            eyeSplit = Math.max(eyeSplit, clip.matrices.length - c.matrices.length);
        }
        if (n.k === "struct") { for (const v of n.fields.values()) findEye(v, depth + 1); return; }
        if (n.k === "opaque") { for (const a of n.origin.args || []) findEye(a, depth + 1); return; }
        for (const c of [n.a, n.b, n.c, n.v, ...(n.args || [])]) findEye(c, depth + 1);
      };
      for (const [name, value] of vout) if (name !== "gl_Position") findEye(value);
      for (const call of analysis.vertex.lightCalls) for (const a of call.args) findEye(a);
      const eyeDepth = (n) => n.k === "op" && n.o === "neg" && n.a.k === "swz" && n.a.s === "z"
        && (() => { const c = chain(n.a.a); return c && c.attribute === clip.attribute
          && (c.matrices.length === 0 ? false : suffix(c.matrices)); })();
      const plan = { warnings, used, projection: [], modelView: clip.matrices,
        position: clip.attribute, matrices: clip.matrices };
      if (eyeSplit > 0) {
        plan.projection = clip.matrices.slice(0, eyeSplit);
        plan.modelView = clip.matrices.slice(eyeSplit);
      }
      for (const m of clip.matrices) used.uniforms.set(m, "mat4");

      /* normal attribute: normalMatrix * normal */
      const findNormal = (n, depth = 0) => {
        if (!n || typeof n !== "object" || depth > 200) return null;
        if (n.k === "op" && n.o === "*" && n.a?.k === "unif" && n.a.t === "mat3"
            && n.b?.k === "attr" && n.b.t === "vec3") return { matrix: n.a.name, attribute: n.b.name };
        if (n.k === "struct") { for (const v of n.fields.values()) { const r = findNormal(v, depth + 1); if (r) return r; } return null; }
        if (n.k === "opaque") { for (const a of n.origin.args || []) { const r = findNormal(a, depth + 1); if (r) return r; } return null; }
        for (const c of [n.a, n.b, n.c, n.v, ...(n.args || [])]) { const r = findNormal(c, depth + 1); if (r) return r; }
        return null;
      };

      /* -- texture maps beside the base colour map --
         The GE samples one texture. A built-in lit material may read more:
         normal, environment, light, ambient-occlusion, emissive and
         material-input maps. Each is recognised by where its value flows,
         never by its name, and left out with a warning naming it. The base
         colour map, the texture that multiplies the diffuse colour, is the
         one drawn; a second texture there is still refused. */
      const samplerOf = (n) => n?.k === "call" && SAMPLE_CALL.test(n.f)
        && n.args[0]?.k === "unif" ? n.args[0] : null;
      /* The texture reads a value makes, by sampler, outside lighting terms. */
      const samplesIn = (n, out = new Map(), seen = new Set(), depth = 0) => {
        if (!n || typeof n !== "object" || seen.has(n) || depth > 200) return out;
        seen.add(n);
        const sampler = samplerOf(n);
        if (sampler && !out.has(sampler.name)) out.set(sampler.name, n);
        if (n.k === "struct") for (const v of n.fields.values()) samplesIn(v, out, seen, depth + 1);
        else if (n.k === "array") for (const v of n.items) samplesIn(v, out, seen, depth + 1);
        else if (n.k !== "opaque")
          for (const c of [n.a, n.b, n.c, n.v, ...(n.args || [])]) samplesIn(c, out, seen, depth + 1);
        return out;
      };
      /* The samplers a colour multiplies directly (through products,
         swizzles and constructors, not through sums or functions). */
      const directSamplers = (n, out = new Set(), depth = 0) => {
        if (!n || typeof n !== "object" || depth > 64) return out;
        const sampler = samplerOf(n);
        if (sampler) out.add(sampler.name);
        else if ((n.k === "op" && n.o === "*") || n.k === "swz" || n.k === "ctor" || n.k === "merge")
          for (const c of [n.a, n.b, n.v, ...(n.args || [])]) directSamplers(c, out, depth + 1);
        return out;
      };
      /* An environment map is a cube map, or a 2D map looked up by anything
         but a mesh texture coordinate (a reflection vector, for example). */
      const meshVarying = (name) => {
        let v = vout.get(name);
        if (v?.k === "swz" && v.s === "xy" && v.a.k === "op" && v.a.o === "*"
            && uniformOnly(v.a.a) && v.a.b.k === "ctor") v = v.a.b.args[0];
        return v?.k === "attr" && v.t === "vec2";
      };
      const readsMeshCoordinate = (n, seen = new Set(), depth = 0) => {
        if (!n || typeof n !== "object" || seen.has(n) || depth > 200) return false;
        seen.add(n);
        if (n.k === "vary") return meshVarying(n.name);
        return [n.a, n.b, n.c, n.v, ...(n.args || [])]
          .some((c) => readsMeshCoordinate(c, seen, depth + 1));
      };
      const environmentRead = (read) => read.args[0].t === "samplerCube"
        || !readsMeshCoordinate(read.args[1]);
      const normalVaryings = new Set([...vout].filter(([, v]) => findNormal(v))
        .map(([name]) => name));
      const readsNormal = (n, seen = new Set(), depth = 0) => {
        if (!n || typeof n !== "object" || seen.has(n) || depth > 200) return false;
        seen.add(n);
        if (n.k === "vary") return normalVaryings.has(n.name);
        if (n.k === "opaque" || n.k === "struct" || n.k === "array") return false;
        return [n.a, n.b, n.c, n.v, ...(n.args || [])]
          .some((c) => readsNormal(c, seen, depth + 1));
      };
      /* True when the value adds what the sampler reads to light. */
      const addsLight = (n, name, seen = new Set(), depth = 0) => {
        if (!n || typeof n !== "object" || seen.has(n) || depth > 200
            || n.k === "opaque") return false;
        seen.add(n);
        if (n.k === "op" && (n.o === "+" || n.o === "-")
            && [[n.a, n.b], [n.b, n.a]].some(([x, y]) =>
              (y.flags & F_LIGHT) && samplesIn(x).has(name))) return true;
        const children = n.k === "struct" ? [...n.fields.values()]
          : [n.a, n.b, n.c, n.v, ...(n.args || [])];
        return children.some((c) => addsLight(c, name, seen, depth + 1));
      };
      const MAP_ROLES = {
        normal: "perturbs the normal; the GE lights with vertex normals",
        environment: "is an environment map; the PSP draws no reflections",
        reflection: "scales the reflection; the PSP draws no reflections",
        light: "adds light; the PSP draws without it",
        occlusion: "scales light; the PSP draws without it",
        emissive: "scales the emissive colour; the PSP draws the colour without it",
        input: "scales a material input; the PSP draws the input without it",
      };
      const leftOut = new Map();
      const leaveOut = (name, role) => {
        if (!leftOut.has(name)) leftOut.set(name, new Set());
        leftOut.get(name).add(role);
      };
      /* The value with the given samplers read as white, folded. */
      const fold = (n) => {
        n.flags = [n.a, n.b, n.c, n.v, ...(n.args || [])]
          .reduce((f, c) => f | (c && typeof c === "object" ? c.flags || 0 : 0), 0);
        if (n.k === "call" && SAMPLE_CALL.test(n.f)) n.flags |= F_TEXTURE;
        if (n.k === "call" && /^(dFdx|dFdy|fwidth)$/.test(n.f)) n.flags |= F_PIXEL;
        if (!n.flags && n.k !== "sel" && size(n.t) && size(n.t) <= 4) {
          let value;
          try { value = evaluate(n, () => fail("constant"), 0); }
          catch { fail(`'${show(n).slice(0, 40)}' has no GE equivalent`); }
          const values = typeof value === "number" ? [value] : Array.from(value);
          if (values.length === size(n.t) || values.length === 1) {
            if (values.every((x) => x === values[0]))
              return { k: "num", t: n.t, v: values[0], flags: 0 };
            return { k: "ctor", t: n.t, flags: 0,
              args: values.map((v) => ({ k: "num", t: "float", v, flags: 0 })) };
          }
        }
        if (n.k === "op" && n.o === "*") {
          if (isNum(n.a, 1) && n.b.t === n.t) return n.b;
          if (isNum(n.b, 1) && n.a.t === n.t) return n.a;
          if (isNum(n.a, 0) || isNum(n.b, 0)) return { k: "num", t: n.t, v: 0, flags: 0 };
        }
        if (n.k === "op" && (n.o === "+" || n.o === "-") && isNum(n.b, 0) && n.a.t === n.t)
          return n.a;
        if (n.k === "op" && n.o === "+" && isNum(n.a, 0) && n.b.t === n.t) return n.b;
        return n;
      };
      const whiten = (n, names) => {
        const memo = new Map();
        const walk = (x, depth) => {
          if (!x || typeof x !== "object" || depth > 200) return x;
          if (memo.has(x)) return memo.get(x);
          let out = x;
          if (names.has(samplerOf(x)?.name))
            out = { k: "ctor", t: x.t, args: [{ k: "num", t: "float", v: 1, flags: 0 }], flags: 0 };
          else if (["op", "call", "ctor", "swz", "sel", "merge", "col"].includes(x.k)) {
            const copy = { ...x };
            let changed = false;
            for (const key of ["a", "b", "c", "v"])
              if (x[key] && typeof x[key] === "object") {
                copy[key] = walk(x[key], depth + 1); changed ||= copy[key] !== x[key];
              }
            if (x.args) {
              copy.args = x.args.map((a) => walk(a, depth + 1));
              changed ||= copy.args.some((a, k) => a !== x.args[k]);
            }
            if (changed) out = fold(copy);
          }
          memo.set(x, out);
          return out;
        };
        return walk(n, 0);
      };
      /* An environment map's blend into a lit material's colour,
         mix(colour, f(environment), k), keeps the colour. */
      const withoutEnvironmentBlend = (n) => {
        const environmentIn = (x) => [...samplesIn(x)].some(([, read]) => environmentRead(read));
        while (n.k === "call" && n.f === "mix" && !environmentIn(n.args[0])
            && environmentIn({ k: "list", args: n.args.slice(1) })) {
          for (const [name, read] of samplesIn({ k: "list", args: n.args.slice(1) }))
            if (!samplesIn(n.args[0]).has(name))
              leaveOut(name, environmentRead(read) ? "environment" : "reflection");
          n = n.args[0];
        }
        return n;
      };
      let baseSamplers = null;
      const extraSamplers = (n) => [...samplesIn(n)]
        .filter(([name]) => !baseSamplers?.has(name));

      /* -- colour factors -- */
      const vertexExpression = (n) => {
        const v = vout.get(n.name);
        if (!v || v.k === "undef") fail(`varying ${n.name} is not written`);
        return v;
      };
      const color = { rgb: [], a: [], texture: null, vertexColor: null, light: [],
        scale: { rgb: 1, a: 1 }, program: { rgb: [], a: [] } };
      /* A colour term of attributes and uniforms only (varyings replaced by
         their vertex-stage values) is shaded per vertex in software and
         interpolated by the GE; anything reading a texture, the pixel or
         lights is not. Returns the vertex-stage term, or null. */
      const perVertex = (n, stage) => {
        const seen = new Map();
        const sub = (x, depth) => {
          if (depth > 64) return null;
          if (seen.has(x)) return seen.get(x);
          let out = null;
          if (x.k === "vary") out = stage === "fragment"
            ? sub(vertexExpression(x), depth + 1) : null;
          else if (x.k === "num" || x.k === "unif" || x.k === "attr") out = x;
          else if (samplerOf(x)) out = null;
          else if (["op", "call", "ctor", "swz", "sel"].includes(x.k)) {
            const copy = { ...x };
            let ok = true, flags = 0;
            for (const key of ["a", "b", "c"]) if (x[key]) {
              copy[key] = sub(x[key], depth + 1); ok &&= !!copy[key];
              flags |= copy[key]?.flags || 0;
            }
            if (x.args) {
              copy.args = x.args.map((a) => sub(a, depth + 1));
              ok &&= copy.args.every(Boolean);
              for (const a of copy.args) flags |= a?.flags || 0;
            }
            copy.flags = flags;
            out = ok ? copy : null;
          }
          seen.set(x, out);
          return out;
        };
        const out = sub(n, 0);
        return out && !(out.flags & ~(F_UNIFORM | F_ATTRIBUTE))
          && (out.flags & F_ATTRIBUTE) ? out : null;
      };
      const addTexture = (n, sel) => {
        const [sampler, coord] = n.args;
        if (sampler.k !== "unif" || sampler.t !== "sampler2D") fail("texture2D needs a sampler uniform");
        const key = sampler.name + "|" + show(coord);
        if (color.texture && color.texture.key !== key) fail("more than one texture is sampled");
        color.texture ??= { key, sampler: sampler.name, coord, sel: new Set() };
        color.texture.sel.add(sel);
        used.uniforms.set(sampler.name, "sampler2D");
      };
      const addVertexColor = (attribute, sel, channel) => {
        if (color.vertexColor && color.vertexColor.name !== attribute.name) fail("two colour attributes");
        color.vertexColor ??= { name: attribute.name, type: attribute.t, channels: new Set() };
        color.vertexColor.channels.add(channel);
        used.attributes.add(attribute.name);
      };
      /* channel: "rgb" or "a"; sel: which components of n feed it. */
      const factor = (n, channel, sel, stage, depth = 0) => {
        if (depth > 64) fail("colour expression is too deep");
        if (n.k === "num") { color.scale[channel] *= n.v; return; }
        if (n.k === "op" && n.o === "*" && (n.a.k === "num" || n.b.k === "num")) {
          for (const side of [n.a, n.b])
            factor(side, channel, side.t === "float" ? "x" : sel, stage, depth + 1);
          return;
        }
        if (n.k === "ctor" && n.args.length === 1 && n.args[0].k === "num") {
          color.scale[channel] *= n.args[0].v; return;
        }
        if (uniformOnly(n)) { color[channel].push({ node: n, sel }); noteUniforms(n); return; }
        if (n.k === "op" && n.o === "*") {
          for (const side of [n.a, n.b])
            factor(side, channel, side.t === "float" ? "x" : sel, stage, depth + 1);
          return;
        }
        if (n.k === "ctor" && n.t === "vec4" && n.args.length === 2 && n.args[0].t === "vec3") {
          if (sel === "xyz") return factor(n.args[0], channel, "xyz", stage, depth + 1);
          if (sel === "w") return factor(n.args[1], channel, "x", stage, depth + 1);
        }
        if (n.k === "ctor" && n.args.length === 1 && n.args[0].t === "float")
          return factor(n.args[0], channel, "x", stage, depth + 1);
        if (n.k === "swz") {
          const s = sel === "x" ? n.s : sel === "xyz" && n.s.length === 3 ? n.s
            : sel === "w" && n.s.length === 4 ? n.s[3] : null;
          if (s === "xyz" || s === "w") return factor(n.a, channel, s, stage, depth + 1);
          if (s === "x" && n.a.t === "float") return factor(n.a, channel, "x", stage, depth + 1);
        }
        if (n.k === "merge" && n.s === "xyz") {
          if (sel === "xyz") return factor(n.v, channel, "xyz", stage, depth + 1);
          if (sel === "w") return factor(n.a, channel, "w", stage, depth + 1);
        }
        if (n.k === "call" && n.f === "texture2D" && stage === "fragment") return addTexture(n, sel);
        if (n.k === "vary" && stage === "fragment") {
          const v = vertexExpression(n);
          if (v.flags & F_LIGHT) { color.light.push({ name: n.name, node: v, channel, sel }); return; }
          return factor(v, channel, sel, "vertex", depth + 1);
        }
        if (n.k === "attr" && stage === "vertex") {
          if (n.t === "vec3" && sel !== "xyz") fail(`colour attribute ${n.name} has no alpha`);
          return addVertexColor(n, sel, channel);
        }
        if (baseSamplers && stage === "fragment") {
          /* A lit material's input map scaling a uniform factor of the
             colour (metalness * texel.b): the factor is drawn without it. */
          const extra = extraSamplers(n), white = extra.length
            && whiten(n, new Set(extra.map(([name]) => name)));
          if (white && uniformOnly(white) && (white.flags & F_UNIFORM)) {
            for (const [name] of extra) leaveOut(name, "input");
            return factor(white, channel, sel, stage, depth + 1);
          }
        }
        const shaded = perVertex(n, stage);
        if (shaded) {
          color.program[channel].push({ node: shaded, sel, text: show(n) });
          return;
        }
        fail(`colour term ${show(n).slice(0, 60)} has no GE equivalent`);
      };

      /* -- fog: mix(colour, fogColor, f(depth)) over the rgb channels -- */
      let rgbNode, alphaNode;
      const split = (n) => {
        if (n.k === "ctor" && n.t === "vec4" && n.args.length === 2 && n.args[0].t === "vec3")
          return [n.args[0], n.args[1], "x"];
        if (n.k === "merge" && n.s === "xyz") return [n.v, n.a, "w"];
        return [{ k: "swz", t: "vec3", a: n, s: "xyz", flags: n.flags }, n, "w"];
      };
      const [rgb0, alpha0, alphaSel] = split(fragColor);
      rgbNode = rgb0; alphaNode = alpha0;
      if (LIT_MODELS[shaderName]) rgbNode = withoutEnvironmentBlend(rgbNode);
      if (rgbNode.k === "call" && rgbNode.f === "mix" && rgbNode.args[2]?.t === "float") {
        const [under, fogColor, amount] = rgbNode.args;
        if (!uniformOnly(fogColor)) fail("fog colour must be uniform");
        const depthVary = (n) => n.k === "vary" && eyeDepth(vertexExpression(n));
        let fog = null;
        if (amount.k === "call" && amount.f === "smoothstep" && depthVary(amount.args[2])
            && uniformOnly(amount.args[0]) && uniformOnly(amount.args[1])) {
          fog = { kind: "linear", near: amount.args[0], far: amount.args[1] };
          warnings.push("fragment: smoothstep fog is drawn as the GE's linear fog");
        } else if (amount.k === "op" && amount.o === "-" && isNum(amount.a, 1)
            && amount.b.k === "call" && amount.b.f === "exp") {
          const e = amount.b.args[0];
          /* exp(-d*d*z*z): fogDensity squared times depth squared */
          const text = show(e);
          const density = analysis.fs.globals.has("fogDensity") ? "fogDensity" : null;
          if (density && /fogDensity/.test(text) && hasKind(e, "vary")) {
            fog = { kind: "exp2", density: { k: "unif", t: "float", name: density, flags: F_UNIFORM } };
            warnings.push("fragment: exponential fog is approximated by the GE's linear fog");
          }
        }
        if (!fog) fail("fog factor has no GE equivalent");
        fog.color = fogColor;
        for (const k of ["near", "far", "density", "color"]) if (fog[k]) noteUniforms(fog[k]);
        plan.fog = fog; rgbNode = under;
        if (fog.kind === "exp2" || fog.kind === "linear") {
          if (eyeSplit < 0) fail("fog depth does not read the eye-space position");
        }
      }

      /* -- lighting -- */
      const model = LIT_MODELS[shaderName];
      const lightCalls = [...analysis.vertex.lightCalls, ...analysis.fragment.lightCalls];
      const sum = (n, out = []) => {
        if (n.k === "op" && n.o === "+") { sum(n.a, out); sum(n.b, out); } else out.push(n);
        return out;
      };
      if (lightCalls.length || (rgbNode.flags & F_LIGHT)) {
        if (!model) fail("lighting functions outside a known lit material");
        const lighting = plan.lighting = { model, emissive: [], lights: [], ambient: null, probe: null };
        if (eyeSplit < 0) fail("lighting does not read the eye-space position");
        let materialStruct = null;
        for (const call of lightCalls) {
          for (const arg of call.args) {
            if (arg.k === "struct" && /Light$/.test(arg.t)) {
              const first = [...arg.fields.values()][0];
              const path = first?.k === "unif" && /^(\w+)\[(\d+)\]\./.exec(first.name);
              if (path) {
                const key = `${path[1]}[${path[2]}]`;
                if (!lighting.lights.some((l) => l.key === key))
                  lighting.lights.push({ key, type: arg.t, fields: Object.fromEntries(
                    [...arg.fields].map(([f, v]) => [f, v.name])) });
                noteUniforms(arg);
              }
            } else if (arg.k === "unif" && arg.name === "ambientLightColor") {
              lighting.ambient = arg.name; noteUniforms(arg);
            } else if (arg === call.probe) {
              lighting.probe = /^(\w+)\[0\]$/.exec(arg.items[0].name)?.[1] ?? null;
              noteUniforms(arg);
            } else if (arg.k === "struct" && [...arg.fields.keys()].some((f) => /diffuseColor/.test(f))
                && !(structFlags(arg) & F_LIGHT)) materialStruct ??= arg;
          }
        }
        for (const light of lighting.lights)
          if (!/^(Directional|Point|Spot|Hemisphere)Light$/.test(light.type))
            fail(`${light.type} has no GE equivalent`);
        if (lighting.lights.length > 4) {
          warnings.push(`fragment: ${lighting.lights.length} lights; the GE applies the first 4`);
          lighting.lights.length = 4;
        }
        if (lighting.lights.some((l) => l.type === "SpotLight"))
          warnings.push("fragment: spot lights are drawn as point lights on the PSP");
        if (lighting.lights.some((l) => l.type === "HemisphereLight"))
          warnings.push("fragment: hemisphere lights are drawn as ambient light on the PSP");
        rgbNode = withoutEnvironmentBlend(rgbNode);
        const field = (re) => materialStruct
          && [...materialStruct.fields].find(([f]) => re.test(f))?.[1];
        baseSamplers = directSamplers(model === "lambert"
          ? sum(rgbNode).find((term) => term.flags & F_LIGHT) : field(/diffuseColor/));
        /* The material inputs and geometry handed to the lighting helpers:
           the GE lights with uniform inputs and vertex normals, so a map
           read there is left out (the diffuse colour is mapped below). */
        const handed = new Set();
        for (const call of lightCalls) for (const arg of call.args) {
          const values = arg.k === "struct" ? [...arg.fields].filter(([f]) =>
            arg !== materialStruct || !/diffuseColor/.test(f)).map(([, v]) => v) : [arg];
          for (const value of values) {
            if (handed.has(value)) continue;
            handed.add(value);
            for (const [name, read] of extraSamplers(value))
              leaveOut(name, environmentRead(read) ? "environment"
                : addsLight(value, name) ? "light"
                : value.t === "vec3" && readsNormal(value) ? "normal" : "input");
          }
        }
        let baseFound = false;
        for (let term of sum(rgbNode)) {
          const extra = extraSamplers(term);
          if (extra.some(([, read]) => environmentRead(read))) {
            /* an environment term added to the colour */
            for (const [name, read] of extra)
              leaveOut(name, environmentRead(read) ? "environment" : "reflection");
            continue;
          }
          if (extra.length) {
            for (const [name] of extra)
              leaveOut(name, !(term.flags & F_LIGHT) ? "emissive"
                : addsLight(term, name) ? "light" : "occlusion");
            term = whiten(term, new Set(extra.map(([name]) => name)));
            /* a map that is the whole term, not a scale on it, is a colour */
            if (!(term.flags & (F_LIGHT | F_UNIFORM))) fail("unlit colour term is not uniform");
          }
          if (!(term.flags & F_LIGHT)) {
            if (!uniformOnly(term)) fail("unlit colour term is not uniform");
            lighting.emissive.push(term); noteUniforms(term); continue;
          }
          if (model === "lambert") {
            /* vLight * (k * diffuseColor) */
            if (!baseFound) { factor(term, "rgb", "xyz", "fragment"); baseFound = true; }
            continue;
          }
          if (term.k !== "opaque") fail("lit term is not one lighting function");
        }
        if (model === "lambert") {
          if (color.light.length < 1) fail("Lambert lighting varying not found");
          /* every light term must scale the same base colour: drop the
             repeated factors the other terms add */
        } else {
          if (!materialStruct) fail("lit material inputs not found");
          const diffuse = field(/diffuseColor/);
          factor(diffuse, "rgb", "xyz", "fragment");
          if (model === "phong") {
            lighting.specular = field(/specularColor/);
            lighting.shininess = field(/specularShininess/);
            if (!uniformOnly(lighting.specular) || !uniformOnly(lighting.shininess))
              fail("specular inputs are not uniform");
            noteUniforms(lighting.specular); noteUniforms(lighting.shininess);
          } else {
            lighting.roughness = analysis.fs.globals.has("roughness") ? "roughness" : null;
            lighting.metalness = analysis.fs.globals.has("metalness") ? "metalness" : null;
            for (const u of [lighting.roughness, lighting.metalness]) if (u) used.uniforms.set(u, "float");
            warnings.push("fragment: physically based shading is approximated by GE Blinn-Phong lighting");
          }
          warnings.push(`fragment: per-pixel lighting is evaluated per vertex on the PSP`);
        }
        let normal = null;
        for (const value of vout.values()) normal ??= findNormal(value);
        for (const call of analysis.vertex.lightCalls) for (const a of call.args) normal ??= findNormal(a);
        if (!normal) fail("lit shader has no normal attribute");
        plan.normal = normal.attribute; used.attributes.add(normal.attribute);
      } else {
        factor(rgbNode, "rgb", "xyz", "fragment");
      }
      factor(alphaNode, "a", alphaSel, "fragment");
      /* texture coordinates */
      if (color.texture) {
        const coord = color.texture.coord;
        const affine = (n) => {
          if (n.k === "swz" && n.s === "xy" && n.a.k === "op" && n.a.o === "*"
              && uniformOnly(n.a.a) && n.a.a.t === "mat3" && n.a.b.k === "ctor"
              && n.a.b.args.length === 2 && isNum(n.a.b.args[1], 1))
            return { transform: n.a.a, inner: n.a.b.args[0] };
          if (n.k === "swz" && n.s === "xy" && n.a.k === "op" && n.a.o === "*"
              && uniformOnly(n.a.a) && n.a.a.t === "mat3" && n.a.b.k === "ctor"
              && n.a.b.args.length === 3 && isNum(n.a.b.args[2], 1))
            return { transform: n.a.a, inner: { k: "pair", x: n.a.b.args[0], y: n.a.b.args[1] } };
          return { transform: null, inner: n };
        };
        let texcoord = null;
        if (coord.k === "vary") {
          const { transform, inner } = affine(vertexExpression(coord));
          if (inner.k !== "attr" || inner.t !== "vec2") fail("texture coordinates are not an attribute");
          texcoord = { attribute: inner.name, transform };
          used.attributes.add(inner.name);
        } else {
          const { transform, inner } = affine(coord);
          const pc = (n, s) => n.k === "swz" && n.s === s && n.a.k === "input" && n.a.name === "gl_PointCoord";
          if (inner.k === "input" && inner.name === "gl_PointCoord") texcoord = { point: true, flipY: false, transform };
          else if (inner.k === "pair" && pc(inner.x, "x") && inner.y.k === "op" && inner.y.o === "-"
              && isNum(inner.y.a, 1) && pc(inner.y.b, "y")) texcoord = { point: true, flipY: true, transform };
          else fail("texture coordinates have no GE equivalent");
        }
        if (texcoord.transform) noteUniforms(texcoord.transform);
        plan.texcoord = texcoord; plan.sampler = color.texture.sampler;
        if (color.texture.sel.size !== 2)
          warnings.push("fragment: the GE modulates colour and alpha by the texture together");
      }
      if (color.vertexColor) {
        plan.vertexColor = color.vertexColor.name;
        if (color.vertexColor.type === "vec4" && color.vertexColor.channels.size !== 2)
          fail("colour attribute alpha is applied to only one channel");
      }
      plan.colorRGB = color.rgb; plan.colorAlpha = color.a;
      plan.colorScale = [plan.lighting ? 1 : color.scale.rgb, color.scale.a];
      if (color.light.length && !plan.lighting) fail("lit varying without lighting");
      /* alpha test */
      for (const d of analysis.fragment.discards) {
        if (!d.cond || d.guards.length) fail("discard has no GE alpha-test equivalent");
        const c = d.cond;
        if (c.k === "op" && c.o === "<" && uniformOnly(c.b) && show(c.a) === show(
            alphaSel === "w" ? { k: "swz", a: alphaNode, s: "w", t: "float" } : alphaNode)) {
          plan.alphaTest = c.b; noteUniforms(c.b);
        } else fail("discard condition is not an alpha test");
      }
      /* point size */
      const pointSize = vout.get("gl_PointSize");
      if (pointSize && pointSize.k !== "undef") {
        const rewrite = (n, depth = 0) => {
          if (!n || typeof n !== "object" || depth > 100) return n;
          if (eyeDepth(n)) return { k: "depth", t: "float", flags: F_UNIFORM };
          if (!["op", "call", "ctor", "swz", "sel", "col", "merge"].includes(n.k)) return n;
          const copy = { ...n };
          for (const key of ["a", "b", "c", "v"]) if (n[key]) copy[key] = rewrite(n[key], depth + 1);
          if (n.args) copy.args = n.args.map((x) => rewrite(x, depth + 1));
          copy.flags = [copy.a, copy.b, copy.c, copy.v, ...(copy.args || [])]
            .reduce((f, x) => f | (x?.flags || 0), 0);
          return copy;
        };
        const size = rewrite(pointSize);
        if (!uniformOnly(size)) fail("point size has no GE equivalent");
        plan.pointSize = size; noteUniforms(size);
      }
      if (color.program.rgb.length || color.program.a.length) {
        const slot = (attribute) => {
          if (attribute.name === clip.attribute) return 0;
          if (attribute.t === "vec2" || attribute.t === "float") {
            if (plan.texcoord && plan.texcoord.attribute !== attribute.name)
              fail(`attribute ${attribute.name} cannot be read per vertex`);
            plan.texcoord ??= { attribute: attribute.name, transform: null };
            used.attributes.add(attribute.name);
            return 1;
          }
          if (attribute.t === "vec3") {
            if (plan.normal && plan.normal !== attribute.name)
              fail(`attribute ${attribute.name} cannot be read per vertex`);
            plan.normal = attribute.name; used.attributes.add(attribute.name);
            return 2;
          }
          fail(`attribute ${attribute.name} cannot be read per vertex`);
        };
        plan.program = compileVertexProgram(color.program, slot, noteUniforms);
        for (const text of new Set([...color.program.rgb, ...color.program.a]
          .map((term) => term.text.slice(0, 48))))
          warnings.push(`fragment: '${text}' is shaded per vertex on the PSP`);
      }
      for (const [name, roles] of leftOut) {
        /* a map that also feeds a kept term is named for that term */
        if (roles.size > 1) roles.delete("reflection");
        for (const role of roles) warnings.push(`fragment: texture '${name}' ${MAP_ROLES[role]}`);
      }
      return plan;
    };

    /* ---- per-vertex programs ------------------------------------------- */
    /* The native per-vertex interpreter (js_webgl_bridge.c, WebglProgram):
       16 vec4 registers, r0-r2 the position, texture-coordinate and normal
       attributes, r3 unused, r4-r15 temporaries. An instruction is two
       words: op | dst << 6 | a << 10 | b << 14 | c << 18 | imm << 22, then
       the a, b and c swizzles (2 bits per destination lane) and the
       destination write mask << 24. */
    const PROGRAM_STEPS = 32, PROGRAM_CONSTANTS = 15;
    const POP = { mov: 0, add: 1, sub: 2, mul: 3, div: 4, neg: 5, min: 6, max: 7,
      abs: 8, floor: 9, ceil: 10, fract: 11, sin: 12, cos: 13, sqrt: 14,
      inversesqrt: 15, exp: 16, exp2: 17, log: 18, log2: 19, pow: 20, step: 21,
      mod: 22, sign: 23, mix: 24, clamp: 25, smoothstep: 26, dot: 27,
      const: 28, matvec: 29, sel: 30, "<": 31, "<=": 32, ">": 33, ">=": 34,
      "==": 35, "!=": 36, not: 37 };
    const compileVertexProgram = (terms, slot, noteUniforms) => {
      const code = [], constants = [], constantAt = new Map();
      const refs = new Array(16).fill(0), memo = new Map(), uses = new Map();
      const XYZW = [0, 1, 2, 3], width = (t) => size(t) || 1;
      const count = (x, depth = 0) => {
        uses.set(x, (uses.get(x) || 0) + 1);
        if (uses.get(x) > 1 || uniformOnly(x) || depth > 64) return;
        for (const key of ["a", "b", "c"]) if (x[key]) count(x[key], depth + 1);
        for (const a of x.args || []) count(a, depth + 1);
      };
      const roots = [...terms.rgb, ...terms.a];
      for (const term of roots) count(term.node);
      const temp = (n) => {
        for (let r = 4; r < 16; r++) if (!refs[r]) { refs[r] = n; return r; }
        fail("per-vertex colour needs more than 12 registers");
      };
      const release = (v) => { if (v.reg >= 4 && --refs[v.reg] === 0) refs[v.reg] = 0; };
      const swz = (v) => v ? v.swz[0] | v.swz[1] << 2 | v.swz[2] << 4 | v.swz[3] << 6 : 0;
      const emit = (op, dst, mask, a, b, c, imm = 0) => {
        if (code.length >= PROGRAM_STEPS * 2)
          fail(`per-vertex colour needs more than ${PROGRAM_STEPS} steps`);
        code.push((op | dst << 6 | (a?.reg || 0) << 10 | (b?.reg || 0) << 14
          | (c?.reg || 0) << 18 | imm << 22) >>> 0,
          (swz(a) | swz(b) << 8 | swz(c) << 16 | mask << 24) >>> 0);
      };
      const broadcast = (v, n) => v.w === 1 && n > 1
        ? { reg: v.reg, swz: [v.swz[0], v.swz[0], v.swz[0], v.swz[0]], w: n } : v;
      const constant = (key, values, node) => {
        if (!constantAt.has(key)) {
          if (constants.length >= PROGRAM_CONSTANTS)
            fail(`per-vertex colour needs more than ${PROGRAM_CONSTANTS} constants`);
          constantAt.set(key, constants.length);
          constants.push(values !== undefined ? { values } : node);
          if (node?.node) noteUniforms(node.node);
        }
        return constantAt.get(key);
      };
      /* A value is { reg, swz, w }; it holds refs[reg] once per pending use. */
      const value = (x, depth = 0) => {
        if (depth > 64) fail("colour expression is too deep");
        if (memo.has(x)) return memo.get(x);
        let v;
        const finish = (reg, n, lanes = XYZW) => {
          v = { reg, swz: lanes, w: n };
          if (reg >= 4) refs[reg] += (uses.get(x) || 1) - 1;
        };
        if (x.k === "attr") {
          v = { reg: slot(x), swz: XYZW, w: width(x.t) };
        } else if (x.k === "num" || uniformOnly(x)) {
          if (/^mat/.test(x.t)) fail("a matrix is used outside a product");
          const index = x.k === "num" ? constant("#" + x.v, [x.v])
            : constant(show(x), undefined, { node: x });
          const reg = temp(1);
          emit(POP.const, reg, 15, null, null, null, index);
          finish(reg, width(x.t));
        } else if (x.k === "swz") {
          const a = value(x.a, depth + 1), s = [...x.s].map((ch) => "xyzw".indexOf(ch
            .replace(/[rs]/, "x").replace(/[gt]/, "y").replace(/[bp]/, "z").replace(/[aq]/, "w")));
          const lanes = [0, 1, 2, 3].map((k) => a.swz[s[Math.min(k, s.length - 1)]]);
          v = { reg: a.reg, swz: lanes, w: s.length };
          if (a.reg >= 4) refs[a.reg] += (uses.get(x) || 1) - 1;
          return memo.set(x, v), v;
        } else if (x.k === "ctor") {
          const parts = x.args.map((a) => value(a, depth + 1)), n = width(x.t);
          if (/^mat/.test(x.t)) fail("a matrix is built per vertex");
          if (parts.length === 1) {
            v = broadcast(parts[0], n);
            v = { ...v, w: n };
            if (v.reg >= 4) refs[v.reg] += (uses.get(x) || 1) - 1;
            return memo.set(x, v), v;
          }
          const reg = temp(1);
          let at = 0;
          for (const part of parts) {
            const lanes = [0, 0, 0, 0];
            let mask = 0;
            for (let k = 0; k < part.w && at + k < 4; k++) {
              lanes[at + k] = part.swz[k]; mask |= 1 << (at + k);
            }
            emit(POP.mov, reg, mask, { reg: part.reg, swz: lanes });
            at += part.w; release(part);
          }
          finish(reg, n);
        } else if (x.k === "op" && x.o === "*" && /^mat[34]$/.test(x.a.t)) {
          if (!uniformOnly(x.a)) fail("a per-vertex matrix product");
          const dims = x.a.t === "mat4" ? 4 : 3, key = show(x.a);
          const first = constants.length;
          if (!constantAt.has(key + "#0"))
            for (let k = 0; k < dims; k++)
              constant(key + "#" + k, undefined, { node: x.a, column: k, dims });
          const b = value(x.b, depth + 1), reg = temp(1);
          emit(POP.matvec, reg, 15, b, { reg: dims, swz: XYZW }, null,
            constantAt.get(key + "#0") ?? first);
          release(b); finish(reg, dims);
        } else if (x.k === "op" || x.k === "call" || x.k === "sel") {
          const name = x.k === "sel" ? "sel" : x.k === "op"
            ? ({ "+": "add", "-": "sub", "*": "mul", "/": "div", neg: "neg",
              "!": "not", "&&": "mul", "||": "max" }[x.o] || x.o) : x.f;
          const argsOf = x.k === "sel" ? [x.c, x.a, x.b] : x.k === "op"
            ? [x.a, x.b].filter(Boolean) : x.args;
          const n = width(x.t);
          if (name === "length" || name === "normalize") {
            const a = value(argsOf[0], depth + 1), d = temp(1);
            emit(POP.dot, d, 15, a, a, null, a.w);
            const reg = name === "length" ? d : temp(1);
            if (name === "length") emit(POP.sqrt, d, 15, { reg: d, swz: XYZW });
            else {
              emit(POP.inversesqrt, d, 15, { reg: d, swz: XYZW });
              emit(POP.mul, reg, 15, a, { reg: d, swz: XYZW });
              refs[d] = 0;
            }
            release(a); finish(reg, name === "length" ? 1 : a.w);
          } else {
            const op = POP[name];
            if (op === undefined || name === "const" || name === "matvec")
              fail(`'${name}' cannot be shaded per vertex`);
            const args = argsOf.map((a) => value(a, depth + 1));
            const wide = Math.max(...args.map((a) => a.w));
            const lanes = name === "dot" ? args : name === "sel"
              ? [broadcast(args[0], 4), ...args.slice(1).map((a) => broadcast(a, wide))]
              : args.map((a) => broadcast(a, wide));
            const reg = temp(1);
            emit(op, reg, 15, lanes[0], lanes[1], lanes[2],
              name === "dot" ? args[0].w : 0);
            for (const a of args) release(a);
            finish(reg, name === "dot" ? 1 : n);
          }
        } else fail(`'${show(x).slice(0, 40)}' cannot be shaded per vertex`);
        memo.set(x, v);
        return v;
      };
      const product = (list, pick) => {
        let total = null;
        for (const term of list) {
          const v = pick(value(term.node), term.sel);
          if (!total) { total = v; continue; }
          const reg = temp(1);
          emit(POP.mul, reg, 15, total, v); release(total); release(v);
          total = { reg, swz: XYZW, w: v.w };
        }
        return total;
      };
      const rgb = product(terms.rgb, (v, sel) => sel === "x" ? broadcast(v, 3)
        : { ...v, w: 3 });
      const alpha = product(terms.a, (v, sel) => sel === "w"
        ? broadcast({ reg: v.reg, swz: [v.swz[3], 0, 0, 0], w: 1 }, 4)
        : broadcast(v, 4));
      const out = temp(1);
      if (rgb) emit(POP.mov, out, 7, rgb);
      if (alpha) emit(POP.mov, out, 8, alpha);
      return { code: Uint32Array.from(code), constants, steps: code.length / 2,
        outputs: (rgb ? 1 : 0) | (alpha ? 2 : 0), out };
    };
    const structFlags = (s) => {
      let f = s.flags || 0;
      for (const v of s.fields.values()) f |= v.k === "struct" ? structFlags(v) : (v.flags || 0);
      return f;
    };

    /* ---- per-draw evaluation of uniform-only nodes ---- */
    const evaluate = (n, lookup, depth) => {
      const ev = (x) => evaluate(x, lookup, depth);
      const arr = (v) => typeof v === "number" ? [v] : Array.isArray(v) ? v : Array.from(v);
      const map2 = (a, b, f) => {
        a = arr(a); b = arr(b);
        const n = Math.max(a.length, b.length), out = new Array(n);
        for (let i = 0; i < n; i++) out[i] = f(a.length === 1 ? a[0] : a[i], b.length === 1 ? b[0] : b[i]);
        return n === 1 ? out[0] : out;
      };
      switch (n.k) {
        case "num": return n.v;
        case "depth": return depth;
        case "unif": return lookup(n.name, n.t);
        case "ctor": {
          const flat = n.args.flatMap((a) => arr(ev(a)));
          const want = { float: 1, vec2: 2, vec3: 3, vec4: 4, mat3: 9, mat4: 16 }[n.t] || flat.length;
          if (flat.length === 1 && want > 1) {
            if (/^mat/.test(n.t)) { const d = Math.sqrt(want); return Array.from({ length: want }, (_, i) => i % (d + 1) ? 0 : flat[0]); }
            return new Array(want).fill(flat[0]);
          }
          return want === 1 ? flat[0] : flat.slice(0, want);
        }
        case "swz": { const v = arr(ev(n.a)); const out = [...n.s].map((c) => v["xyzw".indexOf(c)]); return out.length === 1 ? out[0] : out; }
        case "col": { const m = arr(ev(n.a)), d = Math.sqrt(m.length); return m.slice(n.i * d, n.i * d + d); }
        case "merge": { const v = arr(ev(n.a)).slice(), w = arr(ev(n.v)); [...n.s].forEach((c, k) => { v["xyzw".indexOf(c)] = w[k]; }); return v; }
        case "sel": return ev(n.c) ? ev(n.a) : ev(n.b);
        case "op": {
          const a = ev(n.a);
          if (n.o === "neg") return map2(a, 0, (x) => -x);
          if (n.o === "!") return a ? 0 : 1;
          const b = ev(n.b);
          if (n.o === "*" && /^mat/.test(n.a.t) && /vec/.test(n.b.t)) {
            const d = arr(b).length, out = new Array(d).fill(0);
            for (let c = 0; c < d; c++) for (let r = 0; r < d; r++) out[r] += a[c * d + r] * b[c];
            return out;
          }
          if (n.o === "*" && /^mat/.test(n.a.t) && /^mat/.test(n.b.t)) {
            const d = Math.sqrt(a.length), out = new Array(a.length).fill(0);
            for (let c = 0; c < d; c++) for (let r = 0; r < d; r++) for (let k = 0; k < d; k++)
              out[c * d + r] += a[k * d + r] * b[c * d + k];
            return out;
          }
          const f = { "+": (x, y) => x + y, "-": (x, y) => x - y, "*": (x, y) => x * y, "/": (x, y) => x / y,
            "<": (x, y) => +(x < y), ">": (x, y) => +(x > y), "<=": (x, y) => +(x <= y), ">=": (x, y) => +(x >= y),
            "==": (x, y) => +(x === y), "!=": (x, y) => +(x !== y), "&&": (x, y) => +(x && y), "||": (x, y) => +(x || y) }[n.o];
          if (!f) throw new SemError(`operator ${n.o}`);
          if (n.o === "==" || n.o === "!=") {
            const A = arr(a), B = arr(b), same = A.length === B.length && A.every((x, i) => x === B[i]);
            return +(n.o === "==" ? same : !same);
          }
          return map2(a, b, f);
        }
        case "call": {
          const v = n.args.map(ev), M = Math;
          const one = { abs: M.abs, sqrt: M.sqrt, exp: M.exp, exp2: (x) => 2 ** x, log: M.log, log2: M.log2,
            floor: M.floor, ceil: M.ceil, sin: M.sin, cos: M.cos, tan: M.tan, sign: M.sign,
            fract: (x) => x - M.floor(x), inversesqrt: (x) => 1 / M.sqrt(x),
            radians: (x) => x * M.PI / 180, degrees: (x) => x * 180 / M.PI }[n.f];
          if (one) return map2(v[0], 0, (x) => one(x));
          const two = { pow: M.pow, min: M.min, max: M.max, mod: (x, y) => x - y * M.floor(x / y),
            step: (e, x) => +(x >= e), lessThan: (x, y) => +(x < y),
            lessThanEqual: (x, y) => +(x <= y), greaterThan: (x, y) => +(x > y),
            greaterThanEqual: (x, y) => +(x >= y), equal: (x, y) => +(x === y),
            notEqual: (x, y) => +(x !== y) }[n.f];
          if (two) return map2(v[0], v[1], two);
          if (n.f === "clamp") return map2(map2(v[0], v[1], M.max), v[2], M.min);
          if (n.f === "mix") return map2(map2(v[0], v[1], (x, y) => [x, y]), v[2], (p, t) => p[0] * (1 - t) + p[1] * t);
          if (n.f === "dot") { const a = arr(v[0]), b = arr(v[1]); return a.reduce((s, x, i) => s + x * b[i], 0); }
          if (n.f === "length") return M.hypot(...arr(v[0]));
          if (n.f === "normalize") { const a = arr(v[0]), l = M.hypot(...a) || 1; return a.map((x) => x / l); }
          if (n.f === "smoothstep") return map2(map2(v[0], v[1], (x, y) => [x, y]), v[2], (e, x) => {
            const t = M.min(M.max((x - e[0]) / (e[1] - e[0]), 0), 1); return t * t * (3 - 2 * t); });
          throw new SemError(`function ${n.f} is not evaluated per draw`);
        }
        default: throw new SemError(`node ${n.k} is not evaluated per draw`);
      }
    };

    return { preprocessShader, analyse, buildPlan, evaluate, SemError, show };
  })();
  /* END shader semantics */

  const materialTypeWidth = { float: 1, int: 1, bool: 1, vec2: 2, vec3: 3,
      vec4: 4, mat2: 4, mat3: 9, mat4: 16 },
    MATERIAL_LIGHTING = 1, MATERIAL_FOG = 2, MATERIAL_POINT_SIZE = 4,
    MATERIAL_TEXTURE_TRANSFORM = 8, MATERIAL_SPECULAR = 16,
    MATERIAL_ALPHA_TEST = 32, MATERIAL_POINT_COORD = 64,
    MATERIAL_POINT_COORD_FLIP = 128, MATERIAL_SPLIT = 256,
    MATERIAL_PROGRAM = 512,
    /* Per-vertex program: header, 32 two-word steps, 15 vec4 constants. */
    MATERIAL_PROGRAM_WORD = 128, MATERIAL_PROGRAM_CONSTANT_WORD = 194,
    MATERIAL_PROGRAM_STEP_LIMIT = 65536,
    MATERIAL_NORMAL_WORD = 58, MATERIAL_LIGHT_WORD = 64,
    /* The normal buffer's id and generation: the native lit-vertex cache. */
    MATERIAL_NORMAL_IDENTITY_WORD = 116,
    MATERIAL_LIGHT_WORDS = 13, MATERIAL_LIGHTS = 4;

  const owned = (context, value, Type) =>
    value instanceof Type && value._owner === context && !value._deleted,
    attributeRole = (attributes, source, role) => {
      const pattern = role === "position" ? /position|vertex|coord/i
        : role === "color" ? /colou?r|tint/i : /tex|uv/i;
      const candidates = attributes.filter((entry) => entry.type !== "mat4"
        && pattern.test(entry.name));
      if (role !== "position") return candidates[0] || null;
      /* An attribute gl_Position actually reads beats a name match:
         "aTexCoord" matches /coord/ but is not the position. */
      const expression = /gl_Position\s*=([^;]+)/.exec(source)?.[1] || "",
        read = attributes.filter((entry) => entry.type !== "mat4" &&
          new RegExp("\\b" + entry.name + "\\b").test(expression));
      return read.find((entry) => candidates.includes(entry)) || read[0]
        || candidates[0]
        || attributes.find((entry) => entry.type !== "mat4") || null;
    },
    translateProgram = (program) => {
      const vertex = program._shaders.find((shader) =>
          shader._type === constants.VERTEX_SHADER),
        fragment = program._shaders.find((shader) =>
          shader._type === constants.FRAGMENT_SHADER);
      if (!vertex || !fragment || !vertex._compiled || !fragment._compiled)
        return { error: "A compiled vertex and fragment shader are required" };
      const vertexText = shaderSemantics.preprocessShader(
          shaderText(vertex._source)),
        fragmentText = shaderSemantics.preprocessShader(
          shaderText(fragment._source));
      if (vertexText.error || fragmentText.error)
        return { error: "ERROR: " + (vertexText.error
          ? "vertex shader " + vertexText.error
          : "fragment shader " + fragmentText.error) };
      /* The inspector's exact result stands. Otherwise a program the
         semantic pass understands whole replaces the inspector's refusal or
         dropped terms: its warnings name approximations, not omissions. A
         lit program it cannot map is refused, because the inspector's
         fallback would draw it unlit. */
      const legacy = inspectTranslation(vertexText.text, fragmentText.text);
      if (!legacy.error && !legacy.warnings.length) return legacy;
      const semantic = semanticTranslation(vertexText, fragmentText);
      /* An interface past the WebGL limits is refused on either path. */
      if (semantic.interface) return { error: semantic.error };
      if (semantic.error)
        return semantic.lit ? { error: "ERROR: " + semantic.error } : legacy;
      return semantic;
    },
    inspectTranslation = (vs, fs) => {
      const attributeScan = declarations(vs, "attribute"),
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
          || uniformVectors(vertexUniformScan.entries,
            MAX_VERTEX_UNIFORM_VECTORS) > MAX_VERTEX_UNIFORM_VECTORS
          || uniformVectors(fragmentUniformScan.entries,
            MAX_FRAGMENT_UNIFORM_VECTORS) > MAX_FRAGMENT_UNIFORM_VECTORS
          || uniformVectors(declarations(fs, "varying").entries,
            MAX_VARYING_VECTORS) > MAX_VARYING_VECTORS
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
      const translation = { attributes, uniforms, position, color, texcoord,
          instanceMatrix, instanceColor, instanceTransform, matrices,
          sampler, uniformColor, usesTexture },
        findings = inspectProgram(vs, fs, translation);
      if (findings.errors.length) return { error: findings.errors.join("\n") };
      if (translation.usesTexture && (!sampler || !translation.texcoord))
        return { error: "Texture shaders require a sampler and texture-coordinate attribute" };
      translation.warnings = findings.warnings;
      return translation;
    },
    semanticTranslation = (vertexText, fragmentText) => {
      let analysis, plan;
      try {
        analysis = shaderSemantics.analyse(vertexText.text, fragmentText.text);
        plan = shaderSemantics.buildPlan(analysis,
          (vertexText.defines.get("SHADER_NAME")?.body || "").trim());
      } catch (error) {
        return { error: String(error?.message || error),
          lit: !!(analysis && (analysis.vertex.lightCalls.length
            || analysis.fragment.lightCalls.length)) };
      }
      const global = (name) => analysis.vs.globals.get(name)
        || analysis.fs.globals.get(name);
      const attributes = [...plan.used.attributes].map((name) =>
        ({ type: global(name)?.type, name, size: 1 }));
      if (attributes.length > 8
          || attributes.some((entry) => !/^(float|vec[234])$/.test(entry.type)))
        return { error: "Shader interface exceeds the PSP WebGL limits" };
      const uniforms = [], byName = new Map();
      for (const [name, type] of plan.used.uniforms) {
        const element = /^(\w+)\[(\d+)\]$/.exec(name),
          base = element ? element[1] : name;
        if (byName.has(base)) continue;
        const entry = { type, name: base,
          size: element ? global(base)?.count || 1 : 1 };
        byName.set(base, entry); uniforms.push(entry);
      }
      /* Varyings the fragment stage reads from the vertex stage. */
      const varyings = [...analysis.fs.globals.values()].filter((entry) =>
        entry.storage === "varying"
        && analysis.vs.globals.get(entry.name)?.storage === "varying")
        .map((entry) => ({ type: entry.type, size: entry.count || 1 }));
      if (uniformVectors(varyings, MAX_VARYING_VECTORS) > MAX_VARYING_VECTORS)
        return { error: "Shader interface exceeds the PSP WebGL limits",
          interface: true };
      /* Active uniforms only, per stage, as WebGL counts them. */
      const stage = (globals) => uniforms.filter((entry) =>
        globals.get(entry.name)?.storage === "uniform");
      if (uniformVectors(stage(analysis.vs.globals), MAX_VERTEX_UNIFORM_VECTORS)
            > MAX_VERTEX_UNIFORM_VECTORS
          || uniformVectors(stage(analysis.fs.globals),
            MAX_FRAGMENT_UNIFORM_VECTORS) > MAX_FRAGMENT_UNIFORM_VECTORS)
        return { error: "Shader interface exceeds the PSP WebGL limits",
          interface: true };
      const attribute = (name) => name
        ? attributes.find((entry) => entry.name === name) || null : null;
      return { attributes, uniforms, position: attribute(plan.position),
        color: attribute(plan.vertexColor),
        texcoord: attribute(plan.texcoord?.attribute),
        normal: attribute(plan.normal),
        instanceMatrix: null, instanceColor: null, instanceTransform: null,
        matrices: plan.matrices.map((name) => byName.get(name)),
        sampler: plan.sampler ? byName.get(plan.sampler) : null,
        uniformColor: null, constantColor: null, usesTexture: !!plan.sampler,
        material: plan,
        warnings: plan.warnings.map((text) => "WARNING: "
          + text.replace(/^(\w+): /, "$1 shader: ")) };
    },
    /* A refused or approximated program is reported on the console once,
       so an author who never reads the info log still sees it. */
    reportProgram = (program) => {
      if (!program._log || program._reported) return;
      program._reported = true;
      globalThis.console?.warn?.(
        "WebGL program " + program._id + ": " + program._log);
    };

  class WebGLRenderingContextImpl {
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
      /* The rectangles the native renderer gets: canvas units scaled to a
         clamped drawing buffer (see _deviceRect). */
      this._viewportRequest = new Int32Array(this._viewport);
      this._deviceViewport = new Int32Array(this._viewport);
      this._deviceScissor = new Int32Array(this._scissor);
      this._depthFunc = constants.LESS;
      this._blendColor = new Float32Array(4);
      this._resetBlendFactors();
      this._blendEquation = this._blendEquationAlpha = constants.FUNC_ADD;
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
        this._flushQueued = false; this._flush(true);
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
      this._blendMinmaxExtension = null;
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
      this._surfaceReady = false; this._frameOpen = false;
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
      const event = new WebGLContextEvent("webglcontextlost", {
        __proto__: null, cancelable: true, statusMessage: reason || "" });
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
      this._blendColor.fill(0);
      this._resetBlendFactors();
      this._blendEquation = this._blendEquationAlpha = constants.FUNC_ADD;
      this._cullFace = constants.BACK; this._frontFace = constants.CCW;
      this._rasterStateKey = this._computeRasterStateKey();
      this._viewportRevision = 1; this._scissorRevision = 1;
      this._lineWidth = 1; this._flipY = false; this._premultiply = false;
      this._unpackAlignment = 4; this._packAlignment = 4;
      this._surfaceReady = false; this._frameOpen = false;
      this._resize();
      if (!this._lost)
        this.canvas.dispatchEvent(
          new WebGLContextEvent("webglcontextrestored"));
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
      this._flushHasMaterial = false;
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
      command.texture = null; command.material = null;
      if (command._normalStorage) command._normalStorage.data = null;
      command.normal = null;
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
        command.scissor.set(this._deviceScissor);
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
      wireI32.set(this._deviceScissor, base + 40);
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
      wireU32[base + 27] = draw.blend;
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
      wireU32[base + 95] = draw.material ? commandAt + 1 : 0;
      if (draw.material)
        this._writeMaterial(commandAt, draw.material, null, sources,
          draw.normal);
    }
    /* The blend factor words are compared by _drawTemplateStateMatches,
       since a constant factor's word follows blendColor(). */
    _computeRasterStateKey() {
      const cullKey = this._cullFace === constants.FRONT ? 0
        : this._cullFace === constants.BACK ? 1 : 2;
      return ((this._enabledMask & (CAP_BLEND | CAP_CULL_FACE
          | CAP_DEPTH_TEST | CAP_SCISSOR_TEST))
        | ((this._depthFunc - constants.NEVER) << 5)
        | (cullKey << 12)
        | ((this._frontFace - constants.CW) << 14)
        | (BLEND_WIRE[this._blendEquation] << 15)) >>> 0;
    }
    _drawTemplateStateMatches(plan) {
      return !!plan.commandTemplateU32
        && plan.commandTemplateRasterKey === this._rasterStateKey
        && plan.commandTemplateI32[31] === this._blendSrcWire
        && plan.commandTemplateI32[32] === this._blendDstWire
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
      wireU32[27] = (this._enabledMask & CAP_BLEND) !== 0
        ? BLEND_WIRE[this._blendEquation] : 0;
      wireU32[28] = (this._enabledMask & CAP_DEPTH_TEST) !== 0 ? 1 : 0;
      wireU32[29] = (this._enabledMask & CAP_CULL_FACE) !== 0 ? 1 : 0;
      wireI32[30] = this._depthFunc;
      wireI32[31] = this._blendSrcWire; wireI32[32] = this._blendDstWire;
      wireI32[33] = this._cullFace; wireI32[34] = this._frontFace;
      wireI32.set(this._deviceViewport, 35);
      wireU32[39] = (this._enabledMask & CAP_SCISSOR_TEST) !== 0 ? 1 : 0;
      wireI32.set(this._deviceScissor, 40);
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
                 candidateSources, vertices, drawPlan, material = null,
                 normal = null) {
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
        if (material) {
          command.material = command._materialStorage
            ||= new Float32Array(MATERIAL_WORDS);
          command.material.set(material);
          command.normal = this._snapshotLiveAttribute(
            command._normalStorage ||= {}, normal);
        } else command.material = null;
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
        command.blend = (this._enabledMask & CAP_BLEND) !== 0
          ? BLEND_WIRE[this._blendEquation] : 0;
        command.depth = (this._enabledMask & CAP_DEPTH_TEST) !== 0;
        command.cull = (this._enabledMask & CAP_CULL_FACE) !== 0;
        command.depthFunc = this._depthFunc;
        command.blendSrc = this._blendSrcWire;
        command.blendDst = this._blendDstWire;
        command.cullFace = this._cullFace; command.frontFace = this._frontFace;
        command.viewport.set(this._deviceViewport);
        command.scissorEnabled =
          (this._enabledMask & CAP_SCISSOR_TEST) !== 0;
        command.scissor.set(this._deviceScissor);
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
      wireU32[base + 95] = material ? commandAt + 1 : 0;
      if (material) this._writeMaterial(commandAt, material, normal, sources);
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
        if (textureSnapshot !== null || material
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
    /* frameEnd: this batch ends the task's frame, which painters may then
       show; any earlier flush leaves it open (images_canvas_frame_hold). */
    _flushCurrent(frameEnd) {
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
      let materialWire = null;
      if (this._flushHasMaterial) {
        materialWire = this._materialWireU32;
        materialWire[0] = MATERIAL_WIRE_MAGIC; materialWire[1] = WIRE_VERSION;
        materialWire[2] = MATERIAL_WORDS; materialWire[3] = this._commands.length;
      }
      const outcome = __tilefinchWebGLRender(
        canvas.__handle, width, height, wireU32, sources,
        this._textureWireU32, this._attributes.antialias,
        this._attributes.preserveDrawingBuffer, materialWire, !frameEnd,
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
      this._frameOpen = !frameEnd;
      return FLUSH_OK;
    }
    /* frameEnd: the end of a task (the flush microtask), where the drawing
       buffer holds a complete frame. */
    _flush(frameEnd = false) {
      if (!this._commands.length && this._tailCommands.length)
        this._promoteTailQueue();
      while (this._commands.length) {
        const outcome = this._flushCurrent(
          frameEnd && !this._tailCommands.length);
        if (outcome !== FLUSH_OK) return outcome;
        if (this._tailCommands.length) this._promoteTailQueue();
      }
      if (frameEnd && this._frameOpen) {
        this._frameOpen = false;
        if (this.canvas) publishNativeWebGLFrame(this.canvas.__handle);
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
    /* A canvas larger than the PSP surface gets a proportionally smaller
       drawing buffer, while pages keep sizing the viewport and scissor in
       canvas units (three.js sets the viewport to the size it gave the
       canvas). Scale such rectangles to the drawing buffer so the frame
       keeps its layout; the native renderer refuses a viewport wider or
       taller than the surface, so the viewport is also clamped to it. */
    _deviceRect(rect, out, viewport) {
      const canvas = this.canvas,
        canvasWidth = canvas ? requestedCanvasSize(canvas, "width", 300) : 0,
        canvasHeight = canvas ? requestedCanvasSize(canvas, "height", 150) : 0,
        bufferWidth = this.drawingBufferWidth, bufferHeight = this.drawingBufferHeight,
        sx = canvasWidth > bufferWidth ? bufferWidth / canvasWidth : 1,
        sy = canvasHeight > bufferHeight ? bufferHeight / canvasHeight : 1;
      if (sx === 1 && sy === 1) out.set(rect);
      else {
        const x0 = Math.round(rect[0] * sx), y0 = Math.round(rect[1] * sy);
        out[0] = x0; out[1] = y0;
        out[2] = Math.round((rect[0] + rect[2]) * sx) - x0;
        out[3] = Math.round((rect[1] + rect[3]) * sy) - y0;
      }
      if (viewport) {
        out[2] = Math.min(out[2], bufferWidth);
        out[3] = Math.min(out[3], bufferHeight);
      }
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
      this._viewportRequest.set(this._viewport);
      this._deviceViewport.set(this._viewport);
      this._deviceScissor.set(this._viewport);
      this._viewportRevision++; this._scissorRevision++;
      this._surfaceReady = false; this._frameOpen = false;
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
      const buffer = make.WebGLBuffer(this, this._nextId++);
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
      const shader = make.WebGLShader(this, this._nextId++, type);
      this._shaderCount++; this._resources.add(shader); return shader;
    }
    shaderSource(shader, source) {
      if (!owned(this, shader, WebGLShader)) { this._setError(constants.INVALID_OPERATION); return; }
      source = String(source);
      shader._source = source.length <= MAX_SHADER_BYTES ? source : "";
      shader._compiled = false;
      shader._log = source.length <= MAX_SHADER_BYTES ? "" : "Shader source exceeds 64 KiB";
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
      const program = make.WebGLProgram(this, this._nextId++);
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
        diagnostics.shaderRefusals++; reportProgram(program); return;
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
        diagnostics.shaderRefusals++; reportProgram(program);
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
      translation.normalLocation = translation.normal
        ? program._attributes.find((entry) =>
          entry.name === translation.normal.name)?.location ?? -1
        : -1;
      if (translation.material && !this._materialWireU32) {
        const words = WIRE_HEADER_WORDS + MAX_DRAWS * MATERIAL_WORDS,
          buffer = new ArrayBuffer(words * 4),
          scratch = new ArrayBuffer(MATERIAL_WORDS * 4);
        this._materialWireU32 = new Uint32Array(buffer);
        this._materialWireI32 = new Int32Array(buffer);
        this._materialWireF32 = new Float32Array(buffer);
        this._materialScratchF32 = new Float32Array(scratch);
        this._materialScratchU32 = new Uint32Array(scratch);
        this._materialColorScratch = new Float32Array(4);
        this._materialMatrixScratch = new Float32Array(32);
      }
      program._translation = translation; program._linked = true;
      program._materialCache = null; program._materialInputs = null;
      program._materialInputNames = null; program._materialInputIndex = null;
      if (translation.material) {
        const plan = translation.material,
          chained = new Set([...plan.modelView, ...plan.projection]);
        /* The position matrices are copied into every draw's record, so a
           change to one keeps the record, unless one of the record's own
           terms (colour, lighting, fog, point size, texture transform,
           alpha test, vertex program) reads it too. Eye depth is already a
           depth term in those, so modelViewMatrix * position there does
           not count. */
        const read = new Set(), seen = new Set();
        const visit = (node, depth) => {
          if (!node || typeof node !== "object" || ArrayBuffer.isView(node)
              || depth > 400 || seen.has(node)) return;
          seen.add(node);
          if (node.k === "unif" && typeof node.name === "string") {
            read.add(node.name); return;
          }
          const values = node instanceof Map ? node.values()
            : Array.isArray(node) ? node : Object.values(node);
          for (const value of values) visit(value, depth + 1);
        };
        for (const [key, value] of Object.entries(plan))
          if (key !== "used" && key !== "warnings") visit(value, 0);
        program._materialInputs = new Set();
        for (const name of plan.used.uniforms.keys())
          if (!chained.has(name) || read.has(name))
            program._materialInputs.add(name)
              .add(name.replace(/\[\d+\]$/, ""));
        /* The retained values a record is keyed on: those of the uniforms
           whose setters invalidate it. */
        /* Smallest first, so a compare that fails usually fails early. */
        program._materialInputNames = program._uniforms
          .filter((uniform) => program._materialInputs.has(uniform.name))
          .map((uniform, at) => [(materialTypeWidth[uniform.type] || 1)
            * uniform.size, at, uniform.name])
          .sort((a, b) => a[0] - b[0] || a[1] - b[1])
          .map((entry) => entry[2]);
        program._materialInputIndex = new Map(program._materialInputNames
          .map((name, index) => [name, index]));
        program._materialCache = { valid: false, current: null,
          records: [], hint: -1, misses: 0, seed: 0x9e3779b9 };
      }
      program._log = translation.warnings.join("\n");
      if (program._log) { diagnostics.shaderWarnings++; reportProgram(program); }
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
        else if (uniform.type === "vec4" && uniform.size === 1) program._uniformValues.set(uniform.name, new Float32Array([1, 1, 1, 1]));
        else if (translation.material && uniform.type !== "sampler2D") {
          /* WebGL's initial uniform values: zero, and identity matrices. */
          const width = materialTypeWidth[uniform.type] || 1;
          if (width === 1 && uniform.size === 1)
            program._uniformValues.set(uniform.name, 0);
          else {
            const value = new Float32Array(width * uniform.size);
            if (uniform.type === "mat3")
              for (let at = 0; at < uniform.size; at++)
                value[at * 9] = value[at * 9 + 4] = value[at * 9 + 8] = 1;
            program._uniformValues.set(uniform.name, value);
          }
        }
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
      name = String(name);
      const element = /^(.*)\[(\d+)\]$/.exec(name),
        exact = program._uniforms.find((candidate) => candidate.name === name);
      if (exact || !element) return exact ? make.WebGLUniformLocation(program, exact) : null;
      const entry = program._uniforms.find((candidate) =>
          candidate.name === element[1]),
        index = Number(element[2]);
      return entry && index < entry.size
        ? make.WebGLUniformLocation(program, entry, index) : null;
    }
    getActiveAttrib(program, index) {
      const entry = owned(this, program, WebGLProgram) ? program._attributes[index] : null;
      return entry ? make.WebGLActiveInfo(entry.size, glTypeFor(entry.type), entry.name) : null;
    }
    getActiveUniform(program, index) {
      const entry = owned(this, program, WebGLProgram) ? program._uniforms[index] : null;
      return entry ? make.WebGLActiveInfo(entry.size, glTypeFor(entry.type),
        entry.size > 1 ? entry.name + "[0]" : entry.name) : null;
    }
    /* Whether a set of this uniform should compare its value first: it is
       an input of the program's current, valid material record. */
    _watchesMaterialInput(program, name) {
      return program._materialCache?.valid === true
        && program._materialInputs.has(name);
    }
    _touchProgramUniformState(program, name, unchanged = false) {
      program._uniformStateRevision =
        (program._uniformStateRevision + 1) >>> 0;
      if (program._uniformStateRevision === 0)
        program._uniformStateRevision = 1;
      /* A material's per-draw record is reused until a uniform it evaluates
         is set to another value (the next draw then looks for a kept record
         of the values); position matrices are recombined on every draw. */
      const cache = program._materialCache;
      if (!unchanged && cache && (name === undefined
          || program._materialInputs.has(name))) {
        if (cache.valid || cache.hint < 0)
          cache.hint = program._materialInputIndex.get(name) ?? -1;
        cache.valid = false;
      }
    }
    _uniform(location, value, acceptedType) {
      if (location === null || this._lost) return;
      if (!(location instanceof WebGLUniformLocation)
          || location._program !== this._program
          || location._type !== acceptedType) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      const values = location._program._uniformValues,
        unchanged = this._watchesMaterialInput(location._program,
          location._name) && sameMaterialValue(values.get(location._name), value);
      values.set(location._name, value);
      this._touchProgramUniformState(location._program, location._name,
        unchanged);
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
      let retained = location._program._uniformValues.get(location._name),
        unchanged = false;
      if (!(retained instanceof Float32Array) || retained.length !== count) {
        retained = new Float32Array(count);
        location._program._uniformValues.set(location._name, retained);
      } else if (this._watchesMaterialInput(location._program, location._name))
        unchanged = sameMaterialValue(retained[0], Math.fround(x))
          && (count < 2 || sameMaterialValue(retained[1], Math.fround(y)))
          && (count < 3 || sameMaterialValue(retained[2], Math.fround(z)))
          && (count < 4 || sameMaterialValue(retained[3], Math.fround(w)));
      retained[0] = x;
      if (count > 1) retained[1] = y;
      if (count > 2) retained[2] = z;
      if (count > 3) retained[3] = w;
      this._touchProgramUniformState(location._program, location._name,
        unchanged);
    }
    _uniformFloatArray(location, value, count, acceptedType) {
      if (location === null || this._lost) return;
      if (location instanceof WebGLUniformLocation
          && (location._size > 1 || location._offset))
        return this._uniformArray(location, value, count, acceptedType);
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
      let retained = location._program._uniformValues.get(location._name),
        unchanged = false;
      if (!(retained instanceof Float32Array) || retained.length !== count) {
        retained = new Float32Array(count);
        location._program._uniformValues.set(location._name, retained);
      } else if (this._watchesMaterialInput(location._program, location._name)) {
        unchanged = true;
        for (let index = 0; index < count && unchanged; index++)
          unchanged = sameMaterialValue(retained[index],
            Math.fround(Number(value[index])));
      }
      if (nativeCopy) retained.set(value);
      else for (let index = 0; index < count; index++)
        retained[index] = Number(value[index]);
      this._touchProgramUniformState(location._program, location._name,
        unchanged);
      if (acceptedType === "mat4") location._program._matrixRevision++;
    }
    _uniformArray(location, value, count, acceptedType) {
      if (location._program !== this._program
          || location._type !== acceptedType) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      if ((!ArrayBuffer.isView(value) && !Array.isArray(value))
          || value.length < count || value.length % count) {
        this._setError(constants.INVALID_VALUE); return;
      }
      const length = Math.min(value.length,
        (location._size - location._offset) * count);
      for (let index = 0; index < length; index++)
        if (!Number.isFinite(Number(value[index]))) {
          this._setError(constants.INVALID_VALUE); return;
        }
      let retained = location._program._uniformValues.get(location._name),
        unchanged = false;
      if (!(retained instanceof Float32Array)
          || retained.length !== count * location._size) {
        retained = new Float32Array(count * location._size);
        location._program._uniformValues.set(location._name, retained);
      } else unchanged = this._watchesMaterialInput(location._program,
        location._name);
      const base = location._offset * count;
      if (unchanged)
        for (let index = 0; index < length; index++) {
          const previous = retained[base + index];
          retained[base + index] = Number(value[index]);
          if (unchanged)
            unchanged = sameMaterialValue(previous, retained[base + index]);
        }
      else for (let index = 0; index < length; index++)
        retained[base + index] = Number(value[index]);
      this._touchProgramUniformState(location._program, location._name,
        unchanged);
    }
    uniform1i(location, value) {
      const type = location instanceof WebGLUniformLocation
        && (location._type === "int" || location._type === "bool")
        ? location._type : "sampler2D";
      this._uniform(location, toWebGLInt32(value), type);
    }
    uniform1iv(location, value) {
      if (location instanceof WebGLUniformLocation && location._size > 1) {
        this._uniformArray(location, value, 1, location._type); return;
      }
      if ((!ArrayBuffer.isView(value) && !Array.isArray(value)) || !value.length) {
        this._setError(constants.INVALID_VALUE); return;
      }
      this.uniform1i(location, value[0]);
    }
    uniform2i(location, x, y) { this._uniformScalars(location, "ivec2", 2, x, y, 0, 0); }
    uniform3i(location, x, y, z) { this._uniformScalars(location, "ivec3", 3, x, y, z, 0); }
    uniform4i(location, x, y, z, w) { this._uniformScalars(location, "ivec4", 4, x, y, z, w); }
    uniform2iv(location, value) { this._uniformFloatArray(location, value, 2, "ivec2"); }
    uniform3iv(location, value) { this._uniformFloatArray(location, value, 3, "ivec3"); }
    uniform4iv(location, value) { this._uniformFloatArray(location, value, 4, "ivec4"); }
    uniform1f(location, value) {
      const v = Number(value);
      if (!Number.isFinite(v)) { this._setError(constants.INVALID_VALUE); return; }
      this._uniform(location, v, location instanceof WebGLUniformLocation
        && location._type === "bool" ? "bool" : "float");
    }
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
    uniformMatrix3fv(location, transpose, value) {
      if (transpose) { this._setError(constants.INVALID_VALUE); return; }
      this._uniformFloatArray(location, value, 9, "mat3");
    }
    uniformMatrix2fv(location, transpose, value) {
      if (transpose) { this._setError(constants.INVALID_VALUE); return; }
      this._uniformFloatArray(location, value, 4, "mat2");
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
    vertexAttrib1fv(index, v) { this.vertexAttrib4f(index, v?.[0], 0, 0, 1); }
    vertexAttrib2fv(index, v) { this.vertexAttrib4f(index, v?.[0], v?.[1], 0, 1); }
    vertexAttrib3fv(index, v) { this.vertexAttrib4f(index, v?.[0], v?.[1], v?.[2], 1); }
    vertexAttrib4fv(index, v) { this.vertexAttrib4f(index, v?.[0], v?.[1], v?.[2], v?.[3]); }
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
      if (parameter === VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE)
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
      const texture = make.WebGLTexture(this, this._nextId++);
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
        /* The GE path samples level 0 only, so a mipmapped filter
           minifies with its level-0 filter. */
        if (value === constants.NEAREST_MIPMAP_NEAREST
            || value === constants.NEAREST_MIPMAP_LINEAR) value = constants.NEAREST;
        else if (value === constants.LINEAR_MIPMAP_NEAREST
            || value === constants.LINEAR_MIPMAP_LINEAR) value = constants.LINEAR;
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
    /* Mipmaps are not built (level 0 only, see texParameteri). */
    generateMipmap(target) {
      if (target !== constants.TEXTURE_2D) this._setError(constants.INVALID_ENUM);
      else if (!this._texture?._pixels) this._setError(constants.INVALID_OPERATION);
    }
    clearColor(r, g, b, a) {
      r = Number(r); g = Number(g); b = Number(b); a = Number(a);
      if (!Number.isFinite(r) || !Number.isFinite(g)
          || !Number.isFinite(b) || !Number.isFinite(a)) {
        this._setError(constants.INVALID_VALUE); return;
      }
      /* Compare in the stored Float32 precision: a double such as .012 is
         never equal to its float, so a game re-setting an unchanged color
         every frame would otherwise invalidate its retained frame list. */
      r = Math.fround(Math.max(0, Math.min(1, r)));
      g = Math.fround(Math.max(0, Math.min(1, g)));
      b = Math.fround(Math.max(0, Math.min(1, b)));
      a = Math.fround(Math.max(0, Math.min(1, a)));
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
    _resetBlendFactors() {
      this._blendSrc = this._blendSrcAlpha = constants.ONE;
      this._blendDst = this._blendDstAlpha = constants.ZERO;
      this._blendSrcWire = constants.ONE; this._blendDstWire = constants.ZERO;
    }
    /* The factor word a draw carries (BLEND_FACTORS). The drawing buffer is
       opaque, so its alpha is 1: DST_ALPHA weighs as ONE, and
       ONE_MINUS_DST_ALPHA and SRC_ALPHA_SATURATE (min(As, 1 - Ad) for the
       colour channels) as ZERO. A constant factor is blendColor(), or one
       minus it, rounded to a byte per channel: a GE fixed colour. */
    _blendFactorWire(factor) {
      if (factor === constants.DST_ALPHA) return constants.ONE;
      if (factor === constants.ONE_MINUS_DST_ALPHA
          || factor === constants.SRC_ALPHA_SATURATE) return constants.ZERO;
      if (factor < constants.CONSTANT_COLOR) return factor;
      const color = this._blendColor,
        inverse = factor === constants.ONE_MINUS_CONSTANT_COLOR
          || factor === constants.ONE_MINUS_CONSTANT_ALPHA;
      let word = BLEND_FIXED;
      for (let k = 0; k < 3; k++) {
        const value = factor >= constants.CONSTANT_ALPHA ? color[3] : color[k];
        word |= Math.round((inverse ? 1 - value : value) * 255) << (8 * k);
      }
      return word;
    }
    _updateBlendWire() {
      const source = this._blendFactorWire(this._blendSrc),
        destination = this._blendFactorWire(this._blendDst);
      if (source === this._blendSrcWire && destination === this._blendDstWire)
        return;
      this._blendSrcWire = source; this._blendDstWire = destination;
      this._touchRepeatedCommandState();
    }
    blendFunc(source, destination) {
      this.blendFuncSeparate(source, destination, source, destination);
    }
    blendFuncSeparate(sourceRGB, destinationRGB, sourceAlpha,
                      destinationAlpha) {
      /* SRC_ALPHA_SATURATE is a source factor only, as in WebGL 1. The
         GE's source operand weighs by the drawing buffer's colour and its
         destination operand by the fragment's (pspgu.h GU_OTHER_COLOR);
         neither squares a colour, so SRC_COLOR and ONE_MINUS_SRC_COLOR
         are refused as source colour factors, and DST_COLOR and
         ONE_MINUS_DST_COLOR as destination ones. */
      const valid = (factor, source) => BLEND_FACTORS.includes(factor)
        && (source || factor !== constants.SRC_ALPHA_SATURATE);
      if (!valid(sourceRGB, true) || !valid(destinationRGB, false)
          || !valid(sourceAlpha, true) || !valid(destinationAlpha, false)
          || sourceRGB === constants.SRC_COLOR
          || sourceRGB === constants.ONE_MINUS_SRC_COLOR
          || destinationRGB === constants.DST_COLOR
          || destinationRGB === constants.ONE_MINUS_DST_COLOR) {
        this._setError(constants.INVALID_ENUM); return;
      }
      /* WebGL refuses a constant colour factor with a constant alpha one. */
      const constant = (factor) => factor < constants.CONSTANT_COLOR ? 0
        : factor < constants.CONSTANT_ALPHA ? 1 : 2;
      if (constant(sourceRGB) && constant(destinationRGB)
          && constant(sourceRGB) !== constant(destinationRGB)) {
        this._setError(constants.INVALID_OPERATION); return;
      }
      /* The PSP GE has one factor pair for all four channels. The drawing
         buffer is opaque (alpha: false), so the alpha factors never reach
         the screen: they are kept and reported, and the colour factors
         apply to all four channels, which keeps the common (SRC_ALPHA,
         ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA) transparency shape
         drawing as WebGL does. */
      this._blendSrcAlpha = sourceAlpha; this._blendDstAlpha = destinationAlpha;
      this._blendSrc = sourceRGB; this._blendDst = destinationRGB;
      this._updateBlendWire();
    }
    _blendEquationValid(mode) {
      return mode === constants.FUNC_ADD || mode === constants.FUNC_SUBTRACT
        || mode === constants.FUNC_REVERSE_SUBTRACT
        || (this._blendMinmaxExtension !== null
          && (mode === MIN_EXT || mode === MAX_EXT));
    }
    blendEquation(mode) { this.blendEquationSeparate(mode, mode); }
    blendEquationSeparate(rgb, alpha) {
      if (!this._blendEquationValid(rgb) || !this._blendEquationValid(alpha)) {
        this._setError(constants.INVALID_ENUM); return;
      }
      /* The GE has one blend equation, and the opaque drawing buffer never
         shows alpha, so the RGB equation applies to all four channels.
         FUNC_SUBTRACT and FUNC_REVERSE_SUBTRACT use the blend factors;
         MIN_EXT and MAX_EXT ignore them, as in WebGL. */
      this._blendEquationAlpha = alpha;
      if (this._blendEquation !== rgb) {
        this._blendEquation = rgb;
        this._rasterStateKey = this._computeRasterStateKey();
        this._touchRepeatedCommandState();
      }
    }
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
      const request = this._viewportRequest;
      if (request[0] !== x || request[1] !== y
          || request[2] !== width || request[3] !== height) {
        request[0] = x; request[1] = y; request[2] = width; request[3] = height;
        this._viewport[0] = x; this._viewport[1] = y;
        this._viewport[2] = Math.min(MAX_DRAWING_WIDTH, width);
        this._viewport[3] = Math.min(MAX_DRAWING_HEIGHT, height);
        this._deviceRect(request, this._deviceViewport, true);
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
        this._deviceRect(this._scissor, this._deviceScissor, false);
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
    /* The context has no stencil buffer (stencil: false), so WebGL's
       stencil state is retained and the stencil test always passes. */
    stencilMask(mask) { this._stencilMask = toWebGLInt32(mask); }
    stencilMaskSeparate(face, mask) { this._stencilMask = toWebGLInt32(mask); }
    stencilFunc() {} stencilFuncSeparate() {} stencilOp() {} stencilOpSeparate() {}
    clearStencil(value) { this._clearStencil = toWebGLInt32(value); }
    /* Clamped to [0, 1], as in WebGL 1; constant factors read it. */
    blendColor(red, green, blue, alpha) {
      const values = [red, green, blue, alpha];
      for (let k = 0; k < 4; k++) {
        const value = Number(values[k]);
        this._blendColor[k] = value > 0 ? (value < 1 ? value : 1) : 0;
      }
      this._updateBlendWire();
    }
    depthRange(near, far) {
      near = Math.min(1, Math.max(0, Number(near)));
      far = Math.min(1, Math.max(0, Number(far)));
      if (near > far) { this._setError(constants.INVALID_OPERATION); return; }
      /* The GE depth range is fixed at [0, 1]. */
      if (near !== 0 || far !== 1) this._setError(constants.INVALID_OPERATION);
    }
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
      /* The native product returns false for a non-finite result. */
      if (!nativeCombined)
        for (let component = 0; component < matrix.length; component++)
          if (!Number.isFinite(matrix[component])) return false;
      program._combinedMatrixRevision = program._matrixRevision;
      return true;
    }
    /* Evaluates the program's uniform-only terms for this draw: the colour
       (into _materialColorScratch) and the GE material record. */
    _prepareMaterial(program, plan) {
      const cache = program._materialCache;
      let kept = cache?.valid ? cache.current : null;
      /* Once MATERIAL_MISS_STREAK more draws built a record than two per
         reuse (an input that changes every frame, or far more value sets
         than records), a draw looks and keeps only at miss counts 8, 16
         and 32 and then at every 64th: a look costs about 0.7 ms on the
         PSP, enough to push a frame near the 60 Hz edge past it. */
      const misses = cache?.misses ?? 0,
        looks = cache && !kept && (misses < MATERIAL_MISS_STREAK
          || (misses & (misses - 1)) === 0 || (misses & 63) === 0);
      if (looks) {
        /* An input was set to another value since the record was built.
           A record is kept for each of the last few input value sets and
           reused while the inputs hold exactly its values again: materials
           of one type share a program and set their own colours before
           each draw. The input whose change dropped the current record
           rules most records out first. */
        const hint = cache.hint, records = cache.records,
          hinted = hint < 0 ? undefined
            : program._uniformValues.get(program._materialInputNames[hint]);
        for (let index = 0; index < records.length; index++) {
          const entry = records[index];
          if ((hint < 0 || materialKeyPartMatches(entry.key,
                entry.offsets[hint], hinted))
              && this._materialKeyMatches(program, entry.key)) {
            kept = entry; break;
          }
        }
        if (kept) { cache.current = kept; cache.valid = true; cache.hint = -1; }
      }
      if (kept) {
        /* Two draws of credit per reuse, so a program that finds a kept
           record at least a third of the time keeps looking. */
        cache.misses = Math.max(0, cache.misses - 2);
        diagnostics.materialRecordHits++;
        this._materialScratchU32.set(kept.words);
        this._materialColorScratch.set(kept.color);
        if (plan.projection.length
            && (!this._materialChain(program, plan.modelView, 2)
              || !this._materialChain(program, plan.projection, 18)))
          return null;
        return this._materialScratchF32;
      }
      diagnostics.materialRecordBuilds++;
      const record = this._buildMaterial(program, plan);
      if (!record || !cache) return record;
      /* A miss while records are still free is the program filling them,
         not a sign that looking is wasted. */
      if (!looks || cache.records.length === MATERIAL_KEPT_RECORDS)
        cache.misses = Math.min(cache.misses + 1, 0x7fffffff);
      if (!looks) return record;
      /* A full program replaces a record picked at random (a fixed
         sequence), so more value sets than records drawn in turn still
         find some of theirs. */
      const records = cache.records;
      let entry = null;
      if (records.length === MATERIAL_KEPT_RECORDS) {
        let seed = cache.seed;
        seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
        cache.seed = seed >>> 0 || 1;
        entry = records[cache.seed % MATERIAL_KEPT_RECORDS];
      }
      /* False only for a retained value that is neither a number nor a
         Float32Array, which the setters never store: such a record is not
         kept and is built again on every draw. */
      const slot = entry ?? { key: null, offsets: null,
        words: new Uint32Array(MATERIAL_WORDS), color: new Float32Array(4) };
      if (this._materialKey(program, slot)) {
        if (!entry) records.push(slot);
        slot.words.set(this._materialScratchU32);
        slot.color.set(this._materialColorScratch);
        cache.current = slot; cache.valid = true; cache.hint = -1;
      }
      return record;
    }
    /* Writes into entry.key a copy of the program's record inputs: per
       uniform its length (-1 for a number) and then its values, and into
       entry.offsets where each uniform starts. False, with the entry
       untouched, when a value is neither. */
    _materialKey(program, entry) {
      const values = program._uniformValues,
        names = program._materialInputNames;
      let length = 0;
      for (let index = 0; index < names.length; index++) {
        const value = values.get(names[index]);
        if (value instanceof Float32Array) length += value.length + 1;
        else if (typeof value === "number") length += 2;
        else return false;
      }
      if (entry.key?.length !== length) entry.key = new Float64Array(length);
      entry.offsets ??= new Int32Array(names.length);
      const key = entry.key, offsets = entry.offsets;
      let at = 0;
      for (let index = 0; index < names.length; index++) {
        const value = values.get(names[index]);
        offsets[index] = at;
        if (typeof value === "number") {
          key[at++] = -1; key[at++] = value;
        } else {
          key[at++] = value.length; key.set(value, at); at += value.length;
        }
      }
      return true;
    }
    /* Whether the inputs hold exactly the key's values; 0 and -0 differ. */
    _materialKeyMatches(program, key) {
      const values = program._uniformValues,
        names = program._materialInputNames;
      let at = 0;
      for (let index = 0; index < names.length; index++) {
        const value = values.get(names[index]);
        if (value instanceof Float32Array) {
          const length = value.length;
          if (key[at++] !== length) return false;
          for (let k = 0; k < length; k++) {
            const a = key[at++], b = value[k];
            if (a !== b || (a === 0 && 1 / a !== 1 / b)) return false;
          }
        } else if (typeof value === "number") {
          if (key[at++] !== -1) return false;
          const a = key[at++];
          if (a !== value || (a === 0 && 1 / a !== 1 / value)) return false;
        } else return false;
      }
      return at === key.length;
    }
    /* Writes the product of the named mat4 uniforms into the record; false
       when it is not finite. Uniform setters admit finite values only, so
       a single matrix (three.js's modelViewMatrix, projectionMatrix) is
       copied unchecked. */
    _materialChain(program, names, at) {
      const values = program._uniformValues;
      if (names.length === 1) {
        const value = values.get(names[0]);
        if (!(value instanceof Float32Array) || value.length !== 16)
          return false;
        this._materialScratchF32.set(value, at);
        return true;
      }
      const output = this._materialMatrixScratch.subarray(16, 32),
        scratch = this._materialMatrixScratch;
      output.fill(0); output[0] = output[5] = output[10] = output[15] = 1;
      for (const name of names) {
        multiplyMatrix4Into(scratch, output, values.get(name));
        output.set(scratch.subarray(0, 16));
      }
      for (let k = 0; k < 16; k++)
        if (!Number.isFinite(output[k])) return false;
      this._materialScratchF32.set(output, at);
      return true;
    }
    _buildMaterial(program, plan) {
      const values = program._uniformValues, evaluate = shaderSemantics.evaluate,
        record = this._materialScratchF32, words = this._materialScratchU32,
        color = this._materialColorScratch;
      const lookup = (name, type) => {
        const value = values.get(name);
        if (value !== undefined) return value;
        const element = /^(\w+)\[(\d+)\]$/.exec(name),
          array = element && values.get(element[1]),
          width = materialTypeWidth[type] || 1;
        if (!ArrayBuffer.isView(array)) return 0;
        const at = Number(element[2]) * width;
        return width === 1 ? array[at] : array.subarray(at, at + width);
      };
      const vector = (value) => typeof value === "number" ? [value] : value,
        value3 = (name) => vector(lookup(name, "vec3"));
      try {
        color[0] = color[1] = color[2] = plan.colorScale[0];
        color[3] = plan.colorScale[1];
        for (const { node, sel } of plan.colorRGB) {
          const value = vector(evaluate(node, lookup, 1));
          for (let k = 0; k < 3; k++)
            color[k] *= sel === "x" ? value[0] : value[k];
        }
        for (const { node, sel } of plan.colorAlpha)
          color[3] *= vector(evaluate(node, lookup, 1))[sel === "w" ? 3 : 0];
        record.fill(0);
        let flags = 0;
        if (plan.projection.length) {
          flags |= MATERIAL_SPLIT;
          if (!this._materialChain(program, plan.modelView, 2)
              || !this._materialChain(program, plan.projection, 18))
            return null;
        }
        const lighting = plan.lighting;
        if (lighting) {
          flags |= MATERIAL_LIGHTING;
          const ambient = [0, 0, 0];
          if (lighting.ambient) {
            const v = value3(lighting.ambient);
            for (let k = 0; k < 3; k++) ambient[k] += v[k];
          }
          if (lighting.probe) {
            /* band 0 of the irradiance probe, over three's PI convention */
            const v = values.get(lighting.probe);
            for (let k = 0; v && k < 3; k++) ambient[k] += v[k] * 0.886227 / Math.PI;
          }
          let lights = 0;
          for (const light of lighting.lights) {
            const key = light.key, at = MATERIAL_LIGHT_WORD
              + lights * MATERIAL_LIGHT_WORDS;
            if (light.type === "HemisphereLight") {
              const sky = value3(key + ".skyColor"),
                ground = value3(key + ".groundColor");
              for (let k = 0; k < 3; k++) ambient[k] += (sky[k] + ground[k]) / 2;
              continue;
            }
            const directional = light.type === "DirectionalLight",
              lightColor = value3(key + ".color");
            words[at] = directional ? 1 : 2;
            record.set(Array.from(value3(key
              + (directional ? ".direction" : ".position"))).slice(0, 3), at + 1);
            record.set(lightColor, at + 4); record.set(lightColor, at + 7);
            record[at + 10] = 1;
            if (!directional) {
              /* three's (1 - d / distance) ^ decay range falloff, matched
                 by GE 1 / (1 + k d) at half the range. */
              const distance = Number(lookup(key + ".distance", "float")),
                decay = Number(lookup(key + ".decay", "float"));
              if (distance > 0 && decay > 0)
                record[at + 11] = (2 ** decay - 1) * 2 / distance;
            }
            lights++;
          }
          words[1] = lights;
          record.set(ambient, 34);
          for (const node of lighting.emissive) {
            const v = vector(evaluate(node, lookup, 1));
            for (let k = 0; k < 3; k++) record[37 + k] += v[k];
          }
          let specular = null, shininess = 0;
          if (lighting.model === "phong") {
            specular = vector(evaluate(lighting.specular, lookup, 1));
            shininess = Number(vector(evaluate(lighting.shininess, lookup, 1))[0]);
          } else if (lighting.model === "standard") {
            const roughness = Math.max(0.05,
                Number(lookup(lighting.roughness || "", "float")) || 0),
              metalness = Number(lookup(lighting.metalness || "", "float")) || 0,
              base = vector(values.get("diffuse") || [1, 1, 1]);
            shininess = Math.min(128, Math.max(1, 2 / roughness ** 4 - 2));
            specular = [0, 1, 2].map((k) => 0.04 + (base[k] - 0.04) * metalness);
          }
          if (specular) {
            /* three's normalized Blinn-Phong lobe, G = 1/4, at normal
               incidence: S * (n / 2 + 1) / 4. */
            const scale = 0.25 * (shininess * 0.5 + 1);
            for (let k = 0; k < 3; k++)
              record[40 + k] = Math.min(1, specular[k] * scale);
            record[43] = Math.min(128, Math.max(0, shininess));
            if (record[40] || record[41] || record[42]) flags |= MATERIAL_SPECULAR;
          }
        }
        if (plan.fog) {
          flags |= MATERIAL_FOG;
          record.set(vector(evaluate(plan.fog.color, lookup, 1)).slice(0, 3), 44);
          if (plan.fog.kind === "exp2") {
            const density = Number(vector(evaluate(plan.fog.density, lookup, 1))[0]);
            record[47] = 0; record[48] = density > 0 ? 1.73 / density : 1e30;
          } else {
            record[47] = Number(vector(evaluate(plan.fog.near, lookup, 1))[0]);
            record[48] = Number(vector(evaluate(plan.fog.far, lookup, 1))[0]);
          }
        }
        if (plan.pointSize) {
          flags |= MATERIAL_POINT_SIZE;
          const near = Number(vector(evaluate(plan.pointSize, lookup, 1))[0]),
            far = Number(vector(evaluate(plan.pointSize, lookup, 2))[0]);
          /* size = constant + scale / eye depth */
          const scale = 2 * (near - far);
          record[49] = near - scale; record[50] = scale;
        }
        if (plan.texcoord?.transform) {
          flags |= MATERIAL_TEXTURE_TRANSFORM;
          const m = vector(evaluate(plan.texcoord.transform, lookup, 1));
          record[51] = m[0]; record[52] = m[3]; record[53] = m[6];
          record[54] = m[1]; record[55] = m[4]; record[56] = m[7];
        }
        if (plan.texcoord?.point) {
          flags |= MATERIAL_POINT_COORD;
          if (plan.texcoord.flipY) flags |= MATERIAL_POINT_COORD_FLIP;
        }
        if (plan.alphaTest) {
          flags |= MATERIAL_ALPHA_TEST;
          record[57] = Number(vector(evaluate(plan.alphaTest, lookup, 1))[0]);
        }
        const vertexProgram = plan.program;
        if (vertexProgram) {
          flags |= MATERIAL_PROGRAM;
          const constants = vertexProgram.constants;
          words[MATERIAL_PROGRAM_WORD] = vertexProgram.steps
            | constants.length << 8 | vertexProgram.outputs << 16
            | vertexProgram.out << 20;
          words.set(vertexProgram.code, MATERIAL_PROGRAM_WORD + 2);
          const matrices = new Map();
          for (let k = 0; k < constants.length; k++) {
            const entry = constants[k], at = MATERIAL_PROGRAM_CONSTANT_WORD + k * 4;
            let lanes = entry.values;
            if (!lanes && entry.column !== undefined) {
              const key = entry.node;
              if (!matrices.has(key))
                matrices.set(key, vector(evaluate(entry.node, lookup, 1)));
              const m = matrices.get(key);
              lanes = m.slice(entry.column * entry.dims,
                (entry.column + 1) * entry.dims);
            } else lanes ??= vector(evaluate(entry.node, lookup, 1));
            for (let lane = 0; lane < 4; lane++) {
              const v = Number(lanes[lane] ?? 0);
              if (!Number.isFinite(v)) return null;
              record[at + lane] = v;
            }
          }
        }
        words[0] = flags;
        for (let at = 0; at < MATERIAL_PROGRAM_WORD; at++)
          if (at !== 0 && at !== 1 && !(at >= MATERIAL_LIGHT_WORD
              && (at - MATERIAL_LIGHT_WORD) % MATERIAL_LIGHT_WORDS === 0)
              && at !== MATERIAL_NORMAL_IDENTITY_WORD
              && at !== MATERIAL_NORMAL_IDENTITY_WORD + 1
              && at !== MATERIAL_NORMAL_IDENTITY_WORD + 2
              && !Number.isFinite(record[at])) return null;
        for (let k = 0; k < 4; k++) if (!Number.isFinite(color[k])) return null;
      } catch (error) {
        return null;
      }
      return record;
    }
    _writeMaterial(commandAt, record, normal, sources, normalSnapshot) {
      const base = WIRE_HEADER_WORDS + commandAt * MATERIAL_WORDS;
      this._materialWireF32.set(record, base);
      if (normalSnapshot !== undefined) {
        writeCommandAttribute(this, this._materialWireI32,
          this._materialWireU32, base, MATERIAL_NORMAL_WORD,
          normalSnapshot, sources, null);
        writeCommandSourceIdentity(this._materialWireI32, base,
          MATERIAL_NORMAL_IDENTITY_WORD, normalSnapshot);
      } else {
        writeLiveSourceIdentity(this._materialWireI32, base,
          MATERIAL_NORMAL_IDENTITY_WORD, normal);
        const wire = this._materialWireI32, at = base + MATERIAL_NORMAL_WORD,
          enabled = normal?.enabled && normal.buffer;
        wire[at] = this._sourceIndex(sources, null,
          enabled ? normal.buffer._data : null);
        wire[at + 1] = enabled ? normal.size : 0;
        wire[at + 2] = enabled ? normal.type : 0;
        wire[at + 3] = enabled && normal.normalized ? 1 : 0;
        wire[at + 4] = enabled ? normal.stride : 0;
        wire[at + 5] = enabled ? normal.offset : 0;
      }
      this._flushHasMaterial = true;
    }
    /* Reports a draw the PSP cannot make once per program, mode and
       reason on the console; the caller sets the WebGL error. */
    _reportDrawLimit(program, mode, reason, text, advice) {
      const reported = program._drawLimitReports ||= new Set(),
        key = reason + mode;
      if (reported.has(key)) return;
      reported.add(key);
      globalThis.console?.warn?.("WebGL program " + program._id + ": "
        + text + ", so it is not drawn; " + advice);
    }
    /* A draw over one submission's vertex budget is queued as several
       draws, in the order one draw rasterizes its primitives, so ordering,
       depth and blending are those of the single draw. Each part is a
       whole number of primitives: TRIANGLES split on multiples of 3, LINES
       on 2, POINTS on 1; strips overlap their neighbour by the shared
       vertices (one for LINE_STRIP, two from an even start for
       TRIANGLE_STRIP, keeping the winding). Indexed draws split by index
       range and read the same vertices. Instanced draws split by instance
       range (per-instance attributes start at the part's first instance),
       or draw each instance in vertex parts when one instance is over the
       budget. Parts that do not fit in the current batch flush it, as any
       draw would. The parts are planned once per draw plan. */
    _enqueueSplitDraw(mode, first, count, index, position, color, texcoord,
                      texture, instanceCount, instanceMatrix, instanceColor,
                      instanceTransform, matrix, uniformColor, plan,
                      material, normal, partLimit) {
      if (this._repeatedCommandCapture)
        this._repeatedCommandCapture.failed = true;
      if (plan.splitInstances !== instanceCount
          || plan.splitLimit !== partLimit) {
        plan.splitParts = splitDrawParts(plan, mode, first, count,
          index, instanceCount, instanceMatrix, instanceColor,
          instanceTransform, partLimit);
        plan.splitInstances = instanceCount;
        plan.splitLimit = partLimit;
      }
      const parts = plan.splitParts;
      for (let at = 0; at < parts.length; at++) {
        const part = parts[at];
        let partIndex = null;
        if (index) {
          partIndex = part.index;
          partIndex.data = index.data; partIndex.id = index.id;
          partIndex.generation = index.generation;
          partIndex.usage = index.usage; partIndex.type = index.type;
          partIndex.maximum = index.maximum;
          partIndex.offset = index.offset
            + part.start * typeBytes(index.type);
        }
        if (!this._enqueueDraw(mode, part.first, part.count, partIndex,
            position, color, texcoord, texture, part.instances,
            part.instanceMatrix, part.instanceColor, part.instanceTransform,
            matrix, uniformColor, null, part.count * part.instances,
            part.plan, material, normal)) return false;
      }
      diagnostics.splitDraws++;
      diagnostics.splitParts += parts.length;
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
        if (Number.isInteger(instanceCount) && count)
          this._reportDrawLimit(this._program, mode, "instances", "a draw of "
            + instanceCount + " instances is over the PSP limit of "
            + MAX_INSTANCES + " instances per draw", "draw at most "
            + MAX_INSTANCES + " instances at a time");
        this._setError(instanceCount > MAX_INSTANCES
          ? constants.OUT_OF_MEMORY : constants.INVALID_VALUE); return;
      }
      if (!count || !instanceCount) return;
      const program = this._program,
        translation = program._translation,
        shadingSteps = translation.material?.program?.steps || 0,
        /* One native submission holds MAX_VERTICES expanded vertices, and
           a per-vertex shaded draw MATERIAL_PROGRAM_STEP_LIMIT steps. A
           larger draw is queued in parts (_enqueueSplitDraw). */
        partLimit = shadingSteps ? Math.min(MAX_VERTICES,
          Math.floor(MATERIAL_PROGRAM_STEP_LIMIT / shadingSteps))
          : MAX_VERTICES,
        split = count * instanceCount > partLimit;
      if (split && count > partLimit && (mode === constants.LINE_LOOP
          || mode === constants.TRIANGLE_FAN)) {
        const loop = mode === constants.LINE_LOOP;
        this._reportDrawLimit(program, mode, "split", "a "
          + DRAW_MODE_NAMES[mode] + " draw of " + count + " vertices is over"
          + " the PSP limit of " + partLimit + " vertices per submission"
          + (partLimit < MAX_VERTICES ? " for this program's " + shadingSteps
            + "-step per-vertex shading" : "")
          + ", and a " + (loop ? "loop" : "fan")
          + " cannot be split into parts exactly", "draw "
          + (loop ? "LINE_STRIP or loops" : "TRIANGLES or fans")
          + " of at most " + partLimit + " vertices");
        this._setError(constants.OUT_OF_MEMORY); return;
      }
      const vertexState = this._vertexArray?._state
          || this._defaultVertexArrayState,
        previousPlan = vertexState.drawPlan;
      let cachedPlan = previousPlan;
      let position, color, texcoord, instanceMatrix, instanceColor,
        instanceTransform, normal = null;
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
        normal = cachedPlan.normal;
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
            && ((translation.texcoord
                 && (!texcoord?.enabled || !texcoord.buffer))
                || !this._texture?._pixels)) {
          this._setError(constants.INVALID_OPERATION); return;
        }
        normal = translation.normalLocation >= 0
          ? this._attributesState[translation.normalLocation] : null;
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
                  texcoord, vertexFirst, vertexCount))
            || (normal?.enabled && (normal.divisor !== 0
                || !this._attributeRangeAvailable(
                  normal, vertexFirst, vertexCount)))
            || (translation.material && (instanceCount > 1
                || instanceMatrix || instanceColor || instanceTransform))) {
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
          instanceTransform, maximumInstanceCount: instanceCount, normal,
          uniformColorValue: translation.uniformColor
            ? program._uniformValues.get(translation.uniformColor.name)
            : translation.constantColor || DEFAULT_UNIFORM_COLOR,
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
      let material = null;
      if (translation.material) {
        material = this._prepareMaterial(program, translation.material);
        if (!material) { this._setError(constants.INVALID_OPERATION); return; }
      }
      const selectedColor = material
        ? this._materialColorScratch : cachedPlan.uniformColorValue;
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
      if (split ? !this._enqueueSplitDraw(
            mode, first, count, index, position, color, texcoord,
            textureSnapshot, instanceCount, instanceMatrix, instanceColor,
            instanceTransform, program._combinedMatrix, uniformColor,
            cachedPlan, material, normal, partLimit)
          : !this._enqueueDraw(
            mode, first, count, index, position, color, texcoord,
            textureSnapshot, instanceCount, instanceMatrix, instanceColor,
            instanceTransform, program._combinedMatrix, uniformColor,
            null, count * instanceCount, cachedPlan, material, normal)) return;
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
        [constants.MAX_VARYING_VECTORS]: MAX_VARYING_VECTORS,
        [constants.MAX_VERTEX_UNIFORM_VECTORS]: MAX_VERTEX_UNIFORM_VECTORS,
        [constants.MAX_FRAGMENT_UNIFORM_VECTORS]: MAX_FRAGMENT_UNIFORM_VECTORS,
        [constants.ARRAY_BUFFER_BINDING]: this._arrayBuffer,
        [constants.ELEMENT_ARRAY_BUFFER_BINDING]: this._elementBuffer,
        [VERTEX_ARRAY_BINDING_OES]: this._vertexArray,
        [constants.TEXTURE_BINDING_2D]: this._texture,
        [constants.ACTIVE_TEXTURE]: constants.TEXTURE0,
        [constants.CURRENT_PROGRAM]: this._program,
        [constants.COLOR_CLEAR_VALUE]: new Float32Array(this._clearColor),
        [constants.DEPTH_CLEAR_VALUE]: this._clearDepth,
        [constants.DEPTH_FUNC]: this._depthFunc,
        [constants.BLEND_SRC_RGB]: this._blendSrc,
        [constants.BLEND_DST_RGB]: this._blendDst,
        [constants.BLEND_SRC_ALPHA]: this._blendSrcAlpha,
        [constants.BLEND_DST_ALPHA]: this._blendDstAlpha,
        [constants.BLEND_COLOR]: new Float32Array(this._blendColor),
        [constants.BLEND_EQUATION_RGB]: this._blendEquation,
        [constants.BLEND_EQUATION_ALPHA]: this._blendEquationAlpha,
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
    getShaderPrecisionFormat() { return make.WebGLShaderPrecisionFormat(127, 127, 23); }
    getSupportedExtensions() {
      return ["ANGLE_instanced_arrays", "EXT_blend_minmax",
        "OES_vertex_array_object", "TILEFINCH_repeated_frame_commands"];
    }
    getExtension(name) {
      name = String(name).toLowerCase();
      if (name === "angle_instanced_arrays") {
        if (!this._instancedExtension)
          this._instancedExtension = new AngleInstancedArrays(this);
        return this._instancedExtension;
      }
      if (name === "ext_blend_minmax")
        return this._blendMinmaxExtension ||= new ExtBlendMinmax();
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
      const framebuffer = make.WebGLFramebuffer(this, this._nextId++);
      this._framebufferCount++; this._resources.add(framebuffer);
      return framebuffer;
    }
    createRenderbuffer() {
      if (this._lost) return null;
      if (this._renderbufferCount >= MAX_RENDERBUFFER_OBJECTS) {
        this._setError(constants.OUT_OF_MEMORY); return null;
      }
      const renderbuffer = make.WebGLRenderbuffer(this, this._nextId++);
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

  /* Instances take the interface prototypes; the context's members move
     onto WebGLRenderingContext.prototype, where the constants and
     webgl-interfaces.js's forwarding members already are. Only this module
     constructs them (the interfaces' own constructors throw). */
  const implementation = (Impl, Interface) =>
      (...args) => reflectConstruct(Impl, args, Interface),
    make = {
      WebGLRenderingContext: implementation(
        WebGLRenderingContextImpl, WebGLRenderingContext),
      WebGLBuffer: implementation(WebGLBufferImpl, WebGLBuffer),
      WebGLTexture: implementation(WebGLTextureImpl, WebGLTexture),
      WebGLShader: implementation(WebGLShaderImpl, WebGLShader),
      WebGLProgram: implementation(WebGLProgramImpl, WebGLProgram),
      WebGLUniformLocation: implementation(
        WebGLUniformLocationImpl, WebGLUniformLocation),
      WebGLActiveInfo: implementation(WebGLActiveInfoImpl, WebGLActiveInfo),
      WebGLShaderPrecisionFormat: implementation(
        WebGLShaderPrecisionFormatImpl, WebGLShaderPrecisionFormat),
      WebGLFramebuffer: implementation(WebGLFramebufferImpl, WebGLFramebuffer),
      WebGLRenderbuffer: implementation(
        WebGLRenderbufferImpl, WebGLRenderbuffer),
    };
  /* A member replaces webgl-interfaces.js's forwarding one, or fills an
     empty slot; one a page defined itself stays, and so does every member
     of a prototype the page froze (the forwarding ones then serve). The
     forwarding members reach this module's through the table. The
     context's internal members go where it alone reaches them: onto the
     interface prototype when they can, else onto a prototype between it
     and the contexts. */
  {
    const source = WebGLRenderingContextImpl.prototype,
      target = WebGLRenderingContext.prototype,
      keys = ownKeys(source),
      methods = { __proto__: null },
      internals = { __proto__: target };
    let between = false;
    for (let at = 0; at < keys.length; at++) {
      const key = keys[at];
      if (key === "constructor") continue;
      const descriptor = getOwnPropertyDescriptor(source, key),
        stub = stubs[key];
      if (stub !== undefined)
        methods[key] = descriptor.get === undefined
          ? descriptor.value : descriptor.get;
      const held = getOwnPropertyDescriptor(target, key);
      let placed = false;
      if (held === undefined
          || (stub !== undefined
              && (held.value === stub || held.get === stub))) {
        try {
          defineProperty(target, key, descriptor);
          placed = true;
        } catch {
          /* Frozen by the page, or the page's own non-configurable
             member. */
        }
      }
      if (!placed && stub === undefined) {
        defineProperty(internals, key, descriptor);
        between = true;
      }
    }
    publish("__tilefinchWebGLMethods", freeze(methods));
    if (between) {
      const Context = function () {};
      Context.prototype = internals;
      make.WebGLRenderingContext =
        implementation(WebGLRenderingContextImpl, Context);
    }
  }

  publish("__tilefinchCreateWebGLContext", (canvas, attributes) => {
    let context = contextForCanvas(canvas);
    if (context) return context;
    if (activeContexts().length >= MAX_CONTEXTS) return null;
    if (!boundedDrawingSize(canvas.width, canvas.height)) return null;
    context = make.WebGLRenderingContext(canvas, attributes || {});
    contexts.set(canvas,
      typeof WeakRef === "function" ? new WeakRef(context) : context);
    liveContexts.push(typeof WeakRef === "function" ? new WeakRef(context) : context);
    context._resize();
    diagnostics.contexts++; return context;
  });
  publish("__tilefinchWebGLResize", (canvas) => contextForCanvas(canvas)?._resize());
  publish("__tilefinchWebGLConnected", (canvas) => contextForCanvas(canvas)?._schedule());
  /* Also runs inside a task (a relayout a script's geometry read starts
     adopts the image table), where the frame is unfinished until the
     task's own flush. */
  publish("__tilefinchFlushWebGLSurfaces", () => {
    for (const context of activeContexts())
      context._flush(!context._flushQueued);
  });
  publish("__tilefinchWebGLReadback", (canvas) => {
    const context = contextForCanvas(canvas);
    if (!context || context._lost) return null;
    if (context._flush() !== FLUSH_OK) return null;
    const snapshot = __tilefinchWebGLSnapshot(canvas.__handle);
    return snapshot ? { width: Number(snapshot.width), height: Number(snapshot.height),
      pixels: new Uint8ClampedArray(snapshot.pixels), originClean: true,
      surfaceUnavailable: false } : null;
  });
  try {
    defineProperty(globalThis, "WebGLVertexArrayOES", { __proto__: null,
      configurable: true, enumerable: true, writable: true,
      value: WebGLVertexArrayOES });
  } catch {
    /* A page's own non-configurable global of that name stays. */
  }
  defineProperty(globalThis, "__tilefinchWebGLDiagnostics",
    { __proto__: null, value: diagnostics, configurable: false, enumerable: false });
})();
