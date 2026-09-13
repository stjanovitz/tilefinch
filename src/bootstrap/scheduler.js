(() => {
  /* See dom.js: the timer clock mirror is written on the first pump, which
     is well after hardening.js has taken its snapshot. */
  Object.defineProperty(globalThis, "__tilefinchNow", {
    enumerable: false,
    configurable: false,
    writable: true,
    value: 0,
  });
  const trustedWrap = globalThis.__tilefinchWrap,
    pageVisible = globalThis.__tilefinchPageVisible,
    schedulerNow = globalThis.__tilefinchSchedulerNow,
    createMessageEvent = globalThis.__tilefinchCreateMessageEvent,
    diagnosticString = String,
    diagnosticOwnDescriptor = Object.getOwnPropertyDescriptor,
    timerApply = Reflect.apply;
  Object.defineProperty(globalThis, "__tilefinchDiagnosticLookup", {
    enumerable: false,
    configurable: false,
    writable: false,
    value(name) {
      const descriptor = diagnosticOwnDescriptor(
        globalThis,
        diagnosticString(name),
      );
      if (!descriptor || typeof descriptor.value !== "function")
        throw new ReferenceError("diagnostic function is not a global export");
      return descriptor.value;
    },
  });
  const initialSchedulerSample = Number(schedulerNow()),
    usesSampledClock = Number.isFinite(initialSchedulerSample) &&
      initialSchedulerSample >= 0;
  let now = usesSampledClock ? initialSchedulerSample : 0,
    nextId = 1,
    lastCallbackAt = -1,
    activeTimer = null,
    activeCancelAttempts = 0,
    activeCancelHits = 0,
    timeoutCallbacks = 0,
    intervalCallbacks = 0;
  const timers = [];
  const limit = 128;
  /* The frame loop registers an animation-frame timer every presented frame.
     Reusing retired records and one shared empty argument list keeps the
     steady game loop free of per-frame allocations, which would otherwise
     pace QuickJS's full collections onto gameplay frames. */
  const EMPTY_TIMER_ARGS = Object.freeze([]);
  const timerPool = [];
  let retryMessageDrains = () => {};
  /* HTML timer delays use Web IDL `long` conversion before negative values
     are clamped. JavaScript's ToInt32 operator is the same modulo-2^32,
     truncate-toward-zero conversion and evaluates an author coercion once. */
  const normalizeTimerDelay = (value) => {
    const signed = Number(value) >> 0;
    return signed > 0 ? signed : 0;
  };
  globalThis.__tilefinchNormalizeTimerDelay = normalizeTimerDelay;
  const currentSchedulerTime = () => {
    if (usesSampledClock) {
      const sampled = Number(schedulerNow());
      if (Number.isFinite(sampled) && sampled >= now) now = sampled;
    }
    return now;
  };
  Object.defineProperty(globalThis.__tilefinchRootCensus, "timers", {
    get: () => timers.length,
  });
  function schedule(callback, delay, repeat, args, kind = "timeout") {
    if (typeof callback !== "function" || timers.length >= limit) return 0;
    const id = nextId++;
    const span = Math.max(repeat ? 1 : 0, normalizeTimerDelay(delay)),
      registeredAt = currentSchedulerTime();
    const timer = timerPool.pop();
    if (timer) {
      timer.id = id;
      timer.callback = callback;
      timer.due = registeredAt + span;
      timer.span = span;
      timer.repeat = repeat;
      timer.args = args;
      timer.kind = kind;
      timers.push(timer);
    } else {
      timers.push({
        id, callback, due: registeredAt + span, span, repeat, args, kind,
      });
    }
    return id;
  }
  function releaseTimer(timer) {
    timer.callback = null;
    timer.args = EMPTY_TIMER_ARGS;
    if (timerPool.length < limit) timerPool.push(timer);
  }
  function clear(id) {
    const numericId = Number(id);
    if (activeTimer && activeTimer.id === numericId) {
      activeCancelAttempts++;
      if (activeTimer.repeat) {
        activeTimer.repeat = false;
        activeCancelHits++;
      }
      return;
    }
    const at = timers.findIndex((timer) => timer.id === numericId);
    if (at >= 0) releaseTimer(timers.splice(at, 1)[0]);
  }
  const timerPriority = (timer) =>
    timer.kind === "animation-frame"
      ? 0
      : timer.kind === "render-observer"
        ? 1
        : timer.kind === "render-fixup"
          ? 2
          : 3;
  const timerOrder = (a, b) =>
    a.due - b.due || timerPriority(a) - timerPriority(b) || a.id - b.id;
  function invokeTimer() {
    /* Detach before calling so author callbacks never observe the reusable
       timer record as `this`. */
    const callback = this.callback;
    try {
      lastCallbackAt = now;
      if (this.kind === "interval") intervalCallbacks++;
      else timeoutCallbacks++;
      /* Animation timestamps describe the frame that is actually being
         serviced. A late browser tick therefore skips time instead of
         replaying a backlog of synthetic 16 ms frames. Visual timers
         remain queued while the owning document is hidden. */
      if (this.kind === "animation-frame") callback(now);
      /* HTML timers invoke callable handlers with the owning Window as their
         callback this-value.  A plain call only happens to work for sloppy
         functions; strict functions observe undefined and real-world state
         machines use that distinction.  Keep animation-frame's existing
         callback contract separate. */
      else timerApply(callback, globalThis, this.args);
    } catch (error) {
      __tilefinchReportUncaught(error, "timer callback");
    }
  }
  const scheduleTimeout = (callback, delay, ...args) =>
      schedule(callback, delay, false, args, "timeout"),
    scheduleInterval = (callback, delay, ...args) =>
      schedule(callback, delay, true, args, "interval");
  /* Lazy platform groups can be installed after page code has replaced the
     public timer names. Keep the browser-owned scheduler capabilities under
     hardening-protected names so Worker/FileReader lifetime cleanup cannot be
     redirected through author code. */
  globalThis.__tilefinchScheduleTimeout = scheduleTimeout;
  globalThis.__tilefinchScheduleInterval = scheduleInterval;
  globalThis.__tilefinchCancelTimer = clear;
  globalThis.setTimeout = scheduleTimeout;
  globalThis.setInterval = scheduleInterval;
  globalThis.clearTimeout = clear;
  globalThis.clearInterval = clear;
  globalThis.requestAnimationFrame = (callback) => {
    if (typeof callback !== "function")
      throw new TypeError("callback must be a function");
    /* The dispatcher invokes animation-frame callbacks with exactly one
       timestamp argument, so the author callback needs no adapter. */
    return schedule(callback, 16, false, EMPTY_TIMER_ARGS, "animation-frame");
  };
  globalThis.cancelAnimationFrame = clear;
  globalThis.__tilefinchScheduleRenderObserver = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "render-observer");
  globalThis.__tilefinchScheduleRenderFixup = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "render-fixup");
  if (globalThis.MessageEvent === undefined)
    globalThis.MessageEvent = class MessageEvent extends Event {
      constructor(type, options = {}) {
        super(type, options);
        this.data = options.data ?? null;
        this.origin = String(options.origin || "");
        this.lastEventId = String(options.lastEventId || "");
        this.source = options.source ?? null;
        this.ports = Array.isArray(options.ports) ? options.ports : [];
      }
    };
  {
    const token = {},
      portBrands = new WeakSet(),
      channelLimit = 8,
      messageQueueLimit = 32,
      blockedDrains = new Set();
    let channelCount = 0;
    const scheduleEndpointDrain = (endpoint) => {
      if (endpoint.drainPending || endpoint.queue.length === 0) return true;
      const receiver = endpoint.receiver;
      if (!receiver || receiver._closed || !receiver._started) {
        blockedDrains.delete(endpoint);
        return true;
      }
      const id = schedule(() => {
        endpoint.drainPending = false;
        const current = endpoint.receiver;
        if (!current || current._closed || !current._started) return;
        const item = endpoint.queue.shift();
        if (item) current._deliver(item.data, item.ports);
        if (endpoint.queue.length) scheduleEndpointDrain(endpoint);
      }, 0, false, EMPTY_TIMER_ARGS, "message");
      if (!id) {
        blockedDrains.add(endpoint);
        return false;
      }
      endpoint.drainPending = true;
      blockedDrains.delete(endpoint);
      return true;
    };
    retryMessageDrains = () => {
      for (const endpoint of blockedDrains) {
        if (!scheduleEndpointDrain(endpoint)) break;
      }
    };
    class MessagePort extends EventTarget {
      constructor(key, owner, endpoint = null) {
        super();
        if (key !== token) throw new TypeError("Illegal constructor");
        portBrands.add(this);
        this._owner = owner;
        /* Scheduled tasks belong to the transferable endpoint, not to one
           JavaScript wrapper. Transfer redirects the endpoint atomically so
           tasks queued before the move reach the receiving port. */
        this._endpoint = endpoint || {
          receiver: this,
          queue: [],
          drainPending: false,
        };
        this._peer = null;
        this._closed = false;
        this._transferPending = false;
        this._started = false;
        this._onmessage = null;
        this._onmessageerror = null;
        this._onmessageListener = (event) =>
          this._onmessage?.call(this, event);
        this._onmessageerrorListener = (event) =>
          this._onmessageerror?.call(this, event);
      }
      get onmessage() {
        return this._onmessage;
      }
      set onmessage(callback) {
        const next = typeof callback === "function" ? callback : null;
        if (this._onmessage === null && next !== null)
          super.addEventListener("message", this._onmessageListener);
        else if (this._onmessage !== null && next === null)
          super.removeEventListener("message", this._onmessageListener);
        this._onmessage = next;
        if (this._onmessage) this.start();
      }
      get onmessageerror() {
        return this._onmessageerror;
      }
      set onmessageerror(callback) {
        const next = typeof callback === "function" ? callback : null;
        if (this._onmessageerror === null && next !== null)
          super.addEventListener(
            "messageerror", this._onmessageerrorListener);
        else if (this._onmessageerror !== null && next === null)
          super.removeEventListener(
            "messageerror", this._onmessageerrorListener);
        this._onmessageerror = next;
      }
      start() {
        if (this._closed || this._started) return;
        this._started = true;
        scheduleEndpointDrain(this._endpoint);
      }
      close() {
        if (this._closed) return;
        this._closed = true;
        if (this._endpoint.receiver === this) {
          this._endpoint.receiver = null;
          this._endpoint.queue.length = 0;
          blockedDrains.delete(this._endpoint);
        }
        if (this._onmessage !== null)
          super.removeEventListener("message", this._onmessageListener);
        if (this._onmessageerror !== null)
          super.removeEventListener(
            "messageerror", this._onmessageerrorListener);
        this._onmessage = null;
        this._onmessageerror = null;
        this._owner.closed++;
        if (this._owner.closed === 2) channelCount--;
      }
      _enqueue(data, ports = []) {
        if (this._closed) return;
        const endpoint = this._endpoint;
        if (endpoint.queue.length >= messageQueueLimit) return;
        endpoint.queue.push({ data, ports });
        /* A refused scheduler slot leaves the bounded FIFO intact. The timer
           pump retries it once capacity becomes available. */
        scheduleEndpointDrain(endpoint);
      }
      _deliver(data, ports = []) {
        if (this._closed || !this._started) return;
        const event = trusted(createMessageEvent(
          "message", data, "", null, ports));
        this.dispatchEvent(event);
      }
      postMessage(value, transfer = []) {
        if (this._closed) return;
        const ports = [],
          copied = globalThis.__tilefinchCloneWorkerValue(
            value, globalThis.__tilefinchWorkerCloneIntrinsics.owner,
            transfer, ports),
          target = this._peer;
        if (target && !target._closed) target._enqueue(copied, ports);
      }
    }
    class MessageChannel {
      constructor() {
        if (channelCount >= channelLimit)
          throw new RangeError("message channel quota exceeded");
        channelCount++;
        const owner = { closed: 0 };
        this.port1 = new MessagePort(token, owner);
        this.port2 = new MessagePort(token, owner);
        this.port1._peer = this.port2;
        this.port2._peer = this.port1;
      }
    }
    globalThis.MessagePort = MessagePort;
    globalThis.MessageChannel = MessageChannel;
    /* Structured clone prepares a replacement endpoint first and commits the
       move only after the entire message has cloned successfully. This keeps
       transfer-list failures transactional. */
    Object.defineProperty(globalThis, "__tilefinchPrepareMessagePortTransfer", {
      configurable: false,
      enumerable: false,
      writable: false,
      value(port, targetPrototype) {
        if (!portBrands.has(port) || port._closed || port._transferPending)
          return null;
        const peer = port._peer,
          endpoint = port._endpoint,
          copy = new MessagePort(token, port._owner, endpoint);
        if (targetPrototype && typeof targetPrototype === "object")
          Object.setPrototypeOf(copy, targetPrototype);
        let committed = false;
        return {
          original: port,
          peer,
          copy,
          canCommit() {
            return !committed && !port._closed && port._peer === peer;
          },
          commit() {
            if (!this.canCommit()) return false;
            committed = true;
            port._transferPending = true;
            copy._peer = peer;
            /* Transfer creates a new receiving port whose message queue is
               disabled until onmessage is assigned or start() is called. */
            copy._started = false;
            endpoint.receiver = copy;
            if (peer && peer._peer === port) peer._peer = copy;
            port._peer = null;
            port._closed = true;
            if (port._onmessage !== null)
              EventTarget.prototype.removeEventListener.call(
                port, "message", port._onmessageListener);
            if (port._onmessageerror !== null)
              EventTarget.prototype.removeEventListener.call(
                port, "messageerror", port._onmessageerrorListener);
            port._onmessage = null;
            port._onmessageerror = null;
            return true;
          },
        };
      },
    });
  }
  {
    const channels = new Map(),
      channelLimit = 32,
      nameLimit = 256;
    let channelCount = 0;
    class BroadcastChannel extends EventTarget {
      constructor(name) {
        super();
        if (arguments.length === 0)
          throw new TypeError("Broadcast channel name required");
        name = String(name);
        if (name.length > nameLimit)
          throw new DOMException(
            "Broadcast channel name exceeds bounded length",
            "QuotaExceededError",
          );
        if (channelCount >= channelLimit)
          throw new DOMException(
            "Broadcast channel limit reached",
            "QuotaExceededError",
          );
        this.__name = name;
        this.__closed = false;
        this.onmessage = null;
        this.onmessageerror = null;
        let group = channels.get(name);
        if (!group) {
          group = new Set();
          channels.set(name, group);
        }
        group.add(this);
        channelCount++;
      }
      get name() {
        return this.__name;
      }
      postMessage(value) {
        if (this.__closed)
          throw new DOMException(
            "Broadcast channel is closed",
            "InvalidStateError",
          );
        if (arguments.length === 0)
          throw new TypeError("Broadcast message required");
        const group = channels.get(this.__name);
        if (!group) return;
        const message = structuredClone(value);
        for (const target of group) {
          if (target === this || target.__closed) continue;
          const data = structuredClone(message);
          if (
            !schedule(
              () => {
                if (target.__closed) return;
                const event = trusted(createMessageEvent(
                  "message", data,
                  String(globalThis.location?.origin || ""), null, []));
                if (typeof target.onmessage === "function")
                  try {
                    globalThis.__tilefinchRunTask(
                      "broadcast-channel-handler",
                      target.onmessage,
                      target,
                      [event],
                    );
                  } catch (error) {
                    __tilefinchReportUncaught(error, "broadcast channel");
                  }
                target.dispatchEvent(event);
              },
              0,
              false,
              [],
              "message",
            )
          )
            throw new DOMException(
              "Message task limit reached",
              "QuotaExceededError",
            );
        }
      }
      close() {
        if (this.__closed) return;
        this.__closed = true;
        const group = channels.get(this.__name);
        group?.delete(this);
        if (group?.size === 0) channels.delete(this.__name);
        channelCount--;
      }
    }
    Object.defineProperty(globalThis.__tilefinchRootCensus, "broadcastChannels", {
      get: () => channelCount,
    });
    globalThis.BroadcastChannel = BroadcastChannel;
  }
  globalThis.requestIdleCallback = (callback, options = {}) => {
    if (typeof callback !== "function")
      throw new TypeError("callback required");
    const requested = now,
      timeout =
        options && options.timeout !== undefined
          ? Math.max(0, Number(options.timeout) || 0)
          : Infinity;
    return schedule(
      () => {
        const started = __tilefinchPerformanceSample(11);
        globalThis.__tilefinchRunTask(
          "idle-callback",
          callback,
          globalThis,
          [
            {
              didTimeout: now - requested >= timeout,
              timeRemaining() {
                return Math.max(
                  0,
                  8 - (__tilefinchPerformanceNow(7) - started),
                );
              },
            },
          ],
        );
      },
      1,
      false,
      [],
      "idle",
    );
  };
  globalThis.cancelIdleCallback = clear;
  {
    const listeners = new Map();
    const viewport = {
      width: innerWidth,
      height: innerHeight,
      offsetLeft: 0,
      offsetTop: 0,
      pageLeft: 0,
      pageTop: 0,
      scale: globalThis.__tilefinchDiagnosticMobileSafari
        ? 1
        : Number(globalThis.__tilefinchViewportScale) || 1,
      onresize: null,
      onscroll: null,
      addEventListener(type, callback) {
        if (typeof callback !== "function") return;
        const key = String(type);
        if (!listeners.has(key)) listeners.set(key, []);
        const list = listeners.get(key);
        if (!list.includes(callback)) list.push(callback);
      },
      removeEventListener(type, callback) {
        const list = listeners.get(String(type));
        if (!list) return;
        const at = list.indexOf(callback);
        if (at >= 0) list.splice(at, 1);
      },
      dispatchEvent(event) {
        event.target = viewport;
        const type = String(event.type);
        for (const callback of [...(listeners.get(type) || [])])
          try {
            globalThis.__tilefinchRunTask(
              "visual-viewport:" + type,
              callback,
              viewport,
              [event],
            );
          } catch (error) {
            __tilefinchReportUncaught(error, "visualViewport " + type);
          }
        const handler = viewport["on" + type];
        if (typeof handler === "function")
          try {
            globalThis.__tilefinchRunTask(
              "visual-viewport-handler:" + type,
              handler,
              viewport,
              [event],
            );
          } catch (error) {
            __tilefinchReportUncaught(error, "visualViewport " + type);
          }
        return !event.defaultPrevented;
      },
    };
    globalThis.visualViewport = viewport;
  }
  {
    let pageTop = 0,
      pending = false,
      smoothGeneration = 0;
    const apply = (value, notify = true, smoothContinuation = false) => {
      if (!smoothContinuation) smoothGeneration++;
      const next = Math.max(0, Math.min(2147483647, Number(value) || 0));
      if (next === pageTop) return false;
      pageTop = next;
      visualViewport.pageTop = next;
      if (notify) pending = true;
      return true;
    };
    Object.defineProperties(globalThis, {
      scrollX: {
        configurable: true,
        enumerable: true,
        get() {
          return 0;
        },
      },
      pageXOffset: {
        configurable: true,
        enumerable: true,
        get() {
          return 0;
        },
      },
      scrollY: {
        configurable: true,
        enumerable: true,
        get() {
          return pageTop;
        },
      },
      pageYOffset: {
        configurable: true,
        enumerable: true,
        get() {
          return pageTop;
        },
      },
    });
    globalThis.__tilefinchApplyPageScroll = apply;
    globalThis.__tilefinchFlushPageScroll = () => {
      if (!pending) return false;
      pending = false;
      globalThis.dispatchEvent(__tilefinchTrustedEvent(new Event("scroll")));
      visualViewport.dispatchEvent(__tilefinchTrustedEvent(new Event("scroll")));
      return true;
    };
    globalThis.scrollTo = (xOrOptions, y) => {
      const scrollingElement = document.scrollingElement,
        top =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? Number(xOrOptions.top) || 0
            : Number(y) || 0,
        requestedBehavior =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? String(xOrOptions.behavior || "auto")
            : "auto",
        behavior =
          requestedBehavior === "auto" &&
          scrollingElement &&
          globalThis.getComputedStyle &&
          getComputedStyle(scrollingElement).scrollBehavior === "smooth"
            ? "smooth"
            : requestedBehavior,
        generation = ++smoothGeneration;
      if (behavior !== "smooth" || top === pageTop) {
        const accepted = __tilefinchRequestScroll(top);
        apply(accepted, true);
        return;
      }
      const start = pageTop;
      let frame = 0;
      const step = () => {
        if (generation !== smoothGeneration) return;
        frame++;
        const elapsed = frame / 12,
          progress = 1 - (1 - elapsed) * (1 - elapsed),
          accepted = __tilefinchRequestScroll(
            start + (top - start) * progress,
          );
        apply(accepted, true, true);
        if (frame < 12) requestAnimationFrame(step);
      };
      requestAnimationFrame(step);
    };
    globalThis.scroll = (...args) => globalThis.scrollTo(...args);
    globalThis.scrollBy = (xOrOptions, y) => {
      const delta =
        typeof xOrOptions === "object" && xOrOptions !== null
          ? Number(xOrOptions.top) || 0
          : Number(y) || 0;
      globalThis.scrollTo({ top: pageTop + delta });
    };
  }
  globalThis.__tilefinchPumpTimers = (elapsed, maxCallbacks) => {
    if (usesSampledClock) currentSchedulerTime();
    else now += Math.max(0, Number(elapsed) || 0);
    globalThis.__tilefinchNow = now;
    let ran = 0;
    const maximum = Math.max(0, Number(maxCallbacks) || 0),
      visible = pageVisible();
    while (ran < maximum) {
      timers.sort(timerOrder);
      const timerIndex = visible
          ? (timers[0]?.due <= now ? 0 : -1)
          : timers.findIndex((candidate) =>
            candidate.due <= now && candidate.kind !== "animation-frame" &&
            candidate.kind !== "render-observer" &&
            candidate.kind !== "render-fixup");
      const timer = timerIndex < 0 ? null : timers[timerIndex];
      if (!timer || timer.due > now) break;
      timers.splice(timerIndex, 1);
      /* Per-frame timer kinds reuse one provenance label; author timers keep
         the id-bearing label their uncaught-error diagnostics rely on. */
      const label = timer.kind === "animation-frame"
        ? "timer:animation-frame"
        : timer.kind === "render-observer"
          ? "timer:render-observer"
          : timer.kind === "render-fixup"
            ? "timer:render-fixup"
            : "timer:" + String(timer.kind) + ":id=" + String(timer.id);
      activeTimer = timer;
      try {
        globalThis.__tilefinchRunTask(label, invokeTimer, timer);
      } finally {
        activeTimer = null;
      }
      ran++;
      if (timer.repeat) {
        timer.due = currentSchedulerTime() + timer.span;
        timers.push(timer);
      } else releaseTimer(timer);
      retryMessageDrains();
    }
    return ran;
  };
  globalThis.__tilefinchPendingTimers = () => timers.length;
  /* Source-free native/lab liveness snapshot. Keep the returned tuple numeric
     and bounded so diagnostics never serialize callbacks, arguments, URLs, or
     challenge payloads. This is called only by the native diagnostic seam. */
  globalThis.__tilefinchSchedulerSnapshot = () => {
    let due = 0, oldestOverdue = 0, nearestFuture = Infinity;
    for (const timer of timers) {
      const remaining = timer.due - now;
      if (remaining <= 0) {
        due++;
        if (-remaining > oldestOverdue) oldestOverdue = -remaining;
      } else if (remaining < nearestFuture) nearestFuture = remaining;
    }
    return [
      now,
      usesSampledClock ? 1 : 0,
      timers.length,
      due,
      oldestOverdue,
      Number.isFinite(nearestFuture) ? nearestFuture : -1,
      lastCallbackAt,
      activeCancelAttempts,
      activeCancelHits,
      timeoutCallbacks,
      intervalCallbacks,
    ];
  };
  globalThis.__tilefinchDescribeTimers = () =>
    JSON.stringify({
      now,
      lastFramePost: globalThis.__tilefinchLastFramePost || null,
      frameLifecycle: globalThis.__tilefinchFrameLifecycle || [],
      uncaught: [...(globalThis.__tilefinchUncaughtErrors || [])].slice(-4),
      timers: [...timers]
        .sort((a, b) => a.due - b.due || a.id - b.id)
        .slice(0, 16)
        .map((timer) => ({
          id: timer.id,
          kind: timer.kind,
          due: timer.due,
          remaining: timer.due - now,
          span: timer.span,
          repeat: timer.repeat,
          name: String(timer.callback.name || ""),
        })),
    });
  const trusted = globalThis.__tilefinchTrustedEvent,
    nonBubblingEventTypes = new Set([
      "focus", "blur", "load", "error", "scroll", "mouseenter",
      "mouseleave", "pointerenter", "pointerleave",
    ]),
    composedEventTypes = new Set([
      "beforeinput", "click", "input", "keydown", "keypress", "keyup",
      "pointerdown", "pointermove", "pointerup", "mousedown", "mousemove",
      "mouseup",
    ]),
    cancelableEventTypes = new Set([
      "beforeinput", "click", "keydown", "keypress", "keyup",
      "pointerdown", "pointermove", "pointerup", "mousedown", "mousemove",
      "mouseup", "submit",
    ]);
  const tilefinchEvent = (name, overrides = null) => {
    name = String(name);
    const bubbles = !nonBubblingEventTypes.has(name),
      options = {
        bubbles,
        /* User-agent-dispatched UI events cross open and closed shadow
           boundaries. Without composed=true a visually hit shadow child can
           receive pointer events while the host/controller never sees them. */
        composed: composedEventTypes.has(name),
        cancelable: cancelableEventTypes.has(name),
        ...(overrides || {}),
      };
    let event;
    if (name === "input" || name === "beforeinput")
      event = new InputEvent(name, {
        ...options,
        data: null,
        inputType: name === "input" ? "insertText" : "",
      });
    else if (name.startsWith("key")) event = new KeyboardEvent(name, options);
    else if (name.startsWith("pointer") || name === "click") {
      const clickFromPointer =
        name === "click" && options.pointerId !== undefined;
      event = new PointerEvent(name, {
        ...options,
        pointerType:
          name === "click" && !clickFromPointer ? "" : "mouse",
        pointerId: name === "click" && !clickFromPointer ? -1 : options.pointerId,
        isPrimary: name !== "click" || clickFromPointer,
        detail:
          name === "click"
            ? clickFromPointer ? 1 : 0
            : 0,
      });
    }
    else if (
      name === "focus" ||
      name === "blur" ||
      name === "focusin" ||
      name === "focusout"
    )
      event = new FocusEvent(name, options);
    else if (name.startsWith("composition"))
      event = new CompositionEvent(name, options);
    else if (name === "submit") event = new SubmitEvent(name, options);
    else if (name.startsWith("mouse"))
      event = new MouseEvent(name, {
        ...options,
        detail: 0,
      });
    else event = new Event(name, options);
    return trusted(event);
  };
  globalThis.__tilefinchDispatchAt = (selector, type) => {
    const target = selector
        ? document.querySelector(String(selector))
        : document,
      name = String(type);
    if (!target) return false;
    if (name === "focus" && typeof target.focus === "function") {
      target.focus();
      return true;
    }
    if (name === "blur" && typeof target.blur === "function") {
      target.blur();
      return true;
    }
    return target.dispatchEvent(tilefinchEvent(name));
  };
  globalThis.__tilefinchDispatchHandle = (handle, type) => {
    const name = String(type);
    if (
      name === "focus" &&
      !globalThis.__tilefinchFocusEventsObserved?.() &&
      globalThis.__tilefinchSetFocusHandle?.(handle)
    )
      return true;
    const target = trustedWrap(Number(handle));
    if (!target) return false;
    if (name === "focus" && typeof target.focus === "function") {
      target.focus();
      return true;
    }
    if (name === "blur" && typeof target.blur === "function") {
      target.blur();
      return true;
    }
    return target.dispatchEvent(tilefinchEvent(name));
  };
  globalThis.__tilefinchLabelDefault = (target, native = false) => {
    if (!(target instanceof HTMLLabelElement)) return false;
    const control = target.control;
    if (!control || control.disabled) return false;
    control.focus?.();
    if (!native) {
      control.click();
      return true;
    }
    const state =
        globalThis.__tilefinchBeginControlDefault?.(control, true) || null,
      clickEvent = tilefinchEvent("click");
    clickEvent.__tilefinchControlDefaultPrepared = true;
    const accepted = control.dispatchEvent(clickEvent);
    globalThis.__tilefinchFinishControlDefault?.(state, accepted);
    return true;
  };
  let pointerHoverTarget = null,
    pointerMoveProbeTarget = null,
    pointerCaptureTarget = null,
    pointerMarkupPossible =
      !!globalThis.__tilefinchPointerMarkupInitiallyPresent;
  Element.prototype.setPointerCapture = function (pointerId) {
    if (Number(pointerId) !== 1)
      throw new DOMException("Unknown pointer", "NotFoundError");
    if (pointerCaptureTarget === this) return;
    if (pointerCaptureTarget)
      pointerCaptureTarget.dispatchEvent(
        tilefinchEvent("lostpointercapture", { pointerId: 1 }),
      );
    pointerCaptureTarget = this;
    this.dispatchEvent(tilefinchEvent("gotpointercapture", { pointerId: 1 }));
  };
  Element.prototype.releasePointerCapture = function (pointerId) {
    if (Number(pointerId) !== 1 || pointerCaptureTarget !== this) return;
    pointerCaptureTarget = null;
    this.dispatchEvent(tilefinchEvent("lostpointercapture", { pointerId: 1 }));
  };
  Element.prototype.hasPointerCapture = function (pointerId) {
    return Number(pointerId) === 1 && pointerCaptureTarget === this;
  };
  globalThis.__tilefinchPointerMarkupChanged = () => {
    pointerMarkupPossible = true;
    pointerMoveProbeTarget = null;
  };
  const dispatchHoverTransition = (next, point) => {
    const previous = pointerHoverTarget;
    if (previous === next) return true;
    const eventPath = globalThis.__tilefinchShadowEventPath,
      previousPath = previous ? eventPath(previous, true) : [],
      nextPath = next ? eventPath(next, true) : [];
    let shared = 0;
    while (
      shared < previousPath.length &&
      shared < nextPath.length &&
      previousPath[previousPath.length - 1 - shared] ===
        nextPath[nextPath.length - 1 - shared]
    )
      shared++;
    const fire = (target, name, relatedTarget) =>
      target.dispatchEvent(
        tilefinchEvent(name, { ...point, relatedTarget }),
      );
    let accepted = true;
    if (previous) accepted = fire(previous, "pointerout", next) && accepted;
    for (let at = 0; at < previousPath.length - shared; at++)
      accepted =
        fire(previousPath[at], "pointerleave", next) && accepted;
    if (next) accepted = fire(next, "pointerover", previous) && accepted;
    for (let at = nextPath.length - shared - 1; at >= 0; at--)
      accepted = fire(nextPath[at], "pointerenter", previous) && accepted;
    if (previous) accepted = fire(previous, "mouseout", next) && accepted;
    for (let at = 0; at < previousPath.length - shared; at++)
      accepted = fire(previousPath[at], "mouseleave", next) && accepted;
    if (next) accepted = fire(next, "mouseover", previous) && accepted;
    for (let at = nextPath.length - shared - 1; at >= 0; at--)
      accepted = fire(nextPath[at], "mouseenter", previous) && accepted;
    pointerHoverTarget = next;
    return accepted;
  };
  globalThis.__tilefinchDispatchActivationHandle = (
    handle,
    phase = 0,
    clientX = 0,
    clientY = 0,
    offsetX = 0,
    offsetY = 0,
    buttons = 0,
  ) => {
    phase = Number(phase) | 0;
    let target = trustedWrap(Number(handle));
    if (pointerCaptureTarget && phase >= 1 && phase <= 4)
      target = pointerCaptureTarget;
    if ((!target && phase !== 1) || (target?.disabled && phase !== 1))
      return false;
    if (phase === 1) {
      const sameTarget = target === pointerHoverTarget,
        moveObserved =
          !!globalThis.__tilefinchPointerMoveEventsObserved?.(),
        hoverObserved =
          !!globalThis.__tilefinchPointerHoverEventsObserved?.();
      if (
        sameTarget &&
        pointerMoveProbeTarget === target &&
        !moveObserved
      )
        return true;
      if (!sameTarget && !hoverObserved && !pointerMarkupPossible) {
        pointerHoverTarget = target;
        pointerMoveProbeTarget = null;
        if (!moveObserved) return true;
      }
    }
    let point = null;
    if (phase) {
      clientX = Number(clientX) || 0;
      clientY = Number(clientY) || 0;
      if (pointerCaptureTarget && typeof target.getBoundingClientRect === "function") {
        const bounds = target.getBoundingClientRect();
        offsetX = clientX - bounds.left;
        offsetY = clientY - bounds.top;
      }
      point = {
        clientX,
        clientY,
        pageX: clientX + Number(globalThis.scrollX || 0),
        pageY: clientY + Number(globalThis.scrollY || 0),
        offsetX: Number(offsetX) || 0,
        offsetY: Number(offsetY) || 0,
        button: phase === 1 ? -1 : 0,
        buttons: Number(buttons) >>> 0,
        pointerId: 1,
        pressure: buttons ? 0.5 : 0,
      };
    }
    const fire = (name) => target.dispatchEvent(tilefinchEvent(name, point));
    if (phase === 1) {
      const hoverAccepted =
        target === pointerHoverTarget ||
        (!globalThis.__tilefinchPointerHoverEventsObserved?.() &&
          !pointerMarkupPossible)
          ? true
          : dispatchHoverTransition(target, point);
      if (target !== pointerHoverTarget) pointerHoverTarget = target;
      if (!target) {
        pointerMoveProbeTarget = null;
        return hoverAccepted;
      }
      let pointerAccepted = true,
        mouseAccepted = true;
      if (
        globalThis.__tilefinchPointerMoveEventsObserved?.() ||
        pointerMarkupPossible
      ) {
        pointerAccepted = fire("pointermove");
        mouseAccepted = fire("mousemove");
      }
      pointerMoveProbeTarget = target;
      return hoverAccepted && pointerAccepted && mouseAccepted;
    }
    if (phase === 2) {
      const pointerAccepted = fire("pointerdown"),
        mouseAccepted = fire("mousedown");
      if (
        pointerAccepted &&
        mouseAccepted &&
        typeof target.focus === "function"
      )
        target.focus();
      return pointerAccepted && mouseAccepted;
    }
    if (phase === 3) {
      const pointerAccepted = fire("pointerup"),
        mouseAccepted = fire("mouseup");
      if (pointerCaptureTarget) {
        const captured = pointerCaptureTarget;
        pointerCaptureTarget = null;
        captured.dispatchEvent(
          tilefinchEvent("lostpointercapture", { pointerId: 1 }),
        );
      }
      return pointerAccepted && mouseAccepted;
    }
    if (phase === 4) {
      const accepted = fire("pointercancel");
      if (pointerCaptureTarget) {
        const captured = pointerCaptureTarget;
        pointerCaptureTarget = null;
        captured.dispatchEvent(
          tilefinchEvent("lostpointercapture", { pointerId: 1 }),
        );
      }
      return accepted;
    }
    const clickDefault = () => {
      const controlState =
          globalThis.__tilefinchBeginControlDefault?.(target, true) || null,
        clickEvent = tilefinchEvent("click", point);
      clickEvent.__tilefinchControlDefaultPrepared = true;
      let accepted = false,
        completed = false;
      try {
        accepted = target.dispatchEvent(clickEvent);
        completed = true;
        if (
          accepted &&
          !controlState &&
          !globalThis.__tilefinchDetailsDefault?.(target)
        )
          globalThis.__tilefinchLabelDefault(target, true);
        return accepted;
      } finally {
        /* A watchdog can interrupt an author handler after the bootstrap has
           staged checkbox/radio selectedness but before dispatchEvent
           returns its cancellation bit. Roll the staged default back on
           every incomplete dispatch; C then classifies the admitted handler
           as RUNTIME_FAILED and must not replay it. */
        globalThis.__tilefinchFinishControlDefault?.(
          controlState,
          completed && accepted,
        );
      }
    };
    if (phase === 5) return clickDefault();
    /* Controller activation is keyboard-like, not a fabricated pointer
       gesture.  Pointer Events requires its click to use pointerId -1 and an
       empty pointerType; low-level pointer/mouse events are reserved for the
       actual pointer path above. */
    if (phase === 0) {
      if (typeof target.focus === "function") target.focus();
      return clickDefault();
    }
    const pointerAccepted = fire("pointerdown"),
      mouseAccepted = fire("mousedown");
    if (pointerAccepted && mouseAccepted && typeof target.focus === "function")
      target.focus();
    fire("pointerup");
    fire("mouseup");
    return clickDefault();
  };
  globalThis.__tilefinchDispatchInputHandle = (
    handle,
    type,
    data,
    inputType,
    currentValue,
  ) => {
    const target = trustedWrap(Number(handle));
    if (!target) return false;
    const name = String(type);
    if (name === "input" && currentValue !== null) {
      globalThis.__tilefinchSyncNativeControlValue?.(target, currentValue);
    }
    return target.dispatchEvent(
      trusted(
        new InputEvent(name, {
          bubbles: true,
          cancelable: name === "beforeinput",
          data: data === null ? null : String(data),
          inputType: String(inputType || ""),
        }),
      ),
    );
  };
  globalThis.__tilefinchDispatchSubmitHandle = (formHandle, submitterHandle) => {
    const form = trustedWrap(Number(formHandle)),
      submitter = trustedWrap(Number(submitterHandle));
    if (!form) return false;
    if (!form.noValidate && !submitter?.formNoValidate && !form.checkValidity())
      return false;
    return form.dispatchEvent(
      trusted(
        new SubmitEvent("submit", {
          bubbles: true,
          cancelable: true,
          submitter,
        }),
      ),
    );
  };
})();
