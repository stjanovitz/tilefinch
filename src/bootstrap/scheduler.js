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
    diagnosticString = String,
    diagnosticOwnDescriptor = Object.getOwnPropertyDescriptor;
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
  let now = 0,
    nextId = 1;
  const timers = [];
  const limit = 128;
  Object.defineProperty(globalThis.__tilefinchRootCensus, "timers", {
    get: () => timers.length,
  });
  function schedule(callback, delay, repeat, args, kind = "timeout") {
    if (typeof callback !== "function" || timers.length >= limit) return 0;
    const id = nextId++;
    const span = Math.max(0, Number(delay) || 0);
    timers.push({ id, callback, due: now + span, span, repeat, args, kind });
    return id;
  }
  function clear(id) {
    const at = timers.findIndex((timer) => timer.id === Number(id));
    if (at >= 0) timers.splice(at, 1);
  }
  const timerPriority = (timer) =>
    timer.kind === "animation-frame"
      ? 0
      : timer.kind === "render-observer"
        ? 1
        : timer.kind === "render-fixup"
          ? 2
          : 3;
  globalThis.setTimeout = (callback, delay, ...args) =>
    schedule(callback, delay, false, args, "timeout");
  globalThis.setInterval = (callback, delay, ...args) =>
    schedule(callback, Math.max(1, Number(delay) || 0), true, args, "interval");
  globalThis.clearTimeout = clear;
  globalThis.clearInterval = clear;
  globalThis.requestAnimationFrame = (callback) => {
    if (typeof callback !== "function")
      throw new TypeError("callback must be a function");
    return schedule(
      (timestamp) => callback(timestamp),
      16,
      false,
      [now + 16],
      "animation-frame",
    );
  };
  globalThis.cancelAnimationFrame = clear;
  globalThis.__tilefinchScheduleRenderObserver = (callback) =>
    schedule(callback, 16, false, [], "render-observer");
  globalThis.__tilefinchScheduleRenderFixup = (callback) =>
    schedule(callback, 16, false, [], "render-fixup");
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
      channelLimit = 8;
    let channelCount = 0;
    class MessagePort {
      constructor(key, owner) {
        if (key !== token) throw new TypeError("Illegal constructor");
        this._owner = owner;
        this._peer = null;
        this._closed = false;
        this._started = false;
        this._queue = [];
        this._listeners = new Map();
        this._onmessage = null;
        this.onmessageerror = null;
      }
      get onmessage() {
        return this._onmessage;
      }
      set onmessage(callback) {
        this._onmessage = typeof callback === "function" ? callback : null;
        if (this._onmessage) this.start();
      }
      addEventListener(type, callback) {
        if (typeof callback !== "function") return;
        const key = String(type);
        if (!this._listeners.has(key)) this._listeners.set(key, []);
        const list = this._listeners.get(key);
        if (!list.includes(callback)) list.push(callback);
      }
      removeEventListener(type, callback) {
        const list = this._listeners.get(String(type));
        if (!list) return;
        const at = list.indexOf(callback);
        if (at >= 0) list.splice(at, 1);
      }
      start() {
        if (this._closed || this._started) return;
        this._started = true;
        const queued = this._queue.splice(0);
        for (const data of queued) this._enqueue(data);
      }
      close() {
        if (this._closed) return;
        this._closed = true;
        this._queue = [];
        this._listeners.clear();
        this._onmessage = null;
        this._owner.closed++;
        if (this._owner.closed === 2) channelCount--;
      }
      _enqueue(data) {
        if (this._closed) return;
        const id = schedule(() => this._deliver(data), 0, false, [], "message");
        if (!id) throw new RangeError("message task limit exceeded");
      }
      _deliver(data) {
        if (this._closed) return;
        if (!this._started) {
          if (this._queue.length < 32) this._queue.push(data);
          return;
        }
        const event = new MessageEvent("message", {
          data,
          origin: "",
          source: null,
          ports: [],
        });
        Object.defineProperty(event, "isTrusted", {
          value: true,
          configurable: true,
        });
        try {
          if (typeof this._onmessage === "function")
            globalThis.__tilefinchRunTask(
              "message-port-handler",
              this._onmessage,
              this,
              [event],
            );
          for (const callback of [...(this._listeners.get("message") || [])])
            globalThis.__tilefinchRunTask(
              "message-port-listener",
              callback,
              this,
              [event],
            );
        } catch (error) {
          __tilefinchReportUncaught(error, "message port");
        }
      }
      postMessage(value, transfer = []) {
        if (this._closed) return;
        if (
          transfer !== undefined &&
          transfer !== null &&
          Array.from(transfer).length
        )
          throw new DOMException(
            "Transferable objects are not supported",
            "DataCloneError",
          );
        const copied = structuredClone(value),
          target = this._peer;
        if (target && !target._closed) target._enqueue(copied);
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
                const event = new MessageEvent("message", {
                  data,
                  origin: String(globalThis.location?.origin || ""),
                  source: null,
                  ports: [],
                });
                Object.defineProperty(event, "isTrusted", {
                  configurable: true,
                  value: true,
                });
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
      const top =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? Number(xOrOptions.top) || 0
            : Number(y) || 0,
        requestedBehavior =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? String(xOrOptions.behavior || "auto")
            : "auto",
        behavior =
          requestedBehavior === "auto" &&
          globalThis.getComputedStyle &&
          getComputedStyle(document.scrollingElement).scrollBehavior ===
            "smooth"
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
    now += Math.max(0, Number(elapsed) || 0);
    globalThis.__tilefinchNow = now;
    let ran = 0;
    const maximum = Math.max(0, Number(maxCallbacks) || 0);
    while (ran < maximum) {
      timers.sort(
        (a, b) =>
          a.due - b.due ||
          timerPriority(a) - timerPriority(b) ||
          a.id - b.id,
      );
      const timer = timers[0];
      if (!timer || timer.due > now) break;
      timers.shift();
      globalThis.__tilefinchRunTask(
        "timer:" + String(timer.kind) + ":id=" + String(timer.id),
        () => {
          try {
            timer.callback(...timer.args);
          } catch (error) {
            __tilefinchReportUncaught(error, "timer callback");
          }
        },
      );
      ran++;
      if (timer.repeat) {
        timer.due = now + timer.span;
        timers.push(timer);
      }
    }
    return ran;
  };
  globalThis.__tilefinchPendingTimers = () => timers.length;
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
  const trusted = globalThis.__tilefinchTrustedEvent;
  const tilefinchEvent = (name, overrides = null) => {
    name = String(name);
    const bubbles = ![
        "focus",
        "blur",
        "load",
        "error",
        "scroll",
        "mouseenter",
        "mouseleave",
        "pointerenter",
        "pointerleave",
      ].includes(name),
      options = {
        bubbles,
        cancelable: [
          "beforeinput",
          "click",
          "keydown",
          "keypress",
          "keyup",
          "pointerdown",
          "pointermove",
          "pointerup",
          "mousedown",
          "mousemove",
          "mouseup",
          "submit",
        ].includes(name),
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
    else if (name.startsWith("pointer"))
      event = new PointerEvent(name, {
        ...options,
        pointerType: "mouse",
        isPrimary: true,
      });
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
    else if (name === "click" || name.startsWith("mouse"))
      event = new MouseEvent(name, {
        ...options,
        detail: name === "click" ? 1 : 0,
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
    pointerMarkupPossible =
      !!globalThis.__tilefinchPointerMarkupInitiallyPresent;
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
    const target = trustedWrap(Number(handle));
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
      return pointerAccepted && mouseAccepted;
    }
    if (phase === 4) return fire("pointercancel");
    const clickDefault = () => {
      const controlState =
          globalThis.__tilefinchBeginControlDefault?.(target, true) || null,
        clickEvent = tilefinchEvent("click", point);
      clickEvent.__tilefinchControlDefaultPrepared = true;
      const accepted = target.dispatchEvent(clickEvent);
      globalThis.__tilefinchFinishControlDefault?.(controlState, accepted);
      if (
        accepted &&
        !controlState &&
        !globalThis.__tilefinchDetailsDefault?.(target)
      )
        globalThis.__tilefinchLabelDefault(target, true);
      return accepted;
    };
    if (phase === 5) return clickDefault();
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
