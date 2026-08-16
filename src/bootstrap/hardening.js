(() => {
  const ownKeys = Reflect.ownKeys,
    getDescriptor = Object.getOwnPropertyDescriptor,
    defineProperty = Object.defineProperty,
    protectedFunctions = new Set([
      "__tilefinchBoundedAncestorPath",
      "__tilefinchCurrentScriptForStable",
      "__tilefinchDiagnosticLookup",
      "__tilefinchDispatchActivationHandle",
      "__tilefinchDispatchAt",
      "__tilefinchDispatchDOMContentLoaded",
      "__tilefinchDispatchHandle",
      "__tilefinchDispatchInputHandle",
      "__tilefinchDispatchSubmitHandle",
      "__tilefinchFocusEventsObserved",
      "__tilefinchFocusObserverDelta",
      "__tilefinchRegisterNativeNodeStateCleanup",
      "__tilefinchRetireNativeNodeState",
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
      "__tilefinchDeliverNetwork",
      "__tilefinchIntersectionRecheck",
      "__tilefinchMediaRecheck",
      "__tilefinchMaybeStartMotion",
      "__tilefinchMotionRecheck",
      "__tilefinchBeginMotionObservation",
      "__tilefinchParserMutationCheckpoint",
      "__tilefinchPendingNetworkRequests",
      "__tilefinchPendingTimers",
      "__tilefinchPumpTimers",
      "__tilefinchRebindDocument",
      "__tilefinchRecordResourceTiming",
      "__tilefinchRefreshNamedProperties",
      "__tilefinchResizeRecheck",
      "__tilefinchRestoreSameDocument",
      "__tilefinchRestoreSectionState",
      "__tilefinchSaveSectionState",
      "__tilefinchSetFrameWindowState",
      "__tilefinchStylesheetHasMotionKeyframes",
    ]);
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
