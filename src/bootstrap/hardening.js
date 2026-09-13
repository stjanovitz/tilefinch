(() => {
  const ownKeys = Reflect.ownKeys,
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
      "__tilefinchDiagnosticLookup",
      "__tilefinchDispatchActivationHandle",
      "__tilefinchDispatchAt",
      "__tilefinchDispatchDOMContentLoaded",
      "__tilefinchDispatchHandle",
      "__tilefinchDispatchInputHandle",
      "__tilefinchDispatchSubmitHandle",
      "__tilefinchDispatchWindowEventCheckpointed",
      "__tilefinchEventObserverDelta",
      "__tilefinchFetchForWorker",
      "__tilefinchFetchWorkerScript",
      "__tilefinchFetchWorkerScriptSync",
      "__tilefinchFocusEventsObserved",
      "__tilefinchFocusObserverDelta",
      "__tilefinchInvokeEventTargetCheckpointed",
      "__tilefinchIsProxy",
      "__tilefinchGameAudioCommand",
      "__tilefinchGameAudioDecode",
      "__tilefinchGetTextPrefix",
      "__tilefinchGetStyleAttributePrefix",
      "__tilefinchMarkNativeFunction",
      "__tilefinchNormalizeTimerDelay",
      "__tilefinchScheduleTimeout",
      "__tilefinchScheduleInterval",
      "__tilefinchCancelTimer",
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
      /* Host entry points the event loop invokes by name on every tick (see
         runtime_call in src/js_runtime/event_loop.inc and friends).  They
         were left writable, so page script could replace any of them and
         take over the tick.  __tilefinchReceiveMessage is deliberately absent:
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
      "__tilefinchPendingNetworkRequests",
      "__tilefinchPendingTimers",
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
      "__tilefinchTrustedString",
      "__tilefinchUpdateGamepad",
    ]);
  const markNative = globalThis.__tilefinchMarkNativeFunction,
    markInterface = (constructor) => {
      if (typeof constructor !== "function") return;
      markNative(constructor);
      const prototype = constructor.prototype;
      if (!prototype) return;
      for (const key of ownKeys(prototype)) {
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
  for (const key of ownKeys(globalThis)) {
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
})();
