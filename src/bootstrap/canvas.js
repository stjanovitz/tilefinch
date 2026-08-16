(() => {
  /* Pixel storage remains deliberately small and lazy. QuickJS charges every
     backing Uint8ClampedArray to the existing script heap, while this
     per-surface ceiling prevents one canvas from consuming the whole PSP
     profile before ordinary script state gets a chance to run. */
  const pixelLimit = 64 * 1024,
    states = new WeakMap(),
    contexts = new WeakMap(),
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
      state.surfaceUnavailable =
        size.width !== 0 &&
        size.height !== 0 &&
        size.width > Math.floor(pixelLimit / size.height);
      state.fill = [0, 0, 0, 255];
      state.fillStyle = "#000000";
      state.fillPaint = null;
      state.stroke = [0, 0, 0, 255];
      state.strokeStyle = "#000000";
      state.globalAlpha = 1;
      state.lineWidth = 1;
      state.lineCap = "butt";
      state.lineJoin = "miter";
      state.font = "10px sans-serif";
      state.textAlign = "start";
      state.textBaseline = "alphabetic";
      state.transform = [1, 0, 0, 1, 0, 0];
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
          fill: [0, 0, 0, 255],
          fillStyle: "#000000",
          fillPaint: null,
          stroke: [0, 0, 0, 255],
          strokeStyle: "#000000",
          globalAlpha: 1,
          lineWidth: 1,
          lineCap: "butt",
          lineJoin: "miter",
          font: "10px sans-serif",
          textAlign: "start",
          textBaseline: "alphabetic",
          transform: [1, 0, 0, 1, 0, 0],
          path: [],
          subpath: null,
          lineDash: [],
          lineDashOffset: 0,
          stack: [],
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
      const left = Math.max(0, Math.floor(x)),
        top = Math.max(0, Math.floor(y)),
        right = Math.min(state.width, Math.ceil(x + width)),
        bottom = Math.min(state.height, Math.ceil(y + height));
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
      if (x < 0 || y < 0 || x >= state.width || y >= state.height) return;
      const pixels = ensureSurface(state);
      if (!pixels) return;
      const at = (y * state.width + x) * 4,
        sourceAlpha = (color[3] / 255) * state.globalAlpha;
      if (sourceAlpha <= 0) return;
      if (sourceAlpha >= 1) {
        pixels[at] = color[0];
        pixels[at + 1] = color[1];
        pixels[at + 2] = color[2];
        pixels[at + 3] = 255;
        return;
      }
      const destinationAlpha = pixels[at + 3] / 255,
        outputAlpha = sourceAlpha + destinationAlpha * (1 - sourceAlpha);
      if (outputAlpha <= 0) return;
      for (let channel = 0; channel < 3; channel++)
        pixels[at + channel] = byte(
          (color[channel] * sourceAlpha +
            pixels[at + channel] *
              destinationAlpha *
              (1 - sourceAlpha)) /
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
    fillPolygon = (state, points, color) => {
      if (points.length < 3) return;
      let top = Infinity,
        bottom = -Infinity;
      for (const point of points) {
        top = Math.min(top, point.y);
        bottom = Math.max(bottom, point.y);
      }
      top = Math.max(0, Math.ceil(top));
      bottom = Math.min(state.height - 1, Math.floor(bottom));
      if ((bottom - top + 1) * points.length > 256 * 1024)
        throw new DOMException(
          "Path exceeds the bounded canvas work limit",
          "QuotaExceededError",
        );
      for (let y = top; y <= bottom; y++) {
        const crossings = [];
        for (let index = 0; index < points.length; index++) {
          const first = points[index],
            second = points[(index + 1) % points.length];
          if (
            (first.y <= y && second.y > y) ||
            (second.y <= y && first.y > y)
          )
            crossings.push(
              first.x +
                ((y - first.y) * (second.x - first.x)) /
                  (second.y - first.y),
            );
        }
        crossings.sort((left, right) => left - right);
        for (let index = 0; index + 1 < crossings.length; index += 2) {
          const left = Math.max(0, Math.ceil(crossings[index])),
            right = Math.min(
              state.width - 1,
              Math.floor(crossings[index + 1]),
            );
          for (let x = left; x <= right; x++) blendPixel(state, x, y, color);
        }
      }
    };

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
    constructor(canvas, repetition) {
      this._state = stateFor(canvas);
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

  class Path2D {
    constructor(path) {
      this._commands =
        path instanceof Path2D
          ? path._commands.map((command) => [...command])
          : [];
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
    get globalAlpha() {
      return stateFor(this.canvas).globalAlpha;
    }
    set globalAlpha(value) {
      value = Number(value);
      if (Number.isFinite(value) && value >= 0 && value <= 1)
        stateFor(this.canvas).globalAlpha = value;
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
    save() {
      const state = stateFor(this.canvas);
      if (state.stack.length < 16)
        state.stack.push({
          fill: [...state.fill],
          fillStyle: state.fillStyle,
          fillPaint: state.fillPaint,
          stroke: [...state.stroke],
          strokeStyle: state.strokeStyle,
          globalAlpha: state.globalAlpha,
          lineWidth: state.lineWidth,
          lineCap: state.lineCap,
          lineJoin: state.lineJoin,
          font: state.font,
          textAlign: state.textAlign,
          textBaseline: state.textBaseline,
          transform: [...state.transform],
          lineDash: [...state.lineDash],
          lineDashOffset: state.lineDashOffset,
        });
    }
    restore() {
      const state = stateFor(this.canvas),
        saved = state.stack.pop();
      if (!saved) return;
      state.fill = saved.fill;
      state.fillStyle = saved.fillStyle;
      state.fillPaint = saved.fillPaint;
      state.stroke = saved.stroke;
      state.strokeStyle = saved.strokeStyle;
      state.globalAlpha = saved.globalAlpha;
      state.lineWidth = saved.lineWidth;
      state.lineCap = saved.lineCap;
      state.lineJoin = saved.lineJoin;
      state.font = saved.font;
      state.textAlign = saved.textAlign;
      state.textBaseline = saved.textBaseline;
      state.transform = saved.transform;
      state.lineDash = saved.lineDash;
      state.lineDashOffset = saved.lineDashOffset;
    }
    clearRect(x, y, width, height) {
      const state = stateFor(this.canvas),
        rect = normalizedRect(state, x, y, width, height);
      if (!rect || !state.pixels) return;
      for (let row = rect.top; row < rect.bottom; row++) {
        const start = (row * state.width + rect.left) * 4,
          end = (row * state.width + rect.right) * 4;
        state.pixels.fill(0, start, end);
      }
    }
    fillRect(x, y, width, height) {
      const state = stateFor(this.canvas);
      if (state.fillPaint) {
        const rect = normalizedRect(state, x, y, width, height);
        if (!rect) return;
        for (let row = rect.top; row < rect.bottom; row++)
          for (let column = rect.left; column < rect.right; column++)
            blendPixel(
              state,
              column,
              row,
              state.fillPaint._colorAt(column, row),
            );
        return;
      }
      if (
        state.transform.some(
          (value, index) => value !== [1, 0, 0, 1, 0, 0][index],
        )
      ) {
        if (
          ![x, y, width, height].every((value) =>
            Number.isFinite(Number(value)),
          )
        )
          return;
        fillPolygon(
          state,
          [
            transformPoint(state, x, y),
            transformPoint(state, Number(x) + Number(width), y),
            transformPoint(
              state,
              Number(x) + Number(width),
              Number(y) + Number(height),
            ),
            transformPoint(state, x, Number(y) + Number(height)),
          ],
          state.fill,
        );
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
      if (sourceAlpha <= 0) return;
      for (let row = rect.top; row < rect.bottom; row++)
        for (let column = rect.left; column < rect.right; column++) {
          const at = (row * state.width + column) * 4;
          if (sourceAlpha >= 1) {
            pixels[at] = red;
            pixels[at + 1] = green;
            pixels[at + 2] = blue;
            pixels[at + 3] = 255;
            continue;
          }
          const destinationAlpha = pixels[at + 3] / 255,
            outputAlpha =
              sourceAlpha + destinationAlpha * (1 - sourceAlpha);
          if (outputAlpha <= 0) continue;
          pixels[at] = byte(
            (red * sourceAlpha +
              pixels[at] * destinationAlpha * (1 - sourceAlpha)) /
              outputAlpha,
          );
          pixels[at + 1] = byte(
            (green * sourceAlpha +
              pixels[at + 1] * destinationAlpha * (1 - sourceAlpha)) /
              outputAlpha,
          );
          pixels[at + 2] = byte(
            (blue * sourceAlpha +
              pixels[at + 2] * destinationAlpha * (1 - sourceAlpha)) /
              outputAlpha,
          );
          pixels[at + 3] = byte(outputAlpha * 255);
        }
    }
    strokeRect(x, y, width, height) {
      const state = stateFor(this.canvas),
        points = [
          transformPoint(state, x, y),
          transformPoint(state, Number(x) + Number(width), y),
          transformPoint(
            state,
            Number(x) + Number(width),
            Number(y) + Number(height),
          ),
          transformPoint(state, x, Number(y) + Number(height)),
        ];
      for (let index = 0; index < points.length; index++)
        drawLine(
          state,
          points[index],
          points[(index + 1) % points.length],
          state.stroke,
          state.lineWidth,
        );
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
        commands = path instanceof Path2D ? path._commands : state.path,
        subpaths = [];
      let current = null,
        currentSource = null;
      const append = (point) => {
        if (!Number.isFinite(point.x) || !Number.isFinite(point.y)) return;
        if (!current) {
          current = [];
          subpaths.push(current);
        }
        current.push(transformPoint(state, point.x, point.y));
        currentSource = point;
      };
      for (const command of commands) {
        if (command[0] === "M") {
          current = [];
          subpaths.push(current);
          append({ x: command[1], y: command[2] });
        } else if (command[0] === "L") {
          append({ x: command[1], y: command[2] });
        } else if (command[0] === "R") {
          const x = command[1],
            y = command[2],
            width = command[3],
            height = command[4];
          current = [];
          subpaths.push(current);
          for (const point of [
            { x, y },
            { x: x + width, y },
            { x: x + width, y: y + height },
            { x, y: y + height },
            { x, y },
          ])
            append(point);
        } else if (command[0] === "A") {
          let start = command[4],
            end = command[5];
          const anticlockwise = command[6],
            full = Math.PI * 2;
          if (!anticlockwise) {
            while (end < start) end += full;
            end = Math.min(end, start + full);
          } else {
            while (end > start) end -= full;
            end = Math.max(end, start - full);
          }
          const steps = Math.min(
            32,
            Math.max(1, Math.ceil((Math.abs(end - start) / full) * 32)),
          );
          for (let step = 0; step <= steps; step++) {
            const angle = start + ((end - start) * step) / steps;
            append({
              x: command[1] + Math.cos(angle) * command[3],
              y: command[2] + Math.sin(angle) * command[3],
            });
          }
        } else if (command[0] === "Q") {
          const from = currentSource || {
            x: command[1],
            y: command[2],
          };
          for (let step = 1; step <= 16; step++) {
            const t = step / 16,
              inverse = 1 - t;
            append({
              x:
                inverse * inverse * from.x +
                2 * inverse * t * command[1] +
                t * t * command[3],
              y:
                inverse * inverse * from.y +
                2 * inverse * t * command[2] +
                t * t * command[4],
            });
          }
        } else if (command[0] === "C") {
          const from = currentSource || {
            x: command[1],
            y: command[2],
          };
          for (let step = 1; step <= 24; step++) {
            const t = step / 24,
              inverse = 1 - t;
            append({
              x:
                inverse ** 3 * from.x +
                3 * inverse * inverse * t * command[1] +
                3 * inverse * t * t * command[3] +
                t ** 3 * command[5],
              y:
                inverse ** 3 * from.y +
                3 * inverse * inverse * t * command[2] +
                3 * inverse * t * t * command[4] +
                t ** 3 * command[6],
            });
          }
        } else if (command[0] === "E") {
          let start = command[6],
            end = command[7];
          const anticlockwise = command[8],
            full = Math.PI * 2,
            cosine = Math.cos(command[5]),
            sine = Math.sin(command[5]);
          if (!anticlockwise) {
            while (end < start) end += full;
            end = Math.min(end, start + full);
          } else {
            while (end > start) end -= full;
            end = Math.max(end, start - full);
          }
          const steps = Math.min(
            32,
            Math.max(1, Math.ceil((Math.abs(end - start) / full) * 32)),
          );
          for (let step = 0; step <= steps; step++) {
            const angle = start + ((end - start) * step) / steps,
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
    }
    fill(path) {
      const state = stateFor(this.canvas);
      for (const points of this._subpaths(path)) {
        if (points.length >= 3)
          fillPolygon(
            state,
            points,
            state.fillPaint
              ? state.fillPaint._colorAt(points[0].x, points[0].y)
              : state.fill,
          );
      }
    }
    stroke(path) {
      const state = stateFor(this.canvas);
      for (const points of this._subpaths(path))
        for (let index = 1; index < points.length; index++)
          drawLine(
            state,
            points[index - 1],
            points[index],
            state.stroke,
            state.lineWidth,
          );
    }
    isPointInPath(pathOrX, xOrY, yOrRule) {
      const path = pathOrX instanceof Path2D ? pathOrX : null,
        x = Number(path ? xOrY : pathOrX),
        y = Number(path ? yOrRule : xOrY);
      for (const points of this._subpaths(path)) {
        let inside = false;
        for (
          let index = 0, previous = points.length - 1;
          index < points.length;
          previous = index++
        ) {
          const first = points[index],
            second = points[previous];
          if (
            first.y > y !== second.y > y &&
            x <
              ((second.x - first.x) * (y - first.y)) /
                (second.y - first.y) +
                first.x
          )
            inside = !inside;
        }
        if (inside) return true;
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
        match = /([1-9][0-9]*(?:\.[0-9]+)?)px/.exec(state.font),
        height = match ? Number(match[1]) : 10,
        width = text.length * height * 0.6;
      return {
        width,
        actualBoundingBoxLeft: 0,
        actualBoundingBoxRight: width,
        actualBoundingBoxAscent: height * 0.8,
        actualBoundingBoxDescent: height * 0.2,
        fontBoundingBoxAscent: height * 0.8,
        fontBoundingBoxDescent: height * 0.2,
      };
    }
    fillText(text, x, y, maximumWidth) {
      text = String(text).slice(0, 256);
      const state = stateFor(this.canvas),
        metrics = this.measureText(text),
        match = /([1-9][0-9]*(?:\.[0-9]+)?)px/.exec(state.font),
        height = match ? Number(match[1]) : 10,
        limit =
          maximumWidth === undefined
            ? metrics.width
            : Math.max(0, Number(maximumWidth)),
        scale = metrics.width > limit && metrics.width > 0 ? limit / metrics.width : 1,
        advance = height * 0.6 * scale;
      x = Number(x);
      y = Number(y);
      if (state.textAlign === "center") x -= Math.min(metrics.width, limit) / 2;
      else if (["right", "end"].includes(state.textAlign))
        x -= Math.min(metrics.width, limit);
      if (state.textBaseline === "top") y += height * 0.8;
      else if (state.textBaseline === "middle") y += height * 0.3;
      else if (state.textBaseline === "bottom") y -= height * 0.2;
      for (let index = 0; index < text.length; index++) {
        if (!/\s/.test(text[index]))
          fillPolygon(
            state,
            [
              transformPoint(state, x + index * advance, y - height * 0.75),
              transformPoint(
                state,
                x + index * advance + advance * 0.7,
                y - height * 0.75,
              ),
              transformPoint(
                state,
                x + index * advance + advance * 0.7,
                y,
              ),
              transformPoint(state, x + index * advance, y),
            ],
            state.fillPaint
              ? state.fillPaint._colorAt(x + index * advance, y)
              : state.fill,
          );
      }
    }
    strokeText(text, x, y, maximumWidth) {
      const state = stateFor(this.canvas),
        previousFill = state.fill,
        previousPaint = state.fillPaint;
      state.fill = state.stroke;
      state.fillPaint = null;
      this.fillText(text, x, y, maximumWidth);
      state.fill = previousFill;
      state.fillPaint = previousPaint;
    }
    drawImage(source, ...arguments_) {
      if (!(source instanceof HTMLCanvasElement))
        throw new DOMException(
          "Only canvas sources are retained by this bounded backend",
          "NotSupportedError",
        );
      const sourceState = stateFor(source),
        sourcePixels = sourceState.pixels;
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
      const target = stateFor(this.canvas),
        retainedSourcePixels =
          sourceState === target ? sourcePixels.slice() : sourcePixels,
        width = Math.min(target.width, Math.max(0, Math.ceil(Math.abs(dw)))),
        height = Math.min(target.height, Math.max(0, Math.ceil(Math.abs(dh))));
      for (let row = 0; row < height; row++)
        for (let column = 0; column < width; column++) {
          const sourceX = Math.floor(sx + (column * sw) / width),
            sourceY = Math.floor(sy + (row * sh) / height);
          if (
            sourceX < 0 ||
            sourceY < 0 ||
            sourceX >= sourceState.width ||
            sourceY >= sourceState.height
          )
            continue;
          const at = (sourceY * sourceState.width + sourceX) * 4,
            point = transformPoint(target, dx + column, dy + row);
          blendPixel(target, point.x, point.y, [
            retainedSourcePixels[at],
            retainedSourcePixels[at + 1],
            retainedSourcePixels[at + 2],
            retainedSourcePixels[at + 3],
          ]);
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
      if (!(source instanceof HTMLCanvasElement))
        throw new DOMException(
          "Only canvas patterns are retained by this bounded backend",
          "NotSupportedError",
        );
      return new CanvasPattern(source, repetition);
    }
    createImageData(width, height) {
      return new ImageData(width, height);
    }
    getImageData(x, y, width, height) {
      x = Math.trunc(Number(x));
      y = Math.trunc(Number(y));
      const size = requirePositiveSize(width, height);
      if (Number(width) < 0) x -= size.width;
      if (Number(height) < 0) y -= size.height;
      const output = new ImageData(size.width, size.height),
        state = stateFor(this.canvas),
        source = state.pixels;
      if (!source) return output;
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
      if (state) resetState(state);
    }
  };
  globalThis.__tilefinchCanvasDimension = (node, name) =>
    dimension(node, name, name === "width" ? 300 : 150);
  globalThis.__tilefinchSetCanvasDimension = (node, name, value) =>
    node.setAttribute(name, String(Number(value) >>> 0));
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
