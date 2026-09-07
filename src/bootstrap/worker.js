/* Native installs this source first, removes the temporary installer property,
   and only then passes the privileged compiler as an argument.  Author code
   can poison ordinary constructors before this lazy module runs, but no such
   constructor can observe a compiler bridge on the global object. */
globalThis.__tilefinchInstallWorker = (
  runWorkerNative,
  traceWorkerNative,
  createWorkerRealmNative,
  destroyWorkerRealmNative,
) => {
  /* Worker is intentionally a first-use module. Ordinary pages should not pay
     to compile its message/lifecycle machinery merely because the constructor
     is standards-visible. */
  const blobForURL = globalThis.__tilefinchBlobForURL,
    trustedString = globalThis.__tilefinchTrustedString,
    workerIntrinsics = globalThis.__tilefinchWorkerIntrinsics,
    cloneWorkerValue = globalThis.__tilefinchCloneWorkerValue,
    createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap,
    createWorkerPerformance = globalThis.__tilefinchCreateWorkerPerformance,
    trustedEvent = globalThis.__tilefinchTrustedEvent,
    markNative = globalThis.__tilefinchMarkNativeFunction,
    ownerNavigator = globalThis.navigator,
    ownerLocation = globalThis.location,
    ownerPerformance = globalThis.performance;
  if (
    typeof runWorkerNative !== "function" ||
    typeof traceWorkerNative !== "function" ||
    typeof createWorkerRealmNative !== "function" ||
    typeof destroyWorkerRealmNative !== "function" ||
    typeof blobForURL !== "function" ||
    typeof trustedString !== "function" ||
    !workerIntrinsics || typeof workerIntrinsics.sourceForBlob !== "function" ||
    typeof workerIntrinsics.apply !== "function" ||
    typeof cloneWorkerValue !== "function" ||
    typeof createPrivateWeakMap !== "function" ||
    typeof createWorkerPerformance !== "function" ||
    typeof trustedEvent !== "function"
  )
    throw new Error("Worker bootstrap is unavailable");

  const identifierCode = (code) =>
      (code >= 48 && code <= 57) || (code >= 65 && code <= 90) ||
      (code >= 97 && code <= 122) || code === 36 || code === 95,
    replaceDynamicImport = (source) => {
      let output = "", copied = 0, search = 0;
      for (let count = 0; count < 128; count++) {
        const at = workerIntrinsics.indexOf(source, "import(", search);
        if (at < 0) break;
        search = at + 1;
        if (at && identifierCode(workerIntrinsics.charCodeAt(source, at - 1)))
          continue;
        output += workerIntrinsics.slice(source, copied, at) +
          "__tilefinchWorkerImport(";
        copied = at + 7;
        search = copied;
      }
      return copied ? output + workerIntrinsics.slice(source, copied) : source;
    },
    stripExportLists = (source) => {
      let output = "", copied = 0, search = 0;
      for (let count = 0; count < 128; count++) {
        const at = workerIntrinsics.indexOf(source, "export", search);
        if (at < 0) break;
        search = at + 1;
        if ((at && identifierCode(workerIntrinsics.charCodeAt(source, at - 1)))
            || identifierCode(workerIntrinsics.charCodeAt(source, at + 6)))
          continue;
        let open = at + 6;
        while (open < source.length) {
          const code = workerIntrinsics.charCodeAt(source, open);
          if (code !== 9 && code !== 10 && code !== 12 && code !== 13
              && code !== 32) break;
          open++;
        }
        if (workerIntrinsics.charCodeAt(source, open) !== 123) continue;
        const close = workerIntrinsics.indexOf(source, "}", open + 1);
        if (close < 0) break;
        let end = close + 1;
        while (end < source.length) {
          const code = workerIntrinsics.charCodeAt(source, end);
          if (code !== 9 && code !== 10 && code !== 12 && code !== 13
              && code !== 32) break;
          end++;
        }
        if (workerIntrinsics.charCodeAt(source, end) === 59) end++;
        output += workerIntrinsics.slice(source, copied, at) + ";";
        copied = end;
        search = end;
      }
      return copied ? output + workerIntrinsics.slice(source, copied) : source;
    },
    workerSourceForBlob = (blob) => {
      let source = workerIntrinsics.sourceForBlob(blob);
      source = workerIntrinsics.join(
        workerIntrinsics.split(source, "import.meta"),
        "__tilefinchWorkerMeta",
      );
      return stripExportLists(replaceDynamicImport(source));
    };

  const traceWorkerMessage = (direction, value) => {
      traceWorkerNative(direction, value);
    },
    workerStates = createPrivateWeakMap(),
    newHandlerSlots = () => ({
      message: { value: null, wrapper: null },
      error: { value: null, wrapper: null },
      messageerror: { value: null, wrapper: null },
    }),
    setEventHandler = (map, target, slots, type, value) => {
      const slot = slots[type],
        callback = typeof value === "function" ? value : null;
      if (callback === slot.value) return;
      slot.value = callback;
      if (callback !== null && slot.wrapper === null) {
        /* Event-handler attributes occupy one stable position in the event
           listener list. Replacing a non-null handler preserves that position;
           clearing and later restoring it registers a new position. */
        slot.wrapper = function (event) {
          const current = slot.value;
          if (typeof current === "function")
            return workerIntrinsics.apply(current, target, [event]);
        };
        globalThis.__tilefinchAddEventListener(
          map, type, slot.wrapper, false);
      } else if (callback === null && slot.wrapper !== null) {
        globalThis.__tilefinchRemoveEventListener(
          map, type, slot.wrapper, false);
        slot.wrapper = null;
      }
    },
    reportOwnerListenerError = (state, type, error, item, list) => {
      const handler = state.handlers[type]?.wrapper;
      traceWorkerMessage("owner-listener-error", {
        type: String(type),
        handler: item?.callback === handler,
        ordinal: list.indexOf(item),
        listeners: list.length,
        once: !!item?.once,
        active: !!item?.active,
        signal: !!item?.signal,
        message: String((error && error.message) || error),
        stack: workerIntrinsics.slice(
          trustedString((error && error.stack) || ""), 0, 4096),
        /* Validation tracing is a no-op in ordinary builds.  Retain a bounded
           copy of the actual failing callback here: challenge and framework
           bundles routinely install several anonymous listeners, so an
           ordinal and a minified exception alone cannot identify the missing
           platform contract.  Use captured intrinsics because page code may
           replace Function.prototype.toString or String.prototype.slice. */
        source: workerIntrinsics.slice(
          trustedString(item?.callback), 0, 4096),
      });
    },
    emitWorker = (owner, type, event = {}) => {
      const state = workerStates.get(owner);
      if (!state || !state.active) return;
      if (!(event instanceof Event)) event = new Event(type);
      if (type === "message")
        traceWorkerMessage("deliver-to-owner", event.data);
      globalThis.__tilefinchPrepareEvent(event, owner, [owner]);
      event.currentTarget = owner;
      event.eventPhase = Event.AT_TARGET;
      state.dispatchType = type;
      try {
        if (!event.__stopped) {
          globalThis.__tilefinchInvokeListenerList(
            state.listeners, owner, event, true, state.errorObserver);
          if (!event.__immediateStopped)
            globalThis.__tilefinchInvokeListenerList(
              state.listeners, owner, event, false, state.errorObserver);
        }
      } finally {
        event.currentTarget = null;
        event.eventPhase = Event.NONE;
        event.__dispatching = false;
        globalThis.__tilefinchRecordEvent();
      }
      return !event.defaultPrevented;
    },
    workerErrorEvent = (error, url) => trustedEvent(new ErrorEvent("error", {
      cancelable: true,
      message: String((error && error.message) || error),
      filename: String(url || ""),
      lineno: 0,
      colno: 0,
      /* Worker exceptions are not shared into the owner realm. */
      error: null,
    })),
    reportWorkerError = (owner, error, url) => {
      /* The owner's error listener may itself throw and replace the useful
         diagnostic.  Reuse the validation-only native trace seam to retain
         the originating Worker exception without exposing it to page code. */
      traceWorkerMessage("worker-error", {
        message: String((error && error.message) || error),
        stack: String((error && error.stack) || "").slice(0, 4096),
      });
      if (emitWorker(owner, "error", workerErrorEvent(error, url)))
        globalThis.__tilefinchReportUncaught(error, "worker");
    },
    workerPerformanceStates = createPrivateWeakMap(),
    workerPerformanceEntryTypes = Object.freeze([
      "mark", "measure", "resource",
    ]),
    WorkerPerformance = class Performance {
      constructor() { throw new TypeError("Illegal constructor"); }
      get timeOrigin() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return state.timeOrigin;
      }
      now() {
        const state = workerPerformanceStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return Math.max(0, ownerPerformance.now() - state.monotonicOrigin);
      }
      mark(name, options) { return ownerPerformance.mark(name, options); }
      measure(name, start, end) {
        return ownerPerformance.measure(name, start, end);
      }
      getEntries() {
        return ownerPerformance.getEntries().filter((entry) =>
          workerPerformanceEntryTypes.includes(entry.entryType));
      }
      getEntriesByType(type) {
        type = String(type);
        return workerPerformanceEntryTypes.includes(type)
          ? ownerPerformance.getEntriesByType(type) : [];
      }
      getEntriesByName(name, type) {
        const entries = ownerPerformance.getEntriesByName(name, type);
        return entries.filter((entry) =>
          workerPerformanceEntryTypes.includes(entry.entryType));
      }
      clearMarks(name) { return ownerPerformance.clearMarks(name); }
      clearMeasures(name) { return ownerPerformance.clearMeasures(name); }
      clearResourceTimings() { return ownerPerformance.clearResourceTimings(); }
      setResourceTimingBufferSize(size) {
        return ownerPerformance.setResourceTimingBufferSize(size);
      }
      toJSON() { return { timeOrigin: this.timeOrigin }; }
    },
    createDedicatedWorkerPerformance = () => {
      const value = Object.create(WorkerPerformance.prototype);
      workerPerformanceStates.set(value, {
        timeOrigin: Date.now(),
        monotonicOrigin: ownerPerformance.now(),
      });
      return value;
    };
  Object.defineProperty(WorkerPerformance.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Performance",
  });
  markNative(WorkerPerformance);
  for (const key of Reflect.ownKeys(WorkerPerformance.prototype)) {
    const descriptor = Object.getOwnPropertyDescriptor(
      WorkerPerformance.prototype, key,
    );
    markNative(descriptor?.value);
    markNative(descriptor?.get);
    markNative(descriptor?.set);
  }
  let activeWorkers = 0;
  globalThis.Worker = class Worker extends EventTarget {
    constructor(url) {
      super();
      /* WebIDL string conversion is observable author code.  Snapshot it once
         and bind Blob lookup, CSP admission, location and diagnostics to that
         exact value so a stateful toString() cannot split validation from
         use. */
      const workerURL = trustedString(url),
        blob = blobForURL(workerURL);
      if (!blob) throw new TypeError("only retained blob URLs are supported");
      if (activeWorkers >= 2) throw new RangeError("worker quota exceeded");
      activeWorkers++;
      let state = null;
      try {
      const owner = this,
        WorkerGlobalScope = class WorkerGlobalScope extends EventTarget {
          constructor() {
            super();
            throw new TypeError("Illegal constructor");
          }
        },
        DedicatedWorkerGlobalScope = class DedicatedWorkerGlobalScope
          extends WorkerGlobalScope {
          constructor() {
            super();
          }
        },
        WorkerNavigator = class WorkerNavigator {
          constructor() { throw new TypeError("Illegal constructor"); }
          get userAgent() { return String(ownerNavigator.userAgent || ""); }
          get language() { return String(ownerNavigator.language || ""); }
          get languages() { return workerLanguages; }
          get platform() { return String(ownerNavigator.platform || ""); }
          get onLine() { return !!ownerNavigator.onLine; }
          get hardwareConcurrency() {
            return Math.max(1, Number(ownerNavigator.hardwareConcurrency) || 1);
          }
          get deviceMemory() {
            return Number(ownerNavigator.deviceMemory) || 0.25;
          }
          get userAgentData() { return ownerNavigator.userAgentData; }
          get connection() { return ownerNavigator.connection; }
          get permissions() { return ownerNavigator.permissions; }
          get storage() { return ownerNavigator.storage; }
          get gpu() { return ownerNavigator.gpu; }
        },
        WorkerLocation = class WorkerLocation {
          constructor() { throw new TypeError("Illegal constructor"); }
          get href() { return workerURL; }
          get origin() { return String(ownerLocation.origin || "null"); }
          get protocol() { return workerURL.startsWith("blob:") ? "blob:" : ""; }
          get host() { return ""; }
          get hostname() { return ""; }
          get port() { return ""; }
          get pathname() {
            return workerURL.startsWith("blob:") ? workerURL.slice(5) : workerURL;
          }
          get search() { return ""; }
          get hash() { return ""; }
          toString() { return this.href; }
        },
        scope = Object.create(DedicatedWorkerGlobalScope.prototype);
        state = {
          active: true,
          startTimer: 0,
          handlers: newHandlerSlots(),
          scopeHandlers: newHandlerSlots(),
          listeners: new Map(),
          scopeListeners: new Map(),
          timers: new Set(),
          scope: null,
          dispatchType: "",
          errorObserver: null,
      };
      state.errorObserver = (error, item, list) =>
        reportOwnerListenerError(
          state, state.dispatchType, error, item, list);
      Object.defineProperty(WorkerGlobalScope.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerGlobalScope",
      });
      Object.defineProperty(
        DedicatedWorkerGlobalScope.prototype,
        Symbol.toStringTag,
        { configurable: true, value: "DedicatedWorkerGlobalScope" },
      );
      Object.defineProperty(WorkerNavigator.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerNavigator",
      });
      Object.defineProperty(WorkerLocation.prototype, Symbol.toStringTag, {
        configurable: true,
        value: "WorkerLocation",
      });
      for (const constructor of [
        WorkerGlobalScope,
        DedicatedWorkerGlobalScope,
        WorkerNavigator,
        WorkerLocation,
      ]) {
        markNative(constructor);
        for (const key of Reflect.ownKeys(constructor.prototype)) {
          const descriptor = Object.getOwnPropertyDescriptor(
            constructor.prototype,
            key,
          );
          markNative(descriptor?.value);
          markNative(descriptor?.get);
          markNative(descriptor?.set);
        }
      }
      workerStates.set(this, state);
      scope.WorkerGlobalScope = WorkerGlobalScope;
      scope.DedicatedWorkerGlobalScope = DedicatedWorkerGlobalScope;
      scope.WorkerNavigator = WorkerNavigator;
      scope.WorkerLocation = WorkerLocation;
      scope.name = "";
      scope.postMessage = (value) => {
        traceWorkerMessage("worker-to-owner", value);
        const copied = cloneWorkerValue(value);
        state.outboundPending = (state.outboundPending || 0) + 1;
        setTimeout(() => {
          try {
            emitWorker(
              owner,
              "message",
              trustedEvent(new MessageEvent("message", {
                data: copied,
                origin: "",
                source: null,
                ports: [],
              })),
            );
          } finally {
            state.outboundPending--;
            finishWorkerClose(state);
          }
        }, 0);
      };
      scope.addEventListener = (type, callback, options = false) =>
        globalThis.__tilefinchAddEventListener(
          state.scopeListeners, type, callback, options);
      scope.removeEventListener = (type, callback, options = false) =>
        globalThis.__tilefinchRemoveEventListener(
          state.scopeListeners, type, callback, options);
      Object.defineProperties(scope, {
        onmessage: {
          configurable: true,
          get() { return state.scopeHandlers.message.value; },
          set(value) {
            setEventHandler(
              state.scopeListeners, state.scope || scope,
              state.scopeHandlers, "message", value);
          },
        },
        onmessageerror: {
          configurable: true,
          get() { return state.scopeHandlers.messageerror.value; },
          set(value) {
            setEventHandler(
              state.scopeListeners, state.scope || scope,
              state.scopeHandlers, "messageerror", value);
          },
        },
        onerror: {
          configurable: true,
          get() { return state.scopeHandlers.error.value; },
          set(value) {
            setEventHandler(
              state.scopeListeners, state.scope || scope,
              state.scopeHandlers, "error", value);
          },
        },
      });
      scope.crypto = crypto;
      scope.performance = createDedicatedWorkerPerformance();
      scope.Performance = WorkerPerformance;
      /* Navigation and paint entries belong to Window, not a dedicated
         worker.  Reuse the bounded observer implementation but expose only
         the entry kinds present in Chromium workers. */
      const OwnerPerformanceObserver = globalThis.PerformanceObserver;
      scope.PerformanceObserver = class PerformanceObserver
        extends OwnerPerformanceObserver {};
      Object.defineProperty(
        scope.PerformanceObserver, "supportedEntryTypes",
        { value: workerPerformanceEntryTypes, enumerable: true },
      );
      scope.crossOriginIsolated = false;
      const scheduleWorkerTimer = (callback, delay, repeat, args) => {
          if (typeof callback !== "function") return 0;
          let id = 0;
          const invoke = () => {
            if (!repeat) state.timers.delete(id);
            if (!state.active || state.closing) return;
            try {
              callback.apply(state.scope, args);
            } catch (error) {
              reportWorkerError(owner, error, workerURL);
            }
          };
          id = repeat
            ? setInterval(invoke, Math.max(1, Number(delay) || 0))
            : setTimeout(invoke, delay);
          if (id) state.timers.add(id);
          return id;
        },
        clearWorkerTimer = (id) => {
          clearTimeout(id);
          state.timers.delete(Number(id));
        };
      scope.setTimeout = (callback, delay, ...args) =>
        scheduleWorkerTimer(callback, delay, false, args);
      scope.setInterval = (callback, delay, ...args) =>
        scheduleWorkerTimer(callback, delay, true, args);
      scope.clearTimeout = clearWorkerTimer;
      scope.clearInterval = clearWorkerTimer;
      scope.TextEncoder = TextEncoder;
      scope.TextDecoder = TextDecoder;
      scope.Uint8Array = Uint8Array;
      scope.ArrayBuffer = ArrayBuffer;
      scope.DataView = DataView;
      scope.Blob = Blob;
      scope.URL = URL;
      scope.URLSearchParams = URLSearchParams;
      scope.Headers = Headers;
      scope.Request = Request;
      scope.Response = Response;
      /* Every worker fetch is tied to the worker's lifetime: terminate() and
         close() abort it. An author-supplied signal still aborts it too. */
      state.abortController = new AbortController();
      scope.fetch = (input, init) => {
        if (!state.active || !state.abortController)
          return Promise.reject(
            new DOMException("Worker is terminated", "InvalidStateError"),
          );
        const options = Object.assign({}, init || {}),
          authorSignal = options.signal,
          controller = new AbortController(),
          abort = () =>
            controller.abort(
              authorSignal && authorSignal.aborted
                ? authorSignal.reason
                : state.abortController
                  ? state.abortController.signal.reason
                  : undefined,
            );
        options.signal = controller.signal;
        if (authorSignal) {
          if (authorSignal.aborted) abort();
          else authorSignal.addEventListener("abort", abort, { once: true });
        }
        state.abortController.signal.addEventListener(
          "abort", abort, { once: true },
        );
        const lifetimeSignal = state.abortController.signal,
          cleanup = () => {
            lifetimeSignal.removeEventListener("abort", abort);
            if (authorSignal)
              authorSignal.removeEventListener("abort", abort);
          };
        try {
          return fetch(input, options).then(
            value => { cleanup(); return value; },
            error => { cleanup(); throw error; },
          );
        } catch (error) { cleanup(); throw error; }
      };
      scope.atob = atob;
      scope.btoa = btoa;
      scope.structuredClone = structuredClone;
      scope.queueMicrotask = callback => {
        if (typeof callback !== "function")
          throw new TypeError("microtask callback must be callable");
        queueMicrotask(() => {
          if (state.active && !state.closing) callback();
        });
      };
      const workerLanguages = Object.freeze(
          Array.from(ownerNavigator.languages || []).slice(0, 8),
        ),
        workerNavigator = Object.freeze(Object.create(WorkerNavigator.prototype)),
        workerLocation = Object.freeze(Object.create(WorkerLocation.prototype));
      Object.defineProperties(scope, {
        navigator: { value: workerNavigator, enumerable: true },
        location: { value: workerLocation, enumerable: true },
      });
      const windowOnlyGlobals = new Set([
        "document",
        "window",
        "parent",
        "top",
        "frames",
        "history",
        "localStorage",
        "sessionStorage",
        "customElements",
        "Window",
        "screen",
        "visualViewport",
        "opener",
        "frameElement",
        "speechSynthesis",
        "SpeechSynthesis",
        "SpeechSynthesisVoice",
        "SpeechSynthesisEvent",
        "SpeechSynthesisErrorEvent",
        "SpeechSynthesisUtterance",
      ]);
      /* The worker runs in its own realm (see createWorkerRealmNative).
         Names it does not define resolve, read-only, to the owner's
         globals through this fallback deep in the prototype chain — never
         to window-only surfaces — so `console`, `crypto`, `Event` and the
         lazily installed constructors keep working while `this`,
         `self` and `globalThis` are one genuine worker global. */
      const fallback = new Proxy(Object.create(EventTarget.prototype), {
        has(target, key) {
          return (
            key in target ||
            (typeof key === "string" && !windowOnlyGlobals.has(key) &&
              key in globalThis)
          );
        },
        get(target, key, receiver) {
          if (key in target) return Reflect.get(target, key, receiver);
          return typeof key === "string" && !windowOnlyGlobals.has(key)
            ? globalThis[key]
            : undefined;
        },
        set(target, key, value, receiver) {
          return Reflect.defineProperty(receiver, key, {
            value,
            writable: true,
            enumerable: true,
            configurable: true,
          });
        },
      });
      Object.setPrototypeOf(WorkerGlobalScope.prototype, fallback);
      /* DedicatedWorkerGlobalScope.close(): messages the worker already
         posted in this task still reach the owner, further tasks queued to
         the worker (timers, owner messages) are discarded, and the shutdown
         itself runs once every task-0 timer queued before it has drained. */
      scope.close = () => {
        if (!state.active || state.closing) return;
        state.closing = true;
        /* Shut down once the closing task has ended and every message
           the worker posted (before or after close()) has reached the
           owner. Timer order alone cannot express that: a zero-delay
           timer scheduled during a drain may run in the same pass. */
        setTimeout(() => {
          state.closeTaskEnded = true;
          finishWorkerClose(state);
        }, 0);
      };
      scope.importScripts = (...urls) => {
        if (!state.active)
          throw new DOMException("Worker is terminated", "InvalidStateError");
        if (urls.length > 8)
          throw new RangeError("worker importScripts quota exceeded");
        for (const value of urls) {
          const importedURL = trustedString(value),
            importedBlob = blobForURL(importedURL);
          if (!importedBlob)
            throw new DOMException(
              "Worker script could not be loaded", "NetworkError",
            );
          const importedSource = workerSourceForBlob(importedBlob);
          runWorkerNative(state.realm, importedSource, importedURL);
        }
      };
      scope.__tilefinchWorkerMeta = { url: workerURL };
      scope.__tilefinchWorkerImport = (specifier) =>
        new Promise((resolve, reject) => {
          let tries = 0;
          const attempt = () => {
            if (!state.active || state.closing) return;
            import(String(specifier)).then(resolve, (error) => {
              if (!state.active || state.closing) return;
              if (++tries >= 40) reject(error);
              else scope.setTimeout(attempt, 100);
            });
          };
          scope.setTimeout(attempt, 400);
        });
      state.realm = createWorkerRealmNative(
        scope, DedicatedWorkerGlobalScope.prototype);
      /* A fresh QuickJS global stringifies as [object global]; the worker
         global must present as its scope interface. */
      Object.defineProperty(state.realm, Symbol.toStringTag, {
        value: "DedicatedWorkerGlobalScope",
        configurable: true,
      });
      state.scope = state.realm;
        const source = workerSourceForBlob(blob);
        /* Snapshot the retained Blob before revokeObjectURL can remove its URL,
           then compile/evaluate on the next task turn. */
        state.startTimer = setTimeout(() => {
          state.startTimer = 0;
          if (!state.active) return;
          try {
            runWorkerNative(state.realm, source, workerURL);
          } catch (error) {
            reportWorkerError(this, error, workerURL);
          }
        }, 0);
      } catch (error) {
        activeWorkers--;
        if (state) state.active = false;
        if (state?.realm) {
          try {
            destroyWorkerRealmNative(state.realm);
          } catch (_) {}
          state.realm = null;
        }
        state?.listeners.clear();
        if (state) state.scope = null;
        workerStates.delete(this);
        throw error;
      }
    }
    get onmessage() {
      return workerStates.get(this)?.handlers.message.value || null;
    }
    set onmessage(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(state.listeners, this, state.handlers, "message", value);
    }
    get onerror() {
      return workerStates.get(this)?.handlers.error.value || null;
    }
    set onerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(state.listeners, this, state.handlers, "error", value);
    }
    get onmessageerror() {
      return workerStates.get(this)?.handlers.messageerror.value || null;
    }
    set onmessageerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      setEventHandler(
        state.listeners, this, state.handlers, "messageerror", value);
    }
    addEventListener(type, callback, options = false) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return globalThis.__tilefinchAddEventListener(
        state.listeners, type, callback, options);
    }
    removeEventListener(type, callback, options = false) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return globalThis.__tilefinchRemoveEventListener(
        state.listeners, type, callback, options);
    }
    postMessage(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (!state.active) throw new Error("Worker is terminated");
      traceWorkerMessage("owner-to-worker", value);
      const copied = cloneWorkerValue(value);
      setTimeout(() => {
        if (!state.active || state.closing) return;
        const event = trustedEvent(new MessageEvent("message", {
          data: copied,
          origin: "",
          source: null,
          ports: [],
        }));
        globalThis.__tilefinchPrepareEvent(
          event, state.scope, [state.scope]);
        event.currentTarget = state.scope;
        event.eventPhase = Event.AT_TARGET;
        try {
          if (!event.__stopped) {
            globalThis.__tilefinchInvokeListenerList(
              state.scopeListeners, state.scope, event, true);
            if (!event.__immediateStopped)
              globalThis.__tilefinchInvokeListenerList(
                state.scopeListeners, state.scope, event, false);
          }
        } finally {
          event.currentTarget = null;
          event.eventPhase = Event.NONE;
          event.__dispatching = false;
          globalThis.__tilefinchRecordEvent();
        }
      }, 0);
    }
    terminate() {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      shutdownWorkerState(state);
    }
  };
  /* Shared by Worker.terminate() and DedicatedWorkerGlobalScope.close().
     Besides timers and listeners, abort every fetch the worker started so
     its continuations stop consuming the page's callback and network
     budgets after termination. */
  function finishWorkerClose(state) {
    if (state.closing && state.closeTaskEnded && !(state.outboundPending > 0))
      shutdownWorkerState(state);
  }
  function shutdownWorkerState(state) {
    if (!state.active) return;
    state.active = false;
    activeWorkers--;
    if (state.realm) {
      try {
        destroyWorkerRealmNative(state.realm);
      } catch (_) {}
      state.realm = null;
    }
    if (state.startTimer) {
      clearTimeout(state.startTimer);
      state.startTimer = 0;
    }
    for (const id of state.timers) clearTimeout(id);
    state.timers.clear();
    state.listeners.clear();
    state.scopeListeners.clear();
    if (state.abortController) {
      try {
        state.abortController.abort(
          new DOMException("Worker is terminated", "AbortError"),
        );
      } catch (_) {}
      state.abortController = null;
    }
    state.scope = null;
  }
  Object.defineProperty(globalThis.Worker.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Worker",
  });
  markNative(globalThis.Worker);
  for (const key of Reflect.ownKeys(globalThis.Worker.prototype)) {
    const descriptor = Object.getOwnPropertyDescriptor(
      globalThis.Worker.prototype,
      key,
    );
    markNative(descriptor?.value);
    markNative(descriptor?.get);
    markNative(descriptor?.set);
  }
};
