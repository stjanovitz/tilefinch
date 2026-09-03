/* Native installs this source first, removes the temporary installer property,
   and only then passes the privileged compiler as an argument.  Author code
   can poison ordinary constructors before this lazy module runs, but no such
   constructor can observe a compiler bridge on the global object. */
globalThis.__tilefinchInstallWorker = (runWorkerNative, traceWorkerNative) => {
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
    ownerLocation = globalThis.location;
  if (
    typeof runWorkerNative !== "function" ||
    typeof traceWorkerNative !== "function" ||
    typeof blobForURL !== "function" ||
    typeof trustedString !== "function" ||
    !workerIntrinsics || typeof workerIntrinsics.sourceForBlob !== "function" ||
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
    emitWorker = (owner, type, event = {}) => {
      const state = workerStates.get(owner);
      if (!state || !state.active) return;
      if (!(event instanceof Event)) event = new Event(type);
      globalThis.__tilefinchPrepareEvent(event, owner, [owner]);
      event.currentTarget = owner;
      event.eventPhase = Event.AT_TARGET;
      try {
        const handler = state.handlers[type];
        if (typeof handler === "function")
          try {
            globalThis.__tilefinchRecordEventHandler();
            globalThis.__tilefinchRunTask(
              "worker:" + String(type), handler, owner, [event]);
          } catch (error) {
            globalThis.__tilefinchReportUncaught(
              error, "worker " + String(type));
          }
        if (!event.__stopped) {
          globalThis.__tilefinchInvokeListenerList(
            state.listeners, owner, event, true);
          if (!event.__immediateStopped)
            globalThis.__tilefinchInvokeListenerList(
              state.listeners, owner, event, false);
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
      if (emitWorker(owner, "error", workerErrorEvent(error, url)))
        globalThis.__tilefinchReportUncaught(error, "worker");
    };
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
        scope = Object.create(DedicatedWorkerGlobalScope.prototype),
        state = {
          active: true,
          startTimer: 0,
          handlers: { message: null, error: null, messageerror: null },
          listeners: new Map(),
          scopeListeners: new Map(),
          timers: new Set(),
          scope: null,
      };
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
      scope.self = scope;
      scope.globalThis = scope;
      scope.WorkerGlobalScope = WorkerGlobalScope;
      scope.DedicatedWorkerGlobalScope = DedicatedWorkerGlobalScope;
      scope.WorkerNavigator = WorkerNavigator;
      scope.WorkerLocation = WorkerLocation;
      scope.name = "";
      scope.postMessage = (value) => {
        traceWorkerMessage("worker-to-owner", value);
        const copied = cloneWorkerValue(value);
        setTimeout(() => emitWorker(
          owner,
          "message",
          trustedEvent(new MessageEvent("message", {
            data: copied,
            origin: "",
            source: null,
            ports: [],
          })),
        ), 0);
      };
      scope.addEventListener = (type, callback, options = false) =>
        globalThis.__tilefinchAddEventListener(
          state.scopeListeners, type, callback, options);
      scope.removeEventListener = (type, callback, options = false) =>
        globalThis.__tilefinchRemoveEventListener(
          state.scopeListeners, type, callback, options);
      scope.crypto = crypto;
      scope.performance = createWorkerPerformance();
      scope.crossOriginIsolated = false;
      const scheduleWorkerTimer = (callback, delay, repeat, args) => {
          if (typeof callback !== "function") return 0;
          let id = 0;
          const invoke = () => {
            if (!repeat) state.timers.delete(id);
            if (!state.active) return;
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
      scope.fetch = fetch;
      scope.atob = atob;
      scope.btoa = btoa;
      scope.structuredClone = structuredClone;
      scope.queueMicrotask = queueMicrotask;
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
        "screen",
        "visualViewport",
        "opener",
        "frameElement",
      ]);
      const proxy = new Proxy(scope, {
        has(target, key) {
          return (
            key in target ||
            (!windowOnlyGlobals.has(key) && key in globalThis)
          );
        },
        get(target, key) {
          if (key in target) return target[key];
          return windowOnlyGlobals.has(key) ? undefined : globalThis[key];
        },
        set(target, key, value) {
          target[key] = value;
          return true;
        },
      });
      state.scope = proxy;
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
          runWorkerNative(proxy, importedSource, importedURL);
        }
      };
      try {
        const source = workerSourceForBlob(blob);
        scope.__tilefinchWorkerMeta = { url: workerURL };
        /* Snapshot the retained Blob before revokeObjectURL can remove its URL,
           then compile/evaluate on the next task turn. */
        state.startTimer = setTimeout(() => {
          state.startTimer = 0;
          if (!state.active) return;
          try {
            runWorkerNative(proxy, source, workerURL);
          } catch (error) {
            if (globalThis.console && console.log)
              console.log(
                "tilefinch-worker-error: " +
                  String(error) +
                  " || " +
                  String((error && error.stack) || ""),
              );
            reportWorkerError(this, error, workerURL);
          }
        }, 0);
      } catch (error) {
        state.active = false;
        activeWorkers--;
        state.listeners.clear();
        state.scope = null;
        workerStates.delete(this);
        throw error;
      }
    }
    get onmessage() {
      return workerStates.get(this)?.handlers.message || null;
    }
    set onmessage(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      state.handlers.message = typeof value === "function" ? value : null;
    }
    get onerror() {
      return workerStates.get(this)?.handlers.error || null;
    }
    set onerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      state.handlers.error = typeof value === "function" ? value : null;
    }
    get onmessageerror() {
      return workerStates.get(this)?.handlers.messageerror || null;
    }
    set onmessageerror(value) {
      const state = workerStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      state.handlers.messageerror =
        typeof value === "function" ? value : null;
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
        if (!state.active) return;
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
          const handler = state.scope.onmessage;
          if (typeof handler === "function")
            try {
              globalThis.__tilefinchRunTask(
                "worker-scope-message", handler, state.scope, [event]);
            } catch (error) {
              reportWorkerError(this, error, workerURL);
            }
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
      if (!state.active) return;
      state.active = false;
      activeWorkers--;
      if (state.startTimer) {
        clearTimeout(state.startTimer);
        state.startTimer = 0;
      }
      for (const id of state.timers) clearTimeout(id);
      state.timers.clear();
      state.listeners.clear();
      state.scopeListeners.clear();
      state.scope = null;
    }
  };
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
