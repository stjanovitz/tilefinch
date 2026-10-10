(() => {
  const nativeMotionSource = globalThis.__tilefinchMotionSourceBridge;
  delete globalThis.__tilefinchMotionSourceBridge;
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
    SHADOW_ROOT_LIMIT = 16,
    FRAME_LIMIT = 16,
    DURATION_LIMIT = 4000,
    ITERATION_LIMIT = 8,
    reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches,
    active = new Set(),
    applied = new WeakMap(),
    lifecycleStates = new Set();
  let pending = false,
    observer = null,
    lifecycleTimer = 0,
    lifecycleTimerDue = Infinity,
    scans = 0,
    declarationParses = 0,
    retainedKeyframes = 0,
    retainedRules = 0,
    matchedElements = 0,
    transitionSnapshots = 0,
    transitionSnapshotReads = 0;
  if (globalThis.__tilefinchRootCensus)
    Object.defineProperties(globalThis.__tilefinchRootCensus, {
      motionScans: { get: () => scans },
      motionDeclarationParses: { get: () => declarationParses },
      motionKeyframes: { get: () => retainedKeyframes },
      motionRules: { get: () => retainedRules },
      motionElements: { get: () => matchedElements },
      transitionSnapshots: { get: () => transitionSnapshots },
      transitionSnapshotReads: { get: () => transitionSnapshotReads },
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
      /* ASCII text, the common case, is one byte per code unit: one native
         regular-expression pass instead of a loop per character. */
      if (!/[^\x00-\x7f]/.test(text)) return text.length;
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
        infinite = false,
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
          } else if (token === "infinite") {
            iterations = ITERATION_LIMIT;
            infinite = true;
          }
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
        infinite = authoredIterations === "infinite";
        iterations =
          infinite ? ITERATION_LIMIT : Math.max(
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
            infinite,
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
    keyframeLimit = KEYFRAME_LIMIT,
    ruleLimit = RULE_LIMIT,
  ) => {
    if (depth > 8) return;
    for (let at = begin; at < end && rules.length < ruleLimit; ) {
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
        if (keyframes.size < keyframeLimit) {
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
            keyframeLimit,
            ruleLimit,
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
          keyframeLimit,
          ruleLimit,
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
    lifecycleNow = () => {
      const now = Number(performance.now());
      return Number.isFinite(now) ? Math.max(0, now) : 0;
    },
    armLifecycleTimer = () => {
      let due = Infinity;
      for (const state of lifecycleStates) {
        if (!state.started) due = Math.min(due, state.startAt);
        else if (state.nextIterationAt < state.endAt)
          due = Math.min(due, state.nextIterationAt);
        else if (!state.ended) due = Math.min(due, state.endAt);
      }
      if (lifecycleTimer && due >= lifecycleTimerDue) return;
      if (lifecycleTimer) globalThis.__tilefinchCancelTimer?.(lifecycleTimer);
      lifecycleTimer = 0;
      lifecycleTimerDue = due;
      if (!Number.isFinite(due)) return;
      const delay = Math.max(0, Math.ceil(due - lifecycleNow()));
      lifecycleTimer = globalThis.__tilefinchScheduleTimeout?.(
        serviceLifecycles,
        delay,
      ) || 0;
    },
    serviceLifecycles = () => {
      lifecycleTimer = 0;
      lifecycleTimerDue = Infinity;
      const now = lifecycleNow();
      for (const state of Array.from(lifecycleStates)) {
        if (!state.started && now >= state.startAt) {
          state.started = true;
          dispatchAnimationEvent(state.element, "animationstart", state.config);
          if (state.config.infinite) {
            lifecycleStates.delete(state);
            continue;
          }
        }
        let remainingIterations = ITERATION_LIMIT - 1;
        while (
          state.started &&
          state.nextIterationAt < state.endAt &&
          now >= state.nextIterationAt &&
          remainingIterations-- > 0
        ) {
          dispatchAnimationEvent(
            state.element,
            "animationiteration",
            state.config,
            (state.nextIterationAt - state.startAt) / 1000,
          );
          state.nextIterationAt += state.config.duration;
        }
        if (state.started && !state.ended && now >= state.endAt) {
          state.ended = true;
          dispatchAnimationEvent(
            state.element,
            "animationend",
            state.config,
            (state.config.duration * state.config.iterations) / 1000,
          );
          lifecycleStates.delete(state);
        }
      }
      armLifecycleTimer();
    },
    startLifecycle = (element, state) => {
      const now = lifecycleNow(),
        activeDuration = state.config.duration * state.config.iterations;
      state.element = element;
      state.started = false;
      state.ended = false;
      state.startAt = now + state.config.delay;
      state.nextIterationAt = state.startAt + state.config.duration;
      state.endAt = state.config.infinite
        ? Infinity
        : state.startAt + activeDuration;
      lifecycleStates.add(state);
      armLifecycleTimer();
    },
    stopAnimation = (element, state, cancelled) => {
      state.animation?.cancel();
      lifecycleStates.delete(state);
      if (cancelled && !state.ended)
        dispatchAnimationEvent(
          element,
          "animationcancel",
          state.config,
          (state.animation?.currentTime || 0) / 1000,
        );
      applied.delete(element);
      active.delete(element);
      armLifecycleTimer();
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
      const state = { animation: null, config, signature, static: true };
      applied.set(element, state);
      active.add(element);
      startLifecycle(element, state);
    },
    queryTree = (root, selector, limit, nativeInline) => {
      if (!root || limit <= 0) return [];
      try {
        const scope = root === document ? 0 : root.__handle;
        if (nativeInline && scope !== undefined)
          return globalThis.__tilefinchQueryAll(selector, scope, limit);
        return root.querySelectorAll(selector);
      } catch {
        return [];
      }
    },
    wrappedElement = (value, nativeInline) =>
      nativeInline && typeof value === "number"
        ? globalThis.__tilefinchWrap(value)
        : value,
    collectTreeRoots = (nativeInline) => {
      const roots = [document];
      /* Without any shadow root there is nothing to find among the
         document's elements (this query returned every element). */
      if (globalThis.__tilefinchShadowRootsExist?.() === false) return roots;
      let inspected = 0;
      for (
        let rootIndex = 0;
        rootIndex < roots.length && roots.length < SHADOW_ROOT_LIMIT &&
        inspected < STYLE_NODE_LIMIT;
        rootIndex++
      ) {
        const hosts = queryTree(
          roots[rootIndex],
          "*",
          STYLE_NODE_LIMIT - inspected,
          nativeInline,
        );
        const count = Math.min(
          hosts.length,
          STYLE_NODE_LIMIT - inspected,
        );
        inspected += count;
        for (let index = 0; index < count; index++) {
          /* Native query results are handles. Shadow-root lookup accepts that
             identity directly, so discovering roots must not retain wrappers
             for thousands of unrelated hosts. */
          const shadow = globalThis.__tilefinchShadowRootForHost?.(
            hosts[index],
          );
          if (shadow && !roots.includes(shadow)) {
            roots.push(shadow);
            if (roots.length >= SHADOW_ROOT_LIMIT) break;
          }
        }
      }
      return roots;
    },
    scan = () => {
      scans++;
      let retainedBytes = 0,
        retainedNodes = 0,
        retainedStyles = 0,
        totalKeyframes = 0,
        totalRules = 0;
      const nativeInline = typeof globalThis.__tilefinchQueryAll === "function"
        && !globalThis.__tilefinchHasRemoteNodeWriter,
        roots = collectTreeRoots(nativeInline),
        candidates = new Map(),
        candidateFrames = new Map();
      const nativeSource = nativeMotionSource?.();
      for (const root of roots) {
        if (retainedStyles >= STYLE_LIMIT) break;
        const keyframes = new Map(),
          rules = [],
          nativeMotion = root === document && typeof nativeSource === "string",
          styleNodes = nativeMotion ? [] : queryTree(
            root, "style", STYLE_LIMIT - retainedStyles, nativeInline),
          styleCount = Math.min(
            STYLE_LIMIT - retainedStyles,
            styleNodes.length,
          );
        retainedStyles += styleCount;
        if (nativeMotion) {
          retainedBytes += boundedUtf8Length(nativeSource);
          parseRules(nativeSource, 0, nativeSource.length, keyframes, rules,
            0, KEYFRAME_LIMIT - totalKeyframes, RULE_LIMIT - totalRules);
        }
        for (let styleIndex = 0; styleIndex < styleCount; styleIndex++) {
          const style = styleNodes[styleIndex],
            available = STYLE_BYTES_LIMIT - retainedBytes,
            availableNodes = STYLE_NODE_LIMIT - retainedNodes;
          if (available <= 0 || availableNodes <= 0) break;
          const retained = styleTextPrefix(style, available, availableNodes);
          retainedBytes += retained.bytes;
          retainedNodes += retained.nodes;
          if (/(?:animation|keyframes)/i.test(retained.text))
            parseRules(
              retained.text, 0, retained.text.length, keyframes, rules, 0,
              KEYFRAME_LIMIT - totalKeyframes,
              RULE_LIMIT - totalRules);
        }
        totalKeyframes += keyframes.size;
        totalRules += rules.length;
        for (const rule of rules) {
          if (candidates.size >= ELEMENT_LIMIT) {
            /* Admission is full, but later authored rules still override the
               retained set. Matching those 128 wrappers is bounded and avoids
               both new wrappers and a zero-length query that suppresses the
               cascade. */
            for (const element of candidates.keys()) {
              if (element.getRootNode() !== root) continue;
              let matches = false;
              try { matches = element.matches(rule.selector); } catch {}
              if (!matches) continue;
              candidates.set(element, rule.config);
              candidateFrames.set(element, keyframes.get(rule.config.name));
            }
          } else {
            const elements = queryTree(
              root, rule.selector, ELEMENT_LIMIT, nativeInline);
            for (const value of elements) {
              const element = wrappedElement(value, nativeInline);
              if (!(element instanceof Element)) continue;
              if (!candidates.has(element)
                  && candidates.size >= ELEMENT_LIMIT) break;
              candidates.set(element, rule.config);
              candidateFrames.set(element, keyframes.get(rule.config.name));
            }
          }
        }
        /* An inline animation takes its frames from these keyframes: with
           none, and nothing running to stop, inline styles cannot matter. */
        if (keyframes.size === 0 && active.size === 0 && roots.length === 1)
          continue;
        /* Inspect bounded native attribute prefixes before creating wrappers.
           Most inline styles are geometry/color, not motion; wrapping all of
           them first can exhaust a tight realm even when no animation applies. */
        const revisitCandidates = candidates.size >= ELEMENT_LIMIT,
          inlineElements = revisitCandidates
          ? candidates.keys()
          : queryTree(root, "[style]", ELEMENT_LIMIT, nativeInline);
        for (const value of inlineElements) {
          if (revisitCandidates && value.getRootNode() !== root) continue;
          if (
            retainedBytes >= STYLE_BYTES_LIMIT ||
            retainedNodes >= STYLE_NODE_LIMIT
          )
            break;
          const retained = styleAttributePrefix(
            value,
            STYLE_BYTES_LIMIT - retainedBytes,
          );
          retainedBytes += boundedUtf8Length(retained);
          retainedNodes++;
          const config = /animation/i.test(retained)
            ? animationConfig(declarationMap(retained)) : null;
          if (config) {
            const target = wrappedElement(value, nativeInline);
            if (!(target instanceof Element)) continue;
            if (!candidates.has(target) && candidates.size >= ELEMENT_LIMIT)
              continue;
            candidates.set(target, config);
            candidateFrames.set(target, keyframes.get(config.name));
          }
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
        const frames = candidateFrames.get(element);
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
          config.infinite +
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
        startLifecycle(element, state);
        animation.finished.then(() => {
          if (applied.get(element) !== state) return;
          /* Property interpolation may hit the bounded frame ceiling before
             the authored timeline. The shared lifecycle timer therefore owns
             animationstart/iteration/end timing independently. */
        });
      }
      retainedKeyframes = totalKeyframes;
      retainedRules = totalRules;
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
    observer = globalThis.__tilefinchObserveMutations((records) => {
      for (const record of records)
        if (
          record.type !== "attributes" ||
          record.attributeName === "class"
        ) {
          globalThis.__tilefinchMotionRecheck();
          break;
        }
    }, document.documentElement, {
      subtree: true,
      childList: true,
      characterData: true,
      attributes: true,
      attributeFilter: ["class", "style", "hidden", "id"],
    });
  };
  globalThis.__tilefinchBeginMotionObservation = beginObserving;

  /*
   * Stylesheet transitions. Layout does not animate, but a page that waits
   * for transitionend after changing a class must still get it, or its menu,
   * dialog or carousel stays half-way forever. An element is watched once
   * something listens for its transition events: after each batch of
   * attribute or inline-style changes, its transitioned properties' computed
   * values are compared with the last snapshot, and a changed one starts a
   * transition with the computed duration and delay (cssom.js owns the
   * event timeline). Bounded to the 64 most recently registered elements.
   */
  const TRANSITION_WATCH_LIMIT = 64,
    TRANSITION_DISCOVERY_LIMIT = 128,
    /* For `all`: properties pages commonly transition whose computed value
       does not follow layout (a used height changing with content is not a
       transition), plus display, which cancels; display stays first. */
    TRANSITION_SAMPLE = [
      "display", "opacity", "transform", "visibility", "color",
      "background-color", "filter", "box-shadow", "max-height", "max-width",
      "top", "right", "bottom", "left", "margin-top", "margin-left",
      "translate", "scale", "rotate",
    ],
    transitionWatched = new Map(),
    /* This batch's changed elements (or handles) with the attribute and its
       old value, flattened in threes; past the limit, or for a change with
       no element, every watched element is rescanned. */
    TRANSITION_DIRTY_LIMIT = 16,
    transitionDirty = [];
  let transitionCheckQueued = false,
    transitionDirtyAll = false,
    transitionCheckActive = false;
  const transitionNames = (state) =>
      state.explicit.length
        ? TRANSITION_SAMPLE.concat(
            state.explicit.filter((name) => !TRANSITION_SAMPLE.includes(name)))
        : TRANSITION_SAMPLE,
    transitionSnapshot = (element, names) => {
      transitionSnapshots++;
      transitionSnapshotReads += names.length;
      try {
        return globalThis.__tilefinchTransitionSnapshot(element.__handle, names);
      } catch {
        return null;
      }
    },
    /* Records the element's current values as the baseline. The snapshot's
       own values array is kept, with the name list it answers (state.names
       only changes when a class names a new property). */
    transitionBaseline = (element, state) => {
      let snapshot = transitionSnapshot(element, state.names);
      if (!snapshot) return;
      const explicit = snapshot[0].filter((name) =>
        name !== "all" && name !== "none" && !TRANSITION_SAMPLE.includes(name));
      if (explicit.some((name) => !state.explicit.includes(name))) {
        state.explicit = Array.from(new Set(state.explicit.concat(explicit)))
          .slice(0, 8);
        state.names = transitionNames(state);
        snapshot = transitionSnapshot(element, state.names);
        if (!snapshot) return;
      }
      state.values = snapshot[3];
      state.valueNames = state.names;
      state.rendered = !!snapshot[4];
    },
    /*
     * Where this batch's changes can reach, or null for everywhere. An
     * attribute or inline style of element T can change the computed style
     * of T, its descendants (descendant/child combinators, inheritance) and
     * the subtrees of its later siblings (sibling combinators, nth-of), all
     * inside T's parent; a shadow tree sits natively inside its host, so
     * that covers :host rules too. :has() reaches ancestors and beyond, so a
     * change the engine's :has classifier cannot rule out rescans everything,
     * as does one inside a shadow tree (::slotted, :host). The scopes are
     * element handles.
     */
    transitionScopes = () => {
      const scopes = [];
      if (transitionDirtyAll) return null;
      for (let at = 0; at < transitionDirty.length; at += 3) {
        let target = transitionDirty[at];
        const name = transitionDirty[at + 1];
        if (typeof target === "number")
          target = globalThis.__tilefinchWrap?.(target);
        if (!target?.isConnected) continue;
        let scope = target;
        if (name !== null) {
          /* A newly watched element (name null) checks only itself. */
          if (target.getRootNode() !== document) return null;
          const sensitive = name === undefined
            ? __tilefinchAttributeChangeMayAffectHas("style")
            : __tilefinchAttributeChangeMayAffectHas(name,
                transitionDirty[at + 2], target.getAttribute(name));
          if (sensitive) return null;
          scope = target.parentNode;
          if (!(scope?.__handle > 0)) return null;
        }
        if (!scopes.includes(scope.__handle)) {
          if (scopes.length >= TRANSITION_DIRTY_LIMIT) return null;
          scopes.push(scope.__handle);
        }
      }
      return scopes;
    },
    checkTransitions = () => {
      if (transitionCheckActive) return;
      transitionCheckActive = true;
      try {
        checkTransitionChanges();
      } finally {
        transitionCheckActive = false;
      }
    },
    checkTransitionChanges = () => {
      transitionCheckQueued = false;
      let scopes = null;
      try {
        scopes = transitionScopes();
      } catch {}
      if (transitionDirtyAll || transitionDirty.some((_, at) =>
          at % 3 === 1 && transitionDirty[at] !== null)) {
        let remaining = TRANSITION_DISCOVERY_LIMIT;
        for (const [root, state] of Array.from(transitionWatched)) {
          if (!state.observesSubtree || !root.isConnected) continue;
          if (scopes && !scopes.some((handle) =>
              root.contains(globalThis.__tilefinchWrap(handle)) ||
              __tilefinchStyleReach(root.__handle, [handle]))) continue;
          remaining -= discoverTransitionDescendants(root, remaining);
          if (remaining <= 0) break;
        }
      }
      transitionDirty.length = 0;
      transitionDirtyAll = false;
      for (const [element, state] of Array.from(transitionWatched)) {
        if (!element.isConnected) {
          for (const name of state.running)
            globalThis.__tilefinchCancelTransition(element, name);
          transitionWatched.delete(element);
          continue;
        }
        if (scopes && !__tilefinchStyleReach(element.__handle, scopes))
          continue;
        const names = state.names,
          snapshot = transitionSnapshot(element, names);
        if (!snapshot) continue;
        const [properties, durations, delays, values, rendered] = snapshot,
          previous = state.values,
          previousNames = state.valueNames,
          previouslyRendered = state.rendered;
        state.values = values;
        state.valueNames = names;
        state.rendered = !!rendered;
        if (!rendered) {
          for (const name of state.running)
            globalThis.__tilefinchCancelTransition(element, name);
          state.running.clear();
          continue;
        }
        /* A hidden ancestor suppresses transitions too. Revealing a subtree
           establishes its first rendered style; it is not a transition from
           values sampled while it had no boxes. */
        if (!previouslyRendered) {
          transitionBaseline(element, state);
          continue;
        }
        for (let slot = 1; slot < names.length; slot++) {
          const name = names[slot],
            at = previousNames === names ? slot : previousNames.indexOf(name);
          if (at < 0 || at >= previous.length) continue;
          const before = String(previous[at] ?? ""),
            after = String(values[slot] ?? "");
          if (before === after ||
              globalThis.__tilefinchInlineTransition(element.__handle, name))
            continue;
          let index = -1;
          for (let at = 0; at < properties.length; at++)
            if (properties[at] === name || properties[at] === "all")
              index = at;
          const duration = index < 0 ? 0 : Math.max(0, durations[index]),
            delay = index < 0 ? 0 : delays[index];
          if (duration + delay <= 0) {
            if (state.running.delete(name))
              globalThis.__tilefinchCancelTransition(element, name);
            continue;
          }
          state.running.add(name);
          globalThis.__tilefinchStartTransition(
            element, name, duration, delay, "stylesheet");
        }
        /* A property a class just named in transition-property starts being
           sampled from here. */
        const explicit = properties.filter((name) =>
          name !== "all" && name !== "none" &&
          !TRANSITION_SAMPLE.includes(name) && !state.explicit.includes(name));
        if (explicit.length) {
          state.explicit = state.explicit.concat(explicit).slice(0, 8);
          state.names = transitionNames(state);
          transitionBaseline(element, state);
        }
      }
    },
    /* Restores this turn's logged attribute changes on the element and its
       ancestors through the host setters (which queue no mutation records),
       takes the baseline, and reapplies them: the style before the change
       that the page is about to wait on. */
    baselineBeforeRecentChanges = (element, state) => {
      const changes = globalThis.__tilefinchRecentAttributeChanges?.() || [],
        /* The style attribute stays: rewriting it through the host would
           drop its CSSOM authorization under a strict CSP. */
        relevant = changes.filter((change) =>
          String(change.name).toLowerCase() !== "style" &&
          (change.target === element || change.target.contains?.(element)));
      if (!relevant.length) {
        transitionBaseline(element, state);
        return;
      }
      const current = relevant.map((change) => ({
        handle: change.target.__handle,
        name: change.name,
        value: change.target.getAttribute(change.name),
      }));
      const write = (handle, name, value) =>
        value === null
          ? __tilefinchRemoveAttribute(handle, name)
          : __tilefinchSetAttribute(handle, name, String(value));
      try {
        for (let at = relevant.length - 1; at >= 0; at--)
          write(relevant[at].target.__handle, relevant[at].name,
            relevant[at].oldValue);
        transitionBaseline(element, state);
      } finally {
        for (const change of current)
          write(change.handle, change.name, change.value);
      }
    };
  /* target: the changed element or its handle; name: the changed attribute
     (with oldValue), undefined for an inline style write, or null for an
     element that has just been watched. */
  globalThis.__tilefinchTransitionsDirty = (target, name, oldValue) => {
    if (transitionWatched.size === 0) return;
    if (!target || transitionDirty.length >= TRANSITION_DIRTY_LIMIT * 3)
      transitionDirtyAll = true;
    else if (!transitionDirtyAll)
      transitionDirty.push(target, name, oldValue);
    if (transitionCheckQueued) return;
    transitionCheckQueued = true;
    queueMicrotask(checkTransitions);
  };
  const watchTransitionElement = (target, observesSubtree = false) => {
    if (!(target instanceof Element) || !(target.__handle > 0)) return;
    if (transitionWatched.has(target)) {
      transitionWatched.get(target).observesSubtree ||= observesSubtree;
      return;
    }
    if (transitionWatched.size >= TRANSITION_WATCH_LIMIT)
      transitionWatched.delete(transitionWatched.keys().next().value);
    const state = {
      names: TRANSITION_SAMPLE,
      values: [],
      valueNames: TRANSITION_SAMPLE,
      explicit: [],
      running: new Set(),
      rendered: false,
      observesSubtree,
    };
    transitionWatched.set(target, state);
    baselineBeforeRecentChanges(target, state);
  };
  /* Transition events bubble. An ancestor listener must also observe
     transitions on descendants, including a class which introduces both
     the transition and its changed value. Bound discovery independently
     of retained watches, and avoid synchronous layout during the walk. */
  const discoverTransitionDescendants = (root, limit) => {
    let node = root.firstElementChild, visited = 0, ascents = 0;
    while (node && visited < limit && ascents < limit) {
      visited++;
      if (transitionWatched.has(node) ||
          __tilefinchTransitionSnapshot(node.__handle, null, true)) {
        if (root.__tilefinchWebkitTransitionEnd)
          node.__tilefinchWebkitTransitionEnd = true;
        watchTransitionElement(node);
      }
      const child = node.firstElementChild;
      if (child) { node = child; continue; }
      while (node !== root && !node.nextElementSibling && ascents < limit) {
        node = node.parentElement;
        ascents++;
        if (!node) return visited;
      }
      node = node === root ? null : node.nextElementSibling;
    }
    return visited;
  };
  globalThis.__tilefinchWatchTransitions = (target, type) => {
    if (!(target instanceof Element) || !(target.__handle > 0)) return;
    if (type === "webkitTransitionEnd")
      target.__tilefinchWebkitTransitionEnd = true;
    watchTransitionElement(target, true);
    discoverTransitionDescendants(target, TRANSITION_DISCOVERY_LIMIT);
    globalThis.__tilefinchTransitionsDirty(target, null);
  };
  /* A reveal followed by a forced geometry/style read establishes the
     before-change style immediately. A later change in this same author task
     can transition; waiting until its microtask would lose that baseline. */
  globalThis.__tilefinchHostChannel.styleFlushed = () => {
    if (transitionCheckQueued) checkTransitions();
  };
})();
