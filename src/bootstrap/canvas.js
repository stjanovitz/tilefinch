(() => {
  /* Pixel storage remains deliberately small and lazy. QuickJS charges every
     backing Uint8ClampedArray to the existing script heap, while this
     per-surface ceiling prevents one canvas from consuming the whole PSP
     profile before ordinary script state gets a chance to run. */
  const pixelByteLimit = 512 * 1024,
    pixelLimit = pixelByteLimit / 4,
    compositeOperations = [
      "", "source-over", "copy", "destination-over", "source-in",
      "source-out", "source-atop", "destination-in", "destination-out",
      "destination-atop", "xor", "lighter",
    ],
    states = new WeakMap(),
    contexts = new WeakMap(),
    pendingCommits = [],
    canvasDiagnostics = {
      rectangleCommands: 0,
      rectangleBatches: 0,
      pathRasters: 0,
      textRasters: 0,
      imageRasters: 0,
      imageCommands: 0,
      imageBatches: 0,
      paintCommands: 0,
      paintBatches: 0,
      shadowCommands: 0,
      surfaceCommits: 0,
    },
    flushPaintCommands = (state) => {
      if (!state.paintCommands.length || !state.pixels) return true;
      const payloads = state.paintCommands.map(
          (command) => [command.kind, ...command.args]),
        failed = Number(__tilefinchCanvasRasterPaintBatch(
          state.pixels, state.width, state.height, payloads,
        ));
      if (!Number.isSafeInteger(failed) || failed < 0) return false;
      for (let index = 0; index < state.paintCommands.length; index++) {
        const command = state.paintCommands[index];
        if (failed & (1 << index)) fallbackPaintCommand(state, command);
        else if (command.kind === 0) canvasDiagnostics.pathRasters++;
        else canvasDiagnostics.textRasters++;
      }
      canvasDiagnostics.paintBatches++;
      state.paintCommands.length = 0;
      state.paintCommandValues = 0;
      return true;
    },
    flushImageCommands = (state) => {
      if (!flushPaintCommands(state)) return false;
      if (!state.imageCommands.length || !state.pixels) return true;
      const sources = [],
        serialized = new Float64Array(state.imageCommands.length * 24);
      let at = 0;
      for (const command of state.imageCommands) {
        const sourceIndex = sources.length;
        sources.push(command.source);
        for (const value of [
          sourceIndex, command.sourceWidth, command.sourceHeight,
          command.sx, command.sy, command.sw, command.sh,
          command.dx, command.dy, command.dw, command.dh,
          command.smooth ? 1 : 0, command.globalAlpha, command.operation,
          ...command.transform, ...command.clip,
        ]) serialized[at++] = value;
      }
      if (!__tilefinchCanvasRasterImageBatch(
        state.pixels, state.width, state.height, sources, serialized,
      )) return false;
      canvasDiagnostics.imageBatches++;
      state.imageCommands.length = 0;
      state.imageCommandBytes = 0;
      return true;
    },
    flushRectCommands = (state) => {
      if (!flushImageCommands(state)) return false;
      if (!state.rectCommands.length || !state.pixels) return true;
      const serialized = new Float64Array(state.rectCommands.length * 10);
      let at = 0;
      for (const command of state.rectCommands)
        for (const value of command) serialized[at++] = value;
      if (!__tilefinchCanvasRasterRectBatch(
        state.pixels, state.width, state.height, serialized,
      )) return false;
      canvasDiagnostics.rectangleBatches++;
      state.rectCommands.length = 0;
      return true;
    },
    flushCanvasState = (state) => {
      const dirty = state.dirty;
      if (!dirty || !state.pixels) return false;
      /* A disconnected canvas cannot publish and must not occupy one of the
         eight strong queue slots indefinitely. Keep its dirty state and
         commands intact: the DOM insertion hook schedules it again if the
         author later reconnects the same canvas. */
      if (!state.canvas.isConnected) return false;
      if (!flushRectCommands(state)) return true;
      const committed = __tilefinchCommitCanvasSurface(
        state.canvas.__handle,
        state.width,
        state.height,
        state.pixels,
        dirty.left,
        dirty.top,
        dirty.right,
        dirty.bottom,
      );
      if (committed) state.dirty = null;
      if (committed) canvasDiagnostics.surfaceCommits++;
      return !committed;
    },
    flushCanvasSurfaces = (deferredState = null) => {
      let write = 0;
      for (let index = 0; index < pendingCommits.length; index++) {
        const state = pendingCommits[index];
        if (flushCanvasState(state)) pendingCommits[write++] = state;
      }
      pendingCommits.length = write;
      /* A ninth dirty surface deliberately does not enlarge the retained
         eight-entry queue. Its own microtask gets one bounded publication
         attempt after the queued batch drains; on success it can never be
         stranded merely because it was omitted during scheduling. */
      if (
        deferredState &&
        deferredState.dirty &&
        !pendingCommits.includes(deferredState)
      ) {
        if (flushCanvasState(deferredState) && pendingCommits.length < 8)
          pendingCommits.push(deferredState);
      }
    },
    scheduleCanvasCommit = (state, rect) => {
      if (!state.pixels || !rect) return;
      if (!state.dirty) state.dirty = { ...rect };
      else {
        state.dirty.left = Math.min(state.dirty.left, rect.left);
        state.dirty.top = Math.min(state.dirty.top, rect.top);
        state.dirty.right = Math.max(state.dirty.right, rect.right);
        state.dirty.bottom = Math.max(state.dirty.bottom, rect.bottom);
      }
      if (!pendingCommits.includes(state)) {
        if (pendingCommits.length < 8) pendingCommits.push(state);
        else if (state.canvas.isConnected) {
          const displaced = pendingCommits.findIndex(
            (pending) => !pending.canvas.isConnected,
          );
          if (displaced >= 0) pendingCommits[displaced] = state;
        }
      }
      if (!state.commitQueued) {
        state.commitQueued = true;
        queueMicrotask(() => {
          state.commitQueued = false;
          flushCanvasSurfaces(state);
        });
      }
    },
    markCanvasFull = (state) =>
      scheduleCanvasCommit(state, {
        left: 0,
        top: 0,
        right: state.width,
        bottom: state.height,
      }),
    dimension = (canvas, name, fallback) => {
      const text = canvas.getAttribute(name);
      if (text === null || !/^(?:0|[1-9][0-9]*)$/.test(text))
        return fallback;
      const value = Number(text);
      return Number.isSafeInteger(value) && value <= 0x7fffffff
        ? value
        : fallback;
    },
    dimensions = (canvas) => ({
      width: dimension(canvas, "width", 300),
      height: dimension(canvas, "height", 150),
    }),
    resetState = (state) => {
      const size = dimensions(state.canvas);
      state.width = size.width;
      state.height = size.height;
      state.pixels = null;
      state.dirty = null;
      state.rectCommands.length = 0;
      state.imageCommands.length = 0;
      state.imageCommandBytes = 0;
      state.paintCommands.length = 0;
      state.paintCommandValues = 0;
      state.surfaceUnavailable =
        size.width !== 0 &&
        size.height !== 0 &&
        size.width > Math.floor(pixelLimit / size.height);
      state.originClean = true;
      state.ignoredSaveDepth = 0;
      state.fill = [0, 0, 0, 255];
      state.fillStyle = "#000000";
      state.fillPaint = null;
      state.stroke = [0, 0, 0, 255];
      state.strokeStyle = "#000000";
      state.shadow = [0, 0, 0, 0];
      state.shadowColor = "rgba(0, 0, 0, 0)";
      state.shadowBlur = 0;
      state.shadowOffsetX = 0;
      state.shadowOffsetY = 0;
      state.globalAlpha = 1;
      state.globalCompositeOperation = "source-over";
      state.lineWidth = 1;
      state.lineCap = "butt";
      state.lineJoin = "miter";
      state.miterLimit = 10;
      state.font = "10px sans-serif";
      state.textAlign = "start";
      state.textBaseline = "alphabetic";
      state.direction = "inherit";
      state.imageSmoothingEnabled = true;
      state.imageSmoothingQuality = "low";
      state.transform = [1, 0, 0, 1, 0, 0];
      state.clipRect = [0, 0, size.width, size.height];
      state.clipPaths = [];
      state.path = [];
      state.subpath = null;
      state.lineDash = [];
      state.lineDashOffset = 0;
      state.stack.length = 0;
    },
    stateFor = (canvas) => {
      let state = states.get(canvas);
      if (!state) {
        state = {
          canvas,
          width: 0,
          height: 0,
          pixels: null,
          surfaceUnavailable: false,
          originClean: true,
          ignoredSaveDepth: 0,
          fill: [0, 0, 0, 255],
          fillStyle: "#000000",
          fillPaint: null,
          stroke: [0, 0, 0, 255],
          strokeStyle: "#000000",
          shadow: [0, 0, 0, 0],
          shadowColor: "rgba(0, 0, 0, 0)",
          shadowBlur: 0,
          shadowOffsetX: 0,
          shadowOffsetY: 0,
          globalAlpha: 1,
          globalCompositeOperation: "source-over",
          lineWidth: 1,
          lineCap: "butt",
          lineJoin: "miter",
          miterLimit: 10,
          font: "10px sans-serif",
          textAlign: "start",
          textBaseline: "alphabetic",
          direction: "inherit",
          imageSmoothingEnabled: true,
          imageSmoothingQuality: "low",
          transform: [1, 0, 0, 1, 0, 0],
          clipRect: [0, 0, 0, 0],
          clipPaths: [],
          path: [],
          subpath: null,
          lineDash: [],
          lineDashOffset: 0,
          stack: [],
          dirty: null,
          commitQueued: false,
          rectCommands: [],
          imageCommands: [],
          imageCommandBytes: 0,
          paintCommands: [],
          paintCommandValues: 0,
        };
        states.set(canvas, state);
        resetState(state);
      } else {
        const size = dimensions(canvas);
        if (size.width !== state.width || size.height !== state.height)
          resetState(state);
      }
      return state;
    },
    requirePositiveSize = (width, height) => {
      width = Math.trunc(Number(width));
      height = Math.trunc(Number(height));
      if (!width || !height)
        throw new DOMException(
          "The source width or height is zero",
          "IndexSizeError",
        );
      if (width < 0) width = -width;
      if (height < 0) height = -height;
      if (
        width > pixelLimit ||
        height > Math.floor(pixelLimit / width)
      )
        throw new DOMException(
          "The requested pixel buffer exceeds the canvas limit",
          "QuotaExceededError",
        );
      return { width, height };
    },
    byte = (value) =>
      Math.max(0, Math.min(255, Math.round(Number(value) || 0))),
    hexByte = (value) => value.toString(16).padStart(2, "0"),
    parseColor = (value) => {
      const packed = __tilefinchParseColor(String(value));
      if (packed === null) return null;
      const bits = Number(packed) >>> 0,
        red = (bits >>> 16) & 255,
        green = (bits >>> 8) & 255,
        blue = bits & 255,
        alpha = bits >>> 24;
      return {
        components: [red, green, blue, alpha],
        serialized:
          alpha === 255
            ? `#${hexByte(red)}${hexByte(green)}${hexByte(blue)}`
            : `rgba(${red}, ${green}, ${blue}, ${Number(
                (alpha / 255).toFixed(3),
              )})`,
      };
    },
    ensureSurface = (state) => {
      if (state.surfaceUnavailable || state.width === 0 || state.height === 0)
        return null;
      if (!state.pixels)
        state.pixels = new Uint8ClampedArray(
          state.width * state.height * 4,
        );
      return state.pixels;
    },
    normalizedRect = (state, x, y, width, height) => {
      x = Number(x);
      y = Number(y);
      width = Number(width);
      height = Number(height);
      if (![x, y, width, height].every(Number.isFinite) || !width || !height)
        return null;
      if (width < 0) {
        x += width;
        width = -width;
      }
      if (height < 0) {
        y += height;
        height = -height;
      }
      const left = Math.max(state.clipRect[0], Math.floor(x)),
        top = Math.max(state.clipRect[1], Math.floor(y)),
        right = Math.min(state.clipRect[2], Math.ceil(x + width)),
        bottom = Math.min(state.clipRect[3], Math.ceil(y + height));
      return right <= left || bottom <= top
        ? null
        : { left, top, right, bottom };
    },
    transformPoint = (state, x, y) => {
      const [a, b, c, d, e, f] = state.transform;
      return {
        x: a * Number(x) + c * Number(y) + e,
        y: b * Number(x) + d * Number(y) + f,
      };
    },
    blendPixel = (state, x, y, color) => {
      x = Math.round(x);
      y = Math.round(y);
      if (
        x < state.clipRect[0] || y < state.clipRect[1] ||
        x >= state.clipRect[2] || y >= state.clipRect[3]
      ) return;
      for (const clip of state.clipPaths)
        if (!pointInSubpaths(clip.subpaths, x + 0.5, y + 0.5, clip.evenOdd))
          return;
      const pixels = ensureSurface(state);
      if (!pixels) return;
      const at = (y * state.width + x) * 4,
        sourceAlpha = (color[3] / 255) * state.globalAlpha;
      if (state.globalCompositeOperation === "source-over" && sourceAlpha <= 0)
        return;
      if (state.globalCompositeOperation === "source-over" && sourceAlpha >= 1) {
        pixels[at] = color[0];
        pixels[at + 1] = color[1];
        pixels[at + 2] = color[2];
        pixels[at + 3] = 255;
        return;
      }
      const destinationAlpha = pixels[at + 3] / 255,
        operation = state.globalCompositeOperation;
      let sourceFactor = 1,
        destinationFactor = 1 - sourceAlpha;
      if (operation === "copy") destinationFactor = 0;
      else if (operation === "destination-over") {
        sourceFactor = 1 - destinationAlpha;
        destinationFactor = 1;
      } else if (operation === "source-in") {
        sourceFactor = destinationAlpha;
        destinationFactor = 0;
      } else if (operation === "source-out") {
        sourceFactor = 1 - destinationAlpha;
        destinationFactor = 0;
      } else if (operation === "source-atop") {
        sourceFactor = destinationAlpha;
        destinationFactor = 1 - sourceAlpha;
      } else if (operation === "destination-in") {
        sourceFactor = 0;
        destinationFactor = sourceAlpha;
      } else if (operation === "destination-out") {
        sourceFactor = 0;
        destinationFactor = 1 - sourceAlpha;
      } else if (operation === "destination-atop") {
        sourceFactor = 1 - destinationAlpha;
        destinationFactor = sourceAlpha;
      } else if (operation === "xor") {
        sourceFactor = 1 - destinationAlpha;
        destinationFactor = 1 - sourceAlpha;
      } else if (operation === "lighter") {
        destinationFactor = 1;
      }
      const outputAlpha = Math.min(
        1,
        sourceAlpha * sourceFactor + destinationAlpha * destinationFactor,
      );
      if (outputAlpha <= 0) {
        pixels.fill(0, at, at + 4);
        return;
      }
      for (let channel = 0; channel < 3; channel++)
        pixels[at + channel] = byte(
          (color[channel] * sourceAlpha * sourceFactor +
            pixels[at + channel] * destinationAlpha * destinationFactor) /
            outputAlpha,
        );
      pixels[at + 3] = byte(outputAlpha * 255);
    },
    drawLine = (state, from, to, color, width = 1) => {
      let x0 = Math.round(from.x),
        y0 = Math.round(from.y),
        x1 = Math.round(to.x),
        y1 = Math.round(to.y);
      const dx = Math.abs(x1 - x0),
        sx = x0 < x1 ? 1 : -1,
        dy = -Math.abs(y1 - y0),
        sy = y0 < y1 ? 1 : -1,
        radius = Math.min(8, Math.max(0, Math.floor(width / 2)));
      if (
        (Math.max(dx, -dy) + 1) * (radius * 2 + 1) ** 2 >
        256 * 1024
      )
        throw new DOMException(
          "Stroke exceeds the bounded canvas work limit",
          "QuotaExceededError",
        );
      let error = dx + dy,
        work = 0;
      for (;;) {
        for (let oy = -radius; oy <= radius; oy++)
          for (let ox = -radius; ox <= radius; ox++)
            blendPixel(state, x0 + ox, y0 + oy, color);
        if (x0 === x1 && y0 === y1) break;
        if (++work > pixelLimit) break;
        const twice = 2 * error;
        if (twice >= dy) {
          error += dy;
          x0 += sx;
        }
        if (twice <= dx) {
          error += dx;
          y0 += sy;
        }
      }
    },
    pointInSubpaths = (subpaths, x, y, evenOdd = false) => {
      let winding = 0,
        parity = false;
      for (const points of subpaths) {
        if (points.length < 2) continue;
        for (let index = 0; index < points.length; index++) {
          const first = points[index],
            second = points[(index + 1) % points.length];
          if (
            (first.y <= y && second.y > y) ||
            (second.y <= y && first.y > y)
          ) {
            const crossing =
              first.x +
              ((y - first.y) * (second.x - first.x)) /
                (second.y - first.y);
            if (crossing > x) {
              parity = !parity;
              winding += second.y > first.y ? 1 : -1;
            }
          }
        }
      }
      return evenOdd ? parity : winding !== 0;
    },
    pointSegmentProjection = (point, first, second) => {
      const dx = second.x - first.x,
        dy = second.y - first.y,
        denominator = dx * dx + dy * dy,
        position = denominator
          ? ((point.x - first.x) * dx + (point.y - first.y) * dy) /
            denominator : 0,
        ratio = Math.max(0, Math.min(1, position)),
        x = first.x + ratio * dx,
        y = first.y + ratio * dy;
      return {
        distance: Math.hypot(point.x - x, point.y - y),
        length: Math.sqrt(denominator),
        perpendicular: denominator
          ? Math.abs(dx * (first.y - point.y) -
                     (first.x - point.x) * dy) / Math.sqrt(denominator)
          : Math.hypot(point.x - first.x, point.y - first.y),
        position,
      };
    },
    dashContains = (dash, offset, distance) => {
      if (!dash.length) return true;
      const total = dash.reduce((sum, value) => sum + value, 0);
      if (total <= 0) return true;
      let at = ((distance + offset) % total + total) % total;
      for (let index = 0; index < dash.length; index++) {
        if (at <= dash[index]) return index % 2 === 0;
        at -= dash[index];
      }
      return true;
    },
    flattenedPath = (subpaths) => {
      let count = Math.max(0, subpaths.length - 1);
      for (const points of subpaths) count += points.length;
      const output = new Float64Array(count * 2);
      let at = 0;
      for (let index = 0; index < subpaths.length; index++) {
        if (index) {
          output[at++] = NaN;
          output[at++] = NaN;
        }
        for (const point of subpaths[index]) {
          output[at++] = point.x;
          output[at++] = point.y;
        }
      }
      return output;
    },
    canvasDirtyBounds = (state, subpaths, padding = 0) => {
      let left = state.width,
        top = state.height,
        right = 0,
        bottom = 0,
        found = false;
      for (const points of subpaths)
        for (const point of points) {
          if (!Number.isFinite(point.x) || !Number.isFinite(point.y)) continue;
          found = true;
          left = Math.min(left, point.x);
          top = Math.min(top, point.y);
          right = Math.max(right, point.x);
          bottom = Math.max(bottom, point.y);
        }
      if (!found) return null;
      left = Math.max(state.clipRect[0], Math.floor(left - padding));
      top = Math.max(state.clipRect[1], Math.floor(top - padding));
      right = Math.min(state.clipRect[2], Math.ceil(right + padding));
      bottom = Math.min(state.clipRect[3], Math.ceil(bottom + padding));
      return right > left && bottom > top ? { left, top, right, bottom } : null;
    },
    serializedClips = (state) => {
      let count = 1;
      const paths = [];
      for (const clip of state.clipPaths) {
        const path = flattenedPath(clip.subpaths);
        paths.push({ evenOdd: clip.evenOdd, path });
        count += 2 + path.length;
      }
      const output = new Float64Array(count);
      output[0] = paths.length;
      let at = 1;
      for (const clip of paths) {
        output[at++] = clip.evenOdd ? 1 : 0;
        output[at++] = clip.path.length;
        output.set(clip.path, at);
        at += clip.path.length;
      }
      return output;
    },
    serializedPaint = (paint) => {
      if (!(paint instanceof CanvasGradient)) return new Float64Array();
      const output = new Float64Array(8 + paint._stops.length * 5);
      output[0] = { linear: 1, radial: 2, conic: 3 }[paint._kind] || 0;
      for (let index = 0; index < Math.min(6, paint._values.length); index++)
        output[index + 1] = paint._values[index];
      output[7] = paint._stops.length;
      let at = 8;
      for (const stop of paint._stops) {
        output[at++] = stop.offset;
        for (const channel of stop.color) output[at++] = channel;
      }
      return output;
    },
    canvasFontSpec = (font) => {
      const text = String(font),
        sizeMatch = /(?:^|\s)([1-9][0-9]*(?:\.[0-9]+)?)px(?:\s|\/|$)/.exec(text),
        lower = text.toLowerCase();
      return {
        size: Math.max(1, Math.min(256, Math.round(sizeMatch ? Number(sizeMatch[1]) : 10))),
        family: /\bmonospace\b/.test(lower)
          ? 2
          : /\bsans-serif\b/.test(lower)
            ? 0
            : /\bserif\b/.test(lower) ? 1 : 0,
        bold: /\b(?:bold|[6-9]00)\b/.test(lower),
        italic: /\b(?:italic|oblique)\b/.test(lower),
      };
    },
    transformedLineScale = (state) => {
      const [a, b, c, d] = state.transform,
        determinant = Math.abs(a * d - b * c);
      return Math.max(0.01, Math.min(64, Math.sqrt(determinant)));
    },
    paintCommandCost = (args) => args.reduce((total, value) => {
      if (ArrayBuffer.isView(value)) return total + value.length;
      if (typeof value === "string") return total + Math.ceil(value.length / 4);
      return total + 1;
    }, 1),
    queuePaintCommand = (state, command) => {
      const cost = paintCommandCost(command.args);
      if (
        state.paintCommands.length >= 16 ||
        (state.paintCommands.length &&
          cost > 4096 - Math.min(4096, state.paintCommandValues))
      ) {
        if (!flushPaintCommands(state)) return false;
      }
      state.paintCommands.push(command);
      state.paintCommandValues = Math.min(
        8192, state.paintCommandValues + cost,
      );
      canvasDiagnostics.paintCommands++;
      if (command.shadow) canvasDiagnostics.shadowCommands++;
      return state.paintCommands.length < 16 || flushPaintCommands(state);
    },
    fallbackPaintCommand = (state, command) => {
      const fallback = command.fallback;
      if (!fallback || command.kind !== 0) return;
      const local = {
        ...state,
        fill: fallback.color,
        fillPaint: fallback.paint,
        stroke: fallback.color,
        globalAlpha: fallback.globalAlpha,
        globalCompositeOperation: fallback.operation,
        clipRect: fallback.clipRect,
        clipPaths: fallback.clipPaths,
      };
      if (fallback.fill) {
        const bounds = canvasDirtyBounds(local, fallback.subpaths, 1);
        if (!bounds) return;
        for (let y = bounds.top; y < bounds.bottom; y++)
          for (let x = bounds.left; x < bounds.right; x++)
            if (pointInSubpaths(
              fallback.subpaths, x + 0.5, y + 0.5, fallback.evenOdd,
            ))
              blendPixel(
                local, x, y,
                fallback.paint
                  ? fallback.paint._colorAt(x + 0.5, y + 0.5)
                  : fallback.color,
              );
      } else {
        for (const points of fallback.subpaths)
          for (let index = 1; index < points.length; index++)
            drawLine(
              local, points[index - 1], points[index], fallback.color,
              fallback.lineWidth,
            );
      }
    },
    shadowActive = (state) => state.shadow[3] !== 0 &&
      (state.shadowBlur !== 0 || state.shadowOffsetX !== 0 ||
       state.shadowOffsetY !== 0),
    shadowSamples = (state, workUnits = 0) => {
      if (!shadowActive(state)) return [];
      const x = state.shadowOffsetX, y = state.shadowOffsetY;
      /* Nine samples are enough to soften small controls without introducing
         a general blur pass. Complex paths and long labels deliberately fall
         back to one offset sample so hostile geometry cannot multiply native
         raster work by nine. */
      if (state.shadowBlur <= 0 || workUnits > 512) return [[x, y, 1]];
      const radius = Math.max(1, Math.ceil(state.shadowBlur / 2));
      return [
        [x, y, 0.24],
        [x - radius, y, 0.095], [x + radius, y, 0.095],
        [x, y - radius, 0.095], [x, y + radius, 0.095],
        [x - radius, y - radius, 0.095],
        [x + radius, y - radius, 0.095],
        [x - radius, y + radius, 0.095],
        [x + radius, y + radius, 0.095],
      ];
    },
    shiftedCoordinates = (coordinates, x, y) => {
      const shifted = new Float64Array(coordinates.length);
      for (let at = 0; at < coordinates.length; at += 2) {
        shifted[at] = Number.isFinite(coordinates[at])
          ? coordinates[at] + x : coordinates[at];
        shifted[at + 1] = Number.isFinite(coordinates[at + 1])
          ? coordinates[at + 1] + y : coordinates[at + 1];
      }
      return shifted;
    },
    queuePathShadows = (state, args) => {
      for (const [offsetX, offsetY, weight] of shadowSamples(
        state, args[0].length,
      )) {
        const shadowArgs = [...args];
        shadowArgs[0] = shiftedCoordinates(args[0], offsetX, offsetY);
        shadowArgs.splice(3, 4, ...state.shadow);
        shadowArgs[7] = state.globalAlpha * weight;
        if (!queuePaintCommand(state, {
          kind: 0, args: shadowArgs, shadow: true,
        })) return false;
      }
      return true;
    },
    queueTextShadows = (state, args) => {
      for (const [offsetX, offsetY, weight] of shadowSamples(
        state, String(args[0]).length * Math.max(1, args[3]),
      )) {
        const shadowArgs = [...args],
          transform = new Float64Array(args[15]);
        transform[4] += offsetX;
        transform[5] += offsetY;
        shadowArgs.splice(7, 4, ...state.shadow);
        shadowArgs[11] = state.globalAlpha * weight;
        shadowArgs[15] = transform;
        if (!queuePaintCommand(state, {
          kind: 1, args: shadowArgs, shadow: true,
        })) return false;
      }
      return true;
    },
    shadowPadding = (state) => shadowActive(state)
      ? Math.ceil(state.shadowBlur + Math.max(
          Math.abs(state.shadowOffsetX), Math.abs(state.shadowOffsetY),
        )) : 0;

  /* Keep at most one decoded DOM-image snapshot between draw calls. Sprite
     sheets then avoid repeatedly copying RGBA through QuickJS, while a URL
     change or another source immediately releases the old bounded snapshot. */
  let cachedImageSource = null,
    cachedImageUrl = "",
    cachedImageSnapshot = null;

  class CanvasGradient {
    constructor(kind, values) {
      this._kind = kind;
      this._values = values.map(Number);
      this._stops = [];
    }
    addColorStop(offset, color) {
      offset = Number(offset);
      const parsed = parseColor(color);
      if (!Number.isFinite(offset) || offset < 0 || offset > 1)
        throw new DOMException("Invalid gradient offset", "IndexSizeError");
      if (!parsed) throw new DOMException("Invalid gradient color", "SyntaxError");
      if (this._stops.length >= 16)
        throw new DOMException(
          "Gradient stop quota exceeded",
          "QuotaExceededError",
        );
      this._stops.push({ offset, color: parsed.components });
      this._stops.sort((left, right) => left.offset - right.offset);
    }
    _colorAt(x, y) {
      if (!this._stops.length) return [0, 0, 0, 0];
      let offset = 0;
      if (this._kind === "linear") {
        const [x0, y0, x1, y1] = this._values,
          dx = x1 - x0,
          dy = y1 - y0,
          denominator = dx * dx + dy * dy;
        offset =
          denominator === 0 ? 0 : ((x - x0) * dx + (y - y0) * dy) / denominator;
      } else if (this._kind === "radial") {
        const [x0, y0, r0, x1, y1, r1] = this._values,
          distance = Math.hypot(x - x1, y - y1);
        offset = r1 === r0 ? 0 : (distance - r0) / (r1 - r0);
        if (x0 !== x1 || y0 !== y1) offset = Math.max(offset, 0);
      } else {
        const [start, x0, y0] = this._values;
        offset = ((Math.atan2(y - y0, x - x0) - start) / (Math.PI * 2)) % 1;
        if (offset < 0) offset += 1;
      }
      offset = Math.max(0, Math.min(1, offset));
      if (offset <= this._stops[0].offset)
        return [...this._stops[0].color];
      if (offset >= this._stops[this._stops.length - 1].offset)
        return [...this._stops[this._stops.length - 1].color];
      let left = this._stops[0],
        right = this._stops[this._stops.length - 1];
      for (let index = 1; index < this._stops.length; index++) {
        if (this._stops[index].offset >= offset) {
          left = this._stops[index - 1];
          right = this._stops[index];
          break;
        }
      }
      const span = right.offset - left.offset,
        ratio = span <= 0 ? 0 : (offset - left.offset) / span;
      return left.color.map((value, index) =>
        byte(value + (right.color[index] - value) * ratio),
      );
    }
  }

  class CanvasPattern {
    constructor(source, repetition) {
      this._state = source instanceof HTMLCanvasElement
        ? stateFor(source) : source;
      this._originClean = this._state.originClean !== false;
      this._repetition = repetition;
      this._transform = new DOMMatrix();
    }
    setTransform(transform) {
      this._transform =
        transform instanceof DOMMatrix ? transform : new DOMMatrix(transform);
    }
    _colorAt(x, y) {
      const state = this._state;
      if (!state.pixels || state.width === 0 || state.height === 0)
        return [0, 0, 0, 0];
      x = Math.floor(x - this._transform.e);
      y = Math.floor(y - this._transform.f);
      const repeatX = this._repetition === "repeat" || this._repetition === "repeat-x",
        repeatY = this._repetition === "repeat" || this._repetition === "repeat-y";
      if (!repeatX && (x < 0 || x >= state.width)) return [0, 0, 0, 0];
      if (!repeatY && (y < 0 || y >= state.height)) return [0, 0, 0, 0];
      x = ((x % state.width) + state.width) % state.width;
      y = ((y % state.height) + state.height) % state.height;
      const at = (y * state.width + x) * 4;
      return [
        state.pixels[at],
        state.pixels[at + 1],
        state.pixels[at + 2],
        state.pixels[at + 3],
      ];
    }
  }

  class DOMMatrix {
    constructor(values = [1, 0, 0, 1, 0, 0]) {
      const list =
        values &&
        typeof values === "object" &&
        !("length" in values) &&
        !values[Symbol.iterator]
          ? [values.a, values.b, values.c, values.d, values.e, values.f]
          : Array.from(values);
      this.a = Number(list[0] ?? 1);
      this.b = Number(list[1] ?? 0);
      this.c = Number(list[2] ?? 0);
      this.d = Number(list[3] ?? 1);
      this.e = Number(list[4] ?? 0);
      this.f = Number(list[5] ?? 0);
      this.is2D = true;
    }
    multiply(other) {
      other = other instanceof DOMMatrix ? other : new DOMMatrix(other);
      return new DOMMatrix([
        this.a * other.a + this.c * other.b,
        this.b * other.a + this.d * other.b,
        this.a * other.c + this.c * other.d,
        this.b * other.c + this.d * other.d,
        this.a * other.e + this.c * other.f + this.e,
        this.b * other.e + this.d * other.f + this.f,
      ]);
    }
    translate(x, y = 0) {
      return this.multiply([1, 0, 0, 1, Number(x), Number(y)]);
    }
    scale(x, y = x) {
      return this.multiply([Number(x), 0, 0, Number(y), 0, 0]);
    }
    rotate(angle) {
      const radians = (Number(angle) * Math.PI) / 180,
        cosine = Math.cos(radians),
        sine = Math.sin(radians);
      return this.multiply([cosine, sine, -sine, cosine, 0, 0]);
    }
    toFloat64Array() {
      return new Float64Array([
        this.a,
        this.b,
        0,
        0,
        this.c,
        this.d,
        0,
        0,
        0,
        0,
        1,
        0,
        this.e,
        this.f,
        0,
        1,
      ]);
    }
  }

  const pathCurrentPoint = (commands) => {
      for (let index = commands.length - 1; index >= 0; index--) {
        const command = commands[index];
        if (command[0] === "M" || command[0] === "L")
          return { x: command[1], y: command[2] };
        if (command[0] === "Q") return { x: command[3], y: command[4] };
        if (command[0] === "C") return { x: command[5], y: command[6] };
        if (command[0] === "A")
          return {
            x: command[1] + Math.cos(command[5]) * command[3],
            y: command[2] + Math.sin(command[5]) * command[3],
          };
        if (command[0] === "E")
          return {
            x: command[1] + Math.cos(command[7]) * command[3],
            y: command[2] + Math.sin(command[7]) * command[4],
          };
        if (command[0] === "R")
          return { x: command[1], y: command[2] };
      }
      return null;
    },
    normalizedRadii = (value, width, height) => {
      const list = Array.isArray(value) ? value : [value ?? 0];
      if (list.length < 1 || list.length > 4)
        throw new RangeError("roundRect radii must contain one to four values");
      const numbers = list.map((radius) => {
        if (typeof radius === "object" && radius !== null) {
          const x = Number(radius.x), y = Number(radius.y);
          if (![x, y].every(Number.isFinite) || x < 0 || y < 0)
            throw new RangeError("roundRect radius must be non-negative");
          return Math.min(x, y);
        }
        radius = Number(radius);
        if (!Number.isFinite(radius) || radius < 0)
          throw new RangeError("roundRect radius must be non-negative");
        return radius;
      });
      let radii;
      if (numbers.length === 1) radii = [numbers[0], numbers[0], numbers[0], numbers[0]];
      else if (numbers.length === 2) radii = [numbers[0], numbers[1], numbers[0], numbers[1]];
      else if (numbers.length === 3) radii = [numbers[0], numbers[1], numbers[2], numbers[1]];
      else radii = numbers;
      const maximum = Math.min(Math.abs(width), Math.abs(height)) / 2;
      return radii.map((radius) => Math.min(maximum, radius));
    },
    appendRoundRect = (push, x, y, width, height, radii) => {
      x = Number(x); y = Number(y); width = Number(width); height = Number(height);
      if (![x, y, width, height].every(Number.isFinite)) return;
      const corners = normalizedRadii(radii, width, height);
      if (width < 0) { x += width; width = -width; [corners[0], corners[1], corners[2], corners[3]] = [corners[1], corners[0], corners[3], corners[2]]; }
      if (height < 0) { y += height; height = -height; [corners[0], corners[1], corners[2], corners[3]] = [corners[3], corners[2], corners[1], corners[0]]; }
      const [tl, tr, br, bl] = corners;
      push(["M", x + tl, y]);
      push(["L", x + width - tr, y]);
      push(["A", x + width - tr, y + tr, tr, -Math.PI / 2, 0, false]);
      push(["L", x + width, y + height - br]);
      push(["A", x + width - br, y + height - br, br, 0, Math.PI / 2, false]);
      push(["L", x + bl, y + height]);
      push(["A", x + bl, y + height - bl, bl, Math.PI / 2, Math.PI, false]);
      push(["L", x, y + tl]);
      push(["A", x + tl, y + tl, tl, Math.PI, Math.PI * 1.5, false]);
      push(["Z"]);
    },
    appendArcTo = (commands, push, x1, y1, x2, y2, radius) => {
      [x1, y1, x2, y2, radius] = [x1, y1, x2, y2, radius].map(Number);
      if (radius < 0) throw new DOMException("Negative arc radius", "IndexSizeError");
      if (![x1, y1, x2, y2, radius].every(Number.isFinite)) return;
      const from = pathCurrentPoint(commands);
      if (!from) { push(["M", x1, y1]); return; }
      const first = { x: from.x - x1, y: from.y - y1 },
        second = { x: x2 - x1, y: y2 - y1 },
        firstLength = Math.hypot(first.x, first.y),
        secondLength = Math.hypot(second.x, second.y);
      if (!radius || !firstLength || !secondLength) { push(["L", x1, y1]); return; }
      first.x /= firstLength; first.y /= firstLength;
      second.x /= secondLength; second.y /= secondLength;
      const dot = Math.max(-1, Math.min(1, first.x * second.x + first.y * second.y)),
        angle = Math.acos(dot);
      if (angle < 1e-5 || Math.abs(Math.PI - angle) < 1e-5) { push(["L", x1, y1]); return; }
      const tangent = radius / Math.tan(angle / 2),
        start = { x: x1 + first.x * tangent, y: y1 + first.y * tangent },
        end = { x: x1 + second.x * tangent, y: y1 + second.y * tangent },
        cross = first.x * second.y - first.y * second.x,
        normal = cross < 0 ? { x: first.y, y: -first.x } : { x: -first.y, y: first.x },
        center = { x: start.x + normal.x * radius, y: start.y + normal.y * radius },
        startAngle = Math.atan2(start.y - center.y, start.x - center.x),
        endAngle = Math.atan2(end.y - center.y, end.x - center.x);
      push(["L", start.x, start.y]);
      push(["A", center.x, center.y, radius, startAngle, endAngle, cross > 0]);
    },
    appendSvgArc = (push, from, rx, ry, rotation, largeArc, sweep, x, y) => {
      [rx, ry, rotation, x, y] = [rx, ry, rotation, x, y].map(Number);
      rx = Math.abs(rx); ry = Math.abs(ry);
      if (![rx, ry, rotation, x, y].every(Number.isFinite)) return;
      if (!rx || !ry || (from.x === x && from.y === y)) {
        if (from.x !== x || from.y !== y) push(["L", x, y]);
        return;
      }
      const phi = rotation * Math.PI / 180,
        cosine = Math.cos(phi), sine = Math.sin(phi),
        halfX = (from.x - x) / 2, halfY = (from.y - y) / 2,
        primeX = cosine * halfX + sine * halfY,
        primeY = -sine * halfX + cosine * halfY,
        scale = primeX * primeX / (rx * rx) +
          primeY * primeY / (ry * ry);
      if (scale > 1) {
        const root = Math.sqrt(scale);
        rx *= root; ry *= root;
      }
      const rx2 = rx * rx, ry2 = ry * ry,
        numerator = Math.max(
          0, rx2 * ry2 - rx2 * primeY * primeY - ry2 * primeX * primeX,
        ),
        denominator = rx2 * primeY * primeY + ry2 * primeX * primeX,
        coefficient = (largeArc === sweep ? -1 : 1) *
          Math.sqrt(denominator ? numerator / denominator : 0),
        centerPrimeX = coefficient * rx * primeY / ry,
        centerPrimeY = -coefficient * ry * primeX / rx,
        centerX = cosine * centerPrimeX - sine * centerPrimeY +
          (from.x + x) / 2,
        centerY = sine * centerPrimeX + cosine * centerPrimeY +
          (from.y + y) / 2,
        ux = (primeX - centerPrimeX) / rx,
        uy = (primeY - centerPrimeY) / ry,
        vx = (-primeX - centerPrimeX) / rx,
        vy = (-primeY - centerPrimeY) / ry,
        start = Math.atan2(uy, ux);
      let delta = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
      if (!sweep && delta > 0) delta -= Math.PI * 2;
      else if (sweep && delta < 0) delta += Math.PI * 2;
      push(["E", centerX, centerY, rx, ry, phi, start, start + delta, !sweep]);
    },
    parseSvgPath = (text, push) => {
      text = String(text);
      if (text.length > 16 * 1024) return false;
      const tokens = [], expression =
        /[A-Za-z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g;
      let match, previousEnd = 0;
      while ((match = expression.exec(text)) !== null) {
        if (!/^[\s,]*$/.test(text.slice(previousEnd, match.index)) ||
            tokens.length >= 2048) return false;
        tokens.push(match[0]);
        previousEnd = expression.lastIndex;
      }
      if (!/^[\s,]*$/.test(text.slice(previousEnd))) return false;
      let index = 0, command = "", current = { x: 0, y: 0 },
        subpath = { x: 0, y: 0 }, cubicControl = null,
        quadraticControl = null, commands = 0;
      const isCommand = (token) => /^[A-Za-z]$/.test(token || ""),
        countFor = (letter) => ({
          M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7, Z: 0,
        })[letter.toUpperCase()],
        emit = (value) => {
          if (commands >= 256) return false;
          push(value); commands++;
          return true;
        };
      while (index < tokens.length) {
        if (isCommand(tokens[index])) command = tokens[index++];
        else if (!command) return false;
        const upper = command.toUpperCase(), relative = command !== upper,
          argumentCount = countFor(command);
        if (argumentCount === undefined) return false;
        if (upper === "Z") {
          if (!emit(["Z"])) return false;
          current = { ...subpath }; cubicControl = quadraticControl = null;
          command = "";
          continue;
        }
        if (index + argumentCount > tokens.length || isCommand(tokens[index]))
          return false;
        let first = true;
        while (index < tokens.length && !isCommand(tokens[index])) {
          if (index + argumentCount > tokens.length) return false;
          const values = tokens.slice(index, index + argumentCount).map(Number);
          if (!values.every(Number.isFinite)) return false;
          index += argumentCount;
          const point = (x, y) => ({
            x: x + (relative ? current.x : 0),
            y: y + (relative ? current.y : 0),
          });
          if (upper === "M") {
            const to = point(values[0], values[1]);
            if (!emit([first ? "M" : "L", to.x, to.y])) return false;
            current = to;
            if (first) subpath = { ...to };
          } else if (upper === "L") {
            const to = point(values[0], values[1]);
            if (!emit(["L", to.x, to.y])) return false;
            current = to;
          } else if (upper === "H") {
            const x = values[0] + (relative ? current.x : 0);
            if (!emit(["L", x, current.y])) return false;
            current = { x, y: current.y };
          } else if (upper === "V") {
            const y = values[0] + (relative ? current.y : 0);
            if (!emit(["L", current.x, y])) return false;
            current = { x: current.x, y };
          } else if (upper === "C") {
            const c1 = point(values[0], values[1]),
              c2 = point(values[2], values[3]), to = point(values[4], values[5]);
            if (!emit(["C", c1.x, c1.y, c2.x, c2.y, to.x, to.y])) return false;
            cubicControl = c2; current = to;
          } else if (upper === "S") {
            const c1 = cubicControl
                ? { x: current.x * 2 - cubicControl.x,
                    y: current.y * 2 - cubicControl.y }
                : { ...current },
              c2 = point(values[0], values[1]), to = point(values[2], values[3]);
            if (!emit(["C", c1.x, c1.y, c2.x, c2.y, to.x, to.y])) return false;
            cubicControl = c2; current = to;
          } else if (upper === "Q") {
            const control = point(values[0], values[1]),
              to = point(values[2], values[3]);
            if (!emit(["Q", control.x, control.y, to.x, to.y])) return false;
            quadraticControl = control; current = to;
          } else if (upper === "T") {
            const control = quadraticControl
                ? { x: current.x * 2 - quadraticControl.x,
                    y: current.y * 2 - quadraticControl.y }
                : { ...current },
              to = point(values[0], values[1]);
            if (!emit(["Q", control.x, control.y, to.x, to.y])) return false;
            quadraticControl = control; current = to;
          } else if (upper === "A") {
            const to = point(values[5], values[6]), before = commands;
            appendSvgArc(
              (value) => { if (commands < 256) { push(value); commands++; } },
              current, values[0], values[1], values[2],
              !!values[3], !!values[4], to.x, to.y,
            );
            if (commands === before && (current.x !== to.x || current.y !== to.y))
              return false;
            current = to;
          }
          if (upper !== "C" && upper !== "S") cubicControl = null;
          if (upper !== "Q" && upper !== "T") quadraticControl = null;
          first = false;
          if (upper === "M") command = relative ? "l" : "L";
        }
      }
      return commands !== 0;
    },
    pathCommandsToSubpaths = (commands, matrix) => {
      const subpaths = [];
      let current = null, currentSource = null;
      const transform = (point) => ({
          x: matrix[0] * point.x + matrix[2] * point.y + matrix[4],
          y: matrix[1] * point.x + matrix[3] * point.y + matrix[5],
        }),
        append = (point) => {
          if (!Number.isFinite(point.x) || !Number.isFinite(point.y)) return;
          if (!current) { current = []; subpaths.push(current); }
          current.push(transform(point));
          currentSource = point;
        };
      for (const command of commands) {
        if (command[0] === "M") {
          current = []; subpaths.push(current);
          append({ x: command[1], y: command[2] });
        } else if (command[0] === "L") {
          append({ x: command[1], y: command[2] });
        } else if (command[0] === "R") {
          const x = command[1], y = command[2],
            width = command[3], height = command[4];
          current = []; subpaths.push(current);
          for (const point of [
            { x, y }, { x: x + width, y },
            { x: x + width, y: y + height }, { x, y: y + height }, { x, y },
          ]) append(point);
        } else if (command[0] === "A") {
          let start = command[4], end = command[5];
          const anticlockwise = command[6], full = Math.PI * 2;
          if (!anticlockwise) {
            while (end < start) end += full;
            end = Math.min(end, start + full);
          } else {
            while (end > start) end -= full;
            end = Math.max(end, start - full);
          }
          const steps = Math.min(
            32, Math.max(1, Math.ceil(Math.abs(end - start) / full * 32)),
          );
          for (let step = 0; step <= steps; step++) {
            const angle = start + (end - start) * step / steps;
            append({
              x: command[1] + Math.cos(angle) * command[3],
              y: command[2] + Math.sin(angle) * command[3],
            });
          }
        } else if (command[0] === "Q") {
          const from = currentSource || { x: command[1], y: command[2] };
          for (let step = 1; step <= 16; step++) {
            const t = step / 16, inverse = 1 - t;
            append({
              x: inverse * inverse * from.x + 2 * inverse * t * command[1] +
                t * t * command[3],
              y: inverse * inverse * from.y + 2 * inverse * t * command[2] +
                t * t * command[4],
            });
          }
        } else if (command[0] === "C") {
          const from = currentSource || { x: command[1], y: command[2] };
          for (let step = 1; step <= 24; step++) {
            const t = step / 24, inverse = 1 - t;
            append({
              x: inverse ** 3 * from.x +
                3 * inverse * inverse * t * command[1] +
                3 * inverse * t * t * command[3] + t ** 3 * command[5],
              y: inverse ** 3 * from.y +
                3 * inverse * inverse * t * command[2] +
                3 * inverse * t * t * command[4] + t ** 3 * command[6],
            });
          }
        } else if (command[0] === "E") {
          let start = command[6], end = command[7];
          const anticlockwise = command[8], full = Math.PI * 2,
            cosine = Math.cos(command[5]), sine = Math.sin(command[5]);
          if (!anticlockwise) {
            while (end < start) end += full;
            end = Math.min(end, start + full);
          } else {
            while (end > start) end -= full;
            end = Math.max(end, start - full);
          }
          const steps = Math.min(
            32, Math.max(1, Math.ceil(Math.abs(end - start) / full * 32)),
          );
          for (let step = 0; step <= steps; step++) {
            const angle = start + (end - start) * step / steps,
              localX = Math.cos(angle) * command[3],
              localY = Math.sin(angle) * command[4];
            append({
              x: command[1] + localX * cosine - localY * sine,
              y: command[2] + localX * sine + localY * cosine,
            });
          }
        } else if (command[0] === "Z" && current?.length) {
          current.push({ ...current[0] });
        }
      }
      return subpaths;
    };

  class Path2D {
    constructor(path) {
      this._commands = [];
      if (path instanceof Path2D)
        this._commands = path._commands.map((command) => [...command]);
      else if (path !== undefined && path !== null)
        parseSvgPath(path, (command) => this._push(command));
    }
    _push(command) {
      if (this._commands.length < 256) this._commands.push(command);
    }
    moveTo(x, y) {
      this._push(["M", Number(x), Number(y)]);
    }
    lineTo(x, y) {
      this._push(["L", Number(x), Number(y)]);
    }
    closePath() {
      this._push(["Z"]);
    }
    rect(x, y, width, height) {
      this._push([
        "R",
        Number(x),
        Number(y),
        Number(width),
        Number(height),
      ]);
    }
    arc(x, y, radius, start, end, counterclockwise = false) {
      radius = Number(radius);
      if (radius < 0)
        throw new DOMException("Negative arc radius", "IndexSizeError");
      this._push([
        "A",
        Number(x),
        Number(y),
        radius,
        Number(start),
        Number(end),
        !!counterclockwise,
      ]);
    }
    quadraticCurveTo(controlX, controlY, x, y) {
      this._push([
        "Q",
        Number(controlX),
        Number(controlY),
        Number(x),
        Number(y),
      ]);
    }
    bezierCurveTo(controlX1, controlY1, controlX2, controlY2, x, y) {
      this._push([
        "C",
        Number(controlX1),
        Number(controlY1),
        Number(controlX2),
        Number(controlY2),
        Number(x),
        Number(y),
      ]);
    }
    ellipse(
      x,
      y,
      radiusX,
      radiusY,
      rotation,
      start,
      end,
      counterclockwise = false,
    ) {
      radiusX = Number(radiusX);
      radiusY = Number(radiusY);
      if (radiusX < 0 || radiusY < 0)
        throw new DOMException("Negative ellipse radius", "IndexSizeError");
      this._push([
        "E",
        Number(x),
        Number(y),
        radiusX,
        radiusY,
        Number(rotation),
        Number(start),
        Number(end),
        !!counterclockwise,
      ]);
    }
    roundRect(x, y, width, height, radii = 0) {
      appendRoundRect((command) => this._push(command), x, y, width, height, radii);
    }
    arcTo(x1, y1, x2, y2, radius) {
      appendArcTo(this._commands, (command) => this._push(command), x1, y1, x2, y2, radius);
    }
    addPath(path, transform = new DOMMatrix()) {
      if (!(path instanceof Path2D))
        throw new TypeError("addPath requires a Path2D");
      const matrix = transform instanceof DOMMatrix
          ? transform : new DOMMatrix(transform),
        subpaths = pathCommandsToSubpaths(
          path._commands,
          [matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f],
        );
      for (const points of subpaths) {
        if (!points.length || this._commands.length >= 256) break;
        this._push(["M", points[0].x, points[0].y]);
        for (let index = 1; index < points.length; index++)
          this._push(["L", points[index].x, points[index].y]);
      }
    }
  }

  class ImageData {
    constructor(dataOrWidth, widthOrHeight, heightOrSettings) {
      let data, width, height;
      if (dataOrWidth instanceof Uint8ClampedArray) {
        data = dataOrWidth;
        width = Math.trunc(Number(widthOrHeight));
        if (!Number.isFinite(width) || width <= 0)
          throw new DOMException("Invalid ImageData width", "IndexSizeError");
        if (heightOrSettings === undefined || typeof heightOrSettings === "object")
          height = data.length / 4 / width;
        else height = Math.trunc(Number(heightOrSettings));
        if (
          !Number.isSafeInteger(height) ||
          height <= 0 ||
          data.length !== width * height * 4
        )
          throw new DOMException(
            "ImageData dimensions do not match its data",
            "IndexSizeError",
          );
        requirePositiveSize(width, height);
      } else {
        const size = requirePositiveSize(dataOrWidth, widthOrHeight);
        width = size.width;
        height = size.height;
        data = new Uint8ClampedArray(width * height * 4);
      }
      Object.defineProperties(this, {
        data: { enumerable: true, value: data },
        width: { enumerable: true, value: width },
        height: { enumerable: true, value: height },
        colorSpace: { enumerable: true, value: "srgb" },
      });
    }
  }

  class CanvasRenderingContext2D {
    constructor(canvas) {
      if (!(canvas instanceof HTMLCanvasElement))
        throw new TypeError("CanvasRenderingContext2D requires a canvas");
      Object.defineProperty(this, "canvas", {
        enumerable: true,
        value: canvas,
      });
    }
    get fillStyle() {
      return stateFor(this.canvas).fillStyle;
    }
    set fillStyle(value) {
      if (value instanceof CanvasGradient || value instanceof CanvasPattern) {
        const state = stateFor(this.canvas);
        state.fillPaint = value;
        state.fillStyle = value;
        return;
      }
      const parsed = parseColor(value);
      if (!parsed) return;
      const state = stateFor(this.canvas);
      state.fill = parsed.components;
      state.fillStyle = parsed.serialized;
      state.fillPaint = null;
    }
    get strokeStyle() {
      return stateFor(this.canvas).strokeStyle;
    }
    set strokeStyle(value) {
      const parsed = parseColor(value);
      if (!parsed) return;
      const state = stateFor(this.canvas);
      state.stroke = parsed.components;
      state.strokeStyle = parsed.serialized;
    }
    get shadowColor() {
      return stateFor(this.canvas).shadowColor;
    }
    set shadowColor(value) {
      const parsed = parseColor(value);
      if (!parsed) return;
      const state = stateFor(this.canvas);
      state.shadow = parsed.components;
      state.shadowColor = parsed.serialized;
    }
    get shadowBlur() {
      return stateFor(this.canvas).shadowBlur;
    }
    set shadowBlur(value) {
      value = Number(value);
      if (Number.isFinite(value) && value >= 0)
        stateFor(this.canvas).shadowBlur = Math.min(12, value);
    }
    get shadowOffsetX() {
      return stateFor(this.canvas).shadowOffsetX;
    }
    set shadowOffsetX(value) {
      value = Number(value);
      if (Number.isFinite(value))
        stateFor(this.canvas).shadowOffsetX = Math.max(-256, Math.min(256, value));
    }
    get shadowOffsetY() {
      return stateFor(this.canvas).shadowOffsetY;
    }
    set shadowOffsetY(value) {
      value = Number(value);
      if (Number.isFinite(value))
        stateFor(this.canvas).shadowOffsetY = Math.max(-256, Math.min(256, value));
    }
    get globalAlpha() {
      return stateFor(this.canvas).globalAlpha;
    }
    set globalAlpha(value) {
      value = Number(value);
      if (Number.isFinite(value) && value >= 0 && value <= 1)
        stateFor(this.canvas).globalAlpha = value;
    }
    get globalCompositeOperation() {
      return stateFor(this.canvas).globalCompositeOperation;
    }
    set globalCompositeOperation(value) {
      value = String(value);
      if ([
        "source-over", "source-in", "source-out", "source-atop",
        "destination-over", "destination-in", "destination-out",
        "destination-atop", "copy", "xor", "lighter",
      ].includes(value)) stateFor(this.canvas).globalCompositeOperation = value;
    }
    get lineWidth() {
      return stateFor(this.canvas).lineWidth;
    }
    set lineWidth(value) {
      value = Number(value);
      if (Number.isFinite(value) && value > 0)
        stateFor(this.canvas).lineWidth = Math.min(16, value);
    }
    get lineCap() {
      return stateFor(this.canvas).lineCap;
    }
    set lineCap(value) {
      value = String(value);
      if (["butt", "round", "square"].includes(value))
        stateFor(this.canvas).lineCap = value;
    }
    get lineJoin() {
      return stateFor(this.canvas).lineJoin;
    }
    set lineJoin(value) {
      value = String(value);
      if (["round", "bevel", "miter"].includes(value))
        stateFor(this.canvas).lineJoin = value;
    }
    get miterLimit() {
      return stateFor(this.canvas).miterLimit;
    }
    set miterLimit(value) {
      value = Number(value);
      if (Number.isFinite(value) && value > 0)
        stateFor(this.canvas).miterLimit = Math.min(64, value);
    }
    get font() {
      return stateFor(this.canvas).font;
    }
    set font(value) {
      value = String(value);
      if (
        value.length <= 128 &&
        /(?:^|\s)(?:[1-9][0-9]*(?:\.[0-9]+)?)px(?:\s|\/|$)/.test(value)
      )
        stateFor(this.canvas).font = value;
    }
    get textAlign() {
      return stateFor(this.canvas).textAlign;
    }
    set textAlign(value) {
      value = String(value);
      if (["start", "end", "left", "right", "center"].includes(value))
        stateFor(this.canvas).textAlign = value;
    }
    get textBaseline() {
      return stateFor(this.canvas).textBaseline;
    }
    set textBaseline(value) {
      value = String(value);
      if (
        ["top", "hanging", "middle", "alphabetic", "ideographic", "bottom"].includes(
          value,
        )
      )
        stateFor(this.canvas).textBaseline = value;
    }
    get direction() {
      return stateFor(this.canvas).direction;
    }
    set direction(value) {
      value = String(value);
      if (["ltr", "rtl", "inherit"].includes(value))
        stateFor(this.canvas).direction = value;
    }
    get imageSmoothingEnabled() {
      return stateFor(this.canvas).imageSmoothingEnabled;
    }
    set imageSmoothingEnabled(value) {
      stateFor(this.canvas).imageSmoothingEnabled = !!value;
    }
    get imageSmoothingQuality() {
      return stateFor(this.canvas).imageSmoothingQuality;
    }
    set imageSmoothingQuality(value) {
      value = String(value);
      if (["low", "medium", "high"].includes(value))
        stateFor(this.canvas).imageSmoothingQuality = value;
    }
    save() {
      const state = stateFor(this.canvas);
      if (state.stack.length < 16)
        state.stack.push({
          fill: [...state.fill],
          fillStyle: state.fillStyle,
          fillPaint: state.fillPaint,
          stroke: [...state.stroke],
          strokeStyle: state.strokeStyle,
          shadow: [...state.shadow],
          shadowColor: state.shadowColor,
          shadowBlur: state.shadowBlur,
          shadowOffsetX: state.shadowOffsetX,
          shadowOffsetY: state.shadowOffsetY,
          globalAlpha: state.globalAlpha,
          globalCompositeOperation: state.globalCompositeOperation,
          lineWidth: state.lineWidth,
          lineCap: state.lineCap,
          lineJoin: state.lineJoin,
          miterLimit: state.miterLimit,
          font: state.font,
          textAlign: state.textAlign,
          textBaseline: state.textBaseline,
          direction: state.direction,
          imageSmoothingEnabled: state.imageSmoothingEnabled,
          imageSmoothingQuality: state.imageSmoothingQuality,
          transform: [...state.transform],
          clipRect: [...state.clipRect],
          clipPaths: state.clipPaths.map((clip) => ({
            evenOdd: clip.evenOdd,
            subpaths: clip.subpaths.map((points) =>
              points.map((point) => ({ ...point }))),
          })),
          lineDash: [...state.lineDash],
          lineDashOffset: state.lineDashOffset,
        });
      else
        state.ignoredSaveDepth++;
    }
    restore() {
      const state = stateFor(this.canvas);
      if (state.ignoredSaveDepth > 0) {
        state.ignoredSaveDepth--;
        return;
      }
      const
        saved = state.stack.pop();
      if (!saved) return;
      state.fill = saved.fill;
      state.fillStyle = saved.fillStyle;
      state.fillPaint = saved.fillPaint;
      state.stroke = saved.stroke;
      state.strokeStyle = saved.strokeStyle;
      state.shadow = saved.shadow;
      state.shadowColor = saved.shadowColor;
      state.shadowBlur = saved.shadowBlur;
      state.shadowOffsetX = saved.shadowOffsetX;
      state.shadowOffsetY = saved.shadowOffsetY;
      state.globalAlpha = saved.globalAlpha;
      state.globalCompositeOperation = saved.globalCompositeOperation;
      state.lineWidth = saved.lineWidth;
      state.lineCap = saved.lineCap;
      state.lineJoin = saved.lineJoin;
      state.miterLimit = saved.miterLimit;
      state.font = saved.font;
      state.textAlign = saved.textAlign;
      state.textBaseline = saved.textBaseline;
      state.direction = saved.direction;
      state.imageSmoothingEnabled = saved.imageSmoothingEnabled;
      state.imageSmoothingQuality = saved.imageSmoothingQuality;
      state.transform = saved.transform;
      state.clipRect = saved.clipRect;
      state.clipPaths = saved.clipPaths;
      state.lineDash = saved.lineDash;
      state.lineDashOffset = saved.lineDashOffset;
    }
    clearRect(x, y, width, height) {
      const state = stateFor(this.canvas);
      if (!state.pixels) return;
      if (!flushRectCommands(state)) return;
      if (state.clipPaths.length || state.transform.some(
        (value, index) => value !== [1, 0, 0, 1, 0, 0][index],
      )) {
        const path = new Path2D();
        path.rect(x, y, width, height);
        const subpaths = this._subpaths(path),
          dirty = canvasDirtyBounds(state, subpaths, 1);
        if (__tilefinchCanvasRasterPath(
          state.pixels, state.width, state.height, flattenedPath(subpaths),
          true, false, 0, 0, 0, 0, 1, 2,
          1, 0, 0, 10, new Float64Array(), 0,
          new Float64Array(state.clipRect), new Float64Array(),
          serializedClips(state),
        )) scheduleCanvasCommit(state, dirty);
        return;
      }
      const rect = normalizedRect(state, x, y, width, height);
      if (!rect) return;
      if (!__tilefinchCanvasRasterRect(
        state.pixels, state.width, state.height, rect.left, rect.top,
        rect.right, rect.bottom, 0, 0, 0, 0, 1, 0,
      )) return;
      scheduleCanvasCommit(state, rect);
    }
    fillRect(x, y, width, height) {
      const state = stateFor(this.canvas);
      if (state.imageCommands.length && !flushImageCommands(state)) return;
      if (state.fillPaint || shadowActive(state)) {
        const path = new Path2D();
        path.rect(x, y, width, height);
        this.fill(path);
        return;
      }
      if (
        state.transform.some(
          (value, index) => value !== [1, 0, 0, 1, 0, 0][index],
        )
      ) {
        const path = new Path2D();
        path.rect(x, y, width, height);
        this.fill(path);
        return;
      }
      const
        rect = normalizedRect(state, x, y, width, height),
        pixels = rect && ensureSurface(state);
      if (!rect || !pixels) return;
      const red = state.fill[0],
        green = state.fill[1],
        blue = state.fill[2],
        sourceAlpha = (state.fill[3] / 255) * state.globalAlpha;
      if (sourceAlpha <= 0 && state.globalCompositeOperation === "source-over")
        return;
      if (state.clipPaths.length) {
        if (!flushRectCommands(state)) return;
        for (let row = rect.top; row < rect.bottom; row++)
          for (let column = rect.left; column < rect.right; column++)
            blendPixel(state, column, row, state.fill);
      } else {
        state.rectCommands.push([
          rect.left, rect.top, rect.right, rect.bottom,
          red, green, blue, state.fill[3], state.globalAlpha,
          compositeOperations.indexOf(state.globalCompositeOperation),
        ]);
        canvasDiagnostics.rectangleCommands++;
        if (state.rectCommands.length >= 64 && !flushRectCommands(state)) return;
      }
      scheduleCanvasCommit(state, rect);
    }
    strokeRect(x, y, width, height) {
      const path = new Path2D();
      path.rect(x, y, width, height);
      this.stroke(path);
    }
    beginPath() {
      const state = stateFor(this.canvas);
      state.path = [];
      state.subpath = null;
    }
    moveTo(x, y) {
      const state = stateFor(this.canvas);
      if (state.path.length < 256) state.path.push(["M", Number(x), Number(y)]);
    }
    lineTo(x, y) {
      const state = stateFor(this.canvas);
      if (state.path.length < 256) state.path.push(["L", Number(x), Number(y)]);
    }
    closePath() {
      const state = stateFor(this.canvas);
      if (state.path.length < 256) state.path.push(["Z"]);
    }
    rect(x, y, width, height) {
      const state = stateFor(this.canvas);
      if (state.path.length < 256)
        state.path.push([
          "R",
          Number(x),
          Number(y),
          Number(width),
          Number(height),
        ]);
    }
    arc(x, y, radius, startAngle, endAngle, counterclockwise = false) {
      radius = Number(radius);
      if (radius < 0)
        throw new DOMException("Negative arc radius", "IndexSizeError");
      const state = stateFor(this.canvas);
      if (state.path.length < 256)
        state.path.push([
          "A",
          Number(x),
          Number(y),
          radius,
          Number(startAngle),
          Number(endAngle),
          !!counterclockwise,
        ]);
    }
    roundRect(x, y, width, height, radii = 0) {
      const state = stateFor(this.canvas);
      appendRoundRect(
        (command) => {
          if (state.path.length < 256) state.path.push(command);
        },
        x, y, width, height, radii,
      );
    }
    arcTo(x1, y1, x2, y2, radius) {
      const state = stateFor(this.canvas);
      appendArcTo(
        state.path,
        (command) => {
          if (state.path.length < 256) state.path.push(command);
        },
        x1, y1, x2, y2, radius,
      );
    }
    quadraticCurveTo(controlX, controlY, x, y) {
      const state = stateFor(this.canvas);
      if (state.path.length < 256)
        state.path.push([
          "Q",
          Number(controlX),
          Number(controlY),
          Number(x),
          Number(y),
        ]);
    }
    bezierCurveTo(controlX1, controlY1, controlX2, controlY2, x, y) {
      const state = stateFor(this.canvas);
      if (state.path.length < 256)
        state.path.push([
          "C",
          Number(controlX1),
          Number(controlY1),
          Number(controlX2),
          Number(controlY2),
          Number(x),
          Number(y),
        ]);
    }
    ellipse(
      x,
      y,
      radiusX,
      radiusY,
      rotation,
      startAngle,
      endAngle,
      counterclockwise = false,
    ) {
      radiusX = Number(radiusX);
      radiusY = Number(radiusY);
      if (radiusX < 0 || radiusY < 0)
        throw new DOMException("Negative ellipse radius", "IndexSizeError");
      const state = stateFor(this.canvas);
      if (state.path.length < 256)
        state.path.push([
          "E",
          Number(x),
          Number(y),
          radiusX,
          radiusY,
          Number(rotation),
          Number(startAngle),
          Number(endAngle),
          !!counterclockwise,
        ]);
    }
    _subpaths(path) {
      const state = stateFor(this.canvas),
        commands = path instanceof Path2D ? path._commands : state.path;
      return pathCommandsToSubpaths(commands, state.transform);
    }
    fill(pathOrRule, rule) {
      const path = pathOrRule instanceof Path2D ? pathOrRule : null,
        fillRule = path
          ? String(rule || "nonzero")
          : String(pathOrRule || "nonzero"),
        state = stateFor(this.canvas),
        subpaths = this._subpaths(path),
        pixels = ensureSurface(state);
      if (!pixels || !subpaths.length) return;
      if ((state.rectCommands.length || state.imageCommands.length) &&
          !flushRectCommands(state)) return;
      const flattened = flattenedPath(subpaths),
        dirty = canvasDirtyBounds(state, subpaths, 1 + shadowPadding(state)),
        evenOdd = fillRule === "evenodd";
      if (state.fillPaint instanceof CanvasPattern) {
        if (!flushPaintCommands(state)) return;
        let left = state.width, top = state.height, right = 0, bottom = 0;
        for (const points of subpaths)
          for (const point of points) {
            left = Math.min(left, Math.floor(point.x));
            top = Math.min(top, Math.floor(point.y));
            right = Math.max(right, Math.ceil(point.x));
            bottom = Math.max(bottom, Math.ceil(point.y));
          }
        left = Math.max(left, state.clipRect[0]);
        top = Math.max(top, state.clipRect[1]);
        right = Math.min(right, state.clipRect[2]);
        bottom = Math.min(bottom, state.clipRect[3]);
        for (let y = top; y < bottom; y++)
          for (let x = left; x < right; x++)
            if (pointInSubpaths(subpaths, x + 0.5, y + 0.5, fillRule === "evenodd"))
              blendPixel(
                state, x, y,
                state.fillPaint ? state.fillPaint._colorAt(x + 0.5, y + 0.5) : state.fill,
              );
      } else {
        const args = [
          flattened, true, evenOdd, ...state.fill, state.globalAlpha,
          compositeOperations.indexOf(state.globalCompositeOperation),
          1, 0, 0, 10, new Float64Array(), 0,
          new Float64Array(state.clipRect), serializedPaint(state.fillPaint),
          serializedClips(state),
        ];
        if (!queuePathShadows(state, args)) return;
        if (!queuePaintCommand(state, {
          kind: 0,
          args,
          fallback: {
            fill: true,
            evenOdd,
            subpaths,
            color: [...state.fill],
            paint: state.fillPaint,
            globalAlpha: state.globalAlpha,
            operation: state.globalCompositeOperation,
            clipRect: [...state.clipRect],
            clipPaths: state.clipPaths.map((clip) => ({
              evenOdd: clip.evenOdd,
              subpaths: clip.subpaths.map((points) =>
                points.map((point) => ({ ...point }))),
            })),
          },
        })) return;
      }
      scheduleCanvasCommit(state, dirty);
    }
    stroke(path) {
      const state = stateFor(this.canvas),
        subpaths = this._subpaths(path),
        pixels = ensureSurface(state),
        lineScale = transformedLineScale(state),
        lineWidth = state.lineWidth * lineScale;
      if (!pixels || !subpaths.length) return;
      if ((state.rectCommands.length || state.imageCommands.length) &&
          !flushRectCommands(state)) return;
      const args = [
        flattenedPath(subpaths), false, false, ...state.stroke,
        state.globalAlpha,
        compositeOperations.indexOf(state.globalCompositeOperation),
        lineWidth,
        ["butt", "round", "square"].indexOf(state.lineCap),
        ["miter", "round", "bevel"].indexOf(state.lineJoin),
        state.miterLimit,
        new Float64Array(state.lineDash.map((value) => value * lineScale)),
        state.lineDashOffset * lineScale,
        new Float64Array(state.clipRect), new Float64Array(),
        serializedClips(state),
      ];
      if (!queuePathShadows(state, args)) return;
      if (!queuePaintCommand(state, {
        kind: 0,
        args,
        fallback: {
          fill: false,
          subpaths,
          color: [...state.stroke],
          paint: null,
          globalAlpha: state.globalAlpha,
          operation: state.globalCompositeOperation,
          lineWidth,
          clipRect: [...state.clipRect],
          clipPaths: state.clipPaths.map((clip) => ({
            evenOdd: clip.evenOdd,
            subpaths: clip.subpaths.map((points) =>
              points.map((point) => ({ ...point }))),
          })),
        },
      })) return;
      const strokePadding = lineWidth / 2 *
        (state.lineJoin === "miter" ? state.miterLimit : 1) + 1 +
        shadowPadding(state);
      scheduleCanvasCommit(
        state, canvasDirtyBounds(state, subpaths, strokePadding),
      );
    }
    clip(pathOrRule, rule) {
      const path = pathOrRule instanceof Path2D ? pathOrRule : null,
        fillRule = path
          ? String(rule || "nonzero")
          : String(pathOrRule || "nonzero"),
        state = stateFor(this.canvas),
        subpaths = this._subpaths(path);
      if (!subpaths.length) {
        state.clipRect = [0, 0, 0, 0];
        state.clipPaths = [];
        return;
      }
      let left = state.width, top = state.height, right = 0, bottom = 0;
      for (const points of subpaths)
        for (const point of points) {
          left = Math.min(left, point.x);
          top = Math.min(top, point.y);
          right = Math.max(right, point.x);
          bottom = Math.max(bottom, point.y);
        }
      state.clipRect = [
        Math.max(state.clipRect[0], Math.floor(left)),
        Math.max(state.clipRect[1], Math.floor(top)),
        Math.min(state.clipRect[2], Math.ceil(right)),
        Math.min(state.clipRect[3], Math.ceil(bottom)),
      ];
      const rectangle = subpaths.length === 1 && subpaths[0].length >= 4 &&
        subpaths[0].every((point) =>
          (point.x === left || point.x === right) &&
          (point.y === top || point.y === bottom));
      if (!rectangle) {
        if (state.clipPaths.length < 4)
          state.clipPaths.push({
            subpaths,
            evenOdd: fillRule === "evenodd",
          });
        else {
          /* Never broaden a fifth unrepresentable clip. Emptying the bounded
             clip is a safe, visible degradation and keeps later drawing from
             escaping a limit the page asked us to enforce. */
          state.clipRect = [0, 0, 0, 0];
          state.clipPaths = [];
        }
      }
    }
    isPointInPath(pathOrX, xOrY, yOrRule, maybeRule) {
      const path = pathOrX instanceof Path2D ? pathOrX : null,
        x = Number(path ? xOrY : pathOrX),
        y = Number(path ? yOrRule : xOrY),
        fillRule = String(path ? maybeRule || "nonzero" : yOrRule || "nonzero");
      return pointInSubpaths(this._subpaths(path), x, y, fillRule === "evenodd");
    }
    isPointInStroke(pathOrX, xOrY, maybeY) {
      const path = pathOrX instanceof Path2D ? pathOrX : null,
        point = {
          x: Number(path ? xOrY : pathOrX),
          y: Number(path ? maybeY : xOrY),
        },
        state = stateFor(this.canvas),
        scale = transformedLineScale(state),
        radius = state.lineWidth * scale / 2,
        dash = state.lineDash.map((value) => value * scale),
        dashOffset = state.lineDashOffset * scale;
      for (const points of this._subpaths(path)) {
        let distanceAlong = 0;
        const closed = points.length >= 3 &&
          points[0].x === points[points.length - 1].x &&
          points[0].y === points[points.length - 1].y;
        for (let index = 1; index < points.length; index++) {
          const projection = pointSegmentProjection(
              point, points[index - 1], points[index]),
            first = !closed && index === 1,
            last = !closed && index + 1 === points.length;
          let inside = projection.position >= 0 && projection.position <= 1 &&
            projection.distance <= radius;
          if (!inside && state.lineCap === "round" &&
              ((projection.position < 0 && first) ||
               (projection.position > 1 && last)))
            inside = projection.distance <= radius;
          if (!inside && state.lineCap === "square" && projection.length > 0 &&
              ((projection.position < 0 && first) ||
               (projection.position > 1 && last))) {
            const extension = radius / projection.length;
            inside = projection.perpendicular <= radius &&
              projection.position >= -extension &&
              projection.position <= 1 + extension;
          }
          if (inside && dashContains(
            dash, dashOffset,
            distanceAlong + Math.max(0, Math.min(1, projection.position)) *
              projection.length,
          )) return true;
          distanceAlong += projection.length;
        }
        if (state.lineJoin === "round")
          for (let index = 1; index + 1 < points.length; index++)
            if (Math.hypot(point.x - points[index].x,
                           point.y - points[index].y) <= radius) return true;
      }
      return false;
    }
    setLineDash(segments) {
      const values = Array.from(segments, Number);
      if (
        values.length > 32 ||
        values.some((value) => !Number.isFinite(value) || value < 0)
      )
        throw new TypeError("Invalid line dash");
      if (values.length % 2) values.push(...values);
      stateFor(this.canvas).lineDash = values;
    }
    getLineDash() {
      return [...stateFor(this.canvas).lineDash];
    }
    get lineDashOffset() {
      return stateFor(this.canvas).lineDashOffset;
    }
    set lineDashOffset(value) {
      value = Number(value);
      if (Number.isFinite(value)) stateFor(this.canvas).lineDashOffset = value;
    }
    getTransform() {
      return new DOMMatrix(stateFor(this.canvas).transform);
    }
    setTransform(a, b, c, d, e, f) {
      const state = stateFor(this.canvas);
      if (typeof a === "object") {
        const matrix = a instanceof DOMMatrix ? a : new DOMMatrix([
          a.a,
          a.b,
          a.c,
          a.d,
          a.e,
          a.f,
        ]);
        state.transform = [
          matrix.a,
          matrix.b,
          matrix.c,
          matrix.d,
          matrix.e,
          matrix.f,
        ];
        return;
      }
      const values = [a, b, c, d, e, f].map(Number);
      if (values.every(Number.isFinite)) state.transform = values;
    }
    resetTransform() {
      stateFor(this.canvas).transform = [1, 0, 0, 1, 0, 0];
    }
    transform(a, b, c, d, e, f) {
      const state = stateFor(this.canvas),
        result = new DOMMatrix(state.transform).multiply([
          a,
          b,
          c,
          d,
          e,
          f,
        ]);
      state.transform = [
        result.a,
        result.b,
        result.c,
        result.d,
        result.e,
        result.f,
      ];
    }
    translate(x, y) {
      this.transform(1, 0, 0, 1, Number(x), Number(y));
    }
    scale(x, y = x) {
      this.transform(Number(x), 0, 0, Number(y), 0, 0);
    }
    rotate(angle) {
      const cosine = Math.cos(Number(angle)),
        sine = Math.sin(Number(angle));
      this.transform(cosine, sine, -sine, cosine, 0, 0);
    }
    measureText(text) {
      text = String(text);
      const state = stateFor(this.canvas),
        spec = canvasFontSpec(state.font),
        native = __tilefinchCanvasMeasureText(
          text.slice(0, 1024), spec.size, spec.family, spec.bold, spec.italic,
        ),
        width = Number(native?.width ?? text.length * spec.size * 0.6),
        ascent = Number(native?.ascent ?? spec.size * 0.8),
        descent = Number(native?.descent ?? spec.size * 0.2);
      return {
        width,
        actualBoundingBoxLeft: 0,
        actualBoundingBoxRight: width,
        actualBoundingBoxAscent: ascent,
        actualBoundingBoxDescent: descent,
        fontBoundingBoxAscent: ascent,
        fontBoundingBoxDescent: descent,
      };
    }
    fillText(text, x, y, maximumWidth) {
      text = String(text).slice(0, 256);
      const state = stateFor(this.canvas),
        metrics = this.measureText(text),
        spec = canvasFontSpec(state.font),
        limit =
          maximumWidth === undefined
            ? metrics.width
            : Math.max(0, Number(maximumWidth)),
        scale = metrics.width > limit && metrics.width > 0
          ? limit / metrics.width : 1,
        pixels = ensureSurface(state);
      if (!pixels || !Number.isFinite(limit) || limit <= 0) return;
      if ((state.rectCommands.length || state.imageCommands.length) &&
          !flushRectCommands(state)) return;
      x = Number(x);
      y = Number(y);
      const alignRight = state.textAlign === "right" ||
        (state.textAlign === "end" && state.direction !== "rtl") ||
        (state.textAlign === "start" && state.direction === "rtl");
      if (state.textAlign === "center") x -= Math.min(metrics.width, limit) / 2;
      else if (alignRight)
        x -= Math.min(metrics.width, limit);
      if (state.textBaseline === "top") y += metrics.actualBoundingBoxAscent;
      else if (state.textBaseline === "hanging")
        y += metrics.actualBoundingBoxAscent * 0.8;
      else if (state.textBaseline === "middle")
        y += (metrics.actualBoundingBoxAscent - metrics.actualBoundingBoxDescent) / 2;
      else if (["bottom", "ideographic"].includes(state.textBaseline))
        y -= metrics.actualBoundingBoxDescent;
      const color = state.fillPaint
          ? state.fillPaint._colorAt(x, y) : state.fill,
        textWidth = Math.min(metrics.width, limit),
        dirty = canvasDirtyBounds(state, [[
          transformPoint(state, x, y - metrics.actualBoundingBoxAscent),
          transformPoint(state, x + textWidth, y - metrics.actualBoundingBoxAscent),
          transformPoint(state, x + textWidth, y + metrics.actualBoundingBoxDescent),
          transformPoint(state, x, y + metrics.actualBoundingBoxDescent),
        ]], 1 + shadowPadding(state)),
        args = [
          text, x, y, spec.size, spec.family, spec.bold, spec.italic,
          ...color, state.globalAlpha,
          compositeOperations.indexOf(state.globalCompositeOperation),
          false, 0, new Float64Array(state.transform),
          new Float64Array(state.clipRect), scale, serializedClips(state),
        ];
      if (queueTextShadows(state, args) &&
          queuePaintCommand(state, { kind: 1, args }))
        scheduleCanvasCommit(state, dirty);
    }
    strokeText(text, x, y, maximumWidth) {
      text = String(text).slice(0, 256);
      const state = stateFor(this.canvas),
        metrics = this.measureText(text),
        spec = canvasFontSpec(state.font),
        limit = maximumWidth === undefined
          ? metrics.width : Math.max(0, Number(maximumWidth)),
        scale = metrics.width > limit && metrics.width > 0
          ? limit / metrics.width : 1,
        pixels = ensureSurface(state);
      if (!pixels || !Number.isFinite(limit) || limit <= 0) return;
      if ((state.rectCommands.length || state.imageCommands.length) &&
          !flushRectCommands(state)) return;
      x = Number(x); y = Number(y);
      const alignRight = state.textAlign === "right" ||
        (state.textAlign === "end" && state.direction !== "rtl") ||
        (state.textAlign === "start" && state.direction === "rtl");
      if (state.textAlign === "center") x -= Math.min(metrics.width, limit) / 2;
      else if (alignRight)
        x -= Math.min(metrics.width, limit);
      if (state.textBaseline === "top") y += metrics.actualBoundingBoxAscent;
      else if (state.textBaseline === "hanging")
        y += metrics.actualBoundingBoxAscent * 0.8;
      else if (state.textBaseline === "middle")
        y += (metrics.actualBoundingBoxAscent - metrics.actualBoundingBoxDescent) / 2;
      else if (["bottom", "ideographic"].includes(state.textBaseline))
        y -= metrics.actualBoundingBoxDescent;
      const textWidth = Math.min(metrics.width, limit),
        dirty = canvasDirtyBounds(state, [[
          transformPoint(state, x, y - metrics.actualBoundingBoxAscent),
          transformPoint(state, x + textWidth, y - metrics.actualBoundingBoxAscent),
          transformPoint(state, x + textWidth, y + metrics.actualBoundingBoxDescent),
          transformPoint(state, x, y + metrics.actualBoundingBoxDescent),
        ]], Math.min(16, state.lineWidth) / 2 + 1 + shadowPadding(state));
      const args = [
        text, x, y, spec.size, spec.family, spec.bold, spec.italic,
        ...state.stroke, state.globalAlpha,
        compositeOperations.indexOf(state.globalCompositeOperation),
        true, state.lineWidth, new Float64Array(state.transform),
        new Float64Array(state.clipRect), scale, serializedClips(state),
      ];
      if (queueTextShadows(state, args) &&
          queuePaintCommand(state, { kind: 1, args }))
        scheduleCanvasCommit(state, dirty);
    }
    drawImage(source, ...arguments_) {
      let sourceState = null;
      if (source instanceof HTMLCanvasElement) sourceState = stateFor(source);
      else if (
        typeof HTMLImageElement === "function" &&
        source instanceof HTMLImageElement
      ) {
        const sourceUrl = String(source.currentSrc || source.src || "");
        let snapshot = source === cachedImageSource &&
            sourceUrl === cachedImageUrl ? cachedImageSnapshot : null;
        if (!snapshot) snapshot = __tilefinchCanvasImageSource(source.__handle);
        if (!snapshot) return;
        if (snapshot !== cachedImageSnapshot) {
          snapshot = {
            width: Number(snapshot.width),
            height: Number(snapshot.height),
            pixels: new Uint8ClampedArray(snapshot.pixels),
            originClean: snapshot.sameOrigin !== false,
          };
          cachedImageSource = source;
          cachedImageUrl = sourceUrl;
          cachedImageSnapshot = snapshot;
        }
        sourceState = snapshot;
      } else
        throw new DOMException(
          "The image source is not supported by this bounded backend",
          "NotSupportedError",
        );
      const sourcePixels = sourceState.pixels;
      if (!sourcePixels) return;
      let sx = 0,
        sy = 0,
        sw = sourceState.width,
        sh = sourceState.height,
        dx,
        dy,
        dw,
        dh;
      if (arguments_.length === 2) {
        [dx, dy] = arguments_;
        dw = sw;
        dh = sh;
      } else if (arguments_.length === 4) {
        [dx, dy, dw, dh] = arguments_;
      } else if (arguments_.length === 8) {
        [sx, sy, sw, sh, dx, dy, dw, dh] = arguments_;
      } else throw new TypeError("Invalid drawImage arguments");
      [sx, sy, sw, sh, dx, dy, dw, dh] = [
        sx,
        sy,
        sw,
        sh,
        dx,
        dy,
        dw,
        dh,
      ].map(Number);
      if (![sx, sy, sw, sh, dx, dy, dw, dh].every(Number.isFinite)) return;
      const target = stateFor(this.canvas);
      if (sourceState.originClean === false) target.originClean = false;
      if (target.rectCommands.length && !flushRectCommands(target)) return;
      if (source instanceof HTMLCanvasElement &&
          !flushRectCommands(sourceState)) return;
      const
        retainedSourcePixels =
          source instanceof HTMLCanvasElement && sourceState === target
            ? sourcePixels.slice() : sourcePixels,
        targetPixels = ensureSurface(target),
        destination = [[
          transformPoint(target, dx, dy),
          transformPoint(target, dx + dw, dy),
          transformPoint(target, dx + dw, dy + dh),
          transformPoint(target, dx, dy + dh),
        ]],
        dirty = canvasDirtyBounds(target, destination, 1);
      if (!targetPixels || !sw || !sh || !dw || !dh) return;
      const operation = compositeOperations.indexOf(
        target.globalCompositeOperation,
      );
      if (!target.clipPaths.length) {
        if (target.imageCommands.length >= 16 && !flushImageCommands(target))
          return;
        const sourceAlreadyQueued = target.imageCommands.some(
          (command) => command.source === retainedSourcePixels,
        );
        const sourceBytes = sourceAlreadyQueued ? 0 : retainedSourcePixels.byteLength;
        if (sourceBytes > pixelByteLimit ||
            (sourceBytes > pixelByteLimit - target.imageCommandBytes &&
             !flushImageCommands(target))) return;
        target.imageCommands.push({
          source: retainedSourcePixels,
          sourceWidth: sourceState.width,
          sourceHeight: sourceState.height,
          sx, sy, sw, sh, dx, dy, dw, dh,
          smooth: target.imageSmoothingEnabled,
          globalAlpha: target.globalAlpha,
          operation,
          transform: [...target.transform],
          clip: [...target.clipRect],
        });
        target.imageCommandBytes += sourceBytes;
        canvasDiagnostics.imageCommands++;
        scheduleCanvasCommit(target, dirty);
        return;
      }
      if (!flushImageCommands(target)) return;
      if (__tilefinchCanvasRasterImage(
        targetPixels, target.width, target.height,
        retainedSourcePixels, sourceState.width, sourceState.height,
        sx, sy, sw, sh, dx, dy, dw, dh,
        target.imageSmoothingEnabled, target.globalAlpha, operation,
        new Float64Array(target.transform), new Float64Array(target.clipRect),
        serializedClips(target),
      )) {
        canvasDiagnostics.imageRasters++;
        scheduleCanvasCommit(target, dirty);
      }
    }
    createLinearGradient(x0, y0, x1, y1) {
      return new CanvasGradient("linear", [x0, y0, x1, y1]);
    }
    createRadialGradient(x0, y0, r0, x1, y1, r1) {
      if (Number(r0) < 0 || Number(r1) < 0)
        throw new DOMException("Negative gradient radius", "IndexSizeError");
      return new CanvasGradient("radial", [x0, y0, r0, x1, y1, r1]);
    }
    createConicGradient(startAngle, x, y) {
      return new CanvasGradient("conic", [startAngle, x, y]);
    }
    createPattern(source, repetition = "repeat") {
      repetition = repetition === "" ? "repeat" : String(repetition);
      if (!["repeat", "repeat-x", "repeat-y", "no-repeat"].includes(repetition))
        throw new DOMException("Invalid pattern repetition", "SyntaxError");
      let retained = source;
      if (source instanceof HTMLCanvasElement &&
          !flushRectCommands(stateFor(source))) return null;
      if (
        !(source instanceof HTMLCanvasElement) &&
        typeof HTMLImageElement === "function" &&
        source instanceof HTMLImageElement
      ) {
        const snapshot = __tilefinchCanvasImageSource(source.__handle);
        if (!snapshot) return null;
        retained = {
          width: Number(snapshot.width),
          height: Number(snapshot.height),
          pixels: new Uint8ClampedArray(snapshot.pixels),
          originClean: snapshot.sameOrigin !== false,
        };
      } else if (!(source instanceof HTMLCanvasElement))
        throw new DOMException(
          "The pattern source is not supported by this bounded backend",
          "NotSupportedError",
        );
      const pattern = new CanvasPattern(retained, repetition);
      /* A pattern retains its source pixels for later paint. Latch the
         security state now so no intervening readback can observe them. */
      if (!pattern._originClean) stateFor(this.canvas).originClean = false;
      return pattern;
    }
    createImageData(width, height) {
      return new ImageData(width, height);
    }
    getImageData(x, y, width, height) {
      if (!stateFor(this.canvas).originClean)
        throw new DOMException("Canvas is not origin-clean", "SecurityError");
      x = Math.trunc(Number(x));
      y = Math.trunc(Number(y));
      const size = requirePositiveSize(width, height);
      if (Number(width) < 0) x -= size.width;
      if (Number(height) < 0) y -= size.height;
      const output = new ImageData(size.width, size.height),
        state = stateFor(this.canvas),
        source = state.pixels;
      if (!source) return output;
      if (!flushRectCommands(state)) return output;
      for (let row = 0; row < size.height; row++) {
        const sourceY = y + row;
        if (sourceY < 0 || sourceY >= state.height) continue;
        for (let column = 0; column < size.width; column++) {
          const sourceX = x + column;
          if (sourceX < 0 || sourceX >= state.width) continue;
          const from = (sourceY * state.width + sourceX) * 4,
            to = (row * size.width + column) * 4;
          output.data[to] = source[from];
          output.data[to + 1] = source[from + 1];
          output.data[to + 2] = source[from + 2];
          output.data[to + 3] = source[from + 3];
        }
      }
      return output;
    }
    putImageData(imageData, x, y) {
      if (!(imageData instanceof ImageData))
        throw new TypeError("putImageData requires ImageData");
      x = Math.trunc(Number(x));
      y = Math.trunc(Number(y));
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const state = stateFor(this.canvas),
        target = ensureSurface(state);
      if (!target) return;
      if (!flushRectCommands(state)) return;
      for (let row = 0; row < imageData.height; row++) {
        const targetY = y + row;
        if (targetY < 0 || targetY >= state.height) continue;
        for (let column = 0; column < imageData.width; column++) {
          const targetX = x + column;
          if (targetX < 0 || targetX >= state.width) continue;
          const from = (row * imageData.width + column) * 4,
            to = (targetY * state.width + targetX) * 4;
          target[to] = imageData.data[from];
          target[to + 1] = imageData.data[from + 1];
          target[to + 2] = imageData.data[from + 2];
          target[to + 3] = imageData.data[from + 3];
        }
      }
      const dirty = normalizedRect(
        state, x, y, imageData.width, imageData.height);
      scheduleCanvasCommit(state, dirty);
    }
    getContextAttributes() {
      return { alpha: true, colorSpace: "srgb", willReadFrequently: false };
    }
  }

  const uint32Bytes = (value) =>
      new Uint8Array([
        (value >>> 24) & 255,
        (value >>> 16) & 255,
        (value >>> 8) & 255,
        value & 255,
      ]),
    crc32 = (parts) => {
      let crc = 0xffffffff;
      for (const part of parts)
        for (const value of part) {
          crc ^= value;
          for (let bit = 0; bit < 8; bit++)
            crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
        }
      return (crc ^ 0xffffffff) >>> 0;
    },
    pngChunk = (name, data) => {
      const type = new TextEncoder().encode(name),
        output = new Uint8Array(12 + data.length);
      output.set(uint32Bytes(data.length), 0);
      output.set(type, 4);
      output.set(data, 8);
      output.set(uint32Bytes(crc32([type, data])), 8 + data.length);
      return output;
    },
    encodeCanvasPNG = (canvas) => {
      const state = stateFor(canvas);
      flushRectCommands(state);
      if (state.width === 0 || state.height === 0) return new Uint8Array();
      const pixels =
          state.pixels ||
          new Uint8ClampedArray(state.width * state.height * 4),
        raw = new Uint8Array(state.height * (state.width * 4 + 1));
      for (let row = 0; row < state.height; row++)
        raw.set(
          pixels.subarray(
            row * state.width * 4,
            (row + 1) * state.width * 4,
          ),
          row * (state.width * 4 + 1) + 1,
        );
      const deflateParts = [new Uint8Array([0x78, 0x01])];
      for (let offset = 0; offset < raw.length; offset += 65535) {
        const length = Math.min(65535, raw.length - offset),
          final = offset + length === raw.length,
          header = new Uint8Array([
            final ? 1 : 0,
            length & 255,
            (length >>> 8) & 255,
            (~length) & 255,
            ((~length) >>> 8) & 255,
          ]);
        deflateParts.push(header, raw.subarray(offset, offset + length));
      }
      let a = 1,
        b = 0;
      for (const value of raw) {
        a = (a + value) % 65521;
        b = (b + a) % 65521;
      }
      deflateParts.push(uint32Bytes(((b << 16) | a) >>> 0));
      const compressedLength = deflateParts.reduce(
          (total, part) => total + part.length,
          0,
        ),
        compressed = new Uint8Array(compressedLength);
      let offset = 0;
      for (const part of deflateParts) {
        compressed.set(part, offset);
        offset += part.length;
      }
      const header = new Uint8Array(13);
      header.set(uint32Bytes(state.width), 0);
      header.set(uint32Bytes(state.height), 4);
      header.set([8, 6, 0, 0, 0], 8);
      const parts = [
          new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]),
          pngChunk("IHDR", header),
          pngChunk("IDAT", compressed),
          pngChunk("IEND", new Uint8Array()),
        ],
        length = parts.reduce((total, part) => total + part.length, 0),
        output = new Uint8Array(length);
      offset = 0;
      for (const part of parts) {
        output.set(part, offset);
        offset += part.length;
      }
      return output;
    },
    bytesToBase64 = (bytes) => {
      let binary = "";
      for (let offset = 0; offset < bytes.length; offset += 4096)
        binary += String.fromCharCode(...bytes.subarray(offset, offset + 4096));
      return btoa(binary);
    };

  Object.defineProperties(HTMLCanvasElement.prototype, {
    width: {
      configurable: true,
      enumerable: true,
      get() {
        return dimension(this, "width", 300);
      },
      set(value) {
        this.setAttribute("width", String(Number(value) >>> 0));
      },
    },
    height: {
      configurable: true,
      enumerable: true,
      get() {
        return dimension(this, "height", 150);
      },
      set(value) {
        this.setAttribute("height", String(Number(value) >>> 0));
      },
    },
  });
  HTMLCanvasElement.prototype.getContext = function (type) {
    if (String(type).toLowerCase() !== "2d") return null;
    let context = contexts.get(this);
    if (!context) {
      context = new CanvasRenderingContext2D(this);
      contexts.set(this, context);
    }
    return context;
  };
  HTMLCanvasElement.prototype.toDataURL = function () {
    const state = stateFor(this);
    if (!state.originClean)
      throw new DOMException("Canvas is not origin-clean", "SecurityError");
    if (
      state.width === 0 ||
      state.height === 0 ||
      state.surfaceUnavailable
    )
      return "data:,";
    return "data:image/png;base64," + bytesToBase64(encodeCanvasPNG(this));
  };
  HTMLCanvasElement.prototype.toBlob = function (callback) {
    if (typeof callback !== "function")
      throw new TypeError("toBlob requires a callback");
    const state = stateFor(this);
    if (!state.originClean)
      throw new DOMException("Canvas is not origin-clean", "SecurityError");
    setTimeout(() => {
      if (
        state.width === 0 ||
        state.height === 0 ||
        state.surfaceUnavailable
      ) {
        callback(null);
        return;
      }
      const encoded = encodeCanvasPNG(this);
      callback(
        encoded.byteLength > 256 * 1024
          ? null
          : new Blob([encoded], { type: "image/png" }),
      );
    }, 0);
  };
  globalThis.__tilefinchCanvasAttributeChanged = (node, name) => {
    if (
      node instanceof HTMLCanvasElement &&
      (name === "width" || name === "height")
    ) {
      const state = states.get(node);
      if (state) {
        resetState(state);
        if (!state.surfaceUnavailable && state.width && state.height) {
          ensureSurface(state);
          markCanvasFull(state);
        }
      }
    }
  };
  globalThis.__tilefinchCanvasDimension = (node, name) =>
    dimension(node, name, name === "width" ? 300 : 150);
  globalThis.__tilefinchSetCanvasDimension = (node, name, value) =>
    node.setAttribute(name, String(Number(value) >>> 0));
  globalThis.__tilefinchFlushCanvasSurfaces = flushCanvasSurfaces;
  Object.defineProperty(globalThis, "__tilefinchCanvasDiagnostics", {
    configurable: false,
    enumerable: false,
    value: canvasDiagnostics,
    writable: false,
  });
  globalThis.__tilefinchCanvasConnected = (root) => {
    const candidates = [];
    if (root instanceof HTMLCanvasElement) candidates.push(root);
    if (typeof root?.querySelectorAll === "function") {
      for (const canvas of root.querySelectorAll("canvas")) {
        if (candidates.length >= 8) break;
        candidates.push(canvas);
      }
    }
    for (const canvas of candidates) {
      const state = states.get(canvas);
      if (state?.dirty) scheduleCanvasCommit(state, state.dirty);
    }
  };
  Object.assign(globalThis, {
    CanvasGradient,
    CanvasPattern,
    CanvasRenderingContext2D,
    DOMMatrix,
    DOMMatrixReadOnly: DOMMatrix,
    ImageData,
    Path2D,
  });
})();
