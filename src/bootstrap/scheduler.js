(() => {
  const Object = __tilefinchObject;
  /* Bound before author code runs: what this module converts, collects
     and throws with stays the realm's own when a page replaces the
     global. */
  const { String, Number, Map, Set, Error, TypeError, RangeError,
    DOMException } = globalThis;
  /* See dom.js: the timer clock mirror is written on the first pump, which
     is well after hardening.js has taken its snapshot. */
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchNow", {
    enumerable: false,
    configurable: false,
    writable: true,
    value: 0,
  });
  const trustedWrap = __tilefinchRealmGlobal.__tilefinchWrap,
    pageVisible = __tilefinchRealmGlobal.__tilefinchPageVisible,
    schedulerNow = __tilefinchRealmGlobal.__tilefinchSchedulerNow,
    createMessageEvent = __tilefinchRealmGlobal.__tilefinchCreateMessageEvent,
    diagnosticString = String,
    diagnosticOwnDescriptor = Object.getOwnPropertyDescriptor,
    timerApply = Reflect.apply,
    /* MessagePort's handler attributes register through these, never
       through a page's patched EventTarget.prototype methods. */
    trustedAddListener = Function.call.bind(
      EventTarget.prototype.addEventListener),
    trustedRemoveListener = Function.call.bind(
      EventTarget.prototype.removeEventListener),
    /* The browser's own delivery (input, pointer capture, scroll,
       BroadcastChannel) dispatches with the methods targets inherit before
       author code runs, as in browsers: a page's replacement of
       dispatchEvent sees only its own calls. */
    dispatchTarget = Function.call.bind(EventTarget.prototype.dispatchEvent),
    dispatchDocument = document.dispatchEvent,
    dispatchNode =
      __tilefinchRealmGlobal.__tilefinchTrustedEventShare.dispatchNode,
    isNode = Function.call.bind(Object.prototype.isPrototypeOf, Node.prototype),
    browserDispatch = (target, event) => target === document
      ? dispatchDocument(event)
      : isNode(target) ? dispatchNode(target, event)
        : dispatchTarget(target, event),
    /* Input focuses, unfocuses and activates with the steps themselves,
       never a page's replacement of focus(), blur() or click(). */
    isElement = Function.call.bind(Object.prototype.isPrototypeOf,
      Element.prototype),
    focusSteps = Element.prototype.focus,
    blurSteps = Element.prototype.blur,
    clickSteps = HTMLElement.prototype.click,
    focusElement = (target) => {
      if (isElement(target)) timerApply(focusSteps, target, []);
    };
  /* Native memory the event loop reads every turn instead of calling into
     JS (ScriptRuntime.host_state; slot order is ScriptHostStateSlot). */
  const hostState = __tilefinchRealmGlobal.__tilefinchHostChannel.state,
    HOST_TIMERS = 0,
    HOST_EARLIEST_DUE = 1,
    HOST_NOW = 2,
    HOST_SAMPLED_CLOCK = 3,
    HOST_SCROLL_PENDING = 4,
    HOST_EARLIEST_KIND = 5,
    HOST_EARLIEST_TASK_DUE = 6,
    HOST_TIMER_EXCEPTIONS = 7;
  /* ScriptHostTimerKind: the native loop tells frame callbacks, which run
     at a rendering opportunity, from tasks, which run once due. */
  const TIMER_KIND_CODES = Object.freeze({
    __proto__: null,
    "animation-frame": 1,
    "render-observer": 2,
    "render-fixup": 3,
    timeout: 4,
    interval: 5,
    message: 6,
    "platform-task": 7,
    idle: 8,
  });
  /* Lab capability traces decode generated functions by name; host and
     validation realms only. */
  if (__tilefinchRealmGlobal.__tilefinchTestProbes)
    Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchDiagnosticLookup", {
      enumerable: false,
      configurable: false,
      writable: false,
      value(name) {
        const descriptor = diagnosticOwnDescriptor(
          __tilefinchRealmGlobal,
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
  /* The wheel's size and head after every change to `timers`: the native
     loop skips the timer pump when nothing is due. */
  const publishTimers = () => {
    hostState[HOST_TIMERS] = timers.length;
    hostState[HOST_EARLIEST_DUE] = timers.length ? timers[0].due : Infinity;
    hostState[HOST_EARLIEST_KIND] = timers.length
      ? TIMER_KIND_CODES[timers[0].kind] || 9 : 0;
    /* Sorted by due time: the first timer that is not a frame callback is
       the earliest task. Frame callbacks at the head are few. */
    let taskDue = Infinity;
    for (let i = 0; i < timers.length; i++) {
      const code = TIMER_KIND_CODES[timers[i].kind] || 9;
      if (code > 3) {
        taskDue = timers[i].due;
        break;
      }
    }
    hostState[HOST_EARLIEST_TASK_DUE] = taskDue;
  };
  hostState[HOST_NOW] = now;
  hostState[HOST_SAMPLED_CLOCK] = usesSampledClock ? 1 : 0;
  publishTimers();
  const limit = 128,
    /* Browser-owned clocks that each keep at most one timer pending (the CSS
       transition event clock, the property-effect frame, the CSS animation
       event clock and the motion scan's next chunk) draw on these slots above
       the author limit, so a page that fills the table cannot strand one. */
    reservedLimit = limit + 4;
  /* The frame loop registers an animation-frame timer every presented frame.
     Reusing retired records and one shared empty argument list keeps the
     steady game loop free of per-frame allocations, which would otherwise
     pace QuickJS's full collections onto gameplay frames. */
  const EMPTY_TIMER_ARGS = Object.freeze([]);
  const timerPool = [];
  let retryMessageDrains = () => {};
  /* Browser-owned groups retain their own bounded pending state. A
     refused timer is not a queued task: retry only when a slot is retired,
     without creating another queue or enlarging the timer table. */
  let retryingCapacity = false;
  const retryTimerCapacity = () => {
    if (retryingCapacity) return;
    retryingCapacity = true;
    try {
      retryMessageDrains();
      const ready = __tilefinchTimerCapacity[4];
      __tilefinchTimerCapacity[4] = 0;
      for (let slot = 0; slot < 5; slot++)
        if ((ready & (1 << slot)) && __tilefinchTimerCapacity[slot === 4 ? 5 : slot]) try {
          __tilefinchTimerCapacity[slot === 4 ? 5 : slot]();
        } catch (error) {
          __tilefinchReportUncaught(error, "deferred browser task");
        }
    } finally { retryingCapacity = false; }
  };
  /* HTML timer delays use Web IDL `long` conversion before negative values
     are clamped. JavaScript's ToInt32 operator is the same modulo-2^32,
     truncate-toward-zero conversion and evaluates an author coercion once. */
  const normalizeTimerDelay = (value) => {
    const signed = Number(value) >> 0;
    return signed > 0 ? signed : 0;
  };
  __tilefinchRealmGlobal.__tilefinchNormalizeTimerDelay = normalizeTimerDelay;
  const currentSchedulerTime = () => {
    if (usesSampledClock) {
      const sampled = Number(schedulerNow());
      if (Number.isFinite(sampled) && sampled >= now) now = sampled;
    }
    return now;
  };
  if (__tilefinchRealmGlobal.__tilefinchRootCensus)
    Object.defineProperty(__tilefinchRealmGlobal.__tilefinchRootCensus, "timers", {
      get: () => timers.length,
    });
  function schedule(callback, delay, repeat, args, kind = "timeout",
                    reserved = false, owner = 0) {
    if (typeof callback !== "function" ||
        timers.length >= (reserved ? reservedLimit : limit)) return 0;
    const id = nextId++;
    const timerAlgorithm = kind === "timeout" || kind === "interval",
      parentNesting = timerAlgorithm && activeTimer?.nesting > 0
        ? activeTimer.nesting : 0,
      nesting = timerAlgorithm ? parentNesting + 1 : 0,
      requestedSpan = timerAlgorithm ? normalizeTimerDelay(delay)
        : Math.max(0, Math.min(0xffffffff, Number(delay) || 0)),
      span = timerAlgorithm && parentNesting > 5
        ? Math.max(4, requestedSpan) : requestedSpan,
      registeredAt = currentSchedulerTime();
    /* Delay coercion can run author code that fills the table. Recheck after
       it returns rather than publishing a record beyond the bounded wheel. */
    if (timers.length >= (reserved ? reservedLimit : limit)) return 0;
    const timer = timerPool.pop();
    if (timer) {
      timer.id = id;
      timer.callback = callback;
      timer.due = registeredAt + span;
      timer.span = span;
      timer.repeat = repeat;
      timer.args = args;
      timer.kind = kind;
      timer.owner = owner;
      timer.nesting = nesting;
      timer.requestedSpan = requestedSpan;
      insertTimer(timer);
    } else {
      insertTimer({
        id, callback, due: registeredAt + span, span, repeat, args, kind,
        owner, nesting, requestedSpan,
      });
    }
    return id;
  }
  function releaseTimer(timer) {
    timer.callback = null;
    timer.args = EMPTY_TIMER_ARGS;
    timer.owner = 0;
    if (timerPool.length < limit) timerPool.push(timer);
  }
  /* 0 is private teardown; 1/2 identify Window timers/frames. A Worker's
     existing timer set is its opaque author-timer owner token. Nesting > 0
     already identifies HTML timer algorithms, so ownership replaces the old
     redundant boolean rather than growing every pooled timer record. */
  function clear(id, owner = 0) {
    const numericId = Number(id);
    if (activeTimer && activeTimer.id === numericId
        && (!owner || activeTimer.owner === owner)) {
      activeCancelAttempts++;
      if (activeTimer.repeat) {
        activeTimer.repeat = false;
        activeCancelHits++;
      }
      return true;
    }
    const at = timers.findIndex((timer) => timer.id === numericId
        && (!owner || timer.owner === owner));
    if (at >= 0) {
      releaseTimer(timers.splice(at, 1)[0]);
      publishTimers();
      retryTimerCapacity();
      return true;
    }
    return false;
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
  /* `timers` stays in timerOrder: every insertion lands at its sorted
     place and removals preserve order, so the pump never re-sorts. */
  function insertTimer(timer) {
    let low = 0, high = timers.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (timerOrder(timers[middle], timer) <= 0) low = middle + 1;
      else high = middle;
    }
    timers.splice(low, 0, timer);
    publishTimers();
  }
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
      else timerApply(callback, __tilefinchRealmGlobal, this.args);
    } catch (error) {
      hostState[HOST_TIMER_EXCEPTIONS]++;
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
  __tilefinchRealmGlobal.__tilefinchScheduleTimeout = scheduleTimeout;
  __tilefinchRealmGlobal.__tilefinchScheduleInterval = scheduleInterval;
  __tilefinchRealmGlobal.__tilefinchScheduleTask = (callback, delay = 0) =>
    schedule(callback, delay, false, EMPTY_TIMER_ARGS, "platform-task");
  __tilefinchRealmGlobal.__tilefinchScheduleOwnedTimer =
    (owner, callback, delay, repeat) => schedule(
      callback, delay, repeat, EMPTY_TIMER_ARGS, repeat ? "interval" : "timeout", false, owner);
  __tilefinchRealmGlobal.__tilefinchScheduleFrame = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "animation-frame");
  __tilefinchRealmGlobal.__tilefinchCancelTimer = clear;
  __tilefinchRealmGlobal.__tilefinchSchedulerTime = currentSchedulerTime;
  __tilefinchRealmGlobal.__tilefinchScheduleReservedTask = (callback, delay) =>
    schedule(callback, delay, false, EMPTY_TIMER_ARGS, "platform-task", true);
  __tilefinchRealmGlobal.__tilefinchScheduleReservedFrame = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "animation-frame", true);
  /* The public timer functions are distinct objects whose name and length
     match WebIDL (the optional timeout/handle arguments do not count), as
     page scripts and fingerprinting probes read both. */
  const setTimeout = (handler, timeout = 0, ...args) =>
      schedule(handler, timeout, false, args, "timeout", false, 1),
    setInterval = (handler, timeout = 0, ...args) =>
      schedule(handler, timeout, true, args, "interval", false, 1),
    clearTimeout = (id = 0) => { clear(id, 1); },
    clearInterval = (id = 0) => { clear(id, 1); },
    requestAnimationFrame = (callback) => {
      if (typeof callback !== "function")
        throw new TypeError("callback must be a function");
      /* The dispatcher invokes animation-frame callbacks with exactly one
         timestamp argument, so the author callback needs no adapter. */
      return schedule(callback, 16, false, EMPTY_TIMER_ARGS, "animation-frame", false, 2);
    },
    cancelAnimationFrame = (handle) => { clear(handle, 2); };
  __tilefinchRealmGlobal.setTimeout = setTimeout;
  __tilefinchRealmGlobal.setInterval = setInterval;
  __tilefinchRealmGlobal.clearTimeout = clearTimeout;
  __tilefinchRealmGlobal.clearInterval = clearInterval;
  __tilefinchRealmGlobal.requestAnimationFrame = requestAnimationFrame;
  __tilefinchRealmGlobal.cancelAnimationFrame = cancelAnimationFrame;
  __tilefinchRealmGlobal.__tilefinchScheduleRenderObserver = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "render-observer");
  __tilefinchRealmGlobal.__tilefinchScheduleRenderFixup = (callback) =>
    schedule(callback, 16, false, EMPTY_TIMER_ARGS, "render-fixup");
  {
    const token = {},
      portBrands = new WeakSet(),
      channelLimit = 8,
      messageQueueLimit = 32,
      blockedDrains = new Set();
    let channelCount = 0;
    const scheduleEndpointDrain = (endpoint) => {
      if (endpoint.drainPending || endpoint.deliveryInProgress ||
          endpoint.queue.length === 0) return true;
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
        if (!item) return;
        endpoint.deliveryInProgress = true;
        portSteps.deliver.call(current, item.data, item.ports, () => {
          endpoint.deliveryInProgress = false;
          if (endpoint.queue.length) scheduleEndpointDrain(endpoint);
        });
      }, 0, false, EMPTY_TIMER_ARGS, "message");
      if (!id) {
        blockedDrains.add(endpoint);
        return false;
      }
      endpoint.drainPending = true;
      blockedDrains.delete(endpoint);
      return true;
    };
    /* After every retired timer: no page-patchable Set iterator. */
    const drainsForEach = Function.call.bind(Set.prototype.forEach);
    retryMessageDrains = () => {
      let blocked = false;
      drainsForEach(blockedDrains, (endpoint) => {
        if (!blocked && !scheduleEndpointDrain(endpoint)) blocked = true;
      });
    };
    const portSteps = { __proto__: null };
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
          deliveryInProgress: false,
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
          trustedAddListener(this, "message", this._onmessageListener);
        else if (this._onmessage !== null && next === null)
          trustedRemoveListener(this, "message", this._onmessageListener);
        this._onmessage = next;
        if (this._onmessage) this.start();
      }
      get onmessageerror() {
        return this._onmessageerror;
      }
      set onmessageerror(callback) {
        const next = typeof callback === "function" ? callback : null;
        if (this._onmessageerror === null && next !== null)
          trustedAddListener(
            this, "messageerror", this._onmessageerrorListener);
        else if (this._onmessageerror !== null && next === null)
          trustedRemoveListener(
            this, "messageerror", this._onmessageerrorListener);
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
          trustedRemoveListener(this, "message", this._onmessageListener);
        if (this._onmessageerror !== null)
          trustedRemoveListener(
            this, "messageerror", this._onmessageerrorListener);
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
      _deliver(data, ports = [], complete = null) {
        if (this._closed || !this._started) {
          if (typeof complete === "function") complete();
          return;
        }
        const event = trusted(createMessageEvent(
          "message", data, "", null, ports));
        __tilefinchRealmGlobal.__tilefinchDispatchEventTargetCheckpointed(
          this, event, complete);
      }
      postMessage(value, transfer = []) {
        if (this._closed) return;
        const ports = [],
          copied = __tilefinchRealmGlobal.__tilefinchCloneWorkerValue(
            value, __tilefinchRealmGlobal.__tilefinchWorkerCloneIntrinsics.owner,
            transfer, ports),
          target = this._peer;
        if (target && !target._closed) portSteps.enqueue.call(target, copied, ports);
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
    /* Delivery steps are not members of the interface. */
    for (const name of ["enqueue", "deliver"]) {
      portSteps[name] = MessagePort.prototype["_" + name];
      delete MessagePort.prototype["_" + name];
    }
    __tilefinchRealmGlobal.MessagePort = MessagePort;
    __tilefinchRealmGlobal.MessageChannel = MessageChannel;
    /* Structured clone prepares a replacement endpoint first and commits the
       move only after the entire message has cloned successfully. This keeps
       transfer-list failures transactional. */
    Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchPrepareMessagePortTransfer", {
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
              trustedRemoveListener(
                port, "message", port._onmessageListener);
            if (port._onmessageerror !== null)
              trustedRemoveListener(
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
    /* Delivery reads no page-patchable collection method or global. */
    const channels = new Map(),
      cloneMessage = structuredClone,
      channelGet = Function.call.bind(Map.prototype.get),
      channelSet = Function.call.bind(Map.prototype.set),
      channelDelete = Function.call.bind(Map.prototype.delete),
      groupAdd = Function.call.bind(Set.prototype.add),
      groupDelete = Function.call.bind(Set.prototype.delete),
      groupForEach = Function.call.bind(Set.prototype.forEach),
      groupSize = Function.call.bind(
        Object.getOwnPropertyDescriptor(Set.prototype, "size").get),
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
        let group = channelGet(channels, name);
        if (!group) {
          group = new Set();
          channelSet(channels, name, group);
        }
        groupAdd(group, this);
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
        const group = channelGet(channels, this.__name);
        if (!group) return;
        const message = cloneMessage(value), targets = [];
        groupForEach(group, (target) => { targets[targets.length] = target; });
        for (let at = 0; at < targets.length; at++) {
          const target = targets[at];
          if (target === this || target.__closed) continue;
          const data = cloneMessage(message);
          if (
            !schedule(
              () => {
                if (target.__closed) return;
                const event = trusted(createMessageEvent(
                  "message", data,
                  String(__tilefinchRealmGlobal.location?.origin || ""), null, []));
                if (typeof target.onmessage === "function")
                  try {
                    __tilefinchRealmGlobal.__tilefinchRunTask(
                      "broadcast-channel-handler",
                      target.onmessage,
                      target,
                      [event],
                    );
                  } catch (error) {
                    __tilefinchReportUncaught(error, "broadcast channel");
                  }
                dispatchTarget(target, event);
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
        const group = channelGet(channels, this.__name);
        if (group) {
          groupDelete(group, this);
          if (groupSize(group) === 0) channelDelete(channels, this.__name);
        }
        channelCount--;
      }
    }
    __tilefinchRealmGlobal.BroadcastChannel = BroadcastChannel;
  }
  const requestIdleCallback = (callback, options = {}) => {
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
        __tilefinchRealmGlobal.__tilefinchRunTask(
          "idle-callback",
          callback,
          __tilefinchRealmGlobal,
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
  const cancelIdleCallback = (handle) => clear(handle);
  __tilefinchRealmGlobal.requestIdleCallback = requestIdleCallback;
  __tilefinchRealmGlobal.cancelIdleCallback = cancelIdleCallback;
  const deliverViewportEvent = (viewport, event) => {
    const type = String(event.type);
    dispatchTarget(viewport, event);
    const handler = viewport["on" + type];
    if (typeof handler === "function")
      try {
        __tilefinchRealmGlobal.__tilefinchRunTask(
          "visual-viewport-handler:" + type,
          handler,
          viewport,
          [event],
        );
      } catch (error) {
        __tilefinchReportUncaught(error, "visualViewport " + type);
      }
    return !event.defaultPrevented;
  };
  {
    const viewport = {
      width: innerWidth,
      height: innerHeight,
      offsetLeft: 0,
      offsetTop: 0,
      pageLeft: 0,
      pageTop: 0,
      scale: __tilefinchRealmGlobal.__tilefinchDiagnosticMobileSafari
        ? 1
        : Number(__tilefinchRealmGlobal.__tilefinchViewportScale) || 1,
      onresize: null,
      onscroll: null,
      dispatchEvent(event) {
        return deliverViewportEvent(viewport, event);
      },
    };
    /* CSSOM View: the viewport object is a VisualViewport. */
    class VisualViewport extends EventTarget {
      constructor() {
        throw new TypeError("Illegal constructor");
      }
    }
    Object.setPrototypeOf(viewport, VisualViewport.prototype);
    __tilefinchRealmGlobal.VisualViewport = VisualViewport;
    __tilefinchRealmGlobal.visualViewport = viewport;
  }
  {
    /* The page can replace the global and the viewport's own methods. */
    const viewport = __tilefinchRealmGlobal.visualViewport;
    let pageTop = 0,
      pending = false,
      smoothGeneration = 0;
    const apply = (value, notify = true, smoothContinuation = false) => {
      if (!smoothContinuation) smoothGeneration++;
      const next = Math.max(0, Math.min(2147483647, Number(value) || 0));
      if (next === pageTop) return false;
      pageTop = next;
      viewport.pageTop = next;
      if (notify) {
        pending = true;
        hostState[HOST_SCROLL_PENDING] = 1;
      }
      return true;
    };
    Object.defineProperties(__tilefinchRealmGlobal, {
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
    __tilefinchRealmGlobal.__tilefinchApplyPageScroll = apply;
    /* The native loop calls this only while HOST_SCROLL_PENDING is set. */
    __tilefinchRealmGlobal.__tilefinchFlushPageScroll = () => {
      if (!pending) return false;
      pending = false;
      hostState[HOST_SCROLL_PENDING] = 0;
      /* Viewport scrolling targets Document and bubbles to Window. Dispatching
         directly on Window skips document listeners used by sticky controls. */
      dispatchDocument(trusted(newEvent("scroll", { bubbles: true })));
      deliverViewportEvent(viewport, trusted(newEvent("scroll")));
      return true;
    };
    const scrollTo = (xOrOptions = undefined, y = undefined) => {
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
          __tilefinchRealmGlobal.getComputedStyle &&
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
        if (frame < 12) __tilefinchRealmGlobal.__tilefinchScheduleFrame(step);
      };
      __tilefinchRealmGlobal.__tilefinchScheduleFrame(step);
    };
    const scroll = (...args) => __tilefinchRealmGlobal.scrollTo(...args),
      scrollBy = (xOrOptions = undefined, y = undefined) => {
        const delta =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? Number(xOrOptions.top) || 0
            : Number(y) || 0;
        __tilefinchRealmGlobal.scrollTo({ top: pageTop + delta });
      };
    __tilefinchRealmGlobal.scrollTo = scrollTo;
    __tilefinchRealmGlobal.scroll = scroll;
    __tilefinchRealmGlobal.scrollBy = scrollBy;
  }
  __tilefinchRealmGlobal.__tilefinchPumpTimers = (elapsed, maxCallbacks, tasksOnly) => {
    if (usesSampledClock) currentSchedulerTime();
    else now += Math.max(0, Number(elapsed) || 0);
    __tilefinchRealmGlobal.__tilefinchNow = now;
    hostState[HOST_NOW] = now;
    let ran = 0,
      visible;
    const maximum = Math.max(0, Number(maxCallbacks) || 0);
    while (ran < maximum) {
      /* `timers` is sorted by due time: when its head is not due nothing
         is, whatever the visibility, so the query waits for a due head. */
      if (!(timers[0]?.due <= now)) break;
      if (visible === undefined) visible = pageVisible();
      const timerIndex = visible && !tasksOnly
          ? (timers[0]?.due <= now ? 0 : -1)
          : timers.findIndex((candidate) =>
            candidate.due <= now && candidate.kind !== "animation-frame" &&
            candidate.kind !== "render-observer" &&
            candidate.kind !== "render-fixup");
      const timer = timerIndex < 0 ? null : timers[timerIndex];
      if (!timer || timer.due > now) break;
      timers.splice(timerIndex, 1);
      publishTimers();
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
        __tilefinchRealmGlobal.__tilefinchRunTask(label, invokeTimer, timer);
      } finally {
        activeTimer = null;
      }
      ran++;
      if (timer.repeat) {
        /* Repeating timers recursively run the timer initialization steps.
           Their task nesting therefore rises just like a timeout chain, and
           the same timeout/interval counter applies to combinations of both.
           Browser-internal timers never participate in this counter. */
        if (timer.nesting > 0) {
          const parentNesting = timer.nesting;
          timer.nesting = parentNesting + 1;
          timer.span = parentNesting > 5
            ? Math.max(4, timer.requestedSpan) : timer.requestedSpan;
        }
        timer.due = currentSchedulerTime() + timer.span;
        insertTimer(timer);
      } else releaseTimer(timer);
      retryTimerCapacity();
    }
    return ran;
  };
  /* Source-free native/lab liveness snapshot. Keep the returned tuple numeric
     and bounded so diagnostics never serialize callbacks, arguments, URLs, or
     challenge payloads. This is called only by the native diagnostic seam. */
  __tilefinchRealmGlobal.__tilefinchSchedulerSnapshot = () => {
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
  const createBootstrapEvent = Document.prototype.createEvent;
  const trusted =
      __tilefinchRealmGlobal.__tilefinchTrustedEventShare.trustedEvent,
    newEvent =
      __tilefinchRealmGlobal.__tilefinchTrustedEventShare.createBrowserEvent,
    nonBubblingEventTypes = new Set([
      "focus", "blur", "load", "error", "scroll", "mouseenter",
      "mouseleave", "pointerenter", "pointerleave",
    ]),
    composedEventTypes = new Set([
      "beforeinput", "click", "input", "keydown", "keypress", "keyup",
      "pointerdown", "pointermove", "pointerup", "mousedown", "mousemove",
      "mouseup", "compositionstart", "compositionupdate", "compositionend",
    ]),
    cancelableEventTypes = new Set([
      "beforeinput", "click", "keydown", "keypress", "keyup",
      "pointerdown", "pointermove", "pointerup", "mousedown", "mousemove",
      "mouseup", "submit", "compositionstart",
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
      event = newEvent(name, {
        ...options,
        data: null,
        inputType: name === "input" ? "insertText" : "",
      }, "InputEvent");
    else if (name.startsWith("key"))
      event = newEvent(name, options, "KeyboardEvent");
    else if (name.startsWith("pointer") || name === "click") {
      const clickFromPointer =
        name === "click" && options.pointerId !== undefined;
      event = newEvent(name, {
        ...options,
        pointerType:
          name === "click" && !clickFromPointer ? "" : "mouse",
        pointerId: name === "click" && !clickFromPointer ? -1 : options.pointerId,
        isPrimary: name !== "click" || clickFromPointer,
        detail:
          name === "click"
            ? clickFromPointer ? 1 : 0
            : 0,
      }, "PointerEvent");
    }
    else if (
      name === "focus" ||
      name === "blur" ||
      name === "focusin" ||
      name === "focusout"
    )
      event = newEvent(name, options, "FocusEvent");
    else if (name.startsWith("composition"))
      event = newEvent(name, options, "CompositionEvent");
    else if (name === "submit")
      event = newEvent(name, options, "SubmitEvent");
    else if (name.startsWith("mouse"))
      event = newEvent(name, {
        ...options,
        detail: 0,
      }, "MouseEvent");
    else event = newEvent(name, options);
    return trusted(event);
  };
  /* The keys a PSP user really presses. X on a highlighted element is
     Enter, as on a TV or console browser. The on-screen keyboards report
     as a phone keyboard does, key "Unidentified" with keyCode 229; their
     delete is Backspace. */
  const ENTER_KEY = { key: "Enter", code: "Enter", keyCode: 13 },
    VIRTUAL_KEY = { key: "Unidentified", code: "", keyCode: 229 },
    BACKSPACE_KEY = { key: "Backspace", code: "Backspace", keyCode: 8 },
    { HTMLInputElement, HTMLTextAreaElement } = globalThis,
    selectionOf = document.getSelection,
    selectionChanged = __tilefinchRealmGlobal.__tilefinchSelectionChanged,
    fireKey = (target, type, key) => browserDispatch(target,
      tilefinchEvent(type, {
        key: key.key,
        code: key.code,
        keyCode: key.keyCode,
        which: key.keyCode,
        charCode: type === "keypress" ? key.keyCode : 0,
      })),
    fireComposition = (target, type, data) => browserDispatch(target,
      tilefinchEvent(type, { data })),
    fireInput = (target, type, data, inputType, isComposing) =>
      browserDispatch(target, trusted(newEvent(type, {
        bubbles: true,
        composed: true,
        cancelable: type === "beforeinput" && !isComposing,
        data,
        inputType,
        isComposing,
      }, "InputEvent"))),
    activeElement = () => __tilefinchRealmGlobal.__tilefinchActiveElement,
    /* Keys go to the focused element. A highlighted element that cannot
       take focus leaves an unrelated earlier focus behind (a search field
       the user moved away from); its keys go to the body instead, so that
       field's Enter handler does not run. */
    bodyTarget = () => document.body || document.documentElement || document,
    keyTarget = (target) => {
      const active = activeElement();
      return active?.isConnected && (active === target ||
        (isNode(active) && active.contains(target))) ? active : bodyTarget();
    };
  /* One Enter press: keydown, then keypress unless keydown was cancelled.
     Cancelling either cancels what Enter does. keyup goes where focus is
     at the release, or back to the keydown target. */
  let enterTarget = null, enterFocus = null;
  const enterDown = (target) => {
      enterTarget = keyTarget(target);
      enterFocus = activeElement();
      if (!fireKey(enterTarget, "keydown", ENTER_KEY)) return false;
      /* A keydown handler that removed the focused element leaves the
         body focused. */
      if (!enterTarget.isConnected) enterTarget = bodyTarget();
      return fireKey(enterTarget, "keypress", ENTER_KEY);
    },
    enterUp = (target) => {
      const active = activeElement();
      let at = enterTarget || keyTarget(target);
      if (active && active !== enterFocus && active !== document.body)
        at = active;
      else if (!at.isConnected) at = bodyTarget();
      enterTarget = enterFocus = null;
      return fireKey(at, "keyup", ENTER_KEY);
    };
  __tilefinchRealmGlobal.__tilefinchDispatchAt = (selector, type) => {
    const target = selector
        ? document.querySelector(String(selector))
        : document,
      name = String(type);
    if (!target) return false;
    if ((name === "focus" || name === "blur") && isElement(target)) {
      timerApply(name === "focus" ? focusSteps : blurSteps, target, []);
      return true;
    }
    return browserDispatch(target, tilefinchEvent(name));
  };
  __tilefinchRealmGlobal.__tilefinchDispatchHandle = (handle, type) => {
    const name = String(type);
    if (
      name === "focus" &&
      !__tilefinchRealmGlobal.__tilefinchFocusEventsObserved?.() &&
      __tilefinchRealmGlobal.__tilefinchSetFocusHandle?.(handle)
    )
      return true;
    const target = trustedWrap(Number(handle));
    if (!target) return false;
    if ((name === "focus" || name === "blur") && isElement(target)) {
      timerApply(name === "focus" ? focusSteps : blurSteps, target, []);
      return true;
    }
    return browserDispatch(target, tilefinchEvent(name));
  };
  __tilefinchRealmGlobal.__tilefinchLabelDefault = (target, native = false) => {
    if (!(target instanceof HTMLLabelElement)) return false;
    const control = target.control;
    if (!control || control.disabled) return false;
    focusElement(control);
    if (!native) {
      timerApply(clickSteps, control, []);
      return true;
    }
    const state =
        __tilefinchRealmGlobal.__tilefinchBeginControlDefault?.(control, true) || null,
      clickEvent = tilefinchEvent("click");
    clickEvent.__tilefinchControlDefaultPrepared = true;
    const accepted = browserDispatch(control, clickEvent);
    __tilefinchRealmGlobal.__tilefinchFinishControlDefault?.(state, accepted);
    return true;
  };
  let pointerHoverTarget = null,
    pointerMoveProbeTarget = null,
    pointerCaptureTarget = null,
    pointerMarkupPossible =
      !!__tilefinchRealmGlobal.__tilefinchPointerMarkupInitiallyPresent;
  Element.prototype.setPointerCapture = function setPointerCapture(pointerId) {
    if (Number(pointerId) !== 1)
      throw new DOMException("Unknown pointer", "NotFoundError");
    if (pointerCaptureTarget === this) return;
    if (pointerCaptureTarget)
      browserDispatch(pointerCaptureTarget,
        tilefinchEvent("lostpointercapture", { pointerId: 1 }));
    pointerCaptureTarget = this;
    browserDispatch(this, tilefinchEvent("gotpointercapture", { pointerId: 1 }));
  };
  Element.prototype.releasePointerCapture = function releasePointerCapture(pointerId) {
    if (Number(pointerId) !== 1 || pointerCaptureTarget !== this) return;
    pointerCaptureTarget = null;
    browserDispatch(this, tilefinchEvent("lostpointercapture", { pointerId: 1 }));
  };
  Element.prototype.hasPointerCapture = function hasPointerCapture(pointerId) {
    return Number(pointerId) === 1 && pointerCaptureTarget === this;
  };
  __tilefinchRealmGlobal.__tilefinchPointerMarkupChanged = () => {
    pointerMarkupPossible = true;
    pointerMoveProbeTarget = null;
  };
  const dispatchHoverTransition = (next, point) => {
    const previous = pointerHoverTarget;
    if (previous === next) return true;
    const eventPath = __tilefinchRealmGlobal.__tilefinchShadowEventPath,
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
      browserDispatch(target,
        tilefinchEvent(name, { ...point, relatedTarget }));
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
  __tilefinchRealmGlobal.__tilefinchDispatchActivationHandle = (
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
          !!__tilefinchRealmGlobal.__tilefinchPointerMoveEventsObserved?.(),
        hoverObserved =
          !!__tilefinchRealmGlobal.__tilefinchPointerHoverEventsObserved?.();
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
    if (phase && phase < 6) {
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
        pageX: clientX + Number(__tilefinchRealmGlobal.scrollX || 0),
        pageY: clientY + Number(__tilefinchRealmGlobal.scrollY || 0),
        offsetX: Number(offsetX) || 0,
        offsetY: Number(offsetY) || 0,
        button: phase === 1 ? -1 : 0,
        buttons: Number(buttons) >>> 0,
        pointerId: 1,
        pressure: buttons ? 0.5 : 0,
      };
    }
    const fire = (name) => browserDispatch(target, tilefinchEvent(name, point));
    if (phase === 1) {
      const hoverAccepted =
        target === pointerHoverTarget ||
        (!__tilefinchRealmGlobal.__tilefinchPointerHoverEventsObserved?.() &&
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
        __tilefinchRealmGlobal.__tilefinchPointerMoveEventsObserved?.() ||
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
      if (pointerAccepted && mouseAccepted) focusElement(target);
      return pointerAccepted && mouseAccepted;
    }
    if (phase === 3) {
      const pointerAccepted = fire("pointerup"),
        mouseAccepted = fire("mouseup");
      if (pointerCaptureTarget) {
        const captured = pointerCaptureTarget;
        pointerCaptureTarget = null;
        browserDispatch(captured,
          tilefinchEvent("lostpointercapture", { pointerId: 1 }));
      }
      return pointerAccepted && mouseAccepted;
    }
    if (phase === 4) {
      const accepted = fire("pointercancel");
      if (pointerCaptureTarget) {
        const captured = pointerCaptureTarget;
        pointerCaptureTarget = null;
        browserDispatch(captured,
          tilefinchEvent("lostpointercapture", { pointerId: 1 }));
      }
      return accepted;
    }
    const clickDefault = () => {
      const controlState =
          __tilefinchRealmGlobal.__tilefinchBeginControlDefault?.(target, true) || null,
        clickEvent = tilefinchEvent("click", point);
      clickEvent.__tilefinchControlDefaultPrepared = true;
      let accepted = false,
        completed = false;
      try {
        accepted = browserDispatch(target, clickEvent);
        completed = true;
        if (
          accepted &&
          !controlState &&
          !__tilefinchRealmGlobal.__tilefinchDetailsDefault?.(target)
        )
          __tilefinchRealmGlobal.__tilefinchLabelDefault(target, true);
        return accepted;
      } finally {
        /* A watchdog can interrupt an author handler after the bootstrap has
           staged checkbox/radio selectedness but before dispatchEvent
           returns its cancellation bit. Roll the staged default back on
           every incomplete dispatch; C then classifies the admitted handler
           as RUNTIME_FAILED and must not replay it. */
        __tilefinchRealmGlobal.__tilefinchFinishControlDefault?.(
          controlState,
          completed && accepted,
        );
      }
    };
    if (phase === 5) return clickDefault();
    /* X on a highlighted element is an Enter press (6): keydown and
       keypress, the keyboard-style click (Pointer Events: pointerId -1, an
       empty pointerType, detail 0), keyup. 7 and 8 are the press and the
       release around a default C performs itself (a field's implicit
       submission). 0 is the click alone, for the default button that
       implicit submission clicks; it takes no focus. Pointer and mouse
       events belong to the cursor path above. */
    if (phase === 0) return clickDefault();
    if (phase === 6 || phase === 7) {
      focusElement(target);
      const accepted = enterDown(target);
      if (phase === 7) return accepted;
      let clicked = false;
      try {
        clicked = accepted && clickDefault();
      } finally {
        enterUp(target);
      }
      return clicked;
    }
    if (phase === 8) return enterUp(target);
    const pointerAccepted = fire("pointerdown"),
      mouseAccepted = fire("mousedown");
    if (pointerAccepted && mouseAccepted) focusElement(target);
    fire("pointerup");
    fire("mouseup");
    return clickDefault();
  };
  /* One keyboard edit, in Chrome's order. C calls "beforeinput" before
     the native change and "input" after it, or "inputabort" when the
     change does not happen after all; the key stays down in between.
     Typed text (insertText) is a phone keyboard's plain commit: keydown
     229, textInput, beforeinput, input, keyup. The keyboard session's Done
     (insertCompositionText) commits one composition: keydown 229,
     compositionstart, beforeinput (not cancelable), compositionupdate, the
     change, compositionend, input, keyup. Backspace's keydown and
     beforeinput can cancel its deletion; keyup follows either way. */
  let edit = null;
  const finishEdit = () => {
      const pending = edit;
      edit = null;
      return !pending || fireKey(pending.target.isConnected
        ? pending.target : bodyTarget(), "keyup", pending.key);
    },
    editSelectionChanged = (target, value) => {
      /* The edit leaves the caret after the text, as typing does. */
      if (target instanceof HTMLInputElement ||
          target instanceof HTMLTextAreaElement) {
        const end = value === null ? 0 : String(value).length;
        if (target.__selectionStart === end && target.__selectionEnd === end)
          return;
        target.__selectionStart = target.__selectionEnd = end;
        target.__selectionDirection = "none";
        selectionChanged?.(target);
      } else if (target.isConnected)
        timerApply(selectionOf, document, [])
          ?.collapse(target, target.childNodes.length);
    };
  __tilefinchRealmGlobal.__tilefinchDispatchInputHandle = (
    handle,
    type,
    data,
    inputType,
    currentValue,
  ) => {
    const name = String(type);
    if (name === "inputabort") {
      if (edit?.composing && edit.target.isConnected)
        fireComposition(edit.target, "compositionend", "");
      return finishEdit();
    }
    const target = trustedWrap(Number(handle));
    if (!target) return false;
    data = data === null ? null : String(data);
    inputType = String(inputType || "");
    const composing = inputType === "insertCompositionText";
    if (name === "beforeinput") {
      finishEdit();
      const key = inputType === "deleteContentBackward"
        ? BACKSPACE_KEY : VIRTUAL_KEY;
      edit = { target, key, composing };
      /* A phone keyboard's keydown cannot stop its text. */
      if (!fireKey(target, "keydown", key) && key === BACKSPACE_KEY) {
        finishEdit();
        return false;
      }
      if (composing) {
        fireComposition(target, "compositionstart", "");
        fireInput(target, name, data, inputType, true);
        fireComposition(target, "compositionupdate", data ?? "");
        return true;
      }
      /* Typed text first fires a cancelable textInput TextEvent, as in
         Chrome; cancelling it cancels the insertion like beforeinput. */
      if (inputType === "insertText" && data !== null && data !== "") {
        const text = createBootstrapEvent.call(document, "TextEvent");
        text.initTextEvent("textInput", true, true, __tilefinchRealmGlobal, data);
        if (!browserDispatch(target, trusted(text))) {
          finishEdit();
          return false;
        }
      }
      if (fireInput(target, name, data, inputType, false)) return true;
      finishEdit();
      return false;
    }
    if (name !== "input") return fireInput(target, name, data, inputType, false);
    if (currentValue !== null)
      __tilefinchRealmGlobal.__tilefinchSyncNativeControlValue?.(target, currentValue);
    if (edit?.composing) fireComposition(target, "compositionend", data ?? "");
    const accepted = fireInput(target, name, data, inputType, false);
    editSelectionChanged(target, currentValue);
    finishEdit();
    return accepted;
  };
  __tilefinchRealmGlobal.__tilefinchDispatchSubmitHandle = (formHandle, submitterHandle) => {
    const form = trustedWrap(Number(formHandle)),
      submitter = trustedWrap(Number(submitterHandle));
    if (!form) return false;
    if (!form.noValidate && !submitter?.formNoValidate && !form.checkValidity())
      return false;
    const proceed = browserDispatch(form,
      trusted(
        newEvent("submit", {
          bubbles: true,
          cancelable: true,
          submitter,
        }, "SubmitEvent"),
      ),
    );
    /* Native submission sends the same entry list a script submission does:
       values native control state cannot hold, plus `formdata` additions. A
       list that fails to build leaves the DOM serializer to send the form. */
    if (proceed) {
      try {
        __tilefinchRealmGlobal.__tilefinchSubmittedBody =
          __tilefinchRealmGlobal.__tilefinchSubmissionBody?.(form, submitter) ??
          null;
      } catch {
        __tilefinchRealmGlobal.__tilefinchSubmittedBody = null;
      }
    }
    return proceed;
  };
})();
