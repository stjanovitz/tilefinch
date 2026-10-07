(() => {
  const nativeOwnKeys = Reflect.ownKeys,
    ownPropertyNames = Object.getOwnPropertyNames,
    ownPropertyDescriptors = Object.getOwnPropertyDescriptors,
    deleteProperty = Reflect.deleteProperty,
    getDescriptor = Object.getOwnPropertyDescriptor,
    defineProperty = Object.defineProperty,
    freeze = Object.freeze,
    protectedFunctions = new Set([
      "__tilefinchBoundedAncestorPath",
      "__tilefinchBlobBytes",
      "__tilefinchBlobForURL",
      "__tilefinchCloneWorkerValue",
      "__tilefinchAbortFileReaderForWorker",
      "__tilefinchAbortIndexedDBForWorker",
      "__tilefinchAbortSignalBrand",
      "__tilefinchBlobTextForWorker",
      "__tilefinchAbortXHRForWorker",
      "__tilefinchCloseWebSocketForWorker",
      "__tilefinchDetachArrayBuffer",
      "__tilefinchPrepareMessagePortTransfer",
      "__tilefinchCreatePrivateWeakMap",
      "__tilefinchCreateMessageEvent",
      "__tilefinchCreateWorkerCrypto",
      "__tilefinchSetCryptoBufferNormalizer",
      "__tilefinchCreateWorkerPerformance",
      "__tilefinchWorkerCloneIntrinsics",
      "__tilefinchCurrentScriptForStable",
      "__tilefinchCurrentDocumentURL",
      "__tilefinchSecureContext",
      "__tilefinchDocumentDomainValid",
      "__tilefinchDiagnosticLookup",
      "__tilefinchDispatchActivationHandle",
      "__tilefinchDispatchAt",
      "__tilefinchDispatchDOMContentLoaded",
      "__tilefinchDispatchHandle",
      "__tilefinchDispatchInputHandle",
      "__tilefinchDispatchSubmitHandle",
      "__tilefinchDispatchWindowEventCheckpointed",
      "__tilefinchDispatchEventTargetCheckpointed",
      "__tilefinchEventObserverDelta",
      "__tilefinchFetchForWorker",
      "__tilefinchFetchWorkerScript",
      "__tilefinchFetchWorkerScriptSync",
      "__tilefinchFocusEventsObserved",
      "__tilefinchInvokeEventTargetCheckpointed",
      "__tilefinchIsProxy",
      "__tilefinchGetTextPrefix",
      "__tilefinchGetStyleAttributePrefix",
      "__tilefinchMarkNativeFunction",
      "__tilefinchNormalizeTimerDelay",
      "__tilefinchScheduleTimeout",
      "__tilefinchScheduleInterval",
      "__tilefinchScheduleTask",
      "__tilefinchCancelTimer",
      "__tilefinchSchedulerTime",
      "__tilefinchScheduleReservedTask",
      "__tilefinchScheduleReservedFrame",
      "__tilefinchPointerHoverEventsObserved",
      "__tilefinchPointerMarkupChanged",
      "__tilefinchPointerMoveEventsObserved",
      "__tilefinchRecordEventHandler",
      "__tilefinchCompressionRun",
      "__tilefinchWasmCompile",
      "__tilefinchWasmGlobalGet",
      "__tilefinchWasmGlobalSet",
      "__tilefinchWasmValidate",
      "__tilefinchRegisterNativeNodeStateCleanup",
      "__tilefinchReportUncaught",
      "__tilefinchRetireNativeNodeState",
      "__tilefinchRunTask",
      "__tilefinchSchedulerNow",
      "__tilefinchSetFocusHandle",
      "__tilefinchWrap",
      "__tilefinchWrapRemote",
      "__tilefinchWrapRemoteRelation",
      "__tilefinchWrapRemoteSelector",
      "__tilefinchWrapRemoteStable",
      /* Entry points invoked through the global object by name: by native
         code (runtime_call in src/js_runtime/event_loop.inc and friends) or
         by later bootstrap modules (the motion, resize and trusted-event
         hooks). Left writable, page script could replace one and take over
         that call.  __tilefinchReceiveMessage is deliberately absent:
         tests/suites/web_runtime_forms.inc overrides it to exercise a
         failing realm. */
      "__tilefinchCommitSameDocument",
      "__tilefinchDeliverEventSource",
      "__tilefinchDeliverWebSocket",
      "__tilefinchDeliverMultiplayer",
      "__tilefinchDeliverNetwork",
      "__tilefinchDocumentURLRevision",
      "__tilefinchDetachNetwork",
      "__tilefinchIntersectionRecheck",
      "__tilefinchMediaRecheck",
      "__tilefinchMaybeStartMotion",
      "__tilefinchMotionRecheck",
      "__tilefinchBeginMotionObservation",
      "__tilefinchParserMutationCheckpoint",
      "__tilefinchPumpTimers",
      "__tilefinchSchedulerSnapshot",
      "__tilefinchRebindDocument",
      "__tilefinchRecordNavigationTiming",
      "__tilefinchRecordResourceTiming",
      "__tilefinchRefreshNamedProperties",
      "__tilefinchResizeRecheck",
      "__tilefinchRestoreSameDocument",
      "__tilefinchRestoreSectionState",
      "__tilefinchSaveSectionState",
      "__tilefinchSetFrameWindowState",
      "__tilefinchStylesheetHasMotionKeyframes",
      "__tilefinchTrustedEvent",
      "__tilefinchTrustedString",
      "__tilefinchUpdateGamepad",
    ]);
  /* Set by the host for host and validation realms only; the probes it
     enabled are published by now. */
  deleteProperty(globalThis, "__tilefinchTestProbes");
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
  for (const key of nativeOwnKeys(globalThis)) {
    if (typeof key !== "string" || !key.startsWith("__tilefinch")) continue;
    const descriptor = getDescriptor(globalThis, key);
    if (!descriptor) continue;
    const protectedFunction =
      protectedFunctions.has(key) && typeof descriptor.value === "function";
    if (!descriptor.enumerable && !protectedFunction) continue;
    try {
      defineProperty(globalThis, key, {
        ...descriptor,
        enumerable: false,
        configurable: protectedFunction ? false : descriptor.configurable,
        writable:
          protectedFunction && "writable" in descriptor
            ? false
            : descriptor.writable,
      });
    } catch {
      /* A host-defined non-configurable property is already as hard as this
         page-realm pass can make it. */
    }
  }
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
