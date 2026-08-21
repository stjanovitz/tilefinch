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
    retainedKeyframes = 0,
    retainedRules = 0,
    matchedElements = 0;
  if (globalThis.__tilefinchRootCensus)
    Object.defineProperties(globalThis.__tilefinchRootCensus, {
      motionScans: { get: () => scans },
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
    declarationMap = (text) => {
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
      return name && name !== "none" && duration > 0
        ? { name, duration, delay, iterations, direction, easing }
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
        const declarations = declarationMap(
            text.slice(delimiter + 1, close),
          ),
          config = animationConfig(declarations);
        if (config) rules.push({ selector: header, config });
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
    scan = () => {
      pending = false;
      scans++;
      const keyframes = new Map(),
        rules = [];
      let retainedBytes = 0;
      const styleNodes = Array.from(document.querySelectorAll("style")).slice(
        0,
        STYLE_LIMIT,
      );
      for (const style of styleNodes) {
        const text = String(style.textContent || ""),
          available = STYLE_BYTES_LIMIT - retainedBytes;
        if (available <= 0) break;
        const retained = text.slice(0, available);
        retainedBytes += retained.length;
        parseRules(retained, 0, retained.length, keyframes, rules);
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
        inlineElements = document.querySelectorAll("[style]");
      } catch {}
      for (const element of inlineElements) {
        if (!candidates.has(element) && candidates.size >= ELEMENT_LIMIT)
          break;
        const config = animationConfig(
          declarationMap(element.getAttribute("style") || ""),
        );
        if (config) candidates.set(element, config);
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
          JSON.stringify(frames);
        const previous = applied.get(element);
        if (previous?.signature === signature) continue;
        if (previous) stopAnimation(element, previous, true);
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
    },
    documentHasMotionHint = () => {
      try {
        const roots = [document.head, document.body];
        for (const root of roots) {
          let node = root?.firstElementChild || null;
          for (let visited = 0; node && visited < STYLE_LIMIT; visited++) {
            const text =
              String(node.localName || "").toLowerCase() === "style"
                ? String(node.textContent || "")
                : String(node.getAttribute("style") || "");
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
      reducedMotion ||
      pending ||
      (active.size === 0 && !documentHasMotionHint())
    )
      return;
    pending = true;
    const schedule =
      globalThis.__tilefinchScheduleRenderFixup || queueMicrotask;
    schedule(scan);
  };
  const beginObserving = () => {
    if (
      reducedMotion ||
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
