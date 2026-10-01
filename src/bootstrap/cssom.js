(() => {
  /* Computed-style callers usually already have a canonical CSS name. Let
     the native matcher reject ASCII capitals before entering the per-code-unit
     conversion loop. Capture exec directly so author RegExp prototype changes
     cannot alter this private test; custom-property case remains untouched. */
  const cssUppercase = RegExp.prototype.exec.bind(/[A-Z]/);
  globalThis.__tilefinchCssName = (name) => {
    const text = String(name);
    if (text.startsWith("--")) return text;
    if (text === "cssFloat") return "float";
    const uppercase = cssUppercase(text);
    if (uppercase === null) return text;
    let output = "",
      start = 0;
    for (let at = uppercase.index; at < text.length; at++) {
      const code = text.charCodeAt(at);
      if (code >= 65 && code <= 90) {
        output += text.slice(start, at) + "-" + String.fromCharCode(code + 32);
        start = at + 1;
      }
    }
    return start === 0 ? text : output + text.slice(start);
  };
  const cssName = globalThis.__tilefinchCssName;
  const activeEffects = new Map(),
    authoredTargets = new Map(),
    effectLimit = 32,
    durationLimit = 4000,
    iterationLimit = 8,
    frameLimit = 240,
    longEffectFrameInterval = 32;
  let effectFramePending = false;
  /* A declaration block is a list of property names: length, style[i],
     item(i) and iteration ([...element.style]) all expose them in order.
     chatgpt.com's streaming renderer spreads a style declaration; without
     an iterator the proxy answered "" for Symbol.iterator and threw. */
  const declaredNames = (text) => {
      const names = [];
      let depth = 0, start = 0, source = String(text || "");
      for (let at = 0; at <= source.length && names.length < 128; at++) {
        const character = source[at];
        if (character === "(") depth++;
        else if (character === ")" && depth > 0) depth--;
        else if (at === source.length || (character === ";" && depth === 0)) {
          const part = source.slice(start, at), colon = part.indexOf(":");
          start = at + 1;
          if (colon <= 0) continue;
          const name = cssName(part.slice(0, colon).trim());
          if (name && !names.includes(name)) names.push(name);
        }
      }
      return names;
    },
    declarationProxy = (base, names, set) => {
      base.item = (index) => names()[Number(index) >>> 0] ?? "";
      return new Proxy(base, {
        get(target, name) {
          if (typeof name === "symbol")
            return name === Symbol.iterator
              ? function* () { yield* names(); }
              : undefined;
          if (name === "length") return names().length;
          if (/^(?:0|[1-9][0-9]*)$/.test(name)) return names()[Number(name)];
          return name in target ? target[name] : target.getPropertyValue(name);
        },
        set,
      });
    };
  const effectKey = (handle, property) => String(handle) + ":" + property;
  const colorValue = (value) => {
    const text = String(value).trim().toLowerCase();
    let match = text.match(/^#([0-9a-f]{3}|[0-9a-f]{6})$/i);
    if (match) {
      let hex = match[1];
      if (hex.length === 3)
        hex = hex
          .split("")
          .map((part) => part + part)
          .join("");
      return [
        parseInt(hex.slice(0, 2), 16),
        parseInt(hex.slice(2, 4), 16),
        parseInt(hex.slice(4, 6), 16),
        1,
      ];
    }
    match = text.match(
      /^rgba?\(\s*([0-9.]+)\s*[, ]\s*([0-9.]+)\s*[, ]\s*([0-9.]+)(?:\s*[,/]\s*([0-9.]+))?\s*\)$/,
    );
    return match
      ? [
          Math.min(255, Number(match[1])),
          Math.min(255, Number(match[2])),
          Math.min(255, Number(match[3])),
          match[4] === undefined ? 1 : Math.min(1, Number(match[4])),
        ]
      : null;
  };
  const interpolatorFor = (from, to) => {
    const fromColor = colorValue(from),
      toColor = colorValue(to);
    if (fromColor && toColor)
      return (progress) => {
        const values = fromColor.map(
          (value, index) => value + (toColor[index] - value) * progress,
        );
        return `rgba(${Math.round(values[0])}, ${Math.round(values[1])}, ${Math.round(values[2])}, ${Math.round(values[3] * 1000) / 1000})`;
      };
    const numberPattern = /-?(?:[0-9]*\.)?[0-9]+/g,
      fromParts = String(from).split(numberPattern),
      toParts = String(to).split(numberPattern),
      fromNumbers = String(from).match(numberPattern)?.map(Number) || [],
      toNumbers = String(to).match(numberPattern)?.map(Number) || [];
    if (
      fromNumbers.length &&
      fromNumbers.length === toNumbers.length &&
      fromParts.length === toParts.length &&
      fromParts.every((part, index) => part === toParts[index])
    ) {
      return (progress) => {
        let output = fromParts[0];
        for (let index = 0; index < fromNumbers.length; index++) {
          const value =
            fromNumbers[index] +
            (toNumbers[index] - fromNumbers[index]) * progress;
          output +=
            String(Math.round(value * 1000) / 1000) + fromParts[index + 1];
        }
        return output;
      };
    }
    return null;
  };
  /*
   * The transition lifecycle: transitionrun when a transition is created,
   * transitionstart after its delay, transitionend after its duration and
   * transitioncancel when a newer change replaces it. An inline effect's
   * frames carry its events; without one (a stylesheet transition, or a
   * value that cannot be interpolated and changes at once) one shared clock
   * does, so they arrive on time either way. Pages that wait for
   * transitionend would otherwise wait forever.
   *
   * The clock keeps a single timer, due at the earliest pending event, in a
   * slot reserved for browser clocks: a page that fills the author timer
   * table cannot strand a transition. A negative delay means the transition
   * began that long ago, so it ends delay + duration after the change.
   *
   * At most transitionRecordLimit transitions run at once. Past it the
   * oldest is completed early (its transitionend is queued) rather than
   * dropped, so no page waiting for it is stranded. Starting one re-arms
   * the clock only when it is due before the armed deadline, without
   * rescanning the others; the clock scans once when it fires.
   */
  const runningTransitions = new Map(),
    transitionEventLimit = 60000,
    transitionRecordLimit = 256,
    /* Diagnostics: records visited by clock scans, early completions,
       and the running count. */
    transitionStats = {
      transitionRecordVisits: 0,
      transitionEvictions: 0,
      get transitionsRunning() { return runningTransitions.size; },
    };
  Object.defineProperty(globalThis, "__tilefinchTransitionStats", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: transitionStats,
  });
  let transitionClock = 0,
    transitionClockDue = Infinity;
  const transitionNow = () => {
      const now = Number(globalThis.__tilefinchSchedulerTime?.());
      return Number.isFinite(now) ? now : 0;
    },
    dispatchTransitionEvent = (node, type, property, elapsedMs) => {
      try {
        const init = {
          bubbles: true,
          propertyName: property,
          elapsedTime: Math.max(0, elapsedMs) / 1000,
        };
        node.dispatchEvent(new TransitionEvent(type, init));
        if (type === "transitionend" && node.__tilefinchWebkitTransitionEnd)
          node.dispatchEvent(new TransitionEvent("webkitTransitionEnd", init));
      } catch (error) {
        globalThis.__tilefinchReportUncaught?.(error, "CSS transition event");
      }
    },
    /* Dispatches whatever of one clocked transition is due. A handler may
       cancel or replace it, so it is looked up again after each event. */
    advanceTransition = (key, record, now) => {
      if (runningTransitions.get(key) !== record) return;
      if (!record.started && now >= record.startAt) {
        record.started = true;
        dispatchTransitionEvent(record.node, "transitionstart",
          record.property, Math.max(0, -record.delay));
        if (runningTransitions.get(key) !== record) return;
      }
      if (record.started && now >= record.endAt) {
        runningTransitions.delete(key);
        dispatchTransitionEvent(record.node, "transitionend",
          record.property, record.duration);
      }
    },
    armTransitionClock = (known = null) => {
      let due = Infinity;
      if (known !== null) {
        due = known;
      } else {
        for (const record of runningTransitions.values()) {
          transitionStats.transitionRecordVisits++;
          if (record.clocked)
            due = Math.min(due, record.started ? record.endAt : record.startAt);
        }
      }
      if (transitionClock && due >= transitionClockDue) return;
      if (transitionClock) globalThis.__tilefinchCancelTimer(transitionClock);
      transitionClock = 0;
      transitionClockDue = Infinity;
      if (!Number.isFinite(due)) return;
      transitionClock = globalThis.__tilefinchScheduleReservedTask(
        serviceTransitionClock, Math.max(0, Math.ceil(due - transitionNow())));
      /* Only a broken reservation refuses; the next arm retries. */
      if (transitionClock) transitionClockDue = due;
    },
    serviceTransitionClock = () => {
      transitionClock = 0;
      transitionClockDue = Infinity;
      const now = transitionNow(),
        due = [];
      /* Transitions a handler starts wait for the next service. */
      for (const [key, record] of runningTransitions) {
        transitionStats.transitionRecordVisits++;
        if (record.clocked &&
            now >= (record.started ? record.endAt : record.startAt))
          due.push(key, record);
      }
      for (let at = 0; at < due.length; at += 2)
        advanceTransition(due[at], due[at + 1], now);
      armTransitionClock();
    },
    cancelTransition = (key) => {
      const record = runningTransitions.get(key);
      if (!record) return;
      runningTransitions.delete(key);
      const elapsed = transitionNow() - record.createdAt - record.delay;
      queueMicrotask(() =>
        dispatchTransitionEvent(
          record.node, "transitioncancel", record.property,
          Math.min(record.duration, Math.max(0, elapsed))));
    },
    startTransition = (node, property, duration, delay, source,
                       effect = null) => {
      const handle = node?.__handle;
      if (!(handle > 0)) return;
      const key = effectKey(handle, property);
      cancelTransition(key);
      if (runningTransitions.size >= transitionRecordLimit) {
        const [oldestKey, oldest] = runningTransitions.entries().next().value;
        runningTransitions.delete(oldestKey);
        transitionStats.transitionEvictions++;
        queueMicrotask(() => {
          /* An inline transition's frames stop at its end value with it. */
          if (oldest.effect) finishEffectEarly(oldestKey, oldest.effect);
          if (!oldest.started)
            dispatchTransitionEvent(oldest.node, "transitionstart",
              oldest.property, Math.max(0, -oldest.delay));
          dispatchTransitionEvent(oldest.node, "transitionend",
            oldest.property, oldest.duration);
        });
      }
      duration = Math.min(transitionEventLimit, Math.max(0, duration));
      delay = Math.max(-transitionEventLimit,
        Math.min(transitionEventLimit, delay));
      const createdAt = transitionNow(),
        record = {
          node, property, duration, delay, source, createdAt,
          clocked: !effect,
          effect,
          started: false,
          startAt: createdAt + Math.max(0, delay),
          endAt: createdAt + Math.max(0, delay + duration),
        };
      runningTransitions.set(key, record);
      const current = () => runningTransitions.get(key) === record;
      queueMicrotask(() => {
        if (current())
          dispatchTransitionEvent(node, "transitionrun", property,
            Math.max(0, -delay));
      });
      if (effect) {
        effect.transition = {
          started: () => {
            if (!current() || record.started) return;
            record.started = true;
            dispatchTransitionEvent(node, "transitionstart", property,
              Math.max(0, -delay));
          },
          ended: () => {
            if (!current()) return;
            runningTransitions.delete(key);
            dispatchTransitionEvent(node, "transitionend", property, duration);
          },
        };
        return;
      }
      armTransitionClock(Math.min(record.startAt, record.endAt));
    };
  globalThis.__tilefinchStartTransition = startTransition;
  globalThis.__tilefinchCancelTransition = (node, property) => {
    if (node?.__handle > 0) cancelTransition(effectKey(node.__handle, property));
  };
  /* The watcher leaves a property to the inline path while it runs. */
  globalThis.__tilefinchInlineTransition = (handle, property) =>
    runningTransitions.get(effectKey(handle, property))?.source === "inline";
  /* One entry of each computed transition list for a property, by the list
     position that names it (or `all`), cycling shorter lists. */
  const transitionTimeList = (value) =>
      String(value || "").split(",").map((item) => {
        const match = item.trim().match(/^(-?(?:\d+(?:\.\d*)?|\.\d+))(ms|s)$/i);
        return match
          ? Number(match[1]) * (match[2].toLowerCase() === "s" ? 1000 : 1)
          : 0;
      }),
    computedTransitionTiming = (node, property) => {
      const style = getComputedStyle(node),
        properties = String(style.getPropertyValue("transition-property"))
          .split(",").map((item) => item.trim().toLowerCase()),
        durations = transitionTimeList(
          style.getPropertyValue("transition-duration")),
        delays = transitionTimeList(style.getPropertyValue("transition-delay"));
      let index = -1;
      for (let at = 0; at < properties.length; at++)
        if (properties[at] === property || properties[at] === "all")
          index = at;
      if (index < 0) return null;
      return {
        duration: durations[index % durations.length] || 0,
        delay: delays[index % delays.length] || 0,
      };
    };
  const cancelEffect = (key, restore) => {
    const effect = activeEffects.get(key);
    if (!effect) return;
    effect.cancelled = true;
    activeEffects.delete(key);
    authoredTargets.delete(key);
    if (effect.transition) cancelTransition(key);
    if (restore !== undefined) effect.writeRaw(effect.property, restore);
  };
  /* The effect frame takes a slot reserved for browser clocks, so a page
     that fills the author timer table cannot freeze every effect. */
  const requestEffectFrame = () => {
    if (
      effectFramePending ||
      activeEffects.size === 0 ||
      typeof requestAnimationFrame !== "function"
    )
      return;
    const reserved = globalThis.__tilefinchScheduleReservedFrame;
    /* Only a broken reservation refuses; the next request retries. */
    effectFramePending = !!(typeof reserved === "function"
      ? reserved(runEffectFrame)
      : requestAnimationFrame(runEffectFrame));
  };
  /* The frame visits the effects that were active when it began. Transition
     events run author handlers in the middle of it, and one may cancel or
     replace any effect: each is looked up again before it is touched, and a
     replacement waits for the next frame, so a handler that retargets on
     every start costs a frame per retarget rather than looping here. */
  const frameEffects = [];
  function runEffectFrame(now) {
    effectFramePending = false;
    let running = false;
    for (const [key, effect] of activeEffects) frameEffects.push(key, effect);
    try {
      for (let at = 0; at < frameEffects.length; at += 2)
        if (runEffect(frameEffects[at], frameEffects[at + 1], now))
          running = true;
    } finally {
      frameEffects.length = 0;
    }
    if (running && activeEffects.size !== 0) requestEffectFrame();
  }
  /* One effect's frame; true while it keeps running. */
  function runEffect(key, effect, now) {
    if (activeEffects.get(key) !== effect) return false;
    if (effect.cancelled || effect.state === "idle") {
      activeEffects.delete(key);
      authoredTargets.delete(key);
      return false;
    }
    if (effect.state === "paused") return false;
    if (effect.started === undefined) effect.started = now + effect.delay;
    const elapsed = Math.max(0, (now - effect.started) * effect.rate);
    effect.currentTime = elapsed;
    if (now < effect.started) return true;
    if (effect.ticks === 0) {
      effect.transition?.started();
      if (effect.cancelled || activeEffects.get(key) !== effect)
        return false;
    }
    effect.ticks++;
    const complete =
        elapsed >= effect.duration * effect.iterations ||
        effect.ticks >= frameLimit,
      frameInterval =
        effect.duration > 64 ? longEffectFrameInterval : 0;
    /*
     * rAF remains standards-visible at the scheduler cadence, but a
     * long-running property effect only dirties style at 30 Hz. On the PSP
     * every authored style write can require a layout; avoiding duplicate
     * 16 ms samples halves that steady-state work without delaying short
     * transitions or their completion frame.
     */
    if (
      !complete &&
      effect.lastPresented !== undefined &&
      now - effect.lastPresented < frameInterval
    )
      return true;
    const cycle = Math.min(
        effect.iterations - 1,
        Math.floor(elapsed / effect.duration),
      ),
      cycleTime = elapsed - cycle * effect.duration,
      linear = Math.min(1, cycleTime / effect.duration);
    let directed = linear;
    if (
      effect.direction === "reverse" ||
      (effect.direction === "alternate" && cycle % 2 === 1) ||
      (effect.direction === "alternate-reverse" && cycle % 2 === 0)
    )
      directed = 1 - directed;
    const eased =
      effect.easing === "linear"
        ? directed
        : directed * directed * (3 - 2 * directed);
    effect.writeRaw(effect.property, effect.interpolate(eased));
    effect.frames++;
    effect.lastPresented = now;
    if (complete) {
      completeEffect(key, effect);
      effect.transition?.ended();
    }
    return true;
  }
  /* Writes an effect's end value and retires it. */
  function completeEffect(key, effect) {
    effect.writeRaw(
      effect.property,
      effect.interpolate(
        effect.direction === "reverse" ||
          (effect.direction === "alternate" &&
            effect.iterations % 2 === 0) ||
          (effect.direction === "alternate-reverse" &&
            effect.iterations % 2 === 1)
          ? 0
          : 1,
      ),
    );
    activeEffects.delete(key);
    authoredTargets.delete(key);
    effect.state = "finished";
    effect.resolve?.(effect);
  }
  /* A transition completed early (the running-transition bound) ends its
     visual effect too; the caller dispatches its events. */
  function finishEffectEarly(key, effect) {
    if (activeEffects.get(key) !== effect || effect.cancelled) return;
    effect.transition = null;
    completeEffect(key, effect);
  }
  const scheduleEffect = ({
    handle,
    property,
    from,
    to,
    duration,
    delay = 0,
    iterations = 1,
    direction = "normal",
    easing = "ease",
    writeRaw,
    restore,
  }) => {
    const interpolate = interpolatorFor(from, to);
    if (!interpolate || duration <= 0 ||
        typeof requestAnimationFrame !== "function")
      return null;
    const key = effectKey(handle, property);
    cancelEffect(key);
    if (activeEffects.size >= effectLimit) return null;
    const effect = {
      cancelled: false,
      frames: 0,
      ticks: 0,
      lastPresented: undefined,
      property,
      writeRaw,
      restore,
      state: "running",
      currentTime: 0,
      rate: 1,
      delay,
      duration,
      direction,
      easing,
      iterations,
      interpolate,
      started: undefined,
      to,
    };
    activeEffects.set(key, effect);
    authoredTargets.set(key, String(to));
    requestEffectFrame();
    return effect;
  };
  /* Animations Element.animate started and has not yet finished or
     cancelled, for getAnimations(). Bounded by effectLimit through
     scheduleEffect: an animation with no running effect is never listed. */
  const liveAnimations = new Map();
  globalThis.__tilefinchElementAnimations = (node) => {
    const listed = [];
    for (const [animation, target] of liveAnimations)
      if (node === null || target === node) listed.push(animation);
    return listed;
  };
  globalThis.__tilefinchAnimateElement = (node, keyframes, options = {}) => {
    options = options == null ? {} : options;
    const frames = Array.isArray(keyframes)
        ? keyframes.slice(0, 16)
        : [keyframes || {}, keyframes || {}],
      first = frames[0] || {},
      last = frames[frames.length - 1] || {},
      duration =
        typeof options === "number"
          ? Math.min(durationLimit, Math.max(0, Number(options)))
          : Math.min(
              durationLimit,
              Math.max(0, Number(options.duration) || 0),
            ),
      iterations = Math.min(
        iterationLimit,
        Math.max(1, Math.floor(Number(options.iterations) || 1)),
      ),
      delay = Math.min(
        durationLimit,
        Math.max(0, Number(options.delay) || 0),
      ),
      direction = [
        "normal",
        "reverse",
        "alternate",
        "alternate-reverse",
      ].includes(String(options.direction))
        ? String(options.direction)
        : "normal",
      easing = String(options.easing || "ease"),
      properties = Object.keys(last).slice(0, 8),
      effects = [];
    if (!node || !node.style || !duration) properties.length = 0;
    for (const authoredName of properties) {
      const property = cssName(authoredName),
        from =
          first[authoredName] ??
          getComputedStyle(node).getPropertyValue(property),
        to = last[authoredName],
        old = node.style.getPropertyValue(property),
        effect = scheduleEffect({
          handle: node.__handle,
          property,
          from,
          to,
          duration,
          delay,
          iterations,
          direction,
          easing,
          writeRaw: (name, value) =>
            __tilefinchStyleSet(node.__handle, name, value),
          restore: old,
        });
      if (effect) effects.push(effect);
      else __tilefinchStyleSet(node.__handle, property, String(to));
    }
    let resolveFinishedPromise;
    const finished = new Promise((resolve) => {
        resolveFinishedPromise = resolve;
      }),
      resolveFinished = (value) => {
        liveAnimations.delete(animation);
        resolveFinishedPromise(value);
      };
    let remaining = effects.length,
      playbackRate = 1;
    for (const effect of effects)
      effect.resolve = () => {
        if (--remaining === 0) resolveFinished(animation);
      };
    const animation = {
      get currentTime() {
        return effects[0]?.currentTime || 0;
      },
      set currentTime(value) {
        const time = Math.max(0, Number(value) || 0);
        for (const effect of effects) effect.currentTime = time;
      },
      get playState() {
        return effects[0]?.state || "finished";
      },
      get playbackRate() {
        return effects[0]?.rate ?? playbackRate;
      },
      set playbackRate(value) {
        this.updatePlaybackRate(value);
      },
      /* Keeps each effect's current time and changes how fast it advances
         from here, as the Web Animations model does. */
      updatePlaybackRate(value) {
        const rate = Number(value);
        if (!Number.isFinite(rate) || rate <= 0) return;
        playbackRate = rate;
        const now = performance.now();
        for (const effect of effects) {
          if (effect.started !== undefined)
            effect.started = now - effect.currentTime / rate;
          effect.rate = rate;
        }
      },
      effect:
        typeof globalThis.KeyframeEffect === "function"
          ? new globalThis.KeyframeEffect(node, frames, options)
          : null,
      finished,
      play() {
        for (const effect of effects) effect.state = "running";
        requestEffectFrame();
      },
      pause() {
        for (const effect of effects) effect.state = "paused";
      },
      cancel() {
        for (const effect of effects)
          cancelEffect(
            effectKey(node.__handle, effect.property),
            effect.restore,
          );
        remaining = 0;
        resolveFinished(animation);
      },
      finish() {
        for (const property of properties)
          __tilefinchStyleSet(
            node.__handle,
            cssName(property),
            String(last[property]),
          );
        for (const effect of effects) {
          effect.state = "finished";
          cancelEffect(effectKey(node.__handle, effect.property));
        }
        remaining = 0;
        resolveFinished(animation);
      },
    };
    if (animation.effect)
      globalThis.__tilefinchBindEffectClock?.(animation.effect, () =>
        effects.length === 0
          ? null
          : (effects[0].currentTime || 0) + (effects[0].started === undefined
              ? 0 : effects[0].delay),
      );
    if (remaining === 0) resolveFinished(animation);
    else liveAnimations.set(animation, node);
    return animation;
  };
  const canonicalDisplay = (value) => {
    const raw = String(value).trim().toLowerCase(),
      tokens = raw.split(/\s+/).filter(Boolean);
    if (
      tokens.length === 1 &&
      [
        "none",
        "contents",
        "inline",
        "block",
        "run-in",
        "flow-root",
        "flex",
        "grid",
        "table",
        "ruby",
        "list-item",
        "inline-block",
        "inline-flex",
        "inline-grid",
        "inline-table",
        "table-row-group",
        "table-header-group",
        "table-footer-group",
        "table-row",
        "table-column-group",
        "table-column",
        "table-cell",
        "table-caption",
        "ruby-base",
        "ruby-text",
      ].includes(tokens[0])
    )
      return tokens[0];
    if (tokens.length === 1 && tokens[0] === "flow") return "block";
    const allowed = new Set([
      "block",
      "inline",
      "run-in",
      "flow",
      "flow-root",
      "table",
      "flex",
      "grid",
      "ruby",
      "list-item",
    ]);
    if (
      !tokens.length ||
      tokens.some((token) => !allowed.has(token)) ||
      new Set(tokens).size !== tokens.length
    )
      return null;
    const outer =
        tokens.find((token) => ["block", "inline", "run-in"].includes(token)) ||
        "block",
      inner =
        tokens.find((token) =>
          ["flow", "flow-root", "table", "flex", "grid", "ruby"].includes(
            token,
          ),
        ) || "flow",
      listed = tokens.includes("list-item");
    if (
      tokens.filter((token) => ["block", "inline", "run-in"].includes(token))
        .length > 1 ||
      tokens.filter((token) =>
        ["flow", "flow-root", "table", "flex", "grid", "ruby"].includes(token),
      ).length > 1
    )
      return null;
    if (listed) {
      const prefix = outer === "block" ? "" : outer + " ",
        middle = inner === "flow" ? "" : inner + " ";
      return prefix + middle + "list-item";
    }
    if (outer === "block")
      return inner === "flow"
        ? "block"
        : inner === "flow-root"
          ? "flow-root"
          : inner === "ruby"
            ? "block ruby"
            : inner;
    if (outer === "inline")
      return inner === "flow"
        ? "inline"
        : inner === "flow-root"
          ? "inline-block"
          : inner === "flex"
            ? "inline-flex"
            : inner === "grid"
              ? "inline-grid"
              : inner === "table"
                ? "inline-table"
                : inner === "ruby"
                  ? "ruby"
                  : "inline " + inner;
    return inner === "flow" ? "run-in" : "run-in " + inner;
  };
  const splitTopLevel = (value) => {
    const output = [];
    let start = 0,
      depth = 0;
    value = String(value).trim();
    for (let at = 0; at < value.length; at++) {
      const char = value[at];
      if (char === "(" || char === "[") depth++;
      else if (char === ")" || char === "]") depth = Math.max(0, depth - 1);
      else if (/\s/.test(char) && depth === 0) {
        if (at > start) output.push(value.slice(start, at));
        while (at + 1 < value.length && /\s/.test(value[at + 1])) at++;
        start = at + 1;
      }
    }
    if (start < value.length) output.push(value.slice(start));
    return output;
  };
  const numberPattern = /^[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i,
    canonicalNumber = (value) => {
      const number = Number(value);
      return Number.isFinite(number) && number >= 0 ? String(number) : null;
    };
  const canonicalFlex = (value) => {
    const raw = String(value).trim().toLowerCase();
    if (raw === "none") return { grow: "0", shrink: "0", basis: "auto" };
    const tokens = splitTopLevel(raw),
      numbers = [],
      bases = [];
    for (const token of tokens) {
      if (numberPattern.test(token) && numbers.length < 2) {
        const number = canonicalNumber(token);
        if (number === null) return null;
        numbers.push(number);
      } else bases.push(token);
    }
    if (bases.length > 1 || numbers.length > 2) return null;
    if (numbers.length === 0)
      return bases.length === 1
        ? { grow: "1", shrink: "1", basis: bases[0] }
        : null;
    return {
      grow: numbers[0],
      shrink: numbers[1] || "1",
      basis: bases[0] || "0%",
    };
  };
  const canonicalOutline = (value) => {
    const tokens = splitTopLevel(value);
    if (tokens.length === 1) return tokens[0] === "0" ? "0px" : tokens[0];
    const styles = new Set([
        "auto",
        "none",
        "dotted",
        "dashed",
        "solid",
        "double",
        "groove",
        "ridge",
        "inset",
        "outset",
      ]),
      widths = new Set(["thin", "medium", "thick"]);
    let color = "",
      style = "",
      width = "";
    for (const token of tokens) {
      if (styles.has(token) && !style) style = token;
      else if (
        (widths.has(token) ||
          token === "0" ||
          /^(?:\d*\.)?\d+(?:px|em|ex|rem|%)$/i.test(token) ||
          /^calc\(/i.test(token)) &&
        !width
      )
        width = token === "0" ? "0px" : token;
      else color = color ? color + " " + token : token;
    }
    return [color, style, width].filter(Boolean).join(" ");
  };
  const canonicalTextTransform = (value) => {
    const tokens = splitTopLevel(value);
    if (tokens.length < 2) return String(value).trim();
    const order = [
      "none",
      "capitalize",
      "uppercase",
      "lowercase",
      "math-auto",
      "full-width",
      "full-size-kana",
    ];
    return tokens
      .slice()
      .sort((left, right) => order.indexOf(left) - order.indexOf(right))
      .join(" ");
  };
  const canonicalObjectPosition = (value) => {
    const tokens = splitTopLevel(value);
    if (tokens.length === 1) {
      if (tokens[0] === "center") return "center";
      if (tokens[0] === "top" || tokens[0] === "bottom")
        return "center " + tokens[0];
      return tokens[0] + " center";
    }
    const vertical = (token) => token === "top" || token === "bottom";
    if (
      tokens.length === 2 &&
      (vertical(tokens[0]) ||
        (tokens[0] === "center" &&
          (tokens[1] === "left" || tokens[1] === "right")))
    )
      return tokens[1] + " " + tokens[0];
    if (tokens.length === 4 && vertical(tokens[0]))
      return tokens.slice(2).concat(tokens.slice(0, 2)).join(" ");
    return tokens.join(" ");
  };
  const scrollBoxLonghands = {
      "scroll-margin": [
        "scroll-margin-top",
        "scroll-margin-right",
        "scroll-margin-bottom",
        "scroll-margin-left",
      ],
      "scroll-padding": [
        "scroll-padding-top",
        "scroll-padding-right",
        "scroll-padding-bottom",
        "scroll-padding-left",
      ],
    },
    cssWideKeywords = new Set([
      "inherit",
      "initial",
      "revert",
      "revert-layer",
      "unset",
    ]),
    canonicalScrollLength = (value, padding) => {
      const text = String(value).trim().toLowerCase();
      if (cssWideKeywords.has(text)) return text;
      if (padding && text === "auto") return text;
      if (text === "0" || text === "+0" || text === "-0") return "0px";
      if (/^calc\([\s\S]+\)$/i.test(text)) return text;
      const match = text.match(
        /^(-?(?:\d+(?:\.\d*)?|\.\d+))(px|em|ex|rem|ch|vw|vh|vmin|vmax|%)$/i,
      );
      if (!match || (padding && Number(match[1]) < 0)) return null;
      if (!padding && match[2] === "%") return null;
      return String(Number(match[1])) + match[2].toLowerCase();
    },
    canonicalScrollBox = (name, value) => {
      const padding = name.startsWith("scroll-padding"),
        tokens = splitTopLevel(String(value).trim());
      if (!tokens.length || tokens.length > 4) return null;
      if (tokens.length > 1 && tokens.some((token) => cssWideKeywords.has(token)))
        return null;
      const canonical = tokens.map((token) =>
        canonicalScrollLength(token, padding),
      );
      if (canonical.some((token) => token === null)) return null;
      if (canonical.length === 4 && canonical.every((item) => item === canonical[0]))
        return canonical[0];
      return canonical.join(" ");
    },
    expandScrollBox = (value) => {
      const tokens = splitTopLevel(value);
      return [
        tokens[0],
        tokens[1] || tokens[0],
        tokens[2] || tokens[0],
        tokens[3] || tokens[1] || tokens[0],
      ];
    },
    serializeScrollBox = (values) => {
      if (values.some((value) => !value)) return "";
      if (values.every((value) => value === values[0])) return values[0];
      if (values[0] === values[2] && values[1] === values[3])
        return values[0] + " " + values[1];
      if (values[1] === values[3])
        return values[0] + " " + values[1] + " " + values[2];
      return values.join(" ");
    },
    canonicalScrollKeywordPair = (value, allowed, defaultSecond) => {
      const tokens = splitTopLevel(String(value).trim().toLowerCase());
      if (
        !tokens.length ||
        tokens.length > 2 ||
        tokens.some((token) => !allowed.has(token))
      )
        return null;
      if (tokens.length === 2 && tokens[1] === defaultSecond) return tokens[0];
      if (tokens.length === 2 && tokens[0] === tokens[1]) return tokens[0];
      return tokens.join(" ");
    },
    canonicalScrollbarColor = (value) => {
      const tokens = splitTopLevel(String(value).trim().toLowerCase());
      if (tokens.length === 1 && (tokens[0] === "auto" || cssWideKeywords.has(tokens[0])))
        return tokens[0];
      if (tokens.length !== 2 || tokens.includes("auto")) return null;
      const color = (token) => {
        const match = token.match(/^#([0-9a-f]{6})$/i);
        if (match)
          return `rgb(${parseInt(match[1].slice(0, 2), 16)}, ${parseInt(
            match[1].slice(2, 4),
            16,
          )}, ${parseInt(match[1].slice(4, 6), 16)})`;
        return /^(?:[a-z]+|rgba?\([^)]*\))$/i.test(token) ? token : null;
      };
      const first = color(tokens[0]),
        second = color(tokens[1]);
      return first && second ? first + " " + second : null;
    },
    canonicalCursor = (value) => {
      let text = String(value).trim();
      const keywords = new Set([
        "auto", "default", "none", "context-menu", "help", "pointer",
        "progress", "wait", "cell", "crosshair", "text", "vertical-text",
        "alias", "copy", "move", "no-drop", "not-allowed", "grab",
        "grabbing", "e-resize", "n-resize", "ne-resize", "nw-resize",
        "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize",
        "ns-resize", "nesw-resize", "nwse-resize", "col-resize",
        "row-resize", "all-scroll", "zoom-in", "zoom-out",
      ]);
      if (keywords.has(text.toLowerCase())) return text.toLowerCase();
      const fallback = text.match(/,\s*([a-z-]+)\s*$/i)?.[1]?.toLowerCase();
      if (!fallback || !keywords.has(fallback)) return null;
      text = text
        .replace(/calc\(\s*(-?\d+(?:\.\d+)?)\s*\+\s*0\s*\)/gi, "calc($1)")
        .replace(
          /image-set\(\s*"([^"]+)"\s+([0-9.]+x)/gi,
          'image-set(url("$1") $2',
        )
        .replace(
          /,\s*"([^"]+)"\s+([0-9.]+x)/gi,
          ', url("$1") $2',
        );
      return text;
    };
  const canonicalAlignment = (kind, value) => {
      value = splitTopLevel(String(value).trim().toLowerCase()).join(" ");
      if (!value) return null;
      if (value === "first baseline") value = "baseline";
      const baseline = value === "baseline" || value === "last baseline",
        distribution = [
          "space-between",
          "space-around",
          "space-evenly",
        ].includes(value),
        position = [
          "center",
          "start",
          "end",
          "self-start",
          "self-end",
          "flex-start",
          "flex-end",
        ].includes(value),
        contentPosition = [
          "center",
          "start",
          "end",
          "flex-start",
          "flex-end",
        ].includes(value),
        side = value === "left" || value === "right",
        overflowPosition =
          /^(?:safe|unsafe) (?:center|start|end|self-start|self-end|flex-start|flex-end)$/.test(
            value,
          ),
        overflowContentPosition =
          /^(?:safe|unsafe) (?:center|start|end|flex-start|flex-end)$/.test(
            value,
          ),
        overflowSide = /^(?:safe|unsafe) (?:left|right)$/.test(value);
      if (kind === "align-items")
        return ["normal", "stretch"].includes(value) ||
          baseline ||
          position ||
          overflowPosition
          ? value
          : null;
      if (kind === "justify-items")
        return ["normal", "stretch", "legacy"].includes(value) ||
          baseline ||
          position ||
          side ||
          overflowPosition ||
          overflowSide ||
          /^(?:legacy (?:left|right|center)|(?:left|right|center) legacy)$/.test(
            value,
          )
          ? /^(?:left|right|center) legacy$/.test(value)
            ? "legacy " + value.split(" ")[0]
            : value
          : null;
      if (kind === "align-self" || kind === "justify-self")
        return ["auto", "normal", "stretch"].includes(value) ||
          baseline ||
          position ||
          (kind === "justify-self" && side) ||
          overflowPosition ||
          (kind === "justify-self" && overflowSide)
          ? value
          : null;
      if (kind === "align-content")
        return ["normal", "stretch"].includes(value) ||
          distribution ||
          baseline ||
          contentPosition ||
          overflowContentPosition
          ? value
          : null;
      if (kind === "justify-content")
        return ["normal", "stretch"].includes(value) ||
          distribution ||
          contentPosition ||
          side ||
          overflowContentPosition ||
          overflowSide
          ? value
          : null;
      return null;
    },
    canonicalAlignmentPair = (value, firstKind, secondKind) => {
      const tokens = splitTopLevel(String(value).trim().toLowerCase());
      if (!tokens.length) return null;
      const whole = tokens.join(" "),
        firstWhole = canonicalAlignment(firstKind, whole),
        secondWhole = canonicalAlignment(secondKind, whole);
      if (firstWhole !== null && secondWhole !== null)
        return [firstWhole, secondWhole];
      if (
        firstKind === "align-content" &&
        secondKind === "justify-content" &&
        ["baseline", "last baseline"].includes(firstWhole)
      )
        return [firstWhole, "start"];
      for (let at = 1; at < tokens.length; at++) {
        const first = canonicalAlignment(firstKind, tokens.slice(0, at).join(" ")),
          second = canonicalAlignment(secondKind, tokens.slice(at).join(" "));
        if (first !== null && second !== null) return [first, second];
      }
      return null;
    };
  const modernCssWide = new Set([
      "inherit", "initial", "unset", "revert", "revert-layer",
    ]),
    canonicalModernMobile = (name, value) => {
      const lower = value.toLowerCase();
      if (modernCssWide.has(lower)) return lower;
      if (name === "-webkit-user-select") name = "user-select";
      if (name === "-webkit-text-size-adjust") name = "text-size-adjust";
      if (name === "user-select")
        return ["auto", "text", "none", "all"].includes(lower)
          ? lower : null;
      if (name === "touch-action") {
        if (["auto", "none", "manipulation", "pan-x", "pan-y"].includes(
          lower,
        )) return lower;
        const tokens = splitTopLevel(lower);
        return tokens.length === 2 && tokens.includes("pan-x")
          && tokens.includes("pan-y") ? "pan-x pan-y" : null;
      }
      if (name === "resize")
        return [
          "none", "both", "horizontal", "vertical", "block", "inline",
        ].includes(lower) ? lower : null;
      if (name === "text-size-adjust") {
        if (lower === "auto" || lower === "none") return lower;
        const match = lower.match(/^(?:0|[0-9]+(?:\.[0-9]+)?)%$/);
        return match && Number(lower.slice(0, -1)) <= 250 ? lower : null;
      }
      if (name === "text-wrap-style")
        return ["auto", "balance", "pretty"].includes(lower) ? lower : null;
      if (name === "text-wrap") {
        const tokens = splitTopLevel(lower);
        if (tokens.length === 1) {
          if (tokens[0] === "auto" || tokens[0] === "wrap") return "wrap";
          return ["balance", "pretty"].includes(tokens[0])
            ? tokens[0] : null;
        }
        if (tokens.length === 2 && tokens.includes("wrap")) {
          const style = tokens.find((token) => token !== "wrap");
          if (style === "auto") return "wrap";
          return ["balance", "pretty"].includes(style) ? style : null;
        }
        return null;
      }
      if (name === "isolation")
        return lower === "auto" || lower === "isolate" ? lower : null;
      if (name === "translate") {
        if (lower === "none") return lower;
        const tokens = splitTopLevel(lower);
        if (tokens.length === 3
            && /^(?:0|0(?:\.0+)?px)$/.test(tokens[2])) tokens.pop();
        if (tokens.length < 1 || tokens.length > 2) return null;
        for (let at = 0; at < tokens.length; at++) {
          if (/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|%)?$/.test(tokens[at])) {
            if (!/[a-z%]$/i.test(tokens[at])) {
              if (Number(tokens[at]) !== 0) return null;
              tokens[at] = "0px";
            }
            continue;
          }
          return null;
        }
        if (tokens.length === 2 && tokens[1] === "0px") tokens.pop();
        return tokens.join(" ");
      }
      if (name === "rotate") {
        if (lower === "none") return lower;
        const match = lower.match(
          /^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(deg|turn)$/,
        );
        if (!match) return null;
        const degrees = Number(match[1]) * (match[2] === "turn" ? 360 : 1);
        return Number.isFinite(degrees) && Math.abs(degrees / 90
          - Math.round(degrees / 90)) < 1e-7 ? lower : null;
      }
      if (name === "scale") {
        if (lower === "none") return lower;
        const tokens = splitTopLevel(lower);
        if (tokens.length < 1 || tokens.length > 2
            || tokens.some((token) => !/^(?:\d+(?:\.\d*)?|\.\d+)$/.test(token))) {
          return null;
        }
        if (tokens.length === 2 && Number(tokens[0]) !== Number(tokens[1])) {
          return null;
        }
        return String(Number(tokens[0]));
      }
      if ([
        "border-start-start-radius", "border-start-end-radius",
        "border-end-start-radius", "border-end-end-radius",
      ].includes(name)) {
        if (lower === "0") return "0px";
        return /^(?:\d+(?:\.\d*)?|\.\d+)px$/.test(lower) ? lower : null;
      }
      return undefined;
    };
  const canonicalValue = (name, value) => {
    name = cssName(name);
    const raw = String(value),
      trimmed = raw.trim();
    const modern = canonicalModernMobile(name, trimmed);
    if (modern !== undefined) return modern;
    if (name === "display") return canonicalDisplay(raw);
    if (scrollBoxLonghands[name]) return canonicalScrollBox(name, trimmed);
    if (
      name.startsWith("scroll-margin-") ||
      name.startsWith("scroll-padding-")
    )
      return canonicalScrollLength(
        trimmed,
        name.startsWith("scroll-padding-"),
      );
    if (name === "scroll-snap-type") {
      if (trimmed.toLowerCase() === "none") return "none";
      const tokens = splitTopLevel(trimmed.toLowerCase());
      if (
        !tokens.length ||
        tokens.length > 2 ||
        !["x", "y", "block", "inline", "both"].includes(tokens[0]) ||
        (tokens.length === 2 &&
          !["mandatory", "proximity"].includes(tokens[1]))
      )
        return null;
      return tokens[1] === "proximity" ? tokens[0] : tokens.join(" ");
    }
    if (name === "scroll-snap-align")
      return canonicalScrollKeywordPair(
        trimmed,
        new Set(["none", "start", "end", "center"]),
        "",
      );
    if (name === "scroll-snap-stop")
      return ["normal", "always"].includes(trimmed.toLowerCase())
        ? trimmed.toLowerCase()
        : null;
    if (name === "overscroll-behavior")
      return canonicalScrollKeywordPair(
        trimmed,
        new Set(["auto", "contain", "none", "chain"]),
        "",
      );
    if (
      [
        "overscroll-behavior-x",
        "overscroll-behavior-y",
        "overscroll-behavior-inline",
        "overscroll-behavior-block",
      ].includes(name)
    )
      return ["auto", "contain", "none", "chain"].includes(
        trimmed.toLowerCase(),
      )
        ? trimmed.toLowerCase()
        : null;
    if (name === "scrollbar-width")
      return ["auto", "thin", "none"].includes(trimmed.toLowerCase())
        ? trimmed.toLowerCase()
        : null;
    if (name === "scrollbar-color") return canonicalScrollbarColor(trimmed);
    if (name === "cursor") return canonicalCursor(trimmed);
    if (
      (name === "flex-grow" || name === "flex-shrink") &&
      numberPattern.test(trimmed)
    )
      return canonicalNumber(trimmed);
    if (
      [
        "width",
        "min-width",
        "max-width",
        "height",
        "min-height",
        "max-height",
        "flex-basis",
      ].includes(name) &&
      numberPattern.test(trimmed) &&
      Number(trimmed) === 0
    )
      return "0px";
    if (name === "gap") {
      const values = splitTopLevel(trimmed);
      return values.length === 2 && values[0] === values[1]
        ? values[0]
        : trimmed;
    }
    if (
      [
        "align-items",
        "justify-items",
        "align-self",
        "justify-self",
        "align-content",
        "justify-content",
      ].includes(name)
    )
      return canonicalAlignment(name, trimmed);
    if (
      name === "grid-template-columns" ||
      name === "grid-template-rows"
    )
      return trimmed
        .replace(/\[\s*\]\s*/g, "")
        .replace(/\s+/g, " ")
        .replace(/\s+([,)])/g, "$1")
        .trim();
    if (name === "grid-template-areas") {
      if (trimmed === "none") return "none";
      const rows = [];
      let at = 0;
      while (at < trimmed.length) {
        while (at < trimmed.length && /\s/.test(trimmed[at])) at++;
        if (at >= trimmed.length) break;
        const quote = trimmed[at];
        if ((quote !== '"' && quote !== "'") || rows.length >= 8) return null;
        at++;
        const cells = [];
        while (at < trimmed.length && trimmed[at] !== quote) {
          while (at < trimmed.length && /\s/.test(trimmed[at])) at++;
          if (trimmed[at] === quote) break;
          if (cells.length >= 12) return null;
          if (trimmed[at] === ".") {
            while (trimmed[at] === ".") at++;
            cells.push(".");
            continue;
          }
          const start = at;
          while (
            at < trimmed.length &&
            trimmed[at] !== quote &&
            trimmed[at] !== "." &&
            !/\s/.test(trimmed[at])
          ) {
            const code = trimmed.charCodeAt(at);
            if (
              !(
                (code >= 48 && code <= 57) ||
                (code >= 65 && code <= 90) ||
                (code >= 97 && code <= 122) ||
                trimmed[at] === "_" ||
                trimmed[at] === "-" ||
                code >= 128
              )
            )
              return null;
            at++;
          }
          if (start === at) return null;
          cells.push(trimmed.slice(start, at));
        }
        if (at >= trimmed.length || cells.length === 0) return null;
        at++;
        if (rows.length && cells.length !== rows[0].length) return null;
        rows.push(cells);
      }
      if (!rows.length) return null;
      const names = new Set(rows.flat().filter((cell) => cell !== "."));
      if (names.size > 12) return null;
      for (const name of names) {
        let top = rows.length,
          left = rows[0].length,
          bottom = 0,
          right = 0;
        for (let row = 0; row < rows.length; row++)
          for (let column = 0; column < rows[row].length; column++)
            if (rows[row][column] === name) {
              top = Math.min(top, row);
              left = Math.min(left, column);
              bottom = Math.max(bottom, row + 1);
              right = Math.max(right, column + 1);
            }
        for (let row = top; row < bottom; row++)
          for (let column = left; column < right; column++)
            if (rows[row][column] !== name) return null;
      }
      return rows.map((row) => `"${row.join(" ")}"`).join(" ");
    }
    if (name === "outline") return canonicalOutline(trimmed);
    if (name === "text-transform") return canonicalTextTransform(trimmed);
    if (name === "object-position") return canonicalObjectPosition(trimmed);
    if (/var\(/i.test(raw)) {
      const match = trimmed.match(
        /^var\(\s*(--[-_a-zA-Z0-9\u0080-\uFFFF]+)\s*(?:,[\s\S]*)?\)$/,
      );
      if (!match) return null;
    }
    return raw;
  };
  globalThis.__tilefinchMakeStyle = (handle) => {
    let cssomFallback = null;
    const authoredStyle = () =>
        __tilefinchGetAttribute(handle, "style") || "",
      /* Only a custom element observes its style attribute. Others need
         no before/after snapshot, which costs two attribute reads per
         write; undefined marks "not observed" for notifyStyle. */
      observedStyle = () =>
        globalThis.__tilefinchCustomElementAttributeChanged &&
        globalThis.__tilefinchWrap?.(handle)
          ?.__tilefinchCustomElementState === "custom"
          ? authoredStyle()
          : undefined,
      notifyStyle = (oldValue) => {
        if (oldValue === undefined) return;
        const newValue = authoredStyle();
        if (newValue === oldValue) return;
        globalThis.__tilefinchCustomElementAttributeChanged?.(
          globalThis.__tilefinchWrap?.(handle),
          "style",
          oldValue || null,
          newValue || null,
          null,
        );
      };
    const writeRaw = (name, value) => {
        try {
          if (__tilefinchStyleSet(handle, name, value)) {
            cssomFallback?.delete(name);
            globalThis.__tilefinchQueueFocusFixup?.(handle);
            return true;
          }
        } catch (error) {
          if (error !== null) throw error;
        }
        if (value === "") cssomFallback?.delete(name);
        else (cssomFallback || (cssomFallback = new Map())).set(name, value);
        return true;
      },
      write = (name, value) => {
        const key = effectKey(handle, name);
        cancelEffect(key);
        if (
          value === "" ||
          name.startsWith("transition") ||
          name.startsWith("animation")
        ) {
          const written = writeRaw(name, value);
          if (name.startsWith("animation")) {
            try {
              globalThis.__tilefinchEnsureMotionBootstrap?.();
            } catch {}
            globalThis.__tilefinchBeginMotionObservation?.();
            globalThis.__tilefinchMotionRecheck?.();
          }
          return written;
        }
        /* Tilefinch currently retains transition timing from inline style.
           Avoid computing the property's old value on the overwhelmingly
           common path where this element has no transition declaration. */
        /* Inline transition declarations live in the style attribute, so
           one read and a case-insensitive scan rule them out before the two
           declaration lookups. */
        const transitionHint =
          (/transition/i.test(authoredStyle()) &&
            (__tilefinchStyleGet(handle, "transition-duration") ||
              __tilefinchStyleGet(handle, "transition"))) ||
          cssomFallback?.get("transition-duration") ||
          cssomFallback?.get("transition");
        if (!transitionHint) {
          const written = writeRaw(name, value);
          globalThis.__tilefinchTransitionsDirty?.(handle);
          return written;
        }
        const node = globalThis.__tilefinchWrap?.(handle),
          before = node
            ? getComputedStyle(node).getPropertyValue(name)
            : "";
        if (!writeRaw(name, value) || !node) return true;
        const timing = computedTransitionTiming(node, name);
        if (
          timing &&
          Math.max(0, timing.duration) + timing.delay > 0 &&
          before &&
          String(before) !== String(value)
        ) {
          let effect = null;
          if (timing.duration > 0) {
            writeRaw(name, before);
            effect = scheduleEffect({
              handle,
              property: name,
              from: before,
              to: value,
              duration: Math.min(durationLimit, timing.duration),
              /* A negative delay starts the effect part-way through. */
              delay: Math.min(durationLimit, timing.delay),
              writeRaw,
            });
            if (!effect) writeRaw(name, value);
          }
          startTransition(node, name, timing.duration, timing.delay, "inline",
            effect);
        }
        globalThis.__tilefinchTransitionsDirty?.(handle);
        return true;
      },
      clearFlex = () => {
        write("flex", "");
        write("flex-grow", "");
        write("flex-shrink", "");
        write("flex-basis", "");
      },
      placeLonghands = {
        "place-items": ["align-items", "justify-items"],
        "place-self": ["align-self", "justify-self"],
        "place-content": ["align-content", "justify-content"],
      },
      clearPlace = (name) => {
        const longhands = placeLonghands[name];
        write(name, "");
        write(longhands[0], "");
        write(longhands[1], "");
      };
    const base = {
      get cssText() {
        return authoredStyle();
      },
      set cssText(value) {
        const oldValue = observedStyle();
        value = String(value);
        __tilefinchSetAttribute(handle, "style", value);
        globalThis.__tilefinchTransitionsDirty?.(handle);
        globalThis.__tilefinchQueueFocusFixup?.(handle);
        notifyStyle(oldValue);
      },
      setProperty(name, value, priority = "") {
        const oldValue = observedStyle();
        try {
        name = cssName(name);
        if (name === "--") return false;
        value = String(value);
        priority = String(priority).trim().toLowerCase();
        if (value === "") {
          this.removeProperty(name);
          return true;
        }
        if (priority !== "" && priority !== "important") return false;
        const prioritized = (canonical) =>
          canonical + (priority === "important" ? " !important" : "");
        if (name === "flex") {
          const parsed = canonicalFlex(value);
          if (parsed === null) return false;
          write("flex", "");
          return (
            write("flex-grow", prioritized(parsed.grow)) &&
            write("flex-shrink", prioritized(parsed.shrink)) &&
            write("flex-basis", prioritized(parsed.basis))
          );
        }
        if (placeLonghands[name]) {
          const longhands = placeLonghands[name],
            parsed = canonicalAlignmentPair(
              value,
              longhands[0],
              longhands[1],
            );
          if (parsed === null) return false;
          write(name, "");
          return (
            write(longhands[0], prioritized(parsed[0])) &&
            write(longhands[1], prioritized(parsed[1]))
          );
        }
        if (scrollBoxLonghands[name]) {
          const parsed = canonicalScrollBox(name, value);
          if (parsed === null) return false;
          const expanded = expandScrollBox(parsed);
          write(name, "");
          return scrollBoxLonghands[name].every((longhand, index) =>
            write(longhand, prioritized(expanded[index])),
          );
        }
        const canonical = canonicalValue(name, value);
        return canonical === null ? false : write(name, prioritized(canonical));
        } finally {
          notifyStyle(oldValue);
        }
      },
      getPropertyValue(name) {
        name = cssName(name);
        if (name === "flex") {
          const grow = this.getPropertyValue("flex-grow"),
            shrink = this.getPropertyValue("flex-shrink"),
            basis = this.getPropertyValue("flex-basis");
          if (!grow && !shrink && !basis) return "";
          return (
            (grow || "0") +
            " " +
            (shrink || "1") +
            " " +
            (basis || "auto")
          );
        }
        if (placeLonghands[name]) {
          const longhands = placeLonghands[name],
            first = this.getPropertyValue(longhands[0]),
            second = this.getPropertyValue(longhands[1]);
          if (!first || !second) return "";
          return first === second ? first : first + " " + second;
        }
        if (scrollBoxLonghands[name])
          return serializeScrollBox(
            scrollBoxLonghands[name].map((longhand) =>
              this.getPropertyValue(longhand),
            ),
          );
        const target = authoredTargets.get(effectKey(handle, name));
        const value = (
          target ??
          __tilefinchStyleGet(handle, name) ??
          cssomFallback?.get(name) ??
          ""
        );
        return String(value).replace(/\s*!\s*important\s*$/i, "");
      },
      getPropertyPriority(name) {
        name = cssName(name);
        const longhands = name === "flex"
          ? ["flex-grow", "flex-shrink", "flex-basis"]
          : placeLonghands[name] || scrollBoxLonghands[name] || null;
        if (longhands)
          return longhands.every((longhand) =>
            this.getPropertyValue(longhand) !== "" &&
            this.getPropertyPriority(longhand) === "important")
            ? "important" : "";
        const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"),
          match = authoredStyle().match(
            new RegExp("(?:^|;)\\s*" + escaped +
              "\\s*:[^;]*!\\s*important\\s*(?:;|$)", "i"));
        return match ? "important" : "";
      },
      removeProperty(name) {
        const oldStyle = observedStyle();
        name = cssName(name);
        const old = this.getPropertyValue(name);
        if (name === "flex") clearFlex();
        else if (placeLonghands[name]) clearPlace(name);
        else if (scrollBoxLonghands[name]) {
          write(name, "");
          for (const longhand of scrollBoxLonghands[name])
            write(longhand, "");
        }
        else write(name, "");
        notifyStyle(oldStyle);
        return old;
      },
    };
    return declarationProxy(base, () => declaredNames(authoredStyle()), (
      target, name, value) => {
        if (name === "cssText") {
          target.cssText = value;
          return true;
        }
        target.setProperty(name, value);
        return true;
      });
  };
  globalThis.__tilefinchMakeDetachedStyle = (node) => {
    let text = "", values = Object.create(null), priorities = Object.create(null),
      count = 0;
    const limit = 128,
      parse = (source) => {
        text = String(source).slice(0, 16 * 1024);
        values = Object.create(null);
        priorities = Object.create(null);
        count = 0;
        for (const part of text.split(";")) {
          const at = part.indexOf(":"), name = cssName(part.slice(0, at).trim());
          if (at > 0 && name && !(name in values) && count < limit) {
            let value = part.slice(at + 1).trim();
            if (/!\s*important\s*$/i.test(value)) {
              priorities[name] = "important";
              value = value.replace(/!\s*important\s*$/i, "").trim();
            }
            values[name] = value;
            count++;
          }
        }
      },
      publish = () => {
        text = Object.keys(values).slice(0, limit)
          .map((name) => name + ": " + values[name] +
            (priorities[name] ? " !important" : "")).join("; ");
        if (text) node.setAttribute("style", text);
        else node.removeAttribute("style");
      },
      base = {
        get cssText() { return text; },
        set cssText(value) { parse(value); publish(); },
        setProperty(name, value, priority = "") {
          name = cssName(name);
          value = String(value);
          priority = String(priority).trim().toLowerCase();
          const present = name in values;
          if (!name || (!present && count >= limit)) return;
          if (value === "") {
            if (present) count--;
            delete values[name];
            delete priorities[name];
          } else if (priority !== "" && priority !== "important") {
            return;
          } else {
            const canonical = canonicalValue(name, value);
            if (canonical === null) return;
            if (!present) count++;
            values[name] = canonical;
            if (priority) priorities[name] = priority;
            else delete priorities[name];
          }
          publish();
        },
        getPropertyValue(name) { return values[cssName(name)] || ""; },
        getPropertyPriority(name) {
          return priorities[cssName(name)] || "";
        },
        removeProperty(name) {
          name = cssName(name);
          const old = values[name] || "";
          if (name in values) {
            delete values[name];
            delete priorities[name];
            count--;
          }
          publish();
          return old;
        },
      };
    parse(node.getAttribute("style") || "");
    return declarationProxy(base, () => Object.keys(values), (
      target, name, value) => {
        if (name === "cssText") target.cssText = value;
        else target.setProperty(name, value);
        return true;
      });
  };
})();
