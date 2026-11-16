(() => {
  const nativeOwnKeys = Reflect.ownKeys,
    ownPropertyNames = Object.getOwnPropertyNames,
    ownPropertyDescriptors = Object.getOwnPropertyDescriptors,
    deleteProperty = Reflect.deleteProperty,
    getDescriptor = Object.getOwnPropertyDescriptor,
    defineProperty = Object.defineProperty,
    freeze = Object.freeze;
  /* Set by the host for host and validation realms only; the probes it
     enabled are published by now. */
  deleteProperty(globalThis, "__tilefinchTestProbes");
  /* Every bootstrap module captured the trusted-event share (dom.js); the
     lazy modules get its event creator, brand check and interfaces from
     the globals published below. */
  const createEvent =
      globalThis.__tilefinchTrustedEventShare.createBrowserEvent,
    isEvent = globalThis.__tilefinchTrustedEventShare.isEvent,
    eventInterfaces = globalThis.__tilefinchTrustedEventShare.interfaces();
  deleteProperty(globalThis, "__tilefinchTrustedEventShare");
  const markNative = globalThis.__tilefinchMarkNativeFunction,
    markInterface = (constructor) => {
      if (typeof constructor !== "function") return;
      markNative(constructor);
      const prototype = constructor.prototype;
      if (!prototype) return;
      for (const key of nativeOwnKeys(prototype)) {
        const descriptor = getDescriptor(prototype, key);
        if (!descriptor) continue;
        markNative(descriptor.value);
        markNative(descriptor.get);
        markNative(descriptor.set);
      }
    };
  /* Lazy trusted modules run after author code can replace standard globals.
     Preserve only the narrow intrinsic operation they need rather than
     freezing the page-visible WeakMap constructor.  In particular, Worker
     must not invoke an author constructor while its temporary native compiler
     bridge exists. */
  const NativeWeakMap = WeakMap,
    privateWeakMapGet = Function.call.bind(WeakMap.prototype.get),
    privateWeakMapSet = Function.call.bind(WeakMap.prototype.set),
    privateWeakMapDelete = Function.call.bind(WeakMap.prototype.delete),
    createPrivateWeakMap = () => {
      const map = new NativeWeakMap();
      return freeze({
        get: (key) => privateWeakMapGet(map, key),
        set: (key, value) => {
          privateWeakMapSet(map, key, value);
          return value;
        },
        delete: (key) => privateWeakMapDelete(map, key),
      });
    };
  markNative(createPrivateWeakMap);
  defineProperty(globalThis, "__tilefinchCreatePrivateWeakMap", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: createPrivateWeakMap,
  });
  /* Lazy modules mark some of the events they build trusted, so they must
     not build them with whatever constructor the page left at
     globalThis.Event. A page that calls this gets an ordinary untrusted
     event. */
  markNative(createEvent);
  defineProperty(globalThis, "__tilefinchCreateEvent", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: createEvent,
  });
  /* A page that calls it learns only whether a value is a real Event. */
  markNative(isEvent);
  defineProperty(globalThis, "__tilefinchIsEvent", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: isEvent,
  });
  /* The interfaces lazy modules extend or construct, keyed by global
     name: compat.js's event interfaces (Event, EventTarget and the Event
     subclasses it defines), captured before author code, and DOMException
     and PerformanceEntry, captured here, still before author code. Lazy
     modules evaluate after the page may have replaced those globals, so
     they take these instead (`const { Event, EventTarget } =
     __tilefinchEventInterfaces`). Every entry is the object the page
     itself received at that global, so the table grants a page nothing it
     lacked. Navigator, Document and the element interfaces are those whose
     prototypes lazy modules add members to; the native lazy-module
     triggers find those prototypes here too (runtime_creation.inc). The
     other names in the loop below are the eagerly defined globals a Worker
     shares with its owner (worker.js): the worker reads these instead of
     whatever the owner page left at the global. The loop reads
     descriptors, so it never starts a lazy module. addListener and
     removeListener are EventTarget's own methods as call-bound functions
     (`addListener(target, type, f)`): event handler attributes and the
     browser's own listeners register through them, never through a
     page's patched EventTarget.prototype method, as in browsers; dispatch
     is dispatchEvent the same way. The ECMAScript and platform
     constructors after console are those lazy modules build errors,
     promises, collections and platform objects with: a lazy module
     destructures the ones it names, so its `new TypeError(...)` is the
     realm's whatever the page left at the global. defineProperty, apply,
     promiseThen and promiseReject are Object.defineProperty, Reflect.apply,
     Promise.prototype.then (call-bound) and Promise.reject as they were
     before author code ran; lazy modules publish their factories and
     private hooks through defineProperty. getOwnPropertyDescriptor, ownKeys
     (the realm's own, not the reflection filter installed below), construct,
     freeze and push (Array.prototype.push, call-bound: `push(array, value)`)
     are the same: what a lazy module calls while it loads must not be a
     page's patch, which would run page code inside the module's marking
     window (runtime_platform_window_open). */
  const captured = {
      __proto__: null,
      ...eventInterfaces,
      DOMException: globalThis.DOMException,
      PerformanceEntry: globalThis.PerformanceEntry,
    },
    eventTargetPrototype = eventInterfaces.EventTarget.prototype;
  for (const name of [
    "Navigator", "Document", "Node", "Element", "HTMLElement",
    "HTMLCanvasElement", "ShadowRoot", "StorageManager",
    "AbortController", "AbortSignal", "BroadcastChannel", "Crypto",
    "CustomEvent", "DOMRect", "EventSource", "File", "FileList", "FileReader",
    "FormData", "GPU", "MessageChannel", "MessagePort", "NavigatorUAData",
    "NetworkInformation", "PerformanceMark", "PerformanceMeasure",
    "PerformanceObserverEntryList", "PerformanceResourceTiming",
    "PermissionStatus", "Permissions", "SubtleCrypto",
    "WGSLLanguageFeatures", "WebSocket", "XMLHttpRequest",
    "XMLHttpRequestEventTarget", "XMLHttpRequestUpload", "console",
    "Error", "TypeError", "RangeError", "SyntaxError", "Promise", "Map",
    "Set", "WeakMap", "WeakSet", "WeakRef", "Proxy", "Array", "ArrayBuffer",
    "Uint8Array", "Uint8ClampedArray", "Int32Array", "Uint32Array",
    "Float32Array", "Float64Array", "Date", "RegExp", "URL", "Headers",
    "Request", "Response", "Blob", "TextEncoder", "TextDecoder",
    "URLSearchParams",
  ]) {
    const value = getDescriptor(globalThis, name)?.value;
    if (value !== undefined) captured[name] = value;
  }
  captured.addListener = Function.call.bind(
    eventTargetPrototype.addEventListener);
  captured.removeListener = Function.call.bind(
    eventTargetPrototype.removeEventListener);
  captured.dispatch = Function.call.bind(eventTargetPrototype.dispatchEvent);
  captured.defineProperty = defineProperty;
  captured.getOwnPropertyDescriptor = getDescriptor;
  captured.ownKeys = nativeOwnKeys;
  captured.construct = Reflect.construct;
  captured.freeze = freeze;
  captured.push = Function.call.bind(Array.prototype.push);
  /* AbortController's own abort() and signal getter, and AbortSignal's
     reason getter, call-bound: worker.js ends a worker's lifetime signal
     with these, never with a page's patch of them. */
  const controllerPrototype = globalThis.AbortController.prototype;
  captured.abortController = Function.call.bind(controllerPrototype.abort);
  captured.controllerSignal = Function.call.bind(
    getDescriptor(controllerPrototype, "signal").get);
  captured.signalReason = Function.call.bind(
    getDescriptor(globalThis.AbortSignal.prototype, "reason").get);
  captured.apply = Reflect.apply;
  captured.promiseThen = Function.call.bind(Promise.prototype.then);
  captured.promiseReject = Promise.reject.bind(Promise);
  /* The private hooks lazy modules publish for eager modules and native
     code to call by name, reserved before author code as getters: a page
     can neither define one before its module loads (so its function is
     never called in the module's place) nor set or replace one. A module
     sets its own feature's hooks through the publisher claimHooks() gives
     it as its first statement: the host hands each load one claim
     (__tilefinchLazyLoading), so page code that runs while the module
     loads gets none, and a publisher works only while its load lasts.
     The indices are the host's LazyBootstrapFeature values. */
  const lazyLoading = globalThis.__tilefinchLazyLoading;
  deleteProperty(globalThis, "__tilefinchLazyLoading");
  if (typeof lazyLoading === "function") {
    const hooks = { __proto__: null }, owner = { __proto__: null },
      features = [
        [0, "CanvasAttributeChanged", "CanvasConnected", "CanvasDimension",
          "CanvasReadbackState", "FlushCanvasSurfaces", "SetCanvasDimension"],
        [1, "AbortIndexedDBForWorker"],
        [2, "BeginMotionObservation", "MotionRecheck", "MotionTreeChanged",
          "TransitionsDirty", "WatchTransitions"],
        [5, "CreateWebGLContext", "FlushWebGLSurfaces", "WebGLConnected",
          "WebGLMethods", "WebGLReadback", "WebGLResize"],
        [7, "CloneWasmModule"],
        [19, "WebGLInterfaces"],
      ];
    for (let group = 0; group < features.length; group++)
      for (let at = 1; at < features[group].length; at++) {
        const name = "__tilefinch" + features[group][at];
        if (getDescriptor(globalThis, name)) continue;
        owner[name] = features[group][0];
        defineProperty(globalThis, name, {
          __proto__: null, configurable: false, enumerable: false,
          get: () => hooks[name], set() {},
        });
      }
    captured.claimHooks = () => {
      const feature = lazyLoading();
      return feature < 0 ? null : (name, value) => {
        if (owner[name] === feature && hooks[name] === undefined
            && lazyLoading(feature)) hooks[name] = value;
      };
    };
  }
  defineProperty(globalThis, "__tilefinchEventInterfaces", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: freeze(captured),
  });
  for (const name of [
    "Blob", "DOMImplementation", "MutationObserver", "Navigator", "NavigatorUAData",
    "UserActivation",
    "PluginArray", "MimeTypeArray", "Plugin", "MimeType", "Clipboard",
    "Performance", "PerformanceObserver", "VisibilityStateEntry",
    "Screen", "ScreenOrientation",
    "TrustedHTML", "TrustedScript", "TrustedScriptURL",
    "TrustedTypePolicy", "TrustedTypePolicyFactory",
  ])
    markInterface(globalThis[name]);
  for (const [owner, names] of [
    [globalThis.URL, ["createObjectURL", "revokeObjectURL"]],
    [globalThis.crypto, ["getRandomValues", "randomUUID"]],
    [globalThis.history, ["pushState", "replaceState"]],
    [globalThis.performance, [
      "clearMarks", "clearMeasures", "getEntries", "getEntriesByName",
      "getEntriesByType", "mark", "measure", "now",
    ]],
  ])
    for (const name of names) markNative(owner?.[name]);
  /* The temporary trusted channel applies the same descriptor flags without
     allocating a page-visible array and descriptor objects for every hook. */
  globalThis.__tilefinchHostChannel.hardenGlobalHooks();
  /* WebIDL puts Document members on Document.prototype: in browsers the
     document's only own property is the [LegacyUnforgeable] location, and
     document.createElement === Document.prototype.createElement. The
     bootstrap modules build the main document's members as own properties
     beside the native bridge; move them here, once, after the last module,
     named as WebIDL names them ("createElement", "get body"). Lookups then
     cost one more prototype hop (unmeasurable next to the bridge call).
     Secondary documents (DOMParser, createHTMLDocument, frames) are separate
     objects whose own members predate this move; the prototype returned by
     __tilefinchSecondaryDocumentPrototype keeps every moved name resolving
     for them exactly as it did before, instead of to the main document's
     closures. */
  {
    const mainDocument = globalThis.document,
      DocumentPrototype = globalThis.Document?.prototype,
      getPrototypeOf = Object.getPrototypeOf,
      createObject = Object.create,
      moved = [],
      inheritedDescriptor = (key) => {
        for (let at = DocumentPrototype; at; at = getPrototypeOf(at)) {
          const descriptor = getDescriptor(at, key);
          if (descriptor) return descriptor;
        }
        return undefined;
      },
      rename = (value, name) => {
        if (typeof value === "function" && value.name !== name)
          defineProperty(value, "name", { value: name, configurable: true });
      };
    if (mainDocument && DocumentPrototype)
      for (const key of nativeOwnKeys(mainDocument)) {
        if (typeof key !== "string" || key === "location") continue;
        const own = getDescriptor(mainDocument, key);
        if (!own || !own.configurable) continue;
        const previous =
          key in DocumentPrototype ? inheritedDescriptor(key) : undefined;
        if (
          previous &&
          ("value" in own
            ? own.value === previous.value
            : own.get === previous.get && own.set === previous.set)
        ) {
          /* Already the prototype's own function (getElementById). */
          deleteProperty(mainDocument, key);
          continue;
        }
        let descriptor;
        if (!("value" in own)) {
          rename(own.get, "get " + key);
          rename(own.set, "set " + key);
          descriptor = {
            get: own.get,
            set: own.set,
            enumerable: true,
            configurable: true,
          };
        } else if (typeof own.value === "function") {
          rename(own.value, key);
          descriptor = {
            value: own.value,
            writable: true,
            enumerable: true,
            configurable: true,
          };
        } else {
          /* Constant attributes (nodeType, compatMode...) become getters. */
          const value = own.value;
          descriptor = getDescriptor({ get [key]() { return value; } }, key);
        }
        defineProperty(DocumentPrototype, key, descriptor);
        deleteProperty(mainDocument, key);
        moved.push(key, previous);
      }
    const secondaryPrototypes = new Map();
    defineProperty(globalThis, "__tilefinchSecondaryDocumentPrototype", {
      configurable: false,
      enumerable: false,
      writable: false,
      value: (base) => {
        let prototype = secondaryPrototypes.get(base);
        if (!prototype) {
          prototype = createObject(base);
          for (let at = 0; at < moved.length; at += 2)
            defineProperty(
              prototype,
              moved[at],
              moved[at + 1] || {
                value: undefined,
                writable: true,
                configurable: true,
              },
            );
          secondaryPrototypes.set(base, prototype);
        }
        return prototype;
      },
    });
  }
  /* Native bridge entry points are implementation slots, not Web globals.
     They are deliberately non-enumerable, but reflection APIs expose
     non-enumerable names too. Keep those names out of author-visible global
     inventories while retaining the actual slots for later lazy bootstrap
     groups and native callbacks. */
  const authorGlobalKeys = (keys) => {
      const visible = [];
      for (const key of keys)
        if (typeof key !== "string" || !key.startsWith("__tilefinch"))
          visible.push(key);
      return visible;
    },
    reflectedOwnKeys = function ownKeys(target) {
      const keys = nativeOwnKeys(target);
      return target === globalThis ? authorGlobalKeys(keys) : keys;
    },
    reflectedOwnPropertyNames = function getOwnPropertyNames(target) {
      const keys = ownPropertyNames(target);
      return target === globalThis ? authorGlobalKeys(keys) : keys;
    },
    reflectedOwnPropertyDescriptors = function getOwnPropertyDescriptors(
      target,
    ) {
      const descriptors = ownPropertyDescriptors(target);
      if (target === globalThis)
        for (const key of nativeOwnKeys(descriptors))
          if (typeof key === "string" && key.startsWith("__tilefinch"))
            deleteProperty(descriptors, key);
      return descriptors;
    };
  for (const value of [
    reflectedOwnKeys,
    reflectedOwnPropertyNames,
    reflectedOwnPropertyDescriptors,
  ]) markNative(value);
  defineProperty(Reflect, "ownKeys", {
    configurable: true,
    enumerable: false,
    writable: true,
    value: reflectedOwnKeys,
  });
  defineProperty(Object, "getOwnPropertyNames", {
    configurable: true,
    enumerable: false,
    writable: true,
    value: reflectedOwnPropertyNames,
  });
  defineProperty(Object, "getOwnPropertyDescriptors", {
    configurable: true,
    enumerable: false,
    writable: true,
    value: reflectedOwnPropertyDescriptors,
  });
})();
