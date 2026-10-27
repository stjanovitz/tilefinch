/* A private global lexical constant avoids adding a captured environment slot
   to every DOM/platform method. The public globalThis property stays writable;
   this binding is neither a property nor a privileged host capability. */
const __tilefinchRealmGlobal = this;
(() => {
  const scriptForceAsync = __tilefinchRealmGlobal.__tilefinchScriptForceAsync,
    scriptAsyncAssigned = __tilefinchRealmGlobal.__tilefinchScriptAsyncAssigned,
    elementNonce = __tilefinchRealmGlobal.__tilefinchElementNonce,
    prepareDynamicSubtree = __tilefinchRealmGlobal.__tilefinchPrepareDynamicSubtree,
    appendMany = __tilefinchRealmGlobal.__tilefinchAppendMany,
    /* Host and validation realms only (see hardening.js). */
    testProbes = !!__tilefinchRealmGlobal.__tilefinchTestProbes,
    ancestorApply = Reflect.apply,
    numberFrom = Number,
    numberIsFinite = Number.isFinite,
    mathFloor = Math.floor,
    mathMin = Math.min,
    emptyTaskArguments = Object.freeze([]),
    /* DOM mutation/event paths are iterative and may safely exceed the
       renderer's 64-level recursive fast path. Keep a separate finite cap
       so authored deep trees remain usable without making cycles unbounded. */
    ancestorLimit = 256,
    cloneDepthLimit = 64,
    /* A cycle cannot end, so it always reaches the cap: the bound alone
       detects it without a Set and two calls per step on every dispatch. */
    boundedAncestorPath = (start, parentOf) => {
      const path = [];
      for (let at = start; at; at = parentOf(at)) {
        if (path.length >= ancestorLimit)
          throw new DOMException(
            "Cyclic or excessively deep node ancestry",
            "HierarchyRequestError",
          );
        path.push(at);
      }
      return path;
    };
  /* Resolve a computed CSS length that may still be written as math
     (calc/min/max/clamp, env(), rem, %) to pixels, or NaN. Some properties
     reach script unresolved; chatgpt.com's scroll-margin-top is
     max(0px,calc(calc(max(1rem,env(safe-area-inset-top))+3.625rem)+...)).
     Bounded: 256 tokens, 16 levels of nesting. */
  const cssLengthPixels = (text, fontSize = 16, reference = 0) => {
    const source = String(text ?? "").trim();
    if (source === "" || source.length > 1024) return NaN;
    const tokens = [];
    const pattern =
      /\s*(?:([+-]?(?:\d*\.\d+|\d+)(?:e[+-]?\d+)?)([a-z%]*)|([a-z-]+)\(|([-a-z]+)|([()*/,+-]))/giy;
    let match;
    while (tokens.length <= 256 && pattern.lastIndex < source.length
           && (match = pattern.exec(source)) !== null) {
      if (match[1] !== undefined) {
        const previous = tokens[tokens.length - 1];
        /* A sign binds to the number only where an operand may start. */
        if (/^[+-]/.test(match[1]) && previous
            && (previous.kind === "number" || previous.value === ")")) {
          tokens.push({ kind: "op", value: match[1][0] });
          tokens.push({ kind: "number", value: parseFloat(match[1].slice(1)),
                        unit: match[2].toLowerCase() });
        } else
          tokens.push({ kind: "number", value: parseFloat(match[1]),
                        unit: match[2].toLowerCase() });
      } else if (match[3] !== undefined)
        tokens.push({ kind: "function", value: match[3].toLowerCase() });
      else if (match[4] !== undefined)
        tokens.push({ kind: "ident", value: match[4].toLowerCase() });
      else tokens.push({ kind: "op", value: match[5] });
    }
    if (tokens.length > 256 || pattern.lastIndex < source.trimEnd().length)
      return NaN;
    const rootSize = 16;
    const unitPixels = (value, unit) => {
      switch (unit) {
        case "": case "px": return value;
        case "rem": return value * rootSize;
        case "em": return value * fontSize;
        case "%": return (value * reference) / 100;
        case "vw": return (value * innerWidth) / 100;
        case "vh": return (value * innerHeight) / 100;
        case "vmin": return (value * mathMin(innerWidth, innerHeight)) / 100;
        case "vmax": return (value * Math.max(innerWidth, innerHeight)) / 100;
        case "pt": return (value * 4) / 3;
        default: return NaN;
      }
    };
    let at = 0;
    const peek = () => tokens[at];
    const take = (value) =>
      tokens[at] && tokens[at].value === value ? (at++, true) : false;
    const expression = (depth) => {
      if (depth > 16) return NaN;
      let value = product(depth);
      for (;;) {
        if (take("+")) value += product(depth);
        else if (take("-")) value -= product(depth);
        else return value;
      }
    };
    const product = (depth) => {
      let value = operand(depth);
      for (;;) {
        if (take("*")) value *= operand(depth);
        else if (take("/")) value /= operand(depth);
        else return value;
      }
    };
    const list = (depth) => {
      const values = [expression(depth + 1)];
      while (take(",")) values.push(expression(depth + 1));
      return take(")") ? values : [NaN];
    };
    const operand = (depth) => {
      const token = peek();
      if (!token) return NaN;
      at++;
      if (token.kind === "number") return unitPixels(token.value, token.unit);
      if (token.value === "(") {
        const value = expression(depth + 1);
        return take(")") ? value : NaN;
      }
      if (token.kind !== "function") return NaN;
      if (token.value === "env") {
        const name = peek()?.kind === "ident" ? tokens[at++].value : "";
        let fallback = NaN;
        if (take(",")) fallback = expression(depth + 1);
        if (!take(")")) return NaN;
        return name.startsWith("safe-area-inset-") ? 0
          : (numberIsFinite(fallback) ? fallback : 0);
      }
      const values = list(depth);
      switch (token.value) {
        case "calc": return values.length === 1 ? values[0] : NaN;
        case "min": return mathMin(...values);
        case "max": return Math.max(...values);
        case "clamp":
          return values.length === 3
            ? Math.max(values[0], mathMin(values[1], values[2])) : NaN;
        default: return NaN;
      }
    };
    const result = expression(0);
    return at === tokens.length ? result : NaN;
  };
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchBoundedAncestorPath", {
    enumerable: false,
    configurable: false,
    writable: false,
    value: boundedAncestorPath,
  });
  /* Walks that only need the bound, not a materialised path, carry an inline
     step counter against this same cap.  __tilefinchDetachedParent is writable,
     so `a.__tilefinchDetachedParent = b; b.__tilefinchDetachedParent = a;` turns
     any unbounded ancestor walk into a hung tick.  The counter alone catches
     that -- a cycle cannot terminate, so it always reaches the cap -- and
     unlike boundedAncestorPath it allocates nothing, which matters on paths
     as hot as contains(), closest(), and every mutation record. */
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchAncestorLimit", {
    enumerable: false,
    configurable: false,
    writable: false,
    value: ancestorLimit,
  });
  /* hardening.js takes a single snapshot of Reflect.ownKeys(__tilefinchRealmGlobal), so
     a __tilefinch global created lazily on first write stays enumerable for the
     rest of the page's life.  Declaring them up front makes
     non-enumerability an invariant of the global instead of a property of
     bootstrap ordering.  They stay writable -- both bootstrap and the host
     reset them by plain assignment -- and non-configurable so a later write
     cannot restore the enumerable flag. */
  for (const [name, initial] of [
    ["__tilefinchSubmittedFormHandle", 0],
    ["__tilefinchSubmittedSubmitterHandle", 0],
    ["__tilefinchFragmentInsertCount", 0],
    ["__tilefinchFragmentInsertText", ""],
    ["__tilefinchSelectedControl", null],
  ])
    Object.defineProperty(__tilefinchRealmGlobal, name, {
      enumerable: false,
      configurable: false,
      writable: true,
      value: initial,
    });
  let dynamicPreparationSuppressed = 0,
    cloneCallDepth = 0,
    templateContentsOwnerDocument = null,
    nodeOwnersActive = false;
  /* A native node's adopted owner document is a tag the bridge keeps per
     handle (__tilefinchNodeOwner), so it outlives the node's wrappers:
     1 is this document, 2 the template contents owner, 3-255 the
     script-side documents listed here. Nodes no adoption tagged (template
     contents, nodes parsed into an adopted tree) inherit it natively.
     __tilefinchAdoptedOwner on a wrapper caches it (page script reads
     ownerDocument thousands of times per interaction), and is the only
     record for script-side nodes. Until the first adoption away from this
     document or template contents read, every native node is this
     document's and the bridge keeps no tags. */
  const nodeOwnerDocuments = [null, null, null],
    nodeOwnerTags = new Map(),
    templateOwnerDocument = () =>
      templateContentsOwnerDocument ||
      (templateContentsOwnerDocument =
        __tilefinchRealmGlobal.__tilefinchNewDocument?.() || null),
    nodeOwnerTag = (owner) => {
      if (!owner || owner === document) return 1;
      if (owner === templateContentsOwnerDocument) return 2;
      let tag = nodeOwnerTags.get(owner) || nodeOwnerDocuments.indexOf(null, 3);
      if (nodeOwnerDocuments[tag] === owner) return tag;
      if (tag < 0 && nodeOwnerDocuments.length >= 19) {
        // Reuse the tags no node holds any more before growing.
        const held = new Set(__tilefinchNodeOwner(0));
        for (let at = 3; at < nodeOwnerDocuments.length; at++)
          if (!held.has(at)) {
            nodeOwnerTags.delete(nodeOwnerDocuments[at]);
            nodeOwnerDocuments[at] = null;
          }
        tag = nodeOwnerDocuments.indexOf(null, 3);
      }
      if (tag < 0 && nodeOwnerDocuments.length < 256)
        tag = nodeOwnerDocuments.length;
      if (tag < 0) return 0;
      nodeOwnerDocuments[tag] = owner;
      nodeOwnerTags.set(owner, tag);
      return tag;
    },
    cacheNodeOwner = (node, owner) =>
      Reflect.defineProperty(node, "__tilefinchAdoptedOwner", {
        configurable: true,
        writable: true,
        value: owner,
      }),
    setNodeOwner = (node, owner) => {
      if (node.__handle > 0) {
        // Tag 0 (253 live documents) leaves only the wrapper's record.
        const tag = nodeOwnerTag(owner);
        if (tag > 1 || (tag === 1 && nodeOwnersActive))
          nodeOwnersActive =
            __tilefinchNodeOwner(node.__handle, tag) || nodeOwnersActive;
      }
      cacheNodeOwner(node, owner);
    };
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchSetNodeOwner", {
    enumerable: false,
    configurable: false,
    writable: false,
    value: setNodeOwner,
  });
  /* Nodes a native receiver builds come from the native document: a
     script-side owner's factories would make script-side nodes. Inserting
     them then adopts them into the receiver's owner. */
  const factoryDocument = (node) =>
    node.__handle !== undefined ? document : node.ownerDocument || document;
  const documentListeners = new Map(),
    smoothElementScrolls = new WeakMap();
  let pendingElementScrolls = null;
  const queueElementScroll = (node) => {
    if (pendingElementScrolls) {
      if (pendingElementScrolls.size < 128) pendingElementScrolls.add(node);
      return;
    }
    pendingElementScrolls = new Set([node]);
    setTimeout(() => {
      const pending = pendingElementScrolls;
      pendingElementScrolls = null;
      for (const target of pending) target.dispatchEvent(new Event("scroll"));
    }, 0);
  };
  __tilefinchRealmGlobal.__tilefinchTaskRealm = "top";
  let activeTaskKind = "bootstrap",
    activeTaskSequence = 0,
    nextTaskSequence = 1;
  Object.defineProperties(__tilefinchRealmGlobal, {
    __tilefinchActiveTaskKind: {
      configurable: false,
      enumerable: false,
      get: () => activeTaskKind,
    },
    __tilefinchActiveTaskSequence: {
      configurable: false,
      enumerable: false,
      get: () => activeTaskSequence,
    },
  });
  __tilefinchRealmGlobal.__tilefinchRunTask = (kind, callback, thisArg, args) => {
    const previousKind = activeTaskKind,
      previousSequence = activeTaskSequence;
    activeTaskKind = typeof kind === "string" && kind.length <= 96
      ? kind : String(kind).slice(0, 96);
    activeTaskSequence = nextTaskSequence++;
    try {
      __tilefinchCallbackCheckpoint(activeTaskKind);
      /* Listener functions are author objects: their own `call` and `apply`
         properties are observable and may be poisoned. Web callbacks invoke
         the function itself, so use the captured intrinsic directly. */
      return ancestorApply(
        callback,
        thisArg,
        args === undefined ? emptyTaskArguments : args,
      );
    } finally {
      activeTaskKind = previousKind;
      activeTaskSequence = previousSequence;
    }
  };
  const uncaughtErrors = [];
  let uncaughtReportDepth = 0;
  let uncaughtReportCount = 0;
  __tilefinchRealmGlobal.__tilefinchUncaughtErrors = uncaughtErrors;
  __tilefinchRealmGlobal.__tilefinchUncaughtErrorCount = 0;
  if (testProbes) __tilefinchRealmGlobal.__tilefinchLastUncaughtTask = "";
  __tilefinchRealmGlobal.__tilefinchReportUncaught = (error, context = "callback") => {
    uncaughtReportDepth++;
    try {
      /* Count before any string/stack allocation: an OOM must not disappear
         merely because formatting its diagnostic also fails. Existing own
         property + int32 value keep this publication allocation-free. */
      if (uncaughtReportCount < 2147483647) uncaughtReportCount++;
      __tilefinchRealmGlobal.__tilefinchUncaughtErrorCount = uncaughtReportCount;
      const message = String(error),
        stack = String((error && error.stack) || ""),
        detail =
          stack && stack.indexOf(message) < 0
            ? message + "\n" + stack
            : stack || message;
      if (testProbes)
        __tilefinchRealmGlobal.__tilefinchLastUncaughtTask =
          "realm=" +
          String(__tilefinchRealmGlobal.__tilefinchTaskRealm || "unknown") +
          " task=" +
          String(activeTaskKind) +
          "#" +
          String(activeTaskSequence) +
          " context=" +
          String(context).slice(0, 64);
      if (uncaughtErrors.length >= 16) uncaughtErrors.shift();
      uncaughtErrors.push(String(context) + ": " + detail);
      /*
       * Exceptions reported by platform callbacks are observable through the
       * Window error channel.  Custom-element construction in particular must
       * report, rather than throw, a failed synchronous upgrade.  Keep this
       * before console logging so test and application error handlers see the
       * original value even when diagnostic formatting later fails.
       */
      const onerror = __tilefinchRealmGlobal.onerror;
      /* HTML's exception-reporting algorithm must not recursively report an
         exception raised while it is already firing the Window error event.
         Without this guard, one throwing `error` listener recursively creates
         error events until Tilefinch's callback budget retires the realm. */
      if (uncaughtReportDepth === 1 && typeof onerror === "function")
        try {
          onerror.call(
            __tilefinchRealmGlobal,
            error?.message || message,
            "",
            0,
            0,
            error,
          );
        } catch (_) {}
      if (
        uncaughtReportDepth === 1 &&
        typeof __tilefinchRealmGlobal.Event === "function" &&
        typeof __tilefinchRealmGlobal.dispatchEvent === "function"
      )
        try {
          const event = new Event("error", { cancelable: true });
          event.message = error?.message || message;
          event.filename = "";
          event.lineno = 0;
          event.colno = 0;
          event.error = error;
          /*
           * dispatchEvent invokes the onerror event-handler attribute with an
           * Event argument, while the legacy Window.onerror callback above
           * requires five arguments. Avoid invoking that same property twice;
           * registered "error" listeners still receive the ErrorEvent-shaped
           * value.
           */
          __tilefinchRealmGlobal.onerror = null;
          try {
            __tilefinchRealmGlobal.dispatchEvent(event);
          } finally {
            __tilefinchRealmGlobal.onerror = onerror;
          }
        } catch (_) {}
      if (__tilefinchRealmGlobal.console && typeof console.error === "function")
        console.error("Uncaught " + String(context), detail, provenance);
    } catch (_) {
      /* Reporting must never turn an isolated callback exception into a failed host dispatch, including when the JS heap is exhausted. */
    } finally {
      uncaughtReportDepth--;
    }
  };
  const retentionStats = {
    listenerDrops: 0,
    handlerDrops: 0,
    observerDrops: 0,
    recordDrops: 0,
    parserRecordsCoalesced: 0,
    recordsCoalesced: 0,
    /* Wrapper churn: a wrapper built for a node whose previous wrapper the
       collector already finalized is rebuilt work. */
    wrapperCreates: 0,
    wrappersFinalized: 0,
    /* MutationObserver routing work for the native tilefinch-work record:
       records queued, and observers examined per routed mutation. */
    mutationRecords: 0,
    observerVisits: 0,
  };
  __tilefinchRealmGlobal.__tilefinchRetentionStats = retentionStats;
  /* The lazily loaded traversal module installs the live NodeIterator
     removal hook once; removals must then be enumerated even when no
     MutationObserver is registered. */
  let nodeIteratorHooks = null;
  __tilefinchRealmGlobal.__tilefinchSetNodeIteratorHooks = (hooks) => {
    nodeIteratorHooks = hooks;
    delete __tilefinchRealmGlobal.__tilefinchSetNodeIteratorHooks;
  };
  const mutationChildSnapshot = (node) => {
    if (mutationObservers.length === 0 && !nodeIteratorHooks?.active())
      return [];
    const values = [];
    let child = node?.firstChild || null;
    while (child && values.length < 256) {
      values.push(child);
      child = child.nextSibling;
    }
    if (child) retentionStats.recordDrops++;
    return values;
  };
  const nativeNodeStateCleanups = [];
  __tilefinchRealmGlobal.__tilefinchRegisterNativeNodeStateCleanup = (callback) => {
    if (
      typeof callback === "function" &&
      nativeNodeStateCleanups.length < 4
    )
      nativeNodeStateCleanups.push(callback);
  };
  __tilefinchRealmGlobal.__tilefinchRetireNativeNodeState = (handle, wrapperOnly) => {
    if (wrapperOnly && typeof handle === "object" && handle !== null) {
      /* A native sweep's released wrappers, batched into one call. */
      for (let i = 0; i < handle.length; i++)
        if (nodeCache.delete(Number(handle[i]))) retentionStats.wrappersFinalized++;
      return;
    }
    handle = Number(handle);
    if (nodeCache.delete(handle) && wrapperOnly) retentionStats.wrappersFinalized++;
    if (wrapperOnly) return;
    for (const cleanup of nativeNodeStateCleanups) cleanup(handle);
  };
  const nodeCache = new Map(),
    weakNodeCache = typeof WeakRef === "function";
  const nodeReferenceDeref = weakNodeCache ? WeakRef.prototype.deref : null,
    /* Reflect.apply allocates its argument list; a bound call does not,
       so the event-state hooks can look a wrapper or holder up after they
       began moving state (see __tilefinchHoldEventState). */
    derefReference = weakNodeCache
      ? Function.prototype.call.bind(nodeReferenceDeref)
      : null;
  const cachedNode = (handle) => {
    const entry = nodeCache.get(Number(handle));
    if (!entry) return null;
    if (!weakNodeCache) return entry.wrapper || null;
    const wrapper = derefReference(entry);
    if (wrapper) return wrapper;
    nodeCache.delete(Number(handle));
    return null;
  };
  const cacheNode = (handle, wrapper) => {
    handle = Number(handle);
    /* The same WeakRef is the cache entry's and, for native getters, the
       handle slot's. */
    const reference = weakNodeCache ? new WeakRef(wrapper) : null,
      lease = weakNodeCache
        ? Number(__tilefinchRetainNodeWrapper(handle, reference)) : 0;
    if (!weakNodeCache) {
      nodeCache.set(handle, { wrapper, lease: 0 });
      wrapper.__tilefinchHandleLease = 0;
      return wrapper;
    }
    if (!lease) {
      nodeCache.delete(handle);
      wrapper.__tilefinchHandleLease = 0;
      return wrapper;
    }
    /* The native handle table reclaims cleared WeakRefs in bounded owner
       turns. No per-wrapper registry cell or queued finalizer is needed. */
    const entry = reference;
    entry.handle = handle;
    entry.lease = lease;
    nodeCache.set(handle, entry);
    wrapper.__tilefinchHandleLease = lease;
    return wrapper;
  };
  const inlineHandlerRecordToken = {},
    hasOwnProperty = Object.prototype.hasOwnProperty,
    wrapperListeners = Symbol("wrapper listeners"),
    /* Marks the wrapper of a native document node: parentNode answers the
       document object for it, never that wrapper. */
    nativeDocumentWrapper = Symbol("native document wrapper");
  /* The event-state store. A node's event state (its listener and handler
     maps) is rooted in two per-handle maps (nativeNodeListeners,
     nativeNodeHandlers), so a connected or pinned node keeps it when
     its wrapper is collected, and a later wrapper finds it again. Rooting a
     detached tree's state would also keep alive every wrapper a handler
     closes over (element -> handler -> element through a global root). The
     native owner-turn sweep therefore moves a detached, unpinned tree's
     state into a holder that only the tree's live wrappers reference
     (__tilefinchHoldEventState); a wrapper made later in that tree joins it.
     The collector then reclaims the tree with its handlers once no wrapper
     of it is reachable. Native code roots the state again
     (__tilefinchReleaseEventHolders) before anything lets the tree outlive
     those wrappers: inserting it or into it, or pinning it. An insertion
     into a held tree instead has the moving subtree's wrappers join the
     tree's holder when they can.

     The per-handle maps hold 128 targets each to start with and grow by
     doubling while the page Budget confirms the memory free
     (__tilefinchGrowEventTargets). Past a refused growth a node's state
     lives on its wrapper instead (wrapperListeners, wrapperHandlers), and
     native code keeps that wrapper alive while the node is connected or
     pinned (state 3 below), so the state still lives as long as the node
     can receive events; a detached tree's goes to its holder as above.

     All of it lives in the one closure below, which reaches the rest of
     this module only through its arguments and is reached only through
     what it returns (wrap(), rebind, the event handler and listener
     paths, and the test census). Native code drives it through
     __tilefinchNoteEventState, __tilefinchGrowEventTargets and the two
     hooks it installs (js_event_state.inc). */
  const createEventStateStore = ({
    cachedNode, derefReference, retentionStats, nativeNodeStateCleanups,
    wrapperListeners, hasOwnProperty,
  }) => {
    const nativeNodeListeners = new Map(),
      nativeNodeHandlers = new Map();
    const wrapperEventHolder = Symbol("event holder"),
      wrapperHandlers = Symbol("wrapper handlers"),
      /* handle -> WeakRef of the holder keeping that node's state */
      heldEventStates = new Map(),
      /* On every holder: false, or the next holder in pendingHolders. */
      holderPending = Symbol("pending release");
    /* Holders whose release has not finished (a refused allocation stopped
       it), chained through holderPending and kept alive from here until a
       later release finishes them: their trees may have outlived their
       wrappers by then. */
    let pendingHolders = null;
    /* Where a node's state lives, for __tilefinchNoteEventState: nowhere,
       rooted, held, or kept on its wrapper. (Native code marks a detached
       tree whose state could not be held STUCK: the sweep leaves it until
       the tree changes.) */
    const EVENT_STATE_NONE = 0,
      EVENT_STATE_ROOTED = 1,
      EVENT_STATE_HELD = 2,
      EVENT_STATE_KEPT = 3;
    /* The capacity the pair of maps shares. */
    const nativeEventTargets = { capacity: 128 };
    /* Room for one more target in `storage`, one map of the pair, growing
       the pair's capacity when it is full and native code admits the
       growth. */
    const eventTargetRoom = (storage) => {
      if (storage.size < nativeEventTargets.capacity) return true;
      const grown = Number(
        __tilefinchGrowEventTargets(nativeEventTargets.capacity)) || 0;
      if (grown > nativeEventTargets.capacity)
        nativeEventTargets.capacity = grown;
      return storage.size < nativeEventTargets.capacity;
    };
    const heldEventHolder = (handle) => {
      const reference = heldEventStates.get(handle);
      return reference ? derefReference(reference) || null : null;
    };
    const heldEventState = (handle) =>
      heldEventStates.size ? heldEventHolder(handle)?.get(handle) || null : null;
    /* The node's wrapper carries its state (kept). The state leaves a
       wrapper as an undefined value rather than a deleted property: a
       delete can copy a shape the wrapper shares, an allocation that may
       fail after the state already moved. */
    const keptOnWrapper = (wrapper) =>
      !!wrapper && hasOwnProperty.call(wrapper, wrapperHandlers) &&
      wrapper[wrapperHandlers] !== undefined;
    /* The state left the node's wrapper (rooted or held): the maps it
       shares stay, but new state must not be added to the wrapper alone. */
    const unkeep = (handle) => {
      const wrapper = cachedNode(handle);
      if (keptOnWrapper(wrapper)) wrapper[wrapperHandlers] = undefined;
    };
    /* Puts a node's whole state on `wrapper` and has native code keep that
       wrapper; false when the wrapper cannot carry it (a page froze it) or
       is not the node's live wrapper. Allocates nothing when the wrapper
       already carries it. */
    const keepOnWrapper = (handle, wrapper, listeners, handlers) => {
      if (!wrapper) return false;
      const wasKept = keptOnWrapper(wrapper);
      try {
        if (wasKept) wrapper[wrapperHandlers] = handlers;
        else
          Object.defineProperty(wrapper, wrapperHandlers, {
            value: handlers,
            writable: true,
            configurable: true,
          });
        if (listeners) wrapper[wrapperListeners] = listeners;
      } catch {}
      if (
        wrapper[wrapperHandlers] !== handlers ||
        (listeners && wrapper[wrapperListeners] !== listeners) ||
        !__tilefinchNoteEventState(handle, EVENT_STATE_KEPT, wrapper)
      ) {
        if (!wasKept && keptOnWrapper(wrapper))
          try {
            wrapper[wrapperHandlers] = undefined;
          } catch {}
        return false;
      }
      nativeNodeListeners.delete(handle);
      nativeNodeHandlers.delete(handle);
      return true;
    };
    /* Roots `state` in the per-handle maps; past their capacity it is kept
       on the node's live wrapper. Only a node without a usable wrapper roots
       past the capacity (a held tree re-rooted after its own wrapper went,
       or a frozen wrapper): the state is never dropped. The state leaves
       where it was (a kept wrapper) only once rooted. */
    const rootEventState = (handle, state) => {
      const { listeners, handlers } = state;
      if (!listeners && !handlers) {
        __tilefinchNoteEventState(handle, EVENT_STATE_NONE);
        return;
      }
      if (
        (!listeners || nativeNodeListeners.has(handle) ||
          eventTargetRoom(nativeNodeListeners)) &&
        (!handlers || nativeNodeHandlers.has(handle) ||
          eventTargetRoom(nativeNodeHandlers))
      ) {
        if (listeners) nativeNodeListeners.set(handle, listeners);
        if (handlers) nativeNodeHandlers.set(handle, handlers);
        unkeep(handle);
        __tilefinchNoteEventState(handle, EVENT_STATE_ROOTED);
        return;
      }
      if (keepOnWrapper(handle, cachedNode(handle), listeners, handlers))
        return;
      if (listeners) nativeNodeListeners.set(handle, listeners);
      if (handlers) nativeNodeHandlers.set(handle, handlers);
      unkeep(handle);
      __tilefinchNoteEventState(handle, EVENT_STATE_ROOTED);
    };
    /* Roots again each entry of `holder`. An entry leaves the holder only
       once rooted, so one a refused allocation stopped stays held (the
       caller keeps the holder in pendingHolders). */
    const releaseEventHolder = (holder) => {
      if (!holder?.size) return;
      for (const [handle, state] of holder) {
        rootEventState(handle, state);
        heldEventStates.delete(handle);
        holder.delete(handle);
      }
    };
    /* Queues `holder` for release; allocates nothing. */
    const pendEventHolder = (holder) => {
      if (!holder?.size || holder[holderPending] !== false) return;
      holder[holderPending] = pendingHolders;
      pendingHolders = holder;
    };
    /* Releases the queued holders; one a refused allocation stopped stays
       queued, with those after it. */
    const releasePendingHolders = () => {
      while (pendingHolders !== null) {
        const holder = pendingHolders;
        releaseEventHolder(holder);
        pendingHolders = holder[holderPending];
        holder[holderPending] = false;
      }
    };
    /* Drops the heldEventStates keys a failed hold added for `reference`. */
    const unstageEventHold = (records, reference) => {
      for (let i = 0; i < records.length; i++) {
        const handle = Number(records[i]);
        if (heldEventStates.get(handle) === reference)
          heldEventStates.delete(handle);
      }
    };
    /* Native: `records` are the handles in one detached tree with event
       state, `wrappers` those of its live wrappers. False when a wrapper
       refused the holder: a wrapper the page made non-extensible cannot keep
       the tree's state, which then stays where it was, and native code
       leaves the tree to the sweep until it changes. No wrapper keeps a
       refused holder: a later release would root its entries again, a node
       retired since among them.

       Everything that allocates comes first: the holder with its entries,
       its WeakRef, the stamp property on every wrapper, and the
       heldEventStates keys it adds. Only then does the state leave its old
       places (the per-handle maps, a kept wrapper, an earlier holder), in
       steps that allocate nothing. A refused allocation (the realm's heap
       is full) throws before any state moved, and native code retries the
       tree later. */
    __tilefinchRealmGlobal.__tilefinchHoldEventState = (records, wrappers) => {
      const holder = new Map();
      holder[holderPending] = false;
      for (let i = 0; i < records.length; i++) {
        const handle = Number(records[i]),
          state = heldEventHolder(handle)?.get(handle),
          kept = cachedNode(handle),
          own = keptOnWrapper(kept),
          listeners = nativeNodeListeners.get(handle) || state?.listeners ||
            (own ? kept[wrapperListeners] : null) || null,
          handlers = nativeNodeHandlers.get(handle) || state?.handlers ||
            (own ? kept[wrapperHandlers] : null) || null;
        if (listeners || handlers) holder.set(handle, { listeners, handlers });
      }
      const reference = holder.size ? new WeakRef(holder) : null;
      if (reference) {
        /* The wrappers and what each carried before. A wrapper without the
           stamp property gets it (undefined, as good as none) now, so that
           stamping it allocates nothing. */
        const stamped = [];
        for (let i = 0; i < wrappers.length; i++) {
          const wrapper = cachedNode(wrappers[i]);
          if (!wrapper) continue;
          if (!hasOwnProperty.call(wrapper, wrapperEventHolder)) {
            try {
              wrapper[wrapperEventHolder] = undefined;
            } catch (error) {
              if (Object.isExtensible(wrapper)) throw error;
            }
            if (!hasOwnProperty.call(wrapper, wrapperEventHolder)) return false;
          }
          stamped.push(wrapper, wrapper[wrapperEventHolder]);
        }
        try {
          for (let i = 0; i < records.length; i++) {
            const handle = Number(records[i]);
            if (holder.has(handle) && !heldEventStates.has(handle))
              heldEventStates.set(handle, reference);
          }
        } catch (error) {
          unstageEventHold(records, reference);
          throw error;
        }
        for (let j = 0; j < stamped.length; j += 2) {
          const wrapper = stamped[j];
          try {
            wrapper[wrapperEventHolder] = holder;
          } catch {}
          if (wrapper[wrapperEventHolder] !== holder) {
            for (let k = 0; k < j; k += 2)
              stamped[k][wrapperEventHolder] = stamped[k + 1];
            unstageEventHold(records, reference);
            return false;
          }
        }
      }
      /* From here nothing allocates. */
      for (let i = 0; i < records.length; i++) {
        const handle = Number(records[i]),
          previous = heldEventHolder(handle),
          kept = cachedNode(handle);
        if (previous !== holder) previous?.delete(handle);
        nativeNodeListeners.delete(handle);
        nativeNodeHandlers.delete(handle);
        if (keptOnWrapper(kept)) kept[wrapperHandlers] = undefined;
        if (holder.has(handle)) {
          heldEventStates.set(handle, reference);
          __tilefinchNoteEventState(handle, EVENT_STATE_HELD);
        } else {
          heldEventStates.delete(handle);
          __tilefinchNoteEventState(handle, EVENT_STATE_NONE);
        }
      }
      return true;
    };
    /* The moving subtree's wrappers join `holder`, that of the tree they
       move into, when the held state moving with them is already its own
       and no wrapper keeps another holder or refuses this one. */
    const joinEventHolder = (holder, handles, wrappers) => {
      for (let i = 1; i < handles.length; i++)
        if (heldEventHolder(Number(handles[i])) !== holder) return false;
      for (let i = 0; i < wrappers.length; i++) {
        const wrapper = cachedNode(wrappers[i]);
        if (!wrapper || wrapper[wrapperEventHolder] === holder) continue;
        if (wrapper[wrapperEventHolder]?.size) return false;
        try {
          wrapper[wrapperEventHolder] = holder;
        } catch {}
        if (wrapper[wrapperEventHolder] !== holder) return false;
      }
      return true;
    };
    /* Native: roots again the held state of each handle and that of its
       live wrapper's tree. For an insertion, `handles` are the parent's
       handle and then the held handles moving, and `wrappers`, when given,
       the moving subtree's live wrappers, which join the parent tree's
       holder instead when they can. True when the first handle's tree had a
       holder.

       The holders are queued first, which allocates nothing, and leave the
       queue only once all their state is rooted: a release a refused
       allocation stopped throws, and the queue keeps the rest alive until a
       later release (native code retries one with `handles` 0) finishes
       it. `handles` may be one handle as a number: native code could not
       make the list. */
    __tilefinchRealmGlobal.__tilefinchReleaseEventHolders = (handles, wrappers) => {
      if (typeof handles === "number") {
        if (handles) {
          pendEventHolder(heldEventHolder(handles));
          pendEventHolder(cachedNode(handles)?.[wrapperEventHolder]);
        }
        releasePendingHolders();
        return false;
      }
      const first = Number(handles[0]) || 0,
        holder = first
          ? heldEventHolder(first) || cachedNode(first)?.[wrapperEventHolder]
          : null,
        held = !!holder?.size;
      if (!wrappers || !held || !joinEventHolder(holder, handles, wrappers))
        for (let i = 0; i < handles.length; i++) {
          const handle = Number(handles[i]);
          if (!handle) continue;
          pendEventHolder(heldEventHolder(handle));
          pendEventHolder(cachedNode(handle)?.[wrapperEventHolder]);
        }
      releasePendingHolders();
      return held;
    };
    nativeNodeStateCleanups.push((handle) => {
      nativeNodeListeners.delete(handle);
      nativeNodeHandlers.delete(handle);
      if (heldEventStates.size) {
        heldEventHolder(handle)?.delete(handle);
        heldEventStates.delete(handle);
      }
    });
    const nativeNodeMap = (storage, field, node, create, dropCounter) => {
      const handle = Number(node?.__handle) || 0;
      if (!handle) return null;
      let map = storage.get(handle);
      if (map) return map;
      const held = heldEventState(handle);
      if (held?.[field]) return held[field];
      if (!create) return null;
      if (held) return (held[field] = new Map());
      /* A node whose state its wrapper carries keeps it all there. */
      if (keptOnWrapper(node)) return null;
      if (!eventTargetRoom(storage)) {
        retentionStats[dropCounter]++;
        if (
          keepOnWrapper(
            handle,
            node,
            nativeNodeListeners.get(handle) || node[wrapperListeners] || null,
            nativeNodeHandlers.get(handle) || null,
          )
        )
          return null;
      }
      map = new Map();
      storage.set(handle, map);
      __tilefinchNoteEventState(handle, EVENT_STATE_ROOTED);
      return map;
    };
    const listenerMap = (node, create = false) =>
      nativeNodeMap(
        nativeNodeListeners,
        "listeners",
        node,
        create,
        "listenerDrops",
      );
    const eventHandlerMap = (node, create = false) => {
        const native = nativeNodeMap(
          nativeNodeHandlers,
          "handlers",
          node,
          create,
          "handlerDrops",
        );
        if (native) return native;
        /* A kept node's handlers, or those of a node without a native handle:
           owned by the wrapper alone, so a handler closing over its element
           is an ordinary collectable cycle. */
        let map = node?.[wrapperHandlers] || null;
        if (!map && create && node) {
          map = new Map();
          try {
            if (Object.prototype.hasOwnProperty.call(node, wrapperHandlers))
              node[wrapperHandlers] = map;
            else
              Object.defineProperty(node, wrapperHandlers, {
                value: map,
                writable: true,
                configurable: true,
              });
          } catch {}
          if (node[wrapperHandlers] !== map) return null;
        }
        return map;
    };
    return {
      nativeNodeListeners, nativeNodeHandlers, wrapperEventHolder,
      wrapperHandlers, heldEventStates, nativeEventTargets, heldEventHolder,
      heldEventState, keptOnWrapper, keepOnWrapper, rootEventState,
      listenerMap, eventHandlerMap,
    };
  };
  const {
    nativeNodeListeners, nativeNodeHandlers, wrapperEventHolder,
    wrapperHandlers, heldEventStates, nativeEventTargets, heldEventHolder,
    heldEventState, keptOnWrapper, keepOnWrapper, rootEventState,
    listenerMap, eventHandlerMap,
  } = createEventStateStore({
    cachedNode, derefReference, retentionStats, nativeNodeStateCleanups,
    wrapperListeners, hasOwnProperty,
  });
  const propertyHandlerRecordToken = {},
    installEventHandler = (
      node, type, callback, token = propertyHandlerRecordToken,
    ) => {
      const map = eventHandlerMap(node, true);
      if (!map) return null;
      let record = map.get(type);
      if (!record || record.token !== propertyHandlerRecordToken) {
        record = {
          token: propertyHandlerRecordToken,
          sourceToken: token,
          callback: null,
          wrapper: null,
        };
        map.set(type, record);
      }
      record.sourceToken = token;
      record.callback = typeof callback === "function" ? callback : null;
      const wantsWrapper = record.callback !== null ||
        token === inlineHandlerRecordToken;
      if (wantsWrapper && record.wrapper === null) {
        record.wrapper = function (event) {
          const current = record.callback;
          if (typeof current !== "function") return;
          const returned = ancestorApply(current, this, [event]);
          if (returned === false) event.preventDefault();
        };
        node.addEventListener(type, record.wrapper, false);
      } else if (!wantsWrapper && record.wrapper !== null) {
        node.removeEventListener(type, record.wrapper, false);
        record.wrapper = null;
      }
      return record.callback;
    },
    inlineEventHandler = (node, type, name) => {
      const retained = eventHandlerMap(node, false);
      if (retained?.has(type)) {
        const value = retained.get(type);
        if (value?.token !== propertyHandlerRecordToken) return value;
        if (value.sourceToken !== inlineHandlerRecordToken || value.callback)
          return value.callback;
      }
      if (
        __tilefinchRealmGlobal.__tilefinchCspAllowsInlineEventHandlers === false ||
        typeof node?.getAttribute !== "function"
      )
        return null;
      const source = node.getAttribute(name);
      if (source === null) return null;
      let callable = null;
      try {
        callable = __tilefinchRealmGlobal.__tilefinchCompileInlineEventHandler(source);
      } catch (error) {
        __tilefinchReportUncaught(error, "inline " + name);
      }
      const map = eventHandlerMap(node, true);
      if (!map) return null;
      installEventHandler(node, type, callable, inlineHandlerRecordToken);
      return typeof callable === "function" ? callable : null;
    },
    invalidateInlineEventHandler = (node, attributeName) => {
      const name = String(attributeName || "").toLowerCase();
      if (name.length <= 2 || !name.startsWith("on")) return;
      const type = name.slice(2),
        map = eventHandlerMap(node, false),
        value = map?.get(type),
        attributePresent = typeof node?.getAttribute === "function" &&
          node.getAttribute(name) !== null;
      if (attributePresent) {
        /* Reserve (or retain) the handler listener's current position now;
         * compile the changed source lazily immediately before dispatch. */
        installEventHandler(node, type, null, inlineHandlerRecordToken);
      } else if (value?.token === propertyHandlerRecordToken) {
        installEventHandler(node, type, null);
        map.delete(type);
      }
      if (type.startsWith("mouse") || type.startsWith("pointer"))
        __tilefinchRealmGlobal.__tilefinchPointerMarkupChanged?.();
    };
  __tilefinchRealmGlobal.__tilefinchCanonicalElement = (node) => {
    const tag = String(node?.tagName || "").toLowerCase();
    return tag === "html"
      ? document.documentElement || node
      : tag === "head"
        ? document.head || node
        : tag === "body"
          ? document.body || node
          : node;
  };
  /* State that points back at its owner (Attr and NamedNodeMap caches,
     listener lists, shadow peers) must not live in a realm-global WeakMap:
     its values keep that cycle alive in this collector. Stamp an
     unreflectable private slot on the owner instead, so ordinary cycle
     collection sees the complete ownership graph. Stamping preserves the
     prototype and also works after the author freezes the object. Each call
     makes an independent slot; set() on an already stamped owner throws. */
  const createPrivateSlot = () => {
    class Stamp extends class { constructor(owner) { return owner; } } {
      #value;
      constructor(owner, value) { super(owner); this.#value = value; }
      static get(owner) {
        return owner !== null
          && (typeof owner === "object" || typeof owner === "function")
          && #value in owner ? owner.#value : undefined;
      }
    }
    return {
      get: Stamp.get,
      set: (owner, value) => (new Stamp(owner, value), value),
    };
  };
  const createAttributeState = Object.create,
    attributeSlot = createPrivateSlot(),
    attributeState = (owner, create = false) => {
      const state = attributeSlot.get(owner);
      return state !== undefined || !create
        ? state : attributeSlot.set(owner, createAttributeState(null));
    };
  /* Consumed by the CSSOM installer before author script runs. */
  __tilefinchRealmGlobal.__tilefinchTakeOwnerAttributeState = attributeState;
  /* Nonempty namespace mirrors contain semantic data the native DOM cannot
     reconstruct yet. Keep their existing lifetime across wrapper collection;
     ordinary Attr/NamedNodeMap caches no longer retain discarded owners. */
  const namespacedAttributes = new WeakMap(),
    emptyNamespaceAttributes = Object.freeze([]),
    normalizeNamespace = (namespace) =>
      namespace === null || namespace === undefined || namespace === ""
        ? null
        : String(namespace),
    attributeKey = (namespaceURI, localName) =>
      String(namespaceURI === null ? "" : namespaceURI) +
      "\u0000" +
      String(localName),
    attributeRecord = (
      owner,
      name,
      value,
      namespaceURI = null,
      prefix = null,
    ) => {
      const localName =
        prefix && name.startsWith(prefix + ":")
          ? name.slice(prefix.length + 1)
          : name;
      let cache = owner ? attributeState(owner)?.objects : null;
      if (owner && !cache) {
        cache = new Map();
        attributeState(owner, true).objects = cache;
      }
      const key = attributeKey(namespaceURI, localName);
      let record = cache?.get(key);
      if (!record) {
        record = Object.create(Attr.prototype);
        Object.defineProperties(record, {
          __tilefinchAttributeOwner: {
            configurable: true,
            writable: true,
            value: owner,
          },
          __tilefinchAttributeOwnerDocument: {
            configurable: true,
            writable: true,
            value: owner?.ownerDocument || __tilefinchRealmGlobal.document || null,
          },
          __tilefinchAttributeName: {
            configurable: true,
            writable: true,
            value: String(name),
          },
          __tilefinchAttributeLocalName: {
            configurable: true,
            writable: true,
            value: String(localName),
          },
          __tilefinchAttributeNamespace: {
            configurable: true,
            writable: true,
            value: namespaceURI,
          },
          __tilefinchAttributePrefix: {
            configurable: true,
            writable: true,
            value: prefix,
          },
          __tilefinchAttributeValue: {
            configurable: true,
            writable: true,
            value: String(value),
          },
          nodeType: { value: 2 },
        });
        if (cache) {
          cache.set(key, record);
          let order = attributeState(owner).order;
          if (!order) {
            order = [];
            attributeState(owner).order = order;
          }
          order.push(record);
        }
      }
      record.__tilefinchAttributeOwner = owner;
      record.__tilefinchAttributeName = String(name);
      record.__tilefinchAttributeLocalName = String(localName);
      record.__tilefinchAttributeNamespace = namespaceURI;
      record.__tilefinchAttributePrefix = prefix;
      record.__tilefinchAttributeValue = String(value);
      return record;
    },
    namespaceAttributes = (node, create = false) => {
      let values = namespacedAttributes.get(node);
      if (!values && create) {
        values = [];
        namespacedAttributes.set(node, values);
      }
      /* Ordinary HTML reads need no namespace mirror. Keep the empty case
         shared until a write actually introduces mirrored attributes. */
      return values || emptyNamespaceAttributes;
    };
  const namedWindowProperties = new Map(),
    namedWindowLimit = 128;
  /* Window named properties: a child navigable's target name answers
     first (its WindowProxy; frames.js binds the lookup), then an element
     id. Own Window properties shadow both. */
  let namedChildNavigable = null;
  const namedWindowValue = (name) => {
    const view = namedChildNavigable?.(name);
    return view !== undefined
      ? view
      : document.getElementById(name) || undefined;
  };
  __tilefinchRealmGlobal.__tilefinchBindNamedChildNavigables = (lookup) => {
    delete __tilefinchRealmGlobal.__tilefinchBindNamedChildNavigables;
    namedChildNavigable = lookup;
    /* Retire a name exposed for a child navigable that nothing answers. */
    return (name) => {
      const descriptor = Object.getOwnPropertyDescriptor(__tilefinchRealmGlobal, name);
      if (!namedWindowProperties.has(name)) return;
      /* A page can replace our accessor with defineProperty, bypassing
         its setter. Retire only the exact accessor we installed. */
      if (descriptor?.get !== namedWindowProperties.get(name)) {
        namedWindowProperties.delete(name);
        return;
      }
      if (namedWindowValue(name) !== undefined) return;
      namedWindowProperties.delete(name);
      delete __tilefinchRealmGlobal[name];
    };
  };
  __tilefinchRealmGlobal.__tilefinchExposeNamedProperty = (name) => {
    name = String(name || "");
    if (
      !name ||
      name.length > 128 ||
      namedWindowProperties.has(name) ||
      namedWindowProperties.size >= namedWindowLimit ||
      Object.prototype.hasOwnProperty.call(__tilefinchRealmGlobal, name)
    )
      return false;
    const descriptor = {
      configurable: true,
      enumerable: false,
      get() {
        return namedWindowValue(name);
      },
      set(value) {
        namedWindowProperties.delete(name);
        Object.defineProperty(__tilefinchRealmGlobal, name, {
          configurable: true,
          enumerable: true,
          writable: true,
          value,
        });
      },
    };
    namedWindowProperties.set(name, descriptor.get);
    Object.defineProperty(__tilefinchRealmGlobal, name, descriptor);
    return true;
  };
  class EventTarget {}
  class Node extends EventTarget {}
  Object.defineProperty(Node.prototype, "hasChildNodes", {
    configurable: true,
    enumerable: true,
    writable: true,
    value: function hasChildNodes() {
      return this.firstChild != null;
    },
  });
  Object.defineProperty(Node.prototype, "isConnected", Object.getOwnPropertyDescriptor({
    get isConnected() {
      for (
        let at = this, steps = 0;
        at && steps < ancestorLimit;
        at =
          at instanceof ShadowRoot
            ? at.host
            : at.__tilefinchDetachedParent || at.parentNode,
          steps++
      )
        if (
          at instanceof Document ||
          at.nodeType === Node.DOCUMENT_NODE
        )
          return true;
      return false;
    },
  }, "isConnected"));
  class Attr extends Node {}
  class NamedNodeMap {}
  Node.ELEMENT_NODE = 1;
  Node.ATTRIBUTE_NODE = 2;
  Node.TEXT_NODE = 3;
  Node.CDATA_SECTION_NODE = 4;
  Node.PROCESSING_INSTRUCTION_NODE = 7;
  Node.COMMENT_NODE = 8;
  Node.DOCUMENT_TYPE_NODE = 10;
  Node.DOCUMENT_NODE = 9;
  Node.DOCUMENT_FRAGMENT_NODE = 11;
  Node.DOCUMENT_POSITION_DISCONNECTED = 1;
  Node.DOCUMENT_POSITION_PRECEDING = 2;
  Node.DOCUMENT_POSITION_FOLLOWING = 4;
  Node.DOCUMENT_POSITION_CONTAINS = 8;
  Node.DOCUMENT_POSITION_CONTAINED_BY = 16;
  Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC = 32;
  Object.assign(Node.prototype, {
    DOCUMENT_POSITION_DISCONNECTED: 1,
    DOCUMENT_POSITION_PRECEDING: 2,
    DOCUMENT_POSITION_FOLLOWING: 4,
    DOCUMENT_POSITION_CONTAINS: 8,
    DOCUMENT_POSITION_CONTAINED_BY: 16,
    DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32,
  });
  Object.defineProperties(Attr.prototype, {
    name: {
      get() {
        return this.__tilefinchAttributeName;
      },
    },
    nodeName: {
      get() {
        return this.__tilefinchAttributeName;
      },
    },
    localName: {
      get() {
        return this.__tilefinchAttributeLocalName;
      },
    },
    namespaceURI: {
      get() {
        return this.__tilefinchAttributeNamespace;
      },
    },
    prefix: {
      get() {
        return this.__tilefinchAttributePrefix;
      },
    },
    ownerElement: {
      get() {
        return this.__tilefinchAttributeOwner;
      },
    },
    ownerDocument: {
      get() {
        return (
          this.__tilefinchAttributeOwner?.ownerDocument ||
          this.__tilefinchAttributeOwnerDocument
        );
      },
    },
    specified: { get() { return true; } },
    value: {
      get() {
        return this.__tilefinchAttributeValue;
      },
      set(value) {
        value = String(value);
        const owner = this.__tilefinchAttributeOwner;
        if (owner)
          owner.setAttributeNS(
            this.__tilefinchAttributeNamespace,
            this.__tilefinchAttributeName,
            value,
          );
        this.__tilefinchAttributeValue = value;
      },
    },
    nodeValue: {
      get() {
        return this.value;
      },
      set(value) {
        this.value = value;
      },
    },
    textContent: {
      get() {
        return this.value;
      },
      set(value) {
        this.value = value;
      },
    },
  });
  class CharacterData extends Node {}
  class Text extends CharacterData {}
  Text.prototype.splitText = function splitText(offset) {
    const data = String(this.data);
    offset = Number(offset) >>> 0;
    if (offset > data.length)
      throw new DOMException("Invalid split offset", "IndexSizeError");
    const tail = (this.ownerDocument || document).createTextNode(
      data.slice(offset),
    );
    this.data = data.slice(0, offset);
    this.parentNode?.insertBefore(tail, this.nextSibling);
    return tail;
  };
  class CDATASection extends Text {}
  class ProcessingInstruction extends CharacterData {}
  class Comment extends CharacterData {}
  class DocumentType extends Node {}
  class Element extends Node {}
  Object.defineProperty(Element.prototype, "animate", {
    configurable: true,
    writable: true,
    value: function animate(keyframes, options = undefined) {
      if (typeof __tilefinchRealmGlobal.__tilefinchAnimateElement !== "function") {
        try {
          __tilefinchRealmGlobal.__tilefinchEnsureMotionBootstrap?.();
        } catch {}
      }
      if (typeof __tilefinchRealmGlobal.__tilefinchAnimateElement !== "function")
        throw new DOMException("Animations are unavailable", "NotSupportedError");
      return __tilefinchRealmGlobal.__tilefinchAnimateElement(this, keyframes, options);
    },
  });
  class HTMLElement extends Element {
    constructor() {
      super();
      const stack = __tilefinchRealmGlobal.__tilefinchCustomElementConstructionStack;
      if (stack?.length) {
        const element = stack[stack.length - 1];
        __tilefinchRealmGlobal.__tilefinchPrepareNativePrototype?.(
          element, new.target.prototype);
        Object.setPrototypeOf(element, new.target.prototype);
        return element;
      }
      const constructed =
        __tilefinchRealmGlobal.__tilefinchConstructCustomElement?.(new.target);
      if (constructed) return constructed;
    }
  }
  class HTMLUnknownElement extends HTMLElement {}
  /* Reflected IDL attributes (HTML's "reflect"), for every module: WebIDL
     members are enumerable, and computed accessor keys give the functions
     their WebIDL names ("get dir"). The content attribute is the lowercased
     IDL name unless given. An enumerated attribute reads a known keyword in
     its canonical case and a missing or invalid value as its default. */
  const accessor = (name, object) =>
      Object.getOwnPropertyDescriptor(object, name),
    reflectString = (idl, attribute = idl.toLowerCase()) =>
      accessor(idl, {
        get [idl]() {
          return this.getAttribute(attribute) || "";
        },
        set [idl](value) {
          this.setAttribute(attribute, String(value));
        },
      }),
    reflectBoolean = (idl, attribute = idl.toLowerCase()) =>
      accessor(idl, {
        get [idl]() {
          return this.hasAttribute(attribute);
        },
        set [idl](value) {
          this.toggleAttribute(attribute, !!value);
        },
      }),
    reflectEnumerated = (idl, keywords, missing = "", invalid = missing,
                         attribute = idl.toLowerCase()) =>
      accessor(idl, {
        get [idl]() {
          const value = this.getAttribute(attribute);
          if (value === null) return missing;
          const keyword = String(value).toLowerCase();
          return keywords.includes(keyword) ? keyword : invalid;
        },
        set [idl](value) {
          this.setAttribute(attribute, String(value));
        },
      }),
    /* Documents other than the main one (see hardening.js). */
    secondaryDocumentPrototype = (base) =>
      __tilefinchRealmGlobal.__tilefinchSecondaryDocumentPrototype?.(base) || base;
  /* platform.js and compat.js take these; compat.js deletes the global. */
  __tilefinchRealmGlobal.__tilefinchDomShared = {
    createPrivateSlot,
    reflectString,
    reflectBoolean,
    reflectEnumerated,
    secondaryDocumentPrototype,
  };
  Object.defineProperties(HTMLElement.prototype, {
    title: reflectString("title"),
    lang: reflectString("lang"),
  });
  class SVGElement extends Element {
    getBBox() {
      const rect = this.getBoundingClientRect(),
        number = (name, fallback = 0) => {
          const value = Number.parseFloat(this.getAttribute(name));
          return Number.isFinite(value) ? value : fallback;
        };
      return new DOMRect(
        number("x"),
        number("y"),
        Math.max(0, number("width", rect.width)),
        Math.max(0, number("height", rect.height)),
      );
    }
    getComputedTextLength() {
      const name = String(this.localName || "").toLowerCase();
      if (name !== "text" && name !== "tspan" && name !== "textpath")
        throw new TypeError("SVG text element required");
      const width = Number(this.getBoundingClientRect().width);
      return Number.isFinite(width) && width > 0 ? width : 0;
    }
  }
  Object.defineProperties(SVGElement.prototype, {
    ownerSVGElement: {
      configurable: true,
      enumerable: true,
      get() {
        for (
          let node = this.parentElement, steps = 0;
          node && steps < ancestorLimit;
          node = node.parentElement, steps++
        )
          if (
            node instanceof SVGElement &&
            String(node.localName).toLowerCase() === "svg"
          )
            return node;
        return null;
      },
    },
    viewportElement: {
      configurable: true,
      enumerable: true,
      get() {
        return this.ownerSVGElement;
      },
    },
  });
  /* The bounded profile shares the SVG element wrapper while exposing the
     standard constructor names.  This avoids three per-realm prototype
     objects on PSP; instanceof and the graphics API remain available. */
  const SVGGraphicsElement = SVGElement,
    SVGGeometryElement = SVGElement,
    SVGTextContentElement = SVGElement,
    SVGSVGElement = SVGElement;
  class HTMLDivElement extends HTMLElement {}
  class HTMLBodyElement extends HTMLElement {}
  class HTMLFrameSetElement extends HTMLElement {}
  class HTMLStyleElement extends HTMLElement {}
  class HTMLIFrameElement extends HTMLElement {}
  class HTMLInputElement extends HTMLElement {}
  class HTMLScriptElement extends HTMLElement {}
  class HTMLFormElement extends HTMLElement {
    submit() {
      if (__tilefinchRealmGlobal.__tilefinchQueueFormSubmission?.(this, null)) return;
      __tilefinchRealmGlobal.__tilefinchSubmittedBody =
        __tilefinchRealmGlobal.__tilefinchSubmissionBody?.(this, null) ?? null;
      __tilefinchRealmGlobal.__tilefinchSubmitted = true;
      __tilefinchRealmGlobal.__tilefinchSubmittedFormHandle = this.__handle;
      __tilefinchRealmGlobal.__tilefinchSubmittedSubmitterHandle = 0;
    }
    requestSubmit(submitter = null) {
      if (
        submitter !== null &&
        (!(
          submitter instanceof HTMLButtonElement ||
          submitter instanceof HTMLInputElement
        ) ||
          submitter.closest("form") !== this)
      )
        throw new TypeError("Invalid submitter");
      if (submitter?.disabled) return;
      if (
        !this.noValidate &&
        !submitter?.formNoValidate &&
        !this.checkValidity()
      )
        return;
      const event = __tilefinchTrustedEvent(
        new SubmitEvent("submit", {
          bubbles: true,
          cancelable: true,
          submitter,
        }),
      );
      if (this.dispatchEvent(event)) {
        if (__tilefinchRealmGlobal.__tilefinchQueueFormSubmission?.(this, submitter))
          return;
        __tilefinchRealmGlobal.__tilefinchSubmittedBody =
          __tilefinchRealmGlobal.__tilefinchSubmissionBody?.(this, submitter) ?? null;
        __tilefinchRealmGlobal.__tilefinchSubmitted = true;
        __tilefinchRealmGlobal.__tilefinchSubmittedFormHandle = this.__handle;
        __tilefinchRealmGlobal.__tilefinchSubmittedSubmitterHandle = submitter?.__handle || 0;
      }
    }
  }
  class HTMLButtonElement extends HTMLElement {}
  class HTMLTextAreaElement extends HTMLElement {}
  class HTMLSelectElement extends HTMLElement {}
  class HTMLOptionElement extends HTMLElement {}
  class HTMLSelectedContentElement extends HTMLElement {}
  class HTMLLabelElement extends HTMLElement {}
  class HTMLFieldSetElement extends HTMLElement {}
  class HTMLLegendElement extends HTMLElement {}
  class HTMLOutputElement extends HTMLElement {}
  class HTMLAnchorElement extends HTMLElement {}
  class HTMLLinkElement extends HTMLElement {}
  class HTMLMetaElement extends HTMLElement {}
  class HTMLTemplateElement extends HTMLElement {}
  class HTMLDialogElement extends HTMLElement {}
  class HTMLDetailsElement extends HTMLElement {}
  class HTMLSummaryElement extends HTMLElement {}
  class HTMLImageElement extends HTMLElement {}
  class HTMLCanvasElement extends HTMLElement {}
  class HTMLMediaElement extends HTMLElement {}
  class HTMLAudioElement extends HTMLMediaElement {}
  class HTMLVideoElement extends HTMLMediaElement {}
  class HTMLSourceElement extends HTMLElement {}
  class HTMLSlotElement extends HTMLElement {}
  /* The rest of the HTML standard's element-interface table. None has a
     member of its own here, so one class body serves them all, and their
     prototypes share HTMLElement's native wrapper layer (see
     nativeElementPrototype) instead of each costing a layer of its own. */
  const sharedLayerPrototypes = [];
  for (const stem of (
      "Area Base BR Data DataList DList Directory Embed Font Frame HR " +
      "Head Heading Html LI Map Marquee Menu Meter Mod OList Object " +
      "OptGroup Paragraph Param Picture Pre Progress Quote Span Table " +
      "TableCaption TableCell TableCol TableRow TableSection Time Title " +
      "Track UList ").trim().split(" ")) {
    const name = "HTML" + stem + "Element",
      constructor = { [name]: class extends HTMLElement {} }[name];
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: name,
    });
    sharedLayerPrototypes.push(constructor.prototype);
    Object.defineProperty(__tilefinchRealmGlobal, name, {
      configurable: true,
      writable: true,
      value: constructor,
    });
  }
  /* " tag=index " into sharedLayerPrototypes, read once per tag name. */
  const sharedLayerTags =
    " area=0 base=1 br=2 data=3 datalist=4 dl=5 dir=6 embed=7 font=8 " +
    "frame=9 hr=10 head=11 h1=12 h2=12 h3=12 h4=12 h5=12 h6=12 html=13 " +
    "li=14 map=15 marquee=16 menu=17 meter=18 del=19 ins=19 ol=20 " +
    "object=21 optgroup=22 p=23 param=24 picture=25 pre=26 listing=26 " +
    "xmp=26 progress=27 blockquote=28 q=28 span=29 table=30 caption=31 " +
    "td=32 th=32 col=33 colgroup=33 tr=34 thead=35 tbody=35 tfoot=35 " +
    "time=36 title=37 track=38 ul=39 ";
  sharedLayerPrototypes.push(HTMLUnknownElement.prototype,
    HTMLDivElement.prototype, HTMLBodyElement.prototype);
  /* Elements the table gives plain HTMLElement; any other name that is not
     a valid custom element name is HTMLUnknownElement. */
  const plainHTMLElementTags = " abbr acronym address article aside b " +
    "basefont bdi bdo big center cite code dd dfn dt em figcaption figure " +
    "footer header hgroup i kbd main mark nav nobr noembed noframes " +
    "noscript plaintext rb rp rt rtc ruby s samp search section small " +
    "strike strong sub sup tt u var wbr ";
  const unknownElementTag = (tag) =>
    tag.includes("-")
      ? /^(?:annotation-xml|color-profile|font-face(?:-src|-uri|-format|-name)?|missing-glyph)$/.test(tag) ||
        !/^[a-z]/.test(tag)
      : !plainHTMLElementTags.includes(" " + tag + " ");
  class DocumentFragment extends Node {
    constructor() {
      super();
      return __tilefinchRealmGlobal.document?.createDocumentFragment?.() || this;
    }
  }
  const registerNativeShadowRoot = __tilefinchRealmGlobal.__tilefinchRegisterShadowRoot,
    ensureShadowBootstrap = __tilefinchRealmGlobal.__tilefinchEnsureShadowBootstrap,
    shadowWeakRef = weakNodeCache ? WeakRef : null,
    shadowMapType = Map, shadowWeakMapType = WeakMap;
  delete __tilefinchRealmGlobal.__tilefinchRegisterShadowRoot;
  const shadowConstructionToken = {};
  let shadowState = null, shadowFactory = null, shadowRootCreated = false,
    shadowModeByRoot = null, shadowDelegatesFocusByRoot = null,
    sharedShadowDescriptors = null, sharedShadowHostDescriptors = null;
  __tilefinchRealmGlobal.__tilefinchRegisterNativeNodeStateCleanup?.((handle) => {
    shadowState?.retire(Number(handle));
  });
  const shadowPeerFor = node => shadowState?.peer(node) || null,
    shadowRootForHost = (host, connectivity) =>
      shadowState?.forHost(host, connectivity) || null;
  class ShadowRoot extends DocumentFragment {
    constructor(token) {
      super();
      if (token !== shadowConstructionToken)
        throw new TypeError("Illegal constructor");
    }
    get host() {
      return shadowPeerFor(this);
    }
    get mode() {
      return shadowModeByRoot?.get(this) || "open";
    }
    get delegatesFocus() {
      return !!shadowDelegatesFocusByRoot?.get(this);
    }
    get slotAssignment() {
      return "named";
    }
    get activeElement() {
      if (!this.isConnected) return null;
      const active = (this.ownerDocument === document ? __tilefinchRealmGlobal.__tilefinchActiveElement : null) || null;
      return active && active.getRootNode() === this ? active : null;
    }
    get styleSheets() {
      if (!this.isConnected) return [];
      return Array.from(this.querySelectorAll("style"))
        .map((node) => node.sheet)
        .filter(Boolean);
    }
  }
  class Document extends Node {
    constructor() {
      super();
      return __tilefinchRealmGlobal.__tilefinchNewDocument?.() || this;
    }
  }
  class HTMLDocument extends Document {}
  class XMLDocument extends Document {}
  const nodeListIterator = (list, kind) => {
    let index = 0;
    return {
      next() {
        if (index >= list.length) return { done: true, value: undefined };
        const at = index++;
        return {
          done: false,
          value: kind === 0 ? at : kind === 1 ? list[at] : [at, list[at]],
        };
      },
      [Symbol.iterator]() {
        return this;
      },
    };
  };
  class NodeList {
    constructor() {
      throw new TypeError("Illegal constructor");
    }
    item(index) {
      return this[Number(index) >>> 0] ?? null;
    }
    entries() {
      return nodeListIterator(this, 2);
    }
    forEach(callback, thisArg) {
      if (typeof callback !== "function")
        throw new TypeError("callback must be a function");
      for (let index = 0; index < this.length; index++)
        callback.call(thisArg, this[index], index, this);
    }
    keys() {
      return nodeListIterator(this, 0);
    }
    values() {
      return nodeListIterator(this, 1);
    }
    [Symbol.iterator]() {
      return nodeListIterator(this, 1);
    }
  }
  class HTMLCollection {}
  let createLiveCollection;
  class Window extends EventTarget {}
  const domInterfaces = {
    EventTarget,
    Node,
    Attr,
    NamedNodeMap,
    CharacterData,
    Text,
    CDATASection,
    ProcessingInstruction,
    Comment,
    DocumentType,
    Element,
    HTMLElement,
    HTMLUnknownElement,
    SVGElement,
    SVGGraphicsElement,
    SVGGeometryElement,
    SVGTextContentElement,
    SVGSVGElement,
    HTMLDivElement,
    HTMLBodyElement,
    HTMLFrameSetElement,
    HTMLStyleElement,
    HTMLIFrameElement,
    HTMLInputElement,
    HTMLScriptElement,
    HTMLFormElement,
    HTMLButtonElement,
    HTMLTextAreaElement,
    HTMLSelectElement,
    HTMLOptionElement,
    HTMLSelectedContentElement,
    HTMLLabelElement,
    HTMLFieldSetElement,
    HTMLLegendElement,
    HTMLOutputElement,
    HTMLAnchorElement,
    HTMLLinkElement,
    HTMLMetaElement,
    HTMLTemplateElement,
    HTMLDialogElement,
    HTMLDetailsElement,
    HTMLSummaryElement,
    HTMLImageElement,
    HTMLCanvasElement,
    HTMLMediaElement,
    HTMLAudioElement,
    HTMLVideoElement,
    HTMLSourceElement,
    HTMLSlotElement,
    DocumentFragment,
    ShadowRoot,
    Document,
    HTMLDocument,
    XMLDocument,
    NodeList,
    HTMLCollection,
    Window,
  };
  for (const [name, constructor] of Object.entries(domInterfaces))
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: name,
    });
  Object.assign(__tilefinchRealmGlobal, domInterfaces);
  if (!(__tilefinchRealmGlobal instanceof Window))
    Object.setPrototypeOf(__tilefinchRealmGlobal, Window.prototype);
  /* QuickJS gives its global object an internal `global` class tag which
     wins over the inherited Window prototype tag. Browsing-context globals
     must nevertheless stringify as Window, like their WindowProxy surface.
     Keep an own, immutable brand so deleting an author-visible property
     cannot reveal the engine's internal global class name. */
  Object.defineProperty(__tilefinchRealmGlobal, Symbol.toStringTag, {
    configurable: false,
    value: "Window",
  });
  __tilefinchRealmGlobal.__tilefinchLiveHTMLCollection = (read) => {
    if (!createLiveCollection) {
      /* Keep the reader on the collection, not in a realm-global WeakMap
         value: it may retain the node that retains this collection. Defer
         the private brand and shared traps too; even that one-time setup
         matters to parser startup in tiny realms. */
      let create;
      class LiveCollection {
        #read;
        constructor(read) {
          this.#read = read;
        }
        static {
          const handler = {
            get(object, key) {
              const read = object.#read, values = read();
              if (key === "length") return values.length;
              if (key === "item")
                return (index) => read()[Number(index)] ?? null;
              if (key === "namedItem")
                return (name) => {
                  name = String(name);
                  return (
                    read().find((item) => item.id === name || item.name === name) ??
                    null
                  );
                };
              if (key === Symbol.iterator)
                return () => read()[Symbol.iterator]();
              if (typeof key === "string" && /^\d+$/.test(key))
                return values[Number(key)];
              if (
                ["filter", "find", "forEach", "indexOf", "map", "some"].includes(key)
              )
                return Array.prototype[key].bind(values);
              return object[key];
            },
            has(object, key) {
              if (key === "length") return true;
              if (typeof key === "string" && /^\d+$/.test(key))
                return Number(key) < object.#read().length;
              return key in object;
            },
          };
          const construct = Reflect.construct;
          create = (read) =>
            new Proxy(construct(LiveCollection, [read], HTMLCollection), handler);
        }
      }
      /* Publish only after class initialization has completed successfully. */
      createLiveCollection = create;
    }
    return createLiveCollection(read);
  };
  const characterDataUnsignedLong = (value) => {
    if (typeof value === "bigint")
      throw new TypeError("BigInt cannot be converted to an unsigned long");
    return Number(value) >>> 0;
  };
  Object.defineProperties(CharacterData.prototype, {
    data: {
      configurable: true,
      enumerable: true,
      get() {
        return this.textContent;
      },
      set(value) {
        this.textContent = value === null ? "" : String(value);
      },
    },
    length: {
      configurable: true,
      enumerable: true,
      get() {
        return this.data.length;
      },
    },
    substringData: {
      configurable: true,
      writable: true,
      value: function substringData(offset, count) {
        if (arguments.length < 2)
          throw new TypeError("substringData requires two arguments");
        offset = characterDataUnsignedLong(offset);
        count = characterDataUnsignedLong(count);
        if (offset > this.length)
          throw new DOMException("Offset is out of bounds", "IndexSizeError");
        return this.data.slice(offset, offset + count);
      },
    },
    appendData: {
      configurable: true,
      writable: true,
      value: function appendData(data) {
        if (arguments.length < 1)
          throw new TypeError("appendData requires one argument");
        this.data += String(data);
      },
    },
    insertData: {
      configurable: true,
      writable: true,
      value: function insertData(offset, data) {
        if (arguments.length < 2)
          throw new TypeError("insertData requires two arguments");
        offset = characterDataUnsignedLong(offset);
        if (offset > this.length)
          throw new DOMException("Offset is out of bounds", "IndexSizeError");
        const value = this.data;
        this.data = value.slice(0, offset) + String(data) + value.slice(offset);
      },
    },
    deleteData: {
      configurable: true,
      writable: true,
      value: function deleteData(offset, count) {
        if (arguments.length < 2)
          throw new TypeError("deleteData requires two arguments");
        this.replaceData(offset, count, "");
      },
    },
    replaceData: {
      configurable: true,
      writable: true,
      value: function replaceData(offset, count, data) {
        if (arguments.length < 3)
          throw new TypeError("replaceData requires three arguments");
        offset = characterDataUnsignedLong(offset);
        count = characterDataUnsignedLong(count);
        if (offset > this.length)
          throw new DOMException("Offset is out of bounds", "IndexSizeError");
        const value = this.data;
        this.data =
          value.slice(0, offset) +
          String(data) +
          value.slice(offset + Math.min(count, value.length - offset));
      },
    },
  });
  Node.prototype.contains = function contains(other) {
    if (other === null || other === undefined) return false;
    for (
      let at = other, steps = 0;
      at && steps < ancestorLimit;
      at = at.parentNode, steps++
    )
      if (at === this) return true;
    return false;
  };
  Node.prototype.normalize = function normalize() {
    let previousText = null;
    for (let child = this.firstChild; child; ) {
      const next = child.nextSibling;
      if (child.nodeType === Node.TEXT_NODE) {
        if (!child.data) {
          this.removeChild(child);
        } else if (previousText) {
          previousText.data += child.data;
          this.removeChild(child);
        } else {
          previousText = child;
        }
      } else {
        previousText = null;
        child.normalize?.();
      }
      child = next;
    }
  };
  Node.prototype.appendChild = function appendChild(child) {
    __tilefinchRealmGlobal.__tilefinchValidatePreInsert?.(this, child, null);
    throw new DOMException("Node cannot have children", "HierarchyRequestError");
  };
  const nativeRootNode = __tilefinchRealmGlobal.__tilefinchRootNode;
  Node.prototype.getRootNode = function getRootNode(options = {}) {
    /* Until a shadow root exists every root is a tree root: one native walk
       instead of a wrapper and an instanceof per ancestor. A root with a
       script-side detached parent takes the walk below. */
    if (
      !shadowRootCreated &&
      typeof nativeRootNode === "function" &&
      this.__handle > 0 &&
      !this.__tilefinchDetachedParent
    ) {
      const found = Number(nativeRootNode(this.__handle));
      if (found < 0) return document;
      const root = found > 0 ? wrap(found) : null;
      if (root && !root.__tilefinchDetachedParent)
        return root === document.documentElement ? document : root;
    }
    let at = this,
      shadow = null,
      steps = 0;
    while (at?.parentNode && steps < ancestorLimit) {
      at = at.parentNode;
      steps++;
      if (at instanceof ShadowRoot) {
        shadow = at;
        break;
      }
    }
    if (shadow)
      return options?.composed ? shadow.host.getRootNode(options) : shadow;
    return at === document.documentElement ? document : at;
  };
  const shadowHostNames = new Set([
      "article",
      "aside",
      "blockquote",
      "body",
      "div",
      "footer",
      "h1",
      "h2",
      "h3",
      "h4",
      "h5",
      "h6",
      "header",
      "main",
      "nav",
      "p",
      "section",
      "span",
    ]),
    shadowLightChildren = (host, elementsOnly = false) => {
      const root = shadowRootForHost(host),
        source =
          host.__handle === undefined
            ? Array.from(host.__detachedChildren || host.childNodes || [])
            : __tilefinchChildNodes(host.__handle).map(wrap),
        values = source
          .filter(
            (child) =>
              child !== root &&
              (!elementsOnly || child?.nodeType === Node.ELEMENT_NODE),
          );
      return values;
    },
    detachedShadowQueryAll = (root, selector) => {
      const value = String(selector).trim().toLowerCase(),
        output = [],
        queue = [...(root.children || [])],
        matches = (node) => {
          if (value === "*") return true;
          if (value.startsWith("#"))
            return String(node.getAttribute?.("id") || "").toLowerCase() ===
              value.slice(1);
          if (value.startsWith("."))
            return String(node.getAttribute?.("class") || "")
              .toLowerCase()
              .split(/\s+/)
              .includes(value.slice(1));
          return String(node.localName || "").toLowerCase() === value;
        };
      for (let at = 0; at < queue.length && output.length < 256; at++) {
        const node = queue[at];
        if (matches(node)) output.push(node);
        for (const child of node.children || []) queue.push(child);
      }
      return nodeList(output);
    },
    shadowAdjustedSibling = (node, related, relation) => {
      if (!related) return null;
      return related instanceof ShadowRoot
        ? wrap(__tilefinchRelation(related.__handle, relation))
        : related;
    },
    installShadowHostView = (host) => {
      const descriptors = sharedShadowHostDescriptors ||= {
        shadowRoot: {
          configurable: true,
          enumerable: true,
          get() {
            const root = shadowRootForHost(this);
            return root && shadowModeByRoot.get(root) === "open" ? root : null;
          },
        },
        childNodes: {
          configurable: true,
          enumerable: true,
          get() {
            return nodeList(shadowLightChildren(this));
          },
        },
        children: {
          configurable: true,
          enumerable: true,
          get() {
            if (!this.__tilefinchShadowChildrenCollection)
              Object.defineProperty(this, "__tilefinchShadowChildrenCollection", {
                configurable: true,
                value: __tilefinchRealmGlobal.__tilefinchLiveHTMLCollection(() =>
                  shadowLightChildren(this, true),
                ),
              });
            return this.__tilefinchShadowChildrenCollection;
          },
        },
        firstChild: {
          configurable: true,
          enumerable: true,
          get() {
            return shadowLightChildren(this)[0] || null;
          },
        },
        lastChild: {
          configurable: true,
          enumerable: true,
          get() {
            const values = shadowLightChildren(this);
            return values[values.length - 1] || null;
          },
        },
        firstElementChild: {
          configurable: true,
          enumerable: true,
          get() {
            return shadowLightChildren(this, true)[0] || null;
          },
        },
        lastElementChild: {
          configurable: true,
          enumerable: true,
          get() {
            const values = shadowLightChildren(this, true);
            return values[values.length - 1] || null;
          },
        },
        childElementCount: {
          configurable: true,
          enumerable: true,
          get() {
            return shadowLightChildren(this, true).length;
          },
        },
      };
      Object.defineProperties(host, descriptors);
    };
  Element.prototype.attachShadow = function attachShadow(init) {
    if (shadowRootForHost(this))
      throw new DOMException(
        "Shadow root already attached",
        "NotSupportedError",
      );
    const mode = String(init?.mode || "");
    if (mode !== "open" && mode !== "closed")
      throw new TypeError("ShadowRoot mode required");
    const localName = String(this.localName || "").toLowerCase();
    if (!localName.includes("-") && !shadowHostNames.has(localName))
      throw new DOMException(
        "Element cannot host a shadow tree",
        "NotSupportedError",
      );
    if (!shadowState) {
      if (!shadowFactory) {
        ensureShadowBootstrap();
        shadowFactory = __tilefinchRealmGlobal.__tilefinchCreateShadowState;
        delete __tilefinchRealmGlobal.__tilefinchCreateShadowState;
      }
      shadowState = shadowFactory(shadowMapType, shadowWeakRef, nodeReferenceDeref,
        ancestorApply, __tilefinchIsConnected, emptyTaskArguments,
        createPrivateSlot);
    }
    shadowModeByRoot ||= new shadowWeakMapType();
    shadowDelegatesFocusByRoot ||= new shadowWeakMapType();
    const detached = this.__handle === undefined,
      root = detached
        ? this.ownerDocument.createDocumentFragment()
        : wrap(__tilefinchCreate("div"));
    if (!root) throw new Error("ShadowRoot allocation failed");
    const detachedOperations = detached
      ? {
          appendChild: root.appendChild,
          insertBefore: root.insertBefore,
          removeChild: root.removeChild,
          replaceChild: root.replaceChild,
        }
      : null;
    const sharedNative = !detached && shadowState.prepare(
      root, ShadowRoot.prototype, nativeLayer,
      __tilefinchRealmGlobal.__tilefinchPrepareNativePrototype);
    Object.setPrototypeOf(root, ShadowRoot.prototype);
    if (detachedOperations)
      for (const [name, operation] of Object.entries(detachedOperations))
        Object.defineProperty(root, name, {
          configurable: true,
          writable: true,
          value(...args) {
            return operation.apply(this, args);
          },
        });
    if (!detached) {
      __tilefinchSetAttribute(root.__handle, "style", "display:contents");
      if (!__tilefinchAppend(this.__handle, root.__handle))
        throw new Error("ShadowRoot connection failed");
      let registered = registerNativeShadowRoot(root.__handle, false, sharedNative);
      if (!registered) {
        /* Disconnect reactions may already have weakened every eligible
           entry. Dead native slots still need sweeping in that case. */
        shadowState.weaken();
        registered = registerNativeShadowRoot(root.__handle, true, sharedNative);
      }
      if (!registered) {
        __tilefinchRemove(root.__handle);
        throw new DOMException(
          "Shadow root limit exceeded",
          "QuotaExceededError",
        );
      }
    }
    shadowState.remember(this, root);
    shadowRootCreated = true;
    shadowModeByRoot.set(root, mode);
    shadowDelegatesFocusByRoot.set(root, !!init?.delegatesFocus);
    const shadowDescriptors = sharedShadowDescriptors ||=
      Object.getOwnPropertyDescriptors({
        get namespaceURI() { return null; },
        get parentNode() { return null; },
        get parentElement() { return null; },
        get ownerDocument() {
          return shadowPeerFor(this)?.ownerDocument || null;
        },
        get isConnected() {
          return !!shadowPeerFor(this)?.isConnected;
        },
      });
    const rootDescriptors = { ...shadowDescriptors };
    if (!detached) {
      rootDescriptors.nodeType = {
        configurable: true,
        enumerable: true,
        get: () => Node.DOCUMENT_FRAGMENT_NODE,
      };
      rootDescriptors.nodeName = {
        configurable: true,
        enumerable: true,
        get: () => "#document-fragment",
      };
    } else {
      rootDescriptors.childNodes = {
        configurable: true,
        enumerable: true,
        get() {
          return nodeList(Array.from(this.__detachedChildren || []));
        },
      };
      rootDescriptors.children = {
        configurable: true,
        enumerable: true,
        get() {
          return nodeList(
            Array.from(this.__detachedChildren || []).filter(
              (node) => node?.nodeType === Node.ELEMENT_NODE,
            ),
          );
        },
      };
      rootDescriptors.firstChild = {
        configurable: true,
        enumerable: true,
        get() {
          return this.__detachedChildren?.[0] || null;
        },
      };
      rootDescriptors.lastChild = {
        configurable: true,
        enumerable: true,
        get() {
          const values = this.__detachedChildren || [];
          return values[values.length - 1] || null;
        },
      };
      rootDescriptors.querySelectorAll = {
        configurable: true,
        writable: true,
        value: function querySelectorAll(selector) {
          return detachedShadowQueryAll(this, selector);
        },
      };
      rootDescriptors.querySelector = {
        configurable: true,
        writable: true,
        value: function querySelector(selector) {
          return detachedShadowQueryAll(this, selector)[0] || null;
        },
      };
    }
    Object.defineProperties(root, rootDescriptors);
    installShadowHostView(this);
    return root;
  };
  Object.defineProperty(Element.prototype, "shadowRoot", Object.getOwnPropertyDescriptor({
    get shadowRoot() {
      const root = shadowRootForHost(this);
      return root && shadowModeByRoot.get(root) === "open" ? root : null;
    },
  }, "shadowRoot"));
  const adjacentPosition = (value) => {
    const position = String(value).toLowerCase();
    if (
      position !== "beforebegin" &&
      position !== "afterbegin" &&
      position !== "beforeend" &&
      position !== "afterend"
    )
      throw new DOMException(
        "Invalid insert-adjacent position",
        "SyntaxError",
      );
    return position;
  };
  Element.prototype.insertAdjacentElement = function insertAdjacentElement(where, element) {
    const position = adjacentPosition(where);
    if (!(element instanceof Element))
      throw new TypeError("insertAdjacentElement requires an Element");
    if (position === "beforebegin") {
      if (!this.parentNode) return null;
      this.parentNode.insertBefore(element, this);
    } else if (position === "afterbegin") {
      this.insertBefore(element, this.firstChild);
    } else if (position === "beforeend") {
      this.appendChild(element);
    } else {
      if (!this.parentNode) return null;
      this.parentNode.insertBefore(element, this.nextSibling);
    }
    return element;
  };
  Element.prototype.insertAdjacentText = function insertAdjacentText(where, data) {
    const position = adjacentPosition(where),
      text = factoryDocument(this).createTextNode(String(data));
    if (position === "beforebegin") {
      if (!this.parentNode) return;
      this.parentNode.insertBefore(text, this);
    } else if (position === "afterbegin") {
      this.insertBefore(text, this.firstChild);
    } else if (position === "beforeend") {
      this.appendChild(text);
    } else {
      if (!this.parentNode) return;
      this.parentNode.insertBefore(text, this.nextSibling);
    }
  };
  Element.prototype.moveBefore = function moveBefore(node, child) {
    if (!(node instanceof Node))
      throw new TypeError("moveBefore requires a Node");
    if (child !== null && child !== undefined && !(child instanceof Node))
      throw new TypeError("Reference child must be a Node or null");
    const preserved =
      !!node.isConnected &&
      !!this.isConnected &&
      node.ownerDocument === this.ownerDocument;
    if (preserved)
      __tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved =
        (__tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved || 0) + 1;
    try {
      this.insertBefore(node, child ?? null);
    } finally {
      if (preserved) __tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved--;
    }
    if (preserved) __tilefinchRealmGlobal.__tilefinchCustomElementMoved?.(node);
  };
  {
    const crossOrigin = {
        configurable: true,
        enumerable: true,
        get() {
          const value = this.getAttribute("crossorigin");
          if (value === null) return null;
          return String(value).toLowerCase() === "use-credentials"
            ? "use-credentials"
            : "anonymous";
        },
        set(value) {
          value === null
            ? this.removeAttribute("crossorigin")
            : this.setAttribute("crossorigin", String(value));
        },
      };
    Object.defineProperty(
      HTMLScriptElement.prototype,
      "crossOrigin",
      crossOrigin,
    );
    Object.defineProperty(
      HTMLScriptElement.prototype,
      "noModule",
      reflectBoolean("noModule"),
    );
    Object.defineProperty(
      HTMLLinkElement.prototype,
      "crossOrigin",
      crossOrigin,
    );
    /* HTMLOrSVGElement.nonce reflects the [[CryptographicNonce]] slot, which
       outlives the content attribute a header-delivered CSP hides. */
    const nonce = {
      configurable: true,
      enumerable: true,
      get() {
        return elementNonce(this.__handle);
      },
      set(value) {
        value = String(value);
        if (!elementNonce(this.__handle, value))
          this.setAttribute("nonce", value);
      },
    };
    Object.defineProperty(HTMLElement.prototype, "nonce", nonce);
    Object.defineProperty(SVGElement.prototype, "nonce", nonce);
    /* `draggable` is left out on purpose: `'draggable' in element` is the
       standard drag-and-drop feature test (Modernizr, Amazon's AUI) and
       Tilefinch has no drag-and-drop input, so exposing it would claim
       one. */
    const autofocus = reflectBoolean("autofocus"),
      loading = reflectEnumerated("loading", ["lazy", "eager"], "eager");
    Object.defineProperties(HTMLElement.prototype, {
      dir: reflectEnumerated("dir", ["ltr", "rtl", "auto"]),
      inputMode: reflectEnumerated("inputMode", [
        "none", "text", "tel", "url", "email", "numeric", "decimal", "search",
      ]),
      accessKey: reflectString("accessKey"),
      autofocus,
    });
    Object.defineProperty(SVGElement.prototype, "autofocus", autofocus);
    Object.defineProperty(HTMLImageElement.prototype, "loading", loading);
    Object.defineProperty(HTMLIFrameElement.prototype, "loading", loading);
    Object.defineProperty(HTMLScriptElement.prototype, "text", accessor("text", {
      get text() {
        /* The child text content: Text children only, not descendants. */
        let text = "";
        for (let child = this.firstChild; child; child = child.nextSibling)
          if (child.nodeType === Node.TEXT_NODE) text += child.data;
        return text;
      },
      set text(value) {
        this.textContent = String(value);
      },
    }));
    /* Script types Tilefinch runs. It has no import maps or speculation
       rules, so it says so rather than letting polyfills skip themselves. */
    Object.defineProperty(HTMLScriptElement, "supports", {
      configurable: true,
      enumerable: true,
      writable: true,
      value: { supports(type) {
        type = String(type);
        return type === "classic" || type === "module" || type === "importmap";
      } }.supports,
    });
  }
  Node.prototype.isSameNode = function isSameNode(other) {
    return this === other;
  };
  Node.prototype.isEqualNode = function isEqualNode(other) {
    if (
      other === null ||
      other === undefined ||
      this.nodeType !== other.nodeType ||
      this.nodeName !== other.nodeName
    )
      return false;
    if (this.nodeType === Node.TEXT_NODE || this.nodeType === Node.COMMENT_NODE)
      return this.data === other.data;
    if (this instanceof Element) {
      const left = this.attributes,
        right = other.attributes;
      if (!right || left.length !== right.length) return false;
      for (const attribute of left)
        if (other.getAttribute(attribute.name) !== attribute.value)
          return false;
    }
    const leftContainer =
        String(this.tagName).toLowerCase() === "template" ? this.content : this,
      rightContainer =
        String(other.tagName).toLowerCase() === "template"
          ? other.content
          : other;
    if (!leftContainer || !rightContainer) return false;
    const leftChildren = leftContainer.childNodes,
      rightChildren = rightContainer.childNodes;
    if (
      !leftChildren ||
      !rightChildren ||
      leftChildren.length !== rightChildren.length
    )
      return false;
    for (let i = 0; i < leftChildren.length; i++)
      if (!leftChildren[i].isEqualNode(rightChildren[i])) return false;
    return true;
  };
  {
    const nativeComparePosition = __tilefinchRealmGlobal.__tilefinchComparePosition;
    const disconnectedOrder = new WeakMap();
    let nextDisconnectedOrder = 1;
    const order = (node) => {
        let value = disconnectedOrder.get(node);
        if (value === undefined) {
          value = nextDisconnectedOrder++;
          disconnectedOrder.set(node, value);
        }
        return value;
      },
      path = (node) => {
        const values = [];
        if (node === document) return [document];
        for (let at = node; at && values.length < 512; at = at.parentNode)
          values.push(at);
        if (values[values.length - 1] === document.documentElement)
          values.push(document);
        values.reverse();
        return values;
      };
    Node.prototype.compareDocumentPosition = function compareDocumentPosition(other) {
      if (!(other instanceof Node)) throw new TypeError("Node required");
      if (this === other) return 0;
      if (typeof nativeComparePosition === "function"
          && !shadowRootCreated
          && !this.__tilefinchDetachedParent && !other.__tilefinchDetachedParent
          && this.__handle > 0 && other.__handle > 0) {
        const position = nativeComparePosition(this.__handle, other.__handle);
        if (position !== undefined) return position;
      }
      const left = path(this),
        right = path(other);
      if (left.length === 0 || right.length === 0 || left[0] !== right[0])
        return (
          Node.DOCUMENT_POSITION_DISCONNECTED |
          Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC |
          (order(other) < order(this)
            ? Node.DOCUMENT_POSITION_PRECEDING
            : Node.DOCUMENT_POSITION_FOLLOWING)
        );
      let at = 0;
      while (at < left.length && at < right.length && left[at] === right[at])
        at++;
      if (at === right.length)
        return (
          Node.DOCUMENT_POSITION_PRECEDING | Node.DOCUMENT_POSITION_CONTAINS
        );
      if (at === left.length)
        return (
          Node.DOCUMENT_POSITION_FOLLOWING | Node.DOCUMENT_POSITION_CONTAINED_BY
        );
      const siblings = left[at - 1].childNodes,
        leftIndex = Array.prototype.indexOf.call(siblings, left[at]),
        rightIndex = Array.prototype.indexOf.call(siblings, right[at]);
      return rightIndex >= 0 && rightIndex < leftIndex
        ? Node.DOCUMENT_POSITION_PRECEDING
        : Node.DOCUMENT_POSITION_FOLLOWING;
    };
  }
  const globalEventHandlerPrototypes = [
    Window.prototype,
    Document.prototype,
    HTMLElement.prototype,
    SVGElement.prototype,
  ];
  for (const type of [
    "abort",
    "animationcancel",
    "animationend",
    "animationiteration",
    "animationstart",
    "auxclick",
    "beforeinput",
    "beforetoggle",
    "blur",
    "cancel",
    "canplay",
    "change",
    "click",
    "close",
    "contextmenu",
    "compositionend",
    "compositionstart",
    "compositionupdate",
    "copy",
    "cut",
    "dblclick",
    "drag",
    "dragend",
    "dragenter",
    "dragleave",
    "dragover",
    "dragstart",
    "drop",
    "durationchange",
    "emptied",
    "ended",
    "error",
    "focus",
    "focusin",
    "focusout",
    "input",
    "invalid",
    "keydown",
    "keypress",
    "keyup",
    "loadeddata",
    "loadedmetadata",
    "loadstart",
    "load",
    "mousedown",
    "mouseenter",
    "mouseleave",
    "mousemove",
    "mouseout",
    "mouseover",
    "mouseup",
    "paste",
    "pause",
    "play",
    "playing",
    "pointerdown",
    "pointerenter",
    "pointerleave",
    "pointermove",
    "pointerout",
    "pointerover",
    "pointerup",
    "ratechange",
    "reset",
    "scroll",
    "select",
    "selectionchange",
    "seeking",
    "seeked",
    "stalled",
    "submit",
    "suspend",
    "touchcancel",
    "touchend",
    "touchmove",
    "touchstart",
    "timeupdate",
    "toggle",
    "transitioncancel",
    "transitionend",
    "transitionrun",
    "transitionstart",
    "visibilitychange",
    "volumechange",
    "waiting",
    "wheel",
  ]) {
    const name = "on" + type;
    /* Computed accessor keys name the functions "get onclick" and
       "set onclick" as WebIDL attributes are; literal accessors are already
       enumerable and configurable. */
    const descriptor = Object.getOwnPropertyDescriptor({
        get [name]() {
          return inlineEventHandler(this, type, name);
        },
        set [name](value) {
          const callable = typeof value === "function" ? value : null,
            retained = eventHandlerMap(this, false),
            hasMarkup = typeof this?.getAttribute === "function" &&
              this.getAttribute(name) !== null,
            map = eventHandlerMap(
              this,
              callable !== null || hasMarkup || !!retained,
            );
          if (!map) return;
          installEventHandler(this, type, callable);
        },
      }, name);
    for (const prototype of globalEventHandlerPrototypes)
      if (!(name in prototype))
        Object.defineProperty(prototype, name, descriptor);
  }
  Object.defineProperty(NodeList.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "NodeList",
  });
  const nodeList = (values) => {
    const numericLength = numberFrom(values?.length),
      length = numberIsFinite(numericLength) && numericLength > 0
        ? mathMin(mathFloor(numericLength), 0xffffffff)
        : 0;
    /* The node-handle index space; native results never exceed it, since
       a walk that runs out of handles throws instead. */
    if (length > 16384)
      throw new RangeError("NodeList limit exceeded");
    const result = Object.create(NodeList.prototype);
    for (let index = 0; index < length; index++)
      Object.defineProperty(result, index, {
        configurable: true,
        enumerable: true,
        value: values[index],
      });
    Object.defineProperty(result, "length", {
      configurable: true,
      value: length,
    });
    return result;
  };
  const containingShadowRoot = (node) => {
      for (
        let at = node, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentNode, steps++
      )
        if (at instanceof ShadowRoot) return at;
      return null;
    },
    slotsForRoot = (root) => {
      let slots = attributeState(root)?.slots;
      if (slots) return slots;
      slots = Array.from(root?.querySelectorAll?.("slot") || [])
        .filter(
          (slot) =>
            slot instanceof HTMLSlotElement &&
            containingShadowRoot(slot) === root,
        )
        .slice(0, 128);
      /* Slots retain their parent/root. The cache belongs on that root so
         discarded component trees are not held by a global weak-key value. */
      attributeState(root, true).slots = slots;
      return slots;
    },
    assignedSlotInternal = (node, exposeClosed = false) => {
      const host = node?.parentNode;
      if (!(host instanceof Element)) return null;
      const root = shadowRootForHost(host);
      if (
        !root ||
        (!exposeClosed && shadowModeByRoot.get(root) === "closed")
      )
        return null;
      const name =
        node.nodeType === Node.ELEMENT_NODE
          ? String(node.getAttribute("slot") || "")
          : "";
      for (const slot of slotsForRoot(root))
        if (String(slot.getAttribute("name") || "") === name) return slot;
      return null;
    },
    shadowEventPath = (target, composed) => {
      /* Without a shadow root there is no slot, boundary or origin root to
         compare against: the plain parent chain. */
      const originRoot = shadowRootCreated
          ? target?.getRootNode?.() || null
          : null,
        documentElement = document.documentElement;
      return boundedAncestorPath(target, (node) => {
        if (node instanceof Document) return null;
        if (node === documentElement) return document;
        const slot = shadowRootCreated && assignedSlotInternal(node, true);
        if (slot) return slot;
        if (node instanceof ShadowRoot)
          /*
           * A slotted light-DOM target does not originate inside the shadow
           * root which contributes the slot to its flattened path. A
           * non-composed event therefore continues through that host; only an
           * event whose actual tree root is this shadow root stops here.
           */
          return composed || originRoot !== node
            ? shadowPeerFor(node)
            : null;
        return (
          node.__tilefinchDetachedParent ||
          node.parentNode ||
          node.parentElement
        );
      });
    },
    nodeInsideShadowRoot = (node, root) => {
      for (let at = node, steps = 0; at && steps < ancestorLimit; steps++) {
        if (at === root) return true;
        const candidate = at.getRootNode?.() || null;
        if (!(candidate instanceof ShadowRoot)) return false;
        if (candidate === root) return true;
        at = shadowPeerFor(candidate);
      }
      return false;
    },
    visibleShadowEventPath = (path, currentTarget) => {
      if (!currentTarget) return [];
      const closedRoots = [];
      for (const node of path)
        if (
          node instanceof ShadowRoot &&
          shadowModeByRoot.get(node) === "closed"
        )
          closedRoots.push(node);
      if (!closedRoots.length) return path.slice();
      return path.filter((node) => {
        for (const root of closedRoots)
          if (
            nodeInsideShadowRoot(node, root) &&
            !nodeInsideShadowRoot(currentTarget, root)
          )
            return false;
        return true;
      });
    },
    retargetShadowEvent = (original, current) => {
      /* Retargeting only ever crosses a shadow root. */
      if (!shadowRootCreated) return original;
      let target = original;
      for (let steps = 0; target && steps < ancestorLimit; steps++) {
        const root = target.getRootNode?.() || null;
        if (!(root instanceof ShadowRoot)) return target;
        if (nodeInsideShadowRoot(current, root))
          return target;
        target = shadowPeerFor(root);
      }
      return target || original;
    },
    assignedNodesInternal = (slot, flatten, seen = new Set()) => {
      if (!(slot instanceof HTMLSlotElement) || seen.has(slot)) return [];
      const root = containingShadowRoot(slot);
      if (!root) return [];
      const slots = slotsForRoot(root),
        name = String(slot.getAttribute("name") || "");
      if (slots.find((candidate) =>
        String(candidate.getAttribute("name") || "") === name
      ) !== slot)
        return [];
      const host = shadowPeerFor(root),
        direct = host
          ? shadowLightChildren(host).filter(
              (node) =>
                (node.nodeType === Node.ELEMENT_NODE ||
                  node.nodeType === Node.TEXT_NODE) &&
                assignedSlotInternal(node, true) === slot,
            )
          : [];
      if (!flatten) return direct;
      seen.add(slot);
      const flattened = [];
      for (const node of direct) {
        if (node instanceof HTMLSlotElement)
          flattened.push(...assignedNodesInternal(node, true, seen));
        else flattened.push(node);
        if (flattened.length >= 256) break;
      }
      seen.delete(slot);
      if (flattened.length) return flattened.slice(0, 256);
      for (const child of slot.childNodes) {
        if (child instanceof HTMLSlotElement)
          flattened.push(...assignedNodesInternal(child, true, seen));
        else flattened.push(child);
        if (flattened.length >= 256) break;
      }
      return flattened.slice(0, 256);
    },
    removedSignalledSlotsByRoot = new WeakMap(),
    pendingSlotRoots = new Set(),
    resignalSlotRoots = new Set(),
    forcedSlotRoots = new Set(),
    preObserverSlotMutationRoots = new Set();
  let slotFlushScheduled = false, shadowReleaseScheduled = false;
  const releaseDisconnectedShadowRoots = () => {
    shadowReleaseScheduled = false;
    shadowState?.weaken();
  };
  const shadowRootDepth = (root) => {
      let depth = 0,
        at = root;
      for (let steps = 0; at && steps < ancestorLimit; steps++) {
        const host = shadowPeerFor(at);
        if (!host) break;
        at = containingShadowRoot(host);
        if (at) depth++;
      }
      return depth;
    },
    flushShadowSlotChanges = (afterMutationDelivery = false) => {
      /*
       * Mutation records precede slotchange in a compound microtask.  If an
       * independently queued slot job reaches the checkpoint first, leave
       * the roots signalled; the mutation-delivery epilogue below performs
       * this flush before the next observer compound microtask.
       */
      if (
        !afterMutationDelivery &&
        mutationDeliveryPending &&
        !mutationDeliveryActive
      )
        return;
      slotFlushScheduled = false;
      const roots = Array.from(pendingSlotRoots);
      pendingSlotRoots.clear();
      /*
       * Slot assignment propagates from an inner tree to the slots that
       * consume its host.  Notify inner roots first so a listener never
       * observes an outer assignment while the nested slot still reports its
       * previous state.
       */
      roots.sort((left, right) => shadowRootDepth(right) - shadowRootDepth(left));
      for (const root of roots) {
        preObserverSlotMutationRoots.delete(root);
        const forced = forcedSlotRoots.delete(root);
        for (const slot of slotsForRoot(root)) {
          const current = assignedNodesInternal(slot, false),
            previous = attributeState(slot)?.slotSnapshot || [],
            changed =
              current.length !== previous.length ||
              current.some((node, index) => node !== previous[index]);
          attributeState(slot, true).slotSnapshot = current.slice();
          if (changed || forced)
            slot.dispatchEvent(
              new Event("slotchange", { bubbles: true }),
            );
        }
        for (const { slot, assignment: current } of
          removedSignalledSlotsByRoot.get(root) || []) {
          const previous = attributeState(slot)?.slotSnapshot || [],
            changed =
              current.length !== previous.length ||
              current.some((node, index) => node !== previous[index]);
          attributeState(slot, true).slotSnapshot = current.slice();
          if (changed || forced)
            slot.dispatchEvent(
              new Event("slotchange", { bubbles: true }),
            );
        }
        removedSignalledSlotsByRoot.delete(root);
      }
      if (resignalSlotRoots.size) {
        for (const root of resignalSlotRoots) {
          pendingSlotRoots.add(root);
          forcedSlotRoots.add(root);
        }
        resignalSlotRoots.clear();
        slotFlushScheduled = true;
        queueMicrotask(flushShadowSlotChanges);
      }
    },
    scheduleShadowSlotChange = (
      target,
      removedNodes = [],
      mutationType = "",
    ) => {
      /* Ordinary pages should not pay an ancestor walk for a facility which
         has never been used in this page realm. */
      if (!shadowRootCreated) return;
      /* Plain shadow hosts have no custom-element disconnect reaction.
         Batch the bounded identity-cache check after removals so they do not
         remain strong roots until the native root table happens to fill. */
      if (mutationType === "childList" && removedNodes.length &&
          !shadowReleaseScheduled) {
        queueMicrotask(releaseDisconnectedShadowRoots);
        shadowReleaseScheduled = true;
      }
      let root = null;
      const containing =
        target instanceof ShadowRoot ? target : containingShadowRoot(target);
      if (containing) root = containing;
      else {
        root =
          shadowRootForHost(target) ||
          shadowRootForHost(target?.parentNode);
      }
      if (!root) return;
      /* Structural mutations inside a shadow tree can change its slot set.
         Invalidate once, then share one bounded query across all assignment
         work for this mutation instead of re-querying for every slot. */
      if (containing && mutationType === "childList") {
        const state = attributeState(root);
        if (state) state.slots = null;
      }
      const slots = slotsForRoot(root),
        removedSlots = removedNodes.filter(
        (node) => node instanceof HTMLSlotElement,
      );
      /* Most component trees contain no slots. Retaining and sorting their
         roots until a compound microtask cannot produce a slotchange. */
      if (!slots.length && !removedSlots.length) return;
      for (const slot of slots)
        attributeState(slot, true).pendingSlotAssignment =
          assignedNodesInternal(slot, false);
      if (removedSlots.length) {
        const retained =
          removedSignalledSlotsByRoot.get(root) || [];
        for (const slot of removedSlots)
          if (retained.length < 128)
            retained.push({
              slot,
              assignment: attributeState(slot)?.pendingSlotAssignment || [],
            });
        removedSignalledSlotsByRoot.set(root, retained);
      }
      const priorSignal =
        pendingSlotRoots.has(root) ||
        preObserverSlotMutationRoots.has(root);
      if (mutationDeliveryActive && priorSignal)
        resignalSlotRoots.add(root);
      if (pendingSlotRoots.has(root)) {
        return;
      }
      /*
       * A component library can construct several independent shadow hosts in
       * one task.  The WPT slotchange matrix reaches 40 live roots before its
       * compound microtask; 64 remains a small, reference-only bound while
       * avoiding silent loss at the former 32-root ceiling.
       */
      if (pendingSlotRoots.size >= 64) return;
      pendingSlotRoots.add(root);
      if (!mutationDeliveryActive)
        preObserverSlotMutationRoots.add(root);
      if (!slotFlushScheduled) {
        slotFlushScheduled = true;
        queueMicrotask(flushShadowSlotChanges);
      }
    };
  Object.defineProperties(__tilefinchRealmGlobal, {
    __tilefinchShadowEventPath: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: shadowEventPath,
    },
    __tilefinchRetargetShadowEvent: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: retargetShadowEvent,
    },
    __tilefinchVisibleShadowEventPath: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: visibleShadowEventPath,
    },
    __tilefinchShadowRootForHost: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: shadowRootForHost,
    },
    /* Whether attachShadow has created any root on this page. */
    __tilefinchShadowRootsExist: {
      configurable: false,
      enumerable: false,
      writable: false,
      value: () => shadowRootCreated,
    },
  });
  Object.defineProperties(Element.prototype, {
    slot: {
      configurable: true,
      enumerable: true,
      get() {
        return this.getAttribute("slot") || "";
      },
      set(value) {
        this.setAttribute("slot", String(value));
      },
    },
    assignedSlot: {
      configurable: true,
      enumerable: true,
      get() {
        return assignedSlotInternal(this);
      },
    },
  });
  Object.defineProperty(Text.prototype, "assignedSlot", Object.getOwnPropertyDescriptor({
    get assignedSlot() {
      return assignedSlotInternal(this);
    },
  }, "assignedSlot"));
  Object.defineProperties(HTMLSlotElement.prototype, {
    name: {
      configurable: true,
      enumerable: true,
      get() {
        return this.getAttribute("name") || "";
      },
      set(value) {
        this.setAttribute("name", String(value));
      },
    },
    assignedNodes: {
      configurable: true,
      writable: true,
      /* sequence<Node> in WebIDL: a fresh Array, not a NodeList
         (MDN's dropdown calls .find on assignedElements()). */
      value: function assignedNodes(options = {}) {
        return assignedNodesInternal(this, !!Object(options).flatten);
      },
    },
    assignedElements: {
      configurable: true,
      writable: true,
      value: function assignedElements(options = {}) {
        return assignedNodesInternal(this, !!Object(options).flatten).filter(
          (node) => node.nodeType === Node.ELEMENT_NODE,
        );
      },
    },
  });
  Object.defineProperty(ShadowRoot.prototype, "getElementById", {
    configurable: true,
    writable: true,
    value: function getElementById(id) {
      return this.querySelector("#" + CSS.escape(String(id)));
    },
  });
  const elementsByClassName = (root, names) => {
    const tokens = String(names).trim().split(/\s+/).filter(Boolean);
    return tokens.length === 0
      ? nodeList([])
      : root.querySelectorAll(
          tokens.map((token) => "." + CSS.escape(token)).join(""),
        );
  };
  const elementsByTagName = (root, tag) => {
    tag = String(tag).trim();
    if (!tag) return nodeList([]);
    if (tag !== "*" && !/^[a-z][\w-]*$/i.test(tag))
      return nodeList(
        Array.from(root.querySelectorAll("*")).filter(
          (node) => String(node.localName).toLowerCase() === tag.toLowerCase(),
        ),
      );
    return root.querySelectorAll(tag);
  };
  /* A document has no parent or siblings and no value; these are null, not
     absent. chatgpt.com's frame runtime walks parentNode until null. */
  const documentNull = { configurable: true, enumerable: true, get: () => null };
  const documentIgnoredValue = {
    configurable: true,
    enumerable: true,
    get: () => null,
    set: () => {},
  };
  Object.defineProperties(Document.prototype, {
    firstElementChild: {
      configurable: true, enumerable: true,
      get() { return this.documentElement || null; },
    },
    parentNode: documentNull,
    parentElement: documentNull,
    previousSibling: documentNull,
    nextSibling: documentNull,
    nodeValue: documentIgnoredValue,
    textContent: documentIgnoredValue,
    querySelector: {
      value: __tilefinchDocumentQuerySelector,
      writable: true,
      configurable: true,
    },
    querySelectorAll: {
      value: function querySelectorAll(selector) {
        return nodeList(__tilefinchDocumentQuerySelectorAll.call(this, selector));
      },
      writable: true,
      configurable: true,
    },
    getElementById: {
      value: __tilefinchDocumentGetElementById,
      writable: true,
      configurable: true,
    },
    getElementsByClassName: {
      value: function getElementsByClassName(names) {
        return elementsByClassName(this, names);
      },
      writable: true,
      configurable: true,
    },
    getElementsByTagName: {
      value: function getElementsByTagName(tag) {
        return elementsByTagName(this, tag);
      },
      writable: true,
      configurable: true,
    },
    getElementsByName: {
      value: function getElementsByName(name) {
        name = String(name);
        return nodeList(
          Array.from(this.querySelectorAll("[name]")).filter(
            (element) => element.getAttribute("name") === name,
          ),
        );
      },
      writable: true,
      configurable: true,
    },
  });
  Object.defineProperties(DocumentFragment.prototype, {
    querySelector: {
      value: __tilefinchDocumentFragmentQuerySelector,
      writable: true,
      configurable: true,
    },
    querySelectorAll: {
      value: function querySelectorAll(selector) {
        return nodeList(
          __tilefinchDocumentFragmentQuerySelectorAll.call(this, selector),
        );
      },
      writable: true,
      configurable: true,
    },
  });
  Object.defineProperties(Element.prototype, {
    querySelector: {
      value: __tilefinchElementQuerySelector,
      writable: true,
      configurable: true,
    },
    querySelectorAll: {
      value: function querySelectorAll(selector) {
        return nodeList(__tilefinchElementQuerySelectorAll.call(this, selector));
      },
      writable: true,
      configurable: true,
    },
    getElementsByClassName: {
      value: function getElementsByClassName(names) {
        return elementsByClassName(this, names);
      },
      writable: true,
      configurable: true,
    },
    getElementsByTagName: {
      value: function getElementsByTagName(tag) {
        return elementsByTagName(this, tag);
      },
      writable: true,
      configurable: true,
    },
    matches: {
      value: function matches(selector) {
        selector = String(selector);
        const compact = selector.replace(/\s+/g, "").toLowerCase();
        if (
          compact === ":defined" ||
          compact === ":not(:defined)"
        ) {
          const defined =
            __tilefinchRealmGlobal.__tilefinchCustomElementIsDefined?.(this) ?? true;
          return compact === ":defined" ? defined : !defined;
        }
        return __tilefinchMatches(this.__handle, selector);
      },
      writable: true,
      configurable: true,
    },
    closest: {
      value: function closest(selector) {
        for (
          let at = this, steps = 0;
          at && steps < ancestorLimit;
          at = at.parentElement, steps++
        )
          if (at.matches(selector)) return at;
        return null;
      },
      writable: true,
      configurable: true,
    },
  });
  /* Whether a form-associated custom element is disabled (its own
     attribute or a disabled fieldset ancestor), whatever its class's own
     disabled accessor says; null for any other element. */
  const formAssociatedCustomDisabled = (element) => {
    const internals =
      __tilefinchRealmGlobal.__tilefinchElementInternalsFor?.(element);
    if (
      !__tilefinchRealmGlobal.__tilefinchFormAssociatedCustomElement?.(element)
    )
      return null;
    if (internals)
      try {
        void internals.form;
      } catch (_) {
        return null;
      }
    if (element.hasAttribute("disabled")) return true;
    for (
      let at = element.parentElement, steps = 0;
      at && steps < ancestorLimit;
      at = at.parentElement, steps++
    )
      if (
        String(at.localName || "").toLowerCase() === "fieldset" &&
        at.hasAttribute("disabled")
      )
        return true;
    return false;
  };
  /* For platform.js (:disabled, :enabled) and compat.js (focus), which
     deletes it. */
  __tilefinchRealmGlobal.__tilefinchFormAssociatedCustomDisabled =
    formAssociatedCustomDisabled;
  HTMLElement.prototype.click = function click() {
    if (this.disabled || formAssociatedCustomDisabled(this)) {
      __tilefinchRealmGlobal.__tilefinchLastClickDefault = "disabled";
      return false;
    }
    const inputReset =
        this instanceof HTMLInputElement &&
        String(this.getAttribute("type") || "text").toLowerCase() === "reset",
      controlState = inputReset
        ? null
        : __tilefinchRealmGlobal.__tilefinchBeginControlDefault?.(this) || null,
      clickEvent = new PointerEvent("click", {
          bubbles: true,
          cancelable: true,
          composed: true,
          button: 0,
          buttons: 0,
          detail: 0,
          pointerId: -1,
          pointerType: "",
          isPrimary: false,
        });
    clickEvent.__tilefinchControlDefaultPrepared = true;
    const proceed = this.dispatchEvent(clickEvent),
      controlHandled =
        __tilefinchRealmGlobal.__tilefinchFinishControlDefault?.(controlState, proceed) ||
        false;
    if (!proceed) {
      __tilefinchRealmGlobal.__tilefinchLastClickDefault = "cancelled";
      return false;
    }
    if (controlHandled) __tilefinchRealmGlobal.__tilefinchLastClickDefault = "control";
    else if (
      this instanceof HTMLInputElement &&
      String(this.getAttribute("type") || "text").toLowerCase() === "reset"
    ) {
      const form = this.closest("form");
      __tilefinchRealmGlobal.__tilefinchLastClickDefault =
        form instanceof HTMLFormElement ? "reset" : "no-form";
      if (form instanceof HTMLFormElement)
        __tilefinchRealmGlobal.__tilefinchResetFormFromActivation(form);
    } else if (
      this instanceof HTMLInputElement &&
      String(this.getAttribute("type") || "text").toLowerCase() === "submit"
    ) {
      const form = this.closest("form");
      __tilefinchRealmGlobal.__tilefinchLastClickDefault =
        form instanceof HTMLFormElement ? "submit" : "no-form";
      if (form instanceof HTMLFormElement) form.requestSubmit(this);
    }
    else if (
      this instanceof HTMLButtonElement &&
      String(this.getAttribute("type") || "submit").toLowerCase() === "reset"
    ) {
      const form = this.closest("form");
      __tilefinchRealmGlobal.__tilefinchLastClickDefault =
        form instanceof HTMLFormElement ? "reset" : "no-form";
      if (form instanceof HTMLFormElement) form.reset();
    } else if (
      this instanceof HTMLButtonElement &&
      String(this.getAttribute("type") || "submit").toLowerCase() === "submit"
    ) {
      const form = this.closest("form");
      __tilefinchRealmGlobal.__tilefinchLastClickDefault =
        form instanceof HTMLFormElement ? "submit" : "no-form";
      if (form instanceof HTMLFormElement) form.requestSubmit(this);
    } else if (__tilefinchRealmGlobal.__tilefinchDetailsDefault?.(this))
      __tilefinchRealmGlobal.__tilefinchLastClickDefault = "details";
    else if (__tilefinchRealmGlobal.__tilefinchLabelDefault?.(this, false))
      __tilefinchRealmGlobal.__tilefinchLastClickDefault = "label";
    else __tilefinchRealmGlobal.__tilefinchLastClickDefault = "none";
    return true;
  };
  Element.prototype.insertAdjacentHTML = function insertAdjacentHTML(position, source) {
    position = String(position).toLowerCase();
    if (!["beforebegin", "afterbegin", "beforeend", "afterend"].includes(position))
      throw new DOMException(
        "Invalid adjacent HTML position",
        "SyntaxError",
      );
    if (
      (position === "beforebegin" || position === "afterend") &&
      !this.parentNode
    )
      return;
    const template = factoryDocument(this).createElement("template");
    template.innerHTML = String(source);
    const fragment = factoryDocument(this).createDocumentFragment();
    for (const child of [...template.content.childNodes])
      fragment.appendChild(child);
    if (position === "beforebegin")
      this.parentNode.insertBefore(fragment, this);
    else if (position === "afterbegin")
      this.insertBefore(fragment, this.firstChild);
    else if (position === "beforeend")
      this.appendChild(fragment);
    else
      this.parentNode.insertBefore(fragment, this.nextSibling);
  };
  /* The HTML setters. chatgpt.com's unauthenticated composer refuses to
     initialize without them and otherwise fetches a polyfill first.
     Declarative shadow roots are not parsed, so this is innerHTML. */
  Element.prototype.setHTMLUnsafe = function setHTMLUnsafe(html) {
    this.innerHTML = String(html);
  };
  if (typeof ShadowRoot === "function")
    ShadowRoot.prototype.setHTMLUnsafe = function setHTMLUnsafe(html) {
      this.innerHTML = String(html);
    };
  if (typeof Document === "function")
    Document.parseHTMLUnsafe = function (html) {
      return new DOMParser().parseFromString(String(html), "text/html");
    };
  /* Web Animations introspection over the animations Element.animate
     started. chatgpt.com calls element.getAnimations().some(...) without a
     feature test while positioning its composer. Stylesheet transitions are
     not listed. */
  /* The animation an effect belongs to supplies its local time; chatgpt.com's
     streaming renderer reads effect.getComputedTiming().endTime to pace its
     word reveal. */
  const effectClocks = new WeakMap();
  __tilefinchRealmGlobal.__tilefinchBindEffectClock = (effect, clock) => {
    effectClocks.set(effect, clock);
  };
  const timingNumber = (value, fallback) => {
    const number = Number(value);
    return Number.isFinite(number) && number >= 0 ? number : fallback;
  };
  __tilefinchRealmGlobal.KeyframeEffect = class KeyframeEffect {
    #target;
    #keyframes;
    #timing;
    constructor(target, keyframes, options) {
      this.#target = target ?? null;
      this.#keyframes = Array.isArray(keyframes)
        ? keyframes.slice(0, 16).map((frame) => ({ ...frame }))
        : keyframes && typeof keyframes === "object"
          ? [{ ...keyframes }]
          : [];
      const timing =
        typeof options === "number"
          ? { duration: options }
          : options && typeof options === "object"
            ? options
            : {};
      const easing = __tilefinchRealmGlobal.__tilefinchTimingFunction(
        timing.easing === undefined ? "linear" : String(timing.easing),
      );
      if (easing === null) throw new TypeError("Invalid easing");
      this.#timing = {
        delay: timingNumber(timing.delay, 0),
        endDelay: timingNumber(timing.endDelay, 0),
        fill: ["none", "forwards", "backwards", "both", "auto"].includes(
          String(timing.fill),
        )
          ? String(timing.fill)
          : "auto",
        iterationStart: timingNumber(timing.iterationStart, 0),
        iterations: timingNumber(timing.iterations, 1),
        duration: timingNumber(timing.duration, 0),
        direction: [
          "normal",
          "reverse",
          "alternate",
          "alternate-reverse",
        ].includes(String(timing.direction))
          ? String(timing.direction)
          : "normal",
        easing,
      };
    }
    get target() {
      return this.#target;
    }
    getKeyframes() {
      return this.#keyframes.map((frame) => ({ ...frame }));
    }
    getTiming() {
      return { ...this.#timing };
    }
    getComputedTiming() {
      const timing = this.#timing,
        activeDuration = timing.duration * timing.iterations,
        endTime = Math.max(
          0,
          timing.delay + activeDuration + timing.endDelay,
        ),
        clock = effectClocks.get(this),
        localTime = clock ? clock() : null;
      let progress = null,
        currentIteration = null;
      if (
        typeof localTime === "number" &&
        localTime >= timing.delay &&
        localTime < timing.delay + activeDuration &&
        timing.duration > 0
      ) {
        const elapsed = localTime - timing.delay;
        currentIteration = Math.floor(elapsed / timing.duration);
        progress = (elapsed % timing.duration) / timing.duration;
      }
      return {
        ...timing,
        activeDuration,
        endTime,
        localTime,
        progress,
        currentIteration,
      };
    }
  };
  Element.prototype.getAnimations = function getAnimations() {
    return __tilefinchRealmGlobal.__tilefinchElementAnimations?.(this) || [];
  };
  if (typeof Document === "function")
    Document.prototype.getAnimations = function getAnimations() {
      return __tilefinchRealmGlobal.__tilefinchElementAnimations?.(null) || [];
    };
  __tilefinchRealmGlobal.reportError = function reportError(error) {
    if (arguments.length < 1)
      throw new TypeError("reportError requires an argument");
    __tilefinchRealmGlobal.__tilefinchReportUncaught?.(error, "reportError");
  };
  for (const [name, attribute] of [["inert", "inert"]])
    Object.defineProperty(HTMLElement.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        return this.hasAttribute(attribute);
      },
      set(value) {
        this.toggleAttribute(attribute, !!value);
      },
    });
  const uncachedElementPrototype =(tag, nodeType, namespaceURI) => {
    if (nodeType === Node.TEXT_NODE) return Text.prototype;
    if (nodeType === Node.CDATA_SECTION_NODE) return CDATASection.prototype;
    if (nodeType === Node.PROCESSING_INSTRUCTION_NODE)
      return ProcessingInstruction.prototype;
    if (nodeType === Node.COMMENT_NODE) return Comment.prototype;
    if (nodeType === Node.DOCUMENT_TYPE_NODE) return DocumentType.prototype;
    if (nodeType === Node.DOCUMENT_FRAGMENT_NODE)
      return DocumentFragment.prototype;
    if (namespaceURI === "http://www.w3.org/2000/svg")
      return SVGElement.prototype;
    if (
      nodeType === Node.ELEMENT_NODE &&
      namespaceURI !== undefined &&
      namespaceURI !== "http://www.w3.org/1999/xhtml"
    )
      return Element.prototype;
    switch (String(tag).toLowerCase()) {
      case "":
        return Node.prototype;
      case "div":
        return HTMLDivElement.prototype;
      case "body":
        return HTMLBodyElement.prototype;
      case "frameset":
        return HTMLFrameSetElement.prototype;
      case "style":
        return HTMLStyleElement.prototype;
      case "iframe":
        return HTMLIFrameElement.prototype;
      case "input":
        return HTMLInputElement.prototype;
      case "script":
        return HTMLScriptElement.prototype;
      case "form":
        return HTMLFormElement.prototype;
      case "button":
        return HTMLButtonElement.prototype;
      case "textarea":
        return HTMLTextAreaElement.prototype;
      case "select":
        return HTMLSelectElement.prototype;
      case "option":
        return HTMLOptionElement.prototype;
      case "selectedcontent":
        return HTMLSelectedContentElement.prototype;
      case "label":
        return HTMLLabelElement.prototype;
      case "fieldset":
        return HTMLFieldSetElement.prototype;
      case "legend":
        return HTMLLegendElement.prototype;
      case "output":
        return HTMLOutputElement.prototype;
      case "a":
        return HTMLAnchorElement.prototype;
      case "link":
        return HTMLLinkElement.prototype;
      case "meta":
        return HTMLMetaElement.prototype;
      case "template":
        return HTMLTemplateElement.prototype;
      case "dialog":
        return HTMLDialogElement.prototype;
      case "details":
        return HTMLDetailsElement.prototype;
      case "summary":
        return HTMLSummaryElement.prototype;
      case "img":
        return HTMLImageElement.prototype;
      case "canvas":
        return HTMLCanvasElement.prototype;
      case "audio":
        return HTMLAudioElement.prototype;
      case "video":
        return HTMLVideoElement.prototype;
      case "source":
        return HTMLSourceElement.prototype;
      case "slot":
        return HTMLSlotElement.prototype;
      default: {
        const lower = String(tag).toLowerCase();
        const at = sharedLayerTags.indexOf(" " + lower + "=");
        return at >= 0
          ? sharedLayerPrototypes[
              Number.parseInt(sharedLayerTags.slice(at + lower.length + 2))]
          : unknownElementTag(lower)
            ? HTMLUnknownElement.prototype
            : HTMLElement.prototype;
      }
    }
  };
  /* Every wrapper creation asks; an HTML element's answer depends only on
     its tag name string, so remember it instead of lowercasing and running
     the switch again. */
  const htmlPrototypeByTag = new Map();
  const elementPrototype = (tag, nodeType, namespaceURI) => {
    if (nodeType !== Node.ELEMENT_NODE || typeof tag !== "string" ||
        namespaceURI !== "http://www.w3.org/1999/xhtml")
      return uncachedElementPrototype(tag, nodeType, namespaceURI);
    let prototype = htmlPrototypeByTag.get(tag);
    if (!prototype) {
      prototype = uncachedElementPrototype(tag, nodeType, namespaceURI);
      if (htmlPrototypeByTag.size < 256) htmlPrototypeByTag.set(tag, prototype);
    }
    return prototype;
  };
  const classListOwner = Symbol("classList owner");
  const classListNode = (list) => {
    const node = list?.[classListOwner];
    if (!node) throw new TypeError("Invalid classList receiver");
    return node;
  };
  const classListTokens = (list) =>
        [
          ...new Set(
            String(classListNode(list).getAttribute("class") || "")
              .split(/\s+/)
              .filter(Boolean),
          ),
        ];
  const validateClassToken = (token) => {
        token = String(token);
        if (!token)
          throw new DOMException("Token must not be empty", "SyntaxError");
        if (/[\t\n\f\r ]/.test(token))
          throw new DOMException(
            "Token must not contain ASCII whitespace",
            "InvalidCharacterError",
          );
        return token;
      };
  const writeClassTokens = (list, values) =>
    classListNode(list).setAttribute("class", values.join(" "));
  const classListDescriptors = Object.getOwnPropertyDescriptors({
      contains(token) {
        token = String(token);
        return token !== "" &&
          String(classListNode(this).getAttribute("class") || "")
            .split(/\s+/)
            .includes(token);
      },
      add(...values) {
        values = values.map(validateClassToken);
        const set = new Set(classListTokens(this));
        for (const value of values) set.add(value);
        writeClassTokens(this, [...set]);
      },
      remove(...values) {
        const removed = new Set(values.map(validateClassToken));
        const before = classListTokens(this),
          after = before.filter((value) => !removed.has(value));
        if (classListNode(this).getAttribute("class") !== null)
          writeClassTokens(this, after);
      },
      toggle(token, force) {
        token = validateClassToken(token);
        /* One parse: the same writes add() and remove() would make. */
        const tokens = classListTokens(this),
          present = tokens.includes(token),
          next = force === undefined ? !present : !!force;
        if (next && !present) {
          tokens.push(token);
          writeClassTokens(this, tokens);
        } else if (!next && present)
          writeClassTokens(this, tokens.filter((value) => value !== token));
        return next;
      },
      replace(oldToken, newToken) {
        oldToken = String(oldToken);
        newToken = String(newToken);
        if (!oldToken || !newToken)
          throw new DOMException("Token must not be empty", "SyntaxError");
        if (
          /[\t\n\f\r ]/.test(oldToken) ||
          /[\t\n\f\r ]/.test(newToken)
        )
          throw new DOMException(
            "Token must not contain ASCII whitespace",
            "InvalidCharacterError",
          );
        const values = classListTokens(this),
          at = values.indexOf(oldToken);
        if (at < 0) return false;
        if (oldToken === newToken) {
          writeClassTokens(this, values);
          return true;
        }
        values[at] = newToken;
        writeClassTokens(this, [...new Set(values)]);
        return true;
      },
      item(index) {
        return classListTokens(this)[Number(index)] ?? null;
      },
      get length() {
        return classListTokens(this).length;
      },
      get value() {
        return classListNode(this).getAttribute("class") || "";
      },
      set value(value) {
        classListNode(this).setAttribute("class", String(value));
      },
      values() {
        return classListTokens(this)[Symbol.iterator]();
      },
      keys() {
        return classListTokens(this)
          .map((_, index) => index)
          [Symbol.iterator]();
      },
      entries() {
        return classListTokens(this).entries();
      },
      forEach(callback, thisArg) {
        classListTokens(this).forEach((value, index) =>
          callback.call(thisArg, value, index, this),
        );
      },
      [Symbol.iterator]() {
        return this.values();
      },
      toString() {
        return this.value;
      },
      supports() {
        throw new TypeError("classList does not define supported tokens");
      },
    });
  const classListProxyHandler = {
      get(target, key, receiver) {
        if (typeof key === "string" && /^\d+$/.test(key))
          return classListTokens(target)[Number(key)];
        return Reflect.get(target, key, receiver);
      },
      has(target, key) {
        if (typeof key === "string" && /^\d+$/.test(key))
          return Number(key) < classListTokens(target).length;
        return key in target;
      },
    };
  function makeClassList(node) {
    // Share executable state, but retain the same per-list own properties
    // and strong owner relationship used by native wrapper leases.
    const list = Object.defineProperties({}, classListDescriptors);
    Object.defineProperty(list, classListOwner, { value: node });
    return new Proxy(list, classListProxyHandler);
  }
  const classListValue = Symbol("classList value");
  const classListDescriptor = {
    configurable: true,
    enumerable: true,
    get() {
      let value = this[classListValue];
      if (value === this) {
        value = makeClassList(this);
        this[classListValue] = value;
      }
      return value;
    },
    set() {},
  };
  const installClassList = (node) => {
    if (Object.prototype.hasOwnProperty.call(node, "classList")) return;
    // Preserve the old lazy closure's owner cycle until collection: native
    // wrapper leases must not become immediate refcount releases merely
    // because nobody has read classList yet. Replace it with the token-list
    // owner relationship on first use, without two new functions per node.
    Object.defineProperty(node, classListValue, { value: node, writable: true });
    if (node instanceof Element)
      Object.defineProperty(node, "classList", classListDescriptor);
  };
  let nativeStyles;
  const nativeStyleFor = (node) => {
    let value = nativeStyles?.get(node);
    if (!value) {
      value = __tilefinchRealmGlobal.__tilefinchMakeStyle(node.__handle);
      nativeStyles ||= new shadowWeakMapType();
      nativeStyles.set(node, value);
    }
    return value;
  };
  const nativeStyleDescriptor = {
    configurable: true,
    enumerable: true,
    get() { return nativeStyleFor(this); },
    set(cssText) { nativeStyleFor(this).cssText = String(cssText); },
  };
  const installStyle = (node, handle) => {
    nativeStyles?.delete(node);
    if (node instanceof Element)
      Object.defineProperty(node, "style", nativeStyleDescriptor);
  };
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchInvalidateDetachedStyle", {
    value(node) {
      const state = attributeState(node);
      if (state) state.detachedStyle = undefined;
    },
    writable: false,
    configurable: false,
  });
  Object.defineProperty(Element.prototype, "style", Object.getOwnPropertyDescriptor({
    get style() {
      /* Live wrappers normally install an own accessor so the common lookup
         stays cheap. The property is configurable, as the platform requires;
         deleting it must reveal the prototype CSSStyleDeclaration rather than
         make style disappear from a still-live Element. */
      if (Number(this.__handle) > 0) return nativeStyleFor(this);
      if (!this.__detachedOwner) return undefined;
      /* The declaration's methods retain this node. Keep that cycle on
         the owner, like Attr/NamedNodeMap, rather than in a realm-global
         WeakMap whose values this collector marks unconditionally. */
      const state = attributeState(this, true);
      let value = state.detachedStyle;
      if (!value) {
        value = __tilefinchRealmGlobal.__tilefinchMakeDetachedStyle(this);
        state.detachedStyle = value;
      }
      return value;
    },
    set style(value) {
      const style = this.style;
      if (style) style.cssText = String(value);
    },
  }, "style"));
  /* Most startup paths never request attributes. Share the traps once they
     are needed without making a tiny realm instantiate them eagerly. */
  const makeNamedNodeMapHandler = () => ({
      get(object, key, receiver) {
        if (key === "length") return object.__tilefinchAttributeRead().length;
        if (key === Symbol.iterator)
          return () => object.__tilefinchAttributeRead()[Symbol.iterator]();
        if (typeof key === "string" && /^\d+$/.test(key))
          return object.__tilefinchAttributeRead()[Number(key)];
        if (
          ["filter", "find", "forEach", "map", "slice", "some"].includes(key)
        )
          return Array.prototype[key].bind(object.__tilefinchAttributeRead());
        if (
          typeof key === "string" &&
          !Reflect.has(object, key) &&
          !Reflect.has(NamedNodeMap.prototype, key) &&
          !Reflect.has(Object.prototype, key)
        )
          return object.getNamedItem(key) || undefined;
        return Reflect.get(object, key, receiver);
      },
      has(object, key) {
        if (key === "length") return true;
        if (typeof key === "string" && /^\d+$/.test(key))
          return Number(key) < object.__tilefinchAttributeRead().length;
        return key in object || object.getNamedItem(key) !== null;
      },
      ownKeys(object) {
        const values = object.__tilefinchAttributeRead(),
          owner = object.__tilefinchAttributeOwner,
          html =
            owner.namespaceURI === "http://www.w3.org/1999/xhtml" &&
            owner.ownerDocument?.contentType === "text/html",
          names = [];
        for (const attribute of values)
          if (
            (!html || attribute.name === attribute.name.toLowerCase()) &&
            !names.includes(attribute.name)
          )
            names.push(attribute.name);
        return values.map((_, index) => String(index)).concat(names);
      },
      getOwnPropertyDescriptor(object, key) {
        const values = object.__tilefinchAttributeRead();
        if (typeof key === "string" && /^\d+$/.test(key)) {
          const value = values[Number(key)];
          return value === undefined
            ? undefined
            : {
                configurable: true,
                enumerable: true,
                writable: false,
                value,
              };
        }
        if (typeof key === "string") {
          const value = object.getNamedItem(key),
            owner = object.__tilefinchAttributeOwner,
            html =
              owner.namespaceURI === "http://www.w3.org/1999/xhtml" &&
              owner.ownerDocument?.contentType === "text/html";
          if (
            value &&
            (!html || value.name === value.name.toLowerCase())
          )
            return {
              configurable: true,
              enumerable: false,
              writable: false,
              value,
            };
        }
        return undefined;
      },
  });
  let namedNodeMapHandler;
  const namedNodeMapFor = (owner, read) => {
    let map = attributeState(owner)?.map;
    if (map) return map;
    const target = new NamedNodeMap();
    Object.defineProperties(target, {
      __tilefinchAttributeOwner: { configurable: true, value: owner },
      __tilefinchAttributeRead: { configurable: true, value: read },
    });
    namedNodeMapHandler ||= makeNamedNodeMapHandler();
    map = new Proxy(target, namedNodeMapHandler);
    attributeState(owner, true).map = map;
    return map;
  };
  __tilefinchRealmGlobal.__tilefinchNamedNodeMapFor = namedNodeMapFor;
  Object.assign(NamedNodeMap.prototype, {
    item(index) {
      return this.__tilefinchAttributeRead()[Number(index) >>> 0] || null;
    },
    getNamedItem(name) {
      name = String(name);
      const html =
        this.__tilefinchAttributeOwner.namespaceURI ===
          "http://www.w3.org/1999/xhtml" &&
        this.__tilefinchAttributeOwner.ownerDocument?.contentType ===
          "text/html";
      return (
        this.__tilefinchAttributeRead().find((attribute) =>
          html && attribute.namespaceURI === null
            ? attribute.name === name.toLowerCase()
            : attribute.name === name,
        ) || null
      );
    },
    getNamedItemNS(namespace, localName) {
      namespace = normalizeNamespace(namespace);
      localName = String(localName);
      return (
        this.__tilefinchAttributeRead().find(
          (attribute) =>
            attribute.namespaceURI === namespace &&
            attribute.localName === localName,
        ) || null
      );
    },
    setNamedItem(attribute) {
      if (!(attribute instanceof Attr))
        throw new TypeError("setNamedItem requires an Attr");
      const owner = this.__tilefinchAttributeOwner;
      if (attribute.ownerElement && attribute.ownerElement !== owner)
        throw new DOMException(
          "Attribute is already in use",
          "InUseAttributeError",
        );
      const replaced =
          this.__tilefinchAttributeRead().find(
            (candidate) => candidate.name === attribute.name,
          ) || null,
        cache = attributeState(owner)?.objects || new Map(),
        key = attributeKey(attribute.namespaceURI, attribute.localName),
        order = attributeState(owner)?.order || [];
      attributeState(owner, true).order = order;
      if (replaced === attribute) {
        attribute.value = attribute.value;
        return attribute;
      }
      attributeState(owner).objects = cache;
      attribute.__tilefinchAttributeOwner = owner;
      attribute.__tilefinchAttributeOwnerDocument = owner.ownerDocument;
      cache.set(key, attribute);
      if (attribute.namespaceURI === null)
        owner.setAttribute(attribute.name, attribute.value);
      else
        owner.setAttributeNS(
          attribute.namespaceURI,
          attribute.name,
          attribute.value,
        );
      cache.set(key, attribute);
      if (replaced) {
        const at = order.indexOf(replaced);
        if (at >= 0) order[at] = attribute;
        else if (!order.includes(attribute)) order.push(attribute);
        replaced.__tilefinchAttributeOwner = null;
      } else if (!order.includes(attribute)) {
        order.push(attribute);
      }
      return replaced;
    },
    setNamedItemNS(attribute) {
      if (!(attribute instanceof Attr))
        throw new TypeError("setNamedItemNS requires an Attr");
      const owner = this.__tilefinchAttributeOwner;
      if (attribute.ownerElement && attribute.ownerElement !== owner)
        throw new DOMException(
          "Attribute is already in use",
          "InUseAttributeError",
        );
      const replaced = this.getNamedItemNS(
          attribute.namespaceURI,
          attribute.localName,
        ),
        cache = attributeState(owner)?.objects || new Map(),
        key = attributeKey(attribute.namespaceURI, attribute.localName),
        order = attributeState(owner)?.order || [];
      attributeState(owner, true).order = order;
      if (replaced === attribute) {
        attribute.value = attribute.value;
        return attribute;
      }
      attributeState(owner).objects = cache;
      attribute.__tilefinchAttributeOwner = owner;
      attribute.__tilefinchAttributeOwnerDocument = owner.ownerDocument;
      owner.setAttributeNS(
        attribute.namespaceURI,
        attribute.name,
        attribute.value,
      );
      cache.set(key, attribute);
      if (replaced) {
        const at = order.indexOf(replaced);
        if (at >= 0) order[at] = attribute;
        else if (!order.includes(attribute)) order.push(attribute);
        replaced.__tilefinchAttributeOwner = null;
      } else if (!order.includes(attribute)) {
        order.push(attribute);
      }
      return replaced;
    },
    removeNamedItem(name) {
      const attribute = this.getNamedItem(name);
      if (!attribute)
        throw new DOMException("Attribute was not found", "NotFoundError");
      this.__tilefinchAttributeOwner.removeAttribute(attribute.name);
      attribute.__tilefinchAttributeOwner = null;
      return attribute;
    },
    removeNamedItemNS(namespace, localName) {
      const attribute = this.getNamedItemNS(namespace, localName);
      if (!attribute)
        throw new DOMException("Attribute was not found", "NotFoundError");
      this.__tilefinchAttributeOwner.removeAttributeNS(namespace, localName);
      attribute.__tilefinchAttributeOwner = null;
      return attribute;
    },
  });
  /* Adoption stamps the Attr objects script already holds; one created
     later takes its element's (adopted) document then, so the element's
     attribute list need not be materialised for every adopted node. */
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchAdoptAttributeObjects", {
    enumerable: false,
    configurable: false,
    writable: false,
    value: (node, owner) => {
      const cache = attributeState(node)?.objects;
      if (cache)
        for (const attribute of cache.values())
          if (attribute.__tilefinchAttributeOwner === node)
            attribute.__tilefinchAttributeOwnerDocument = owner;
    },
  });
  __tilefinchRealmGlobal.__tilefinchCreateAttribute = (
    ownerDocument,
    qualifiedName,
    namespace = null,
  ) => {
    qualifiedName = String(qualifiedName);
    const colon = qualifiedName.indexOf(":");
    if (
      !qualifiedName ||
      (namespace !== null &&
        (/[\u0000-\u0020]/.test(qualifiedName) ||
          colon === qualifiedName.length - 1 ||
          (colon >= 0 && qualifiedName.indexOf(":", colon + 1) >= 0)))
    )
      throw new DOMException(
        "Invalid attribute name",
        "InvalidCharacterError",
      );
    const prefix =
        namespace === null || colon < 0
          ? null
          : qualifiedName.slice(0, colon),
      localName =
        namespace === null || colon < 0
          ? qualifiedName
          : qualifiedName.slice(colon + 1),
      record = attributeRecord(
        null,
        qualifiedName,
        "",
        namespace === null ? null : String(namespace),
        prefix,
      );
    record.__tilefinchAttributeOwnerDocument = ownerDocument;
    record.__tilefinchAttributeLocalName = localName;
    return record;
  };
  function makeTokenList(node, attribute) {
    return {
      contains(token) {
        return String(node.getAttribute(attribute) || "")
          .split(/\s+/)
          .includes(String(token));
      },
      add(...tokens) {
        const set = new Set(
          String(node.getAttribute(attribute) || "")
            .split(/\s+/)
            .filter(Boolean),
        );
        for (const token of tokens) set.add(String(token));
        node.setAttribute(attribute, [...set].join(" "));
      },
      remove(...tokens) {
        const removed = new Set(tokens.map(String));
        node.setAttribute(
          attribute,
          String(node.getAttribute(attribute) || "")
            .split(/\s+/)
            .filter((token) => token && !removed.has(token))
            .join(" "),
        );
      },
      toggle(token, force) {
        token = String(token);
        const present = this.contains(token),
          next = force === undefined ? !present : !!force;
        if (next && !present) this.add(token);
        else if (!next && present) this.remove(token);
        return next;
      },
      get value() {
        return node.getAttribute(attribute) || "";
      },
      set value(value) {
        node.setAttribute(attribute, String(value));
      },
      toString() {
        return this.value;
      },
    };
  }
  /* HTML innerText is the rendered text, which the host computes from the
     computed styles (__tilefinchRenderedText); it answers undefined for an
     element that is not being rendered, which reads its textContent. */
  /* The innerText setter: line breaks become <br> elements. */
  const setInnerText = (element, value) => {
    const text = value === null ? "" : String(value);
    if (!/[\r\n]/.test(text)) {
      element.textContent = text;
      return;
    }
    element.textContent = "";
    const ownerDocument = element.ownerDocument || document,
      fragment = ownerDocument.createDocumentFragment(),
      lines = text.replace(/\r\n?/g, "\n").split("\n");
    for (let index = 0; index < lines.length; index++) {
      if (index) fragment.appendChild(ownerDocument.createElement("br"));
      if (lines[index]) fragment.appendChild(
        ownerDocument.createTextNode(lines[index]));
    }
    element.appendChild(fragment);
  };
  // Receiver-only operations share function objects across native wrappers.
  // Keep handle/rebinding and listener closures per node, and copy descriptors
  // (not values) so own-property accessors and subclass overrides are preserved.
  const nativeNodeReceiverDescriptors = Object.getOwnPropertyDescriptors({
    get nodeValue() {
      return this.nodeType === Node.TEXT_NODE ||
        this.nodeType === Node.COMMENT_NODE
        ? this.textContent
        : null;
    },
    set nodeValue(value) {
      if (
        this.nodeType === Node.TEXT_NODE ||
        this.nodeType === Node.COMMENT_NODE
      )
        this.textContent = value == null ? "" : String(value);
    },
    get innerText() {
      if (this.nodeType !== 1) return undefined;
      const handle = this.__handle,
        text = Number.isInteger(handle) && handle > 0
          ? __tilefinchRenderedText(handle)
          : undefined;
      return text === undefined ? this.textContent : text;
    },
    set innerText(value) {
      if (this.nodeType === 1) setInnerText(this, value);
      else this.textContent = value;
    },
    get id() {
      return this.getAttribute("id") || "";
    },
    set id(value) {
      this.setAttribute("id", value);
    },
    get className() {
      return this.getAttribute("class") || "";
    },
    set className(value) {
      this.setAttribute("class", value);
    },
    get role() {
      return this.getAttribute("role");
    },
    set role(value) {
      value === null
        ? this.removeAttribute("role")
        : this.setAttribute("role", value);
    },
    get contentEditable() {
      const value = this.getAttribute("contenteditable");
      return value === null ? "inherit" : value;
    },
    set contentEditable(value) {
      this.setAttribute("contenteditable", String(value));
    },
    get isContentEditable() {
      for (
        let at = this, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentElement, steps++
      ) {
        const value = at.getAttribute("contenteditable");
        if (value !== null) return String(value).toLowerCase() !== "false";
      }
      return false;
    },
    get hidden() {
      return this.hasAttribute("hidden");
    },
    set hidden(value) {
      this.toggleAttribute("hidden", !!value);
    },
    get disabled() {
      if (this.hasAttribute("disabled")) return true;
      const tag = String(this.tagName).toLowerCase();
      if (tag === "option") {
        for (
          let at = this.parentElement, steps = 0;
          at && steps < ancestorLimit;
          at = at.parentElement, steps++
        ) {
          const parentTag = String(at.tagName).toLowerCase();
          if (
            (parentTag === "optgroup" || parentTag === "select") &&
            at.hasAttribute("disabled")
          )
            return true;
          if (parentTag === "select") break;
        }
      }
      /* A disabled fieldset ancestor is rare: one native walk rules it out
         (a script-side tree or a hit keeps the walk below, which also
         applies the first-legend exception). The script walk cost about a
         millisecond per read on a PSP, and menus read this per button. */
      const parent = this.parentElement;
      if (
        parent &&
        !parent.__tilefinchDetachedParent &&
        Number(parent.__handle) > 0 &&
        Number(__tilefinchClosest(parent.__handle, "fieldset[disabled]")) === 0
      )
        return false;
      for (
        let at = parent, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentElement, steps++
      ) {
        if (
          String(at.tagName).toLowerCase() !== "fieldset" ||
          !at.hasAttribute("disabled")
        )
          continue;
        const legend = at.children.find(
          (child) => String(child.tagName).toLowerCase() === "legend",
        );
        if (legend && legend.contains(this)) continue;
        return true;
      }
      return false;
    },
    set disabled(value) {
      this.toggleAttribute("disabled", !!value);
    },
    get value() {
      const tag = String(this.tagName).toLowerCase();
      if (tag === "textarea")
        return __tilefinchRealmGlobal.__tilefinchTextAreaValue?.(this) ?? this.textContent;
      if (tag === "select")
        return __tilefinchRealmGlobal.__tilefinchSelectValue?.(this) || "";
      if (tag === "option") {
        const value = this.getAttributeNS(null, "value");
        return value === null
          ? String(this.textContent).replace(/\s+/g, " ").trim()
          : value;
      }
      if (tag === "input" && __tilefinchRealmGlobal.__tilefinchInputValue)
        return __tilefinchRealmGlobal.__tilefinchInputValue(this);
      return this.getAttribute("value") || "";
    },
    set value(value) {
      const tag = String(this.tagName).toLowerCase();
      if (tag === "textarea" && __tilefinchRealmGlobal.__tilefinchSetTextAreaValue)
        __tilefinchRealmGlobal.__tilefinchSetTextAreaValue(this, value);
      else if (tag === "textarea") this.textContent = String(value);
      else if (tag === "select")
        __tilefinchRealmGlobal.__tilefinchSetSelectValue?.(this, String(value));
      else if (tag === "input" && __tilefinchRealmGlobal.__tilefinchSetInputValue)
        __tilefinchRealmGlobal.__tilefinchSetInputValue(this, value);
      else this.setAttribute("value", String(value));
    },
    get name() {
      return this.getAttribute("name") || "";
    },
    set name(value) {
      this.setAttribute("name", String(value));
    },
    get type() {
      const tag = String(this.tagName).toLowerCase();
      return __tilefinchRealmGlobal.__tilefinchControlType &&
        (tag === "input" || tag === "button")
        ? __tilefinchRealmGlobal.__tilefinchControlType(this)
        : this.getAttribute("type") || "";
    },
    set type(value) {
      this.setAttribute("type", String(value));
    },
    get action() {
      const value = this.getAttribute("action") || "";
      try {
        return new URL(value || location.href, location.href).href;
      } catch {
        return value;
      }
    },
    set action(value) {
      this.setAttribute("action", String(value));
    },
    get checked() {
      return this.hasAttribute("checked");
    },
    set checked(value) {
      if (__tilefinchRealmGlobal.__tilefinchSetChecked)
        __tilefinchRealmGlobal.__tilefinchSetChecked(this, !!value);
      else this.toggleAttribute("checked", !!value);
    },
    get selected() {
      return String(this.tagName).toLowerCase() === "option" &&
        __tilefinchRealmGlobal.__tilefinchOptionSelected
        ? __tilefinchRealmGlobal.__tilefinchOptionSelected(this)
        : this.hasAttribute("selected");
    },
    set selected(value) {
      if (
        String(this.tagName).toLowerCase() === "option" &&
        __tilefinchRealmGlobal.__tilefinchSetOptionSelected
      )
        __tilefinchRealmGlobal.__tilefinchSetOptionSelected(this, !!value);
      else this.toggleAttribute("selected", !!value);
    },
    get placeholder() {
      return this.getAttribute("placeholder") || "";
    },
    set placeholder(value) {
      this.setAttribute("placeholder", String(value));
    },
    get selectionStart() {
      const tag = String(this.tagName).toLowerCase();
      return tag === "input" || tag === "textarea"
        ? this.__selectionStart === undefined
          ? 0
          : this.__selectionStart
        : null;
    },
    set selectionStart(value) {
      if (this.selectionStart !== null)
        this.setSelectionRange(value, this.selectionEnd);
    },
    get selectionEnd() {
      const tag = String(this.tagName).toLowerCase();
      return tag === "input" || tag === "textarea"
        ? this.__selectionEnd === undefined
          ? 0
          : this.__selectionEnd
        : null;
    },
    set selectionEnd(value) {
      if (this.selectionEnd !== null)
        this.setSelectionRange(this.selectionStart, value);
    },
    get selectionDirection() {
      return this.selectionStart === null
        ? null
        : this.__selectionDirection || "none";
    },
    setSelectionRange(start, end, direction = "none") {
      if (this.selectionStart === null)
        throw new DOMException(
          "Selection is unavailable",
          "InvalidStateError",
        );
      const length = this.value.length;
      start = Math.max(0, Math.min(length, Number(start) || 0));
      end = Math.max(0, Math.min(length, Number(end) || 0));
      if (end < start) start = end;
      this.__selectionStart = start;
      this.__selectionEnd = end;
      this.__selectionDirection = ["forward", "backward"].includes(direction)
        ? direction
        : "none";
      __tilefinchRealmGlobal.__tilefinchSelectionChanged?.();
    },
    select() {
      if (this.selectionStart !== null) {
        this.setSelectionRange(0, this.value.length);
        __tilefinchRealmGlobal.__tilefinchSelectedControl = this;
      }
    },
    get tabIndex() {
      const value = parseInt(this.getAttribute("tabindex"), 10);
      return Number.isFinite(value) ? value : this.isContentEditable ? 0 : -1;
    },
    set tabIndex(value) {
      this.setAttribute("tabindex", String(Number(value) || 0));
    },
    get localName() {
      return String(this.tagName || "").toLowerCase();
    },
    get nodeName() {
      return this.nodeType === Node.TEXT_NODE
        ? "#text"
        : this.nodeType === Node.COMMENT_NODE
          ? "#comment"
          : this.nodeType === Node.DOCUMENT_FRAGMENT_NODE
            ? "#document-fragment"
            : this.tagName;
    },
    get ownerDocument() {
      const known = this.__tilefinchAdoptedOwner;
      if (known) return known;
      if (
        !nodeOwnersActive ||
        !(this.__handle > 0)
      )
        return document;
      const tag = __tilefinchNodeOwner(this.__handle),
        owner =
          (tag === 2 ? templateOwnerDocument() : nodeOwnerDocuments[tag]) ||
          document;
      if (tag) cacheNodeOwner(this, owner);
      return owner;
    },
    get namespaceURI() {
      return this.__namespaceURI !== undefined
        ? this.__namespaceURI
        : this.nodeType === Node.ELEMENT_NODE
          ? "http://www.w3.org/1999/xhtml"
          : null;
    },
    get lastElementChild() {
      const values = this.children;
      return values.length ? values[values.length - 1] : null;
    },
    get src() {
      return this.getAttribute("src") || "";
    },
    set src(value) {
      this.setAttribute("src", value);
    },
    get href() {
      const value = this.getAttribute("href") || "";
      try {
        return value ? new URL(value, location.href).href : "";
      } catch {
        return value;
      }
    },
    set href(value) {
      this.setAttribute("href", value);
    },
    get hreflang() {
      return this.getAttribute("hreflang") || "";
    },
    set hreflang(value) {
      this.setAttribute("hreflang", value);
    },
    get rel() {
      return this.getAttribute("rel") || "";
    },
    set rel(value) {
      this.setAttribute("rel", value);
    },
    get target() {
      return this.getAttribute("target") || "";
    },
    set target(value) {
      this.setAttribute("target", value);
    },
    get width() {
      if (this instanceof HTMLCanvasElement) {
        __tilefinchRealmGlobal.__tilefinchEnsureCanvasBootstrap?.();
        if (__tilefinchRealmGlobal.__tilefinchCanvasDimension)
          return __tilefinchRealmGlobal.__tilefinchCanvasDimension(this, "width");
      }
      return this.getAttribute("width") || "";
    },
    set width(value) {
      if (this instanceof HTMLCanvasElement) {
        __tilefinchRealmGlobal.__tilefinchEnsureCanvasBootstrap?.();
        if (__tilefinchRealmGlobal.__tilefinchSetCanvasDimension) {
          __tilefinchRealmGlobal.__tilefinchSetCanvasDimension(this, "width", value);
          return;
        }
      }
      this.setAttribute("width", value);
    },
    get height() {
      if (this instanceof HTMLCanvasElement) {
        __tilefinchRealmGlobal.__tilefinchEnsureCanvasBootstrap?.();
        if (__tilefinchRealmGlobal.__tilefinchCanvasDimension)
          return __tilefinchRealmGlobal.__tilefinchCanvasDimension(this, "height");
      }
      return this.getAttribute("height") || "";
    },
    set height(value) {
      if (this instanceof HTMLCanvasElement) {
        __tilefinchRealmGlobal.__tilefinchEnsureCanvasBootstrap?.();
        if (__tilefinchRealmGlobal.__tilefinchSetCanvasDimension) {
          __tilefinchRealmGlobal.__tilefinchSetCanvasDimension(this, "height", value);
          return;
        }
      }
      this.setAttribute("height", value);
    },
    get sandbox() {
      return makeTokenList(this, "sandbox");
    },
    set sandbox(value) {
      this.setAttribute("sandbox", String(value));
    },
    get ariaAtomic() {
      return this.getAttribute("aria-atomic");
    },
    set ariaAtomic(value) {
      value === null
        ? this.removeAttribute("aria-atomic")
        : this.setAttribute("aria-atomic", value);
    },
    get ariaLive() {
      return this.getAttribute("aria-live");
    },
    set ariaLive(value) {
      value === null
        ? this.removeAttribute("aria-live")
        : this.setAttribute("aria-live", value);
    },
    get defer() {
      return this.hasAttribute("defer");
    },
    set defer(value) {
      value ? this.setAttribute("defer", "") : this.removeAttribute("defer");
    },
    hasAttribute(name) {
      return this.getAttribute(name) !== null;
    },
    hasAttributeNS(namespace, name) {
      return this.getAttributeNS(namespace, name) !== null;
    },
    getAttributeNode(name) {
      return this.attributes.getNamedItem(String(name));
    },
    getAttributeNodeNS(namespace, localName) {
      return this.attributes.getNamedItemNS(
        normalizeNamespace(namespace),
        String(localName),
      );
    },
    setAttributeNode(attribute) {
      return this.attributes.setNamedItem(attribute);
    },
    setAttributeNodeNS(attribute) {
      return this.attributes.setNamedItemNS(attribute);
    },
    getAttributeNames() {
      return this.attributes.map((attribute) => attribute.name);
    },
    getElementById(id) {
      return this.querySelector("#" + String(id));
    },
    matches(selector) {
      selector = String(selector);
      const compact = selector.replace(/\s+/g, "").toLowerCase();
      if (
        compact === ":defined" ||
        compact === ":not(:defined)"
      ) {
        const defined =
          __tilefinchRealmGlobal.__tilefinchCustomElementIsDefined?.(this) ?? true;
        return compact === ":defined" ? defined : !defined;
      }
      return __tilefinchRealmGlobal.__tilefinchElementMatches
        ? __tilefinchRealmGlobal.__tilefinchElementMatches(this, selector)
        : __tilefinchMatches(this.__handle, selector);
    },
    closest(selector) {
      return __tilefinchRealmGlobal.__tilefinchElementClosest
        ? __tilefinchRealmGlobal.__tilefinchElementClosest(this, selector)
        : (() => {
            for (
              let at = this, steps = 0;
              at && steps < ancestorLimit;
              at = at.parentElement, steps++
            )
              if (at.matches(selector)) return at;
            return null;
          })();
    },
    contains(other) {
      /* One native walk up other's tree (form.elements asks this for every
         control in the document); a root with a script-side detached
         parent, or a tree left to script, takes the parentElement walk. */
      if (
        typeof nativeRootNode === "function" &&
        other?.__handle > 0 &&
        this.__handle > 0 &&
        !other.__tilefinchDetachedParent
      ) {
        const found = Number(nativeRootNode(other.__handle, this.__handle));
        if (found === Number(this.__handle)) return true;
        if (found < 0) return false;
        if (found > 0 && !wrap(found)?.__tilefinchDetachedParent) return false;
      }
      for (
        let at = other, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentElement, steps++
      )
        if (at === this) return true;
      return false;
    },
    get clientWidth() {
      if (this === document.documentElement) return innerWidth;
      const geometry = this.__tilefinchGeometryValue();
      /* The retained client box already includes padding and excludes
         borders. Used CSS widths may be border-box sizes; adding padding
         to them again changes overflow decisions. */
      if (geometry.retained) return geometry.clientWidth;
      const style = getComputedStyle(this), width = parseFloat(style.width),
        left = parseFloat(style.getPropertyValue("padding-left")) || parseFloat(style.padding) || 0,
        right = parseFloat(style.getPropertyValue("padding-right")) || parseFloat(style.padding) || 0;
      if (Number.isFinite(width) && width > 0)
        return Math.round(width + (String(this.tagName).toLowerCase() === "input" ? 0 : left + right));
      return geometry.clientWidth;
    },
    get clientHeight() {
      if (this === document.documentElement) return innerHeight;
      const geometry = this.__tilefinchGeometryValue();
      if (geometry.retained) return geometry.clientHeight;
      const style = getComputedStyle(this), height = parseFloat(style.height),
        top = parseFloat(style.getPropertyValue("padding-top")) || parseFloat(style.padding) || 0,
        bottom = parseFloat(style.getPropertyValue("padding-bottom")) || parseFloat(style.padding) || 0;
      if (Number.isFinite(height) && height > 0) return Math.round(height + top + bottom);
      return geometry.clientHeight;
    },
    get clientTop() {
      if (this === document.documentElement) return 0;
      const style = getComputedStyle(this);
      return Math.round(
        parseFloat(style.getPropertyValue("border-top-width")) ||
        parseFloat(style.borderWidth) ||
        0,
      );
    },
    get clientLeft() {
      if (this === document.documentElement) return 0;
      const style = getComputedStyle(this);
      return Math.round(
        (parseFloat(style.getPropertyValue("border-left-width")) ||
          parseFloat(style.borderWidth) ||
          0) +
          (String(this.tagName).toLowerCase() === "input"
            ? parseFloat(style.getPropertyValue("padding-left")) ||
              parseFloat(style.padding) ||
              0
            : 0),
      );
    },
    get offsetWidth() {
      return this.getBoundingClientRect().width;
    },
    get offsetHeight() {
      return this.getBoundingClientRect().height;
    },
    get offsetParent() {
      /* CSSOM View terminates the chain at the root and body, regardless
         of positioning. Falling back to body here made body its own
         offsetParent and turned ordinary author geometry walks into cycles. */
      const owner = this.ownerDocument;
      if (this === owner.documentElement || this === owner.body) return null;
      const position = getComputedStyle(this).position;
      if (position === "fixed") {
        for (
          let at = this.parentElement, steps = 0;
          at && steps < ancestorLimit;
          at = at.parentElement, steps++
        ) {
          const style = getComputedStyle(at);
          if (
            (style.getPropertyValue("transform") &&
              style.getPropertyValue("transform") !== "none") ||
            String(style.getPropertyValue("will-change"))
              .split(/\s*,\s*/)
              .includes("transform") ||
            (style.getPropertyValue("perspective") &&
              style.getPropertyValue("perspective") !== "none") ||
            (style.getPropertyValue("filter") &&
              style.getPropertyValue("filter") !== "none") ||
            String(style.contain).split(/\s+/).includes("paint")
          )
            return at;
        }
        return null;
      }
      for (
        let at = this.parentElement, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentElement, steps++
      )
        if (getComputedStyle(at).position !== "static") return at;
      return owner.body;
    },
    get scrollWidth() {
      return this.__tilefinchGeometryValue().scrollWidth;
    },
    get scrollHeight() {
      return this.__tilefinchGeometryValue().scrollHeight;
    },
    scroll(...args) {
      return this.scrollTo(...args);
    },
    scrollBy(xOrOptions, y) {
      if (
        arguments.length === 1 &&
        (xOrOptions === null || typeof xOrOptions !== "object")
      )
        return Promise.reject(
          new TypeError("Single scroll argument must be a dictionary"),
        );
      if (typeof xOrOptions === "object" && xOrOptions !== null)
        return this.scrollTo({
          left: this.scrollLeft + (Number(xOrOptions.left) || 0),
          top: this.scrollTop + (Number(xOrOptions.top) || 0),
          behavior: xOrOptions.behavior,
        });
      else
        return this.scrollTo(
          this.scrollLeft + (Number(xOrOptions) || 0),
          this.scrollTop + (Number(y) || 0),
        );
    },
    getClientRects() {
      const rect = this.getBoundingClientRect();
      return rect.width && rect.height ? [rect] : [];
    },
    prepend(...values) {
      if (__tilefinchRealmGlobal.__tilefinchParentAppend)
        return __tilefinchRealmGlobal.__tilefinchParentAppend(this, values, true);
      for (let at = values.length - 1; at >= 0; at--) {
        const value =
          values[at] instanceof Node
            ? values[at]
            : document.createTextNode(String(values[at]));
        this.insertBefore(value, this.firstChild);
      }
    },
    removeChild(child) {
      const parent = child && child.parentNode;
      if (
        !child ||
        !parent ||
        (parent !== this && parent.__handle !== this.__handle)
      )
        throw new Error("removeChild failed");
      child.remove();
      return child;
    },
    get dataset() {
      const node = this,
        toAttribute = (name) =>
          "data-" +
          String(name).replace(/[A-Z]/g, (c) => "-" + c.toLowerCase()),
        validate = (name) => {
          name = String(name);
          if (/-[a-z]/.test(name))
            throw new DOMException("Invalid dataset property", "SyntaxError");
          return name;
        };
      return new Proxy(
        {},
        {
          get(target, name) {
            if (typeof name === "symbol") return target[name];
            const value = node.getAttributeNS(null, toAttribute(name));
            return value === null ? undefined : value;
          },
          set(target, name, value) {
            node.setAttributeNS(
              null,
              toAttribute(validate(name)),
              String(value),
            );
            return true;
          },
          deleteProperty(target, name) {
            node.removeAttributeNS(null, toAttribute(validate(name)));
            return true;
          },
          has(target, name) {
            return node.hasAttributeNS(null, toAttribute(name));
          },
        },
      );
    },
    getAttributeNS(namespace, name) {
      namespace = normalizeNamespace(namespace);
      name = String(name);
      /* Dataset reads use the null namespace. For ordinary native HTML
         attributes the host is authoritative: do not construct the entire
         NamedNodeMap/Attr graph just to read one value. Keep case-sensitive,
         qualified and mirrored lookups on the namespace-aware path. */
      if (namespace === null && name === name.toLowerCase() &&
          !name.includes(":") && Number(this.__handle) > 0 &&
          this.namespaceURI === "http://www.w3.org/1999/xhtml" &&
          !namespacedAttributes.get(this)?.length)
        return __tilefinchGetAttribute(this.__handle, name);
      const found = [...this.attributes].find(
        (attribute) =>
          attribute.namespaceURI === namespace &&
          attribute.localName === name,
      );
      if (found) return found.value;
      const prefix =
        namespace === "http://www.w3.org/1999/xlink"
          ? "xlink:"
          : namespace === "http://www.w3.org/XML/1998/namespace"
            ? "xml:"
            : "";
      return prefix ? this.getAttribute(prefix + name) : null;
    },
    removeAttributeNode(attribute) {
      if (!(attribute instanceof Attr))
        throw new TypeError("removeAttributeNode requires an Attr");
      if (attribute.ownerElement !== this)
        throw new DOMException("Attribute was not found", "NotFoundError");
      this.removeAttributeNS(attribute.namespaceURI, attribute.localName);
      return attribute;
    },
    toggleAttribute(name, force = undefined) {
      name = String(name);
      if (!name)
        throw new DOMException(
          "Invalid attribute name",
          "InvalidCharacterError",
        );
      if (this.namespaceURI === "http://www.w3.org/1999/xhtml")
        name = name.toLowerCase();
      const present = this.hasAttribute(name),
        next = force === undefined ? !present : !!force;
      if (next && !present) this.setAttribute(name, "");
      else if (!next && present) this.removeAttribute(name);
      return next;
    },
    get offsetTop() {
      const parent = this.offsetParent,
        rect = this.getBoundingClientRect(),
        geometry = this.__tilefinchGeometryValue();
      let value =
        rect.top - (parent ? parent.getBoundingClientRect().top : 0);
      if (
        !geometry.retained &&
        value <= 0 &&
        getComputedStyle(this).position === "static"
      ) {
        value = 0;
        for (const sibling of parent?.childNodes || []) {
          if (sibling === this) break;
          if (!(sibling instanceof Element)) continue;
          const display = getComputedStyle(sibling).display;
          if (display === "block") value += sibling.offsetHeight;
        }
      }
      return value;
    },
    get offsetLeft() {
      const parent = this.offsetParent,
        rect = this.getBoundingClientRect(),
        geometry = this.__tilefinchGeometryValue();
      let value =
        rect.left - (parent ? parent.getBoundingClientRect().left : 0);
      if (
        !geometry.retained &&
        value <= 0 &&
        getComputedStyle(this).position === "static"
      ) {
        value = 0;
        for (const sibling of parent?.childNodes || []) {
          if (sibling === this) break;
          if (!(sibling instanceof Element)) continue;
          if (getComputedStyle(sibling).display !== "block")
            value += sibling.offsetWidth;
        }
      }
      return value;
    },
    scrollIntoView(options = {}) {
      const dictionary = options && typeof options === "object" ? options : {},
        behavior = dictionary.behavior || "auto",
        block =
          options === false ? "end" : dictionary.block || "start",
        inline = dictionary.inline || "nearest",
        targetStyle = getComputedStyle(this),
        targetFont = parseFloat(targetStyle.fontSize) || 16,
        length = (style, name, font, reference) =>
          cssLengthPixels(style.getPropertyValue(name), font, reference) || 0,
        marginTop = length(targetStyle, "scroll-margin-top", targetFont),
        marginBottom = length(targetStyle, "scroll-margin-bottom", targetFont),
        marginLeft = length(targetStyle, "scroll-margin-left", targetFont),
        marginRight = length(targetStyle, "scroll-margin-right", targetFont);
      for (const at of boundedAncestorPath(
        this.parentElement,
        (node) => node.parentElement,
      )) {
        if (at === document.scrollingElement) continue;
        const style = getComputedStyle(at);
        const scrollsX = ["auto", "scroll", "hidden"].includes(
            style.overflowX,
          ),
          scrollsY = ["auto", "scroll", "hidden"].includes(style.overflowY);
        if (!scrollsX && !scrollsY)
          continue;
        const targetRect = this.getBoundingClientRect(),
          containerRect = at.getBoundingClientRect(),
          containerFont = parseFloat(style.fontSize) || 16,
          paddingTop = length(style, "scroll-padding-top", containerFont,
                              containerRect.height),
          paddingBottom = length(style, "scroll-padding-bottom",
                                 containerFont, containerRect.height),
          paddingLeft = length(style, "scroll-padding-left", containerFont,
                               containerRect.width),
          paddingRight = length(style, "scroll-padding-right", containerFont,
                                containerRect.width),
          start =
            at.scrollTop +
            targetRect.top -
            containerRect.top -
            paddingTop -
            marginTop,
          end =
            at.scrollTop +
            targetRect.bottom -
            containerRect.bottom +
            paddingBottom +
            marginBottom,
          startX =
            at.scrollLeft +
            targetRect.left -
            containerRect.left -
            paddingLeft -
            marginLeft,
          endX =
            at.scrollLeft +
            targetRect.right -
            containerRect.right +
            paddingRight +
            marginRight;
        let top = at.scrollTop,
          left = at.scrollLeft;
        if (block === "end") top = end;
        else if (block === "center") top = (start + end) / 2;
        else if (block === "start") top = start;
        else if (block === "nearest")
          top =
            targetRect.top < containerRect.top
              ? start
              : targetRect.bottom > containerRect.bottom
                ? end
                : at.scrollTop;
        if (inline === "end") left = endX;
        else if (inline === "center") left = (startX + endX) / 2;
        else if (inline === "start") left = startX;
        else if (inline === "nearest")
          left =
            targetRect.left < containerRect.left
              ? startX
              : targetRect.right > containerRect.right
                ? endX
                : at.scrollLeft;
        at.scrollTo({
          left: scrollsX ? left : at.scrollLeft,
          top: scrollsY ? top : at.scrollTop,
          behavior,
        });
      }
      const root = document.scrollingElement;
      if (root && typeof __tilefinchRealmGlobal.scrollTo === "function") {
        const rootStyle = getComputedStyle(root),
          rect = this.getBoundingClientRect(),
          rootFont = parseFloat(rootStyle.fontSize) || 16,
          paddingTop = length(rootStyle, "scroll-padding-top", rootFont,
                              Number(__tilefinchRealmGlobal.innerHeight) || 0),
          paddingBottom = length(rootStyle, "scroll-padding-bottom", rootFont,
                                 Number(__tilefinchRealmGlobal.innerHeight) || 0),
          viewportHeight =
            Number(__tilefinchRealmGlobal.innerHeight) ||
            Number(__tilefinchRealmGlobal.visualViewport?.height) ||
            0,
          start = __tilefinchRealmGlobal.scrollY + rect.top - paddingTop - marginTop,
          end =
            __tilefinchRealmGlobal.scrollY +
            rect.bottom -
            viewportHeight +
            paddingBottom +
            marginBottom;
        let top = start;
        if (block === "end") top = end;
        else if (block === "center") top = (start + end) / 2;
        else if (block === "nearest")
          top =
            rect.top < paddingTop
              ? start
              : rect.bottom > viewportHeight - paddingBottom
                ? end
                : __tilefinchRealmGlobal.scrollY;
        __tilefinchRealmGlobal.scrollTo({ top, behavior });
      }
    },
    getBoundingClientRect() {
      if (!this.isConnected) return new DOMRect(0, 0, 0, 0);
      const g = this.__tilefinchGeometryValue();
      // Retained layout geometry already includes positioned layout,
      // transforms, ancestor/page scrolling and fixed-position adjustment.
      // Avoid rebuilding it with style and ancestor walks on the common
      // path; the fallback below remains for virtual or not-yet-laid-out
      // nodes.
      const authoritativeAutoHeight =
        g.authoritative &&
        (Number(g.height) || 0) <= 0 &&
        shadowRootForHost(this);
      if (g.authoritative && !authoritativeAutoHeight)
        return new DOMRect(
          Number(g.x) || 0,
          Number(g.y) || 0,
          Number(g.width) || 0,
          Number(g.height) || 0,
        );
      const style = getComputedStyle(this);
      if (style.display === "none") return new DOMRect();
      const retainedWidth = Number(g.width) || 0,
        retainedHeight = Number(g.height) || 0,
        parentElement = this.parentElement,
        /* A retained box at (0, 0) is a valid authoritative position. Only
           synthesize flow when the native layout supplied no dimensions;
           otherwise a previous sibling can shift a correct box by its own
           height. */
        needsFlowFallback =
          retainedWidth <= 0 && retainedHeight <= 0,
        positionedContainingElement =
          style.position === "absolute" || style.position === "fixed"
            ? boundedAncestorPath(
                this.parentElement,
                (node) => node.parentElement,
              ).find((at) => {
                const candidate = getComputedStyle(at),
                  transformed =
                    (candidate.getPropertyValue("transform") &&
                      candidate.getPropertyValue("transform") !== "none") ||
                    String(candidate.getPropertyValue("will-change"))
                      .split(/\s*,\s*/)
                      .includes("transform") ||
                    (candidate.getPropertyValue("perspective") &&
                      candidate.getPropertyValue("perspective") !== "none") ||
                    (candidate.getPropertyValue("filter") &&
                      candidate.getPropertyValue("filter") !== "none") ||
                    String(candidate.contain).split(/\s+/).includes("paint");
                return transformed ||
                  (style.position === "absolute" &&
                    candidate.position !== "static");
              }) || null
            : null,
        containingElement =
          style.position === "absolute" || style.position === "fixed"
            ? positionedContainingElement
            : parentElement,
        containingGeometry =
          containingElement?.__tilefinchGeometryValue?.() || null,
        containingRect = containingGeometry
          ? new DOMRect(
              Number(containingGeometry.x) || 0,
              Number(containingGeometry.y) || 0,
              Number(containingGeometry.width) || 0,
              Number(containingGeometry.height) || 0,
            )
          : new DOMRect(0, 0, innerWidth, innerHeight),
        cssPixels = (value, reference, viewportFallback) => {
          value = String(value || "").trim();
          const number = parseFloat(value);
          if (!Number.isFinite(number)) return NaN;
          if (value.endsWith("%"))
            return ((reference || viewportFallback) * number) / 100;
          if (value.endsWith("em") || value.endsWith("rem"))
            return number * 16;
          return number;
        };
      let x = Number(g.x) || 0,
        y = Number(g.y) || 0,
        width = retainedWidth,
        height = retainedHeight;
      const cssWidth = cssPixels(
          style.width,
          containingRect.width,
          innerWidth,
        ),
        cssHeight = cssPixels(
          style.height,
          containingRect.height,
          innerHeight,
        );
      if (Number.isFinite(cssWidth) && cssWidth > 0) width = cssWidth;
      if (Number.isFinite(cssHeight) && cssHeight > 0) height = cssHeight;
      if (
        height <= 0 &&
        !Number.isFinite(cssHeight) &&
        style.position !== "absolute" &&
        style.position !== "fixed"
      ) {
        /* A shadow root is represented natively by a display:contents
           carrier. Until native layout has consumed a dynamic shadow
           mutation, derive the host's auto block size from its in-flow
           descendants. Keep both traversal and nesting explicitly bounded:
           geometry is observable author work and must not turn a deep
           hostile tree into unbounded recursion on PSP. */
        const state = { remaining: 128 },
          measureAutoContentHeight = (container, depth) => {
            if (depth >= 16 || state.remaining <= 0) return 0;
            const shadow =
              container instanceof Element
                ? shadowRootForHost(container)
                : null;
            if (shadow)
              return measureAutoContentHeight(shadow, depth + 1);
            let blocks = 0,
              line = 0,
              child = container?.firstChild || null;
            for (
              let siblings = 0;
              child && siblings < 128 && state.remaining > 0;
              child = child.nextSibling, siblings++
            ) {
              state.remaining--;
              if (
                !(child instanceof Element) &&
                !(child instanceof ShadowRoot)
              )
                continue;
              if (child instanceof ShadowRoot) {
                const contents = measureAutoContentHeight(child, depth + 1);
                line = Math.max(line, contents);
                continue;
              }
              const childStyle = getComputedStyle(child);
              if (
                childStyle.display === "none" ||
                childStyle.position === "absolute" ||
                childStyle.position === "fixed"
              )
                continue;
              const childGeometry =
                  child.__tilefinchGeometryValue?.() || {},
                authoredHeight = cssPixels(
                  childStyle.height,
                  containingRect.height,
                  innerHeight,
                ),
                marginTop =
                  parseFloat(childStyle.getPropertyValue("margin-top")) || 0,
                marginBottom =
                  parseFloat(childStyle.getPropertyValue("margin-bottom")) ||
                  0,
                padding =
                  (parseFloat(
                    childStyle.getPropertyValue("padding-top"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("padding-bottom"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("border-top-width"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("border-bottom-width"),
                  ) || 0);
              let childHeight = Number(childGeometry.height) || 0;
              if (Number.isFinite(authoredHeight) && authoredHeight > 0)
                childHeight = authoredHeight + padding;
              else if (childHeight <= 0)
                childHeight =
                  measureAutoContentHeight(child, depth + 1) + padding;
              const extent = marginTop + childHeight + marginBottom,
                inline =
                  childStyle.display === "inline" ||
                  childStyle.display === "inline-block" ||
                  childStyle.display === "inline-flex" ||
                  childStyle.display === "inline-grid";
              if (inline) line = Math.max(line, extent);
              else {
                blocks += line + extent;
                line = 0;
              }
            }
            return blocks + line;
          };
        height = measureAutoContentHeight(this, 0);
      }
      if (
        needsFlowFallback &&
        (this === document.documentElement || this === document.body)
      ) {
        x = 0;
        y = 0;
        if (width <= 0) width = innerWidth;
        if (height <= 0)
          height =
            this === document.documentElement
              ? innerHeight
              : parseFloat(style.height) || innerHeight;
      } else if (
        needsFlowFallback &&
        style.position !== "absolute" &&
        style.position !== "fixed"
      ) {
        const parentRect = containingRect,
          marginLeft =
            parseFloat(style.getPropertyValue("margin-left")) || 0,
          marginTop =
            parseFloat(style.getPropertyValue("margin-top")) || 0;
        let lineX = parentRect.left,
          lineY = parentRect.top,
          lineHeight = 0,
          blockBottom = parentRect.top;
        for (const sibling of this.parentElement?.childNodes || []) {
          if (sibling === this) break;
          if (!(sibling instanceof Element)) continue;
          const siblingStyle = getComputedStyle(sibling);
          if (
            siblingStyle.display === "none" ||
            siblingStyle.position === "absolute" ||
            siblingStyle.position === "fixed"
          )
            continue;
          const siblingGeometry =
              sibling.__tilefinchGeometryValue?.() || null,
            siblingRect = siblingGeometry
              ? new DOMRect(
                  Number(siblingGeometry.x) || 0,
                  Number(siblingGeometry.y) || 0,
                  Number(siblingGeometry.width) || 0,
                  Number(siblingGeometry.height) || 0,
                )
              : new DOMRect(),
            siblingBottom =
              parseFloat(
                siblingStyle.getPropertyValue("margin-bottom"),
              ) || 0,
            siblingRight =
              parseFloat(
                siblingStyle.getPropertyValue("margin-right"),
              ) || 0,
            inline =
              siblingStyle.display === "inline" ||
              siblingStyle.display === "inline-block";
          if (inline) {
            lineY = Math.max(lineY, blockBottom);
            lineX += siblingRect.width + siblingRight;
            lineHeight = Math.max(
              lineHeight,
              siblingRect.height + siblingBottom,
            );
          } else {
            if (lineHeight > 0) {
              blockBottom = Math.max(
                blockBottom,
                lineY + lineHeight,
              );
              lineX = parentRect.left;
              lineHeight = 0;
            }
            blockBottom = Math.max(
              blockBottom,
              siblingRect.bottom + siblingBottom,
            );
            lineY = blockBottom;
          }
        }
        const inline =
          style.display === "inline" || style.display === "inline-block";
        x = (inline ? lineX : parentRect.left) + marginLeft;
        y =
          (inline
            ? Math.max(lineY, blockBottom)
            : Math.max(blockBottom, lineY + lineHeight)) + marginTop;
        if (style.float === "right")
          x =
            parentRect.right -
            width -
            (parseFloat(style.getPropertyValue("margin-right")) || 0);
      }
      if (style.position === "absolute" || style.position === "fixed") {
        const left = cssPixels(
            style.left,
            containingRect.width,
            innerWidth,
          ),
          top = cssPixels(
            style.top,
            containingRect.height,
            innerHeight,
          ),
          origin =
            style.position === "fixed" && !this.offsetParent
              ? new DOMRect()
              : containingRect;
        if (Number.isFinite(left))
          x =
            origin.left +
            left +
            (parseFloat(style.getPropertyValue("margin-left")) || 0);
        else if (x === 0)
          x =
            origin.left +
            (parseFloat(style.getPropertyValue("margin-left")) || 0);
        if (Number.isFinite(top))
          y =
            origin.top +
            top +
            (parseFloat(style.getPropertyValue("margin-top")) || 0);
        else if (y === 0)
          y =
            origin.top +
            (parseFloat(style.getPropertyValue("margin-top")) || 0);
        /* Native retained geometry subtracts every scrolled DOM ancestor.
           CSS positioned descendants escape scroll containers between
           themselves and their containing block. Restore only those
           intervening scroll offsets; the containing block and its
           ancestors continue to move normally. */
        for (
          let at = this.parentElement, steps = 0;
          at && at !== positionedContainingElement && steps < ancestorLimit;
          at = at.parentElement, steps++
        ) {
          x += Number(at.scrollLeft) || 0;
          y += Number(at.scrollTop) || 0;
        }
      }
      const transform = String(style.transform || ""),
        pair = transform.match(
          /translate(?:3d)?\(\s*(-?[\d.]+)px(?:\s*,\s*(-?[\d.]+)px)?/,
        ),
        translateX = transform.match(
          /translateX\(\s*(-?[\d.]+)px/,
        ),
        translateY = transform.match(
          /translateY\(\s*(-?[\d.]+)px/,
        );
      x += Number(pair?.[1] || translateX?.[1]) || 0;
      y += Number(pair?.[2] || translateY?.[1]) || 0;
      if (needsFlowFallback) {
        const scrollAncestor =
          style.position === "absolute" || style.position === "fixed"
            ? this.offsetParent
            : this.parentElement;
        x -= Number(scrollAncestor?.scrollLeft) || 0;
        y -= Number(scrollAncestor?.scrollTop) || 0;
      }
      return new DOMRect(x, y, width, height);
    },
    append(...values) {
      if (
        __tilefinchRealmGlobal.__tilefinchParentAppend &&
        !__tilefinchRealmGlobal.__tilefinchParentAppendBypass
      )
        return __tilefinchRealmGlobal.__tilefinchParentAppend(this, values, false);
      const nodes = [];
      for (const value of values) {
        const node =
          value instanceof Node
            ? value
            : document.createTextNode(String(value));
        if (node instanceof DocumentFragment) nodes.push(...node.childNodes);
        else nodes.push(node);
      }
      const fragment = document.createDocumentFragment();
      for (const node of nodes) fragment.appendChild(node);
      this.appendChild(fragment);
    },
    replaceChildren(...values) {
      const owner =
          this.__handle !== undefined
            ? document
            : this.ownerDocument || this,
        nodes = values.map((value) =>
          value instanceof Node ? value : owner.createTextNode(String(value)),
        ),
        moved = [];
      for (const node of nodes) {
        const candidates =
          node instanceof DocumentFragment ? [...node.childNodes] : [node];
        for (const candidate of candidates)
          if (candidate.parentNode)
            moved.push({
              node: candidate,
              parent: candidate.parentNode,
              previousSibling: candidate.previousSibling,
              nextSibling: candidate.nextSibling,
            });
      }
      const
        insertion =
          nodes.length === 1
            ? nodes[0]
            : owner.createDocumentFragment();
      __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
        (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
      let removed;
      try {
        if (nodes.length !== 1)
          for (const node of nodes) insertion.appendChild(node);
        __tilefinchRealmGlobal.__tilefinchValidatePreInsert?.(
          this,
          insertion,
          null,
          true,
        );
        removed = [...this.childNodes];
        for (const child of removed) this.removeChild(child);
        if (nodes.length) this.appendChild(insertion);
      } finally {
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
      }
      for (const move of moved)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          move.parent,
          "childList",
          null,
          [],
          [move.node],
          null,
          move.previousSibling,
          move.nextSibling,
        );
      if (removed.length || nodes.length)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "childList",
          null,
          nodes,
          removed,
        );
    },
    before(...values) {
      const parent = this.parentNode;
      if (!parent) return;
      const owner = factoryDocument(this),
        nodes = values.map((value) =>
          value instanceof Node ? value : owner.createTextNode(String(value)),
        ),
        moving = new Set(nodes);
      let previous = this.previousSibling;
      while (previous && moving.has(previous))
        previous = previous.previousSibling;
      const insertion =
          nodes.length === 1
            ? nodes[0]
            : owner.createDocumentFragment();
      if (nodes.length !== 1)
        for (const node of nodes) insertion.appendChild(node);
      const reference = previous ? previous.nextSibling : parent.firstChild;
      if (nodes.length) parent.insertBefore(insertion, reference);
    },
    after(...values) {
      const parent = this.parentNode;
      if (!parent) return;
      const owner = factoryDocument(this),
        nodes = values.map((value) =>
          value instanceof Node ? value : owner.createTextNode(String(value)),
        ),
        moving = new Set(nodes);
      let reference = this.nextSibling;
      while (reference && moving.has(reference))
        reference = reference.nextSibling;
      const insertion =
        nodes.length === 1
          ? nodes[0]
          : owner.createDocumentFragment();
      if (nodes.length !== 1)
        for (const node of nodes) insertion.appendChild(node);
      if (reference && reference.parentNode !== parent) reference = null;
      if (nodes.length) parent.insertBefore(insertion, reference);
    },
    replaceWith(...values) {
      const parent = this.parentNode;
      if (!parent) return;
      const nodes = values.map((value) =>
          value instanceof Node
            ? value
            : document.createTextNode(String(value)),
        ),
        moving = new Set(nodes);
      let reference = this.nextSibling;
      while (reference && moving.has(reference))
        reference = reference.nextSibling;
      for (const node of nodes) parent.insertBefore(node, reference);
      if (!moving.has(this) && this.parentNode === parent)
        parent.removeChild(this);
    },
    replaceChild(node, child) {
      const parent = child && child.parentNode;
      if (
        !node ||
        !child ||
        !parent ||
        (parent !== this && parent.__handle !== this.__handle)
      )
        throw new Error("replaceChild failed");
      if (node === child) {
        const previousSibling = child.previousSibling,
          nextSibling = child.nextSibling;
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "childList",
          null,
          [],
          [child],
          null,
          previousSibling,
          nextSibling,
        );
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "childList",
          null,
          [child],
          [],
          null,
          previousSibling,
          nextSibling,
        );
        return child;
      }
      const oldParent = node.parentNode,
        oldPrevious = node.previousSibling,
        oldNext = node.nextSibling,
        previousSibling =
          child.previousSibling === node
            ? node.previousSibling
            : child.previousSibling,
        nextSibling =
          child.nextSibling === node ? node.nextSibling : child.nextSibling;
      __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
        (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
      try {
        this.insertBefore(node, child);
        this.removeChild(child);
      } finally {
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
      }
      if (oldParent)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          oldParent,
          "childList",
          null,
          [],
          [node],
          null,
          oldPrevious,
          oldNext,
        );
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "childList",
        null,
        [node],
        [child],
        null,
        previousSibling,
        nextSibling,
      );
      return child;
    },
    dispatchEvent(event) {
      const value = event;
      const controlState =
        value instanceof Event &&
        value.type === "click" &&
        !value.__tilefinchControlDefaultPrepared
          ? __tilefinchRealmGlobal.__tilefinchBeginControlDefault?.(this) || null
          : null;
      const path = shadowEventPath(this, !!value.composed);
      /*
       * The composed path itself is authoritative for connection.  In
       * particular, descendants of the internal element used to represent
       * a shadow root can observe a stale native connected bit during the
       * same task in which their host is inserted.  Never discard the
       * Document already reached by the bounded ancestor walk.
       */
      const reachesDocument =
        path[path.length - 1] instanceof Document ||
        path.some(
          (node) =>
            node === document.documentElement || node === document.body,
        );
      if (reachesDocument) {
        if (!(path[path.length - 1] instanceof Document))
          path.push(document);
        path.push(__tilefinchRealmGlobal);
      }
      const invoke = (target, capture, phase) => {
        value.target = retargetShadowEvent(this, target);
        if (target === __tilefinchRealmGlobal)
          __tilefinchRealmGlobal.__tilefinchInvokeWindowEvent(value, capture);
        else if (target === document)
          __tilefinchRealmGlobal.__tilefinchInvokeDocumentEvent(value, capture);
        else if (typeof target?.__invokeEvent === "function")
          target.__invokeEvent(value, capture);
        else
          __tilefinchRealmGlobal.__tilefinchInvokeEventTarget?.(
            target,
            value,
            capture,
            phase,
          );
      };
      __tilefinchRealmGlobal.__tilefinchPrepareEvent(value, this, path);
      for (let i = path.length - 1; i >= 1 && !value.__stopped; i--)
        invoke(path[i], true, 1);
      if (!value.__stopped) {
        invoke(path[0], true, 2);
        if (!value.__stopped)
          invoke(path[0], false, 2);
      }
      if (value.bubbles && !value.__stopped)
        for (let i = 1; i < path.length && !value.__stopped; i++)
          invoke(path[i], false, 3);
      __tilefinchRealmGlobal.__tilefinchFinishEventDispatch(value);
      value.target = retargetShadowEvent(
        this,
        path[path.length - 1] || this,
      );
      __tilefinchRecordEvent();
      const accepted = !value.defaultPrevented;
      if (controlState)
        __tilefinchRealmGlobal.__tilefinchFinishControlDefault?.(controlState, accepted);
      return accepted;
    },
    __tilefinchGeometryValue() {
      const nativeReceiver = this;
      return __tilefinchGeometry(nativeReceiver.__handle);
    },
    get textContent() {
      const nativeReceiver = this;
      if (shadowRootForHost(this))
        return shadowLightChildren(this)
          .map((child) => child.textContent ?? "")
          .join("");
      return __tilefinchGetText(nativeReceiver.__handle) || "";
    },
    set textContent(value) {
      const nativeReceiver = this;
      value = String(value);
      const type = this.nodeType,
        character =
          type === Node.TEXT_NODE ||
          type === Node.CDATA_SECTION_NODE ||
          type === Node.PROCESSING_INSTRUCTION_NODE ||
          type === Node.COMMENT_NODE,
        oldValue = character ? this.textContent : null,
        removedNodes = character ? [] : mutationChildSnapshot(this);
      if (shadowRootForHost(this) && !character) {
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
          (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
        dynamicPreparationSuppressed++;
        try {
          for (const child of shadowLightChildren(this))
            this.removeChild(child);
          if (value)
            this.appendChild(document.createTextNode(value));
        } finally {
          dynamicPreparationSuppressed--;
          __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
        }
      } else {
        __tilefinchSetText(nativeReceiver.__handle, value);
      }
      if (character)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "characterData",
          null,
          [],
          [],
          oldValue,
        );
      else if (removedNodes.length || value)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "childList",
          null,
          value ? mutationChildSnapshot(this) : [],
          removedNodes,
        );
    },
    get innerHTML() {
      const nativeReceiver = this;
      const shadow = shadowRootForHost(this);
      if (
        shadow &&
        String(this.tagName).toLowerCase() !== "template"
      ) {
        /* The native shadow backing node is an implementation detail, not
           part of the host's light-DOM serialization. Serialize clones in
           a detached container so this read cannot expose or mutate it. */
        const container = document.createElement("div");
        dynamicPreparationSuppressed++;
        try {
          for (const child of shadowLightChildren(this))
            container.appendChild(child.cloneNode(true));
        } finally {
          dynamicPreparationSuppressed--;
        }
        return __tilefinchGetInnerHTML(container.__handle) || "";
      }
      const target =
        String(this.tagName).toLowerCase() === "template"
          ? __tilefinchContent(nativeReceiver.__handle)
          : nativeReceiver.__handle;
      return __tilefinchGetInnerHTML(target) || "";
    },
    set innerHTML(value) {
      const nativeReceiver = this;
      value = String(value);
      const shadow = shadowRootForHost(this),
        customLifecycle =
          __tilefinchRealmGlobal.__tilefinchCustomElementLifecycleNeeded?.(this) ||
          false,
        removedNodes = customLifecycle
          ? Array.from(this.childNodes).slice(0, 256)
          : mutationChildSnapshot(this),
        target =
          String(this.tagName).toLowerCase() === "template"
            ? __tilefinchContent(nativeReceiver.__handle)
            : nativeReceiver.__handle;
      let shadowMutation = false;
      if (
        shadow &&
        String(this.tagName).toLowerCase() !== "template"
      ) {
        /* Parsing directly into the host would delete the internal node
           which owns its ShadowRoot. Parse elsewhere and replace only the
           light children. Dynamic script preparation stays suppressed,
           matching native innerHTML's inert-script semantics. */
        const container = document.createElement("div");
        if (!__tilefinchSetInnerHTML(container.__handle, value))
          throw new Error("innerHTML mutation failed");
        const replacements = Array.from(container.childNodes);
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
          (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
        dynamicPreparationSuppressed++;
        try {
          for (const child of shadowLightChildren(this))
            this.removeChild(child);
          for (const child of replacements) this.appendChild(child);
        } finally {
          dynamicPreparationSuppressed--;
          __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
        }
        shadowMutation = true;
      } else if (!__tilefinchSetInnerHTML(target, value)) {
        throw new Error("innerHTML mutation failed");
      }
      const addedNodes = customLifecycle
        ? Array.from(this.childNodes).slice(0, 256)
        : mutationChildSnapshot(this);
      if (
        customLifecycle &&
        !shadowMutation &&
        String(this.tagName).toLowerCase() !== "template"
      ) {
        for (const node of removedNodes)
          __tilefinchRealmGlobal.__tilefinchCustomElementDisconnected?.(node);
        for (const node of addedNodes)
          __tilefinchRealmGlobal.__tilefinchCustomElementConnected?.(node, true);
      }
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "childList",
        null,
        addedNodes,
        removedNodes,
      );
    },
    get tagName() {
      const nativeReceiver = this;
      return __tilefinchTagName(nativeReceiver.__handle) || "";
    },
    get nodeType() {
      const nativeReceiver = this;
      return __tilefinchNodeType(nativeReceiver.__handle);
    },
    get parentElement() {
      const nativeReceiver = this;
      if (this.__tilefinchDetachedParent)
        return this.__tilefinchDetachedParent.nodeType === Node.ELEMENT_NODE
          ? this.__tilefinchDetachedParent
          : null;
      const parent = wrap(__tilefinchRelation(nativeReceiver.__handle, 8));
      /* The native shadow-root carrier is an internal element so layout can
         flatten it, but its JavaScript identity is a DocumentFragment.
         Deriving parentElement from the native "element parent" relation
         leaked that carrier to children of a shadow root. */
      return parent instanceof Element
        ? __tilefinchRealmGlobal.__tilefinchCanonicalElement(parent)
        : null;
    },
    get parentNode() {
      const nativeReceiver = this;
      if (this.__tilefinchDetachedParent) return this.__tilefinchDetachedParent;
      /* Ancestor walks read this once per step: resolve the common case
         (a parent that already has a wrapper) without the documentElement
         getter or a nodeType query. */
      const parentHandle = __tilefinchRelation(nativeReceiver.__handle, 8);
      if (!parentHandle)
        return this === document.documentElement
          ? document
          : this.parentElement;
      const cached = cachedNode(parentHandle);
      if (cached && cached[nativeDocumentWrapper] !== true) return cached;
      /* <html>, and a comment or doctype beside it, have the native
         document as their parent: the document object, not a wrapper. */
      if (__tilefinchNodeType(parentHandle) === 9) return document;
      const parent = wrap(parentHandle);
      return parent || this.parentElement;
    },
    get firstElementChild() {
      const nativeReceiver = this;
      return __tilefinchRealmGlobal.__tilefinchCanonicalElement(
        wrap(__tilefinchRelation(nativeReceiver.__handle, 1)),
      );
    },
    get firstChild() {
      const nativeReceiver = this;
      return wrap(__tilefinchRelation(nativeReceiver.__handle, 4)) || null;
    },
    get lastChild() {
      const nativeReceiver = this;
      return wrap(__tilefinchRelation(nativeReceiver.__handle, 7)) || null;
    },
    get nextElementSibling() {
      const nativeReceiver = this;
      return __tilefinchRealmGlobal.__tilefinchCanonicalElement(
        shadowAdjustedSibling(
          this,
          wrap(__tilefinchRelation(nativeReceiver.__handle, 2)) || null,
          2,
        ),
      );
    },
    get nextSibling() {
      const nativeReceiver = this;
      return shadowAdjustedSibling(
        this,
        wrap(__tilefinchRelation(nativeReceiver.__handle, 5)) || null,
        5,
      );
    },
    get previousElementSibling() {
      const nativeReceiver = this;
      return __tilefinchRealmGlobal.__tilefinchCanonicalElement(
        shadowAdjustedSibling(
          this,
          wrap(__tilefinchRelation(nativeReceiver.__handle, 3)) || null,
          3,
        ),
      );
    },
    get previousSibling() {
      const nativeReceiver = this;
      return shadowAdjustedSibling(
        this,
        wrap(__tilefinchRelation(nativeReceiver.__handle, 6)) || null,
        6,
      );
    },
    get isConnected() {
      const nativeReceiver = this;
      if (this.__tilefinchDetachedParent)
        for (const at of boundedAncestorPath(
          this.__tilefinchDetachedParent,
          (node) => node.__tilefinchDetachedParent || node.parentNode,
        ))
          if (at instanceof Document) return true;
      return __tilefinchIsConnected(nativeReceiver.__handle);
    },
    get children() {
      const nativeReceiver = this;
      if (!this.__tilefinchChildrenCollection)
        Object.defineProperty(this, "__tilefinchChildrenCollection", {
          configurable: true,
          value: __tilefinchRealmGlobal.__tilefinchLiveHTMLCollection(() =>
            __tilefinchChildren(nativeReceiver.__handle).map(wrap)),
        });
      return this.__tilefinchChildrenCollection;
    },
    get childNodes() {
      const nativeReceiver = this;
      return __tilefinchChildNodes(nativeReceiver.__handle).map(wrap);
    },
    get content() {
      /* This receiver serves every native element, so it must also answer
         HTMLMetaElement.content (a reflected attribute) rather than the
         <template> fragment: ChatGPT's keyboard bootstrap reads the
         viewport meta's content and threw on null. */
      if (String(this.localName).toLowerCase() === "meta")
        return this.getAttribute("content") || "";
      const nativeReceiver = this;
      const content = wrap(__tilefinchContent(nativeReceiver.__handle));
      // Template contents inherit the template contents owner natively;
      // the first read only has the bridge start keeping owner tags.
      if (content && !nodeOwnersActive && templateOwnerDocument())
        setNodeOwner(content, templateContentsOwnerDocument);
      return content;
    },
    set content(value) {
      if (String(this.localName).toLowerCase() === "meta")
        this.setAttribute("content", String(value));
    },
    get contentWindow() {
      const nativeReceiver = this;
      return String(this.tagName).toLowerCase() === "iframe"
        ? __tilefinchRealmGlobal.__tilefinchFrameWindow?.(nativeReceiver.__handle) || null
        : null;
    },
    get contentDocument() {
      const nativeReceiver = this;
      if (
        String(this.tagName).toLowerCase() !== "iframe" ||
        !this.isConnected
      )
        return null;
      __tilefinchRealmGlobal.__tilefinchLoadLocalFrame?.(this);
      const view = __tilefinchRealmGlobal.__tilefinchFrameWindow?.(nativeReceiver.__handle),
        value = view?.document;
      return value instanceof Document ? value : null;
    },
    get attributes() {
      const nativeReceiver = this;
      return namedNodeMapFor(this, () => {
        /* The NamedNodeMap proxy reads the whole list for every length,
           index and iteration step; a loop over it was quadratic in native
           reads. Reuse the list within one task while no script mutation
           happened (every one bumps the DOM version, as for form.elements;
           host-native attribute changes happen between tasks). */
        const version = __tilefinchDomVersion(),
          task = activeTaskSequence,
          memo = attributeState(this)?.memo;
        if (memo && memo.version === version && memo.task === task)
          return memo.values;
        const raw = __tilefinchAttributes(nativeReceiver.__handle),
          shadow = namespaceAttributes(this),
          /* Namespace-aware attributes are written through to the host
             DOM so layout and style can see them, and mirrored here to
             keep the qualified name the author wrote.  The host folds the
             name to lower case, so hide its copy behind the mirror. */
          shadowed = new Set(
            shadow.map((attribute) => String(attribute.name).toLowerCase()),
          ),
          ordinary = raw
            .filter(
              (attribute) =>
                !shadowed.has(String(attribute.name).toLowerCase()),
            )
            .map((attribute) =>
              attributeRecord(
                this,
                String(attribute.name),
                String(attribute.value),
              ),
            ),
          current = [...ordinary, ...shadow],
          order = attributeState(this)?.order || [];
        const values = Object.freeze([
          ...order.filter((attribute) => current.includes(attribute)),
          ...current.filter((attribute) => !order.includes(attribute)),
        ]);
        attributeState(this, true).memo = { version, task, values };
        return values;
      });
    },
    get async() {
      const nativeReceiver = this;
      return (
        this instanceof HTMLScriptElement &&
        (scriptForceAsync(nativeReceiver.__handle) || this.hasAttribute("async"))
      );
    },
    set async(value) {
      const nativeReceiver = this;
      if (this instanceof HTMLScriptElement) scriptAsyncAssigned(nativeReceiver.__handle);
      value ? this.setAttribute("async", "") : this.removeAttribute("async");
    },
    setAttribute(name, value) {
      const nativeReceiver = this;
      name = String(name);
      if (!name || /[\u0000\t\n\f\r ]/.test(name))
        throw new DOMException(
          "Invalid attribute name",
          "InvalidCharacterError",
        );
      const html =
        this.namespaceURI === "http://www.w3.org/1999/xhtml";
      if (html)
        name = name.toLowerCase();
      value = String(value);
      const cachedAttributes = attributeState(this)?.objects,
        matching = html
          ? cachedAttributes?.get(attributeKey(null, name))
          : [...this.attributes].find(
              (attribute) => attribute.name === name,
            ),
        oldValue = html
          ? __tilefinchGetAttribute(nativeReceiver.__handle, name)
          : matching?.value ?? null,
        lowerName = name.toLowerCase();
      let result = false;
      /* Attributes on non-HTML elements keep the qualified name the author
         wrote, which the host DOM cannot store, so they are mirrored in a
         namespace-aware list.  They must still be written through: layout,
         style and the inline-SVG rasterizer read the host DOM only, and a
         scripted <svg> whose width/height/viewBox/d never arrive there
         measures as an empty box. */
      if (!html && matching && namespaceAttributes(this).includes(matching)) {
        matching.__tilefinchAttributeValue = value;
        result = __tilefinchSetAttribute(nativeReceiver.__handle, matching.name, value) || true;
      } else if (
        !matching &&
        this.namespaceURI !== "http://www.w3.org/1999/xhtml"
      ) {
        const record = attributeRecord(this, name, value);
        namespaceAttributes(this, true).push(record);
        result = __tilefinchSetAttribute(nativeReceiver.__handle, name, value) || true;
      } else {
        result = __tilefinchSetAttribute(nativeReceiver.__handle, name, value);
        /*
         * Keep an already-observed NamedNodeMap live without constructing
         * one for ordinary setAttribute calls that never expose it.
         */
        if (!html || cachedAttributes)
          attributeRecord(this, name, value);
      }
      if (lowerName === "id") __tilefinchRealmGlobal.__tilefinchExposeNamedProperty(value);
      if (lowerName === "style" && this.__detachedOwner)
        detachedStyles.delete(this);
      invalidateInlineEventHandler(this, lowerName);
      __tilefinchRealmGlobal.__tilefinchCustomElementAttributeChanged?.(
        this,
        lowerName,
        oldValue,
        value,
      );
      __tilefinchRealmGlobal.__tilefinchCanvasAttributeChanged?.(this, lowerName);
      if (
        this instanceof HTMLIFrameElement &&
        (lowerName === "srcdoc" ||
          (lowerName === "src" && !this.hasAttribute("srcdoc"))) &&
        this.isConnected
      )
        __tilefinchRealmGlobal.__tilefinchLoadLocalFrame?.(this, true);
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "attributes",
        name,
        [],
        [],
        oldValue,
      );
      return result;
    },
    setAttributeNS(namespace, name, value) {
      const nativeReceiver = this;
      namespace = normalizeNamespace(namespace);
      name = String(name);
      const colon = name.indexOf(":");
      if (
        !name ||
        /[\u0000-\u0020]/.test(name) ||
        colon === name.length - 1 ||
        (colon >= 0 && name.indexOf(":", colon + 1) >= 0)
      )
        throw new DOMException(
          "Invalid qualified name",
          "InvalidCharacterError",
        );
      const prefixAt = name.indexOf(":"),
        prefix = prefixAt < 0 ? null : name.slice(0, prefixAt),
        localName = prefixAt < 0 ? name : name.slice(prefixAt + 1),
        xml = "http://www.w3.org/XML/1998/namespace",
        xmlns = "http://www.w3.org/2000/xmlns/";
      if (
        (prefix !== null && namespace === null) ||
        (prefix === "xml" && namespace !== xml) ||
        ((name === "xmlns" || prefix === "xmlns") &&
          namespace !== xmlns) ||
        (namespace === xmlns &&
          name !== "xmlns" &&
          prefix !== "xmlns")
      )
        throw new DOMException("Invalid namespace", "NamespaceError");
      const
        values = namespaceAttributes(this, true),
        existing = [...this.attributes].find(
          (attribute) =>
            attribute.namespaceURI === namespace &&
            attribute.localName === localName,
        ),
        at = existing ? values.indexOf(existing) : -1,
        oldValue = existing?.value ?? null;
      if (existing && at < 0) {
        __tilefinchSetAttribute(nativeReceiver.__handle, existing.name, String(value));
        existing.__tilefinchAttributeValue = String(value);
      } else if (existing) {
        existing.__tilefinchAttributeValue = String(value);
        __tilefinchSetAttribute(nativeReceiver.__handle, existing.name, String(value));
      } else if (at < 0) {
        if (values.length >= 64)
          throw new RangeError("attribute limit exceeded");
        const record = attributeRecord(
          this,
          name,
          String(value),
          namespace,
          prefix,
        );
        values.push(record);
        __tilefinchSetAttribute(nativeReceiver.__handle, name, String(value));
      }
      if (namespace === null)
        __tilefinchRealmGlobal.__tilefinchCanvasAttributeChanged?.(this, localName);
      __tilefinchRealmGlobal.__tilefinchCustomElementAttributeChanged?.(
        this,
        localName,
        oldValue,
        String(value),
        namespace,
      );
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "attributes",
        localName,
        [],
        [],
        oldValue,
        null,
        null,
        namespace,
      );
    },
    getAttribute(name) {
      const nativeReceiver = this;
      name = String(name);
      if (this.namespaceURI === "http://www.w3.org/1999/xhtml")
        name = name.toLowerCase();
      /*
       * The host DOM is authoritative for ordinary HTML attributes.
       * Going through `this.attributes` constructs a NamedNodeMap plus an
       * Attr wrapper graph for every first read.  Focusability checks make
       * several such reads per new link, which retained enough wrapper
       * machinery to exhaust a 4 MiB realm after roughly thirty moves.
       */
      if (this.namespaceURI === "http://www.w3.org/1999/xhtml")
        return __tilefinchGetAttribute(nativeReceiver.__handle, name);
      const found = namespaceAttributes(this).find(
        (attribute) => attribute.name === name,
      );
      return found ? found.value : __tilefinchGetAttribute(nativeReceiver.__handle, name);
    },
    removeAttribute(name) {
      const nativeReceiver = this;
      name = String(name);
      const html =
        this.namespaceURI === "http://www.w3.org/1999/xhtml";
      if (html)
        name = name.toLowerCase();
      const cachedAttributes = attributeState(this)?.objects,
        matching = html
          ? cachedAttributes?.get(attributeKey(null, name))
          : [...this.attributes].find(
              (attribute) => attribute.name === name,
            ),
        oldValue = html
          ? __tilefinchGetAttribute(nativeReceiver.__handle, name)
          : matching?.value ?? null,
        lowerName = name.toLowerCase();
      let result = false;
      if (
        !html &&
        matching !== undefined &&
        namespaceAttributes(this).includes(matching)
      ) {
        const values = namespaceAttributes(this),
          at = values.indexOf(matching);
        if (at >= 0) {
          values.splice(at, 1);
          /* The mirror hid a written-through host attribute; drop both. */
          result = __tilefinchRemoveAttribute(nativeReceiver.__handle, matching.name) || true;
        }
      } else {
        result = __tilefinchRemoveAttribute(nativeReceiver.__handle, name);
      }
      if (matching) {
        matching.__tilefinchAttributeOwner = null;
        cachedAttributes?.delete(
          attributeKey(matching.namespaceURI, matching.localName),
        );
      }
      if (oldValue !== null) {
        if (lowerName === "style" && this.__detachedOwner)
          detachedStyles.delete(this);
        invalidateInlineEventHandler(this, lowerName);
        __tilefinchRealmGlobal.__tilefinchCanvasAttributeChanged?.(this, lowerName);
        if (
          this instanceof HTMLIFrameElement &&
          (lowerName === "srcdoc" ||
            (lowerName === "src" && !this.hasAttribute("srcdoc"))) &&
          this.isConnected
        )
          __tilefinchRealmGlobal.__tilefinchLoadLocalFrame?.(this, true);
        __tilefinchRealmGlobal.__tilefinchCustomElementAttributeChanged?.(
          this,
          lowerName,
          oldValue,
          null,
        );
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "attributes",
          matching?.localName || name,
          [],
          [],
          oldValue,
          null,
          null,
          matching?.namespaceURI ?? null,
        );
      }
      return result;
    },
    removeAttributeNS(namespace, name) {
      const nativeReceiver = this;
      namespace = normalizeNamespace(namespace);
      name = String(name);
      const values = namespaceAttributes(this),
        removed = [...this.attributes].find(
          (attribute) =>
            attribute.namespaceURI === namespace &&
            attribute.localName === name,
        );
      if (removed) {
        const at = values.indexOf(removed);
        if (at >= 0) values.splice(at, 1);
        __tilefinchRemoveAttribute(nativeReceiver.__handle, removed.name);
        removed.__tilefinchAttributeOwner = null;
        attributeState(this)?.objects?.delete(
          attributeKey(namespace, removed.localName),
        );
        if (namespace === null)
          __tilefinchRealmGlobal.__tilefinchCanvasAttributeChanged?.(
            this, removed.localName);
        __tilefinchRealmGlobal.__tilefinchCustomElementAttributeChanged?.(
          this,
          removed.localName,
          removed.value,
          null,
          namespace,
        );
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          this,
          "attributes",
          removed.localName,
          [],
          [],
          removed.value,
          null,
          null,
          namespace,
        );
        return;
      }
      const prefix =
        namespace === "http://www.w3.org/1999/xlink"
          ? "xlink:"
          : namespace === "http://www.w3.org/XML/1998/namespace"
            ? "xml:"
            : "";
      if (prefix) this.removeAttribute(prefix + name);
    },
    /* CSSOM View: the document's scrolling element (the root element in
       no-quirks mode) reports and moves the viewport's scroll position. */
    get scrollTop() {
      if (this === document.documentElement)
        return Number(__tilefinchRealmGlobal.scrollY) || 0;
      return this.__tilefinchGeometryValue().scrollTop;
    },
    set scrollTop(value) {
      if (this === document.documentElement) {
        __tilefinchRealmGlobal.scrollTo(Number(__tilefinchRealmGlobal.scrollX) || 0,
                            Number(value) || 0);
        return;
      }
      const nativeReceiver = this;
      smoothElementScrolls.delete(this);
      const g = this.__tilefinchGeometryValue();
      const overflow = getComputedStyle(this).overflow;
      __tilefinchSetElementScroll(
        nativeReceiver.__handle,
        g.scrollLeft,
        overflow === "visible"
          ? 0
          : Number(value) || 0,
      );
      if (this.scrollTop !== g.scrollTop) queueElementScroll(this);
    },
    get scrollLeft() {
      if (this === document.documentElement)
        return Number(__tilefinchRealmGlobal.scrollX) || 0;
      return this.__tilefinchGeometryValue().scrollLeft;
    },
    set scrollLeft(value) {
      if (this === document.documentElement) {
        __tilefinchRealmGlobal.scrollTo(Number(value) || 0,
                            Number(__tilefinchRealmGlobal.scrollY) || 0);
        return;
      }
      const nativeReceiver = this;
      smoothElementScrolls.delete(this);
      const g = this.__tilefinchGeometryValue();
      const overflow = getComputedStyle(this).overflow;
      __tilefinchSetElementScroll(
        nativeReceiver.__handle,
        overflow === "visible"
          ? 0
          : Number(value) || 0,
        g.scrollTop,
      );
      if (this.scrollLeft !== g.scrollLeft) queueElementScroll(this);
    },
    scrollTo(xOrOptions, y) {
      const nativeReceiver = this;
      if (
        arguments.length === 1 &&
        (xOrOptions === null || typeof xOrOptions !== "object")
      )
        return Promise.reject(
          new TypeError("Single scroll argument must be a dictionary"),
        );
      if (this === document.documentElement) {
        if (arguments.length === 1) __tilefinchRealmGlobal.scrollTo(xOrOptions);
        else __tilefinchRealmGlobal.scrollTo(xOrOptions, y);
        return Promise.resolve();
      }
      if (
        xOrOptions &&
        typeof xOrOptions === "object" &&
        xOrOptions.behavior !== undefined &&
        !["auto", "instant", "smooth"].includes(
          String(xOrOptions.behavior),
        )
      )
        return Promise.reject(new TypeError("Invalid scroll behavior"));
      let left = this.scrollLeft,
        top = this.scrollTop;
      if (typeof xOrOptions === "object" && xOrOptions !== null) {
        left = xOrOptions.left ?? left;
        top = xOrOptions.top ?? top;
      } else {
        left = xOrOptions;
        top = y;
      }
      left = Number(left) || 0;
      top = Number(top) || 0;
      const scrollStyle = getComputedStyle(this),
        requestedBehavior =
          typeof xOrOptions === "object" && xOrOptions !== null
            ? String(xOrOptions.behavior || "auto")
            : "auto",
        behavior =
          requestedBehavior === "auto" &&
          scrollStyle.scrollBehavior === "smooth"
            ? "smooth"
            : requestedBehavior,
        startLeft = this.scrollLeft,
        startTop = this.scrollTop,
        acceptsX = scrollStyle.overflowX !== "visible",
        acceptsY = scrollStyle.overflowY !== "visible",
        apply = (nextLeft, nextTop) => {
          __tilefinchSetElementScroll(
            nativeReceiver.__handle,
            acceptsX ? nextLeft : 0,
            acceptsY ? nextTop : 0,
          );
          this.dispatchEvent(new Event("scroll"));
        };
      smoothElementScrolls.delete(this);
      if (
        behavior !== "smooth" ||
        (left === startLeft && top === startTop)
      ) {
        apply(left, top);
        return Promise.resolve();
      }
      const state = {
        frame: 0,
        lastLeft: startLeft,
        lastTop: startTop,
      };
      smoothElementScrolls.set(this, state);
      return new Promise((resolve) => {
        const step = () => {
          if (smoothElementScrolls.get(this) !== state || !this.isConnected) {
            resolve();
            return;
          }
          if (
            state.frame > 0 &&
            (this.scrollLeft !== state.lastLeft ||
              this.scrollTop !== state.lastTop)
          ) {
            smoothElementScrolls.delete(this);
            resolve();
            return;
          }
          state.frame++;
          const elapsed = state.frame / 12,
            progress = 1 - (1 - elapsed) * (1 - elapsed);
          apply(
            startLeft + (left - startLeft) * progress,
            startTop + (top - startTop) * progress,
          );
          state.lastLeft = this.scrollLeft;
          state.lastTop = this.scrollTop;
          if (state.frame < 12) {
            requestAnimationFrame(step);
          } else {
            smoothElementScrolls.delete(this);
            resolve();
          }
        };
        requestAnimationFrame(step);
      });
    },
    appendChild(child) {
      const nativeReceiver = this;
      const oldParent = child?.parentNode || null,
        oldPrevious = child?.previousSibling || null,
        oldNext = child?.nextSibling || null,
        oldOwner = child?.ownerDocument || null,
        wasConnected = !!child?.isConnected;
      // Nodes from createHTMLDocument/createDocument are detached shim
      // objects until adopted into the live document.  Materialize the
      // element before validation so Web IDL sees the adopted native Node,
      // rather than rejecting a standards-valid cross-document insertion.
      if (
        child &&
        child.__handle === undefined &&
        !(child instanceof DocumentFragment)
      )
        child = __tilefinchRealmGlobal.__tilefinchMaterializeDetachedNode?.(child) || child;
      __tilefinchRealmGlobal.__tilefinchValidatePreInsert?.(this, child, null);
      if (child instanceof DocumentFragment) {
        const nodes = [...child.childNodes],
          removals = nodes.map((node) => ({
            node,
            parent: node.parentNode,
            previousSibling: node.previousSibling,
            nextSibling: node.nextSibling,
          }));
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
          (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
        dynamicPreparationSuppressed++;
        try {
          for (const node of nodes) this.appendChild(node);
        } finally {
          dynamicPreparationSuppressed--;
          __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
        }
        prepareDynamicSubtree?.(nativeReceiver.__handle);
        const removalParents = new Set(
          removals.map((removal) => removal.parent).filter(Boolean),
        );
        for (const parent of removalParents) {
          const group = removals.filter(
            (removal) => removal.parent === parent,
          );
          __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
            parent,
            "childList",
            null,
            [],
            group.map((removal) => removal.node),
            null,
            group[0].previousSibling,
            group[group.length - 1].nextSibling,
          );
        }
        if (nodes.length)
          __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
            this,
            "childList",
            null,
            nodes,
            [],
            null,
            nodes[0].previousSibling,
            null,
          );
        return child;
      }
      if (!child || !__tilefinchAppend(nativeReceiver.__handle, child.__handle))
        throw new Error("appendChild failed");
      if (
        wasConnected &&
        !__tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved
      )
        __tilefinchRealmGlobal.__tilefinchCustomElementDisconnected?.(child);
      const targetOwner = this.ownerDocument || document;
      if (oldOwner && targetOwner && oldOwner !== targetOwner)
        __tilefinchRealmGlobal.__tilefinchAdoptNodeOwner?.(child, targetOwner);
      if (!__tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved)
        __tilefinchRealmGlobal.__tilefinchCustomElementConnected?.(child);
      __tilefinchRealmGlobal.__tilefinchCanvasConnected?.(child);
      if (oldParent)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          oldParent,
          "childList",
          null,
          [],
          [child],
          null,
          oldPrevious,
          oldNext,
        );
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "childList",
        null,
        [child],
        [],
        null,
        child.previousSibling,
        null,
      );
      if (child instanceof HTMLIFrameElement)
        __tilefinchRealmGlobal.__tilefinchLoadLocalFrame?.(child);
      if (!dynamicPreparationSuppressed) prepareDynamicSubtree?.(nativeReceiver.__handle);
      return child;
    },
    cloneNode(deep = false) {
      const nativeReceiver = this;
      if (++cloneCallDepth > cloneDepthLimit) {
        cloneCallDepth--;
        throw new DOMException(
          "DOM clone exceeds the bounded depth limit",
          "NotSupportedError",
        );
      }
      try {
        if (!!deep && String(this.tagName).toLowerCase() === "template") {
          const clone = wrap(__tilefinchClone(nativeReceiver.__handle, false));
          for (const child of this.content.childNodes)
            clone.content.appendChild(child.cloneNode(true));
          __tilefinchRealmGlobal.__tilefinchCopyFormCloneState?.(this, clone, true);
          return clone;
        }
        const clone = wrap(__tilefinchClone(nativeReceiver.__handle, !!deep));
        if (deep && clone) {
          const stack = [{ source: this, node: clone, depth: 0 }];
          while (stack.length) {
            const entry = stack.pop();
            if (entry.depth >= cloneDepthLimit)
              throw new DOMException(
                "DOM clone exceeds the bounded depth limit",
                "NotSupportedError",
              );
            if (
              String(entry.source?.tagName || "").toLowerCase() ===
                "template" &&
              entry.source.content &&
              entry.node.content &&
              entry.node.content.childNodes.length === 0
            )
              for (const child of entry.source.content.childNodes)
                entry.node.content.appendChild(child.cloneNode(true));
            const children = entry.node.childNodes || [];
            const sourceChildren = entry.source?.childNodes || [];
            for (let index = children.length - 1; index >= 0; index--) {
              const child = children[index];
              if (child.__handle === undefined)
                Object.defineProperty(child, "__tilefinchDetachedParent", {
                  configurable: true,
                  writable: true,
                  value: entry.node,
                });
              if (
                child.childNodes?.length ||
                String(child.tagName || "").toLowerCase() === "template"
              )
                stack.push({
                  source: sourceChildren[index],
                  node: child,
                  depth: entry.depth + 1,
                });
            }
          }
        }
        __tilefinchRealmGlobal.__tilefinchCopyFormCloneState?.(this, clone, !!deep);
        return clone;
      } finally {
        cloneCallDepth--;
      }
    },
    insertBefore(node, child) {
      const nativeReceiver = this;
      if (arguments.length < 2)
        throw new TypeError("insertBefore requires two arguments");
      const oldParent = node?.parentNode || null,
        oldPrevious = node?.previousSibling || null,
        oldNext = node?.nextSibling || null,
        oldOwner = node?.ownerDocument || null,
        wasConnected = !!node?.isConnected;
      if (
        node &&
        node.__handle === undefined &&
        !(node instanceof DocumentFragment)
      )
        node = __tilefinchRealmGlobal.__tilefinchMaterializeDetachedNode?.(node) || node;
      __tilefinchRealmGlobal.__tilefinchValidatePreInsert?.(this, node, child);
      if (node instanceof DocumentFragment) {
        const nodes = [...node.childNodes],
          previousSibling = child ? child.previousSibling : this.lastChild;
        __tilefinchRealmGlobal.__tilefinchFragmentInsertCount =
          (__tilefinchRealmGlobal.__tilefinchFragmentInsertCount || 0) + 1;
        __tilefinchRealmGlobal.__tilefinchFragmentInsertText =
          (__tilefinchRealmGlobal.__tilefinchFragmentInsertText || "") +
          String(node.textContent || "").slice(0, 80);
        __tilefinchRealmGlobal.__tilefinchMutationSuppressed =
          (__tilefinchRealmGlobal.__tilefinchMutationSuppressed || 0) + 1;
        try {
          for (const item of nodes) this.insertBefore(item, child);
        } finally {
          __tilefinchRealmGlobal.__tilefinchMutationSuppressed--;
        }
        if (nodes.length) {
          __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
            node,
            "childList",
            null,
            [],
            nodes,
          );
          __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
            this,
            "childList",
            null,
            nodes,
            [],
            null,
            previousSibling,
            child,
          );
        }
        return node;
      }
      if (!child) return this.appendChild(node);
      const previousSibling = child.previousSibling;
      if (!__tilefinchInsertBefore(nativeReceiver.__handle, node.__handle, child.__handle))
        throw new Error("insertBefore failed");
      if (
        wasConnected &&
        !__tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved
      )
        __tilefinchRealmGlobal.__tilefinchCustomElementDisconnected?.(node);
      const targetOwner = this.ownerDocument || document;
      if (oldOwner && targetOwner && oldOwner !== targetOwner)
        __tilefinchRealmGlobal.__tilefinchAdoptNodeOwner?.(node, targetOwner);
      if (!__tilefinchRealmGlobal.__tilefinchCustomElementMovePreserved)
        __tilefinchRealmGlobal.__tilefinchCustomElementConnected?.(node);
      __tilefinchRealmGlobal.__tilefinchCanvasConnected?.(node);
      if (oldParent)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          oldParent,
          "childList",
          null,
          [],
          [node],
          null,
          oldPrevious,
          oldNext,
        );
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        this,
        "childList",
        null,
        [node],
        [],
        null,
        previousSibling,
        child,
      );
      prepareDynamicSubtree?.(nativeReceiver.__handle);
      return node;
    },
    remove() {
      const nativeReceiver = this;
      const parent = this.parentNode,
        previousSibling = this.previousSibling,
        nextSibling = this.nextSibling;
      if (this.__tilefinchDetachedParent) {
        this.__tilefinchDetachedParent.removeChild(this);
        return;
      }
      if (
        __tilefinchRealmGlobal.__tilefinchActiveElement === this ||
        this.contains(__tilefinchRealmGlobal.__tilefinchActiveElement) ||
        shadowEventPath(__tilefinchRealmGlobal.__tilefinchActiveElement, true).includes(this)
      )
        __tilefinchRealmGlobal.__tilefinchActiveElement = document.body;
      const result = __tilefinchRemove(nativeReceiver.__handle);
      if (result) __tilefinchRealmGlobal.__tilefinchCustomElementDisconnected?.(this);
      if (parent)
        __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
          parent,
          "childList",
          null,
          [],
          [this],
          null,
          previousSibling,
          nextSibling,
        );
      return undefined;
    },
  });
  /* The hottest receiver getters run natively for ordinary nodes; each
     keeps its script getter above as the fallback for every other case. */
  for (const [name, kind] of [["parentNode", 0], ["parentElement", 1],
                              ["tagName", 2], ["nodeType", 3],
                              ["firstChild", 4], ["lastChild", 5],
                              ["nextSibling", 6], ["previousSibling", 7]]) {
    const descriptor = nativeNodeReceiverDescriptors[name];
    const fast = descriptor?.get &&
      __tilefinchMakeFastGetter(kind, descriptor.get, "get " + name);
    if (typeof fast === "function") descriptor.get = fast;
  }
  for (const [name, kind] of [["getAttribute", 0]]) {
    const descriptor = nativeNodeReceiverDescriptors[name];
    const fast = typeof descriptor?.value === "function" &&
      __tilefinchMakeFastMethod(kind, descriptor.value, name,
                                descriptor.value.length);
    if (typeof fast === "function") descriptor.value = fast;
  }
  let detachedAttributeMethod;
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchBindDetachedAttributeMethods", {
    configurable: true,
    value(lookup) {
      detachedAttributeMethod = lookup;
      delete __tilefinchRealmGlobal.__tilefinchBindDetachedAttributeMethods;
    },
  });
  /* Attribute operations belong to Element's WebIDL interface, not only
     the per-tag native layer. Libraries call the prototype methods directly
     to bypass an element's override. Publish the same receiver-based
     operations here: ordinary wrappers keep their existing fast path and
     custom elements can delegate with super or Function.prototype.call.
     Secondary documents use their own attribute storage, so explicit
     prototype calls must select those operations without calling a possibly
     author-overridden instance method. */
  for (const name of [
    "getAttribute", "getAttributeNS", "getAttributeNames",
    "getAttributeNode", "getAttributeNodeNS", "setAttribute",
    "setAttributeNS", "setAttributeNode", "setAttributeNodeNS",
    "removeAttribute", "removeAttributeNS", "removeAttributeNode",
    "hasAttribute", "hasAttributeNS", "toggleAttribute",
  ]) {
    const native = nativeNodeReceiverDescriptors[name].value,
      method = { [name](...args) {
        if (!(this instanceof Element))
          throw new TypeError("Attribute operation requires an Element");
        const detached = !(Number(this.__handle) > 0) &&
          this.__detachedOwner && detachedAttributeMethod?.(name);
        return (detached || native).apply(this, args);
      } }[name];
    Object.defineProperty(method, "length", { value: native.length });
    Object.defineProperty(Element.prototype, name, {
      configurable: true, enumerable: true, writable: true, value: method,
    });
  }
  /* ParentNode.replaceChildren lives on the per-tag native layer for speed;
     expose the same function where the standard puts it so feature
     detection on Element.prototype finds it. */
  Object.defineProperty(Element.prototype, "replaceChildren", {
    configurable: true,
    enumerable: true,
    writable: true,
    value: nativeNodeReceiverDescriptors.replaceChildren.value,
  });
  Object.defineProperties(Element.prototype, Object.getOwnPropertyDescriptors({
    hasAttributes() {
      /* Lit asks this of every element it walks; answer from the native
         attribute list instead of materializing a NamedNodeMap. Namespaced
         attributes are written through to the host list too. */
      const handle = Number(this.__handle);
      if (handle > 0)
        return __tilefinchAttributes(handle).length !== 0;
      const attributes = this.attributes;
      return !!attributes && attributes.length !== 0;
    },
    /* CSSOM View checkVisibility over the computed styles Tilefinch keeps.
       Limits: elements the UA stylesheet hides (head, script, style...) are
       recognised by name because computed display does not report them;
       the walk follows parents and shadow hosts, not slot assignment;
       content-visibility:auto never skips content here. */
    checkVisibility(options = {}) {
      if (!this.isConnected) return false;
      const checkOpacity = !!(options?.checkOpacity || options?.opacityProperty),
        checkVisibilityCSS =
          !!(options?.checkVisibilityCSS || options?.visibilityProperty);
      for (let at = this, steps = 0; at && steps < ancestorLimit; steps++) {
        if (at.namespaceURI === "http://www.w3.org/1999/xhtml" &&
            hiddenByDefaultTags.includes(" " + at.localName + " "))
          return false;
        const style = getComputedStyle(at),
          display = style.getPropertyValue("display");
        if (display === "none" || (at === this && display === "contents"))
          return false;
        if (at !== this &&
            style.getPropertyValue("content-visibility") === "hidden")
          return false;
        if (checkOpacity && Number(style.getPropertyValue("opacity")) === 0)
          return false;
        if (at === this && checkVisibilityCSS) {
          const visibility = style.getPropertyValue("visibility");
          if (visibility && visibility !== "visible") return false;
        }
        const parent = at.parentNode;
        at = parent instanceof ShadowRoot
          ? parent.host
          : parent?.nodeType === Node.ELEMENT_NODE ? parent : null;
      }
      return true;
    },
  }));
  const hiddenByDefaultTags = " area base basefont datalist head link meta " +
    "noembed noframes param rp script style template title ";
  const nativeNodeEventDescriptors = Object.getOwnPropertyDescriptors({
    addEventListener(type, callback, options = false) {
      type = String(type);
      /* Parsed markup predates JavaScript registration. Materialize its event
       * handler slot before appending an author listener so first-dispatch
       * ordering matches a handler installed while parsing. */
      inlineEventHandler(this, type, "on" + type);
      /* Transitions from stylesheets have no timeline of their own: the
         motion module watches an element once something listens for them. */
      if (type.startsWith("transition") || type === "webkitTransitionEnd") {
        try {
          if (typeof __tilefinchRealmGlobal.__tilefinchWatchTransitions !== "function")
            __tilefinchRealmGlobal.__tilefinchEnsureMotionBootstrap?.();
          __tilefinchRealmGlobal.__tilefinchWatchTransitions?.(this, type);
        } catch {}
      }
      const retained = listenerMap(this, true);
      const listeners = retained || this[wrapperListeners] || new Map();
      this[wrapperListeners] = listeners;
      return __tilefinchRealmGlobal.__tilefinchAddEventListener?.(
        listeners, type, callback, options,
      );
    },
    removeEventListener(type, callback, options = false) {
      const listeners = this[wrapperListeners];
      if (listeners)
        return __tilefinchRealmGlobal.__tilefinchRemoveEventListener?.(
          listeners, type, callback, options,
        );
    },
    __invokeEvent(value, capture) {
      value.currentTarget = this;
      value.eventPhase = this === value.target ? 2 : capture ? 1 : 3;
      if (!capture) inlineEventHandler(this, value.type, "on" + value.type);
      const listeners = this[wrapperListeners];
      if (listeners)
        __tilefinchRealmGlobal.__tilefinchInvokeListenerList?.(
          listeners, this, value, capture,
        );
    },
  });
  const nativePrototypes = new WeakMap();
  const nativeLayer = (base) => {
    let prototype = nativePrototypes.get(base);
    if (!prototype) {
      if (sharedLayerPrototypes.includes(base)) {
        /* A member-less interface: its prototype itself sits above
           HTMLElement's layer, so its wrappers inherit the same native
           members without another copy of the layer. */
        nativeLayer(HTMLElement.prototype);
        prototype = base;
      } else {
        prototype = Object.create(base);
        Object.defineProperties(prototype, nativeNodeEventDescriptors);
        Object.defineProperties(prototype, nativeNodeReceiverDescriptors);
        if (base === HTMLElement.prototype)
          for (const shared of sharedLayerPrototypes)
            Object.setPrototypeOf(shared, prototype);
      }
      nativePrototypes.set(base, prototype);
    }
    return prototype;
  };
  function nativeElementPrototype(tag, type, namespace) {
    return nativeLayer(elementPrototype(tag, type, namespace));
  }
  /* Keeps the native members on a node whose prototype is about to become
     `target` (an upgrade). Members that `target`'s own classes define are
     left to them: an own copy would shadow them, and Lit would save it as
     an initial property value (MDN's runner got a DOMTokenList for its
     string `sandbox`). */
  let sharedCustomElementMembers = false;
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchPrepareNativePrototype", {
    value(node, target) {
      if (!(node?.__handle > 0)) return;
      if (target && Object.prototype.isPrototypeOf.call(HTMLElement.prototype, target)) {
        if (!sharedCustomElementMembers) try {
          /* Preserve author prototype identity without copying the native
             member table (or enumerating its keys) for every component. */
          for (const descriptors of [nativeNodeEventDescriptors, nativeNodeReceiverDescriptors])
            for (const name of Object.keys(descriptors))
              if (!Object.hasOwn(HTMLElement.prototype, name))
                Object.defineProperty(HTMLElement.prototype, name, descriptors[name]);
          sharedCustomElementMembers = true;
        } catch (_) { /* Frozen/refused bases keep per-instance fallback. */ }
        if (sharedCustomElementMembers) return;
      }
      const classDefined = (name) => {
        for (let p = target; p && p !== Element.prototype;
             p = Object.getPrototypeOf(p))
          if (Object.prototype.hasOwnProperty.call(p, name)) return true;
        return false;
      };
      for (const descriptors of [nativeNodeEventDescriptors, nativeNodeReceiverDescriptors])
        for (const name of Object.keys(descriptors))
          if (!Object.prototype.hasOwnProperty.call(node, name) &&
              !(target && classDefined(name)))
            Object.defineProperty(node, name, descriptors[name]);
    },
  });
  const nodeIdentity = ["", 0, null, 0];
  let wrapperTrace = null;
  function wrap(handle) {
    if (!handle) return null;
    const cached = cachedNode(handle);
    if (cached) return cached;
    retentionStats.wrapperCreates++;
    /* [tagName || "", nodeType, namespaceURI, held-in] in one bridge call,
       refilling one array rather than allocating one per wrapper. */
    const held = heldEventStates.size;
    __tilefinchNodeIdentity(handle, nodeIdentity, held);
    const tagName = nodeIdentity[0],
      nodeType = nodeIdentity[1],
      namespaceURI = nodeIdentity[2],
      /* With a tree held: the handle whose holder this wrapper joins (the
         holder of its detached tree, if it has one), or 0. */
      heldIn = held && nodeIdentity[3];
    // Start with the final prototype and final descriptor flags. Changing
    // either after filling the wrapper de-interns QuickJS's large property
    // shape, leaving a separate shape allocation for every visited element.
    const node = Object.create(
      nativeElementPrototype(tagName, nodeType, namespaceURI),
    );
    node.__namespaceURI = namespaceURI;
    if (nodeType === Node.DOCUMENT_NODE)
      Object.defineProperty(node, nativeDocumentWrapper, { value: true });
    Object.defineProperty(node, "__handle", {
      enumerable: false,
      configurable: false,
      get: () => handle,
    });
    installStyle(node, handle);
    installClassList(node);
    // Reserve the slot up front to keep wrapper shapes shared, but allocate
    // a listener map only on registration. Existing retained listeners survive
    // wrapper recreation; overflow maps remain owned by their live wrapper.
    Object.defineProperty(node, wrapperListeners, {
      value: nativeNodeListeners.get(Number(handle)) ||
        heldEventState(Number(handle))?.listeners || null,
      writable: true,
    });
    const exposed =
      wrapperTrace !== null
        ? wrapperTrace(
            node,
            "element." + String(node.tagName || "unknown").toLowerCase(),
          )
        : node;
    /* A wrapper of a held tree joins its holder before it becomes the
       node's live wrapper: were the stamp refused after, the tree could
       outlive the wrappers keeping its holder. */
    if (heldIn) {
      const holder = heldEventHolder(heldIn);
      if (holder) node[wrapperEventHolder] = holder;
    }
    cacheNode(handle, exposed);
    __tilefinchRealmGlobal.__tilefinchRestoreCustomElement?.(exposed);
    return exposed;
  }
  __tilefinchRealmGlobal.__tilefinchWrap = wrap;
  /* The lab's capability trace wraps new node wrappers. Only test-probe
     realms can install it, through a fixed setter; the shipping realm has no
     such hook, and a page-defined global never substitutes wrappers. */
  if (__tilefinchRealmGlobal.__tilefinchTestProbes)
    Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchSetWrapperTrace", {
      value: (trace) => {
        wrapperTrace = typeof trace === "function" ? trace : null;
      },
    });
  /* Tests drop every cached wrapper to exercise rewrapping; nothing in
     the bootstrap clears the cache, so only test-probe realms get it. */
  if (__tilefinchRealmGlobal.__tilefinchTestProbes)
    __tilefinchRealmGlobal.__tilefinchClearNodeCache = () => {
      nodeCache.clear();
    };
  const hitTestElements = (x, y) => {
    x = Number(x);
    y = Number(y);
    if (!Number.isFinite(x) || !Number.isFinite(y))
      throw new TypeError("Coordinates must be finite");
    if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) return [];
    const hits = [];
    let sequence = 0;
    for (const element of document.querySelectorAll("*")) {
      const style = getComputedStyle(element);
      if (
        style.display === "none" ||
        style.visibility === "hidden" ||
        style.pointerEvents === "none" ||
        (!element.firstChild &&
          !Number.isFinite(parseFloat(style.height)) &&
          style.position === "static")
      )
        continue;
      const rect = element.getBoundingClientRect();
      if (
        rect.width > 0 &&
        rect.height > 0 &&
        x >= rect.left &&
        x <= rect.right &&
        y >= rect.top &&
        y <= rect.bottom
      )
        hits.push({
          element,
          zIndex: Number.parseInt(style.zIndex, 10) || 0,
          phase:
            style.position === "absolute" ||
            style.position === "fixed" ||
            style.position === "sticky" ||
            Number.isFinite(Number.parseInt(style.zIndex, 10))
              ? 1
              : 0,
          sequence: sequence++,
        });
      for (const pseudo of ["::before", "::after"]) {
        const pseudoStyle = getComputedStyle(element, pseudo);
        if (
          pseudoStyle.content === "none" ||
          pseudoStyle.display === "none" ||
          pseudoStyle.visibility === "hidden" ||
          pseudoStyle.pointerEvents === "none"
        )
          continue;
        const width = parseFloat(pseudoStyle.width),
          height = parseFloat(pseudoStyle.height);
        if (!(width > 0 && height > 0)) continue;
        const left = rect.left + (parseFloat(pseudoStyle.left) || 0),
          top = rect.top + (parseFloat(pseudoStyle.top) || 0);
        if (
          x >= left &&
          x <= left + width &&
          y >= top &&
          y <= top + height
        )
          hits.push({
            element,
            zIndex: Number.parseInt(pseudoStyle.zIndex, 10) || 0,
            phase:
              pseudoStyle.position === "absolute" ||
              pseudoStyle.position === "fixed" ||
              pseudoStyle.position === "sticky" ||
              Number.isFinite(
                Number.parseInt(pseudoStyle.zIndex, 10),
              )
                ? 1
                : 0,
            sequence: sequence++,
          });
      }
    }
    hits.sort(
      (left, right) =>
        left.phase - right.phase ||
        left.zIndex - right.zIndex ||
        left.sequence - right.sequence,
    );
    return hits.map((hit) => hit.element).reverse();
  };
  document.elementsFromPoint = function (x, y) {
    if (arguments.length < 2) throw new TypeError("Two coordinates required");
    return hitTestElements(x, y);
  };
  document.elementFromPoint = function (x, y) {
    if (arguments.length < 2) throw new TypeError("Two coordinates required");
    return hitTestElements(x, y)[0] || null;
  };
  __tilefinchRealmGlobal.__tilefinchDocumentListeners = documentListeners;
  const mutationObservers = [],
    pendingMutationObservers = new Set(),
    mutationObserverStates = { get: null };
  let mutationDeliveryPending = false,
    mutationDeliveryActive = false,
    parserInsertionsArmed = false;
  /* The native parser journals its own insertions only while some observer
     can receive childList records (see DocumentParserInsertionJournal);
     unobserved documents pay nothing during parsing. */
  const observeParserInsertions = __tilefinchObserveParserInsertions,
    nativeNodeType = __tilefinchNodeType,
    syncParserInsertionJournal = () => {
      const armed = mutationObservers.some((observer) =>
        mutationObserverStates
          .get(observer)
          ?.targets.some((item) => item.options.childList),
      );
      if (armed === parserInsertionsArmed) return;
      parserInsertionsArmed = armed;
      observeParserInsertions(armed);
    },
    parserInsertionNode = (handle) =>
      !handle ? null : nativeNodeType(handle) === 9 ? document : wrap(handle);
  /* A delivery holds at most mutationRecordLimit exact records per
     observer. Further childList changes fold into one trailing record whose
     addedNodes are a few subtree roots containing every added node (roots
     merge at their common ancestor, which may be a node that already
     existed), so an observer that scans addedNodes subtrees still finds
     everything while memory stays bounded. Removed nodes are kept up to the
     same bound. Other record types past the limit are dropped and counted. */
  const mutationRecordLimit = 256,
    overflowRootLimit = 16,
    overflowAncestorLimit = 256,
    overflowCommonAncestor = (left, right) => {
      const chain = new Set();
      for (
        let at = left, steps = 0;
        at && steps < overflowAncestorLimit;
        at = at.parentNode, steps++
      )
        chain.add(at);
      for (
        let at = right, steps = 0;
        at && steps < overflowAncestorLimit;
        at = at.parentNode, steps++
      )
        if (chain.has(at)) return at;
      return null;
    },
    overflowContains = (root, node) => {
      for (
        let at = node, steps = 0;
        at && steps < overflowAncestorLimit;
        at = at.parentNode, steps++
      )
        if (at === root) return true;
      return false;
    },
    foldOverflowRoot = (roots, node) => {
      if (!node || roots.some((root) => overflowContains(root, node)))
        return true;
      if (roots.length < overflowRootLimit) {
        roots.push(node);
        return true;
      }
      const merged = overflowCommonAncestor(roots[roots.length - 1], node);
      if (!merged || merged === document) return false;
      const kept = roots.filter((root) => !overflowContains(merged, root));
      roots.length = 0;
      roots.push(...kept, merged);
      return true;
    },
    foldOverflowRecord = (state, target, addedNodes, removedNodes) => {
      let record = state.overflowRecord;
      if (!record) {
        record = state.overflowRecord = new MutationRecord({
          type: "childList",
          target,
          addedNodes: [],
          removedNodes: [],
        });
        state.records.push(record);
      } else if (record.target !== target)
        record.target =
          overflowCommonAncestor(record.target, target) || record.target;
      let folded = true;
      for (const node of addedNodes)
        folded = foldOverflowRoot(record.addedNodes, node) && folded;
      for (const node of removedNodes) {
        if (record.removedNodes.length >= mutationRecordLimit) {
          folded = false;
          break;
        }
        record.removedNodes.push(node);
      }
      retentionStats.recordsCoalesced++;
      return folded;
    };
  /* Called by the parser's microtask checkpoint before a parser-blocking
     script and once after EOF, with [parent, node, previous, next] handle
     quadruples in insertion order. Each becomes the childList record the
     parser's insertion would have queued; the checkpoint's microtask run
     then delivers them before the script executes. */
  __tilefinchRealmGlobal.__tilefinchParserMutationCheckpoint = (batch) => {
    const length = Math.min(Number(batch?.length) || 0, 0x10000);
    retentionStats.parserRecordsCoalesced = Number(batch?.coalesced) || 0;
    if (!parserInsertionsArmed) return;
    for (let index = 0; index + 3 < length; index += 4) {
      const parent = parserInsertionNode(batch[index]),
        node = parserInsertionNode(batch[index + 1]);
      if (!parent || !node) continue;
      __tilefinchRealmGlobal.__tilefinchNotifyMutation?.(
        parent,
        "childList",
        null,
        [node],
        [],
        null,
        parserInsertionNode(batch[index + 2]),
        parserInsertionNode(batch[index + 3]),
      );
    }
  };
  __tilefinchRealmGlobal.MutationRecord = class MutationRecord {
    constructor(init = {}) {
      this.type = String(init.type || "");
      this.target = init.target || null;
      this.addedNodes = init.addedNodes || [];
      this.removedNodes = init.removedNodes || [];
      this.previousSibling = init.previousSibling ?? null;
      this.nextSibling = init.nextSibling ?? null;
      this.attributeName = init.attributeName ?? null;
      this.attributeNamespace = init.attributeNamespace ?? null;
      this.oldValue = init.oldValue ?? null;
    }
  };
  const mutationObserverInterest = (state) => {
    let interest = 0,
      subtree = false;
    for (const { options } of state.targets) {
      if (options.attributes) interest |= 1;
      if (options.childList) interest |= 2;
      if (options.characterData) interest |= 4;
      if (options.subtree) subtree = true;
    }
    state.interest = interest;
    state.subtree = subtree;
  };
  const NativeMutationObserver = class MutationObserver {
    #state;
    static {
      /* A callback can retain its observer through its component owner.
         Keep that cycle on the observer, not in a global WeakMap value. */
      mutationObserverStates.get = observer =>
        observer && typeof observer === "object" && #state in observer
          ? observer.#state : undefined;
    }
    constructor(callback) {
      if (typeof callback !== "function")
        throw new TypeError("callback required");
      this.#state = {
        callback,
        targets: [],
        records: [],
        overflowRecord: null,
        transientRoots: [],
        pending: false,
        /* Record types some registration takes (bits: attributes 1,
           childList 2, characterData 4), and
           whether any observes a subtree (removals then create transient
           registrations): queueMutationRecords skips the rest unread. */
        interest: 0,
        subtree: false,
      };
    }
    observe(target, options = {}) {
      const state = mutationObserverStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (!(target instanceof Node)) throw new TypeError("Node required");
      options = Object(options);
      const normalized = {
        childList: !!options.childList,
        subtree: !!options.subtree,
        attributes:
          options.attributes === undefined
            ? options.attributeOldValue !== undefined ||
              options.attributeFilter !== undefined
            : !!options.attributes,
        attributeOldValue: !!options.attributeOldValue,
        attributeFilter:
          options.attributeFilter === undefined
            ? null
            : Array.from(options.attributeFilter, String),
        characterData:
          options.characterData === undefined
            ? options.characterDataOldValue !== undefined
            : !!options.characterData,
        characterDataOldValue: !!options.characterDataOldValue,
      };
      if (
        (!normalized.childList &&
          !normalized.attributes &&
          !normalized.characterData) ||
        (!normalized.attributes &&
          (normalized.attributeOldValue ||
            normalized.attributeFilter !== null)) ||
        (!normalized.characterData && normalized.characterDataOldValue)
      )
        throw new TypeError("Invalid MutationObserver options");
      const registration = {
        target,
        /* For the native subtree check; see mutationAncestor. */
        handle: target.__handle > 0 ? target.__handle : 0,
        options: normalized,
      };
      const at = state.targets.findIndex((item) => item.target === target);
      if (at >= 0) {
        const previous = state.targets[at];
        state.targets[at] = registration;
        state.transientRoots = state.transientRoots.filter(
          (item) => item.registration !== previous,
        );
      }
      else state.targets.push(registration);
      mutationObserverInterest(state);
      if (!mutationObservers.includes(this)) {
        /* Observer registrations retain callback closures and target
           wrappers. Sixty-four covers mature test/framework fan-out while
           the QuickJS heap remains the authoritative PSP memory ceiling. */
        if (mutationObservers.length < 64) mutationObservers.push(this);
        else retentionStats.observerDrops++;
      }
      syncParserInsertionJournal();
    }
    disconnect() {
      const state = mutationObserverStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      state.targets = [];
      state.records = [];
      state.overflowRecord = null;
      state.transientRoots = [];
      state.pending = false;
      mutationObserverInterest(state);
      pendingMutationObservers.delete(this);
      const at = mutationObservers.indexOf(this);
      if (at >= 0) mutationObservers.splice(at, 1);
      syncParserInsertionJournal();
    }
    takeRecords() {
      const state = mutationObserverStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      const records = state.records.splice(0);
      state.overflowRecord = null;
      return records;
    }
  };
  __tilefinchRealmGlobal.MutationObserver = NativeMutationObserver;
  /* WebIDL operations are enumerable, unlike authored class methods.
     Enumeration-based wrappers must discover these on an observer instance. */
  for (const name of ["observe", "disconnect", "takeRecords"])
    Object.defineProperty(NativeMutationObserver.prototype, name, {
      enumerable: true,
    });
  const mutationObserverObserve = NativeMutationObserver.prototype.observe,
    mutationObserverTakeRecords = NativeMutationObserver.prototype.takeRecords;
  /* Motion is installed lazily, after pages may replace the public
     constructor or its methods. Keep its observer in this realm's original
     bounded observer machinery, without invoking authored wrappers. */
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchObserveMutations", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: (callback, target, options) => {
      const observer = new NativeMutationObserver(callback);
      ancestorApply(mutationObserverObserve, observer, [target, options]);
      return observer;
    },
  });
  Object.defineProperty(NativeMutationObserver.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "MutationObserver",
  });
  {
    const observers = new Set(),
      resizeStates = { get: null },
      resizeTargetLimit = 1024,
      resizeState = (observer) => {
        const state = resizeStates.get(observer);
        if (!state) throw new TypeError("Illegal invocation");
        return state;
      };
    let resizeTargets = 0;
    let resizeRecheckPending = false,
      resizeDeliveryActive = false;
    class ResizeObserverSize {
      constructor(inlineSize, blockSize) {
        this.inlineSize = inlineSize;
        this.blockSize = blockSize;
      }
    }
    class ResizeObserverEntry {
      constructor(target, geometry) {
        this.target = target;
        this.contentRect = geometry.contentRect;
        this.borderBoxSize = [
          new ResizeObserverSize(
            geometry.borderWidth,
            geometry.borderHeight,
          ),
        ];
        this.contentBoxSize = [
          new ResizeObserverSize(
            geometry.contentWidth,
            geometry.contentHeight,
          ),
        ];
        this.devicePixelContentBoxSize = [
          new ResizeObserverSize(
            geometry.contentWidth,
            geometry.contentHeight,
          ),
        ];
      }
    }
    const finiteLength = (style, name) => {
        const value = parseFloat(style.getPropertyValue(name));
        return Number.isFinite(value) ? Math.max(0, value) : 0;
      },
      resizeGeometry = (target) => {
        /*
         * A disconnected element has no rendered box. Do this before
         * consulting authored width/height: declarations remain readable
         * while detached but must not keep the last observation alive.
         */
        if (!target.isConnected)
          return {
            contentRect: new DOMRect(),
            contentWidth: 0,
            contentHeight: 0,
            borderWidth: 0,
            borderHeight: 0,
          };
        const rect = target.getBoundingClientRect(),
          style = getComputedStyle(target);
        if (style.display === "none")
          return {
            contentRect: new DOMRect(),
            contentWidth: 0,
            contentHeight: 0,
            borderWidth: 0,
            borderHeight: 0,
          };
        const paddingLeft = finiteLength(style, "padding-left"),
          paddingRight = finiteLength(style, "padding-right"),
          paddingTop = finiteLength(style, "padding-top"),
          paddingBottom = finiteLength(style, "padding-bottom"),
          borderLeft = finiteLength(style, "border-left-width"),
          borderRight = finiteLength(style, "border-right-width"),
          borderTop = finiteLength(style, "border-top-width"),
          borderBottom = finiteLength(style, "border-bottom-width"),
          /*
           * Preserve authored fractional dimensions when the retained native
           * style representation has rounded its computed layout value.  The
           * inline declaration wins here only as the already-cascaded style
           * source for this lightweight geometry bridge.
           */
          authoredWidth = parseFloat(
            target.style?.getPropertyValue("width") ||
              style.getPropertyValue("width"),
          ),
          authoredHeight = parseFloat(
            target.style?.getPropertyValue("height") ||
              style.getPropertyValue("height"),
          ),
          borderBox = style.boxSizing === "border-box",
          nonReplacedInline =
            style.display === "inline" &&
            !["IMG", "VIDEO", "CANVAS", "SVG"].includes(target.tagName),
          contentWidth = Math.max(
            0,
            nonReplacedInline
              ? 0
              : Number.isFinite(authoredWidth)
              ? authoredWidth -
                  (borderBox
                    ? paddingLeft + paddingRight + borderLeft + borderRight
                    : 0)
              : rect.width -
                  paddingLeft -
                  paddingRight -
                  borderLeft -
                  borderRight,
          ),
          contentHeight = Math.max(
            0,
            nonReplacedInline
              ? 0
              : Number.isFinite(authoredHeight)
              ? authoredHeight -
                  (borderBox
                    ? paddingTop + paddingBottom + borderTop + borderBottom
                    : 0)
              : rect.height -
                  paddingTop -
                  paddingBottom -
                  borderTop -
                  borderBottom,
          );
        return {
          contentRect: new DOMRect(
            paddingLeft,
            paddingTop,
            contentWidth,
            contentHeight,
          ),
          contentWidth,
          contentHeight,
          borderWidth:
            contentWidth +
            paddingLeft +
            paddingRight +
            borderLeft +
            borderRight,
          borderHeight:
            contentHeight +
            paddingTop +
            paddingBottom +
            borderTop +
            borderBottom,
        };
      },
      observedDimensions = (geometry, box) =>
        box === "border-box"
          ? [geometry.borderWidth, geometry.borderHeight]
          : [geometry.contentWidth, geometry.contentHeight],
      resizeTargetDepth = (target) => {
        let depth = 1,
          at = target;
        for (let steps = 0; at && steps < ancestorLimit; steps++) {
          at =
            at instanceof ShadowRoot
              ? at.host
              : at.__tilefinchDetachedParent || at.parentNode;
          if (at) depth++;
        }
        return depth;
      },
      gatherResizeObservations = (depthLimit) => {
        const batches = [];
        let shallowestDepth = Infinity,
          skipped = false;
        for (const observer of observers) {
          const state = resizeState(observer);
          if (!state.targets.size) continue;
          const entries = [];
          for (const [target, options] of state.targets) {
            const geometry = resizeGeometry(target),
              dimensions = observedDimensions(geometry, options.box),
              previous = state.lastSizes.get(target);
            if (
              previous &&
              previous[0] === dimensions[0] &&
              previous[1] === dimensions[1]
            )
              continue;
            const depth = resizeTargetDepth(target);
            if (depth <= depthLimit) {
              skipped = true;
              continue;
            }
            state.lastSizes.set(target, dimensions);
            shallowestDepth = Math.min(shallowestDepth, depth);
            if (entries.length < 128)
              entries.push(new ResizeObserverEntry(target, geometry));
            else retentionStats.recordDrops++;
          }
          if (entries.length) batches.push([observer, entries]);
        }
        return { batches, shallowestDepth, skipped };
      },
      deliverResizeObservations = () => {
        resizeRecheckPending = false;
        resizeDeliveryActive = true;
        let depth = 0,
          skipped = false,
          iterations = 0;
        /*
         * Deliver repeated observations within one rendering step. After
         * each broadcast only deeper targets remain eligible; changed
         * shallower targets are deferred and reported through Window.onerror.
         */
        while (iterations++ < 64) {
          const gathered = gatherResizeObservations(depth);
          skipped ||= gathered.skipped;
          if (!gathered.batches.length) break;
          depth = gathered.shallowestDepth;
          for (const [observer, entries] of gathered.batches) {
            const state = resizeState(observer);
            if (!state.targets.size) continue;
            try {
              __tilefinchRealmGlobal.__tilefinchRunTask(
                "resize-observer",
                state.callback,
                observer,
                [entries, observer],
              );
            } catch (error) {
              __tilefinchReportUncaught(error, "ResizeObserver");
            }
          }
        }
        if (iterations > 64) skipped = true;
        resizeDeliveryActive = false;
        if (skipped) {
          __tilefinchReportUncaught(
            new Error(
              "ResizeObserver loop completed with undelivered notifications.",
            ),
            "ResizeObserver loop",
          );
          scheduleResizeRecheck();
        }
      },
      scheduleResizeRecheck = () => {
        if (!observers.size) return false;
        if (resizeDeliveryActive) return true;
        if (resizeRecheckPending) return true;
        resizeRecheckPending = true;
        const schedule =
          __tilefinchRealmGlobal.__tilefinchScheduleRenderObserver || queueMicrotask;
        schedule(deliverResizeObservations);
        return true;
      };
    __tilefinchRealmGlobal.__tilefinchResizeRecheck = scheduleResizeRecheck;
    __tilefinchRealmGlobal.ResizeObserverSize = ResizeObserverSize;
    __tilefinchRealmGlobal.ResizeObserverEntry = ResizeObserverEntry;
    __tilefinchRealmGlobal.ResizeObserver = class ResizeObserver {
      #state;
      static {
        resizeStates.get = observer =>
          observer && typeof observer === "object" && #state in observer
            ? observer.#state : undefined;
      }
      constructor(callback) {
        if (typeof callback !== "function")
          throw new TypeError("callback required");
        /* Empty observers have no delivery work and must not be retained.
           Bound live registrations, not the number of component instances:
           pages commonly create one observer per card. */
        this.#state = {
          callback, targets: new Map(), lastSizes: new Map(),
        };
      }
      observe(target, options = {}) {
        const state = resizeState(this);
        if (!(target instanceof Element))
          throw new TypeError("Element required");
        const box = String(options?.box || "content-box");
        if (
          box !== "content-box" &&
          box !== "border-box" &&
          box !== "device-pixel-content-box"
        )
          throw new TypeError("Invalid ResizeObserver box");
        /* Option getters may register other observations. Admit against
           the current state after coercion, and roll back on allocation
           refusal before charging the live-registration count. */
        const added = !state.targets.has(target);
        if (added &&
            (state.targets.size >= 128 || resizeTargets >= resizeTargetLimit))
          throw new RangeError("ResizeObserver target limit reached");
        const wasActive = observers.has(this);
        observers.add(this);
        try {
          state.targets.set(target, { box });
        } catch (error) {
          if (!wasActive) observers.delete(this);
          throw error;
        }
        if (added) resizeTargets++;
        scheduleResizeRecheck();
      }
      unobserve(target) {
        const state = resizeState(this);
        if (!(target instanceof Element))
          throw new TypeError("Element required");
        if (state.targets.delete(target)) resizeTargets--;
        state.lastSizes.delete(target);
        if (!state.targets.size) observers.delete(this);
      }
      disconnect() {
        const state = resizeState(this);
        resizeTargets -= state.targets.size;
        state.targets.clear();
        state.lastSizes.clear();
        observers.delete(this);
      }
    };
  }
  {
    __tilefinchRealmGlobal.IntersectionObserverEntry = class IntersectionObserverEntry {
      constructor(init = {}) {
        /* Copy entry fields as own data properties. Compatibility shims may
           install getter-only prototype fields; ordinary assignment must
           not invoke those inherited accessors and abort observer delivery. */
        Object.defineProperties(this, Object.getOwnPropertyDescriptors({ ...init }));
      }
    };
    Object.defineProperty(
      IntersectionObserverEntry.prototype,
      "intersectionRatio",
      { value: 0, writable: true, configurable: true },
    );
    const parseMargin = (text) => {
      const source = String(text ?? "0px").trim() || "0px",
        parts = source.split(/\s+/);
      if (parts.length > 4) throw new SyntaxError("Invalid rootMargin");
      const tokens = parts.map((part) => {
        const match = /^([+-]?(?:\d+(?:\.\d*)?|\.\d+))(px|%)$/.exec(part);
        if (!match) throw new SyntaxError("Invalid rootMargin");
        const value = Number(match[1]);
        if (!Number.isFinite(value))
          throw new SyntaxError("Invalid rootMargin");
        return { value, unit: match[2] };
      });
      if (tokens.length === 1) tokens.push(tokens[0], tokens[0], tokens[0]);
      else if (tokens.length === 2) tokens.push(tokens[0], tokens[1]);
      else if (tokens.length === 3) tokens.push(tokens[1]);
      return {
        tokens,
        text: tokens.map((token) => `${token.value}${token.unit}`).join(" "),
      };
    };
    const resolveMargin = (parsed, width, height) =>
      parsed.tokens.map((token, index) =>
        token.unit === "%"
          ? (token.value / 100) * (index % 2 === 0 ? height : width)
          : token.value,
      );
    const normalizeThresholds = (threshold) => {
      const values =
        threshold === undefined
          ? [0]
          : typeof threshold === "number"
            ? [threshold]
            : Array.from(threshold);
      if (!values.length) values.push(0);
      for (let index = 0; index < values.length; index++) {
        const value = Number(values[index]);
        if (!Number.isFinite(value) || value < 0 || value > 1)
          throw new RangeError("threshold must be between 0 and 1");
        values[index] = value;
      }
      values.sort((left, right) => left - right);
      return values.filter(
        (value, index) => index === 0 || value !== values[index - 1],
      );
    };
    const thresholdIndex = (thresholds, ratio) => {
      let index = 0;
      while (index < thresholds.length && thresholds[index] <= ratio) index++;
      return index;
    };
    /*
     * Every (observer, target) registration is re-evaluated on each update
     * (after a layout commit and on scroll), so the page-wide registration
     * count, not the number of observers, is what costs time. An observer
     * with no target costs nothing per update and is not retained here;
     * the Budget-charged JavaScript heap bounds observers like any other
     * object. Pages commonly create one observer per lazy component
     * (theguardian.com creates 75).
     *
     * An update step evaluates at most UPDATE_SLICE registrations and
     * continues in the next render-observer task. A registration costs
     * about 2.7 us of host lab time (mostly its getBoundingClientRect;
     * entries are built only on a state change), roughly 0.25 ms on the
     * PSP at the 80-100x calibration, so a step stays near 8 ms there
     * however many targets a page observes. Notifications are queued as
     * tasks anyway; a large page receives its later ones a few frames
     * later instead of in one long step. REGISTRATION_LIMIT bounds a full
     * pass to 32 steps and the retained state to about 150 KB of heap.
     */
    const REGISTRATION_LIMIT = 1024,
      UPDATE_SLICE = 32,
      active = new Set(),
      fresh = [];
    let registrations = 0,
      freshAt = 0,
      pass = null,
      passRequested = false,
      updateQueued = false,
      continuationQueued = false,
      stepId = 0;
    const rootBoundsFor = (observer) => {
      if (observer._boundsStep === stepId) return observer._bounds;
      const raw =
          observer.root == null || observer.root === document
            ? new DOMRect(0, 0, innerWidth, innerHeight)
            : observer.root.getBoundingClientRect(),
        margin = resolveMargin(observer._rootMargin, raw.width, raw.height);
      observer._boundsStep = stepId;
      return (observer._bounds = [
        raw.left - margin[3],
        raw.top - margin[0],
        raw.width + margin[1] + margin[3],
        raw.height + margin[0] + margin[2],
      ]);
    };
    /* Queues an entry only when the intersecting state or the threshold
       index changed; an unchanged target allocates nothing but its rect. */
    const evaluate = (observer, item, time) => {
      const rect = item.getBoundingClientRect(),
        bounds = rootBoundsFor(observer),
        left = Math.max(rect.left, bounds[0]),
        top = Math.max(rect.top, bounds[1]),
        right = Math.min(rect.right, bounds[0] + bounds[2]),
        bottom = Math.min(rect.bottom, bounds[1] + bounds[3]),
        interWidth = Math.max(0, right - left),
        interHeight = Math.max(0, bottom - top),
        rectArea = rect.width * rect.height,
        zeroTouch = rectArea === 0 && right >= left && bottom >= top,
        intersecting = (interWidth > 0 && interHeight > 0) || zeroTouch,
        ratio =
          intersecting && rectArea > 0
            ? (interWidth * interHeight) / rectArea
            : 0,
        threshold = thresholdIndex(observer.thresholds, ratio),
        previous = observer.lastState.get(item);
      if (
        previous !== undefined &&
        previous.intersecting === intersecting &&
        previous.threshold === threshold
      )
        return false;
      observer.lastState.set(item, { intersecting, threshold });
      if (observer.records.length >= 128) {
        retentionStats.recordDrops++;
        return false;
      }
      observer.records.push(
        new IntersectionObserverEntry({
          time,
          target: item,
          rootBounds: new DOMRect(bounds[0], bounds[1], bounds[2], bounds[3]),
          boundingClientRect: rect,
          intersectionRect: intersecting
            ? new DOMRect(left, top, interWidth, interHeight)
            : new DOMRect(),
          isIntersecting: intersecting,
          intersectionRatio: ratio,
        }),
      );
      return true;
    };
    const scheduleDelivery = (observer) => {
      if (observer.pending || !observer.records.length) return;
      observer.pending = true;
      queueMicrotask(() => {
        observer.pending = false;
        const entries = observer.takeRecords();
        if (!entries.length) return;
        try {
          __tilefinchRealmGlobal.__tilefinchRunTask(
            "intersection-observer",
            observer.callback,
            observer,
            [entries, observer],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "IntersectionObserver");
        }
      });
    };
    const step = () => {
      if (freshAt >= fresh.length && !pass && !passRequested) return;
      let budget = UPDATE_SLICE,
        time;
      const touched = new Set(),
        visit = (observer, item) => {
          budget--;
          if (time === undefined) time = __tilefinchPerformanceSample(10);
          if (evaluate(observer, item, time)) touched.add(observer);
        };
      stepId++;
      /* First observations go first: a new target owes its initial entry. */
      while (freshAt < fresh.length && budget > 0) {
        const observer = fresh[freshAt],
          item = fresh[freshAt + 1];
        freshAt += 2;
        if (observer.targets.has(item) && !observer.lastState.has(item))
          visit(observer, item);
      }
      if (freshAt >= fresh.length) fresh.length = freshAt = 0;
      while (budget > 0) {
        if (!pass) {
          if (!passRequested) break;
          passRequested = false;
          pass = { observers: Array.from(active), at: 0, items: null, item: 0 };
        }
        const observer = pass.observers[pass.at];
        if (!observer) {
          pass = null;
          continue;
        }
        pass.items ||= Array.from(observer.targets);
        while (pass.item < pass.items.length && budget > 0) {
          const item = pass.items[pass.item++];
          if (observer.targets.has(item)) visit(observer, item);
        }
        if (pass.item >= pass.items.length) {
          pass.at++;
          pass.items = null;
          pass.item = 0;
        }
      }
      for (const observer of touched) scheduleDelivery(observer);
      if (
        !continuationQueued &&
        (freshAt < fresh.length || pass || passRequested)
      ) {
        continuationQueued = true;
        (__tilefinchRealmGlobal.__tilefinchScheduleRenderObserver || queueMicrotask)(
          () => {
            continuationQueued = false;
            step();
          },
        );
      }
    };
    const queueUpdate = (schedule) => {
      if (updateQueued) return;
      updateQueued = true;
      schedule(() => {
        updateQueued = false;
        step();
      });
    };
    /* The update microtask is queued even with nothing observed, as the
       recheck always has been; recovery policies read a layout commit's
       pending checkpoint as the page still settling. */
    const requestPass = () => {
      if (active.size) passRequested = true;
      queueUpdate(queueMicrotask);
    };
    __tilefinchRealmGlobal.__tilefinchIntersectionRecheck = () => {
      requestPass();
      __tilefinchRealmGlobal.__tilefinchResizeRecheck?.();
    };
    let scrollHooked = false;
    const hookScroll = () => {
      if (scrollHooked || typeof __tilefinchRealmGlobal.addEventListener !== "function")
        return;
      scrollHooked = true;
      __tilefinchRealmGlobal.addEventListener("scroll", requestPass, {
        capture: true,
        passive: true,
      });
    };
    __tilefinchRealmGlobal.IntersectionObserver = class {
      constructor(callback, options = {}) {
        if (typeof callback !== "function")
          throw new TypeError("callback required");
        const root = options.root ?? null;
        if (
          root !== null &&
          root !== document &&
          !(root instanceof Element)
        )
          throw new TypeError("root must be an Element or Document");
        this.callback = callback;
        this.root = root;
        this._rootMargin = parseMargin(options.rootMargin);
        this.rootMargin = this._rootMargin.text;
        this.thresholds = normalizeThresholds(options.threshold);
        this.targets = new Set();
        this.lastState = new Map();
        this.records = [];
        this.pending = false;
      }
      observe(target) {
        if (!(target instanceof Element))
          throw new TypeError("Element required");
        hookScroll();
        if (this.targets.has(target)) return;
        if (registrations >= REGISTRATION_LIMIT)
          throw new RangeError("IntersectionObserver target limit reached");
        registrations++;
        this.targets.add(target);
        active.add(this);
        /* Observe/unobserve churn before the step runs must not grow the
           queue past the live registrations. */
        if (fresh.length - freshAt >= 4 * REGISTRATION_LIMIT) {
          const live = [];
          for (let at = freshAt; at < fresh.length; at += 2)
            if (
              fresh[at].targets.has(fresh[at + 1]) &&
              !fresh[at].lastState.has(fresh[at + 1])
            )
              live.push(fresh[at], fresh[at + 1]);
          fresh.length = freshAt = 0;
          fresh.push(...live);
        }
        fresh.push(this, target);
        /*
         * Preserve the ordinary initial-observation microtask used by the
         * lightweight runtime. When resize work is pending, place the
         * intersection sample in the later render-fixup slot so the
         * platform-mandated ResizeObserver ordering remains truthful.
         */
        queueUpdate(
          __tilefinchRealmGlobal.__tilefinchResizeRecheck?.() === true
            ? __tilefinchRealmGlobal.__tilefinchScheduleRenderFixup || queueMicrotask
            : queueMicrotask,
        );
      }
      unobserve(target) {
        if (this.targets.delete(target)) {
          registrations--;
          if (!this.targets.size) active.delete(this);
        }
        this.lastState.delete(target);
      }
      disconnect() {
        registrations -= this.targets.size;
        this.targets.clear();
        this.lastState.clear();
        this.records = [];
        active.delete(this);
      }
      takeRecords() {
        return this.records.splice(0);
      }
    };
  }
  let motionInlineHintGeneration = 1,
    styleSheetGeneration = 1,
    motionInlineHintCheckedGeneration = 0,
    motionInlineHintCached = false,
    motionInlineHintScans = 0;
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchStyleSheetGeneration", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: () => styleSheetGeneration,
  });
  const mutationListHasStyleSheetNode = (nodes) => {
    for (let index = 0; index < nodes.length; index++) {
      const name = String(nodes[index]?.localName || "").toLowerCase();
      if (name === "style" || name === "link") return true;
    }
    return false;
  };
  /* CSS motion's bounded style-text scan, shared with the lazy motion.js
     (__tilefinchMotionStyleScan): at most 64 sibling elements per root,
     256 KiB and 4096 text nodes in all. */
  const MOTION_STYLE_LIMIT = 64,
    MOTION_STYLE_BYTES_LIMIT = 256 * 1024,
    MOTION_STYLE_NODE_LIMIT = 4096,
    motionUtf8Length = (text) => {
      /* ASCII text, the common case, is one byte per code unit: one native
         regular-expression pass instead of a loop per character. */
      if (!/[^\x00-\x7f]/.test(text)) return text.length;
      let bytes = 0;
      for (let at = 0; at < text.length; at++) {
        const code = text.charCodeAt(at);
        if (code <= 0x7f) bytes++;
        else if (code <= 0x7ff) bytes += 2;
        else if (
          code >= 0xd800 &&
          code <= 0xdbff &&
          at + 1 < text.length &&
          text.charCodeAt(at + 1) >= 0xdc00 &&
          text.charCodeAt(at + 1) <= 0xdfff
        ) {
          bytes += 4;
          at++;
        } else bytes += 3;
      }
      return bytes;
    },
    motionStyleTextPrefix = (style, maximumBytes, maximumNodes) => {
      if (!style || maximumBytes <= 0 || maximumNodes <= 0)
        return { text: "", nodes: 0, bytes: 0 };
      try {
        const result = __tilefinchRealmGlobal.__tilefinchGetTextPrefix?.(
            typeof style === "number" ? style : style.__handle,
            Math.min(MOTION_STYLE_BYTES_LIMIT, maximumBytes),
            Math.min(MOTION_STYLE_NODE_LIMIT, maximumNodes),
          );
        return result && typeof result === "object"
          ? {
              text: String(result.text || ""),
              nodes: Math.max(0, Math.min(maximumNodes, result.nodes | 0)),
              bytes: Math.max(
                0,
                Math.min(
                  maximumBytes,
                  typeof result.bytes === "number"
                    ? result.bytes | 0
                    : motionUtf8Length(String(result.text || "")),
                ),
              ),
            }
          : { text: "", nodes: 0, bytes: 0 };
      } catch {
        return { text: "", nodes: 0, bytes: 0 };
      }
    },
    motionStyleAttributePrefix = (element, maximumBytes) => {
      if (!element || maximumBytes <= 0) return "";
      try {
        return String(
          __tilefinchRealmGlobal.__tilefinchGetStyleAttributePrefix?.(
            typeof element === "number" ? element : element.__handle,
            Math.min(MOTION_STYLE_BYTES_LIMIT, maximumBytes),
          ) ?? "",
        );
      } catch {
        return "";
      }
    },
    /* Whether a <style> or style attribute in the head or body names
       keyframes or an animation. */
    motionInlineHint = () => {
      let inspectedBytes = 0,
        inspectedNodes = 0;
      for (const root of [document.head, document.body]) {
        let node = root?.firstElementChild || null;
        for (
          let visited = 0;
          node &&
          visited < MOTION_STYLE_LIMIT &&
          inspectedBytes < MOTION_STYLE_BYTES_LIMIT &&
          inspectedNodes < MOTION_STYLE_NODE_LIMIT;
          visited++
        ) {
          const available = MOTION_STYLE_BYTES_LIMIT - inspectedBytes;
          let text = "";
          if (String(node.localName || "").toLowerCase() === "style") {
            const retained = motionStyleTextPrefix(
              node,
              available,
              MOTION_STYLE_NODE_LIMIT - inspectedNodes,
            );
            text = retained.text;
            inspectedNodes += retained.nodes;
            inspectedBytes += retained.bytes;
          } else {
            text = motionStyleAttributePrefix(node, available);
            inspectedNodes++;
            inspectedBytes += motionUtf8Length(text);
          }
          if (/@(?:-webkit-)?keyframes\b|\banimation(?:-name)?\s*:/i.test(text))
            return true;
          node = node.nextElementSibling;
        }
      }
      return false;
    };
  Object.defineProperty(__tilefinchRealmGlobal, "__tilefinchMotionStyleScan", {
    value: Object.freeze({
      styleLimit: MOTION_STYLE_LIMIT,
      bytesLimit: MOTION_STYLE_BYTES_LIMIT,
      nodeLimit: MOTION_STYLE_NODE_LIMIT,
      utf8Length: motionUtf8Length,
      styleTextPrefix: motionStyleTextPrefix,
      styleAttributePrefix: motionStyleAttributePrefix,
      inlineHint: motionInlineHint,
    }),
  });
  __tilefinchRealmGlobal.__tilefinchMaybeStartMotion = () => {
    if (typeof __tilefinchRealmGlobal.__tilefinchMotionRecheck === "function") {
      __tilefinchRealmGlobal.__tilefinchBeginMotionObservation?.();
      __tilefinchRealmGlobal.__tilefinchMotionRecheck();
      return true;
    }
    let hinted = false;
    try {
      hinted = !!__tilefinchRealmGlobal.__tilefinchStylesheetHasMotionKeyframes?.();
      if (
        !hinted &&
        motionInlineHintCheckedGeneration !== motionInlineHintGeneration
      ) {
        /* Dynamic inline styles may not have reached the native cascade yet.
         * Scan once per relevant style mutation, not after every class
         * mutation on motion-free applications. The native parse summary
         * covers completed nested author styles. */
        motionInlineHintCached = false;
        motionInlineHintScans++;
        motionInlineHintCached = motionInlineHint();
        motionInlineHintCheckedGeneration = motionInlineHintGeneration;
      }
      hinted ||= motionInlineHintCached;
    } catch {}
    if (!hinted) return false;
    try {
      if (!__tilefinchRealmGlobal.__tilefinchEnsureMotionBootstrap?.()) return false;
    } catch {
      return false;
    }
    __tilefinchRealmGlobal.__tilefinchBeginMotionObservation?.();
    __tilefinchRealmGlobal.__tilefinchMotionRecheck?.();
    return true;
  };
  /* This turn's attribute changes with their old values, so the transition
     watcher can recover the style before them for an element whose
     transitionend listener is added just after its class changed. */
  const recentAttributeChanges = [];
  let recentAttributeChangesQueued = false;
  __tilefinchRealmGlobal.__tilefinchRecentAttributeChanges = () => recentAttributeChanges;
  const scheduleMutationDelivery = () => {
    if (!mutationDeliveryPending) {
      mutationDeliveryPending = true;
      /* Delivery is a host microtask, not a call into the page's current
         Promise. Observer-backed Promise polyfills otherwise deadlock:
         their reaction needs this very mutation notification to run. */
      mutationPromiseThen(mutationPromiseResolve(), () => {
        mutationDeliveryPending = false;
        /* DOM "notify mutation observers": the observers pending now, in
           the order they were queued, each taking its records, ending its
           transient registrations and then being called. One a callback
           queues waits for the next round. */
        const notifySet = [...pendingMutationObservers];
        pendingMutationObservers.clear();
        mutationDeliveryActive = true;
        for (const item of notifySet) {
          const itemState = mutationObserverStates.get(item);
          if (!itemState) continue;
          itemState.pending = false;
          const records = ancestorApply(
            mutationObserverTakeRecords, item, emptyTaskArguments,
          );
          itemState.transientRoots = [];
          if (records.length)
            try {
              __tilefinchRealmGlobal.__tilefinchRunTask(
                "mutation-observer",
                itemState.callback,
                item,
                [records, item],
              );
            } catch (error) {
              __tilefinchReportUncaught(error, "MutationObserver");
            }
        }
        mutationDeliveryActive = false;
        /* An observer with no records keeps transient registrations until
           its next delivery; left in place they accumulated (64 per
           observer), each an ancestor walk on every mutation. End them
           once this round's callbacks (which may still reach it through a
           removed subtree, and so queue it) have run. */
        for (const item of mutationObservers) {
          const itemState = mutationObserverStates.get(item);
          if (itemState && !itemState.pending && itemState.transientRoots.length)
            itemState.transientRoots = [];
        }
        if (pendingSlotRoots.size)
          flushShadowSlotChanges(true);
      });
    }
  };
  const clearRecentAttributeChanges = () => {
      recentAttributeChanges.length = 0;
      recentAttributeChangesQueued = false;
    },
    mutationPromiseResolve = Function.call.bind(Promise.resolve, Promise),
    mutationPromiseThen = Function.call.bind(Promise.prototype.then),
    styleSheetAttributeNames = ["disabled", "href", "media", "rel", "type"],
    nativeIsAncestor = __tilefinchRealmGlobal.__tilefinchIsAncestor,
    nativeNextAncestorEntry = __tilefinchRealmGlobal.__tilefinchNextAncestorEntry;
  /* Whether the node with `handle` is on the parentNode chain of the native
     mutation target, by one native parent-pointer walk instead of a wrapper
     per ancestor; undefined when only the wrapper walk can tell (either is
     not an ordinary native node). Handles are cached at registration;
     wrappers are never rebound to another handle. */
  const mutationAncestor = (handle, targetHandle) =>
    handle > 0 && targetHandle > 0
      ? nativeIsAncestor(handle, targetHandle)
      : undefined;
  /* The next transient registration from `start` the target may be within:
     one native call passes over those whose roots are not on its chain. */
  const nextTransient = (transients, start, targetHandle) => {
    if (!(targetHandle > 0) || start >= transients.length) return start;
    const next = nativeNextAncestorEntry(targetHandle, transients, start);
    return next === undefined ? start : next;
  };
  /* The wrapper walk behind the checks the native walk cannot answer: the
     target's ancestors. */
  const collectMutationAncestors = (target) => {
    const nodes = new Set();
    for (
      let at = target?.parentNode, steps = 0;
      at && steps < ancestorLimit;
      at = at.parentNode, steps++
    )
      nodes.add(at);
    return { nodes };
  };
  /* DOM "queue a mutation record": one record per interested observer.
     A registration that can neither take this record nor gain a transient
     registration from it is skipped before any ancestry check, and the
     target's wrapper-walk ancestors are collected at most once, only for
     checks the native walk declines. targetHandle: the target's handle
     when it is an ordinary native node, else 0. */
  const queueMutationRecords = (
    target,
    targetHandle,
    type,
    attributeName,
    addedNodes,
    removedNodes,
    oldValue,
    previousSibling,
    nextSibling,
    attributeNamespace,
  ) => {
    const removal = type === "childList" && removedNodes.length > 0,
      typeBit =
        type === "attributes" ? 1
        : type === "childList" ? 2
        : type === "characterData" ? 4 : 0,
      filterName = type === "attributes" ? String(attributeName) : "";
    let connected = null,
      ancestors = null;
    /* Indexed loops: a for-of here allocated an iterator per observer and
       per registration list, on every mutation. The array is read live,
       as an Array Iterator reads it. */
    for (let observerIndex = 0; observerIndex < mutationObservers.length;
         observerIndex++) {
      const observer = mutationObservers[observerIndex];
      retentionStats.observerVisits++;
      const observerState = mutationObserverStates.get(observer);
      if (!observerState) continue;
      /* No registration takes this type or gains a transient one from it,
         and no transient registration exists: nothing to examine. */
      if (
        !(observerState.interest & typeBit) &&
        !(removal && observerState.subtree) &&
        observerState.transientRoots.length === 0
      )
        continue;
      let matched = false;
      let attributeOldValue = false,
        characterDataOldValue = false;
      const targets = observerState.targets;
      for (let targetIndex = 0; targetIndex < targets.length; targetIndex++) {
        const watched = targets[targetIndex],
          options = watched.options,
          wanted =
            (type === "attributes" &&
              options.attributes &&
              (!options.attributeFilter ||
                options.attributeFilter.includes(filterName))) ||
            (type === "childList" && options.childList) ||
            (type === "characterData" && options.characterData),
          transient = removal && options.subtree;
        if (!wanted && !transient) continue;
        let within = target === watched.target;
        if (!within && options.subtree && watched.target === document) {
          if (connected === null)
            connected = !!target?.isConnected &&
              (!shadowRootCreated || target.getRootNode() === document);
          within = connected;
        }
        if (!within && options.subtree) {
          /* Native ancestry, like parentNode, stops at a shadow root. */
          let found = watched.target === document
              ? document.documentElement
                ? mutationAncestor(
                    document.documentElement.__handle,
                    targetHandle,
                  )
                : targetHandle > 0
                  ? false
                  : undefined
              : mutationAncestor(watched.handle, targetHandle);
          if (found === undefined) {
            if (ancestors === null)
              ancestors = collectMutationAncestors(target);
            found =
              ancestors.nodes.has(watched.target) ||
              (watched.target === document &&
                ancestors.nodes.has(document.documentElement));
          }
          within = found;
        }
        if (!within) continue;
        /* Removing a subtree creates transient registrations for every
         * applicable subtree observation, even when that observation did
         * not request childList records.  Keep registrations distinct so
         * overlapping oldValue/filter requirements are merged at mutation
         * time rather than lost behind the first match. */
        if (transient) {
          for (const root of removedNodes) {
            if (observerState.transientRoots.length >= 64) break;
            if (!observerState.transientRoots.some((item) =>
              item.root === root && item.registration === watched)) {
              observerState.transientRoots.push({
                root,
                handle: root?.__handle > 0 ? root.__handle : 0,
                registration: watched,
              });
              scheduleMutationDelivery();
            }
          }
        }
        if (wanted) {
          matched = true;
          attributeOldValue ||= options.attributeOldValue;
          characterDataOldValue ||= options.characterDataOldValue;
        }
      }
      const transients = observerState.transientRoots;
      for (
        let index = nextTransient(transients, 0, targetHandle);
        index < transients.length;
        index = nextTransient(transients, index + 1, targetHandle)
      ) {
          const transient = transients[index],
            options = transient.registration.options,
            wanted =
              (type === "attributes" &&
                options.attributes &&
                (!options.attributeFilter ||
                  options.attributeFilter.includes(filterName))) ||
              (type === "childList" && options.childList) ||
              (type === "characterData" && options.characterData),
            nested = removal && options.subtree;
          if (!wanted && !nested) continue;
          let within = target === transient.root;
          if (!within) {
            within = mutationAncestor(transient.handle, targetHandle);
            if (within === undefined) {
              if (ancestors === null)
                ancestors = collectMutationAncestors(target);
              within = ancestors.nodes.has(transient.root);
            }
          }
          if (!within) continue;
          if (nested) {
            for (const root of removedNodes) {
              if (observerState.transientRoots.length >= 64) break;
              if (!observerState.transientRoots.some((item) =>
                item.root === root &&
                item.registration === transient.registration)) {
                observerState.transientRoots.push({
                  root,
                  handle: root?.__handle > 0 ? root.__handle : 0,
                  registration: transient.registration,
                });
                scheduleMutationDelivery();
              }
            }
          }
          if (wanted) {
            matched = true;
            attributeOldValue ||= options.attributeOldValue;
            characterDataOldValue ||= options.characterDataOldValue;
          }
      }
      if (!matched) continue;
      if (observerState.records.length >= mutationRecordLimit) {
        if (
          type !== "childList" ||
          !foldOverflowRecord(
            observerState,
            target,
            addedNodes,
            removedNodes,
          )
        ) {
          retentionStats.recordDrops++;
          continue;
        }
      } else {
        retentionStats.mutationRecords++;
        observerState.records.push(new MutationRecord({
        type,
        target,
        attributeName: attributeName || null,
        addedNodes: [...addedNodes],
        removedNodes: [...removedNodes],
        previousSibling,
        nextSibling,
        attributeNamespace,
        oldValue:
          type === "attributes"
            ? attributeOldValue
              ? oldValue
              : null
            : type === "characterData"
              ? characterDataOldValue
                ? oldValue
                : null
              : null,
        }));
      }
      if (!observerState.pending) {
        observerState.pending = true;
        pendingMutationObservers.add(observer);
      }
      scheduleMutationDelivery();
    }
  };
  /* Read-only inputs; each delivered MutationRecord receives its own lists.
     Attribute/text notifications otherwise allocate two unused arrays. */
  const emptyMutationNodes = Object.freeze([]);
  /* Whether a tree change can add, remove or reorder child navigables (an
     iframe or frame element, or a subtree holding one, searched natively). */
  const nativeFrameHandles = __tilefinchRealmGlobal.__tilefinchFrameHandles,
    childNavigableContainer = (node) => {
      const name = String(node?.localName || "").toLowerCase();
      return name === "iframe" || name === "frame";
    },
    childNavigablesMoved = (nodes) => {
      for (let index = 0; index < nodes.length; index++) {
        const node = nodes[index];
        if (!(node instanceof Element)) continue;
        if (childNavigableContainer(node)) return true;
        /* Natively: a wrapper per descendant would keep it alive. */
        if (node.__handle > 0 && nativeFrameHandles(node.__handle).length)
          return true;
      }
      return false;
    };
  __tilefinchRealmGlobal.__tilefinchNotifyMutation = (
    target,
    type,
    attributeName,
    addedNodes = emptyMutationNodes,
    removedNodes = emptyMutationNodes,
    oldValue = null,
    previousSibling = null,
    nextSibling = null,
    attributeNamespace = null,
  ) => {
    if (__tilefinchRealmGlobal.__tilefinchMutationSuppressed) return;
    if (type === "childList" && removedNodes.length)
      nodeIteratorHooks?.removed(
        target,
        removedNodes,
        previousSibling,
        nextSibling,
      );
    /* An ordinary native target: its ancestry can be walked natively. */
    const handle = target?.__handle,
      nativeTarget =
        handle > 0 && !target.__tilefinchDetachedParent ? handle : 0;
    if (type === "attributes" && attributeNamespace === null && handle > 0) {
      if (recentAttributeChanges.length >= 64) recentAttributeChanges.shift();
      recentAttributeChanges.push({ target, name: attributeName, oldValue });
      if (!recentAttributeChangesQueued) {
        recentAttributeChangesQueued = true;
        /* A plain promise job holds the same queue position as
           queueMicrotask without its page-task wrapper. */
        mutationPromiseThen(
          mutationPromiseResolve(),
          clearRecentAttributeChanges,
        );
      }
      __tilefinchRealmGlobal.__tilefinchTransitionsDirty?.(target, attributeName, oldValue);
    }
    __tilefinchRealmGlobal.__tilefinchQueueFocusFixup?.(
      target,
      type,
      attributeName,
      addedNodes,
    );
    if (mutationObservers.length)
      queueMutationRecords(
        target,
        nativeTarget,
        type,
        attributeName,
        addedNodes,
        removedNodes,
        oldValue,
        previousSibling,
        nextSibling,
        attributeNamespace,
      );
    __tilefinchRealmGlobal.__tilefinchResizeRecheck?.();
    if (type === "childList"
      ? childNavigablesMoved(addedNodes) || childNavigablesMoved(removedNodes)
      : type === "attributes" &&
        attributeName === "name" &&
        childNavigableContainer(target))
      __tilefinchRealmGlobal.__tilefinchChildNavigablesChanged?.(
        type === "attributes" ? target : null,
      );
    /* Names are read only for the mutation kinds that can use them. */
    let styleSheetMutation = false,
      motionStyleMutation = false;
    if (type === "childList") {
      const targetName = String(target?.localName || "").toLowerCase();
      styleSheetMutation =
        targetName === "style" ||
        mutationListHasStyleSheetNode(addedNodes) ||
        mutationListHasStyleSheetNode(removedNodes);
      motionStyleMutation =
        targetName === "style" ||
        addedNodes.some(
          (node) =>
            node instanceof Element &&
            String(node.localName || "").toLowerCase() === "style",
        );
    } else if (type === "characterData") {
      styleSheetMutation = motionStyleMutation =
        String(target?.parentElement?.localName || "").toLowerCase() ===
        "style";
    } else if (type === "attributes") {
      motionStyleMutation = attributeName === "style";
      if (
        styleSheetAttributeNames.includes(
          String(attributeName || "").toLowerCase(),
        )
      ) {
        const targetName = String(target?.localName || "").toLowerCase();
        styleSheetMutation = targetName === "style" || targetName === "link";
      }
    }
    if (styleSheetMutation) {
      /* New or changed rules can restyle the focused element anywhere. */
      __tilefinchRealmGlobal.__tilefinchQueueFocusFixup?.();
      styleSheetGeneration++;
      if (!styleSheetGeneration) styleSheetGeneration = 1;
    }
    if (motionStyleMutation) {
      motionInlineHintGeneration++;
      if (!motionInlineHintGeneration) motionInlineHintGeneration = 1;
      __tilefinchRealmGlobal.__tilefinchMaybeStartMotion();
    } else if (type === "attributes" && attributeName === "class") {
      __tilefinchRealmGlobal.__tilefinchMaybeStartMotion();
    }
    scheduleShadowSlotChange(target, removedNodes, type);
  };
  /* Test-only counters, read by host tests and the lab's telemetry. Later
     modules add theirs when the census exists. */
  if (testProbes) {
    const census = {};
    Object.defineProperties(census, {
      nodeWrappers: { get: () => nodeCache.size },
      nativeListenerTargets: { get: () => nativeNodeListeners.size },
      nativeHandlerTargets: { get: () => nativeNodeHandlers.size },
      heldEventTargets: { get: () => heldEventStates.size },
      eventTargetCapacity: { get: () => nativeEventTargets.capacity },
      mutationObservers: { get: () => mutationObservers.length },
      slotAssignmentRoots: { get: () => pendingSlotRoots.size },
      motionInlineHintScans: { get: () => motionInlineHintScans },
    });
    __tilefinchRealmGlobal.__tilefinchRootCensus = census;
  }
})();
