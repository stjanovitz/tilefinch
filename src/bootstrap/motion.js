(() => {
  /*
   * CSS motion uses the same bounded property-effect scheduler as
   * Element.animate() and inline transitions.  The parser intentionally
   * retains one animation per element, sixteen keyframe blocks, and the
   * first 128 matched elements.  Those bounds cover the common top-site
   * loading/attention effects without turning authored CSS into an
   * unbounded second retained style tree on the PSP.
   */
  const STYLE_LIMIT = 64,
    STYLE_BYTES_LIMIT = 256 * 1024,
    STYLE_NODE_LIMIT = 4096,
    KEYFRAME_LIMIT = 16,
    RULE_LIMIT = 128,
    ELEMENT_LIMIT = 128,
    FRAME_LIMIT = 16,
    DURATION_LIMIT = 4000,
    ITERATION_LIMIT = 8,
    reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches,
    active = new Set(),
    applied = new WeakMap();
  let pending = false,
    observer = null,
    scans = 0,
    declarationParses = 0,
    retainedKeyframes = 0,
    retainedRules = 0,
    matchedElements = 0;
  if (globalThis.__tilefinchRootCensus)
    Object.defineProperties(globalThis.__tilefinchRootCensus, {
      motionScans: { get: () => scans },
      motionDeclarationParses: { get: () => declarationParses },
      motionKeyframes: { get: () => retainedKeyframes },
      motionRules: { get: () => retainedRules },
      motionElements: { get: () => matchedElements },
    });

  class AnimationEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.animationName = String(init.animationName || "");
      this.elapsedTime = Math.max(0, Number(init.elapsedTime) || 0);
      this.pseudoElement = String(init.pseudoElement || "");
    }
  }
  if (globalThis.AnimationEvent === undefined)
    globalThis.AnimationEvent = AnimationEvent;

  const findBlockEnd = (text, open, limit = text.length) => {
      let depth = 1,
        quote = "";
      for (let at = open + 1; at < limit; at++) {
        const character = text[at];
        if (quote) {
          if (character === "\\") at++;
          else if (character === quote) quote = "";
        } else if (character === '"' || character === "'") quote = character;
        else if (character === "/" && text[at + 1] === "*") {
          at += 2;
          while (
            at + 1 < limit &&
            !(text[at] === "*" && text[at + 1] === "/")
          )
            at++;
          at++;
        } else if (character === "{") depth++;
        else if (character === "}" && --depth === 0) return at;
      }
      return limit;
    },
    boundedUtf8Length = (text) => {
      let bytes = 0;
      for (let at = 0; at < text.length; at++) {
        const code = text.charCodeAt(at);
        if (code <= 0x7f) bytes++;
        else if (code <= 0x7ff) bytes += 2;
        else if (
          code >= 0xd800 &&
          code <= 0xdbff &&
          at + 1 < text.length &&
          text.charCodeAt(at + 1) >= 0xdc00 &&
          text.charCodeAt(at + 1) <= 0xdfff
        ) {
          bytes += 4;
          at++;
        } else bytes += 3;
      }
      return bytes;
    },
    styleTextPrefix = (style, maximumBytes, maximumNodes) => {
      if (!style || maximumBytes <= 0 || maximumNodes <= 0)
        return { text: "", nodes: 0, bytes: 0 };
      try {
        const result = globalThis.__tilefinchGetTextPrefix?.(
            typeof style === "number" ? style : style.__handle,
            Math.min(STYLE_BYTES_LIMIT, maximumBytes),
            Math.min(STYLE_NODE_LIMIT, maximumNodes),
          );
        return result && typeof result === "object"
          ? {
              text: String(result.text || ""),
              nodes: Math.max(0, Math.min(maximumNodes, result.nodes | 0)),
              bytes: Math.max(
                0,
                Math.min(
                  maximumBytes,
                  typeof result.bytes === "number"
                    ? result.bytes | 0
                    : boundedUtf8Length(String(result.text || "")),
                ),
              ),
            }
          : { text: "", nodes: 0, bytes: 0 };
      } catch {
        return { text: "", nodes: 0, bytes: 0 };
      }
    },
    styleAttributePrefix = (element, maximumBytes) => {
      if (!element || maximumBytes <= 0) return "";
      try {
        return String(
          globalThis.__tilefinchGetStyleAttributePrefix?.(
            typeof element === "number" ? element : element.__handle,
            Math.min(STYLE_BYTES_LIMIT, maximumBytes),
          ) ?? "",
        );
      } catch {
        return "";
      }
    },
    declarationMap = (text) => {
      declarationParses++;
      const declarations = new Map();
      for (let at = 0; at < text.length; ) {
        let end = at,
          colon = -1,
          depth = 0,
          quote = "";
        for (; end < text.length; end++) {
          const character = text[end];
          if (quote) {
            if (character === "\\") end++;
            else if (character === quote) quote = "";
          } else if (character === '"' || character === "'")
            quote = character;
          else if (character === "(") depth++;
          else if (character === ")" && depth) depth--;
          else if (character === ":" && depth === 0 && colon < 0) colon = end;
          else if (character === ";" && depth === 0) break;
        }
        if (colon >= at) {
          const name = text.slice(at, colon).trim().toLowerCase(),
            value = text
              .slice(colon + 1, end)
              .replace(/\s*!important\s*$/i, "")
              .trim();
          if (name && value) declarations.set(name, value);
        }
        at = end + 1;
      }
      return declarations;
    },
    firstListItem = (value) => {
      let depth = 0,
        quote = "";
      const text = String(value || "");
      for (let at = 0; at < text.length; at++) {
        const character = text[at];
        if (quote) {
          if (character === "\\") at++;
          else if (character === quote) quote = "";
        } else if (character === '"' || character === "'") quote = character;
        else if (character === "(") depth++;
        else if (character === ")" && depth) depth--;
        else if (character === "," && depth === 0)
          return text.slice(0, at).trim();
      }
      return text.trim();
    },
    timeMs = (value) => {
      const match = String(value || "")
        .trim()
        .match(/^(-?(?:\d+(?:\.\d*)?|\.\d+))(ms|s)$/i);
      if (!match) return null;
      const number =
        Number(match[1]) * (match[2].toLowerCase() === "s" ? 1000 : 1);
      return Number.isFinite(number)
        ? Math.min(DURATION_LIMIT, Math.max(0, number))
        : null;
    },
    animationConfig = (declarations) => {
      let name = "",
        duration = 0,
        delay = 0,
        iterations = 1,
        direction = "normal",
        easing = "ease";
      const shorthand = firstListItem(declarations.get("animation"));
      if (shorthand) {
        const tokens = shorthand.match(
          /(?:[^\s("'\\]+|"(?:\\.|[^"])*"|'(?:\\.|[^'])*'|\([^)]*\))+/g,
        ) || [];
        let sawTime = false;
        for (const token of tokens) {
          const parsedTime = timeMs(token);
          if (parsedTime !== null) {
            if (!sawTime) duration = parsedTime;
            else delay = parsedTime;
            sawTime = true;
          } else if (token === "infinite") iterations = ITERATION_LIMIT;
          else if (/^\d+(?:\.\d+)?$/.test(token))
            iterations = Math.max(
              1,
              Math.min(ITERATION_LIMIT, Math.ceil(Number(token))),
            );
          else if (
            ["normal", "reverse", "alternate", "alternate-reverse"].includes(
              token,
            )
          )
            direction = token;
          else if (
            [
              "linear",
              "ease",
              "ease-in",
              "ease-out",
              "ease-in-out",
            ].includes(token)
          )
            easing = token;
          else if (
            ![
              "step-start",
              "step-end",
              "both",
              "forwards",
              "backwards",
              "none",
              "running",
              "paused",
            ].includes(token) &&
            !token.includes("(")
          )
            name = token.replace(/^['"]|['"]$/g, "");
        }
      }
      if (declarations.has("animation-name"))
        name = firstListItem(declarations.get("animation-name")).replace(
          /^['"]|['"]$/g,
          "",
        );
      const authoredDuration = timeMs(
        firstListItem(declarations.get("animation-duration")),
      );
      if (authoredDuration !== null) duration = authoredDuration;
      const authoredDelay = timeMs(
        firstListItem(declarations.get("animation-delay")),
      );
      if (authoredDelay !== null) delay = authoredDelay;
      const authoredIterations = firstListItem(
        declarations.get("animation-iteration-count"),
      );
      if (authoredIterations) {
        iterations =
          authoredIterations === "infinite"
            ? ITERATION_LIMIT
            : Math.max(
                1,
                Math.min(
                  ITERATION_LIMIT,
                  Math.ceil(Number(authoredIterations) || 1),
                ),
              );
      }
      if (declarations.has("animation-direction"))
        direction = firstListItem(declarations.get("animation-direction"));
      if (declarations.has("animation-timing-function"))
        easing = firstListItem(
          declarations.get("animation-timing-function"),
        );
      const timeline = firstListItem(
        declarations.get("animation-timeline") ||
          declarations.get("scroll-timeline"),
      );
      const scrollLinked =
        !!timeline && timeline !== "auto" && timeline !== "none";
      return name && name !== "none" && duration > 0
        ? {
            name,
            duration,
            delay,
            iterations,
            direction,
            easing,
            scrollLinked,
          }
        : null;
    },
    keyframeOffset = (header) => {
      const first = String(header).split(",", 1)[0].trim().toLowerCase();
      if (first === "from") return 0;
      if (first === "to") return 1;
      const match = first.match(/^(\d+(?:\.\d+)?)%$/);
      if (!match) return null;
      return Math.max(0, Math.min(1, Number(match[1]) / 100));
    },
    parseKeyframes = (text, begin, end) => {
      const frames = [];
      for (let at = begin; at < end && frames.length < FRAME_LIMIT; ) {
        while (at < end && /\s/.test(text[at])) at++;
        const open = text.indexOf("{", at);
        if (open < 0 || open >= end) break;
        const close = findBlockEnd(text, open, end);
        if (close >= end) break;
        const offset = keyframeOffset(text.slice(at, open));
        if (offset !== null) {
          const frame = { offset };
          for (const [name, value] of declarationMap(
            text.slice(open + 1, close),
          )) {
            if (!name.startsWith("animation")) frame[name] = value;
          }
          frames.push(frame);
        }
        at = close + 1;
      }
      frames.sort((left, right) => left.offset - right.offset);
      return frames;
    };

  const parseRules = (
    text,
    begin,
    end,
    keyframes,
    rules,
    depth = 0,
  ) => {
    if (depth > 8) return;
    for (let at = begin; at < end && rules.length < RULE_LIMIT; ) {
      while (at < end && /\s/.test(text[at])) at++;
      if (at >= end) break;
      let delimiter = at,
        quote = "",
        round = 0;
      for (; delimiter < end; delimiter++) {
        const character = text[delimiter];
        if (quote) {
          if (character === "\\") delimiter++;
          else if (character === quote) quote = "";
        } else if (character === '"' || character === "'") quote = character;
        else if (character === "/" && text[delimiter + 1] === "*") {
          delimiter += 2;
          while (
            delimiter + 1 < end &&
            !(text[delimiter] === "*" && text[delimiter + 1] === "/")
          )
            delimiter++;
          delimiter++;
        } else if (character === "(") round++;
        else if (character === ")" && round) round--;
        else if (
          round === 0 &&
          (character === "{" || character === ";")
        )
          break;
      }
      if (delimiter >= end) break;
      if (text[delimiter] === ";") {
        at = delimiter + 1;
        continue;
      }
      const close = findBlockEnd(text, delimiter, end);
      if (close >= end) break;
      const header = text.slice(at, delimiter).trim(),
        lower = header.toLowerCase();
      if (/^@(?:-webkit-)?keyframes\s+/i.test(header)) {
        if (keyframes.size < KEYFRAME_LIMIT) {
          const name = header
            .replace(/^@(?:-webkit-)?keyframes\s+/i, "")
            .trim()
            .replace(/^['"]|['"]$/g, "");
          if (name)
            keyframes.set(
              name,
              parseKeyframes(text, delimiter + 1, close),
            );
        }
      } else if (lower.startsWith("@media ")) {
        let matches = false;
        try {
          matches = matchMedia(header.slice(7).trim()).matches;
        } catch {}
        if (matches)
          parseRules(
            text,
            delimiter + 1,
            close,
            keyframes,
            rules,
            depth + 1,
          );
      } else if (
        lower.startsWith("@layer") ||
        lower.startsWith("@supports ")
      ) {
        parseRules(
          text,
          delimiter + 1,
          close,
          keyframes,
          rules,
          depth + 1,
        );
      } else if (!header.startsWith("@")) {
        const body = text.slice(delimiter + 1, close);
        /* Conservative native-string rejection only: animationConfig reads
           animation* declarations. Unrelated colors/layout need no JS token
           map. False positives (strings/comments/custom properties) still
           take the complete parser; no CSS matching or cascade is skipped. */
        if (/animation/i.test(body)) {
          const config = animationConfig(declarationMap(body));
          if (config) rules.push({ selector: header, config });
        }
      }
      at = close + 1;
    }
  };

  const dispatchAnimationEvent = (element, type, config, elapsedTime = 0) => {
      try {
        element.dispatchEvent(
          new AnimationEvent(type, {
            bubbles: true,
            animationName: config.name,
            elapsedTime,
          }),
        );
      } catch (error) {
        globalThis.__tilefinchReportUncaught?.(error, "CSS animation event");
      }
    },
    stopAnimation = (element, state, cancelled) => {
      state.animation?.cancel();
      if (cancelled)
        dispatchAnimationEvent(
          element,
          "animationcancel",
          state.config,
          (state.animation?.currentTime || 0) / 1000,
        );
      applied.delete(element);
      active.delete(element);
    },
    staticFrame = (frames, config) => {
      const reverse =
        config.direction === "reverse" ||
        (config.direction === "alternate" && config.iterations % 2 === 0) ||
        (config.direction === "alternate-reverse" && config.iterations % 2);
      let frame = frames[reverse ? 0 : frames.length - 1];
      const hidden = (candidate) =>
        String(candidate?.opacity || "") === "0" ||
        String(candidate?.visibility || "").toLowerCase() === "hidden";
      if (hidden(frame)) {
        const visible = frames.find((candidate) => !hidden(candidate));
        if (visible) frame = visible;
      }
      return frame;
    },
    applyStaticFrame = (element, frames, config, signature) => {
      const frame = staticFrame(frames, config);
      if (!frame) return;
      for (const property of ["opacity", "visibility", "display"]) {
        if (frame[property] !== undefined)
          element.style.setProperty(property, frame[property], "important");
      }
      if (config.scrollLinked) {
        element.style.setProperty("transform", "none", "important");
      } else if (frame.transform !== undefined) {
        element.style.setProperty("transform", frame.transform, "important");
      }
      applied.set(element, { animation: null, config, signature, static: true });
    },
    scan = () => {
      scans++;
      const keyframes = new Map(),
        rules = [];
      let retainedBytes = 0,
        retainedNodes = 0;
      const nativeInline = typeof globalThis.__tilefinchQueryAll === "function"
        && !globalThis.__tilefinchHasRemoteNodeWriter;
      const styleNodes = nativeInline
          ? globalThis.__tilefinchQueryAll("style", 0, STYLE_LIMIT)
          : document.querySelectorAll("style"),
        styleCount = Math.min(STYLE_LIMIT, styleNodes.length);
      for (let styleIndex = 0; styleIndex < styleCount; styleIndex++) {
        const style = styleNodes[styleIndex];
        const available = STYLE_BYTES_LIMIT - retainedBytes,
          availableNodes = STYLE_NODE_LIMIT - retainedNodes;
        if (available <= 0 || availableNodes <= 0) break;
        const retained = styleTextPrefix(style, available, availableNodes);
        retainedBytes += retained.bytes;
        retainedNodes += retained.nodes;
        if (/(?:animation|keyframes)/i.test(retained.text))
          parseRules(retained.text, 0, retained.text.length, keyframes, rules);
      }
      const candidates = new Map();
      for (const rule of rules) {
        let elements = [];
        try {
          elements = document.querySelectorAll(rule.selector);
        } catch {
          continue;
        }
        for (const element of elements) {
          if (!(element instanceof Element)) continue;
          if (!candidates.has(element) && candidates.size >= ELEMENT_LIMIT)
            break;
          candidates.set(element, rule.config);
        }
      }
      let inlineElements = [];
      try {
        // Inspect bounded native attribute prefixes before creating wrappers.
        // Most inline styles are geometry/color, not motion; wrapping all of
        // them first can exhaust a tight realm even when no animation applies.
        inlineElements = nativeInline
          ? globalThis.__tilefinchQueryAll("[style]", 0, ELEMENT_LIMIT)
          : document.querySelectorAll("[style]");
      } catch {}
      for (const element of inlineElements) {
        if (
          retainedBytes >= STYLE_BYTES_LIMIT ||
          retainedNodes >= STYLE_NODE_LIMIT
        )
          break;
        if (!nativeInline && !candidates.has(element)
          && candidates.size >= ELEMENT_LIMIT) break;
        const retained = styleAttributePrefix(
          element,
          STYLE_BYTES_LIMIT - retainedBytes,
        );
        retainedBytes += boundedUtf8Length(retained);
        retainedNodes++;
        const config = /animation/i.test(retained)
          ? animationConfig(declarationMap(retained)) : null;
        if (config) {
          const target = nativeInline ? globalThis.__tilefinchWrap(element) : element;
          if (!(target instanceof Element)) continue;
          if (!candidates.has(target) && candidates.size >= ELEMENT_LIMIT) break;
          candidates.set(target, config);
        }
      }
      for (const element of Array.from(active)) {
        if (!candidates.has(element)) {
          const state = applied.get(element);
          if (state) stopAnimation(element, state, true);
          else active.delete(element);
        }
      }
      for (const [element, config] of candidates) {
        const frames = keyframes.get(config.name);
        if (!frames || frames.length < 2) continue;
        const signature =
          config.name +
          ":" +
          config.duration +
          ":" +
          config.delay +
          ":" +
          config.iterations +
          ":" +
          config.direction +
          ":" +
          config.easing +
          ":" +
          config.scrollLinked +
          ":" +
          JSON.stringify(frames);
        const previous = applied.get(element);
        if (previous?.signature === signature) continue;
        if (previous) stopAnimation(element, previous, true);
        if (reducedMotion) {
          applyStaticFrame(element, frames, config, signature);
          continue;
        }
        const animation = element.animate(frames, {
            duration: config.duration,
            delay: config.delay,
            iterations: config.iterations,
            direction: config.direction,
            easing: config.easing,
            fill: "both",
          }),
          state = { animation, config, signature };
        applied.set(element, state);
        active.add(element);
        queueMicrotask(() => {
          if (applied.get(element) === state)
            dispatchAnimationEvent(element, "animationstart", config);
        });
        animation.finished.then(() => {
          if (applied.get(element) !== state) return;
          dispatchAnimationEvent(
            element,
            "animationend",
            config,
            (config.duration * config.iterations) / 1000,
          );
          active.delete(element);
        });
      }
      retainedKeyframes = keyframes.size;
      retainedRules = rules.length;
      matchedElements = candidates.size;
      pending = false;
    },
    documentHasMotionHint = () => {
      try {
        if (globalThis.__tilefinchStylesheetHasMotionKeyframes?.())
          return true;
        const roots = [document.head, document.body];
        let inspectedBytes = 0,
          inspectedNodes = 0;
        for (const root of roots) {
          let node = root?.firstElementChild || null;
          for (
            let visited = 0;
            node &&
            visited < STYLE_LIMIT &&
            inspectedBytes < STYLE_BYTES_LIMIT &&
            inspectedNodes < STYLE_NODE_LIMIT;
            visited++
          ) {
            const available = STYLE_BYTES_LIMIT - inspectedBytes;
            let text = "";
            if (String(node.localName || "").toLowerCase() === "style") {
              const retained = styleTextPrefix(
                node,
                available,
                STYLE_NODE_LIMIT - inspectedNodes,
              );
              text = retained.text;
              inspectedNodes += retained.nodes;
              inspectedBytes += retained.bytes;
            } else {
              text = styleAttributePrefix(node, available);
              inspectedNodes++;
              inspectedBytes += boundedUtf8Length(text);
            }
            if (
              /@(?:-webkit-)?keyframes\b|\banimation(?:-name)?\s*:/i.test(
                text,
              )
            )
              return true;
            node = node.nextElementSibling;
          }
        }
      } catch {
        return false;
      }
      return false;
    };

  globalThis.__tilefinchMotionRecheck = () => {
    if (
      pending ||
      (active.size === 0 && !documentHasMotionHint())
    )
      return;
    pending = true;
    /* Reduced-motion is a static cascade repair, not animation work. Coalesce
       style + content construction at the end of this script turn so the
       authoritative layout observes the final state without a timer or a
       follow-up relayout. */
    if (reducedMotion) {
      queueMicrotask(scan);
      return;
    }
    const schedule =
      globalThis.__tilefinchScheduleRenderFixup || queueMicrotask;
    schedule(scan);
  };
  const beginObserving = () => {
    if (
      observer ||
      !document.documentElement ||
      !documentHasMotionHint()
    )
      return;
    observer = new MutationObserver((records) => {
      for (const record of records)
        if (
          record.type !== "attributes" ||
          record.attributeName === "class"
        ) {
          globalThis.__tilefinchMotionRecheck();
          break;
        }
    });
    observer.observe(document.documentElement, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ["class"],
    });
  };
  globalThis.__tilefinchBeginMotionObservation = beginObserving;
})();
