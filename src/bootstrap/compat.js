(() => {
  const Object = __tilefinchObject;
  /* Bound before author code runs: what this module converts, collects
     and throws with stays the realm's own when a page replaces the
     global. DOMException is bound where this module defines it. */
  const { String, Number, Map, Set, Error, TypeError, RangeError, Node,
    Document, ShadowRoot, HTMLInputElement, HTMLButtonElement,
    HTMLSelectElement, HTMLDetailsElement, HTMLSummaryElement, Text, Comment,
    DocumentType } = globalThis;
  let DOMException;
  const boundedAncestorPath = globalThis.__tilefinchBoundedAncestorPath,
    ancestorLimit = globalThis.__tilefinchAncestorLimit,
    tilefinchBlobBytes = globalThis.__tilefinchBlobBytes,
    tilefinchBlobForURL = globalThis.__tilefinchBlobForURL,
    tilefinchBlobType = Object.getOwnPropertyDescriptor(
      Blob.prototype,
      "type",
    ).get,
    trustedFunctionApply = Function.call.bind(Function.prototype.apply),
    containerEventTarget = EventTarget.prototype,
    dispatchNode =
      __tilefinchRealmGlobal.__tilefinchTrustedEventShare.dispatchNode,
    /* Event handler attributes register through these, never through a
       page's replacement of the EventTarget methods or global. */
    trustedAddEventListener = Function.call.bind(
      EventTarget.prototype.addEventListener),
    trustedRemoveEventListener = Function.call.bind(
      EventTarget.prototype.removeEventListener),
    /* Host and validation realms only (see hardening.js). */
    testProbes = !!globalThis.__tilefinchTestProbes,
    TrustedObject = Object,
    TrustedString = String,
    /* Platform's URL; internal reads use its fields (_origin, _href), which
       an author cannot replace the way the prototype getters can be. */
    TrustedURL = URL,
    TrustedTypeError = TypeError,
    TrustedRangeError = RangeError,
    TrustedWeakMap = WeakMap,
    TrustedWeakRef = WeakRef,
    TrustedFinalizationRegistry = FinalizationRegistry,
    trustedWeakMapGet = Function.call.bind(WeakMap.prototype.get),
    trustedWeakMapSet = Function.call.bind(WeakMap.prototype.set),
    trustedWeakMapHas = Function.call.bind(WeakMap.prototype.has),
    trustedWeakMapDelete = Function.call.bind(WeakMap.prototype.delete),
    trustedWeakSetAdd = Function.call.bind(WeakSet.prototype.add),
    trustedWeakSetHas = Function.call.bind(WeakSet.prototype.has),
    trustedWeakRefDeref = Function.call.bind(WeakRef.prototype.deref),
    trustedFinalizationRegistryRegister = Function.call.bind(
      FinalizationRegistry.prototype.register,
    ),
    trustedFinalizationRegistryUnregister = Function.call.bind(
      FinalizationRegistry.prototype.unregister,
    ),
    trustedMapGet = Function.call.bind(Map.prototype.get),
    trustedMapSet = Function.call.bind(Map.prototype.set),
    trustedMapHas = Function.call.bind(Map.prototype.has),
    trustedMapDelete = Function.call.bind(Map.prototype.delete),
    trustedMapEntries = Function.call.bind(Map.prototype.entries),
    trustedMapSize = Function.call.bind(
      Object.getOwnPropertyDescriptor(Map.prototype, "size").get,
    ),
    trustedArraySort = Function.call.bind(Array.prototype.sort),
    trustedObjectCreate = Object.create,
    trustedObjectFreeze = Object.freeze,
    trustedDefineProperty = Object.defineProperty,
    trustedStringLower = Function.call.bind(String.prototype.toLowerCase),
    arrayBufferIsView = ArrayBuffer.isView,
    Uint8ArrayCtor = Uint8Array,
    trustedArrayBufferByteLength = Function.call.bind(
      Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get,
    ),
    typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype),
    trustedTypedArrayBuffer = Function.call.bind(
      Object.getOwnPropertyDescriptor(typedArrayPrototype, "buffer").get,
    ),
    trustedTypedArrayByteOffset = Function.call.bind(
      Object.getOwnPropertyDescriptor(typedArrayPrototype, "byteOffset").get,
    ),
    trustedTypedArrayByteLength = Function.call.bind(
      Object.getOwnPropertyDescriptor(typedArrayPrototype, "byteLength").get,
    ),
    trustedDataViewBuffer = Function.call.bind(
      Object.getOwnPropertyDescriptor(DataView.prototype, "buffer").get,
    ),
    trustedDataViewByteOffset = Function.call.bind(
      Object.getOwnPropertyDescriptor(DataView.prototype, "byteOffset").get,
    ),
    trustedDataViewByteLength = Function.call.bind(
      Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get,
    ),
    trustedPromiseResolve = Function.call.bind(Promise.resolve, Promise),
    trustedPromiseReject = Function.call.bind(Promise.reject, Promise),
    trustedPromiseThen = Function.call.bind(Promise.prototype.then),
    navigatorBrand = globalThis.__tilefinchNavigatorBrand,
    trustedCryptoRandomFill = globalThis.__tilefinchCryptoRandomFill,
    trustedUint8ArraySet = Function.call.bind(Uint8Array.prototype.set),
    isArrayBuffer = (value) => {
      try {
        trustedArrayBufferByteLength(value);
        return true;
      } catch (_) {
        return false;
      }
    },
    copyArrayBufferBytes = (buffer, byteOffset = 0, byteLength,
      maximum = Infinity) => {
      const bufferLength = trustedArrayBufferByteLength(buffer);
      if (byteLength === undefined) byteLength = bufferLength - byteOffset;
      if (byteLength > maximum) return null;
      const source = new Uint8ArrayCtor(buffer, byteOffset, byteLength),
        copy = new Uint8ArrayCtor(byteLength);
      trustedUint8ArraySet(copy, source, 0);
      return copy;
    },
    copyArrayBufferViewBytes = (view, maximum) => {
      let buffer, byteOffset, byteLength;
      try {
        buffer = trustedTypedArrayBuffer(view);
        byteOffset = trustedTypedArrayByteOffset(view);
        byteLength = trustedTypedArrayByteLength(view);
      } catch (_) {
        buffer = trustedDataViewBuffer(view);
        byteOffset = trustedDataViewByteOffset(view);
        byteLength = trustedDataViewByteLength(view);
      }
      return copyArrayBufferBytes(buffer, byteOffset, byteLength, maximum);
    };
  delete globalThis.__tilefinchNavigatorBrand;
  /* See dom.js: hardening.js only sees the globals that exist when it runs,
     so anything created on first write is declared here instead. */
  for (const [name, initial] of [
    ["__tilefinchParentAppendBypass", false],
    ["__tilefinchMutationSuppressed", 0],
  ])
    Object.defineProperty(globalThis, name, {
      enumerable: false,
      configurable: false,
      writable: true,
      value: initial,
    });
  {
    let pageVisible = true,
      recordVisibilityState =
        typeof globalThis.__tilefinchRecordVisibilityPerformance === "function"
          ? globalThis.__tilefinchRecordVisibilityPerformance
          : null;
    delete globalThis.__tilefinchRecordVisibilityPerformance;
    const dispatchVisibility = document.dispatchEvent.bind(document);
    Object.defineProperty(document, "visibilityState", {
      configurable: true,
      enumerable: true,
      get() {
        return pageVisible ? "visible" : "hidden";
      },
    });
    Object.defineProperty(document, "hidden", {
      configurable: true,
      enumerable: true,
      get() {
        return !pageVisible;
      },
    });
    globalThis.__tilefinchPageVisible = () => pageVisible;
    globalThis.__tilefinchApplyPageVisibility = (visible) => {
      visible = !!visible;
      if (visible === pageVisible) return false;
      pageVisible = visible;
      /* HTML queues the timeline entry before visibilitychange. Performance
         telemetry is optional to the lifecycle itself: a refused entry must
         not suppress the state transition or its event. */
      try {
        recordVisibilityState?.(pageVisible ? "visible" : "hidden");
      } catch (_) {}
      dispatchVisibility(browserEvent(EventInterface, "visibilitychange"));
      return true;
    };
    document.exitPointerLock = { exitPointerLock() {} }.exitPointerLock;
  }
  const domShared = globalThis.__tilefinchDomShared,
    { reflectString, secondaryDocumentPrototype, asciiLower, checkDoctypeName,
      contextFragment } = domShared;
  /* Native code runs it only where the ARIA reflections are installed
     (reflected_attributes.inc); elsewhere it goes with the channel. */
  globalThis.__tilefinchHostChannel.ariaElementReferences =
    domShared.ariaElementReferences;
  delete domShared.ariaElementReferences;
  {
    /* Reflected <meta> attributes. Without the interface, meta.content fell
       through to a generic null getter; ChatGPT's mobile keyboard bootstrap
       reads viewport meta.content.includes(...) and threw. */
    Object.defineProperties(HTMLMetaElement.prototype, {
      name: reflectString("name"),
      content: reflectString("content"),
      httpEquiv: reflectString("httpEquiv", "http-equiv"),
      media: reflectString("media"),
      scheme: reflectString("scheme"),
    });
    Object.defineProperties(HTMLLinkElement.prototype, {
      as: reflectString("as"),
      integrity: reflectString("integrity"),
      /* The deferred-CSS idiom assigns link.media = "all" from onload; an
         expando left the media attribute, and the sheet, disabled. */
      media: reflectString("media"),
      referrerPolicy: reflectString("referrerPolicy"),
    });
    Object.defineProperty(
      HTMLScriptElement.prototype,
      "integrity",
      reflectString("integrity"),
    );
  }
  {
    const anchorLocation = (node) => {
      try {
        const value = node.getAttribute("href");
        return value === null ? null : new URL(value, node.ownerDocument?.baseURI || location.href);
      } catch {
        return null;
      }
    };
    const anchorPart = (name) => ({
      configurable: true,
      enumerable: true,
      get() {
        const parsed = anchorLocation(this);
        return parsed ? parsed[name] : "";
      },
    });
    Object.defineProperties(HTMLAnchorElement.prototype, {
      protocol: anchorPart("protocol"),
      host: anchorPart("host"),
      hostname: anchorPart("hostname"),
      port: anchorPart("port"),
      pathname: anchorPart("pathname"),
      search: anchorPart("search"),
      hash: anchorPart("hash"),
      origin: anchorPart("origin"),
    });
    HTMLAnchorElement.prototype.toString = function toString() {
      return this.href;
    };
  }
  const storageKinds = new WeakMap(),
    storageFallback = [new Map(), new Map()],
    storageAvailable = globalThis.__tilefinchStorageAvailable(),
    storageLength = globalThis.__tilefinchStorageLength,
    storageKey = globalThis.__tilefinchStorageKey,
    storageGet = globalThis.__tilefinchStorageGet,
    storageSet = globalThis.__tilefinchStorageSet,
    storageRemove = globalThis.__tilefinchStorageRemove,
    storageClear = globalThis.__tilefinchStorageClear;
  class Storage {
    constructor(local, token) {
      if (token !== storageKinds)
        throw new TypeError("Illegal constructor");
      storageKinds.set(this, !!local);
    }
    get length() {
      const local = storageKinds.get(this);
      return storageAvailable
        ? Number(storageLength(local)) || 0
        : storageFallback[local ? 1 : 0].size;
    }
    key(index) {
      const local = storageKinds.get(this),
        numeric = Number(index);
      if (storageAvailable) return storageKey(local, numeric);
      if (!Number.isInteger(numeric) || numeric < 0) return null;
      return [...storageFallback[local ? 1 : 0].keys()][numeric] ?? null;
    }
    getItem(key) {
      const local = storageKinds.get(this);
      key = String(key);
      if (storageAvailable) return storageGet(local, key);
      const fallback = storageFallback[local ? 1 : 0];
      return fallback.has(key) ? fallback.get(key) : null;
    }
    setItem(key, value) {
      const local = storageKinds.get(this);
      key = String(key);
      value = String(value);
      if (storageAvailable && !storageSet(local, key, value))
        throw new DOMException("Storage quota exceeded", "QuotaExceededError");
      if (!storageAvailable) {
        const fallback = storageFallback[local ? 1 : 0],
          replacing = fallback.has(key),
          previous = replacing ? fallback.get(key) : "",
          bytes =
            [...fallback].reduce(
              (total, item) => total + item[0].length + item[1].length,
              0,
            ) -
            (replacing ? key.length + previous.length : 0) +
            key.length +
            value.length;
        if ((!replacing && fallback.size >= 128) || bytes > 32768)
          throw new DOMException(
            "Storage quota exceeded",
            "QuotaExceededError",
          );
        fallback.set(key, value);
      }
    }
    removeItem(key) {
      const local = storageKinds.get(this);
      key = String(key);
      // The store keeps an item whose removal it could not record.
      if (storageAvailable && !storageRemove(local, key))
        throw new DOMException("Storage could not be changed", "UnknownError");
      if (!storageAvailable) storageFallback[local ? 1 : 0].delete(key);
    }
    clear() {
      const local = storageKinds.get(this);
      if (storageAvailable && !storageClear(local))
        throw new DOMException("Storage could not be changed", "UnknownError");
      if (!storageAvailable) storageFallback[local ? 1 : 0].clear();
    }
  }
  Object.defineProperty(globalThis, "Storage", {
    configurable: true,
    writable: true,
    value: Storage,
  });
  function makeStorage(local) {
    const target = new Storage(local, storageKinds),
      proxy = new Proxy(target, {
      get(storage, property, receiver) {
        if (
          typeof property !== "string" ||
          Reflect.has(storage, property)
        )
          return Reflect.get(storage, property, receiver);
        const value = storage.getItem(property);
        return value === null ? undefined : value;
      },
      set(storage, property, value, receiver) {
        if (
          typeof property !== "string" ||
          Reflect.has(storage, property)
        )
          return Reflect.set(storage, property, value, receiver);
        storage.setItem(property, value);
        return true;
      },
      deleteProperty(storage, property) {
        if (
          typeof property !== "string" ||
          Reflect.has(storage, property)
        )
          return Reflect.deleteProperty(storage, property);
        storage.removeItem(property);
        return true;
      },
      defineProperty(storage, property, descriptor) {
        if (
          typeof property !== "string" ||
          Reflect.has(storage, property)
        )
          return Reflect.defineProperty(storage, property, descriptor);
        if ("get" in descriptor || "set" in descriptor)
          return false;
        storage.setItem(property, descriptor.value);
        return true;
      },
      ownKeys(storage) {
        const keys = Reflect.ownKeys(storage);
        for (let index = 0; index < storage.length; index++) {
          const key = storage.key(index);
          if (key !== null && !keys.includes(key)) keys.push(key);
        }
        return keys;
      },
      getOwnPropertyDescriptor(storage, property) {
        const descriptor = Reflect.getOwnPropertyDescriptor(
          storage,
          property,
        );
        if (descriptor || typeof property !== "string") return descriptor;
        const value = storage.getItem(property);
        return value === null
          ? undefined
          : {
              configurable: true,
              enumerable: true,
              writable: true,
              value,
            };
      },
      });
    storageKinds.set(proxy, !!local);
    return proxy;
  }
  globalThis.localStorage = makeStorage(true);
  globalThis.sessionStorage = makeStorage(false);
  Object.defineProperty(document, "cookie", {
    configurable: true,
    enumerable: true,
    get() {
      return __tilefinchCookieGet();
    },
    set(value) {
      __tilefinchCookieSet(String(value));
    },
  });
  Object.defineProperty(globalThis, "isSecureContext", {
    configurable: true,
    enumerable: true,
    value: !!globalThis.__tilefinchSecureContext(),
    writable: false,
  });
  /* WebIDL event attributes live on the prototype in browsers.  Keeping the
     dispatcher state in a private table avoids exposing Tilefinch's mutable
     bookkeeping as enumerable/own page properties while still allowing the
     DOM bridge to update one event during propagation. */
  const eventStates = new WeakMap(),
    eventStateGet = eventStates.get.bind(eventStates),
    eventStateSet = eventStates.set.bind(eventStates),
    eventState = (event) => {
      const state = eventStateGet(event);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    },
    /* isTrusted is an unforgeable own attribute whose getter browsers share
       across events; one descriptor also spares a closure per event. */
    isTrustedDescriptor = {
      __proto__: null,
      configurable: false,
      enumerable: true,
      get: Object.getOwnPropertyDescriptor({
        get isTrusted() {
          return eventState(this).trusted;
        },
      }, "isTrusted").get,
    };
  /* compat.js runs before author code, so the interface objects it keeps in
     locals (EventInterface and the rest) stay the bootstrap's own after a
     page replaces the globals; browserEvent below builds with them. */
  const EventInterface = globalThis.Event = class Event {
    constructor(type, options = {}) {
      options ??= {};
      if (arguments.length < 1)
        throw new TypeError("Event type is required");
      /* Null prototype: a field another interface adds later (ProgressEvent's
         loaded) must not reach a setter the page put on Object.prototype. */
      eventStateSet(this, {
        __proto__: null,
        type: String(type),
        bubbles: !!options.bubbles,
        cancelable: !!options.cancelable,
        composed: !!options.composed,
        defaultPrevented: false,
        eventPhase: 0,
        currentTarget: null,
        target: null,
        timeStamp: __tilefinchPerformanceSample(9),
        initialized: true,
        dispatching: false,
        stopped: false,
        immediateStopped: false,
        passive: false,
        path: null,
        trusted: false,
      });
      trustedDefineProperty(this, "isTrusted", isTrustedDescriptor);
    }
    preventDefault() {
      const state = eventState(this);
      if (state.cancelable && !state.passive) state.defaultPrevented = true;
    }
    stopPropagation() {
      this.__stopped = true;
    }
    stopImmediatePropagation() {
      this.__stopped = true;
      this.__immediateStopped = true;
    }
    composedPath() {
      const path = this.__path;
      if (!path) return [];
      return globalThis.__tilefinchVisibleShadowEventPath
        ? globalThis.__tilefinchVisibleShadowEventPath(path, this.currentTarget)
        : this.currentTarget
          ? path.slice()
          : [];
    }
    initEvent(type, bubbles = false, cancelable = false) {
      if (arguments.length < 1)
        throw new TypeError("Event type is required");
      const state = eventState(this);
      if (state.dispatching) return;
      state.type = String(type);
      state.bubbles = !!bubbles;
      state.cancelable = !!cancelable;
      state.defaultPrevented = false;
      state.initialized = true;
      state.trusted = false;
      state.target = null;
      state.stopped = false;
      state.immediateStopped = false;
    }
  };
  /* Literal accessors with computed keys are named "get type" etc. and are
     enumerable and configurable, as WebIDL attributes are. */
  const eventField = (name, convert, writable = true) =>
    Object.getOwnPropertyDescriptor(writable ? {
      get [name]() { return eventState(this)[name]; },
      set [name](value) { eventState(this)[name] = convert(value); },
    } : {
      get [name]() { return eventState(this)[name]; },
    }, name);
  Object.defineProperties(Event.prototype, {
    type: eventField("type", String, false),
    bubbles: eventField("bubbles", Boolean, false),
    cancelable: eventField("cancelable", Boolean, false),
    composed: eventField("composed", Boolean, false),
    defaultPrevented: eventField("defaultPrevented", Boolean, false),
    eventPhase: eventField("eventPhase", (value) => Number(value) || 0),
    currentTarget: eventField("currentTarget", (value) => value ?? null),
    target: eventField("target", (value) => value ?? null),
    timeStamp: eventField("timeStamp", Number, false),
    __initialized: {
      configurable: true,
      get() { return eventState(this).initialized; },
      set(value) { eventState(this).initialized = !!value; },
    },
    __dispatching: {
      configurable: true,
      get() { return eventState(this).dispatching; },
      set(value) { eventState(this).dispatching = !!value; },
    },
    __stopped: {
      configurable: true,
      get() { return eventState(this).stopped; },
      set(value) { eventState(this).stopped = !!value; },
    },
    __immediateStopped: {
      configurable: true,
      get() { return eventState(this).immediateStopped; },
      set(value) { eventState(this).immediateStopped = !!value; },
    },
    __passive: {
      configurable: true,
      get() { return eventState(this).passive; },
      set(value) { eventState(this).passive = !!value; },
    },
    __path: {
      configurable: true,
      get() { return eventState(this).path; },
      set(value) { eventState(this).path = value; },
    },
    returnValue: {
      configurable: true,
      enumerable: true,
      get() {
        return !this.defaultPrevented;
      },
      set(value) {
        if (!value) this.preventDefault();
      },
    },
    cancelBubble: {
      configurable: true,
      enumerable: true,
      get() {
        return !!this.__stopped;
      },
      set(value) {
        if (value) this.stopPropagation();
      },
    },
    srcElement: {
      configurable: true,
      enumerable: true,
      get() {
        return this.target;
      },
    },
    [Symbol.toStringTag]: {
      configurable: true,
      value: "Event",
    },
  });
  for (const [name, value] of Object.entries({
    NONE: 0,
    CAPTURING_PHASE: 1,
    AT_TARGET: 2,
    BUBBLING_PHASE: 3,
  })) {
    /* Web IDL constants are enumerable. */
    Object.defineProperty(Event, name, { enumerable: true, value });
    Object.defineProperty(Event.prototype, name, { enumerable: true, value });
  }
  const createEvent = (interfaceName) => {
    const name = String(interfaceName || ""),
      Constructor =
        name === "CustomEvent"
          ? CustomEvent
          : name === "ErrorEvent"
            ? ErrorEvent
          : name === "StorageEvent"
            ? StorageEvent
          : name === "CompositionEvent"
            ? CompositionEvent
          : name.toLowerCase() === "textevent"
            ? TextEventInterface
            : name === "FocusEvent"
              ? FocusEvent
              : name === "KeyboardEvent"
                ? KeyboardEvent
                : name === "MouseEvent" || name === "MouseEvents"
                  ? MouseEvent
                  : name === "UIEvent" || name === "UIEvents"
                    ? UIEvent
                    : Event,
      event = Constructor === TextEventInterface
        ? new TextEventInterface("", textEventToken)
        : new Constructor("");
    event.__initialized = false;
    return event;
  };
  document.createEvent = createEvent;
  Document.prototype.createEvent = createEvent;
  const trustedEvent = (event, trusted = true) => {
    eventState(event).trusted = trusted;
    return event;
  };
  /* Events the browser itself fires: built with a captured interface object
     from a null-prototype copy of the init literal's own fields, so neither
     a replaced global constructor nor an Object.prototype getter takes part
     in making an event that trustedEvent may then mark. */
  const emptyEventInit = Object.freeze({ __proto__: null }),
    trustedObjectKeys = Object.keys,
    browserEvent = (Interface, type, init) => {
      let dictionary = emptyEventInit;
      if (init != null) {
        dictionary = { __proto__: null };
        const keys = trustedObjectKeys(init);
        for (let index = 0; index < keys.length; index++)
          dictionary[keys[index]] = init[keys[index]];
      }
      const event = new Interface(type, dictionary);
      /* UIEventInit's view defaults to null for a page's own events; the
         browser's UI events belong to this window. */
      if (event.view === null) event.view = globalThis;
      return event;
    };
  /* Boot-time only: the host captures it once the bootstrap has run and
     deletes it before any author code (runtime_creation.inc). */
  Object.defineProperty(globalThis, "__tilefinchTrustedEvent", {
    configurable: true,
    value: trustedEvent,
  });
  globalThis.CustomEvent = class CustomEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.detail = options.detail === undefined ? null : options.detail;
    }
    initCustomEvent(type, bubbles = false, cancelable = false, detail = null) {
      if (arguments.length < 1)
        throw new TypeError("CustomEvent type is required");
      if (this.__dispatching) return;
      this.initEvent(type, bubbles, cancelable);
      this.detail = detail;
    }
  };
  globalThis.UIEvent = class UIEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.view = options.view ?? null;
      this.detail = Number(options.detail) || 0;
      this.which = Number(options.which) || 0;
    }
    initUIEvent(type, bubbles = false, cancelable = false, view = null,
      detail = 0) {
      if (arguments.length < 1)
        throw new TypeError("UIEvent type is required");
      if (this.__dispatching) return;
      this.initEvent(type, bubbles, cancelable);
      this.view = view;
      this.detail = Number(detail) || 0;
    }
  };
  /* The legacy init methods' modifier arguments, in their order. */
  const initModifiers = (target, ctrlKey, altKey, shiftKey, metaKey) => {
    target.ctrlKey = !!ctrlKey;
    target.altKey = !!altKey;
    target.shiftKey = !!shiftKey;
    target.metaKey = !!metaKey;
  };
  const modifierFields = (target, options) => {
    target.ctrlKey = !!options.ctrlKey;
    target.shiftKey = !!options.shiftKey;
    target.altKey = !!options.altKey;
    target.metaKey = !!options.metaKey;
    target.getModifierState = (key) =>
      ({
        Alt: target.altKey,
        Control: target.ctrlKey,
        Meta: target.metaKey,
        Shift: target.shiftKey,
      })[String(key)] || false;
  };
  globalThis.MouseEvent = class MouseEvent extends UIEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      for (const name of [
        "screenX",
        "screenY",
        "clientX",
        "clientY",
        "pageX",
        "pageY",
        "offsetX",
        "offsetY",
        "movementX",
        "movementY",
      ])
        this[name] = Number(options[name]) || 0;
      /* Canvas examples still use the legacy layer coordinates. In
         Tilefinch's flat page surface they are the target-relative offsets. */
      this.layerX = options.layerX === undefined
        ? this.offsetX
        : Number(options.layerX) || 0;
      this.layerY = options.layerY === undefined
        ? this.offsetY
        : Number(options.layerY) || 0;
      this.x = this.clientX;
      this.y = this.clientY;
      this.button = Number(options.button) || 0;
      this.buttons = Number(options.buttons) || 0;
      /* UI Events: a mouse event's legacy which is its button plus one. */
      this.which = this.button + 1;
      this.relatedTarget = options.relatedTarget ?? null;
      modifierFields(this, options);
    }
    initMouseEvent(type, bubbles = false, cancelable = false, view = null,
      detail = 0, screenX = 0, screenY = 0, clientX = 0, clientY = 0,
      ctrlKey = false, altKey = false, shiftKey = false, metaKey = false,
      button = 0, relatedTarget = null) {
      if (arguments.length < 1)
        throw new TypeError("MouseEvent type is required");
      if (this.__dispatching) return;
      this.initUIEvent(type, bubbles, cancelable, view, detail);
      this.screenX = Number(screenX) || 0;
      this.screenY = Number(screenY) || 0;
      this.x = this.clientX = Number(clientX) || 0;
      this.y = this.clientY = Number(clientY) || 0;
      this.pageX = this.clientX + (Number(globalThis.scrollX) || 0);
      this.pageY = this.clientY + (Number(globalThis.scrollY) || 0);
      initModifiers(this, ctrlKey, altKey, shiftKey, metaKey);
      this.button = Number(button) || 0;
      this.which = this.button + 1;
      this.relatedTarget = relatedTarget;
    }
  };
  {
    /* No standard defines fromElement and toElement yet (w3c/pointerevents
       issue 605); Chromium and WebKit answer them alike. A mouseout or
       mouseleave goes from its target to its related target; every other
       type, a created event's included, from the related target to the
       target. Only a node is answered. */
    const NodeInterface = Node,
      node = (value) => value instanceof NodeInterface ? value : null,
      leaving = (event) =>
        event.type === "mouseout" || event.type === "mouseleave";
    Object.defineProperties(MouseEvent.prototype,
      Object.getOwnPropertyDescriptors({
        get fromElement() {
          return node(leaving(this) ? this.target : this.relatedTarget);
        },
        get toElement() {
          return node(leaving(this) ? this.relatedTarget : this.target);
        },
      }));
  }
  globalThis.PointerEvent = class PointerEvent extends MouseEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.pointerId = Number(
        options.pointerId === undefined ? 0 : options.pointerId,
      );
      this.width = Number(options.width === undefined ? 1 : options.width);
      this.height = Number(options.height === undefined ? 1 : options.height);
      this.pressure = Number(options.pressure) || 0;
      this.tangentialPressure = Number(options.tangentialPressure) || 0;
      this.tiltX = Number(options.tiltX) || 0;
      this.tiltY = Number(options.tiltY) || 0;
      this.twist = Number(options.twist) || 0;
      this.pointerType =
        options.pointerType === undefined ? "" : String(options.pointerType);
      this.isPrimary = !!options.isPrimary;
    }
  };
  globalThis.KeyboardEvent = class KeyboardEvent extends UIEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.key = String(options.key || "");
      this.code = String(options.code || "");
      this.location = Number(options.location) || 0;
      this.repeat = !!options.repeat;
      this.isComposing = !!options.isComposing;
      this.charCode = Number(options.charCode) || 0;
      this.keyCode = Number(options.keyCode) || 0;
      this.which =
        Number(options.which === undefined ? this.keyCode : options.which) || 0;
      modifierFields(this, options);
    }
    initKeyboardEvent(type, bubbles = false, cancelable = false, view = null,
      key = "", location = 0, ctrlKey = false, altKey = false,
      shiftKey = false, metaKey = false) {
      if (arguments.length < 1)
        throw new TypeError("KeyboardEvent type is required");
      if (this.__dispatching) return;
      this.initUIEvent(type, bubbles, cancelable, view, 0);
      this.key = String(key);
      this.location = Number(location) || 0;
      initModifiers(this, ctrlKey, altKey, shiftKey, metaKey);
    }
  };
  for (const [name, value] of Object.entries({
    DOM_KEY_LOCATION_STANDARD: 0,
    DOM_KEY_LOCATION_LEFT: 1,
    DOM_KEY_LOCATION_RIGHT: 2,
    DOM_KEY_LOCATION_NUMPAD: 3,
  })) {
    Object.defineProperty(KeyboardEvent, name, { enumerable: true, value });
    Object.defineProperty(KeyboardEvent.prototype, name, {
      enumerable: true, value,
    });
  }
  globalThis.InputEvent = class InputEvent extends UIEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.data = options.data === undefined ? null : options.data;
      this.inputType = String(options.inputType || "");
      this.isComposing = !!options.isComposing;
      this.dataTransfer = options.dataTransfer ?? null;
    }
    getTargetRanges() {
      return [];
    }
  };
  /* UI Events' legacy TextEvent: not constructible, made by
     createEvent("TextEvent") and initTextEvent. The scheduler dispatches a
     trusted "textInput" before beforeinput for typed text, as Chrome does,
     so React's 'TextEvent' in window check picks a path that fires. */
  const textEventToken = {};
  const TextEventInterface = class TextEvent extends UIEvent {
    constructor(type, token) {
      if (token !== textEventToken) throw new TypeError("Illegal constructor");
      super(type, {});
      this.data = "";
    }
    initTextEvent(type, bubbles = false, cancelable = false, view = null,
                  data = "") {
      if (this.__dispatching) return;
      this.initEvent(type, bubbles, cancelable);
      this.view = view;
      this.data = String(data);
    }
  };
  globalThis.TextEvent = TextEventInterface;
  const FocusEventInterface = globalThis.FocusEvent = class FocusEvent extends UIEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.relatedTarget = options.relatedTarget ?? null;
    }
  };
  globalThis.CompositionEvent = class CompositionEvent extends UIEvent {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.data = String(options.data || "");
    }
  };
  globalThis.SubmitEvent = class SubmitEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.submitter = options.submitter ?? null;
    }
  };
  const TrustedFormData = globalThis.FormData;
  const FormDataEventInterface = globalThis.FormDataEvent = class FormDataEvent extends EventInterface {
    constructor(type, init) {
      const data = init?.formData;
      if (!(data instanceof TrustedFormData))
        throw new TypeError("FormDataEvent requires formData");
      super(type, init);
      eventState(this).formData = data;
    }
    get [Symbol.toStringTag]() { return "FormDataEvent"; }
  };
  trustedDefineProperty(FormDataEventInterface.prototype,
    "formData", eventField("formData", null, false));
  globalThis.TransitionEvent = class TransitionEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.propertyName = String(options.propertyName || "");
      this.elapsedTime = Math.max(0, Number(options.elapsedTime) || 0);
      this.pseudoElement = String(options.pseudoElement || "");
    }
  };
  const ToggleEventInterface = globalThis.ToggleEvent = class ToggleEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.oldState = String(options.oldState || "closed");
      this.newState = String(options.newState || "closed");
    }
  };
  globalThis.ProgressEvent = class ProgressEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      const state = eventState(this);
      state.lengthComputable = !!options.lengthComputable;
      state.loaded = Math.max(0, Number(options.loaded) || 0);
      state.total = Math.max(0, Number(options.total) || 0);
    }
    get [Symbol.toStringTag]() { return "ProgressEvent"; }
  };
  /* ProgressEvent's Web IDL attributes are readonly prototype accessors in
     browsers.  Reuse Event's private state instead of adding another WeakMap
     or exposing three mutable own properties on every network event. */
  const progressEventConstructor = ProgressEvent.prototype.constructor;
  delete ProgressEvent.prototype.constructor;
  Object.defineProperties(ProgressEvent.prototype, {
    lengthComputable: {
      configurable: true,
      enumerable: true,
      get() { return eventState(this).lengthComputable; },
    },
    loaded: {
      configurable: true,
      enumerable: true,
      get() { return eventState(this).loaded; },
    },
    total: {
      configurable: true,
      enumerable: true,
      get() { return eventState(this).total; },
    },
    constructor: {
      configurable: true,
      writable: true,
      value: progressEventConstructor,
    },
  });
  const errorEventStates = new WeakMap(),
    errorEventState = (event) => {
      const state = errorEventStates.get(event);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    };
  globalThis.ErrorEvent = class ErrorEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      errorEventStates.set(this, {
        message: String(options.message || ""),
        filename: String(options.filename || ""),
        lineno: Math.max(0, Math.trunc(Number(options.lineno) || 0)),
        colno: Math.max(0, Math.trunc(Number(options.colno) || 0)),
        error: options.error === undefined ? null : options.error,
      });
    }
    get [Symbol.toStringTag]() { return "ErrorEvent"; }
  };
  const errorEventConstructor = ErrorEvent.prototype.constructor;
  delete ErrorEvent.prototype.constructor;
  const errorEventField = (name) => ({
    configurable: true,
    enumerable: true,
    get() { return errorEventState(this)[name]; },
  });
  Object.defineProperties(ErrorEvent.prototype, {
    message: errorEventField("message"),
    filename: errorEventField("filename"),
    lineno: errorEventField("lineno"),
    colno: errorEventField("colno"),
    error: errorEventField("error"),
    constructor: {
      configurable: true,
      writable: true,
      value: errorEventConstructor,
    },
  });
  /* Events that carry only their init dictionary. Tilefinch is one tab
     and never fires cross-document `storage` events, but pages construct
     them (Firebase's localStorage polling builds StorageEvents); same-
     document navigation fires the other two. `fields` maps each attribute
     to its default and its conversion. PageTransitionEvent and
     BeforeUnloadEvent are left out: nothing fires them. */
  const initEventStates = new WeakMap(),
    initEventState = (event) => {
      const state = initEventStates.get(event);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    },
    initEventStore = (event, init, fields) => {
      const state = {};
      for (const name in fields) {
        const value = init == null ? undefined : init[name];
        state[name] = value === undefined
          ? fields[name][0]
          : fields[name][1](value);
      }
      initEventStates.set(event, state);
      return state;
    },
    defineInitEvent = (Interface, fields) => {
      const descriptors = { [Symbol.toStringTag]: {
        configurable: true,
        value: Interface.name,
      } };
      for (const name in fields)
        descriptors[name] = Object.getOwnPropertyDescriptor({
          get [name]() { return initEventState(this)[name]; },
        }, name);
      Object.defineProperties(Interface.prototype, descriptors);
      Object.defineProperty(globalThis, Interface.name, {
        configurable: true,
        writable: true,
        value: Interface,
      });
    },
    anyValue = (value) => value,
    nullableString = (value) => value === null ? null : String(value),
    storageEventFields = {
      key: [null, nullableString],
      oldValue: [null, nullableString],
      newValue: [null, nullableString],
      url: ["", String],
      storageArea: [null, (value) => {
        if (value !== null && !(value instanceof Storage))
          throw new TypeError("storageArea is not a Storage");
        return value;
      }],
    };
  defineInitEvent(class StorageEvent extends Event {
    constructor(type, init = undefined) {
      super(type, init ?? {});
      initEventStore(this, init, storageEventFields);
    }
    initStorageEvent(type, bubbles = false, cancelable = false, key = null,
      oldValue = null, newValue = null, url = "", storageArea = null) {
      if (arguments.length < 1)
        throw new TypeError("StorageEvent type is required");
      if (this.__dispatching) return;
      this.initEvent(type, bubbles, cancelable);
      initEventStore(this, { key, oldValue, newValue, url, storageArea },
        storageEventFields);
    }
  }, storageEventFields);
  const hashChangeEventFields = { oldURL: ["", String], newURL: ["", String] };
  defineInitEvent(class HashChangeEvent extends Event {
    constructor(type, init = undefined) {
      super(type, init ?? {});
      initEventStore(this, init, hashChangeEventFields);
    }
  }, hashChangeEventFields);
  const popStateEventFields = {
    state: [null, anyValue],
    hasUAVisualTransition: [false, Boolean],
  };
  defineInitEvent(class PopStateEvent extends Event {
    constructor(type, init = undefined) {
      super(type, init ?? {});
      initEventStore(this, init, popStateEventFields);
    }
  }, popStateEventFields);
  /* HTML's rejection tracking, as in workers: the engine's tracker reports
     each rejection to the host-locked __tilefinchQueuePromiseRejection; one
     still unhandled when its task runs fires unhandledrejection on the
     window, and a handler attached after that fires rejectionhandled. At
     most 16 rejections are tracked at once. */
  const promiseRejectionEventFields = {
      promise: [undefined, anyValue],
      reason: [undefined, anyValue],
    },
    PromiseRejectionEvent = class PromiseRejectionEvent extends Event {
      constructor(type, init = undefined) {
        if (init?.promise === undefined)
          throw new TypeError("PromiseRejectionEvent requires a promise");
        super(type, init);
        initEventStore(this, init, promiseRejectionEventFields);
      }
    },
    pendingRejections = new Map(),
    reportedRejections = [],
    fireRejection = (type, promise, reason) => domShared.browserDispatch(
      globalThis, trustedEvent(browserEvent(PromiseRejectionEvent, type, {
        cancelable: type === "unhandledrejection", promise, reason,
      })));
  defineInitEvent(PromiseRejectionEvent, promiseRejectionEventFields);
  Object.defineProperty(globalThis, "__tilefinchQueuePromiseRejection", {
    value(promise, reason, handled) {
      let record = pendingRejections.get(promise);
      if (handled) {
        if (!record) return;
        pendingRejections.delete(promise);
        if (!record.reported)
          return domShared.cancelTimer(record.timer);
        reportedRejections.splice(reportedRejections.indexOf(promise), 1);
        globalThis.__tilefinchScheduleTask(
          () => fireRejection("rejectionhandled", promise, reason));
        return;
      }
      while (!record && pendingRejections.size >= 16
             && reportedRejections.length)
        pendingRejections.delete(reportedRejections.shift());
      if (record || pendingRejections.size >= 16) return;
      record = { reported: false, timer: 0 };
      /* Tracked only once its task is queued: a full queue (0) or a throw
         leaves no record to hold the promise and fill the tracker. */
      record.timer = globalThis.__tilefinchScheduleTask(() => {
        if (pendingRejections.get(promise) !== record) return;
        record.reported = true;
        reportedRejections.push(promise);
        fireRejection("unhandledrejection", promise, reason);
      });
      if (record.timer) pendingRejections.set(promise, record);
    },
  });
  const messageEventStates = new WeakMap(),
    messageEventStateGet = messageEventStates.get.bind(messageEventStates),
    messageEventStateSet = messageEventStates.set.bind(messageEventStates),
    trustedIsArray = Array.isArray,
    messageEventPorts = (ports) => {
      const result = [];
      /* The browser's own port lists are arrays, copied by index so a
         page's array iterator never runs; other iterables are sequences. */
      if (trustedIsArray(ports))
        for (let at = 0; at < ports.length && at < 16; at++)
          result[at] = ports[at];
      else if (ports != null)
        for (const port of ports) {
          if (result.length >= 16) break;
          result[result.length] = port;
        }
      return Object.freeze(result);
    },
    messageEventState = (event) => {
      const state = messageEventStateGet(event);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    };
  globalThis.MessageEvent = class MessageEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      messageEventStateSet(this, {
        data: options.data === undefined ? null : options.data,
        origin: String(options.origin || ""),
        lastEventId: String(options.lastEventId || ""),
        source: options.source ?? null,
        ports: messageEventPorts(options.ports),
      });
    }
    initMessageEvent(type, bubbles = false, cancelable = false, data = null,
                     origin = "", lastEventId = "", source = null,
                     ports = []) {
      if (this.__dispatching) return;
      this.initEvent(type, bubbles, cancelable);
      messageEventStateSet(this, {
        data,
        origin: String(origin),
        lastEventId: String(lastEventId),
        source: source ?? null,
        ports: messageEventPorts(ports),
      });
    }
  };
  /* Web IDL installs attributes before operations and leaves the interface
     object's constructor property last.  Class syntax creates constructor and
     methods first, which is observably unlike browser MessageEvent and is used
     by compatibility probes.  Re-seat the two class-created properties once;
     this changes no per-event storage. */
  const messageEventConstructor = MessageEvent.prototype.constructor,
    messageEventInit = MessageEvent.prototype.initMessageEvent;
  delete MessageEvent.prototype.constructor;
  delete MessageEvent.prototype.initMessageEvent;
  const messageEventField = (name) => ({
    configurable: true,
    enumerable: true,
    get() { return messageEventState(this)[name]; },
  });
  Object.defineProperties(MessageEvent.prototype, {
    data: messageEventField("data"),
    origin: messageEventField("origin"),
    lastEventId: messageEventField("lastEventId"),
    source: messageEventField("source"),
    ports: messageEventField("ports"),
    userActivation: {
      configurable: true,
      enumerable: true,
      get() { return null; },
    },
    [Symbol.toStringTag]: {
      configurable: true,
      value: "MessageEvent",
    },
  });
  /* Native messaging already has a serialized value.  It must not feed that
     value back through MessageEventInit dictionary defaults: a serialized
     undefined is observably distinct from the dictionary's omitted-data null
     default.  Keep this constructor hidden and immutable for trusted bootstrap
     delivery paths. */
  Object.defineProperty(globalThis, "__tilefinchCreateMessageEvent", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(type, data, origin = "", source = null, ports = []) {
      const event = browserEvent(messageEventConstructor, type);
      messageEventStateSet(event, {
        data,
        origin: String(origin),
        lastEventId: "",
        source: source ?? null,
        ports: messageEventPorts(ports),
      });
      return event;
    },
  });
  Object.defineProperties(MessageEvent.prototype, {
    initMessageEvent: {
      configurable: true,
      writable: true,
      value: messageEventInit,
    },
    constructor: {
      configurable: true,
      writable: true,
      value: messageEventConstructor,
    },
  });
  const CloseEventInterface = globalThis.CloseEvent = class CloseEvent extends Event {
    constructor(type, options = {}) {
      options ??= {};
      super(type, options);
      this.wasClean = !!options.wasClean;
      this.code = Number(options.code) || 0;
      this.reason = String(options.reason || "");
    }
  };
  /* The interfaces the other bootstrap modules build browser events with,
     captured while compat.js runs for the same reason as EventInterface.
     They reach them through the share's createBrowserEvent (dom.js), and
     hardening.js publishes the table as __tilefinchEventInterfaces for the
     lazy modules to extend. */
  const browserInterfaces = trustedObjectFreeze({
    __proto__: null,
    Event: EventInterface,
    EventTarget,
    UIEvent,
    FocusEvent: FocusEventInterface,
    MouseEvent,
    PointerEvent,
    KeyboardEvent,
    InputEvent,
    CompositionEvent,
    SubmitEvent,
    FormDataEvent: FormDataEventInterface,
    TransitionEvent,
    ToggleEvent: ToggleEventInterface,
    ProgressEvent: progressEventConstructor,
    MessageEvent: messageEventConstructor,
    CloseEvent: CloseEventInterface,
    ErrorEvent: errorEventConstructor,
    HashChangeEvent,
    PopStateEvent,
  });
  __tilefinchRealmGlobal.__tilefinchTrustedEventShare.install(
    trustedEvent,
    (type, init, name = "Event") =>
      browserEvent(browserInterfaces[name], type, init),
    /* Only EventInterface's constructor gives an object event state. */
    (value) => eventStateGet(value) !== undefined,
    browserInterfaces,
  );
  const fileReaderDispatch = domShared.dispatchSimpleEvent;
  const markFocus = (target, on) => {
    if (!target?.__handle) return;
    if (on) __tilefinchSetAttribute(target.__handle, "data-tilefinch-focus", "");
    else __tilefinchRemoveAttribute(target.__handle, "data-tilefinch-focus");
  };
  const formAssociatedCustomDisabled =
    globalThis.__tilefinchFormAssociatedCustomDisabled;
  delete globalThis.__tilefinchFormAssociatedCustomDisabled;
  const isFocusable = (target) => {
    if (
      target.disabled ||
      formAssociatedCustomDisabled(target) ||
      target.hidden ||
      !target.isConnected ||
      getComputedStyle(target).visibility === "hidden"
    )
      return false;
    const tabindex = target.getAttribute("tabindex");
    if (tabindex !== null && /^[-+]?\d+$/.test(tabindex.trim())) return true;
    const tag = String(target.tagName).toLowerCase(),
      type = String(target.getAttribute("type") || "").toLowerCase();
    if (target.namespaceURI === "http://www.w3.org/2000/svg")
      return tag === "a" && target.hasAttribute("href");
    if (target.namespaceURI !== "http://www.w3.org/1999/xhtml") return false;
    if (tag === "input") return type !== "hidden";
    if (["button", "select", "textarea", "iframe"].includes(tag)) return true;
    if (tag === "a") return target.hasAttribute("href");
    if (tag === "summary") {
      const details = target.parentElement;
      return (
        details instanceof HTMLDetailsElement &&
        details.children.find(
          (child) => child instanceof HTMLSummaryElement,
        ) === target
      );
    }
    return target.isContentEditable;
  };
  let focusFixupPending = false,
    focusCheckQueued = false;
  const checkFocusAfterMutations = () => {
    focusCheckQueued = false;
    const active = globalThis.__tilefinchActiveElement;
    if (
      focusFixupPending ||
      !active ||
      active === document.body ||
      isFocusable(active)
    )
      return;
    focusFixupPending = true;
    const schedule =
      globalThis.__tilefinchScheduleRenderFixup ||
      ((callback) => __tilefinchRealmGlobal.__tilefinchScheduleFrame(callback));
    schedule(() => {
      focusFixupPending = false;
      const current = globalThis.__tilefinchActiveElement;
      if (
        current &&
        current !== document.body &&
        !isFocusable(current)
      )
        globalThis.__tilefinchActiveElement = document.body;
    });
  };
  const nativeIsAncestor = globalThis.__tilefinchIsAncestor,
    nativeIsConnected = globalThis.__tilefinchIsConnected,
    selectorsTestAttribute = globalThis.__tilefinchSelectorsTestAttribute;
  /* Whether a child-list insertion under `handle` moved the focused
     element: one of the inserted nodes is it or contains it (it stays
     connected but inherits its new ancestors' visibility). Only an
     insertion under one of its ancestors can; the added nodes are checked
     only then, so insertions beside or away from it stay cheap. */
  let rootElementHandle = 0;
  const focusSubtreeInserted = (handle, added, activeHandle) => {
    if (!added?.length) return false;
    if (handle > 0 && nativeIsAncestor(handle, activeHandle) === false) {
      /* The native chain stops at a shadow root, so this only proves the
         insertion missed the focused element in the light tree: one native
         walk to the root element instead of getRootNode's script walk
         (pages with shadow roots take that on every insertion). A stale or
         replaced root element answers "relevant". */
      if (!(rootElementHandle > 0))
        rootElementHandle = document.documentElement?.__handle || 0;
      return (
        !(rootElementHandle > 0) ||
        nativeIsAncestor(rootElementHandle, activeHandle) !== true
      );
    }
    for (let i = 0; i < added.length; i++) {
      const nodeHandle = added[i]?.__handle;
      if (!(nodeHandle > 0) || nodeHandle === activeHandle) return true;
      if (nativeIsAncestor(nodeHandle, activeHandle) !== false) return true;
    }
    return false;
  };
  /* Whether a mutation of `target` (a node or a native handle) can change
     the focused element's focusability: a change to the element itself or
     an ancestor (attributes, inline style and the inherited visibility
     they drive), a removal that disconnected it, or an insertion that moved
     it (a subtree holding it re-parented, e.g. under a hidden container).
     Other child-list or text changes do not matter; a data-* attribute no
     selector tests changes nothing. Anything the native tree cannot answer
     counts as relevant. Structural selectors that restyle through siblings
     are not followed (isFocusable itself reads only visibility). */
  const focusMutationRelevant = (target, active, type, name, added) => {
    const handle = typeof target === "number" ? target : target?.__handle,
      activeHandle = active.__handle;
    if (target === active || !(activeHandle > 0)) return true;
    if (type === "childList" || type === "characterData")
      return (
        !nativeIsConnected(activeHandle) ||
        (type === "childList" &&
          focusSubtreeInserted(handle, added, activeHandle))
      );
    if (
      type === "attributes" &&
      /^data-/i.test(name) &&
      !/^data-tilefinch/i.test(name) &&
      typeof selectorsTestAttribute === "function" &&
      !selectorsTestAttribute(name)
    )
      return false;
    if (!(handle > 0) || handle === activeHandle) return true;
    if (!nativeIsConnected(activeHandle)) return true;
    if (nativeIsAncestor(handle, activeHandle) !== false) return true;
    /* The native chain stops at a shadow root: inside a shadow tree the
       host's ancestors are not seen, so every mutation counts. */
    return active.getRootNode() !== document;
  };
  /* Called for every mutation (with its target, record type, attribute
     name and, for child lists, the added nodes) and inline style write
     (with the element's handle); no target:
     always relevant, e.g. a style sheet change. Whether the focused element
     survived them is asked once, after the task's mutations, not after
     each one, and only when one of them could have changed it: isFocusable
     reads the element's computed visibility, which re-cascades its
     ancestor chain once other mutations have invalidated the style cache
     (an answer streaming beside a focused composer did this after every
     task). */
  const nativeBodyHandle = globalThis.__tilefinchBody;
  globalThis.__tilefinchQueueFocusFixup = (target, type, name, added) => {
    if (focusCheckQueued || focusFixupPending) return;
    const active = globalThis.__tilefinchActiveElement;
    /* The body's handle: no wrapper lookup per mutation. */
    if (!active || active.__handle === nativeBodyHandle()) return;
    if (
      target !== undefined &&
      !focusMutationRelevant(target, active, type, name, added)
    )
      return;
    focusCheckQueued = true;
    queueMicrotask(checkFocusAfterMutations);
  };
  const collapseFocusSelection = (target) => {
    const type = String(target.type || "").toLowerCase();
    if (
      target instanceof HTMLInputElement &&
      (type === "text" || type === "number")
    ) {
      const parent = target.parentNode,
        offset = domShared.listIndex(parent?.childNodes || [], target);
      if (parent && offset >= 0) getSelection().collapse(parent, offset);
    } else if (target.isContentEditable && target.firstChild) {
      getSelection().collapse(target.firstChild, 0);
    }
  };
  /* Whether `target` is rendered for focus: neither it nor an ancestor
     (across shadow hosts) computes display:none. One native computed
     value per step and no declaration object, except where the host has
     no cascade to answer from (no stylesheet attached). */
  const computedStyleRead = globalThis.__tilefinchComputedStyleRead,
    computedStyleOf = getComputedStyle,
    focusRendered = (target) => {
      for (let at = target, steps = 0; at && steps < 1024; steps++) {
        const handle = at.__handle;
        if (!(handle > 0)) return true;
        const display = computedStyleRead(handle, "display", "") ||
          computedStyleOf(at).display;
        if (display === "none") return false;
        const parent = at.parentNode;
        at = parent instanceof ShadowRoot
          ? parent.host
          : parent?.nodeType === Node.ELEMENT_NODE ? parent : null;
      }
      return true;
    };
  const focusElement = function focus(options = undefined) {
    const shadow = globalThis.__tilefinchShadowRootForHost?.(this);
    if (shadow?.delegatesFocus) {
      let delegated = null;
      for (const candidate of __tilefinchArrayFrom(shadow.querySelectorAll("*")).slice(
        0,
        128,
      ))
        if (isFocusable(candidate)) {
          delegated = candidate;
          break;
        }
      if (delegated && delegated !== this) {
        trustedFunctionApply(focusElement, delegated, []);
        return;
      }
    }
    /* Only a rendered element takes focus. The fixup after mutations
       keeps the cheaper isFocusable alone. */
    if (
      globalThis.__tilefinchActiveElement === this ||
      !isFocusable(this) ||
      !focusRendered(this)
    )
      return;
    const previous = globalThis.__tilefinchActiveElement || document.body;
    if (!globalThis.__tilefinchFocusEventsObserved?.()) {
      if (previous && previous !== document.body) markFocus(previous, false);
      globalThis.__tilefinchActiveElement = this;
      collapseFocusSelection(this);
      markFocus(this, true);
      return;
    }
    /* Focus events are the browser's own: never a page's dispatchEvent. */
    const fire = (target, type, options) =>
      dispatchNode(target,
        trustedEvent(browserEvent(FocusEventInterface, type, options)));
    if (previous && previous !== document.body) {
      markFocus(previous, false);
      globalThis.__tilefinchActiveElement = document.body;
      fire(previous, "blur", { relatedTarget: this });
      fire(previous, "focusout", { bubbles: true, relatedTarget: this });
    }
    globalThis.__tilefinchActiveElement = this;
    collapseFocusSelection(this);
    markFocus(this, true);
    fire(this, "focus", { relatedTarget: previous });
    fire(this, "focusin", { bubbles: true, relatedTarget: previous });
  };
  const blurElement = function blur() {
    if (globalThis.__tilefinchActiveElement !== this) return;
    const next = document.body,
      fire = (type, options) =>
        dispatchNode(this,
          trustedEvent(browserEvent(FocusEventInterface, type, options)));
    markFocus(this, false);
    fire("blur", { relatedTarget: next });
    fire("focusout", { bubbles: true, relatedTarget: next });
    globalThis.__tilefinchActiveElement = next;
  };
  Element.prototype.focus = focusElement;
  Element.prototype.blur = blurElement;
  HTMLElement.prototype.focus = focusElement;
  HTMLElement.prototype.blur = blurElement;
  Object.defineProperties(HTMLDialogElement.prototype, {
    open: {
      get() {
        return this.hasAttribute("open");
      },
      set(value) {
        this.toggleAttribute("open", !!value);
      },
    },
    returnValue: {
      get() {
        return this.__returnValue || "";
      },
      set(value) {
        this.__returnValue = String(value);
      },
    },
  });
  HTMLDialogElement.prototype.show = function show() {
    if (!this.isConnected)
      throw new DOMException("Dialog is not connected", "InvalidStateError");
    if (this.open) {
      if (this.hasAttribute("data-tilefinch-modal"))
        throw new DOMException("Dialog is already modal", "InvalidStateError");
      return;
    }
    this.removeAttribute("data-tilefinch-modal");
    this.setAttribute("open", "");
  };
  HTMLDialogElement.prototype.showModal = function showModal() {
    if (!this.isConnected)
      throw new DOMException("Dialog is not connected", "InvalidStateError");
    if (this.open) {
      if (this.hasAttribute("data-tilefinch-modal")) return;
      throw new DOMException("Dialog is already open", "InvalidStateError");
    }
    this.setAttribute("data-tilefinch-modal", "");
    this.setAttribute("open", "");
    const target =
      this.querySelector("button,input,textarea,select,[tabindex]") || this;
    target.focus();
  };
  HTMLDialogElement.prototype.close = function close(value) {
    if (!this.open) return;
    if (value !== undefined) this.returnValue = value;
    this.removeAttribute("open");
    this.removeAttribute("data-tilefinch-modal");
    dispatchNode(this, trustedEvent(browserEvent(EventInterface, "close")));
  };
  HTMLDialogElement.prototype.requestClose = function requestClose(value) {
    if (!this.open) return;
    const event = trustedEvent(
      browserEvent(EventInterface, "cancel", { cancelable: true }),
    );
    if (dispatchNode(this, event)) this.close(value);
  };
  Object.defineProperty(HTMLDetailsElement.prototype, "open", Object.getOwnPropertyDescriptor({
    get open() {
      return this.hasAttribute("open");
    },
    set open(value) {
      this.toggleAttribute("open", !!value);
    },
  }, "open"));
  const detailsToggleTasks = new TrustedWeakMap();
  domShared.detailsOpenChanged = (details, open) => {
    let pending = trustedWeakMapGet(detailsToggleTasks, details);
    const newState = open ? "open" : "closed";
    if (pending) {
      pending.newState = newState;
      return;
    }
    pending = { oldState: open ? "closed" : "open", newState };
    const timer = __tilefinchRealmGlobal.__tilefinchScheduleTask?.(() => {
      trustedWeakMapDelete(detailsToggleTasks, details);
      dispatchNode(details, trustedEvent(
        browserEvent(ToggleEventInterface, "toggle", pending)));
    });
    /* A refused scheduler slot is not a permanently queued notification. */
    if (timer) trustedWeakMapSet(detailsToggleTasks, details, pending);
  };
  globalThis.__tilefinchDetailsDefault = (target) => {
    if (!(target instanceof HTMLSummaryElement)) return false;
    const details = target.parentElement;
    if (!(details instanceof HTMLDetailsElement)) return false;
    const first = details.children.find(
      (child) => child instanceof HTMLSummaryElement,
    );
    if (first !== target) return false;
    details.open = !details.open;
    return true;
  };
  Object.defineProperties(HTMLLabelElement.prototype, {
    htmlFor: {
      configurable: true,
      get() {
        return this.getAttribute("for") || "";
      },
      set(value) {
        this.setAttribute("for", String(value));
      },
    },
    control: {
      configurable: true,
      get() {
        const id = this.htmlFor;
        const candidates = id
          ? [document.getElementById(id)]
          : __tilefinchArrayFrom(this.querySelectorAll("*"));
        for (const candidate of candidates) {
          if (
            /^(?:button|input|meter|output|progress|select|textarea)$/.test(
              String(candidate?.localName || "").toLowerCase(),
            )
          )
            return candidate;
          const internals =
            globalThis.__tilefinchElementInternalsFor?.(candidate);
          if (
            !internals &&
            globalThis.__tilefinchFormAssociatedCustomElement?.(candidate)
          )
            return candidate;
          if (internals)
            try {
              void internals.form;
              return candidate;
            } catch (_) {}
        }
        return null;
      },
    },
    form: {
      configurable: true,
      get() {
        const control = this.control;
        if (!control) return null;
        if ("form" in control) return control.form;
        try {
          return globalThis.__tilefinchElementInternalsFor?.(control)?.form ||
            null;
        } catch (_) {
          return null;
        }
      },
    },
  });
  globalThis.__tilefinchBeginControlDefault = (target, native = false) => {
    if (native && target instanceof HTMLSelectElement) {
      /* options is a live HTMLOptionsCollection; take a snapshot. */
      const options = __tilefinchArrayFrom(target.options),
        states = options.map((option) => [option, option.selected]);
      if (!options.length) return null;
      let next = target.selectedIndex;
      for (let checked = 0; checked < options.length; checked++) {
        next = (next + 1 + options.length) % options.length;
        if (!options[next].disabled) break;
      }
      const changed = next !== target.selectedIndex && !options[next].disabled;
      if (changed) target.selectedIndex = next;
      return { kind: "select", target, changed, states };
    }
    if (
      !(target instanceof HTMLInputElement) &&
      !(target instanceof HTMLButtonElement)
    )
      return null;
    const type = String(target.type || "text").toLowerCase();
    if (type === "reset")
      return { kind: "reset", target, changed: false, states: [] };
    if (native && target instanceof HTMLInputElement && type === "range") {
      const value = target.value;
      target.stepUp();
      return {
        kind: "range",
        target,
        changed: target.value !== value,
        value,
        states: [],
      };
    }
    if (!(target instanceof HTMLInputElement)) return null;
    if (type === "checkbox") {
      const states = domShared.setChecked(target, !target.checked);
      const indeterminate = target.indeterminate;
      if (indeterminate) target.indeterminate = false;
      return {
        kind: "checkbox",
        target,
        changed: states !== null,
        states, indeterminate,
      };
    }
    if (type !== "radio") return null;
    if (target.checked)
      return {
        kind: "radio",
        target,
        changed: false,
        states: null,
      };
    const states = domShared.setChecked(target, true);
    return { kind: "radio", target, changed: states !== null, states };
  };
  globalThis.__tilefinchFinishControlDefault = (state, accepted) => {
    if (!state) return false;
    if (!accepted) {
      if (state.kind === "select") {
        /* Only the options the default moved: each write invalidates the
           select's cached selection, so rewriting all of them would
           recompute it once per option. */
        for (const [option, selected] of state.states)
          if (option.selected !== selected) option.selected = selected;
      } else if (state.kind === "range") state.target.value = state.value;
      else if (state.states) domShared.restoreChoices(state.states, true);
      if (state.kind === "checkbox" && state.target.indeterminate !== state.indeterminate)
        state.target.indeterminate = state.indeterminate;
      return true;
    }
    if (state.kind === "reset") {
      state.target.closest("form")?.reset();
      return true;
    }
    if (state.changed) {
      dispatchNode(state.target,
        trustedEvent(browserEvent(EventInterface, "input", { bubbles: true })),
      );
      dispatchNode(state.target,
        trustedEvent(browserEvent(EventInterface, "change", { bubbles: true })),
      );
    }
    return true;
  };
  Object.defineProperty(HTMLElement.prototype, "popover", Object.getOwnPropertyDescriptor({
    get popover() {
      return this.getAttribute("popover");
    },
    set popover(value) {
      value === null
        ? this.removeAttribute("popover")
        : this.setAttribute("popover", String(value));
    },
  }, "popover"));
  const setPopoverState = (target, open) => {
    if (!target.hasAttribute("popover"))
      throw new DOMException("Element is not a popover", "NotSupportedError");
    const current = target.hasAttribute("data-tilefinch-popover-open");
    if (current === open) return current;
    const oldState = current ? "open" : "closed",
      newState = open ? "open" : "closed",
      before = trustedEvent(
        browserEvent(ToggleEventInterface, "beforetoggle", {
          cancelable: open,
          oldState,
          newState,
        }),
      );
    if (!dispatchNode(target, before)) return current;
    if (open) target.setAttribute("data-tilefinch-popover-open", "");
    else target.removeAttribute("data-tilefinch-popover-open");
    dispatchNode(target,
      trustedEvent(
        browserEvent(ToggleEventInterface, "toggle", { oldState, newState }),
      ),
    );
    return open;
  };
  HTMLElement.prototype.showPopover = function showPopover() {
    setPopoverState(this, true);
  };
  HTMLElement.prototype.hidePopover = function hidePopover() {
    setPopoverState(this, false);
  };
  HTMLElement.prototype.togglePopover = function togglePopover(force) {
    const open =
      force === undefined
        ? !this.hasAttribute("data-tilefinch-popover-open")
        : !!force;
    return setPopoverState(this, open);
  };
  let readDOMException;
  globalThis.DOMException = DOMException = class DOMException extends Error {
    #state;
    static {
      readDOMException = value => #state in value ? value.#state : undefined;
    }
    /* The browser throws these too: its String and code table, not the
       page's String or Object.prototype. */
    constructor(message = "", name = "Error") {
      super(TrustedString(message));
      this.name = TrustedString(name);
      this.#state = { message: this.message, name: this.name };
      this.code =
        {
          __proto__: null,
          IndexSizeError: 1,
          HierarchyRequestError: 3,
          WrongDocumentError: 4,
          InvalidCharacterError: 5,
          NoModificationAllowedError: 7,
          NotFoundError: 8,
          NotSupportedError: 9,
          InUseAttributeError: 10,
          InvalidStateError: 11,
          SyntaxError: 12,
          InvalidModificationError: 13,
          NamespaceError: 14,
          InvalidAccessError: 15,
          TypeMismatchError: 17,
          SecurityError: 18,
          NetworkError: 19,
          AbortError: 20,
          URLMismatchError: 21,
          QuotaExceededError: 22,
          TimeoutError: 23,
          InvalidNodeTypeError: 24,
          DataCloneError: 25,
        }[this.name] || 0;
    }
  };
  domShared.readDOMException = readDOMException;
  /* dom.js, platform.js and custom-elements.js bind it now. */
  __tilefinchRealmGlobal.__tilefinchTrustedEventShare.defineDOMException(
    DOMException);
  for (const [name, code] of Object.entries({
    INDEX_SIZE_ERR: 1,
    DOMSTRING_SIZE_ERR: 2,
    HIERARCHY_REQUEST_ERR: 3,
    WRONG_DOCUMENT_ERR: 4,
    INVALID_CHARACTER_ERR: 5,
    NO_DATA_ALLOWED_ERR: 6,
    NO_MODIFICATION_ALLOWED_ERR: 7,
    NOT_FOUND_ERR: 8,
    NOT_SUPPORTED_ERR: 9,
    INUSE_ATTRIBUTE_ERR: 10,
    INVALID_STATE_ERR: 11,
    SYNTAX_ERR: 12,
    INVALID_MODIFICATION_ERR: 13,
    NAMESPACE_ERR: 14,
    INVALID_ACCESS_ERR: 15,
    VALIDATION_ERR: 16,
    TYPE_MISMATCH_ERR: 17,
    SECURITY_ERR: 18,
    NETWORK_ERR: 19,
    ABORT_ERR: 20,
    URL_MISMATCH_ERR: 21,
    QUOTA_EXCEEDED_ERR: 22,
    TIMEOUT_ERR: 23,
    INVALID_NODE_TYPE_ERR: 24,
    DATA_CLONE_ERR: 25,
  })) {
    Object.defineProperty(DOMException, name, { value: code });
    Object.defineProperty(DOMException.prototype, name, { value: code });
  }
  {
    const { Array, DOMRect, Element, document } = globalThis;
    /* Past the limit the oldest ranges stop following mutations (their
       boundaries keep their last values) instead of a new one throwing. */
    const liveRanges = [],
      liveRangeLimit = 4096,
      liveRangeDrop = 256,
      indexOf = Function.call.bind(Array.prototype.indexOf),
      push = Function.call.bind(Array.prototype.push),
      domPosition = Function.call.bind(Node.prototype.compareDocumentPosition),
      contains = Function.call.bind(Node.prototype.contains),
      has = Function.call.bind(Set.prototype.has),
      add = Function.call.bind(Set.prototype.add);
    const visitRanges = (visit) => {
      let kept = 0;
      for (let i = 0; i < liveRanges.length; i++) {
        const points = trustedWeakRefDeref(liveRanges[i]);
        if (!points) continue;
        liveRanges[kept++] = liveRanges[i];
        if (visit) {
          visit(points.start);
          visit(points.end);
        }
      }
      liveRanges.length = kept;
      if (!kept)
        domShared.rangeMutation = domShared.rangeSplit =
          domShared.rangeMerge = domShared.rangeReplace = null;
    };
    /* textContent and innerHTML are about to replace every child of
       `parent`: a boundary inside one moves to (parent, 0), without the
       child lists MutationObservers need. */
    const replaceRanges = (parent) => {
      let last, inside;
      visitRanges((point) => {
        if (point.node !== parent) {
          /* Boundaries share nodes: ask the DOM once per node. */
          if (point.node !== last)
            inside = contains(parent, last = point.node);
          if (!inside) return;
          point.node = parent;
        }
        point.offset = 0;
      });
    };
    const adjustRanges = (target, type, added, removed, previous, next,
                          oldValue, offset, count, length) => {
      if (type === "characterData") {
        count ??= oldValue?.length ?? 0;
        length ??= target.data.length;
        visitRanges((point) => {
          if (point.node !== target || point.offset <= offset) return;
          point.offset = point.offset <= offset + count
            ? offset : point.offset + length - count;
        });
      } else if (type === "childList" && (added.length || removed.length)) {
        let index, removedSet;
        const mutationIndex = () => {
          if (index !== undefined) return index;
          const children = target.childNodes;
          index = added.length ? indexOf(children, added[0]) : -1;
          if (index < 0) {
            index = next ? indexOf(children, next) : -1;
            if (index < 0 && previous) index = indexOf(children, previous) + 1;
            /* A move can already have reinserted the removed node in the
               final list. Recover its removal position without that node. */
            if (!added.length && removed.length === 1 && index > 0) {
              const moved = indexOf(children, removed[0]);
              if (moved >= 0 && moved < index) index--;
            }
          }
          if (index < 0) index = 0;
          return index;
        };
        let last, lastRemoved;
        const wasRemoved = (node) => node === last ? lastRemoved
          : (lastRemoved = removedFrom(last = node));
        const removedFrom = (node) => {
          if (removed.length === 1) return contains(removed[0], node);
          if (!removedSet) {
            removedSet = new Set();
            for (let i = 0; i < removed.length; i++)
              add(removedSet, removed[i]);
          }
          for (let at = node, depth = 0; at && depth < ancestorLimit;
               at = at.parentNode, depth++)
            if (has(removedSet, at)) return true;
          return false;
        };
        visitRanges((point) => {
          if (point.node !== target) {
            if (removed.length && wasRemoved(point.node)) {
              point.node = target;
              point.offset = mutationIndex();
            }
          } else if (point.offset > 0) {
            const at = mutationIndex();
            if (point.offset > at) {
              point.offset = Math.max(at, point.offset - removed.length);
              if (point.offset > at) point.offset += added.length;
            }
          }
        });
      }
    };
    const splitRanges = (node, offset, tail, parent) => {
      const index = indexOf(parent.childNodes, tail);
      visitRanges((point) => {
        if (point.node === node && point.offset > offset) {
          point.node = tail;
          point.offset -= offset;
        } else if (point.node === parent && point.offset === index) {
          point.offset++;
        }
      });
    };
    const mergeRanges = (node, previous, offset, parent) => {
      let index;
      visitRanges((point) => {
        if (point.node === node) {
          point.node = previous;
          point.offset += offset;
        } else if (point.node === parent &&
            point.offset === (index ??= indexOf(parent.childNodes, node))) {
          point.node = previous;
          point.offset = offset;
        }
      });
    };
    const nodeExtent = (node) => isCharacterDataNode(node)
      ? node.data.length : node?.childNodes?.length || 0;
    const boundedOffset = (node, value) => {
      value = Number(value);
      if (
        !node ||
        !Number.isInteger(value) ||
        value < 0 ||
        value > nodeExtent(node)
      )
        throw new DOMException("Invalid range offset", "IndexSizeError");
      return value;
    };
    const ancestor = (left, right) => {
      const seen = new Set();
      for (
        let at = left, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentNode, steps++
      )
        seen.add(at);
      for (
        let at = right, steps = 0;
        at && steps < ancestorLimit;
        at = at.parentNode, steps++
      )
        if (seen.has(at)) return at;
      return document;
    };
    /* Whether boundary point (node, offset) is after (other, otherOffset), or
       in another tree (a range across trees collapses as if it were). */
    const boundaryAfter = (node, offset, other, otherOffset) =>
      node !== other &&
        node.compareDocumentPosition(other) & Node.DOCUMENT_POSITION_DISCONNECTED
        ? true
        : compareBoundaryPoints(node, offset, other, otherOffset) > 0;
    const isTextNode = (node) =>
      node?.nodeType === Node.TEXT_NODE ||
      node?.nodeType === Node.CDATA_SECTION_NODE;
    /* The next node in tree order after `node` and its descendants. */
    const followingNode = (node) => {
      for (let at = node, steps = 0; at && steps < ancestorLimit; steps++) {
        if (at.nextSibling) return at.nextSibling;
        at = at.parentNode;
      }
      return null;
    };
    /* DOM's boundary-point comparison: -1 before, 0 equal, 1 after. */
    const compareBoundaryPoints = (node, offset, otherNode, otherOffset) => {
      if (node === otherNode)
        return offset === otherOffset ? 0 : offset < otherOffset ? -1 : 1;
      const position = node.compareDocumentPosition(otherNode);
      if (position & Node.DOCUMENT_POSITION_PRECEDING)
        return -compareBoundaryPoints(otherNode, otherOffset, node, offset);
      if (position & Node.DOCUMENT_POSITION_CONTAINED_BY) {
        let child = otherNode;
        for (let steps = 0; child && child.parentNode !== node &&
             steps < ancestorLimit; steps++)
          child = child.parentNode;
        const index = Array.prototype.indexOf.call(node.childNodes, child);
        return index < offset ? 1 : -1;
      }
      return -1;
    };
    /* The DOM standard's "extract" and "clone the contents" of a range.
       Partially contained ancestors are cloned shallowly and filled from a
       subrange; contained children are moved (or deep-cloned). chatgpt.com's
       streaming renderer extracts ranges that start and end in different
       elements to commit an answer's final text. */
    const isCharacterDataNode = (node) =>
        node?.nodeType === Node.TEXT_NODE ||
        node?.nodeType === Node.CDATA_SECTION_NODE ||
        node?.nodeType === Node.COMMENT_NODE ||
        node?.nodeType === Node.PROCESSING_INSTRUCTION_NODE,
      rangeNodeLength = (node) =>
        isCharacterDataNode(node) ? node.data.length : node.childNodes.length,
      inclusiveAncestor = (ancestorNode, node) =>
        ancestorNode === node || !!ancestorNode?.contains?.(node),
      rangeContents = (range, extract, depth) => {
        if (depth > ancestorLimit)
          throw new DOMException("Range is too deeply nested",
                                 "NotSupportedError");
        const start = range.startContainer, startOffset = range.startOffset,
          end = range.endContainer, endOffset = range.endOffset,
          owner = start.ownerDocument || document,
          fragment = owner.createDocumentFragment();
        if (range.collapsed) return fragment;
        if (start === end && isCharacterDataNode(start)) {
          const clone = start.cloneNode(false);
          clone.data = start.data.substring(startOffset, endOffset);
          fragment.appendChild(clone);
          if (extract) start.deleteData(startOffset, endOffset - startOffset);
          return fragment;
        }
        let common = start;
        for (let steps = 0; !inclusiveAncestor(common, end); steps++) {
          if (!common.parentNode || steps > ancestorLimit)
            throw new DOMException("Range has no common ancestor",
                                   "NotSupportedError");
          common = common.parentNode;
        }
        const children = [...common.childNodes];
        const firstPartial = inclusiveAncestor(start, end) ? null
          : children.find((child) => inclusiveAncestor(child, start)) || null;
        const lastPartial = inclusiveAncestor(end, start) ? null
          : children.find((child) => inclusiveAncestor(child, end)) || null;
        const contained = children.filter((child, index) =>
          compareBoundaryPoints(common, index, start, startOffset) >= 0 &&
          compareBoundaryPoints(common, index + 1, end, endOffset) <= 0 &&
          child !== firstPartial && child !== lastPartial);
        if (contained.some((child) => child.nodeType === Node.DOCUMENT_TYPE_NODE))
          throw new DOMException("Range contains a doctype",
                                 "HierarchyRequestError");
        let newNode = start, newOffset = startOffset;
        if (extract && !inclusiveAncestor(start, end)) {
          let reference = start;
          for (let steps = 0; reference.parentNode &&
               !inclusiveAncestor(reference.parentNode, end) &&
               steps < ancestorLimit; steps++)
            reference = reference.parentNode;
          newNode = reference.parentNode;
          newOffset = Array.prototype.indexOf.call(newNode.childNodes,
                                                   reference) + 1;
        }
        if (extract) {
          range.setStart(newNode, newOffset);
          range.setEnd(newNode, newOffset);
        }
        const partial = (child, fromStart) => {
          if (isCharacterDataNode(child)) {
            const clone = child.cloneNode(false);
            if (fromStart) {
              clone.data = start.data.substring(startOffset);
              if (extract)
                start.deleteData(startOffset, start.data.length - startOffset);
            } else {
              clone.data = end.data.substring(0, endOffset);
              if (extract) end.deleteData(0, endOffset);
            }
            fragment.appendChild(clone);
            return;
          }
          const clone = child.cloneNode(false), sub = new Range();
          fragment.appendChild(clone);
          if (fromStart) {
            sub.setStart(start, startOffset);
            sub.setEnd(child, rangeNodeLength(child));
          } else {
            sub.setStart(child, 0);
            sub.setEnd(end, endOffset);
          }
          clone.appendChild(rangeContents(sub, extract, depth + 1));
        };
        if (firstPartial) partial(firstPartial, true);
        for (const child of contained)
          fragment.appendChild(extract ? child : child.cloneNode(true));
        if (lastPartial) partial(lastPartial, false);
        return fragment;
      };
    const Range = domShared.Range = __tilefinchRealmGlobal.Range = class Range {
      #points;
      constructor() {
        if (liveRanges.length >= liveRangeLimit) visitRanges(null);
        if (liveRanges.length >= liveRangeLimit) {
          const kept = liveRanges.length - liveRangeDrop;
          for (let i = 0; i < kept; i++)
            liveRanges[i] = liveRanges[i + liveRangeDrop];
          liveRanges.length = kept;
        }
        this.#points = {
          start: { node: document, offset: 0 },
          end: { node: document, offset: 0 },
        };
        push(liveRanges, new TrustedWeakRef(this.#points));
        domShared.rangeMutation = adjustRanges;
        domShared.rangeSplit = splitRanges;
        domShared.rangeMerge = mergeRanges;
        domShared.rangeReplace = replaceRanges;
      }
      get startContainer() { return this.#points.start.node; }
      get startOffset() { return this.#points.start.offset; }
      get endContainer() { return this.#points.end.node; }
      get endOffset() { return this.#points.end.offset; }
      #setStart(node, offset) {
        this.#points.start.node = node;
        this.#points.start.offset = offset;
      }
      #setEnd(node, offset) {
        this.#points.end.node = node;
        this.#points.end.offset = offset;
      }
      get collapsed() {
        return (
          this.startContainer === this.endContainer &&
          this.startOffset === this.endOffset
        );
      }
      get commonAncestorContainer() {
        return ancestor(this.startContainer, this.endContainer);
      }
      /* A start after the end (or in another tree) collapses the range to
         it, and an end before the start likewise. */
      setStart(node, offset) {
        offset = boundedOffset(node, offset);
        this.#setStart(node, offset);
        if (boundaryAfter(node, offset, this.endContainer, this.endOffset)) {
          this.#setEnd(node, offset);
        }
      }
      setEnd(node, offset) {
        offset = boundedOffset(node, offset);
        this.#setEnd(node, offset);
        if (boundaryAfter(this.startContainer, this.startOffset, node, offset)) {
          this.#setStart(node, offset);
        }
      }
      setStartBefore(node) {
        this.setStart(
          node.parentNode,
          Array.prototype.indexOf.call(node.parentNode.childNodes, node),
        );
      }
      setStartAfter(node) {
        this.setStart(
          node.parentNode,
          Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1,
        );
      }
      setEndBefore(node) {
        this.setEnd(
          node.parentNode,
          Array.prototype.indexOf.call(node.parentNode.childNodes, node),
        );
      }
      setEndAfter(node) {
        this.setEnd(
          node.parentNode,
          Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1,
        );
      }
      collapse(toStart = false) {
        if (toStart) {
          this.#setEnd(this.startContainer, this.startOffset);
        } else {
          this.#setStart(this.endContainer, this.endOffset);
        }
      }
      selectNodeContents(node) {
        this.#setStart(node, 0);
        this.#setEnd(node, nodeExtent(node));
      }
      cloneRange() {
        const copy = new Range();
        copy.#setStart(this.startContainer, this.startOffset);
        copy.#setEnd(this.endContainer, this.endOffset);
        return copy;
      }
      extractContents() {
        return rangeContents(this, true, 0);
      }
      deleteContents() {
        rangeContents(this, true, 0);
      }
      cloneContents() {
        return rangeContents(this, false, 0);
      }
      /* DOM Parsing: parse with the start node (or its parent element) as
         the fragment parser's context, <body> for none or <html>. Unlike
         innerHTML, the scripts stay unstarted and script-inserted, so they
         run, under CSP, once the fragment is connected. Reddit's bundles
         build markup this way. */
      createContextualFragment(markup) {
        markup = String(markup);
        const node = this.startContainer,
          type = node?.nodeType,
          owner =
            type === Node.DOCUMENT_NODE ? node : node?.ownerDocument || document;
        let element =
          type === Node.ELEMENT_NODE
            ? node
            : type === Node.TEXT_NODE ||
                type === Node.CDATA_SECTION_NODE ||
                type === Node.COMMENT_NODE
              ? node.parentElement
              : null,
          name = String(element?.localName || "").toLowerCase();
        if (
          !element ||
          (name === "html" &&
            element.namespaceURI === "http://www.w3.org/1999/xhtml")
        )
          name = "body";
        if (owner !== document) {
          /* A script-side DOMParser/createHTMLDocument document: its own
             bounded innerHTML copy. XML fragment parsing is not provided. */
          if (owner.contentType !== "text/html")
            throw new DOMException(
              "XML fragment parsing is not supported",
              "NotSupportedError",
            );
          const holder = owner.createElement(name),
            result = owner.createDocumentFragment();
          holder.innerHTML = markup;
          for (const child of [...holder.childNodes]) result.appendChild(child);
          return result;
        }
        const result = document.createDocumentFragment();
        if (!__tilefinchSetInnerHTML(result.__handle, markup, name))
          throw new DOMException(
            "Contextual fragment exceeds bounded parsing",
            "NotSupportedError",
          );
        return result;
      }
      selectNode(node) {
        const parent = node?.parentNode;
        if (!parent)
          throw new DOMException("Node has no parent", "InvalidNodeTypeError");
        const index = Array.prototype.indexOf.call(parent.childNodes, node);
        this.#setStart(parent, index);
        this.#setEnd(parent, index + 1);
      }
      compareBoundaryPoints(how, source) {
        how = Number(how);
        if (!(source instanceof Range)) throw new TypeError("Range required");
        if (!(how >= 0 && how <= 3 && Number.isInteger(how)))
          throw new DOMException("Invalid comparison", "NotSupportedError");
        const ownEnd = how === 1 || how === 2,
          sourceEnd = how === 2 || how === 3;
        return compareBoundaryPoints(
          ownEnd ? this.endContainer : this.startContainer,
          ownEnd ? this.endOffset : this.startOffset,
          sourceEnd ? source.endContainer : source.startContainer,
          sourceEnd ? source.endOffset : source.startOffset,
        );
      }
      comparePoint(node, offset) {
        if (node?.getRootNode?.() !== this.startContainer.getRootNode?.())
          throw new DOMException("Node is in another tree", "WrongDocumentError");
        offset = boundedOffset(node, offset);
        if (compareBoundaryPoints(node, offset, this.startContainer,
                                  this.startOffset) < 0) return -1;
        return compareBoundaryPoints(node, offset, this.endContainer,
                                     this.endOffset) > 0 ? 1 : 0;
      }
      isPointInRange(node, offset) {
        if (node?.getRootNode?.() !== this.startContainer.getRootNode?.())
          return false;
        return this.comparePoint(node, offset) === 0;
      }
      intersectsNode(node) {
        if (!(node instanceof Node)) throw new TypeError("Node required");
        if (domPosition(node, this.startContainer) &
            Node.DOCUMENT_POSITION_DISCONNECTED) return false;
        const parent = node.parentNode;
        if (!parent) return true;
        const offset = Array.prototype.indexOf.call(parent.childNodes, node);
        return (
          compareBoundaryPoints(parent, offset, this.endContainer,
                                this.endOffset) < 0 &&
          compareBoundaryPoints(parent, offset + 1, this.startContainer,
                                this.startOffset) > 0
        );
      }
      insertNode(node) {
        if (!(node instanceof Node)) throw new TypeError("Node required");
        const container = this.startContainer,
          offset = this.startOffset,
          split = isTextNode(container),
          parent = split ? container.parentNode : container;
        if (!parent || node === container ||
            (!split && isCharacterDataNode(container)))
          throw new DOMException("Invalid range insertion", "HierarchyRequestError");
        let reference = split ? container : container.childNodes[offset] || null;
        __tilefinchValidatePreInsert(parent, node, reference);
        if (split) reference = container.splitText(offset);
        if (node === reference) reference = node.nextSibling;
        const count = node instanceof DocumentFragment ? node.childNodes.length : 1;
        let end = reference
          ? Array.prototype.indexOf.call(parent.childNodes, reference)
          : parent.childNodes.length;
        if (node.parentNode === parent &&
            Array.prototype.indexOf.call(parent.childNodes, node) < end)
          end--;
        parent.insertBefore(node, reference);
        if (this.collapsed)
          this.#setEnd(parent, Math.min(end + count, parent.childNodes.length));
      }
      surroundContents(newParent) {
        if (!(newParent instanceof Node)) throw new TypeError("Node required");
        const common = this.commonAncestorContainer;
        for (const endpoint of [this.startContainer, this.endContainer]) {
          for (let at = endpoint, depth = 0; at && at !== common &&
               depth < ancestorLimit; at = at.parentNode, depth++) {
            if (!isTextNode(at))
              throw new DOMException("Range partly contains a non-Text node",
                                     "InvalidStateError");
          }
        }
        if (newParent.nodeType === Node.DOCUMENT_NODE ||
            newParent.nodeType === Node.DOCUMENT_TYPE_NODE ||
            newParent.nodeType === Node.DOCUMENT_FRAGMENT_NODE)
          throw new DOMException("Invalid range container", "InvalidNodeTypeError");
        const fragment = this.extractContents();
        if (newParent.firstChild) newParent.replaceChildren();
        this.insertNode(newParent);
        newParent.appendChild(fragment);
        this.selectNode(newParent);
      }
      /* The text of the Text nodes between the boundary points, with the
         start and end Text nodes cut at their offsets. */
      toString() {
        const start = this.startContainer, end = this.endContainer,
          startOffset = this.startOffset, endOffset = this.endOffset;
        if (start === end && isTextNode(start))
          return start.data.substring(startOffset, endOffset);
        let text = isTextNode(start) ? start.data.substring(startOffset) : "",
          at = isCharacterDataNode(start)
            ? followingNode(start)
            : start.childNodes[startOffset] ?? followingNode(start);
        const stop = isCharacterDataNode(end)
          ? end
          : end.childNodes[endOffset] ?? followingNode(end);
        for (; at && at !== stop; at = at.firstChild || followingNode(at))
          if (isTextNode(at)) text += at.data;
        if (isTextNode(end) && end !== start)
          text += end.data.substring(0, endOffset);
        return text;
      }
      getBoundingClientRect() {
        const node = this.commonAncestorContainer;
        return node instanceof Element
          ? node.getBoundingClientRect()
          : node?.parentElement?.getBoundingClientRect() || new DOMRect();
      }
      getClientRects() {
        const rect = this.getBoundingClientRect();
        return domShared.rectList(rect.width && rect.height ? rect : null);
      }
      detach() {}
    };
    ["START_TO_START", "START_TO_END", "END_TO_END", "END_TO_START"].forEach(
      (name, value) => {
        const constant = { value, enumerable: true };
        Object.defineProperty(Range, name, constant);
        Object.defineProperty(Range.prototype, name, constant);
      },
    );
  }
  {
    const sameNode = (left, right) =>
        left === right ||
        (!!left &&
          !!right &&
          left.__handle !== undefined &&
          left.__handle === right.__handle),
      parentKind = (node) => Number(node?.nodeType),
      parentOf = (node) =>
        node?.__tilefinchDetachedParent || node?.parentNode || null,
      /* By index: a page's patch of the array iterator never runs. */
      childrenOf = (node) => domShared.listCopy(node?.childNodes || []),
      nativeRootNode = globalThis.__tilefinchRootNode,
      wrapHandle = globalThis.__tilefinchWrap,
      /* Is `node` a host-including inclusive ancestor of `parent`: one native
         walk to the root past shadow roots and template contents; a root with
         a script-side detached parent takes the bounded walk, which throws on
         a cycle or an over-deep chain. */
      isAncestor = (node, parent) => {
        if (
          typeof nativeRootNode === "function" &&
          parent?.__handle > 0 &&
          node?.__handle > 0 &&
          !parent.__tilefinchDetachedParent
        ) {
          const found = Number(
            nativeRootNode(parent.__handle, node.__handle, true));
          if (found === Number(node.__handle)) return true;
          if (found < 0) return false;
          if (found > 0 && !wrapHandle(found)?.__tilefinchDetachedParent)
            return false;
        }
        for (
          let at = parent, steps = 0;
          at;
          at = at instanceof ShadowRoot ? at.host : parentOf(at), steps++
        ) {
          if (steps >= ancestorLimit)
            throw new DOMException(
              "Cyclic or excessively deep node ancestry",
              "HierarchyRequestError",
            );
          if (sameNode(at, node)) return true;
        }
        return false;
      };
    const nativePreInsertClear = globalThis.__tilefinchPreInsertClear,
      planDocumentInsert = domShared.planDocumentInsert;
    const validatePreInsert = (
      parent,
      node,
      child,
      replacingAll = false,
    ) => {
      /* Every insertion: the native table settles the ordinary case, and
         script raises what applies in any other. */
      const clear = nativePreInsertClear(
        parent?.__handle, node, node?.__handle, child, child?.__handle);
      if (clear > 0 ||
          (clear < 0 && !wrapHandle(-clear)?.__tilefinchDetachedParent))
        return true;
      if (!(node instanceof Node)) throw new TypeError("Node required");
      if (child !== null && child !== undefined && !(child instanceof Node))
        throw new TypeError("Reference child must be a Node or null");
      const parentType = parentKind(parent);
      if (
        parentType !== Node.DOCUMENT_NODE &&
        parentType !== Node.DOCUMENT_FRAGMENT_NODE &&
        parentType !== Node.ELEMENT_NODE
      )
        throw new DOMException(
          "Node cannot have children",
          "HierarchyRequestError",
        );
      if (isAncestor(node, parent))
        throw new DOMException(
          "Node is an ancestor of parent",
          "HierarchyRequestError",
        );
      if (
        child !== null &&
        child !== undefined &&
        !sameNode(parentOf(child), parent)
      )
        throw new DOMException(
          "Reference node is not a child",
          "NotFoundError",
        );
      const nodeType = parentKind(node);
      if (
        domShared.listIndex(
          [
            Node.DOCUMENT_FRAGMENT_NODE,
            Node.DOCUMENT_TYPE_NODE,
            Node.ELEMENT_NODE,
            Node.TEXT_NODE,
            Node.CDATA_SECTION_NODE,
            Node.PROCESSING_INSTRUCTION_NODE,
            Node.COMMENT_NODE,
          ],
          nodeType,
        ) < 0
      )
        throw new DOMException(
          "Node type cannot be inserted",
          "HierarchyRequestError",
        );
      if (parentType !== Node.DOCUMENT_NODE) {
        if (nodeType === Node.DOCUMENT_TYPE_NODE)
          throw new DOMException(
            "Doctype requires a document parent",
            "HierarchyRequestError",
          );
        return true;
      }
      /* The shared document check (platform.js), on the children as this
         node and reference see them. */
      if (replacingAll) planDocumentInsert([], node, false, null);
      else {
        const children = childrenOf(parent);
        for (let at = 0; at < children.length; at++)
          children[at] = sameNode(children[at], node) ? node
            : sameNode(children[at], child) ? child : children[at];
        planDocumentInsert(children, node, false, child ?? null);
      }
      return true;
    };
    globalThis.__tilefinchValidatePreInsert = validatePreInsert;
    const genericInsertBefore = function insertBefore(node, child) {
      if (arguments.length < 2)
        throw new TypeError("insertBefore requires two arguments");
      validatePreInsert(this, node, child);
      const own = this.insertBefore;
      if (typeof own === "function" && own !== genericInsertBefore)
        return own.call(this, node, child);
      throw new DOMException(
        "Node cannot accept children",
        "HierarchyRequestError",
      );
    };
    Object.defineProperty(Node.prototype, "insertBefore", {
      configurable: true,
      writable: true,
      value: genericInsertBefore,
    });
    const parentAppend = (parent, values, prepend) => {
      /* A document checks the whole list before any argument moves; a
         script-side one then takes the nodes in directly, so a doctype
         need not pass through a fragment. */
      if (parent.nodeType === Node.DOCUMENT_NODE) {
        const plan = planDocumentInsert(
          childrenOf(parent), values, true, prepend ? undefined : null);
        const { items } = plan;
        if (trustedIsArray(parent.__detachedChildren)) {
          for (let at = 0; at < items.length; at++)
            rawDetachedInsert(parent, items[at], plan.before);
          return;
        }
        /* The page's document takes no new children (platform.js
           replaceChildren): a call that leaves the children as they are,
           such as append(root, root), changes nothing, as in Chromium, and
           any other throws before an argument moves. */
        const children = childrenOf(parent), after = [];
        for (let at = 0; at < children.length; at++)
          if (domShared.listIndex(items, children[at]) < 0)
            after[after.length] = children[at];
        const place = plan.before
            ? domShared.listIndex(after, plan.before) : after.length,
          /* As splice() takes it: -1 is before the last. */
          start = place >= 0 ? place : after.length > 0 ? after.length - 1 : 0;
        let same = after.length + items.length === children.length;
        for (let at = 0; same && at < children.length; at++)
          same = children[at] === (at < start ? after[at]
            : at < start + items.length ? items[at - start]
            : after[at - items.length]);
        if (!same)
          throw new DOMException("The document cannot take new children",
            "HierarchyRequestError");
        return;
      }
      const owner =
          parent.nodeType === Node.DOCUMENT_NODE
            ? parent
            : parent.ownerDocument || document,
        nodes = values.map((value) =>
          value instanceof Node ? value : owner.createTextNode(String(value)),
        );
      if (nodes.length === 0) return;
      /* A single node has no temporary fragment in the DOM algorithm;
         moving it through one fires disconnection too early. */
      if (nodes.length === 1) {
        parent.insertBefore(nodes[0], prepend ? parent.firstChild : null);
        return;
      }
      let insertion = nodes[0];
      if (nodes.length > 1) {
        insertion = owner.createDocumentFragment();
        for (const node of nodes) insertion.appendChild(node);
      }
      validatePreInsert(parent, insertion, prepend ? parent.firstChild : null);
      if (
        !prepend &&
        parent.__handle !== undefined &&
        typeof parent.append === "function" && parent.append !== append
      ) {
        const batch =
          insertion.nodeType === Node.DOCUMENT_FRAGMENT_NODE
            ? [...insertion.childNodes]
            : [insertion];
        globalThis.__tilefinchParentAppendBypass = true;
        try {
          return parent.append(...batch);
        } finally {
          globalThis.__tilefinchParentAppendBypass = false;
        }
      }
      parent.insertBefore(insertion, prepend ? parent.firstChild : null);
    };
    globalThis.__tilefinchParentAppend = parentAppend;
    const append = function append(...values) {
        return parentAppend(this, values, false);
      },
      prepend = function prepend(...values) {
        return parentAppend(this, values, true);
      };
    for (const proto of [
      Document.prototype,
      DocumentFragment.prototype,
      Element.prototype,
    ]) {
      Object.defineProperties(proto, {
        append: { configurable: true, writable: true, value: append },
        prepend: { configurable: true, writable: true, value: prepend },
      });
    }
    let markupOf = null;
    const detachedTreeConnected = (node) => {
        for (
          let at = node, steps = 0;
          at && steps < ancestorLimit;
          at =
            at instanceof ShadowRoot
              ? at.host
              : parentOf(at),
            steps++
        )
          if (at.nodeType === Node.DOCUMENT_NODE) return true;
        return false;
      },
      adoptTreeOwner = (node, owner) => {
        if (!node || node.nodeType === Node.DOCUMENT_NODE) return;
        if ("__detachedOwner" in node) node.__detachedOwner = owner;
        else globalThis.__tilefinchSetNodeOwner(node, owner);
        const children = node.childNodes || [];
        for (let at = 0; at < children.length; at++)
          adoptTreeOwner(children[at], owner);
      },
      rawDetachedInsert = (parent, node, child) => {
        if (node === child) return node;
        const values =
            node.nodeType === Node.DOCUMENT_FRAGMENT_NODE
              ? childrenOf(node)
              : [node],
          removals = [];
        for (let at = 0; at < values.length; at++) {
          const value = values[at];
          removals[at] = {
            value,
            parent: parentOf(value),
            connected: detachedTreeConnected(value),
            owner: value.ownerDocument || null,
            previousSibling: value.previousSibling,
            nextSibling: value.nextSibling,
          };
        }
        for (let at = 0; at < removals.length; at++)
          if (removals[at].connected)
            globalThis.__tilefinchCustomElementDisconnected?.(
              removals[at].value,
            );
        for (let at = 0; at < values.length; at++) {
          const value = values[at],
            old = parentOf(value);
          if (trustedIsArray(old?.__detachedChildren)) {
            const list = old.__detachedChildren;
            domShared.listDrop(list, domShared.listIndex(list, value));
          }
          if (value.__detachedParent !== undefined)
            value.__detachedParent = null;
          value.__tilefinchDetachedParent = null;
        }
        const list = parent.__detachedChildren,
          at =
            child === null || child === undefined
              ? list.length
              : domShared.listIndex(list, child);
        if (at < 0)
          throw new DOMException(
            "Reference node is not a child",
            "NotFoundError",
          );
        for (let move = list.length - 1; move >= at; move--)
          list[move + values.length] = list[move];
        for (let index = 0; index < values.length; index++)
          list[at + index] = values[index];
        for (let index = 0; index < values.length; index++) {
          const value = values[index];
          if (value.__detachedParent !== undefined)
            value.__detachedParent = parent;
          value.__tilefinchDetachedParent = parent;
          adoptTreeOwner(
            value,
            parent.nodeType === Node.DOCUMENT_NODE
              ? parent
              : parent.ownerDocument,
          );
          const nextOwner = value.ownerDocument || null,
            removal = removals[index];
          if (removal?.owner && nextOwner && removal.owner !== nextOwner) {
            globalThis.__tilefinchPrepareCustomElementAdoptionTree?.(
              value,
              removal.owner,
              nextOwner,
            );
            globalThis.__tilefinchCustomElementAdoptedTree?.(
              value,
              removal.owner,
              nextOwner,
            );
          }
          if (!value.__tilefinchDetachedRemove)
            value.__tilefinchDetachedRemove = value.remove;
          value.remove = function () {
            const owner = parentOf(this);
            if (trustedIsArray(owner?.__detachedChildren))
              owner.removeChild(this);
            else if (typeof this.__tilefinchDetachedRemove === "function")
              this.__tilefinchDetachedRemove.call(this);
          };
        }
        for (let index = 0; index < removals.length; index++)
          if (removals[index].parent)
            globalThis.__tilefinchNotifyMutation?.(
              removals[index].parent,
              "childList",
              null,
              [],
              [removals[index].value],
              null,
              removals[index].previousSibling,
              removals[index].nextSibling,
            );
        if (values.length)
          globalThis.__tilefinchNotifyMutation?.(
            parent,
            "childList",
            null,
            values,
            [],
            null,
            values[0].previousSibling,
            values[values.length - 1].nextSibling,
          );
        if (detachedTreeConnected(parent))
          for (let index = 0; index < values.length; index++)
            globalThis.__tilefinchCustomElementConnected?.(values[index], true);
        return node;
      },
      /* Script-side nodes have no native serializer: serialize.js walks
         them (HTML for an HTML document's nodes, XML otherwise). Loading it
         through the XMLSerializer accessor captured here survives a page
         that replaced the global. */
      loadMarkup = Object.getOwnPropertyDescriptor(globalThis,
                                                   "XMLSerializer")?.get,
      detachedMarkup = (node, self) => {
        if (!markupOf) {
          loadMarkup?.call(globalThis);
          markupOf = globalThis.__tilefinchMarkup;
          delete globalThis.__tilefinchMarkup;
        }
        return markupOf(node,
                        (node.ownerDocument || node).contentType !== "text/html",
                        self);
      },
      markupAccessor = (node) => {
        if (node.nodeType === Node.ELEMENT_NODE)
          Object.defineProperty(node, "innerHTML", {
            configurable: true,
            get() {
              return detachedMarkup(this, false);
            },
            set: Object.getOwnPropertyDescriptor(node, "innerHTML")?.set,
          });
        return node;
      },
      installContainer = (container) => {
        markupAccessor(container);
        Object.defineProperties(container, {
          dispatchEvent: {
            configurable: true,
            writable: true,
            value: function dispatchEvent(event) {
              const path = boundedAncestorPath(
                this,
                (at) => at.__tilefinchDetachedParent || at.parentNode,
              );
              globalThis.__tilefinchPrepareEvent(event, this, path, true);
              for (
                let at = path.length - 1;
                at >= 1 && !event.__stopped;
                at--
              )
                globalThis.__tilefinchInvokeEventTarget(
                  path[at],
                  event,
                  true,
                  1,
                );
              if (!event.__stopped) {
                globalThis.__tilefinchInvokeEventTarget(
                  this,
                  event,
                  true,
                  2,
                );
                if (!event.__stopped)
                  globalThis.__tilefinchInvokeEventTarget(
                    this,
                    event,
                    false,
                    2,
                  );
              }
              if (event.bubbles && !event.__stopped)
                for (let at = 1; at < path.length && !event.__stopped; at++)
                  globalThis.__tilefinchInvokeEventTarget(
                    path[at],
                    event,
                    false,
                    3,
                  );
              globalThis.__tilefinchFinishEventDispatch(event);
              return !event.defaultPrevented;
            },
          },
          insertBefore: {
            configurable: true,
            writable: true,
            value: function insertBefore(node, child) {
              if (arguments.length < 2)
                throw new TypeError("insertBefore requires two arguments");
              validatePreInsert(this, node, child);
              return rawDetachedInsert(this, node, child);
            },
          },
          appendChild: {
            configurable: true,
            writable: true,
            value: function appendChild(node) {
              return this.insertBefore(node, null);
            },
          },
          append: { configurable: true, writable: true, value: append },
          prepend: { configurable: true, writable: true, value: prepend },
          replaceChildren: {
            configurable: true,
            writable: true,
            value: function replaceChildren(...values) {
              const items = this.nodeType === Node.DOCUMENT_NODE
                  ? planDocumentInsert(childrenOf(this), values, true, null).items
                  : null,
                owner = this.ownerDocument || this,
                nodes = [],
                moves = [];
              /* By index: no page patch of the Array methods or iterator. */
              for (let at = 0; at < values.length; at++)
                nodes[at] = values[at] instanceof Node ? values[at]
                  : owner.createTextNode(String(values[at]));
              const insertion =
                nodes.length === 1 ? nodes[0] : owner.createDocumentFragment();
              for (let at = 0; at < nodes.length; at++) {
                const node = nodes[at],
                  candidates = node.nodeType === Node.DOCUMENT_FRAGMENT_NODE
                    ? childrenOf(node) : [node];
                for (let n = 0; n < candidates.length; n++) {
                  const candidate = candidates[n];
                  if (candidate.parentNode)
                    moves[moves.length] = {
                      node: candidate,
                      parent: candidate.parentNode,
                      previousSibling: candidate.previousSibling,
                      nextSibling: candidate.nextSibling,
                    };
                }
              }
              if (!items) validatePreInsert(this, insertion, null, true);
              globalThis.__tilefinchMutationSuppressed =
                (globalThis.__tilefinchMutationSuppressed || 0) + 1;
              let removed;
              try {
                if (!items && nodes.length !== 1)
                  for (let at = 0; at < nodes.length; at++)
                    insertion.appendChild(nodes[at]);
                removed = childrenOf(this);
                for (let at = 0; at < removed.length; at++)
                  this.removeChild(removed[at]);
                if (items)
                  for (let at = 0; at < items.length; at++)
                    rawDetachedInsert(this, items[at], null);
                else if (nodes.length) this.appendChild(insertion);
              } finally {
                globalThis.__tilefinchMutationSuppressed--;
              }
              for (let at = 0; at < moves.length; at++) {
                const move = moves[at];
                globalThis.__tilefinchNotifyMutation?.(
                  move.parent,
                  "childList",
                  null,
                  [],
                  [move.node],
                  null,
                  move.previousSibling,
                  move.nextSibling,
                );
              }
              if (removed.length || nodes.length)
                globalThis.__tilefinchNotifyMutation?.(
                  this,
                  "childList",
                  null,
                  nodes,
                  removed,
                );
            },
          },
          removeChild: {
            configurable: true,
            writable: true,
            value: function removeChild(node) {
              if (!sameNode(parentOf(node), this))
                throw new DOMException("Node is not a child", "NotFoundError");
              const previousSibling = node.previousSibling,
                nextSibling = node.nextSibling,
                connected = detachedTreeConnected(node),
                at = domShared.listIndex(this.__detachedChildren, node);
              if (connected)
                globalThis.__tilefinchCustomElementDisconnected?.(node);
              domShared.listDrop(this.__detachedChildren, at);
              if (node.__detachedParent !== undefined)
                node.__detachedParent = null;
              node.__tilefinchDetachedParent = null;
              globalThis.__tilefinchNotifyMutation?.(
                this,
                "childList",
                null,
                [],
                [node],
                null,
                previousSibling,
                nextSibling,
              );
              return node;
            },
          },
        });
        if (container.nodeType !== Node.DOCUMENT_NODE)
          Object.defineProperty(container, "textContent", {
            configurable: true,
            get() {
              return this.__detachedChildren
                .map((node) => node.textContent ?? "")
                .join("");
            },
            set(value) {
              const removed = [...this.__detachedChildren];
              globalThis.__tilefinchMutationSuppressed =
                (globalThis.__tilefinchMutationSuppressed || 0) + 1;
              try {
                for (const child of removed) this.removeChild(child);
                value = String(value);
                if (value)
                  this.appendChild(
                    this.ownerDocument.createTextNode(value),
                  );
              } finally {
                globalThis.__tilefinchMutationSuppressed--;
              }
              globalThis.__tilefinchNotifyMutation?.(
                this,
                "childList",
                null,
                [...this.__detachedChildren],
                removed,
              );
            },
          });
        return container;
      };
    const detachedLeaf = (owner, type, name, data = "") => {
      const proto =
          type === Node.DOCUMENT_TYPE_NODE
            ? DocumentType.prototype
            : type === Node.COMMENT_NODE
              ? Comment.prototype
              : type === Node.TEXT_NODE
                ? Text.prototype
                : Node.prototype,
        node = Object.create(proto);
      Object.defineProperties(node, {
        __detachedOwner: { value: owner, writable: true, configurable: true },
        __detachedParent: { value: null, writable: true },
        nodeType: { value: type },
        nodeName: { value: name },
        ownerDocument: {
          get() {
            return this.__detachedOwner;
          },
        },
        parentNode: {
          get() {
            return this.__detachedParent;
          },
        },
        parentElement: {
          get() {
            return this.__detachedParent?.nodeType === Node.ELEMENT_NODE
              ? this.__detachedParent
              : null;
          },
        },
        nextSibling: {
          get() {
            const p = this.__detachedParent;
            if (!p) return null;
            return (
              p.__detachedChildren[domShared.listIndex(p.__detachedChildren, this) + 1] ||
              null
            );
          },
        },
        previousSibling: {
          get() {
            const p = this.__detachedParent;
            if (!p) return null;
            return (
              p.__detachedChildren[domShared.listIndex(p.__detachedChildren, this) - 1] ||
              null
            );
          },
        },
        data: { value: String(data), writable: true },
        textContent: {
          get() {
            return type === Node.DOCUMENT_TYPE_NODE ? null : this.data;
          },
          set(value) {
            if (type !== Node.DOCUMENT_TYPE_NODE) this.data = String(value);
          },
        },
        nodeValue: {
          get() {
            return type === Node.DOCUMENT_TYPE_NODE ? null : this.data;
          },
          set(value) {
            if (type !== Node.DOCUMENT_TYPE_NODE) this.data = String(value);
          },
        },
      });
      node.remove = function () {
        const parent = this.parentNode;
        if (parent) parent.removeChild(this);
      };
      node.cloneNode = function () {
        return detachedLeaf(owner, type, name, node.data);
      };
      node.insertBefore = genericInsertBefore;
      return node;
    };
    const originalXMLDocument = document.implementation.createDocument,
      upgradeDocument = (doc) => {
        const upgrade = (node, parent = null) => {
          if (parent)
            Object.defineProperty(node, "__tilefinchDetachedParent", {
              configurable: true,
              writable: true,
              value: parent,
            });
          if (trustedIsArray(node?.__detachedChildren)) {
            installContainer(node);
          }
          const children = childrenOf(node);
          for (let at = 0; at < children.length; at++)
            upgrade(children[at], node);
          return node;
        };
        installContainer(doc);
        const children = childrenOf(doc);
        for (let at = 0; at < children.length; at++) upgrade(children[at], doc);
        const originalCreateElement = doc.createElement,
          originalCreateElementNS = doc.createElementNS,
          originalCreateFragment = doc.createDocumentFragment;
        doc.createElement = (tag) => upgrade(originalCreateElement(tag));
        doc.createElementNS = (namespace, tag) =>
          upgrade(originalCreateElementNS(namespace, tag));
        doc.createDocumentFragment = () => upgrade(originalCreateFragment());
        doc.createComment = (value) =>
          detachedLeaf(doc, Node.COMMENT_NODE, "#comment", value);
        doc.createProcessingInstruction = (target, data) =>
          detachedLeaf(
            doc,
            Node.PROCESSING_INSTRUCTION_NODE,
            String(target),
            data,
          );
        doc.createCDATASection = (data) =>
          detachedLeaf(doc, Node.CDATA_SECTION_NODE, "#cdata-section", data);
        Object.defineProperty(doc, "doctype", {
          configurable: true,
          get() {
            return domShared.listOfType(
              this.__detachedChildren,
              Node.DOCUMENT_TYPE_NODE,
              true,
            );
          },
        });
        return doc;
      };
    const originalNewDocument = globalThis.__tilefinchNewDocument;
    if (typeof originalNewDocument === "function")
      globalThis.__tilefinchNewDocument = (xml) =>
        upgradeDocument(originalNewDocument(xml));
    const makeDoctype = (name = "html", publicId = "", systemId = "") => {
      name = checkDoctypeName(name);
      const node = detachedLeaf(null, Node.DOCUMENT_TYPE_NODE, name);
      Object.defineProperties(node, {
        name: { value: name },
        publicId: { value: String(publicId) },
        systemId: { value: String(systemId) },
      });
      node.cloneNode = () => makeDoctype(name, publicId, systemId);
      return node;
    };
    document.implementation.createDocumentType = makeDoctype;
    /* Secondary documents backed by native nodes (createHTMLDocument and
       DOMParser): the document object is script-side, but its tree is a
       detached native subtree whose nodes report it as their owner (the
       bridge's per-handle owner tag; parsed descendants inherit it). Every
       element member, selector, innerHTML and serializer is therefore the
       real one, each node costs what a page node costs, and the document has
       no browsing context, so custom elements are neither constructed nor
       upgraded until a node is adopted into the page. */
    /* The builder is the lazy documents.js module, loaded on first use. What
       it reads from the realm is captured now, before page script runs. */
    const documentBuilderInputs = {
        factories: domShared.documentFactories,
        createPrivateSlot: domShared.createPrivateSlot,
        setNativeNodeOwner: globalThis.__tilefinchSetNodeOwner,
        adoptIntoDocument: globalThis.__tilefinchAdoptNodeOwner,
        liveCollection: globalThis.__tilefinchLiveHTMLCollection,
        liveChildNodes: domShared.liveChildNodes,
        ancestorLimit: globalThis.__tilefinchAncestorLimit,
        escapeSelector: CSS.escape,
        makeDoctype,
        asciiLower,
        createAttribute: globalThis.__tilefinchCreateAttribute,
        secondaryDocumentPrototype,
        htmlDocumentPrototype: HTMLDocument.prototype,
        xmlDocumentPrototype: XMLDocument.prototype,
        realm: globalThis,
        Node,
        Document,
        ShadowRoot,
        Range,
        HTMLBodyElement,
        HTMLFrameSetElement,
        DOMException,
        MapType: Map,
        objectCreate: Object.create,
        defineProperty: Object.defineProperty,
        defineProperties: Object.defineProperties,
        arrayFrom: Array.from,
        planDocumentInsert,
        listIndex: domShared.listIndex,
        listCopy: domShared.listCopy,
      },
      ensureDocumentsBootstrap = globalThis.__tilefinchEnsureDocumentsBootstrap;
    let documentBuilder = null;
    const nativeDocuments = () => {
      if (documentBuilder) return documentBuilder;
      ensureDocumentsBootstrap();
      const { realm } = documentBuilderInputs,
        create = realm.__tilefinchCreateSecondaryDocuments;
      delete realm.__tilefinchCreateSecondaryDocuments;
      return (documentBuilder = create(documentBuilderInputs));
    };
    globalThis.DOMParser = class DOMParser {
      parseFromString(input, type) {
        const source = String(input),
          mime = String(type).trim().toLowerCase();
        if (
          mime !== "text/html" &&
          mime !== "text/xml" &&
          mime !== "application/xml" &&
          mime !== "application/xhtml+xml" &&
          mime !== "image/svg+xml"
        )
          throw new TypeError("Unsupported DOMParser type: " + mime);
        if (source.length > 256 * 1024)
          throw new RangeError("parsed document exceeds bounded size");
        if (mime === "text/html")
          return nativeDocuments().parseNativeHTMLDocument(source);
        if (mime === "image/svg+xml")
          return nativeDocuments().parseNativeSVGDocument(source, mime);
        return nativeDocuments().parseErrorDocument(
          mime, "XML documents are not supported by this browser.");
      }
    };
    domShared.createHTMLDocument = (title) =>
      nativeDocuments().createNativeHTMLDocument(title);
    /* A frame's documents are native secondary documents, so its elements
       are the typed native elements a page's are (HTMLImageElement and the
       rest), not script stand-ins. */
    globalThis.__tilefinchCreateFrameDocument = (standards) =>
      nativeDocuments().createNativeFrameDocument(standards);
    globalThis.__tilefinchBindFrameDocument = (doc, location, view) =>
      nativeDocuments().bindFrameDocument(doc, location, view);
    /* dom.js's innerHTML getter for a native XML document's elements. */
    domShared.markup = detachedMarkup;
    Object.defineProperty(Element.prototype, "outerHTML", Object.getOwnPropertyDescriptor({
      get outerHTML() {
        const owner = this.ownerDocument;
        if (this.__handle === undefined ? this.__detachedChildren
            : owner !== document && owner.contentType !== "text/html")
          return detachedMarkup(this, true);
        const container = (
          this.__handle !== undefined ? document : this.ownerDocument || document
        ).createElement("div");
        container.appendChild(this.cloneNode(true));
        return container.innerHTML;
      },
      /* The parent is the parser's context (domShared.contextFragment). */
      set outerHTML(value) {
        /* A native secondary document's root has no parentNode. */
        const parent = this.parentNode;
        if (parent ? parent.nodeType === Node.DOCUMENT_NODE
            : this.ownerDocument?.documentElement === this)
          throw new DOMException("The element's parent is a document",
            "NoModificationAllowedError");
        if (!parent) return;
        const fragment = contextFragment(parent, String(value), this),
          added = [...fragment.childNodes];
        const previousSibling = this.previousSibling,
          nextSibling = this.nextSibling;
        globalThis.__tilefinchMutationSuppressed =
          (globalThis.__tilefinchMutationSuppressed || 0) + 1;
        try {
          parent.insertBefore(fragment, this);
          parent.removeChild(this);
        } finally {
          globalThis.__tilefinchMutationSuppressed--;
        }
        globalThis.__tilefinchNotifyMutation?.(
          parent,
          "childList",
          null,
          added,
          [this],
          null,
          previousSibling,
          nextSibling,
        );
      },
    }, "outerHTML"));
    Document.prototype.cloneNode = function cloneNode(deep = false) {
      const clone = this.contentType === "text/html"
        ? globalThis.__tilefinchNewDocument() : new Document();
      if (deep)
        for (const child of this.childNodes || [])
          clone.appendChild(child.cloneNode(true));
      return clone;
    };
    document.implementation.createDocument = (
      namespace,
      qualifiedName,
      doctype,
    ) => {
      const doc = upgradeDocument(
        originalXMLDocument(namespace, qualifiedName || "", doctype || null),
      );
      Object.setPrototypeOf(doc, secondaryDocumentPrototype(XMLDocument.prototype));
      doc.createAttribute = (name) =>
        __tilefinchCreateAttribute(doc, String(name));
      doc.createAttributeNS = (namespace, name) =>
        __tilefinchCreateAttribute(doc, name, namespace, true);
      const cloneXMLDocument = doc.cloneNode;
      doc.cloneNode = (deep = false) => {
        const clone = cloneXMLDocument.call(doc, deep);
        Object.setPrototypeOf(clone, secondaryDocumentPrototype(XMLDocument.prototype));
        return clone;
      };
      return doc;
    };
    const doctypeWrap = globalThis.__tilefinchWrap,
      doctypeRelation = globalThis.__tilefinchRelation;
    const mainDoctype =
      childrenOf(document).find(
        (node) => node.nodeType === Node.DOCUMENT_TYPE_NODE,
      ) || makeDoctype("html");
    mainDoctype.__detachedOwner = document;
    mainDoctype.__detachedParent = document;
    if (mainDoctype.__handle !== undefined) {
      Object.defineProperties(mainDoctype, {
        name: { configurable: true, value: "html" },
        nodeName: { configurable: true, value: "html" },
        publicId: { configurable: true, value: "" },
        systemId: { configurable: true, value: "" },
        ownerDocument: {
          configurable: true,
          get() {
            return this.__detachedOwner;
          },
        },
        /* The native parent: a page script may remove the doctype. */
        parentNode: {
          configurable: true,
          get() {
            return doctypeRelation(this.__handle, 8) ? document : null;
          },
        },
        parentElement: { configurable: true, get: () => null },
        /* The parsed doctype keeps its native siblings (a comment may sit
           between it and <html>). */
        previousSibling: {
          configurable: true,
          get() {
            return doctypeWrap(doctypeRelation(this.__handle, 6)) || null;
          },
        },
        nextSibling: {
          configurable: true,
          get() {
            return doctypeWrap(doctypeRelation(this.__handle, 5)) || null;
          },
        },
      });
    }
    Object.defineProperty(document, "doctype", {
      configurable: true,
      enumerable: true,
      get() {
        return mainDoctype.parentNode === document ? mainDoctype : null;
      },
    });
    document.createProcessingInstruction = (target, data) =>
      detachedLeaf(
        document,
        Node.PROCESSING_INSTRUCTION_NODE,
        String(target),
        data,
      );
    document.createCDATASection = (data) =>
      detachedLeaf(document, Node.CDATA_SECTION_NODE, "#cdata-section", data);
  }
  let readAbortSignalState, readAbortControllerState;
  const defaultAbortReason = () =>
    new DOMException("This operation was aborted", "AbortError"),
    abortSignalToken = Object.freeze({}),
    abortSignalState = (signal) => {
      const state = readAbortSignalState(signal);
      if (!state) throw new TrustedTypeError("Illegal invocation");
      return state;
    },
    abortControllerState = (controller) => {
      const state = readAbortControllerState(controller);
      if (!state) throw new TrustedTypeError("Illegal invocation");
      return state;
    },
    addAbortAlgorithm = (signal, algorithm) => {
      const state = abortSignalState(signal);
      if (state.aborted) return false;
      if (state.algorithms.length >= 128)
        throw new RangeError("Abort algorithm limit exceeded");
      state.algorithms.push(algorithm);
      return true;
    },
    removeAbortAlgorithm = (signal, algorithm) => {
      const state = abortSignalState(signal),
        at = state.algorithms.indexOf(algorithm);
      if (at >= 0) state.algorithms.splice(at, 1);
    },
    addAbortDependent = (source, dependent) => {
      const state = abortSignalState(source),
        retained = state.dependents;
      let write = 0;
      for (let at = 0; at < retained.length; at++)
        if (trustedWeakRefDeref(retained[at].target))
          retained[write++] = retained[at];
      retained.length = write;
      if (retained.length >= 128)
        throw new RangeError("Abort dependency limit exceeded");
      const entry = { target: new TrustedWeakRef(dependent) };
      retained.push(entry);
      return entry;
    },
    removeAbortDependent = (source, entry) => {
      const retained = abortSignalState(source).dependents,
        at = retained.indexOf(entry);
      if (at >= 0) retained.splice(at, 1);
    },
    abortRootSignals = (signals) => {
      const roots = [];
      for (const signal of signals) {
        const state = abortSignalState(signal),
          candidates = state.sourceSignals === null
            ? [signal] : state.sourceSignals;
        for (const candidate of candidates) {
          if (roots.includes(candidate)) continue;
          if (roots.length >= 64)
            throw new RangeError("AbortSignal source limit exceeded");
          roots.push(candidate);
        }
      }
      return roots;
    },
    registerAbortDependencies = (dependent, sources) => {
      const subscriptions = [];
      try {
        for (const source of sources)
          subscriptions.push({
            signal: source,
            entry: addAbortDependent(source, dependent),
          });
      } catch (error) {
        for (const item of subscriptions)
          removeAbortDependent(item.signal, item.entry);
        throw error;
      }
      abortSignalState(dependent).sourceSignals = sources.slice();
      return subscriptions;
    },
    markAbortSignal = (signal, reason, queue) => {
      const state = abortSignalState(signal);
      if (state.aborted) return false;
      state.aborted = true;
      state.reason = reason === undefined ? defaultAbortReason() : reason;
      state.nextAbortEvent = null;
      if (queue.tail)
        abortSignalState(queue.tail).nextAbortEvent = signal;
      else queue.head = signal;
      queue.tail = signal;
      return true;
    },
    abortSignal = (signal, reason) => {
      const queue = { head: null, tail: null };
      if (!markAbortSignal(signal, reason, queue)) return;
      /* Flatten the dependent graph in source-registration order before
       * running cleanup. Source listeners therefore observe every dependent
       * as aborted, while listeners bound to a dependent remain installed
       * until that dependent's own abort steps run. */
      for (let current = queue.head; current;
           current = abortSignalState(current).nextAbortEvent) {
        const state = abortSignalState(current),
          dependents = state.dependents.splice(0);
        for (const entry of dependents) {
          const dependent = trustedWeakRefDeref(entry.target);
          if (dependent) markAbortSignal(dependent, state.reason, queue);
        }
      }
      let current = queue.head;
      while (current) {
        const state = abortSignalState(current),
          next = state.nextAbortEvent,
          algorithms = state.algorithms.splice(0);
        state.nextAbortEvent = null;
        for (const algorithm of algorithms)
          try { algorithm(); } catch (error) {
            __tilefinchReportUncaught(error, "AbortSignal algorithm");
          }
        /* EventTarget's own dispatch, never a page's replacement. */
        fileReaderDispatch(current, trustedEvent(browserEvent(EventInterface, "abort")));
        current = next;
      }
    };
  globalThis.AbortSignal = class AbortSignal extends EventTarget {
    #state;
    static {
      /* onabort and fetch algorithms may close over the signal. Keep the
         cycle on its owner, not in a realm-global weak-key value. */
      readAbortSignalState = signal => signal &&
        (typeof signal === "object" || typeof signal === "function") &&
        #state in signal ? signal.#state : undefined;
    }
    constructor(token) {
      if (token !== abortSignalToken)
        throw new TrustedTypeError("Illegal constructor");
      super();
      const state = {
        aborted: false,
        reason: undefined,
        onabort: null,
        onabortListener: null,
        algorithms: [],
        dependents: [],
        /* null denotes an independent root. A dependent created from an empty
         * sequence has an empty array and must not become a fictitious root. */
        sourceSignals: null,
        nextAbortEvent: null,
      };
      state.onabortListener = (event) => state.onabort?.call(this, event);
      this.#state = state;
    }
    get aborted() {
      return abortSignalState(this).aborted;
    }
    get reason() {
      return abortSignalState(this).reason;
    }
    get onabort() {
      return abortSignalState(this).onabort;
    }
    set onabort(callback) {
      const state = abortSignalState(this);
      const next = typeof callback === "function" ? callback : null;
      if (state.onabort === null && next !== null)
        trustedAddEventListener(this, "abort", state.onabortListener);
      else if (state.onabort !== null && next === null)
        trustedRemoveEventListener(this, "abort", state.onabortListener);
      state.onabort = next;
    }
    throwIfAborted() {
      const state = abortSignalState(this);
      if (state.aborted) throw state.reason;
    }
    static abort(
      reason = defaultAbortReason(),
    ) {
      const signal = new AbortSignal(abortSignalToken);
      abortSignal(signal, reason);
      return signal;
    }
    static timeout(milliseconds) {
      milliseconds = Number(milliseconds);
      if (!Number.isFinite(milliseconds) || milliseconds < 0)
        throw new RangeError("Invalid abort timeout");
      milliseconds = Math.min(0xffffffff, Math.trunc(milliseconds));
      const signal = new AbortSignal(abortSignalToken);
      __tilefinchRealmGlobal.__tilefinchScheduleTask(
        () =>
          abortSignal(signal,
            new DOMException("The operation timed out", "TimeoutError"),
          ),
        milliseconds,
      );
      return signal;
    }
    static any(signals) {
      if (signals === null || signals === undefined
          || typeof signals[Symbol.iterator] !== "function")
        throw new TypeError("AbortSignal sequence required");
      const result = new AbortSignal(abortSignalToken),
        inputs = [];
      let subscriptions = [];
      let count = 0;
      const cleanup = () => {
        for (const item of subscriptions)
          removeAbortDependent(item.signal, item.entry);
        subscriptions.length = 0;
      };
      try {
        for (const signal of signals) {
          if (++count > 64)
            throw new RangeError("AbortSignal sequence limit exceeded");
          if (!(signal instanceof AbortSignal))
            throw new TypeError("AbortSignal sequence contains invalid value");
          inputs.push(signal);
        }
        /* Web IDL converts the complete sequence before the DOM algorithm
         * examines signal state. This also preserves the first aborted input's
         * reason when iterator side effects abort more than one source. */
        for (const signal of inputs) {
          if (signal.aborted) {
            abortSignal(result, signal.reason);
            return result;
          }
        }
        subscriptions = registerAbortDependencies(
          result, abortRootSignals(inputs));
        addAbortAlgorithm(result, cleanup);
      } catch (error) {
        cleanup();
        throw error;
      }
      return result;
    }
  };
  globalThis.AbortController = class AbortController {
    #state;
    static {
      readAbortControllerState = controller => controller &&
        (typeof controller === "object" || typeof controller === "function") &&
        #state in controller ? controller.#state : undefined;
    }
    constructor() {
      this.#state = {
        signal: new AbortSignal(abortSignalToken),
      };
    }
    get signal() {
      return abortControllerState(this).signal;
    }
    abort(reason) {
      abortSignal(abortControllerState(this).signal, reason);
    }
  };
  trustedDefineProperty(globalThis, "__tilefinchAddAbortAlgorithm", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: addAbortAlgorithm,
  });
  trustedDefineProperty(globalThis, "__tilefinchRemoveAbortAlgorithm", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: removeAbortAlgorithm,
  });
  const requestSignalFollowers = new TrustedFinalizationRegistry((held) => {
      for (const item of held.subscriptions) {
        const source = trustedWeakRefDeref(item.source);
        if (source) removeAbortDependent(source, item.entry);
      }
    }),
    followAbortSignal = (source) => {
      const signal = new AbortSignal(abortSignalToken),
        sourceState = abortSignalState(source);
      if (sourceState.aborted) {
        abortSignal(signal, sourceState.reason);
        return { signal, dispose() {} };
      }
      const subscriptions = registerAbortDependencies(
        signal, abortRootSignals([source]));
      let active = true;
      const dispose = () => {
        if (!active) return;
        active = false;
        for (const item of subscriptions)
          removeAbortDependent(item.signal, item.entry);
        subscriptions.length = 0;
        trustedFinalizationRegistryUnregister(requestSignalFollowers, signal);
      };
      addAbortAlgorithm(signal, dispose);
      trustedFinalizationRegistryRegister(
        requestSignalFollowers,
        signal,
        {
          subscriptions: subscriptions.map((item) => ({
            source: new TrustedWeakRef(item.signal),
            entry: item.entry,
          })),
        },
        signal,
      );
      return { signal, dispose };
    };
  Object.defineProperty(globalThis, "__tilefinchAbortSignalBrand", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(signal) {
      return abortSignalState(signal).aborted;
    },
  });
  const httpTokenPattern = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
  const isHttpToken = (value) => httpTokenPattern.test(String(value));
  const invalidHeaderValue = (value) =>
    /[\x00-\x08\x0a-\x1f\x7f]/.test(String(value));
  const normalizeHeaderName = (name) => {
    name = String(name);
    if (!isHttpToken(name)) throw new TypeError("Invalid HTTP header name");
    return name.toLowerCase();
  };
  const normalizeHeaderValue = (value) => {
    value = String(value);
    if (invalidHeaderValue(value))
      throw new TypeError("Invalid HTTP header value");
    return value.replace(/^[ \t]+|[ \t]+$/g, "");
  };
  const forbiddenMethod = (method) =>
    ["CONNECT", "TRACE", "TRACK"].includes(String(method).toUpperCase());
  const normalizeMethod = (method) => {
    const upper = method.toUpperCase();
    return /^(DELETE|GET|HEAD|OPTIONS|POST|PUT)$/.test(upper) ? upper : method;
  };
  const normalizeXhrMethod = (method) => {
    method = String(method);
    if (!isHttpToken(method))
      throw new DOMException("Invalid HTTP method", "SyntaxError");
    if (forbiddenMethod(method))
      throw new DOMException("Forbidden HTTP method", "SecurityError");
    return normalizeMethod(method);
  };
  const normalizeXhrHeader = (name, value) => {
    name = String(name);
    value = String(value);
    if (!isHttpToken(name) || invalidHeaderValue(value))
      throw new DOMException("Invalid HTTP header", "SyntaxError");
    return [name, value];
  };
  const installXhrValidation = (XHR) => {
    const nativeOpen = XHR.prototype.open,
      nativeSetRequestHeader = XHR.prototype.setRequestHeader;
    /* WebIDL length 2: method and url are required. */
    XHR.prototype.open = function open(method, url) {
      const args = [...arguments];
      args[0] = normalizeXhrMethod(method);
      return nativeOpen.apply(this, args);
    };
    XHR.prototype.setRequestHeader = function setRequestHeader(name, value) {
      [name, value] = normalizeXhrHeader(name, value);
      return nativeSetRequestHeader.call(this, name, value);
    };
  };
  const headerStorage = new TrustedWeakMap(),
    immutableHeaderMaps = new WeakSet(),
    headerRevision = Symbol(),
    copyHeaderValues = (source) => {
      const copy = [];
      for (let i = 0; i < source.length; i++) copy[i] = source[i];
      return copy;
    },
    joinHeaderValues = Function.call.bind(Array.prototype.join),
    headerEntryLimit = 256,
    headerMap = (headers) => {
      if (!trustedWeakMapHas(headerStorage, headers))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(headerStorage, headers);
    },
    writableHeaderMap = (headers) => {
      const map = headerMap(headers);
      if (trustedWeakSetHas(immutableHeaderMaps, map))
        throw new TrustedTypeError("Headers are immutable");
      return map;
    },
    sortedHeaderEntries = (headers) => {
      const entries = [];
      for (const [name, value] of trustedMapEntries(headerMap(headers))) {
        if (name === "set-cookie") {
          for (let i = 0; i < value.length; i++)
            entries.push([name, value[i]]);
        } else entries.push([name, value]);
      }
      trustedArraySort(entries, (left, right) =>
        left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0);
      return entries;
    },
    checkHeaderEntryLimit = (map) => {
      /* Ordinary repeated names share one slot. Cookie entries cannot be
         combined, so count every field against the same existing limit. */
      const cookies = trustedMapGet(map, "set-cookie");
      if (trustedMapSize(map) + (cookies ? cookies.length - 1 : 0) >=
          headerEntryLimit)
        throw new RangeError("Header entry limit exceeded");
    },
    headerChanged = (map) => {
      if (map[headerRevision] !== undefined) map[headerRevision]++;
    },
    headerIterator = (headers, mode) => {
      const map = headerMap(headers);
      if (map[headerRevision] === undefined) map[headerRevision] = 0;
      let revision = -1, entries;
      return (function* () {
        for (let index = 0; index < headerEntryLimit; index++) {
          if (revision !== map[headerRevision]) {
            entries = sortedHeaderEntries(headers);
            revision = map[headerRevision];
          }
          if (index >= entries.length) return;
          const pair = entries[index];
          yield mode < 0 ? pair : pair[mode];
        }
      })();
    };
  /* A no-cors Request's headers have Fetch's "request-no-cors" guard: they
     silently keep only no-CORS-safelisted request-headers (Accept,
     Accept-Language, Content-Language and a simple Content-Type), so a
     disallowed header is dropped and the request still goes out.
     js_fetch_cors.c refuses anything else as a backstop. */
  const noCorsHeaderMaps = new TrustedWeakMap(),
    corsUnsafeHeaderByte = /[\x00-\x08\x0a-\x1f"():<>?@[\\\]{}\x7f]/,
    noCorsSafelisted = (name, value) => {
      if (value.length > 128) return false;
      if (name === "accept") return !corsUnsafeHeaderByte.test(value);
      if (name === "accept-language" || name === "content-language")
        return /^[0-9A-Za-z *,\-.;=]*$/.test(value);
      if (name !== "content-type" || corsUnsafeHeaderByte.test(value))
        return false;
      const essence = value.split(";", 1)[0].trim().toLowerCase();
      return essence === "application/x-www-form-urlencoded" ||
        essence === "multipart/form-data" || essence === "text/plain";
    },
    noCorsHeaders = (source) => {
      const headers = new Headers();
      trustedWeakMapSet(noCorsHeaderMaps, headerMap(headers), true);
      for (const [key, value] of trustedMapEntries(headerMap(source)))
        if (key !== "set-cookie") setHeader(headers, key, value);
      return headers;
    };
  /* Fetch's append, which the constructor's fill uses too: never the
     page-replaceable Headers.prototype.append, which would otherwise see
     (and could drop) every transport header block. */
  const appendHeader = (headers, name, value) => {
    name = normalizeHeaderName(name);
    value = normalizeHeaderValue(value);
    const map = writableHeaderMap(headers),
      old = trustedMapGet(map, name);
    if (name === "set-cookie") {
      if (trustedWeakMapHas(noCorsHeaderMaps, map)) return;
      checkHeaderEntryLimit(map);
      if (old) old[old.length] = value;
      else trustedMapSet(map, name, [value]);
      headerChanged(map);
      return;
    }
    const combined = old === undefined ? value : old + ", " + value;
    if (trustedWeakMapHas(noCorsHeaderMaps, map)
        && !noCorsSafelisted(name, combined)) return;
    if (old === undefined) checkHeaderEntryLimit(map);
    trustedMapSet(map, name, combined);
    headerChanged(map);
  },
    /* Fetch's own set, get and has, for the same reason: the fetch and XHR
       pipeline never calls a page-replaceable Headers method. */
    setHeader = (headers, name, value) => {
      name = normalizeHeaderName(name);
      value = normalizeHeaderValue(value);
      const map = writableHeaderMap(headers);
      if (trustedWeakMapHas(noCorsHeaderMaps, map)
          && !noCorsSafelisted(name, value)) return;
      if (!trustedMapHas(map, name)) checkHeaderEntryLimit(map);
      trustedMapSet(map, name, name === "set-cookie" ? [value] : value);
      headerChanged(map);
    },
    getHeader = (headers, name) => {
      name = normalizeHeaderName(name);
      const value = trustedMapGet(headerMap(headers), name);
      return value === undefined ? null :
        name === "set-cookie" ? joinHeaderValues(value, ", ") : value;
    },
    hasHeader = (headers, name) =>
      trustedMapHas(headerMap(headers), normalizeHeaderName(name)),
    /* Sets `name` unless the headers have it (an inferred Content-Type). */
    defaultHeader = (headers, name, value) => {
      if (!hasHeader(headers, name)) setHeader(headers, name, value);
    };
  class Headers {
    constructor(init = {}) {
      const map = new Map();
      trustedWeakMapSet(headerStorage, this, map);
      if (init instanceof Headers) {
        for (const [key, value] of trustedMapEntries(headerMap(init)))
          trustedMapSet(map, key,
            key === "set-cookie" ? copyHeaderValues(value) : value);
      } else if (typeof init === "string") {
        for (const line of init.split("\n")) {
          if (!line) continue;
          const at = line.indexOf(":");
          if (at <= 0) throw new TypeError("Invalid HTTP header block");
          appendHeader(this, line.slice(0, at), line.slice(at + 1));
        }
      } else if (init !== null
                 && typeof init?.[Symbol.iterator] === "function") {
        let count = 0;
        for (const pair of init) {
          if (++count > headerEntryLimit)
            throw new RangeError("Header entry limit exceeded");
          if (pair === null || pair === undefined
              || typeof pair[Symbol.iterator] !== "function")
            throw new TypeError("Header pair must contain two values");
          const values = [];
          for (const value of pair) {
            if (values.length >= 2)
              throw new TypeError("Header pair must contain two values");
            values.push(value);
          }
          if (values.length !== 2)
            throw new TypeError("Header pair must contain two values");
          appendHeader(this, values[0], values[1]);
        }
      } else if (init && typeof init === "object") {
        const keys = Object.keys(init);
        if (keys.length > headerEntryLimit)
          throw new RangeError("Header entry limit exceeded");
        for (const key of keys) appendHeader(this, key, init[key]);
      }
    }
    append(name, value) {
      appendHeader(this, name, value);
    }
    set(name, value) {
      setHeader(this, name, value);
    }
    get(name) {
      return getHeader(this, name);
    }
    getSetCookie() {
      const values = trustedMapGet(headerMap(this), "set-cookie");
      return values ? copyHeaderValues(values) : [];
    }
    has(name) {
      return hasHeader(this, name);
    }
    delete(name) {
      name = normalizeHeaderName(name);
      const map = writableHeaderMap(this);
      if (trustedMapDelete(map, name)) headerChanged(map);
    }
    forEach(callback, thisArg) {
      if (typeof callback !== "function")
        throw new TrustedTypeError("Headers callback must be a function");
      for (const [key, value] of headerIterator(this, -1))
        trustedFunctionApply(callback, thisArg, [value, key, this]);
    }
    entries() {
      return headerIterator(this, -1);
    }
    keys() {
      return headerIterator(this, 0);
    }
    values() {
      return headerIterator(this, 1);
    }
    [Symbol.iterator]() {
      return this.entries();
    }
  }
  const inferredRequestBodyContentType = (source) => {
    if (source === null || source === undefined) return "";
    if (source instanceof URLSearchParams)
      return "application/x-www-form-urlencoded;charset=UTF-8";
    /* FormData chooses a fresh boundary while its entries are snapshotted so
       the delimiter can be checked against every retained part. */
    if (source instanceof FormData) return "";
    if (source instanceof Blob) return source.type || "";
    if (isArrayBuffer(source) || arrayBufferIsView(source)) return "";
    /* Fetch's BodyInit extraction converts every remaining admitted value to
       a USVString and supplies this MIME type when the caller did not provide
       one.  Keep the decision on the Request so headers reflect it before the
       body is consumed, and so Window, Worker, XHR and sendBeacon share the
       same wire behavior. */
    return "text/plain;charset=UTF-8";
  },
    ensureRequestBodyContentType = (headers, source) => {
      if (hasHeader(headers, "content-type")) return;
      const inferredType = inferredRequestBodyContentType(source);
      if (inferredType) setHeader(headers, "content-type", inferredType);
    },
    requestBodyByteLimit = 256 * 1024,
    requestReferrerPolicies = new Set([
      "",
      "no-referrer",
      "no-referrer-when-downgrade",
      "origin",
      "origin-when-cross-origin",
      "same-origin",
      "strict-origin",
      "strict-origin-when-cross-origin",
      "unsafe-url",
    ]),
    normalizeRequestReferrer = (value) => {
      value = String(value);
      if (value === "" || value === "about:client") return value;
      const parsed = new TrustedURL(value, location.href);
      if (parsed._origin !== location.origin) return "about:client";
      const href = parsed._href,
        fragment = href.indexOf("#");
      return fragment < 0 ? href : href.slice(0, fragment);
    },
    snapshotRequestBody = (source, headers, navigationForm = false) => {
      if (source === null || source === undefined) return null;
      const encoder = new TextEncoder(),
        admit = (bytes) => {
          if (bytes.byteLength > requestBodyByteLimit)
            throw new RangeError("Request body exceeds bounded size");
          return bytes;
        };
      if (source instanceof URLSearchParams) {
        ensureRequestBodyContentType(headers, source);
        return admit(encoder.encode(source.toString()));
      }
      if (source instanceof FormData)
        return domShared.formCollections().encodeMultipart(source, headers,
          navigationForm, requestBodyByteLimit, encoder, String, RangeError,
          Uint8Array, Uint8ArrayCtor, Blob, domShared.fileName, tilefinchBlobBytes,
          trustedCryptoRandomFill, defaultHeader);
      ensureRequestBodyContentType(headers, source);
      if (source instanceof Blob)
        return admit(tilefinchBlobBytes(source).slice());
      if (isArrayBuffer(source)) return admit(copyArrayBufferBytes(source));
      if (arrayBufferIsView(source))
        return admit(copyArrayBufferViewBytes(source));
      return admit(encoder.encode(String(source)));
    };
  domShared.multipartSubmission = (data) =>
    snapshotRequestBody(data, null, true).buffer;
  const requestBodyStorage = new TrustedWeakMap(),
    requestBodyState = (request) => {
      if (!trustedWeakMapHas(requestBodyStorage, request))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(requestBodyStorage, request);
    },
    bufferedRequestStream = (bytes) => new ReadableStream({
      start(controller) {
        controller.enqueue(bytes);
        /* The queue is the sole owner until it is drained or cancelled. */
        bytes = null;
        controller.close();
      },
    }),
    consumeRequestBody = async (request, signal = null, release = false) => {
      const state = requestBodyState(request);
      if (state.body === null) return new Uint8Array();
      return await __tilefinchConsumeReadableByteStream(
        state.body, requestBodyByteLimit, signal, release);
    },
    proxyRequestBody = (source) => {
      const reader = source.getReader();
      let released = false;
      const release = () => {
        if (released) return;
        released = true;
        reader.releaseLock();
      };
      return new ReadableStream({
        async pull(controller) {
          try {
            const result = await reader.read();
            if (result.done) {
              release();
              controller.close();
            } else controller.enqueue(result.value);
          } catch (error) {
            release();
            throw error;
          }
        },
        async cancel(reason) {
          try {
            return await reader.cancel(reason);
          } finally {
            release();
          }
        },
      });
    };
  /* Body consumers keep the existing byte ceiling, and reuse FormData's
     256-entry limit. A multipart part has an additional 8 KiB header cap;
     decoding and delimiter scanning never stringify the full binary body. */
  const bodyByteIndexOf = Function.call.bind(Uint8Array.prototype.indexOf),
    mimeEssence = (value) => value.split(";", 1)[0].trim().toLowerCase(),
    mimeParameter = (value, name) => {
      /* Skip quoted semicolons and unescape MIME quoted-pairs. First
         occurrence wins. All callers pass a bounded header value. */
      const parameter = /;\s*([^;=\s]+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"[ \t]*|([^;]*))(?=;|$)/g;
      for (let match; (match = parameter.exec(value)) !== null;) {
        if (match[1].toLowerCase() !== name) continue;
        if (match[2] !== undefined) return match[2].replace(/\\(.)/g, "$1");
        const token = match[3].trim();
        return isHttpToken(token) ? token : null;
      }
      return null;
    },
    parseMultipartBody = (bytes, boundary, decoder, data) => {
      const invalid = () => new TypeError("Invalid multipart form body"),
        /* Undo the encoder's escapes (Fetch's multipart/form-data parser). */
        unescapeName = (value) => value === null ? null : value.replace(/%(0A|0D|22)/g,
          (_, code) => code === "0A" ? "\n" : code === "0D" ? "\r" : '"');
      if (!boundary || boundary.length > 70 ||
          !/^[0-9a-zA-Z'()+_,.\/:=? -]*[0-9a-zA-Z'()+_,.\/:=?-]$/.test(boundary))
        throw invalid();
      const marker = new TextEncoder().encode("--" + boundary),
        size = trustedTypedArrayByteLength(bytes),
        buffer = trustedTypedArrayBuffer(bytes),
        baseOffset = trustedTypedArrayByteOffset(bytes),
        span = (start, end) =>
          new Uint8ArrayCtor(buffer, baseOffset + start, end - start),
        limitSteps = 256,
        delimiterAt = (offset) => {
          if (offset > size - marker.length || bytes[offset] !== 45)
            return null;
          for (let i = 1; i < marker.length; i++)
            if (bytes[offset + i] !== marker[i]) return null;
          let end = offset + marker.length;
          const closing = bytes[end] === 45 && bytes[end + 1] === 45;
          if (closing) end += 2;
          while (end < size && (bytes[end] === 32 || bytes[end] === 9)) end++;
          if (closing && end === size) return { at: offset, next: end, closing };
          if (bytes[end] !== 13 || bytes[end + 1] !== 10) return null;
          return { at: offset, next: end + 2, closing };
        },
        nextDelimiter = (start) => {
          for (let at = start; at < size;) {
            const end = bodyByteIndexOf(bytes, 13, at);
            if (end < 0) return null;
            if (bytes[end + 1] === 10) {
              const delimiter = delimiterAt(end + 2);
              if (delimiter) return delimiter;
            }
            at = end + 1;
          }
          return null;
        };
      let delimiter = delimiterAt(0) || nextDelimiter(0);
      if (!delimiter) throw invalid();
      for (let part = 0; !delimiter.closing && part <= limitSteps; part++) {
        if (part === limitSteps)
          throw new RangeError("FormData entry limit exceeded");
        const start = delimiter.next,
          headerEndLimit = Math.min(size - 3, start + 8192);
        let headerEnd = start;
        for (; headerEnd < headerEndLimit; headerEnd++)
          if (bytes[headerEnd] === 13 && bytes[headerEnd + 1] === 10 &&
              bytes[headerEnd + 2] === 13 && bytes[headerEnd + 3] === 10)
            break;
        if (headerEnd >= headerEndLimit) throw invalid();
        const headers = decoder.decode(span(start, headerEnd)).split("\r\n");
        let disposition = null, type = null;
        for (const line of headers) {
          const colon = line.indexOf(":");
          if (colon <= 0 || !isHttpToken(line.slice(0, colon))) throw invalid();
          const key = line.slice(0, colon).toLowerCase();
          if (key === "content-disposition" && disposition === null)
            disposition = line.slice(colon + 1).trim();
          if (key === "content-type" && type === null)
            type = line.slice(colon + 1).trim();
        }
        if (!disposition || mimeEssence(disposition) !== "form-data")
          throw invalid();
        const name = unescapeName(mimeParameter(disposition, "name")),
          filename = unescapeName(mimeParameter(disposition, "filename")),
          contentStart = headerEnd + 4;
        if (name === null) throw invalid();
        delimiter = nextDelimiter(contentStart);
        if (!delimiter) throw invalid();
        const content = span(contentStart, delimiter.at - 2);
        data.append(name, filename === null ? decoder.decode(content) :
          new File([content], filename, { type: type || "text/plain" }));
      }
      return data;
    },
    parseFormBody = (bytes, headers) => {
      const type = getHeader(headers, "content-type") || "";
      if (type.length > 8192) throw new TypeError("Form data MIME type exceeds bound");
      const essence = mimeEssence(type);
      if (essence !== "application/x-www-form-urlencoded" &&
          essence !== "multipart/form-data")
        throw new TypeError("Unsupported form data encoding");
      const data = new FormData(), decoder = new TextDecoder("utf-8", { ignoreBOM: true });
      if (essence === "multipart/form-data")
        return parseMultipartBody(bytes, mimeParameter(type, "boundary"), decoder, data);
      /* URLSearchParams skips a leading '?'; a body is not a URL query
         suffix and must preserve it as part of the field name. */
      const text = decoder.decode(bytes);
      for (const [name, value] of new URLSearchParams(text.startsWith("?") ? "&" + text : text))
        data.append(name, value);
      return data;
    };
  let Response;
  /* Chrome's wording for a URL that does not parse. */
  const requestURL = (input, operation) => {
    const text = String(input);
    try {
      return new URL(text, location.href).href;
    } catch (_) {
      throw new TypeError(
        `Failed to ${operation}: Failed to parse URL from ${text}`);
    }
  };
  class Request {
    constructor(input, init = {}) {
      init ??= {};
      const prior = input instanceof Request ? input : null;
      const initBody = init.body,
        inheritsBody = prior && (initBody === undefined || initBody === null),
        priorBody = prior ? requestBodyState(prior) : null;
      if (inheritsBody && (prior.bodyUsed || prior.body?.locked))
        throw new TypeError("Body has already been consumed");
      const url = prior ? prior.url : requestURL(input, "construct 'Request'");
      const method = String(
        init.method === undefined
          ? prior
            ? prior.method
            : "GET"
          : init.method,
      );
      if (!isHttpToken(method) || forbiddenMethod(method))
        throw new TypeError("Invalid HTTP method");
      const normalizedMethod = normalizeMethod(method),
        initHeaders = new Headers(
        init.headers === undefined
          ? prior
            ? prior.headers
            : {}
          : init.headers,
        ),
        mode = String(
          init.mode === undefined ? (prior ? prior.mode : "cors") : init.mode,
        ),
        credentials = String(
          init.credentials === undefined
            ? prior ? prior.credentials : "same-origin"
            : init.credentials,
        ),
        cache = String(
          init.cache === undefined ? (prior ? prior.cache : "default")
            : init.cache,
        ),
        redirect = String(
          init.redirect === undefined ? (prior ? prior.redirect : "follow")
            : init.redirect,
        ),
        referrer = normalizeRequestReferrer(
          init.referrer === undefined
            ? prior ? prior.referrer : "about:client"
            : init.referrer,
        ),
        referrerPolicy = String(
          init.referrerPolicy === undefined
            ? prior ? prior.referrerPolicy : ""
            : init.referrerPolicy,
        ),
        integrity = String(
          init.integrity === undefined ? (prior ? prior.integrity : "")
            : init.integrity,
        ),
        keepalive = init.keepalive === undefined
          ? prior ? prior.keepalive : false
          : !!init.keepalive,
        signal = init.signal === undefined
          ? (prior ? prior.signal : null)
          : init.signal;
      if (!["same-origin", "cors", "no-cors"].includes(mode))
        throw new TypeError("Invalid request mode");
      if (mode === "no-cors" && normalizedMethod !== "GET" &&
          normalizedMethod !== "HEAD" && normalizedMethod !== "POST")
        throw new TypeError("no-cors requests allow only GET, HEAD and POST");
      const headers =
        mode === "no-cors" ? noCorsHeaders(initHeaders) : initHeaders;
      if (!["omit", "same-origin", "include"].includes(credentials))
        throw new TypeError("Invalid credentials mode");
      if (![
        "default", "no-store", "reload", "no-cache", "force-cache",
        "only-if-cached",
      ].includes(cache))
        throw new TypeError("Invalid request cache mode");
      if (cache === "only-if-cached" && mode !== "same-origin")
        throw new TypeError("only-if-cached requires same-origin mode");
      if (!["follow", "error", "manual"].includes(redirect))
        throw new TypeError("Invalid redirect mode");
      if (!requestReferrerPolicies.has(referrerPolicy))
        throw new TypeError("Invalid referrer policy");
      if (signal !== null && !(signal instanceof AbortSignal))
        throw new TypeError("Request signal must be an AbortSignal");
      const bodySource = initBody === undefined ? null : initBody,
        streamSource = bodySource instanceof ReadableStream
          ? bodySource : null;
      if (
        streamSource &&
        (streamSource.locked || __tilefinchReadableStreamDisturbed(streamSource))
      )
        throw new TypeError("Request body stream is unusable");
      if (streamSource && init.duplex !== "half")
        throw new TypeError("Streaming Request body requires duplex: 'half'");
      const bodyPresent = inheritsBody
        ? priorBody.body !== null
        : bodySource !== null && bodySource !== undefined;
      if ((normalizedMethod === "GET" || normalizedMethod === "HEAD") && bodyPresent)
        throw new TypeError("GET and HEAD requests cannot have a body");
      const follower = signal ? followAbortSignal(signal) : null,
        requestSignal = follower
          ? follower.signal : new AbortSignal(abortSignalToken);
      try {
        let bodyBytesSnapshot = streamSource
          ? null
          : snapshotRequestBody(bodySource, headers),
          body = streamSource;
        if (inheritsBody) {
          bodyBytesSnapshot = priorBody.body === null
            ? null
            : __tilefinchConsumeBufferedReadableByteStream(
                priorBody.body, requestBodyByteLimit);
          body = priorBody.body === null
            ? null
            : bodyBytesSnapshot === null
              ? proxyRequestBody(priorBody.body)
              : null;
        }
        if (body === null && bodyBytesSnapshot !== null)
          body = bufferedRequestStream(bodyBytesSnapshot);
        const requestState = {
          buffered: bodyBytesSnapshot !== null,
          body,
          signal: requestSignal,
        };
        trustedWeakMapSet(requestBodyStorage, this, requestState);
        this.url = url;
        this.method = normalizedMethod;
        this.headers = headers;
        this.mode = mode;
        this.credentials = credentials;
        this.cache = cache;
        this.redirect = redirect;
        this.referrer = referrer;
        this.referrerPolicy = referrerPolicy;
        this.integrity = integrity;
        this.keepalive = keepalive;
        this.destination = "";
        this.duplex = "half";
      } catch (error) {
        follower?.dispose();
        throw error;
      }
    }
    get body() {
      return requestBodyState(this).body;
    }
    get bodyUsed() {
      return this.body !== null && __tilefinchReadableStreamDisturbed(this.body);
    }
    get signal() {
      return requestBodyState(this).signal;
    }
    arrayBuffer() {
      return consumeRequestBody(this).then((bytes) => bytes.buffer);
    }
    bytes() {
      return consumeRequestBody(this);
    }
    text() {
      return consumeRequestBody(this).then(
        (bytes) => new TextDecoder().decode(bytes));
    }
    json() {
      return this.text().then(JSON.parse);
    }
    blob() {
      return consumeRequestBody(this).then(
        (bytes) => new Blob([bytes], {
            type: getHeader(this.headers, "content-type") || "",
          }));
    }
    formData() {
      return consumeRequestBody(this).then((bytes) => parseFormBody(bytes, this.headers));
    }
    clone() {
      if (this.bodyUsed || this.body?.locked)
        throw new TypeError("Body has already been consumed");
      if (this.body === null) return new Request(this);
      const state = requestBodyState(this),
        branches = this.body.tee(true);
      state.body = branches[0];
      state.buffered = false;
      return new Request(this, { body: branches[1], duplex: "half" });
    }
  }
  globalThis.Headers = Headers;
  globalThis.Request = Request;
  {
    const cryptoToken = {},
      subtleToken = {},
      cryptoInstances = new WeakSet(),
      subtleInstances = new WeakSet(),
      /* Worker normalizers are realm functions: a rooted WeakMap value
         would keep the realm, its Crypto key and all globals alive. */
      subtleBufferNormalizers = domShared.createPrivateSlot(),
      cryptoSubtleInstances = domShared.createPrivateSlot(),
      /* The natives that hold a key's secret and run the primitives, for
         subtle-crypto.js. */
      cryptoKeyNative = globalThis.__tilefinchCryptoKey,
      cryptoRunNative = globalThis.__tilefinchCryptoRun,
      zeroCryptoBytes = Function.call.bind(Uint8Array.prototype.fill),
      integerRandomViews = new Set([
        "[object Int8Array]",
        "[object Uint8Array]",
        "[object Uint8ClampedArray]",
        "[object Int16Array]",
        "[object Uint16Array]",
        "[object Int32Array]",
        "[object Uint32Array]",
        "[object BigInt64Array]",
        "[object BigUint64Array]",
      ]);
    let ensureSubtleCryptoBootstrap =
        globalThis.__tilefinchEnsureSubtleCryptoBootstrap,
      /* subtle-crypto.js's factory, then the step it returns that adds the
         operations: each is kept until it completes. */
      createSubtleCrypto = null,
      installSubtleCrypto = null;
    delete globalThis.__tilefinchCryptoKey;
    delete globalThis.__tilefinchCryptoRun;
    /* Keys come only from subtle-crypto.js, which adds the attributes. */
    class CryptoKey {
      constructor() {
        throw new TypeError("Illegal constructor");
      }
    }
    Object.defineProperty(CryptoKey.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "CryptoKey",
    });
    class SubtleCrypto {
      constructor(token) {
        if (token !== subtleToken) throw new TypeError("Illegal constructor");
        subtleInstances.add(this);
      }
      digest(algorithm, data) {
        try {
          if (!subtleInstances.has(this))
            throw new TypeError("Illegal invocation");
          const name =
            typeof algorithm === "string"
              ? algorithm
              : algorithm && typeof algorithm === "object"
                ? algorithm.name
                : undefined;
          if (name === undefined)
            throw new TypeError("Algorithm name is required");
          if (typeof name === "symbol")
            throw new TypeError("Symbol is not a string");
          /* WebCrypto normalizes the name case-insensitively; the index is
             the native TilefinchWebDigest. Plain comparisons keep no table
             object resident and cannot be redirected by page script. */
          const upper = TrustedString(name).toUpperCase();
          const algorithmIndex = upper === "SHA-1" ? 0
            : upper === "SHA-256" ? 1
            : upper === "SHA-384" ? 2
            : upper === "SHA-512" ? 3 : -1;
          if (algorithmIndex < 0)
            throw new DOMException(
              "Unsupported digest algorithm",
              "NotSupportedError",
            );
          const normalizers = subtleBufferNormalizers.get(this);
          if (normalizers) data = normalizers.input(data);
          let source = data;
          if (data instanceof ArrayBuffer)
            source = new Uint8ArrayCtor(data);
          else if (arrayBufferIsView(data)) {
            if (
              typeof SharedArrayBuffer !== "undefined" &&
              data.buffer instanceof SharedArrayBuffer
            )
              throw new TypeError("Shared BufferSource is not supported");
            source = new Uint8ArrayCtor(
              data.buffer, data.byteOffset, data.byteLength);
          }
          /* Native admission handles a raw ArrayBuffer originating in another
             bounded same-origin Window realm. Worker-local raw buffers were
             normalized to a local view above before crossing realms. */
          /* subtle-crypto.js orders a scope's results and runs a digest
             over a slice's work in slices. */
          const order = domShared.orderCrypto,
            result = trustedPromiseResolve(order
              ? order(this, __tilefinchCryptoDigest(source, algorithmIndex,
                true))
              : __tilefinchCryptoDigest(source, algorithmIndex));
          return normalizers
            ? trustedPromiseThen(result, normalizers.output) : result;
        } catch (error) {
          return trustedPromiseReject(error);
        }
      }
    }
    Object.defineProperty(SubtleCrypto.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "SubtleCrypto",
    });
    class Crypto {
      constructor(token) {
        if (token !== cryptoToken) throw new TypeError("Illegal constructor");
        cryptoInstances.add(this);
      }
      get subtle() {
        if (!cryptoInstances.has(this))
          throw new TypeError("Illegal invocation");
        /* The first read adds SubtleCrypto's operations other than digest
           and CryptoKey's attributes (subtle-crypto.js), so a realm that
           never reads it holds none of them. A read that runs out of memory
           building or adding them leaves digest, and the next read goes on
           from the step that failed; a failed module load is not retried
           (the loader marks it failed). */
        if (ensureSubtleCryptoBootstrap) {
          try {
            /* The operations are added in the module's marking window, so
               they get the receiver and argument checks. The factory is
               read in the load's own call, before page code can run. */
            ensureSubtleCryptoBootstrap(() => {
              if (!installSubtleCrypto) {
                createSubtleCrypto ||=
                  __tilefinchRealmGlobal.__tilefinchCreateSubtleCrypto;
                delete __tilefinchRealmGlobal.__tilefinchCreateSubtleCrypto;
                installSubtleCrypto = createSubtleCrypto(
                  SubtleCrypto, CryptoKey, subtleInstances,
                  subtleBufferNormalizers, cryptoKeyNative, cryptoRunNative,
                  trustedCryptoRandomFill, isArrayBuffer, arrayBufferIsView,
                  copyArrayBufferBytes, copyArrayBufferViewBytes,
                  trustedTypedArrayByteLength, trustedTypedArrayBuffer,
                  zeroCryptoBytes,
                  trustedPromiseResolve, trustedPromiseThen,
                  trustedPromiseReject,
                  domShared.createPrivateSlot(), domShared,
                );
                createSubtleCrypto = null;
              }
              installSubtleCrypto();
              ensureSubtleCryptoBootstrap = installSubtleCrypto = null;
            });
          } catch (_) {}
        }
        return cryptoSubtleInstances.get(this);
      }
      getRandomValues(array) {
        if (!cryptoInstances.has(this))
          throw new TypeError("Illegal invocation");
        if (!ArrayBuffer.isView(array))
          throw new TypeError("parameter 1 is not of type 'ArrayBufferView'.");
        if (!integerRandomViews.has(Object.prototype.toString.call(array)))
          throw new DOMException(
            "An integer TypedArray is required",
            "TypeMismatchError",
          );
        if (array.byteLength > 65536)
          throw new DOMException(
            "The requested length exceeds 65,536 bytes",
            "QuotaExceededError",
          );
        return __tilefinchCryptoRandomFill(array);
      }
      randomUUID() {
        if (!cryptoInstances.has(this))
          throw new TypeError("Illegal invocation");
        const b = this.getRandomValues(new Uint8Array(16));
        b[6] = (b[6] & 15) | 64;
        b[8] = (b[8] & 63) | 128;
        const h = [...b].map((v) => v.toString(16).padStart(2, "0")).join("");
        return (
          h.slice(0, 8) +
          "-" +
          h.slice(8, 12) +
          "-" +
          h.slice(12, 16) +
          "-" +
          h.slice(16, 20) +
          "-" +
          h.slice(20)
        );
      }
    }
    Object.defineProperty(Crypto.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "Crypto",
    });
    const createCrypto = () => {
      const value = new Crypto(cryptoToken);
      const subtle = new SubtleCrypto(subtleToken);
      cryptoSubtleInstances.set(value, subtle);
      return value;
    };
    globalThis.Crypto = Crypto;
    globalThis.CryptoKey = CryptoKey;
    globalThis.SubtleCrypto = SubtleCrypto;
    const realmCrypto = createCrypto();
    globalThis.crypto = realmCrypto;
    /* A receiving realm may not have touched crypto.subtle yet. */
    domShared.ensureSubtleOperations =
      Object.getOwnPropertyDescriptor(Crypto.prototype, "subtle").get.bind(realmCrypto);
    /* Dedicated workers and frame windows get a distinct platform object.
       Sharing the owner object's mutable own properties would violate
       realm isolation, and its SubtleCrypto names the scope that owns a
       pending derivation. */
    globalThis.__tilefinchCreateWorkerCrypto = createCrypto;
    /* A worker or frame that goes away stops its derivations: rejected
       when the scope can still see them, otherwise dropped. */
    domShared.stopCryptoScope = (crypto, observable) =>
      domShared.stopCryptoJobs?.(cryptoSubtleInstances.get(crypto),
        observable);
    globalThis.__tilefinchSetCryptoBufferNormalizer = (
      crypto, input, output,
    ) => {
      const subtle = cryptoSubtleInstances.get(crypto);
      if (!subtle || typeof input !== "function" || typeof output !== "function")
        throw new TypeError("invalid Crypto normalizer");
      const current = subtleBufferNormalizers.get(subtle);
      if (current) {
        current.input = input;
        current.output = output;
      } else subtleBufferNormalizers.set(subtle, { input, output });
    };
  }
  if (globalThis.console === undefined) {
    /* Computed keys give every console method its own name. */
    const emit = (level) =>
        ({
          /* The values go untouched: only a console trace reads them
             (js_console_log), so a getter fires as with DevTools closed. */
          [level]: (...args) => __tilefinchConsoleLog(level, ...args),
        })[level],
      noop = (name) => ({ [name]: () => {} })[name];
    globalThis.console = {
      assert: noop("assert"),
      clear: noop("clear"),
      count: noop("count"),
      countReset: noop("countReset"),
      log: emit("log"),
      info: emit("info"),
      warn: emit("warn"),
      error: emit("error"),
      debug: emit("debug"),
      dir: noop("dir"),
      dirxml: noop("dirxml"),
      trace: emit("trace"),
      group: noop("group"),
      groupCollapsed: noop("groupCollapsed"),
      groupEnd: noop("groupEnd"),
      table: noop("table"),
      time: noop("time"),
      timeEnd: noop("timeEnd"),
      timeLog: noop("timeLog"),
    };
  }
  const nativeManagedHeaders = new Set([
    "accept",
    "accept-charset",
    "accept-encoding",
    "access-control-request-headers",
    "access-control-request-method",
    "access-control-request-private-network",
    "connection",
    "content-length",
    "content-type",
    "cookie",
    "cookie2",
    "date",
    "dnt",
    "expect",
    "host",
    "keep-alive",
    "origin",
    "permissions-policy",
    "referer",
    "set-cookie",
    "set-cookie2",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "upgrade-insecure-requests",
    "user-agent",
    "via",
    "ua",
    "ua-mobile",
    "ua-platform",
    "ua-full-version",
    "ua-full-version-list",
    "ua-arch",
    "ua-bitness",
    "ua-model",
    "ua-platform-version",
    "x-http-method",
    "x-http-method-override",
    "x-method-override",
  ]);
  const nativeForbiddenHeader = (name) =>
    name.startsWith("sec-") ||
    name.startsWith("proxy-") ||
    nativeManagedHeaders.has(name);
  const nativeHeaderBlock = (headers) => {
    const lines = [];
    for (let [name, value] of sortedHeaderEntries(headers)) {
      name = String(name).toLowerCase();
      value = String(value);
      if (!nativeForbiddenHeader(name)) lines.push(name + ": " + value);
    }
    return lines.join("\n");
  };
  const headersFromTransport = (raw) => {
    const headers = new Headers(raw.headers ||
      "content-type: " + raw.contentType + "\n");
    /* Native strips these fields before publishing a response header block.
       Keep that boundary for every transport result, including replay. */
    const map = headerMap(headers);
    trustedMapDelete(map, "set-cookie");
    trustedMapDelete(map, "set-cookie2");
    return headers;
  };
  const serializeRequestBody = (request) => {
    if (request instanceof Request) {
      const state = requestBodyState(request);
      if (state.body === null) return undefined;
      const bytes = __tilefinchConsumeBufferedReadableByteStream(
        state.body, requestBodyByteLimit);
      if (bytes === null)
        throw new TypeError("Streaming body requires asynchronous consumption");
      return bytes === null
        ? undefined
        : bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
          ? bytes.buffer
          : bytes.slice().buffer;
    }
    /* XHR and sendBeacon share this serializer through a small internal
       request-shaped record. Snapshot those one-shot sources here; public
       Request objects were already snapshotted by their constructor. */
    const bytes = snapshotRequestBody(request._bodySource, request.headers);
    return bytes === null
      ? undefined
      : bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
        ? bytes.buffer
        : bytes.slice().buffer;
  },
    serializeRequestBodyAsync = async (request) => {
      if (request.body === null) return undefined;
      const bytes = await consumeRequestBody(request, request.signal, true);
      return bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
        ? bytes.buffer
        : bytes.slice().buffer;
    };
  const networkQueueLimit = 128,
    /* Mirror FETCH_REQUEST_BODY_LIMIT. A Request may be inspected locally up
       to the broader Body mixin bound, but a network operation must reject
       before retaining bytes the native scheduler cannot publish. */
    networkQueueByteLimit = 96 * 1024,
    nativeNetworkLimit = 4;
  let nextNetworkId = 1,
    activeNetwork = 0,
    preflightPollTimer = 0,
    preflightDecisions = 0;
  const pendingNetwork = new Map(),
    nativeNetwork = new Map(),
    waitingNetwork = [],
    /* Requests waiting for their CORS preflight (the -2 deferral), in
       arrival order. They hold no ordering claim over the requests behind
       them, which start meanwhile. */
    parkedNetwork = [],
    networkQueueStats = {
      admitted: 0,
      launched: 0,
      completed: 0,
      cancelled: 0,
      timedOut: 0,
      rejected: 0,
      rejectedBytes: 0,
      launchFailed: 0,
      poolDeferred: 0,
      localBlobReads: 0,
      localBlobFailures: 0,
      currentCount: 0,
      peakCount: 0,
      currentBytes: 0,
      peakBytes: 0,
      waiting: 0,
      active: 0,
    },
    networkQueueView = testProbes ? {} : null;
  /* A read-only view for host tests. */
  if (networkQueueView) {
    for (const key of Object.keys(networkQueueStats))
      Object.defineProperty(networkQueueView, key, {
        enumerable: true,
        get: () => networkQueueStats[key],
      });
    Object.freeze(networkQueueView);
    Object.defineProperty(globalThis, "__tilefinchNetworkQueueStats", {
      value: networkQueueView,
      writable: false,
      configurable: false,
    });
  }
  /* The native result snapshot and pending-work count read the backing
     record directly: plain data properties, no getter call per turn. */
  globalThis.__tilefinchHostChannel.networkQueueStats = networkQueueStats;
  const networkRetainedBytes = (...values) => {
    let total = 128;
    for (const value of values) {
      if (value === undefined || value === null) continue;
      if (typeof value === "string") total += value.length * 2;
      else if (value instanceof ArrayBuffer) total += value.byteLength;
      else if (ArrayBuffer.isView(value)) total += value.byteLength;
      else total += String(value).length * 2;
      if (total > networkQueueByteLimit) return total;
    }
    return total;
  };
  /* Whether the queue can admit one more request retaining `bytes`, as
     networkRetainedBytes counts them. */
  const networkQueueAdmits = (bytes) =>
    networkQueueStats.currentCount < networkQueueLimit &&
    bytes <= networkQueueByteLimit - networkQueueStats.currentBytes;
  const releaseNetwork = (entry) => {
    if (!entry || !pendingNetwork.has(entry.id)) return false;
    pendingNetwork.delete(entry.id);
    if (entry.state === "waiting" || entry.state === "parked") {
      const queue = entry.state === "parked" ? parkedNetwork : waitingNetwork;
      const at = queue.indexOf(entry);
      if (at >= 0) queue.splice(at, 1);
      networkQueueStats.waiting = Math.max(0, networkQueueStats.waiting - 1);
    } else if (entry.state === "active") {
      nativeNetwork.delete(entry.nativeId);
      activeNetwork = Math.max(0, activeNetwork - 1);
      networkQueueStats.active = activeNetwork;
    }
    networkQueueStats.currentCount = Math.max(
      0,
      networkQueueStats.currentCount - 1,
    );
    networkQueueStats.currentBytes = Math.max(
      0,
      networkQueueStats.currentBytes - entry.bytes,
    );
    entry.state = "done";
    entry.start = null;
    return true;
  };
  /* Resubmits parked requests, ahead of the FIFO and in their order, once a
     preflight of this realm has been decided. The probe is one native
     call that reads a counter; it does not run request admission. */
  const pollPreflights = () => {
    preflightPollTimer = 0;
    if (!parkedNetwork.length) return;
    const decisions = Number(__tilefinchCorsPreflightDecisions());
    if (decisions !== preflightDecisions) {
      preflightDecisions = decisions;
      const resumed = parkedNetwork.splice(0, parkedNetwork.length);
      for (let i = resumed.length - 1; i >= 0; i--) {
        resumed[i].state = "waiting";
        waitingNetwork.unshift(resumed[i]);
      }
      pumpNetworkQueue();
    }
    if (parkedNetwork.length && !preflightPollTimer)
      preflightPollTimer = __tilefinchRealmGlobal.__tilefinchScheduleTask(
        pollPreflights, 10);
  };
  const pumpNetworkQueue = () => {
    while (activeNetwork < nativeNetworkLimit && waitingNetwork.length) {
      const entry = waitingNetwork.shift();
      if (!entry || entry.state !== "waiting") continue;
      networkQueueStats.waiting = Math.max(0, networkQueueStats.waiting - 1);
      if (entry.nativeId) domShared.cancelTimer(entry.nativeId);
      entry.state = "launching";
      try {
        const nativeId = Number(entry.start());
        if (nativeId === -2) {
          /* Native policy admitted this request but its preflight is not
             decided: park it and go on with the requests behind it. */
          if (!parkedNetwork.length && !preflightPollTimer)
            preflightDecisions = Number(__tilefinchCorsPreflightDecisions());
          entry.state = "parked";
          entry.nativeId = 0;
          parkedNetwork.push(entry);
          networkQueueStats.waiting++;
          if (!preflightPollTimer)
            preflightPollTimer = __tilefinchRealmGlobal.__tilefinchScheduleTask(
              pollPreflights, 10);
          continue;
        }
        if (nativeId === 0 || nativeId === -1) {
          /* Native policy has already admitted this immutable request.  Zero
             means the shared page scheduler is temporarily full; -1 means
             this realm's own requests fill its fetch pool or native request
             table.  Put the same entry back at the head so neither a later
             mutation nor a later request can overtake it.  A shared
             scheduler frees capacity without telling this realm, so retry
             from a future timer turn instead of spinning the single browser
             thread.  This realm's own completions and cancellations already
             pump the queue, so a full pool keeps only a slow fallback
             timer: polling it every 10 ms re-ran the whole native admission
             path hundreds of times while one long-lived request stayed
             open. */
          entry.state = "waiting";
          waitingNetwork.unshift(entry);
          networkQueueStats.waiting++;
          if (nativeId === -1) networkQueueStats.poolDeferred++;
          entry.nativeId = __tilefinchRealmGlobal.__tilefinchScheduleTask(
            pumpNetworkQueue, nativeId === -1 ? 250 : 10);
          break;
        }
        if (!Number.isSafeInteger(nativeId) || nativeId < 0)
          throw new RangeError("native network queue rejected request");
        entry.nativeId = nativeId;
        entry.state = "active";
        activeNetwork++;
        networkQueueStats.active = activeNetwork;
        networkQueueStats.launched++;
        nativeNetwork.set(nativeId, entry);
      } catch (error) {
        networkQueueStats.launchFailed++;
        const reject = entry.reject;
        releaseNetwork(entry);
        entry.resolve = null;
        entry.reject = null;
        reject(error);
      }
    }
  };
  const queueNetwork = (
    start,
    bytes,
    resolve,
    reject,
    timingName = "",
    timingInitiator = "",
    timingRecorder = __tilefinchRecordResourceTiming,
  ) => {
    bytes = Math.max(128, Math.floor(Number(bytes) || 128));
    if (!networkQueueAdmits(bytes)) {
      networkQueueStats.rejected++;
      if (bytes > networkQueueByteLimit - networkQueueStats.currentBytes)
        networkQueueStats.rejectedBytes++;
      /* Concurrency only queues; a full queue is a network error. */
      throw new TypeError("network pending queue quota exceeded");
    }
    let id = nextNetworkId++;
    if (id > Number.MAX_SAFE_INTEGER) {
      nextNetworkId = 1;
      id = nextNetworkId++;
    }
    const entry = {
      id,
      nativeId: 0,
      start,
      resolve,
      reject,
      timingName,
      timingInitiator,
      timingRecorder,
      bytes,
      state: "waiting",
    };
    pendingNetwork.set(id, entry);
    waitingNetwork.push(entry);
    networkQueueStats.admitted++;
    networkQueueStats.currentCount++;
    networkQueueStats.currentBytes += bytes;
    networkQueueStats.waiting++;
    networkQueueStats.peakCount = Math.max(
      networkQueueStats.peakCount,
      networkQueueStats.currentCount,
    );
    networkQueueStats.peakBytes = Math.max(
      networkQueueStats.peakBytes,
      networkQueueStats.currentBytes,
    );
    pumpNetworkQueue();
    return id;
  };
  const cancelNetwork = (id, reason, notify = true) => {
    const entry = pendingNetwork.get(Number(id));
    if (!entry) return false;
    const reject = entry.reject,
      nativeId = entry.nativeId,
      wasActive = entry.state === "active",
      timedOut =
        String(
          (reason && reason.name) || (reason && reason.message) || reason || "",
        )
          .toLowerCase()
          .includes("timeout") ||
        String((reason && reason.message) || reason || "")
          .toLowerCase()
          .includes("timed out");
    releaseNetwork(entry);
    entry.resolve = null;
    entry.reject = null;
    if (wasActive)
      __tilefinchCancelNetwork(
        nativeId,
        String((reason && reason.message) || reason || "request aborted"),
      );
    networkQueueStats.cancelled++;
    if (timedOut) networkQueueStats.timedOut++;
    if (notify) reject(reason);
    queueMicrotask(pumpNetworkQueue);
    return true;
  };
  globalThis.__tilefinchDeliverNetwork = (
    id,
    ok,
    value,
    timingAvailable = false,
    nameLookupUs = 0,
    connectUs = 0,
    appconnectUs = 0,
    firstByteUs = 0,
    totalUs = 0,
    encodedBodyBytes = 0,
    decodedBodyBytes = 0,
    measured = false,
    cacheHit = false,
    cacheValidated = false,
    timingAllowed = false,
    nextHopProtocol = 0,
    responseStatus = 0,
    contentType = "",
  ) => {
    const entry = nativeNetwork.get(Number(id));
    if (!entry) {
      queueMicrotask(pumpNetworkQueue);
      return true;
    }
    const resolve = entry.resolve,
      reject = entry.reject;
    releaseNetwork(entry);
    entry.resolve = null;
    entry.reject = null;
    networkQueueStats.completed++;
    if (!ok && /timeout|timed out/i.test(String(value || "")))
      networkQueueStats.timedOut++;
    if (
      timingAvailable && entry.timingName &&
      typeof entry.timingRecorder === "function"
    ) {
      entry.timingRecorder(
        entry.timingName,
        entry.timingInitiator,
        nameLookupUs,
        connectUs,
        appconnectUs,
        firstByteUs,
        totalUs,
        encodedBodyBytes,
        decodedBodyBytes,
        measured,
        cacheHit,
        cacheValidated,
        timingAllowed,
        nextHopProtocol,
        responseStatus,
        contentType,
      );
    }
    if (ok) resolve(value);
    else reject(new TypeError(String(value || "network request failed")));
    queueMicrotask(pumpNetworkQueue);
    return true;
  };
  const snapshotLocalBlobRequest = (method, inputURL) => {
    const url = new URL(inputURL, location.href).href;
    if (!url.startsWith("blob:")) return null;
    if (method !== "GET" && method !== "HEAD") {
      networkQueueStats.localBlobFailures++;
      throw new TypeError("Blob URLs support only GET and HEAD");
    }
    const blob = tilefinchBlobForURL(url);
    if (!blob) {
      networkQueueStats.localBlobFailures++;
      throw new TypeError("Blob URL is no longer available");
    }
    const bytes = tilefinchBlobBytes(blob),
      type = tilefinchBlobType.call(blob);
    networkQueueStats.localBlobReads++;
    return {
      bodyBytes: method === "HEAD" ? new ArrayBuffer() : bytes.buffer,
      bodyLength: method === "HEAD" ? 0 : bytes.byteLength,
      contentType: type,
      headers:
        "content-length: " + String(bytes.byteLength) + "\n" +
        (type ? "content-type: " + type + "\n" : ""),
      status: 200,
      url,
    };
  };
  /* The lab's per-request telemetry globals (test-probe realms only). */
  if (testProbes) {
    globalThis.__tilefinchXHRSendCalls = 0;
    globalThis.__tilefinchXHRLastError = "";
  }
  const responseBrands = new WeakSet(),
    responseMetadata = new WeakMap(),
    responseBodyStorage = new TrustedWeakMap(),
    responseOwnedBytesToken = {},
    immutableResponseToken = {},
    immutableResponse = (body, init) =>
      new Response(body, init, immutableResponseToken),
    responseByteStream = (holder) => new ReadableStream({
      pull(controller) {
        const bytes = holder.bytes;
        if (bytes === null || holder.at >= bytes.length) {
          holder.bytes = null;
          controller.close();
          return;
        }
        const end = Math.min(bytes.length, holder.at + 4096);
        controller.enqueue(bytes.slice(holder.at, end));
        holder.at = end;
      },
      cancel() { holder.bytes = null; },
    }),
    responseBodyState = (response) => {
      if (!trustedWeakMapHas(responseBodyStorage, response))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(responseBodyStorage, response);
    };
  Response = class Response {
    constructor(body = null, init = {}, token = null) {
      init ??= {};
      const immutable = token === immutableResponseToken;
      const status = Number(init.status === undefined ? 200 : init.status);
      if (
        !(immutable && status === 0) &&
        (!Number.isInteger(status) || status < 200 || status > 599)
      )
        throw new RangeError("Invalid response status");
      if (body !== null && body !== undefined &&
          [101, 204, 205, 304].includes(status))
        throw new TypeError("Response status cannot have a body");
      const statusText = String(init.statusText === undefined ? "" : init.statusText);
      if (statusText && /[^\t\x20-\x7e\x80-\xff]/.test(statusText))
        throw new TypeError("Invalid response status text");
      const headers = new Headers(init.headers);
      const streamBody = body instanceof ReadableStream ? body : null;
      if (
        streamBody &&
        (streamBody.locked || __tilefinchReadableStreamDisturbed(streamBody))
      )
        throw new TypeError("Response body stream is unusable");
      const supplied = init.bodyBytes,
        suppliedBodyBytes = supplied !== undefined;
      const trustedOwnedBytes = init.__tilefinchOwnedBytesToken ===
        responseOwnedBytesToken;
      let bytes = suppliedBodyBytes
        ? trustedOwnedBytes && isArrayBuffer(supplied)
          ? new Uint8ArrayCtor(supplied)
          : isArrayBuffer(supplied)
          ? copyArrayBufferBytes(supplied)
          : arrayBufferIsView(supplied)
            ? copyArrayBufferViewBytes(supplied)
            : null
        : streamBody ? null : snapshotRequestBody(body, headers);
      if (suppliedBodyBytes && bytes === null)
        throw new TypeError("Response bodyBytes must be a BufferSource");
      if (immutable) trustedWeakSetAdd(immutableHeaderMaps, headerMap(headers));
      const bodyState = { body: null, buffered: null, immutable };
      trustedWeakMapSet(responseBodyStorage, this, bodyState);
      this.status = status;
      this.statusText = statusText;
      this.url = String(init.url === undefined ? "" : init.url);
      this.headers = headers;
      trustedWeakSetAdd(responseBrands, this);
      trustedWeakMapSet(responseMetadata, this, {
        headers: this.headers,
        status: this.status,
      });
      this.ok = this.status >= 200 && this.status < 300;
      this.redirected = !!init.redirected;
      this.type = String(init.type || "default");
      /* The stream owns this immutable byte snapshot.  Metadata may release
       * its reference after tee()/consumption, but that must never empty a
       * pull which has not run yet. */
      if ((body === null || body === undefined) && !suppliedBodyBytes) {
        bodyState.body = null;
      } else if (streamBody) {
        bodyState.body = streamBody;
      } else {
        bodyState.buffered = { at: 0, bytes };
        bodyState.body = responseByteStream(bodyState.buffered);
      }
    }
    get body() {
      return responseBodyState(this).body;
    }
    get bodyUsed() {
      return this.body !== null && __tilefinchReadableStreamDisturbed(this.body);
    }
  };
  globalThis.Response = Response;
  {
    const takeBytes = async (response) => {
        const state = responseBodyState(response);
        if (state.body === null) return new Uint8Array();
        if (state.buffered && state.buffered.bytes !== null) {
          if (state.body.locked ||
              __tilefinchReadableStreamDisturbed(state.body))
            throw new TypeError("Body has already been consumed");
          const bytes = state.buffered.bytes;
          if (bytes.byteLength > 256 * 1024)
            throw new RangeError("Body exceeds bounded size");
          /* Match the ordinary consumer's persistent reader lock without
             paying its promise/chunk/copy path. The shared holder also lets
             a public reader or cancellation release the retained body. */
          const reader = state.body.getReader();
          __tilefinchMarkReadableStreamDisturbed(state.body);
          state.buffered.bytes = null;
          state.buffered = null;
          /* The stream may have prefetched one bounded public chunk. Cancel
             it so that copy is released and the internal close settles. */
          await reader.cancel();
          return bytes;
        }
        const bytes = await __tilefinchConsumeReadableByteStream(
          state.body, 256 * 1024);
        return bytes;
      };
    Response.prototype.text = function text() {
      return takeBytes(this).then((bytes) => new TextDecoder().decode(bytes));
    };
    Response.prototype.json = function json() {
      return this.text().then(JSON.parse);
    };
    Response.prototype.arrayBuffer = function arrayBuffer() {
      return takeBytes(this).then((bytes) => bytes.buffer);
    };
    Response.prototype.bytes = function bytes() {
      return this.arrayBuffer().then((buffer) => new Uint8Array(buffer));
    };
    Response.prototype.blob = function blob() {
      return takeBytes(this).then(
        (bytes) => new Blob([bytes], {
          type: getHeader(this.headers, "content-type") || "",
        }));
    };
    Response.prototype.formData = function formData() {
      return takeBytes(this).then((bytes) => parseFormBody(bytes, this.headers));
    };
    Response.prototype.clone = function clone() {
      if (this.bodyUsed || this.body?.locked)
        throw new TypeError("Body has already been consumed");
      const init = {
        status: this.status,
        statusText: this.statusText,
        url: this.url,
        headers: this.headers,
        redirected: this.redirected,
        type: this.type,
      };
      const state = responseBodyState(this);
      const token = state.immutable ? immutableResponseToken : null;
      if (this.body === null) return new Response(null, init, token);
      const [first, second] = this.body.tee(true);
      state.body = first;
      state.buffered = null;
      return new Response(second, init, token);
    };
    Response.error = () =>
      immutableResponse(null, { status: 0, statusText: "", type: "error" });
    Response.redirect = (url, status = 302) => {
      status = Number(status);
      if (![301, 302, 303, 307, 308].includes(status))
        throw new RangeError("Invalid redirect status");
      return immutableResponse(null, {
        status,
        headers: { location: new URL(String(url), location.href).href },
      });
    };
    Response.json = (data, init = {}) => {
      init ??= {};
      const serialized = JSON.stringify(data);
      if (serialized === undefined)
        throw new TypeError("Response data cannot be serialized as JSON");
      const headers = new Headers(init.headers);
      defaultHeader(headers, "content-type", "application/json");
      return new Response(serialized, { ...init, headers });
    };
    const trustedResponseArrayBuffer = Response.prototype.arrayBuffer,
      trustedHeadersGet = Headers.prototype.get;
    trustedDefineProperty(globalThis, "__tilefinchConsumeWasmResponse", {
      configurable: false,
      enumerable: false,
      writable: false,
      value(response) {
        if (!trustedWeakSetHas(responseBrands, response))
          return Promise.reject(
            new TypeError("WebAssembly streaming source must be a Response"));
        const metadata = trustedWeakMapGet(responseMetadata, response),
          contentType = trustedFunctionApply(
            trustedHeadersGet, metadata.headers, ["content-type"]),
          essence = typeof contentType === "string"
            ? contentType.split(";", 1)[0].trim().toLowerCase() : "";
        if (essence !== "application/wasm")
          return Promise.reject(new TypeError(
            "WebAssembly response has an unsupported MIME type"));
        if (metadata.status < 200 || metadata.status >= 300)
          return Promise.reject(new TypeError(
            "WebAssembly response is not successful"));
        return trustedFunctionApply(trustedResponseArrayBuffer, response, []);
      },
    });
  }
  const fetchInternal = (
    input,
    init = {},
    timingRecorder = __tilefinchRecordResourceTiming,
    workerScript = false,
  ) => {
    try {
      /* The URL is parsed first, as the Request constructor's first step,
         and a URL that does not parse never reaches the network. */
      if (!(input instanceof Request))
        input = requestURL(input, "execute 'fetch' on '"
          + (timingRecorder ? "Window" : "WorkerGlobalScope") + "'");
      const request = new Request(input, init);
      request.signal.throwIfAborted();
      if (
        request.body !== null &&
        !requestBodyState(request).buffered
      )
        return serializeRequestBodyAsync(request).then((bytes) =>
          fetchInternal(request, {
            body: new Uint8Array(bytes),
            duplex: "half",
          }));
      const body = serializeRequestBody(request),
        contentType = getHeader(request.headers, "content-type") || "",
        accept = getHeader(request.headers, "accept") || "*/*",
        authorHeaderBlock = nativeHeaderBlock(request.headers),
        headerBlock = authorHeaderBlock,
        signal = request.signal,
        method = request.method,
        url = request.url,
        mode = request.mode,
        credentials = request.credentials;
      let local;
      try {
        local = snapshotLocalBlobRequest(method, url);
      } catch (error) {
        return Promise.reject(error);
      }
      if (local) {
        if (request.integrity) {
          const integrity = __tilefinchVerifyResourceIntegrity(
            request.integrity, local.bodyBytes);
          if (integrity === 2 || integrity === 3)
            return Promise.reject(new TypeError(
              integrity === 2
                ? "Response integrity mismatch"
                : "Invalid response integrity metadata"));
        }
        return new Promise((resolve, reject) => {
          let settled = false;
          const abort = () => {
            if (settled) return;
            settled = true;
            reject(
              signal.reason === undefined ? defaultAbortReason() : signal.reason,
            );
          };
          addAbortAlgorithm(signal, abort);
          queueMicrotask(() => {
            if (settled) return;
            settled = true;
            removeAbortAlgorithm(signal, abort);
            if (signal.aborted) {
              reject(
                signal.reason === undefined
                  ? defaultAbortReason() : signal.reason,
              );
              return;
            }
            resolve(
              immutableResponse(method === "HEAD" ? null : local.bodyBytes, {
                status: local.status,
                url: local.url,
                headers: new Headers(local.headers),
                bodyBytes: method === "HEAD" ? undefined : local.bodyBytes,
              }),
            );
          });
        });
      }
      /* Tilefinch intentionally has no page-owned HTTP cache. Fetch requires
         only-if-cached to avoid the network and return a 504 on a cache miss;
         every request is therefore a deterministic miss. */
      if (request.cache === "only-if-cached")
        return Promise.resolve(immutableResponse(null, {
          status: 504,
          statusText: "Gateway Timeout",
          url,
          type: "basic",
        }));
      return new Promise((resolve, reject) => {
        let id = 0;
        const abort = () =>
          cancelNetwork(
            id,
            signal.reason === undefined ? defaultAbortReason() : signal.reason,
          );
        id = queueNetwork(
          () =>
            __tilefinchFetchAsync(
              method,
              url,
              body,
              contentType,
              headerBlock,
              mode,
              credentials,
              undefined,
              workerScript,
              request.referrer,
              request.referrerPolicy,
              request.redirect,
              request.integrity,
              accept,
              request.cache,
            ),
          networkRetainedBytes(
            method,
            url,
            body,
            contentType,
            headerBlock,
            mode,
            credentials,
          ),
          (raw) => {
            removeAbortAlgorithm(signal, abort);
            try {
              signal.throwIfAborted();
              const responseStatus = Number(raw.status),
                nullBody = method === "HEAD" ||
                  raw.type === "opaque" || raw.type === "opaqueredirect" ||
                  [101, 204, 205, 304].includes(responseStatus);
              resolve(
                immutableResponse(nullBody ? null : raw.body, {
                  status: responseStatus,
                  url: raw.url,
                  headers:
                    raw.type === "opaqueredirect" || raw.type === "opaque"
                      ? new Headers()
                      : headersFromTransport(raw),
                  bodyBytes: nullBody ? undefined : raw.bodyBytes,
                  __tilefinchOwnedBytesToken: responseOwnedBytesToken,
                  redirected: !!raw.redirected,
                  type: raw.type || "basic",
                }),
              );
            } catch (error) {
              reject(error);
            }
          },
          (error) => {
            removeAbortAlgorithm(signal, abort);
            /* Fetch deliberately does not expose transport diagnostics to
               author code.  Native keeps the detailed curl/TLS/DNS failure
               for Tilefinch's diagnostics, while the web-visible rejection
               matches the Fetch network-error surface used by browsers. */
            reject(
              error instanceof TypeError
                ? new TypeError("Failed to fetch")
                : error,
            );
          },
          url,
          "fetch",
          timingRecorder,
        );
        if (pendingNetwork.has(id)) {
          addAbortAlgorithm(signal, abort);
          if (signal.aborted) abort();
        }
      });
    } catch (error) {
      return Promise.reject(error);
    }
  };
  const fetch = (input, init = {}) =>
    fetchInternal(input, init, __tilefinchRecordResourceTiming);
  globalThis.fetch = fetch;
  /* Dedicated workers own a separate performance timeline.  Their wrapper
     records the filtered Response surface in that timeline, so suppress the
     Window entry here without exposing native phase timings to a callback
     supplied by author code. */
  globalThis.__tilefinchFetchForWorker = (input, init = {}) =>
    fetchInternal(input, init, null);
  globalThis.__tilefinchFetchWorkerScript = (input, init = {}) =>
    fetchInternal(input, init, null, true);
  globalThis.__tilefinchFetchWorkerScriptSync = (
    url, mode = "cors", credentials = "same-origin",
  ) => {
    /* A transport or policy failure is the script's NetworkError, as an
       HTTP error status is. */
    let raw = null;
    try {
      raw = __tilefinchFetchSync(
        "GET", String(url), undefined, "", "", mode, credentials, 2,
        undefined, "about:client", "");
    } catch (_) {}
    if (!raw || Number(raw.status) < 200 || Number(raw.status) >= 300)
      throw new DOMException("Worker script could not be loaded", "NetworkError");
    if (typeof raw.body === "string") return raw.body;
    if (raw.bodyBytes instanceof ArrayBuffer)
      return new TextDecoder().decode(raw.bodyBytes);
    return "";
  };
  Object.defineProperty(Navigator.prototype, "sendBeacon", {
    configurable: true,
    enumerable: true,
    writable: true,
    value: function sendBeacon(url, data = null) {
      if (!navigatorBrand(this))
        throw new TypeError("Illegal invocation");
      try {
        const target = new URL(String(url), location.href);
        if (target.protocol !== "http:" && target.protocol !== "https:")
          return false;
        /* Extract the body once, before any Request exists: measuring a
           Request's body consumes it, and the fetch() of that Request then
           rejected, so no beacon carrying data was ever sent. The same
           extraction fixes the Content-Type (text/plain, a Blob's type,
           multipart with boundary, urlencoded). */
        const headers = new Headers(),
          body =
            data === null || data === undefined
              ? null
              : snapshotRequestBody(data, headers);
        /* Admit exactly as the fetch below will be queued, URL and headers
           included: true must mean the beacon was queued. */
        if (
          (body !== null && body.byteLength > 64 * 1024) ||
          !networkQueueAdmits(
            networkRetainedBytes(
              "POST",
              target.href,
              body,
              getHeader(headers, "content-type") || "",
              nativeHeaderBlock(headers),
              "cors",
              "include",
            ),
          )
        )
          return false;
        fetch(target.href, {
          method: "POST",
          body,
          headers,
          credentials: "include",
          keepalive: true,
        }).catch(() => {});
        return true;
      } catch (_) {
        return false;
      }
    },
  });
  {
    const webSockets = new Map(),
      webSocketLimit = 2,
      webSocketByteLimit = 64 * 1024,
      webSocketQueueLimit = 16,
      webSocketDispatch = EventTarget.prototype.dispatchEvent;
    class WebSocket extends EventTarget {
      constructor(url, protocols = []) {
        super();
        if (arguments.length < 1)
          throw new TypeError("WebSocket requires a URL");
        const raw = String(url),
          mapped = raw.replace(/^ws:/i, "http:").replace(/^wss:/i, "https:"),
          parsed = new URL(mapped, location.href),
          requestedWebSocketScheme = /^wss?:/i.test(raw);
        if (
          (!requestedWebSocketScheme &&
            parsed.protocol !== "http:" &&
            parsed.protocol !== "https:") ||
          (requestedWebSocketScheme &&
            parsed.protocol !== "http:" &&
            parsed.protocol !== "https:")
        )
          throw new DOMException("Invalid WebSocket scheme", "SyntaxError");
        if (parsed.hash)
          throw new DOMException("WebSocket URLs cannot have fragments", "SyntaxError");
        const list = [];
        if (typeof protocols === "string") list.push(protocols);
        else {
          const iterator = protocols?.[Symbol.iterator];
          if (typeof iterator !== "function")
            throw new TypeError("WebSocket protocols must be iterable");
          const values = iterator.call(protocols);
          try {
            while (list.length <= 16) {
              const next = values.next();
              if (next.done) break;
              list.push(String(next.value));
            }
          } finally {
            if (list.length > 16 && typeof values.return === "function")
              values.return();
          }
        }
        if (
          parsed.username ||
          parsed.password ||
          list.length > 16 ||
          list.some(
            (item, index) =>
              !httpTokenPattern.test(String(item)) ||
              list.map(String).indexOf(String(item)) !== index,
          )
        )
          throw new DOMException("Invalid WebSocket protocol", "SyntaxError");
        this.url = parsed.href.replace(
          /^https?:/,
          parsed.protocol === "https:" ? "wss:" : "ws:",
        );
        this.readyState = WebSocket.CONNECTING;
        this.bufferedAmount = 0;
        this.extensions = "";
        this.protocol = "";
        this._binaryType = "blob";
        this.onopen = null;
        this.onmessage = null;
        this.onerror = null;
        this.onclose = null;
        this._nativeId = 0;
        this._queue = [];
        this._sending = false;
        this._closeRequest = null;
        if (webSockets.size < webSocketLimit) {
          try {
            this._nativeId = Number(
              __tilefinchWebSocketStart(this.url, list.map(String).join(", ")),
            );
          } catch (_) {}
        }
        if (this._nativeId > 0) webSockets.set(this._nativeId, this);
        else if (!__tilefinchRealmGlobal.__tilefinchScheduleTask(
          () => this._fail("duplex transport unavailable"), 0)) {
          this.readyState = WebSocket.CLOSED;
          throw new DOMException("WebSocket task queue is full", "QuotaExceededError");
        }
      }
      get binaryType() {
        return this._binaryType;
      }
      set binaryType(value) {
        value = String(value);
        if (value === "blob" || value === "arraybuffer")
          this._binaryType = value;
      }
      _emit(event) {
        trustedFunctionApply(webSocketDispatch, this, [event]);
        try {
          const handler = this["on" + event.type];
          if (typeof handler === "function")
            globalThis.__tilefinchRunTask(
              "websocket:" + String(event.type), handler, this, [event],
            );
        } catch (error) {
          globalThis.__tilefinchReportUncaught(error, "websocket:" + event.type);
        }
      }
      _finish(code, reason, wasClean, withError) {
        if (this.readyState === WebSocket.CLOSED) return;
        if (this._nativeId) webSockets.delete(this._nativeId);
        this._nativeId = 0;
        this.readyState = WebSocket.CLOSED;
        this._queue = [];
        this.bufferedAmount = 0;
        if (withError) this._emit(browserEvent(EventInterface, "error"));
        this._emit(browserEvent(CloseEventInterface, "close", {
          code, reason, wasClean,
        }));
      }
      _fail(reason) {
        this._finish(1006, String(reason || ""), false, true);
      }
      _pump() {
        if (
          (this.readyState !== WebSocket.OPEN &&
            this.readyState !== WebSocket.CLOSING) ||
          this._sending ||
          !this._queue.length
        ) {
          if (
            !this._sending &&
            !this._queue.length &&
            this._closeRequest &&
            this._nativeId
          ) {
            const request = this._closeRequest;
            this._closeRequest = null;
            if (!__tilefinchWebSocketClose(
              this._nativeId, request.code, request.reason))
              this._fail("WebSocket close admission failed");
          }
          return;
        }
        const item = this._queue[0];
        if (__tilefinchWebSocketSend(
          this._nativeId, item.bytes.buffer, item.binary))
          this._sending = true;
      }
      send(data) {
        if (this.readyState === WebSocket.CONNECTING)
          throw new DOMException("WebSocket is connecting", "InvalidStateError");
        if (this.readyState !== WebSocket.OPEN) return;
        let bytes, binary = true;
        if (typeof data === "string") {
          bytes = new TextEncoder().encode(data);
          binary = false;
        } else if (data instanceof ArrayBuffer) {
          bytes = new Uint8Array(data.slice(0));
        } else if (ArrayBuffer.isView(data)) {
          bytes = new Uint8Array(
            data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
          );
        } else if (typeof Blob !== "undefined" && data instanceof Blob) {
          bytes = new Uint8Array(tilefinchBlobBytes(data));
        } else {
          throw new TypeError("Unsupported WebSocket message type");
        }
        if (
          bytes.byteLength > webSocketByteLimit - this.bufferedAmount ||
          this._queue.length >= webSocketQueueLimit
        )
          throw new DOMException("WebSocket send queue is full", "QuotaExceededError");
        this._queue.push({ bytes, binary });
        this.bufferedAmount += bytes.byteLength;
        this._pump();
      }
      close(code, reason = "") {
        const numericCode = code === undefined ? 1000 : Number(code);
        reason = String(reason);
        if (
          code !== undefined &&
          (!Number.isInteger(numericCode) ||
            (numericCode !== 1000 &&
              (numericCode < 3000 || numericCode > 4999)))
        )
          throw new DOMException("Invalid WebSocket close code", "InvalidAccessError");
        if (new TextEncoder().encode(reason).length > 123)
          throw new DOMException("WebSocket close reason is too long", "SyntaxError");
        if (this.readyState === WebSocket.CLOSED ||
            this.readyState === WebSocket.CLOSING) return;
        this.readyState = WebSocket.CLOSING;
        this._closeRequest = { code: numericCode, reason };
        this._pump();
      }
    }
    for (const [name, value] of Object.entries({
      CONNECTING: 0,
      OPEN: 1,
      CLOSING: 2,
      CLOSED: 3,
    })) {
      Object.defineProperty(WebSocket, name, { value });
      Object.defineProperty(WebSocket.prototype, name, { value });
    }
    globalThis.WebSocket = WebSocket;
    Object.defineProperty(globalThis, "__tilefinchCloseWebSocketForWorker", {
      configurable: false,
      enumerable: false,
      writable: false,
      value(socket) {
        if (!(socket instanceof WebSocket)) return false;
        if (socket._nativeId) {
          webSockets.delete(socket._nativeId);
          try { __tilefinchWebSocketClose(socket._nativeId, 1000, ""); }
          catch (_) {}
        }
        socket._nativeId = 0;
        socket.readyState = WebSocket.CLOSED;
        socket._queue = [];
        socket._sending = false;
        socket._closeRequest = null;
        socket.bufferedAmount = 0;
        socket.onopen = null;
        socket.onmessage = null;
        socket.onerror = null;
        socket.onclose = null;
        return true;
      },
    });
    globalThis.__tilefinchDeliverWebSocket = (
      id,
      kind,
      payload,
      detail,
      code,
      clean,
    ) => {
      const socket = webSockets.get(Number(id));
      if (!socket || socket.readyState === WebSocket.CLOSED) return false;
      if (kind === "open") {
        if (socket.readyState !== WebSocket.CONNECTING) return false;
        socket.protocol = String(detail || "");
        socket.readyState = WebSocket.OPEN;
        socket._emit(browserEvent(EventInterface, "open"));
        socket._pump();
      } else if (kind === "text" || kind === "binary") {
        if (socket.readyState !== WebSocket.OPEN) return false;
        const buffer = payload instanceof ArrayBuffer
          ? payload : new Uint8Array(payload).buffer;
        let data;
        if (kind === "text") data = new TextDecoder().decode(buffer);
        else data = socket.binaryType === "arraybuffer"
          ? buffer : new Blob([buffer]);
        socket._emit(browserEvent(messageEventConstructor, "message", {
          data,
          origin: new TrustedURL(socket.url)._origin,
          source: null,
        }));
      } else if (kind === "drain") {
        const item = socket._queue.shift();
        socket._sending = false;
        if (item)
          socket.bufferedAmount = Math.max(
            0, socket.bufferedAmount - item.bytes.byteLength);
        socket._pump();
      } else {
        socket._finish(Number(code), String(detail || ""), !!clean, !clean);
      }
      return true;
    };
  }
  Object.defineProperty(navigator, "tilefinch", {
    configurable: false,
    enumerable: false,
    value: Object.freeze({
      /* {notice: "once"} only quiets a repeat of the native exit notice:
         the browser still shows it on this document's first entry. */
      requestPageControls(target = document.documentElement, options) {
        if (!target || typeof target.requestFullscreen !== "function") {
          return Promise.reject(new TypeError("Page controls need an element"));
        }
        __tilefinchPageControlsNotice(options?.notice === "once");
        return target.requestFullscreen();
      },
      pageControlsExitChord: "Start+Select",
      pageControlsNotices: Object.freeze(["always", "once"]),
    }),
  });
  {
    /* The channel is multiplayer.js's, built on the first host(), join()
       or discover(): the native loads it, and the factory it leaves on the
       global is read and deleted in that same call, before page code can
       run (Interface snapshot in docs/engineering/JS_RUNTIME.md). */
    const ensureMultiplayer =
      globalThis.__tilefinchEnsureMultiplayerBootstrap;
    let createChannel = null;
    const openChannel = (mode, code, options) => {
      if (!createChannel)
        ensureMultiplayer(() => {
          createChannel = __tilefinchRealmGlobal.__tilefinchCreateMultiplayer;
          delete __tilefinchRealmGlobal.__tilefinchCreateMultiplayer;
        });
      return createChannel(mode, code, options);
    };
    Object.defineProperty(navigator, "tilefinchMultiplayer", {
      configurable: false,
      enumerable: false,
      value: Object.freeze({
        host(options) { return openChannel("host", "", options); },
        join(code, options) { return openChannel("join", code, options); },
        discover(options) { return openChannel("discover", "", options); },
        supported: true,
        maxMessageSize: 512,
      }),
    });
  }
  let xhrEventTargetState, xhrPublicState, xhrPrivateState;
  /* The optional native recorder retains numeric metadata only. */
  const xhrIteratorPrototype = Object.getPrototypeOf((function* () {})()),
    xhrIteratorNext = Function.call.bind(xhrIteratorPrototype.next),
    xhrIteratorReturn = Function.call.bind(xhrIteratorPrototype.return);
  /* The host lab's private-session recorder. Test-probe realms hand its
     installer to native code; nothing else can enable it. */
  let xhrDiagnosticRecord = null;
  const xhrDiagnosticAdd = (state, column, value = 1, replace = false) => {
    if (xhrDiagnosticRecord && state && state.diagnosticSequence)
      xhrDiagnosticRecord(state.diagnosticSequence, column, value, replace);
  },
    /* A readyState change: the lab's state trace and recorder phase. */
    xhrTracePhase = (state, phase) => {
      if (!testProbes) return;
      state.stateTrace.push(phase);
      xhrDiagnosticAdd(state, 4, phase, true);
    };
  if (testProbes)
    globalThis.__tilefinchHostChannel.xhrDiagnostics = (recorder) => {
      xhrDiagnosticRecord = recorder;
    };
  const xhrEventHandlerTypes = [
      "abort",
      "error",
      "load",
      "loadend",
      "loadstart",
      "progress",
      "timeout",
  ];
  class XMLHttpRequestEventTarget extends EventTarget {
    #eventState;
    #publicState;
    static {
      xhrEventTargetState = (target) => target.#eventState;
      xhrPublicState = (target) => target.#publicState;
    }
    constructor() {
      super();
      this.#eventState = {
        handlers: new Map(),
        handlerListeners: new Map(),
      };
      this.#publicState = Object.create(null);
    }
  }
  const xhrSetEventHandler = (target, type, value) => {
    const state = xhrEventTargetState(target);
    if (!state) throw new TypeError("Illegal invocation");
    const callback = typeof value === "function" ? value : null,
      wrapper = state.handlerListeners.get(type);
    state.handlers.set(type, callback);
    if (callback && !wrapper) {
      const listener = function (event) {
        const active = xhrEventTargetState(this)?.handlers.get(type);
        if (typeof active === "function") return active.call(this, event);
      };
      state.handlerListeners.set(type, listener);
      trustedAddEventListener(target, type, listener);
    } else if (!callback && wrapper) {
      trustedRemoveEventListener(target, type, wrapper);
      state.handlerListeners.delete(type);
    }
  };
  for (const type of xhrEventHandlerTypes)
    Object.defineProperty(XMLHttpRequestEventTarget.prototype, "on" + type, {
      configurable: true,
      enumerable: true,
      get() {
        return xhrEventTargetState(this)?.handlers.get(type) || null;
      },
      set(value) {
        xhrSetEventHandler(this, type, value);
      },
    });
  Object.defineProperty(
    XMLHttpRequestEventTarget.prototype,
    Symbol.toStringTag,
    { configurable: true, value: "XMLHttpRequestEventTarget" },
  );
  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
  Object.defineProperty(XMLHttpRequestUpload.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "XMLHttpRequestUpload",
  });
  const xhrSetPublic = (target, name, value) => {
    const state = xhrPublicState(target);
    if (!state) throw new TypeError("Illegal invocation");
    state[name] = value;
  };
  const xhrResetResponse = (xhr) => {
    const state = xhrPrivateState(xhr);
    state.responseText = "";
    state.responseHeaders = new Headers();
    state.responseBytes = 0;
    state.responseLengthComputable = false;
    xhrSetPublic(xhr, "status", 0);
    xhrSetPublic(xhr, "statusText", "");
    xhrSetPublic(xhr, "response", null);
    xhrSetPublic(xhr, "responseURL", "");
    xhrSetPublic(xhr, "responseXML", null);
  };
  class TilefinchXMLHttpRequest extends XMLHttpRequestEventTarget {
    #privateState;
    static {
      xhrPrivateState = (target) => target.#privateState;
    }
    constructor() {
      super();
      xhrSetPublic(this, "readyState", 0);
      xhrSetPublic(this, "status", 0);
      xhrSetPublic(this, "statusText", "");
      xhrSetPublic(this, "response", null);
      xhrSetPublic(this, "responseURL", "");
      xhrSetPublic(this, "responseXML", null);
      this.onreadystatechange = null;
      xhrSetPublic(this, "timeout", 0);
      xhrSetPublic(this, "withCredentials", false);
      xhrSetPublic(this, "upload", new XMLHttpRequestUpload());
      this.#privateState = {
        async: true,
        worker: false,
        done: true,
        generation: 0,
        headers: new Headers(),
        method: "GET",
        mimeType: "",
        requestId: 0,
        responseBytes: 0,
        responseLengthComputable: false,
        responseHeaders: new Headers(),
        responseText: "",
        responseType: "",
        sent: false,
        stateTrace: testProbes ? [] : null,
        timeoutId: 0,
        uploadBytes: 0,
        uploadComplete: false,
        uploadStarted: false,
        url: "",
        diagnosticSequence: 0,
      };
      if (testProbes)
        xhrEventTargetState(this).diagnosticState = this.#privateState;
    }
    get responseType() {
      return xhrPrivateState(this).responseType;
    }
    set responseType(value) {
      const state = xhrPrivateState(this);
      value = String(value);
      if (
        !["", "text", "json", "arraybuffer", "blob", "document"].includes(value)
      )
        return;
      if (!state.async && !state.worker)
        throw new DOMException("Response type is unavailable for synchronous Window XHR",
          "InvalidAccessError");
      const readyState = xhrPublicState(this).readyState;
      if (readyState === 3 || readyState === 4)
        throw new DOMException(
          "Response type cannot change now",
          "InvalidStateError",
        );
      state.responseType = value;
    }
    get responseText() {
      const state = xhrPrivateState(this);
      xhrDiagnosticAdd(state, 5);
      if (state.responseType !== "" && state.responseType !== "text")
        throw new DOMException(
          "responseText is unavailable for this response type",
          "InvalidStateError",
        );
      const readyState = xhrPublicState(this).readyState;
      return readyState === 3 || readyState === 4
        ? state.responseText : "";
    }
    open(method, url, async = true) {
      const state = xhrPrivateState(this);
      const nextMethod = normalizeMethod(String(method)),
        nextURL = String(url), synchronous = !async;
      if (synchronous && !state.worker &&
          (xhrPublicState(this).timeout !== 0 || state.responseType !== ""))
        throw new DOMException("Timeout and response type are unavailable for synchronous Window XHR",
          "InvalidAccessError");
      if (!state.done && state.requestId)
        cancelNetwork(
          state.requestId,
          new DOMException("Request reopened", "AbortError"),
          false,
        );
      if (state.timeoutId) domShared.cancelTimer(state.timeoutId);
      state.timeoutId = 0;
      state.generation += 1;
      state.method = nextMethod;
      state.url = nextURL;
      state.async = !synchronous;
      xhrResetResponse(this);
      state.headers = new Headers();
      state.requestId = 0;
      state.done = true;
      state.sent = false;
      state.uploadBytes = 0;
      state.uploadComplete = false;
      state.uploadStarted = false;
      const opening = xhrPublicState(this).readyState !== 1;
      xhrSetPublic(this, "readyState", 1);
      if (testProbes) state.stateTrace = [1];
      if (opening) xhrEmit(this, "readystatechange");
    }
    setRequestHeader(name, value) {
      const state = xhrPrivateState(this);
      if (this.readyState !== 1 || state.sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      appendHeader(state.headers, name, value);
    }
    overrideMimeType(type) {
      if (this.readyState === 3 || this.readyState === 4)
        throw new DOMException("Response is loading", "InvalidStateError");
      xhrPrivateState(this).mimeType = String(type);
    }
    emit(type, loaded = 0, total = 0) {
      const event = browserEvent(EventInterface, type);
      event.target = this;
      event.loaded = loaded;
      event.total = total;
      event.lengthComputable = total > 0;
      const handler = this["on" + type];
      if (typeof handler === "function") handler.call(this, event);
      for (const callback of [...(this.listeners.get(type) || [])])
        callback.call(this, event);
    }
    _state(value) {
      xhrSetPublic(this, "readyState", value);
      xhrEmit(this, "readystatechange");
    }
    _finish(type, generation) {
      const state = xhrPrivateState(this);
      if (state.done || state.generation !== generation) return false;
      state.done = true;
      if (state.timeoutId) domShared.cancelTimer(state.timeoutId);
      state.timeoutId = 0;
      state.requestId = 0;
      const loaded = state.responseBytes,
        total = state.responseLengthComputable ? loaded : 0;
      xhrEmit(this, type, loaded, total);
      if (state.generation !== generation) return false;
      xhrEmit(this, "loadend", loaded, total);
      return state.generation === generation;
    }
    *_apply(raw, generation) {
      const state = xhrPrivateState(this);
      if (state.done || state.generation !== generation) return;
      if (state.uploadStarted && !state.uploadComplete) {
        state.uploadComplete = true;
        for (const type of ["progress", "load", "loadend"]) {
          yield [this.upload, type, state.uploadBytes, state.uploadBytes, true];
          if (state.done || state.generation !== generation) return;
        }
      }
      xhrSetPublic(this, "status", Number(raw.status) || 0);
      xhrDiagnosticAdd(state, 1, Number(raw.status) || 0, true);
      xhrSetPublic(this, "responseURL", raw.url || state.url);
      state.responseHeaders = headersFromTransport(raw);
      xhrSetPublic(this, "readyState", 2);
      xhrTracePhase(state, 2);
      yield [this, "readystatechange", 0, 0, false];
      if (state.done || state.generation !== generation) return;
      const supplied =
          raw.bodyBytes instanceof ArrayBuffer
            ? new Uint8Array(raw.bodyBytes)
            : null,
        responseMime = state.mimeType ||
          state.responseHeaders.get("content-type") || "",
        needsText =
          state.responseType === "" ||
          state.responseType === "text" ||
          state.responseType === "json" ||
          state.responseType === "document" ||
          /(?:xml|html)/i.test(responseMime),
        charset = /(?:^|;)\s*charset\s*=\s*(?:"([^"]*)"|'([^']*)'|([^;\s]*))/i
          .exec(responseMime),
        textBytes = supplied || new Uint8Array();
      let decodedText = "";
      if (needsText && raw.body === undefined) {
        try {
          decodedText = new TextDecoder(
            charset ? charset[1] || charset[2] || charset[3] : "utf-8",
          ).decode(textBytes);
        } catch (_) {
          decodedText = new TextDecoder().decode(textBytes);
        }
      }
      state.responseText = needsText
        ? raw.body !== undefined
          ? String(raw.body)
          : decodedText
        : "";
      xhrDiagnosticAdd(state, 3, state.responseText.length, true);
      xhrSetPublic(this, "readyState", 3);
      xhrTracePhase(state, 3);
      yield [this, "readystatechange", 0, 0, false];
      if (state.done || state.generation !== generation) return;
      const fallbackBody = raw.body === undefined ? "" : String(raw.body),
        byteLength = Number.isFinite(Number(raw.bodyLength))
          ? Math.max(0, Number(raw.bodyLength))
          : supplied
            ? supplied.byteLength
            : new TextEncoder().encode(fallbackBody).byteLength;
      state.responseBytes = byteLength;
      xhrDiagnosticAdd(state, 2, byteLength, true);
      state.responseLengthComputable = true;
      yield [this, "progress", byteLength, byteLength, true];
      if (state.generation !== generation || state.done) return;
      if (state.responseType === "" || state.responseType === "text")
        xhrSetPublic(this, "response", state.responseText);
      else if (state.responseType === "json") {
        try {
          xhrSetPublic(this, "response", JSON.parse(state.responseText));
        } catch (_) {
          xhrSetPublic(this, "response", null);
        }
      } else if (state.responseType === "arraybuffer") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        xhrSetPublic(this, "response",
          bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
            ? bytes.buffer
            : bytes.slice().buffer);
      } else if (state.responseType === "blob") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        xhrSetPublic(this, "response", new Blob([bytes], {
          type: state.responseHeaders.get("content-type") || "",
        }));
      } else if (state.responseType === "document") {
        const mime =
          /xml/i.test(responseMime)
            ? "text/xml"
            : "text/html";
        xhrSetPublic(this, "response", new DOMParser().parseFromString(
          state.responseText,
          mime,
        ));
      } else xhrSetPublic(this, "response", null);
      if (
        state.responseType === "" &&
        /(?:xml|html)/i.test(responseMime)
      ) {
        const mime = /xml/i.test(responseMime) ? "text/xml" : "text/html";
        xhrSetPublic(this, "responseXML", new DOMParser().parseFromString(
          state.responseText,
          mime,
        ));
      } else if (state.responseType === "document") {
        xhrSetPublic(this, "responseXML", this.response);
      }
      xhrSetPublic(this, "readyState", 4);
      xhrTracePhase(state, 4);
      yield [this, "readystatechange", 0, 0, false];
      if (state.done || state.generation !== generation) return;
      state.done = true;
      if (state.timeoutId) domShared.cancelTimer(state.timeoutId);
      state.timeoutId = 0;
      state.requestId = 0;
      yield [this, "load", byteLength, byteLength, true];
      if (state.generation !== generation) return;
      yield [this, "loadend", byteLength, byteLength, true];
      if (state.generation === generation)
        xhrDiagnosticAdd(state, 4, 5, true);
    }
    _fail(error, type = "error", generation) {
      const state = xhrPrivateState(this);
      if (state.done || state.generation !== generation) return;
      if (testProbes)
        globalThis.__tilefinchXHRLastError = String(
          (error && error.stack) || error || type,
        );
      state.sent = false;
      xhrResetResponse(this);
      if (!xhrState(this, 4, generation)) return;
      if (!xhrUploadFinish(this, type, generation)) return;
      xhrFinish(this, type, generation);
    }
    send(body = null) {
      const state = xhrPrivateState(this);
      if (this.readyState !== 1 || state.sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      if (testProbes) globalThis.__tilefinchXHRSendCalls++;
      /* XHR drops the body of a GET or HEAD (pages pass "" to every send). */
      if (state.method === "GET" || state.method === "HEAD") body = null;
      if (xhrDiagnosticRecord)
        state.diagnosticSequence = xhrDiagnosticRecord(0, 0, 0, false);
      state.sent = true;
      state.done = false;
      const generation = state.generation;
      xhrEmit(this, "loadstart");
      if (state.generation !== generation || state.done || !state.sent) return;
      try {
        const request = { _bodySource: body, headers: state.headers },
          serialized = serializeRequestBody(request),
          method = state.method || "GET",
          url = state.url,
          contentType = getHeader(state.headers, "content-type") || "",
          accept = getHeader(state.headers, "accept") || "*/*",
          headerBlock = nativeHeaderBlock(state.headers),
          credentials = this.withCredentials ? "include" : "same-origin",
          timeout = Math.max(0, Number(this.timeout) || 0);
        if (!xhrUploadStart(this, method, serialized, generation)) return;
        let local;
        try {
          local = snapshotLocalBlobRequest(method, url);
        } catch (error) {
          if (state.async) {
            queueMicrotask(() => xhrFail(this, error, "error", generation));
            return;
          }
          throw error;
        }
        if (local) {
          if (state.async)
            queueMicrotask(() => xhrApply(this, local, generation));
          else xhrApply(this, local, generation);
          return;
        }
        if (state.async) {
          let id;
          try {
            id = queueNetwork(
              () =>
                __tilefinchFetchAsync(
                  method,
                  url,
                  serialized,
                  contentType,
                  headerBlock,
                  "cors",
                  credentials,
                  timeout,
                  undefined,
                  undefined,
                  undefined,
                  undefined,
                  undefined,
                  accept,
                  undefined,
                  state.responseType === "" ||
                    state.responseType === "text" ||
                    state.responseType === "json" ||
                    state.responseType === "document",
                ),
              networkRetainedBytes(
                method,
                url,
                serialized,
                contentType,
                headerBlock,
                "cors",
                credentials,
              ),
              (raw) => xhrApply(this, raw, generation),
              (error) => {
                const timedOut =
                  timeout > 0 && /timeout|timed out/i.test(String(error));
                xhrFail(
                  this,
                  error,
                  timedOut ? "timeout" : "error",
                  generation,
                );
              },
              url,
              "xmlhttprequest",
            );
          } catch (error) {
            /* An async XHR reports transport/admission failure from a later
               task.  Dispatching it inside send() makes an ordinary retry
               handler recurse in one JavaScript turn when the bounded queue
               is full, unlike the browser networking model and without an
               opportunity for completed requests to release pressure. */
            __tilefinchRealmGlobal.__tilefinchScheduleTask(() => xhrFail(this, error, "error", generation), 0);
            return;
          }
          if (state.generation !== generation || state.done) {
            if (pendingNetwork.has(id))
              cancelNetwork(
                id,
                new DOMException("Request superseded", "AbortError"),
                false,
              );
            return;
          }
          state.requestId = pendingNetwork.has(id) ? Number(id) : 0;
          if (!state.done && timeout > 0)
            state.timeoutId = __tilefinchRealmGlobal.__tilefinchScheduleTask(() => {
              if (state.done || state.generation !== generation) return;
              cancelNetwork(
                state.requestId,
                new DOMException("The operation timed out", "TimeoutError"),
                false,
              );
              xhrFail(this, new DOMException(
                "The operation timed out", "TimeoutError"), "timeout", generation);
            }, timeout);
        } else {
          const raw = __tilefinchFetchSync(
              method,
              url,
              serialized,
              contentType,
              headerBlock,
              "cors",
              credentials,
            );
          xhrApply(this, raw, generation);
        }
      } catch (error) {
        xhrFail(this, error, "error", generation);
      }
    }
    abort() {
      const state = xhrPrivateState(this);
      const readyState = this.readyState;
      if (readyState === 0 || (readyState === 1 && !state.sent)) return;
      xhrDiagnosticAdd(state, 4, 7, true);
      state.generation += 1;
      const generation = state.generation;
      if (state.timeoutId) domShared.cancelTimer(state.timeoutId);
      state.timeoutId = 0;
      if (state.requestId)
        cancelNetwork(
          state.requestId,
          new DOMException("This operation was aborted", "AbortError"),
          false,
        );
      state.sent = false;
      xhrResetResponse(this);
      /* DONE abort is a silent reset. Active abort reports DONE throughout
         its error events, then resets only if no handler reopened the XHR. */
      if (readyState === 4) {
        state.done = true;
        state.requestId = 0;
        xhrSetPublic(this, "readyState", 0);
        return;
      }
      if (!xhrState(this, 4, generation)) return;
      if (!xhrUploadFinish(this, "abort", generation)) return;
      xhrFinish(this, "abort", generation);
      if (state.generation === generation && this.readyState === 4)
        xhrSetPublic(this, "readyState", 0);
    }
    getResponseHeader(name) {
      return this.readyState < 2
        ? null
        : xhrPrivateState(this).responseHeaders.get(name);
    }
    getAllResponseHeaders() {
      if (this.readyState < 2) return "";
      let output = "";
      xhrPrivateState(this).responseHeaders.forEach(
        (value, name) => (output += name + ": " + value + "\r\n"),
      );
      return output;
    }
  }
  Object.defineProperty(TilefinchXMLHttpRequest, "name", {
    configurable: true,
    value: "XMLHttpRequest",
  });
  if (testProbes) {
    globalThis.__tilefinchXHRResponseCount = 0;
    globalThis.__tilefinchXHRLastStatus = 0;
    globalThis.__tilefinchXHRLastResponseType = "";
    globalThis.__tilefinchXHRLastTextLength = 0;
    globalThis.__tilefinchXHRLastByteLength = 0;
    globalThis.__tilefinchXHRLastStates = "";
  }
  for (const name of [
    "readyState",
    "status",
    "statusText",
    "response",
    "responseURL",
    "responseXML",
    "upload",
  ])
    Object.defineProperty(TilefinchXMLHttpRequest.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        const privateState = xhrPrivateState(this);
        const state = xhrPublicState(this);
        if (!state) throw new TypeError("Illegal invocation");
        if (name === "response") {
          xhrDiagnosticAdd(privateState, 6);
          if (privateState.responseType === "" ||
              privateState.responseType === "text")
            return state.readyState === 3 || state.readyState === 4
              ? privateState.responseText : "";
          if (state.readyState !== 4) return null;
        }
        return state[name];
      },
    });
  for (const name of ["timeout", "withCredentials"])
    Object.defineProperty(TilefinchXMLHttpRequest.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        xhrPrivateState(this);
        const state = xhrPublicState(this);
        if (!state) throw new TypeError("Illegal invocation");
        return state[name];
      },
      set(value) {
        const request = xhrPrivateState(this);
        const state = xhrPublicState(this);
        if (!state) throw new TypeError("Illegal invocation");
        if (name === "timeout") {
          const timeout = +value >>> 0;
          if (!request.async && !request.worker)
            throw new DOMException("Timeout is unavailable for synchronous Window XHR",
              "InvalidAccessError");
          state.timeout = timeout;
        } else {
          if (state.readyState > 1 || request.sent)
            throw new DOMException("Request is already sending", "InvalidStateError");
          state.withCredentials = !!value;
        }
      },
    });
  Object.defineProperty(
    TilefinchXMLHttpRequest.prototype,
    "onreadystatechange",
    {
      configurable: true,
      enumerable: true,
      get() {
        xhrPrivateState(this);
        return xhrEventTargetState(this)
          ?.handlers.get("readystatechange") || null;
      },
      set(value) {
        xhrPrivateState(this);
        xhrSetEventHandler(this, "readystatechange", value);
      },
    },
  );
  const xhrStateImpl = TilefinchXMLHttpRequest.prototype._state,
    xhrFinishImpl = TilefinchXMLHttpRequest.prototype._finish,
    xhrApplyImpl = TilefinchXMLHttpRequest.prototype._apply,
    xhrFailImpl = TilefinchXMLHttpRequest.prototype._fail,
    xhrCheckpointDispatch = globalThis.__tilefinchDispatchEventTargetCheckpointed,
    xhrEmitTarget = (target, type, loaded = 0, total = 0,
                     lengthComputable = total > 0, complete = null) => {
      /* XMLHttpRequest is the user agent, not author script.  Its progress
         events are trusted ProgressEvents in browsers; readystatechange is
         the one plain Event in this sequence.  Challenge runtimes inspect
         both distinctions when deciding whether a response callback came
         from the transport or from an authored redispatch. */
      const event = trustedEvent(
        type === "readystatechange"
          ? browserEvent(EventInterface, type)
          : browserEvent(progressEventConstructor, type, {
              lengthComputable,
              loaded,
              total,
            }),
      );
      const state = xhrDiagnosticRecord
        ? xhrEventTargetState(target).diagnosticState : null;
      if (type === "readystatechange") xhrDiagnosticAdd(state, 7);
      else if (type === "progress") xhrDiagnosticAdd(state, 8);
      else if (type === "load") xhrDiagnosticAdd(state, 9);
      if (complete) xhrCheckpointDispatch(target, event, complete);
      else fileReaderDispatch(target, event);
    },
    xhrEmit = (xhr, type, loaded = 0, total = 0) =>
      xhrEmitTarget(xhr, type, loaded, total),
    xhrGenerationCurrent = (xhr, generation) =>
      xhrPrivateState(xhr)?.generation === generation,
    xhrUploadStart = (xhr, method, serialized, generation) => {
      const state = xhrPrivateState(xhr);
      if (method === "GET" || method === "HEAD" || serialized === undefined)
        return xhrGenerationCurrent(xhr, generation);
      const bytes = typeof serialized === "string"
        ? new TextEncoder().encode(serialized).byteLength
        : serialized.byteLength;
      state.uploadBytes = bytes;
      state.uploadStarted = true;
      state.uploadComplete = false;
      xhrEmitTarget(xhr.upload, "loadstart", 0, bytes, true);
      return xhrGenerationCurrent(xhr, generation) && !state.done;
    },
    xhrUploadFinish = (xhr, type, generation) => {
      const state = xhrPrivateState(xhr);
      if (!xhrGenerationCurrent(xhr, generation)) return false;
      if (!state.uploadStarted || state.uploadComplete) return !state.done;
      state.uploadComplete = true;
      const bytes = state.uploadBytes;
      if (type === "load") {
        xhrEmitTarget(xhr.upload, "progress", bytes, bytes, true);
        if (!xhrGenerationCurrent(xhr, generation)) return false;
      }
      xhrEmitTarget(xhr.upload, type, type === "load" ? bytes : 0, bytes, true);
      if (!xhrGenerationCurrent(xhr, generation)) return false;
      xhrEmitTarget(
        xhr.upload, "loadend", type === "load" ? bytes : 0, bytes, true,
      );
      return xhrGenerationCurrent(xhr, generation) && !state.done;
    },
    xhrState = (xhr, value, generation) => {
      const state = xhrPrivateState(xhr);
      if (state.generation !== generation || state.done) return false;
      xhrTracePhase(state, value);
      xhrStateImpl.call(xhr, value);
      return state.generation === generation && !state.done;
    },
    xhrFinish = (xhr, type, generation) =>
      xhrFinishImpl.call(xhr, type, generation),
    xhrApply = (xhr, raw, generation) => {
      if (!xhrGenerationCurrent(xhr, generation)) return;
      const supplied =
          raw.bodyBytes instanceof ArrayBuffer
            ? raw.bodyBytes.byteLength
            : null,
        reported = Number(raw.bodyLength),
        byteLength = Number.isFinite(reported)
          ? Math.max(0, reported)
          : supplied !== null
            ? supplied
            : new TextEncoder().encode(String(raw.body || "")).byteLength;
      const iterator = xhrApplyImpl.call(xhr, raw, generation),
        completed = () => {
          if (testProbes && xhrGenerationCurrent(xhr, generation)
          && xhr.readyState === 4 && xhr.status !== 0) {
            const state = xhrPrivateState(xhr);
            xhrDiagnosticAdd(state, 1, xhr.status, true);
            xhrDiagnosticAdd(state, 2, byteLength, true);
            xhrDiagnosticAdd(state, 3, state.responseText.length, true);
            globalThis.__tilefinchXHRResponseCount++;
            globalThis.__tilefinchXHRLastStatus = xhr.status;
            globalThis.__tilefinchXHRLastResponseType = state.responseType;
            globalThis.__tilefinchXHRLastTextLength = state.responseText.length;
            globalThis.__tilefinchXHRLastByteLength = byteLength;
            globalThis.__tilefinchXHRLastStates = state.stateTrace.join(".");
          }
        };
      /* Serialize the buffered native response across browser-invoked event
         checkpoints. Never advance readyState past a listener's pending
         promises. Sync send()/authored dispatchEvent() stay synchronous. */
      if (xhrPrivateState(xhr).async) {
        let advancing = false, advanceAgain = false;
        const next = () => {
          /* Listener-free events complete inline. Trampoline those steps
             rather than nesting the full dispatcher for every response
             state: sanitizer/PSP stacks must not lose loadend to an otherwise
             recoverable stack refusal after readyState has reached DONE. */
          if (advancing) { advanceAgain = true; return; }
          advancing = true;
          try {
            for (let eventIndex = 0; eventIndex < 12; eventIndex++) {
              advanceAgain = false;
              const step = xhrIteratorNext(iterator);
              if (step.done) { completed(); return; }
              const event = step.value;
              xhrEmitTarget(event[0], event[1], event[2], event[3], event[4], next);
              if (!advanceAgain) return;
            }
            throw new Error("XHR response event sequence exceeded its bound");
          } catch (error) {
            xhrIteratorReturn(iterator);
            xhrFail(xhr, error, "error", generation);
          } finally {
            advancing = false;
          }
        };
        next();
      } else {
        for (let event = 0; event < 12; event++) {
          const step = xhrIteratorNext(iterator);
          if (step.done) { completed(); return; }
          const value = step.value;
          xhrEmitTarget(value[0], value[1], value[2], value[3], value[4]);
        }
        xhrIteratorReturn(iterator);
        throw new Error("XHR response event sequence exceeded its bound");
      }
    },
    xhrFail = (xhr, error, type = "error", generation) => {
      if (!xhrGenerationCurrent(xhr, generation)) return;
      const previous = testProbes ? globalThis.__tilefinchXHRLastError : "";
      xhrFailImpl.call(xhr, error, type, generation);
      xhrDiagnosticAdd(xhrPrivateState(xhr), 4, 6, true);
      if (testProbes && type !== "error")
        globalThis.__tilefinchXHRLastError = previous;
    };
  const constructXHRForWorker = Reflect.construct;
  trustedDefineProperty(globalThis, "__tilefinchCreateWorkerXHR", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(newTarget) {
      const xhr = constructXHRForWorker(TilefinchXMLHttpRequest, [], newTarget);
      xhrPrivateState(xhr).worker = true;
      return xhr;
    },
  });
  /* A dedicated worker may be terminated while one of its owner-backed XHR
     transports is still pending.  Abort that transport without dispatching
     callbacks into the retired Worker realm.  The public abort() algorithm is
     deliberately not used here because it synchronously fires abort/loadend. */
  Object.defineProperty(globalThis, "__tilefinchAbortXHRForWorker", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: (xhr) => {
      const state = xhrPrivateState(xhr);
      if (!state) return false;
      state.generation += 1;
      if (state.timeoutId) domShared.cancelTimer(state.timeoutId);
      state.timeoutId = 0;
      if (state.requestId)
        cancelNetwork(
          state.requestId,
          new DOMException("Worker is terminated", "AbortError"),
          false,
        );
      state.requestId = 0;
      state.done = true;
      state.sent = false;
      state.uploadComplete = true;
      xhrSetPublic(xhr, "status", 0);
      xhrSetPublic(xhr, "readyState", 0);
      xhrSetPublic(xhr, "response", null);
      return true;
    },
  });
  delete TilefinchXMLHttpRequest.prototype.emit;
  delete TilefinchXMLHttpRequest.prototype._state;
  delete TilefinchXMLHttpRequest.prototype._finish;
  delete TilefinchXMLHttpRequest.prototype._apply;
  delete TilefinchXMLHttpRequest.prototype._fail;
  Object.defineProperty(
    TilefinchXMLHttpRequest.prototype,
    Symbol.toStringTag,
    { configurable: true, value: "XMLHttpRequest" },
  );
  installXhrValidation(TilefinchXMLHttpRequest);
  for (const [name, value] of Object.entries({
    UNSENT: 0,
    OPENED: 1,
    HEADERS_RECEIVED: 2,
    LOADING: 3,
    DONE: 4,
  })) {
    Object.defineProperty(TilefinchXMLHttpRequest, name, { value });
    Object.defineProperty(TilefinchXMLHttpRequest.prototype, name, { value });
  }
  globalThis.XMLHttpRequestEventTarget = XMLHttpRequestEventTarget;
  globalThis.XMLHttpRequestUpload = XMLHttpRequestUpload;
  globalThis.XMLHttpRequest = TilefinchXMLHttpRequest;
  const markXhrNative = globalThis.__tilefinchMarkNativeFunction;
  for (const constructor of [
    XMLHttpRequestEventTarget,
    XMLHttpRequestUpload,
    TilefinchXMLHttpRequest,
  ]) {
    markXhrNative(constructor);
    for (const key of Reflect.ownKeys(constructor.prototype)) {
      const descriptor = Object.getOwnPropertyDescriptor(
        constructor.prototype,
        key,
      );
      if (
        key !== "constructor" &&
        typeof key === "string" &&
        typeof descriptor?.value === "function" &&
        descriptor.value.name === ""
      )
        Object.defineProperty(descriptor.value, "name", {
          configurable: true,
          value: key,
        });
      markXhrNative(descriptor?.value);
      markXhrNative(descriptor?.get);
      markXhrNative(descriptor?.set);
    }
  }
  /* SharedWorker is a shell: present for feature detection, refusing every
     construction. Sites that test `window.SharedWorker` to tell a modern
     browser from an old one take their modern path instead of loading
     larger legacy and polyfill bundles, and real use takes
     its try/catch fallback. It is eager on purpose: routing it through the
     lazy Worker group would charge that module to pages that only probe. */
  globalThis.SharedWorker = class SharedWorker extends EventTarget {
    /* Refuses before super(), so a refused construction allocates no
       EventTarget. The parameter gives the constructor length 1. */
    constructor(scriptURL) {
      if (arguments.length < 1)
        throw new TypeError(
          "Failed to construct 'SharedWorker': 1 argument required");
      throw new DOMException(
        "SharedWorker is not supported", "NotSupportedError");
    }
  };
  Object.defineProperty(globalThis.SharedWorker.prototype, Symbol.toStringTag,
    { configurable: true, value: "SharedWorker" });
  markXhrNative(globalThis.SharedWorker);
  if (globalThis.__tilefinchRootCensus)
    Object.defineProperty(globalThis.__tilefinchRootCensus, "pendingNetwork", {
      get: () => pendingNetwork.size,
    });

  /* Small device-state APIs live with the compatibility layer so platform.js
     remains below the source-fallback admission ceiling. Public objects are
     still allocated only when author code first asks for each capability. */
  /* Every powerful-feature name in the Permissions Registry and the
     earlier PermissionName enum (which fingerprinting scripts still walk)
     answers "denied": the PSP has none of these features, except
     "microphone", whose state is the site's own (prompt, granted or denied:
     src/js_media_capture.c) and changes with a "change" event. Other names
     reject with a TypeError, as the Permissions spec requires. One string,
     not a Set: it is all the realm keeps for them. */
  const platformStatusToken = {},
    permissionNames = " accelerometer accessibility-events \
ambient-light-sensor background-fetch background-sync bluetooth camera \
clipboard clipboard-read clipboard-write device-info display-capture \
geolocation gyroscope idle-detection local-fonts magnetometer microphone \
midi nfc notifications payment-handler periodic-background-sync \
persistent-storage push screen-wake-lock speaker speaker-selection \
storage-access window-management ";
  let networkInformation = null,
    batteryManager = null,
    batteryPromise = null,
    permissions = null;
  const microphonePermission = globalThis.__tilefinchMediaCapturePermission,
    /* The newest microphone statuses, bounded; an older one stops
       receiving change events. */
    microphoneStatuses = [],
    permissionState = (name) =>
      name === "microphone" && typeof microphonePermission === "function"
        ? microphonePermission() : "denied";
  delete globalThis.__tilefinchMediaCapturePermission;
  const networkInformationStates = new TrustedWeakMap(),
    permissionStatusStates = new TrustedWeakMap(),
    batteryStates = new TrustedWeakMap(),
    networkInformationState = value => {
      const state = trustedWeakMapGet(networkInformationStates, value);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    },
    permissionStatusState = value => {
      const state = trustedWeakMapGet(permissionStatusStates, value);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    },
    batteryState = value => {
      const state = trustedWeakMapGet(batteryStates, value);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    };
  class NetworkInformation extends EventTarget {
    constructor() {
      if (arguments[0] !== platformStatusToken)
        throw new TypeError("Illegal constructor");
      super();
      trustedWeakMapSet(networkInformationStates, this, { onchange: null });
    }
    get onchange() { return networkInformationState(this).onchange; }
    set onchange(value) {
      networkInformationState(this).onchange =
        typeof value === "function" ? value : null;
    }
    /* The PSP's only network interface is 802.11b Wi-Fi. Report the
       standardized connection kind and the radio's first-hop ceiling rather
       than leaving two members missing from an interface we already expose. */
    get type() { networkInformationState(this); return "wifi"; }
    get downlinkMax() { networkInformationState(this); return 11; }
    get effectiveType() { networkInformationState(this); return "3g"; }
    get rtt() { networkInformationState(this); return 300; }
    get downlink() { networkInformationState(this); return 1.5; }
    get saveData() { networkInformationState(this); return false; }
  }
  class BatteryManager extends EventTarget {
    constructor() {
      if (arguments[0] !== platformStatusToken)
        throw new TypeError("Illegal constructor");
      super();
      trustedWeakMapSet(batteryStates, this, {
        onchargingchange: null,
        onchargingtimechange: null,
        ondischargingtimechange: null,
        onlevelchange: null,
      });
    }
    get charging() { batteryState(this); return true; }
    get chargingTime() { batteryState(this); return 0; }
    get dischargingTime() { batteryState(this); return Infinity; }
    get level() { batteryState(this); return 1; }
    get onchargingchange() { return batteryState(this).onchargingchange; }
    set onchargingchange(value) {
      batteryState(this).onchargingchange =
        typeof value === "function" ? value : null;
    }
    get onchargingtimechange() {
      return batteryState(this).onchargingtimechange;
    }
    set onchargingtimechange(value) {
      batteryState(this).onchargingtimechange =
        typeof value === "function" ? value : null;
    }
    get ondischargingtimechange() {
      return batteryState(this).ondischargingtimechange;
    }
    set ondischargingtimechange(value) {
      batteryState(this).ondischargingtimechange =
        typeof value === "function" ? value : null;
    }
    get onlevelchange() { return batteryState(this).onlevelchange; }
    set onlevelchange(value) {
      batteryState(this).onlevelchange =
        typeof value === "function" ? value : null;
    }
  }
  class PermissionStatus extends EventTarget {
    constructor(token, name) {
      if (token !== platformStatusToken)
        throw new TypeError("Illegal constructor");
      super();
      name = String(name);
      trustedWeakMapSet(permissionStatusStates, this, {
        name,
        state: permissionState(name),
        onchange: null,
      });
      if (name === "microphone") {
        if (microphoneStatuses.length === 16) microphoneStatuses.shift();
        microphoneStatuses.push(this);
      }
    }
    get name() { return permissionStatusState(this).name; }
    get state() { return permissionStatusState(this).state; }
    get onchange() { return permissionStatusState(this).onchange; }
    set onchange(value) {
      const state = permissionStatusState(this);
      if (state.onchange)
        trustedRemoveEventListener(this, "change", state.onchange);
      state.onchange = typeof value === "function" ? value : null;
      if (state.onchange)
        trustedAddEventListener(this, "change", state.onchange);
    }
  }
  class Permissions {
    constructor() {
      if (arguments[0] !== platformStatusToken)
        throw new TypeError("Illegal constructor");
    }
    query(descriptor) {
      let name;
      try {
        if (this !== permissions)
          throw new TypeError("Illegal invocation");
        if (descriptor === null || descriptor === undefined)
          throw new TypeError("Permission descriptor is required");
        if (typeof descriptor !== "object" && typeof descriptor !== "function")
          throw new TypeError("Permission descriptor must be an object");
        const value = descriptor.name;
        if (typeof value === "symbol")
          throw new TypeError("Symbol is not a string");
        name = String(value);
        if (name.includes(" ") || !permissionNames.includes(" " + name + " "))
          throw new TypeError("Unsupported permission name");
      } catch (error) {
        return Promise.reject(error);
      }
      /* Permissions.query() snapshots its descriptor before returning and
         resolves from the permissions task source. A later author mutation
         cannot alter the pending request, while conversion/receiver failures
         remain promise rejections. Keep successful completion bounded by the
         scheduler's existing timer quota. */
      return new Promise((resolve, reject) => {
        const task = __tilefinchRealmGlobal.__tilefinchScheduleTask(
          () => resolve(new PermissionStatus(platformStatusToken, name)), 0);
        if (task === 0)
          reject(new DOMException(
            "Permission query task quota exceeded", "QuotaExceededError"));
      });
    }
  }
  /* Native code calls this at a task boundary after a microphone decision
     changed (captured, then deleted before author code). */
  globalThis.__tilefinchPermissionsChanged = () => {
    for (const status of microphoneStatuses) {
      const state = trustedWeakMapGet(permissionStatusStates, status),
        next = permissionState("microphone");
      if (state.state === next) continue;
      state.state = next;
      status.dispatchEvent(browserEvent(EventInterface, "change"));
    }
  };
  for (const [constructor, tag] of [
    [NetworkInformation, "NetworkInformation"],
    [BatteryManager, "BatteryManager"],
    [Permissions, "Permissions"],
    [PermissionStatus, "PermissionStatus"],
  ]) {
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: tag,
    });
    for (const key of Reflect.ownKeys(constructor.prototype)) {
      if (key === "constructor" || key === Symbol.toStringTag) continue;
      const descriptor = Object.getOwnPropertyDescriptor(
        constructor.prototype,
        key,
      );
      if (descriptor)
        Object.defineProperty(constructor.prototype, key, {
          ...descriptor,
          enumerable: true,
        });
    }
  }
  Object.defineProperties(Navigator.prototype, {
    connection: {
      configurable: true,
      enumerable: true,
      get() {
        if (!navigatorBrand(this))
          throw new TypeError("Illegal invocation");
        if (networkInformation === null)
          networkInformation = new NetworkInformation(platformStatusToken);
        return networkInformation;
      },
    },
    getBattery: {
      configurable: true,
      enumerable: true,
      value: function getBattery() {
        if (!navigatorBrand(this))
          return trustedPromiseReject(new TypeError("Illegal invocation"));
        if (batteryManager === null)
          batteryManager = new BatteryManager(platformStatusToken);
        /* The Battery Status API stores one promise on each Navigator.  This
           identity is observable and prevents repeated probes from creating
           needless reactions and allocations on the PSP. */
        if (batteryPromise === null)
          batteryPromise = trustedPromiseResolve(batteryManager);
        return batteryPromise;
      },
    },
    permissions: {
      configurable: true,
      enumerable: true,
      get() {
        if (!navigatorBrand(this))
          throw new TypeError("Illegal invocation");
        if (permissions === null)
          permissions = new Permissions(platformStatusToken);
        return permissions;
      },
    },
  });
  globalThis.NetworkInformation = NetworkInformation;
  globalThis.BatteryManager = BatteryManager;
  globalThis.Permissions = Permissions;
  globalThis.PermissionStatus = PermissionStatus;

  /* Trusted Types is useful even without CSP enforcement: its core API is
     always exposed and the wrappers stringify at ordinary DOM sinks. Keep
     each Window/Worker factory independent and cap its policy bookkeeping so
     author code cannot turn policy names into unbounded PSP heap retention. */
  const createTrustedTypesRealm = () => {
    const valueStates = new TrustedWeakMap(), policyStates = new TrustedWeakMap(),
      factoryStates = new TrustedWeakMap(), constructionToken = {}, policyLimit = 32,
      valueState = (value, type) => {
        const state = trustedWeakMapGet(valueStates, value);
        if (!state || (type && state.type !== type))
          throw new TrustedTypeError("Illegal invocation");
        return state;
      },
      makeValue = (Type, type, value) => {
        const result = trustedObjectCreate(Type.prototype);
        trustedWeakMapSet(valueStates, result, {
          type, value: TrustedString(value),
        });
        return result;
      };
    class TrustedHTML {
      constructor(token) {
        if (token !== constructionToken) throw new TrustedTypeError("Illegal constructor");
      }
      toString() { return valueState(this, "TrustedHTML").value; }
      toJSON() { return valueState(this, "TrustedHTML").value; }
    }
    class TrustedScript {
      constructor(token) {
        if (token !== constructionToken) throw new TrustedTypeError("Illegal constructor");
      }
      toString() { return valueState(this, "TrustedScript").value; }
      toJSON() { return valueState(this, "TrustedScript").value; }
    }
    class TrustedScriptURL {
      constructor(token) {
        if (token !== constructionToken) throw new TrustedTypeError("Illegal constructor");
      }
      toString() { return valueState(this, "TrustedScriptURL").value; }
      toJSON() { return valueState(this, "TrustedScriptURL").value; }
    }
    const trustedTypesByName = {
        createHTML: [TrustedHTML, "TrustedHTML"],
        createScript: [TrustedScript, "TrustedScript"],
        createScriptURL: [TrustedScriptURL, "TrustedScriptURL"],
      },
      createPolicyValue = (policy, functionName, input, extra) => {
        const state = trustedWeakMapGet(policyStates, policy);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        const callback = state.options[functionName];
        if (callback === null)
          throw new TrustedTypeError(functionName + " callback is not configured");
        const converted = TrustedString(input),
          output = trustedFunctionApply(callback, undefined, [converted, ...extra]),
          [Type, type] = trustedTypesByName[functionName];
        return makeValue(Type, type, output == null ? "" : output);
      },
      trustedScriptForEval = value => {
        const state = trustedWeakMapGet(valueStates, value);
        return state?.type === "TrustedScript" ? state.value : undefined;
      };
    class TrustedTypePolicy {
      constructor(token) {
        if (token !== constructionToken) throw new TrustedTypeError("Illegal constructor");
      }
      get name() {
        const state = trustedWeakMapGet(policyStates, this);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        return state.name;
      }
      createHTML(input, ...args) {
        return createPolicyValue(this, "createHTML", input, args);
      }
      createScript(input, ...args) {
        return createPolicyValue(this, "createScript", input, args);
      }
      createScriptURL(input, ...args) {
        return createPolicyValue(this, "createScriptURL", input, args);
      }
    }
    class TrustedTypePolicyFactory {
      constructor(token) {
        if (token !== constructionToken) throw new TrustedTypeError("Illegal constructor");
      }
      createPolicy(name, options = {}) {
        const state = trustedWeakMapGet(factoryStates, this);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        name = TrustedString(name);
        options = TrustedObject(options);
        const callbacks = trustedObjectCreate(null);
        for (const key of ["createHTML", "createScript", "createScriptURL"]) {
          const callback = options[key];
          if (callback != null && typeof callback !== "function")
            throw new TrustedTypeError(key + " must be callable");
          callbacks[key] = callback == null ? null : callback;
        }
        if (name === "default" && state.defaultPolicy !== null)
          throw new TrustedTypeError("default policy already exists");
        if (state.policyCount >= policyLimit)
          throw new TrustedRangeError("trusted type policy quota exceeded");
        const policy = trustedObjectCreate(TrustedTypePolicy.prototype);
        trustedWeakMapSet(policyStates, policy, { name, options: callbacks });
        state.policyCount++;
        if (name === "default") state.defaultPolicy = policy;
        return policy;
      }
      isHTML(value) {
        return trustedWeakMapGet(valueStates, value)?.type === "TrustedHTML";
      }
      isScript(value) {
        return trustedWeakMapGet(valueStates, value)?.type === "TrustedScript";
      }
      isScriptURL(value) {
        return trustedWeakMapGet(valueStates, value)?.type === "TrustedScriptURL";
      }
      get emptyHTML() {
        const state = trustedWeakMapGet(factoryStates, this);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        return state.emptyHTML;
      }
      get emptyScript() {
        const state = trustedWeakMapGet(factoryStates, this);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        return state.emptyScript;
      }
      get defaultPolicy() {
        const state = trustedWeakMapGet(factoryStates, this);
        if (!state) throw new TrustedTypeError("Illegal invocation");
        return state.defaultPolicy;
      }
      getPropertyType(tagName, property, elementNamespace = "") {
        if (!trustedWeakMapHas(factoryStates, this))
          throw new TrustedTypeError("Illegal invocation");
        const tag = trustedStringLower(TrustedString(tagName)),
          name = TrustedString(property);
        void TrustedString(elementNamespace ?? "");
        if (name === "innerHTML" || name === "outerHTML") return "TrustedHTML";
        if (tag === "iframe" && name === "srcdoc") return "TrustedHTML";
        if (tag === "script" && name === "src") return "TrustedScriptURL";
        if (tag === "script" && (name === "innerText" || name === "text"
                                 || name === "textContent"))
          return "TrustedScript";
        return null;
      }
      getAttributeType(tagName, attribute, elementNamespace = "",
                       attributeNamespace = "") {
        if (!trustedWeakMapHas(factoryStates, this))
          throw new TrustedTypeError("Illegal invocation");
        const tag = trustedStringLower(TrustedString(tagName)),
          name = trustedStringLower(TrustedString(attribute)),
          elementNS = TrustedString(elementNamespace ?? ""),
          attributeNS = TrustedString(attributeNamespace ?? "");
        if (!attributeNS && /^on[a-z]/.test(name)) return "TrustedScript";
        if (!attributeNS && tag === "iframe" && name === "srcdoc")
          return "TrustedHTML";
        if (!attributeNS && tag === "script" && name === "src")
          return "TrustedScriptURL";
        if (tag === "script" && name === "href"
            && (elementNS === "http://www.w3.org/2000/svg"
                || attributeNS === "http://www.w3.org/1999/xlink"))
          return "TrustedScriptURL";
        return null;
      }
    }
    for (const [Type, tag] of [
      [TrustedHTML, "TrustedHTML"], [TrustedScript, "TrustedScript"],
      [TrustedScriptURL, "TrustedScriptURL"],
      [TrustedTypePolicy, "TrustedTypePolicy"],
      [TrustedTypePolicyFactory, "TrustedTypePolicyFactory"],
    ])
      trustedDefineProperty(Type.prototype, Symbol.toStringTag, {
        configurable: true, value: tag,
      });
    const factory = trustedObjectCreate(TrustedTypePolicyFactory.prototype),
      state = { defaultPolicy: null, policyCount: 0 };
    state.emptyHTML = makeValue(TrustedHTML, "TrustedHTML", "");
    state.emptyScript = makeValue(TrustedScript, "TrustedScript", "");
    trustedWeakMapSet(factoryStates, factory, state);
    return trustedObjectFreeze({
      TrustedHTML, TrustedScript, TrustedScriptURL,
      TrustedTypePolicy, TrustedTypePolicyFactory, trustedTypes: factory,
      __tilefinchTrustedScriptForEval: trustedScriptForEval,
    });
  };
  const trustedTypesRealm = createTrustedTypesRealm();
  const defineTrustedTypesGlobal = (name) =>
    trustedDefineProperty(globalThis, name, {
      configurable: true,
      enumerable: false,
      writable: false,
      value: trustedTypesRealm[name],
    });
  defineTrustedTypesGlobal("TrustedHTML");
  defineTrustedTypesGlobal("TrustedScript");
  defineTrustedTypesGlobal("TrustedScriptURL");
  defineTrustedTypesGlobal("TrustedTypePolicy");
  defineTrustedTypesGlobal("TrustedTypePolicyFactory");
  defineTrustedTypesGlobal("trustedTypes");
  trustedDefineProperty(globalThis, "__tilefinchTrustedScriptForEval", {
    configurable: false, enumerable: false, writable: false,
    value: trustedTypesRealm.__tilefinchTrustedScriptForEval,
  });
  trustedDefineProperty(globalThis, "__tilefinchCreateTrustedTypesRealm", {
    configurable: false, enumerable: false, writable: false,
    value: createTrustedTypesRealm,
  });
  /* Number and BigInt toLocaleString and Date's toLocale*String format
     through intl.js, which loads on first use and hands its formatters to
     this closure once through a frozen hook, so a page's own Intl global
     never formats them. A date formatted without locales or options keeps
     the engine's (or the deterministic clock's) text and loads nothing. */
  {
    let formatNumber = null, formatDate = null;
    trustedDefineProperty(globalThis, "__tilefinchRegisterLocaleFormat", {
      configurable: false, enumerable: false, writable: false,
      value: (number, date) => {
        if (formatNumber) return;
        formatNumber = number;
        formatDate = date;
      },
    });
    const loadIntl = TrustedObject.getOwnPropertyDescriptor(globalThis, "Intl")
        ?.get,
      ready = () => {
        if (!formatNumber && loadIntl)
          try {
            trustedFunctionApply(loadIntl, globalThis, []);
          } catch (_) {
            /* Formatting falls back to the engine. */
          }
        return formatNumber !== null;
      },
      dateProto = Date.prototype,
      thisTime = dateProto.getTime,
      method = (target, name, value) =>
        trustedDefineProperty(target, name, {
          configurable: true, enumerable: false, writable: true, value,
        });
    for (const proto of [Number.prototype, BigInt.prototype]) {
      const valueOf = proto.valueOf, engine = proto.toLocaleString;
      method(proto, "toLocaleString", {
        toLocaleString() {
          const value = trustedFunctionApply(valueOf, this, []);
          return ready() ? formatNumber(value, arguments[0], arguments[1])
            : trustedFunctionApply(engine, value, []);
        },
      }.toLocaleString);
    }
    for (const [name, required, defaults] of [
      ["toLocaleString", "any", "all"],
      ["toLocaleDateString", "date", "date"],
      ["toLocaleTimeString", "time", "time"],
    ]) {
      const engine = dateProto[name];
      method(dateProto, name, {
        [name]() {
          const time = trustedFunctionApply(thisTime, this, []);
          return (arguments[0] === undefined && arguments[1] === undefined) ||
              !ready()
            ? trustedFunctionApply(engine, this, arguments)
            : formatDate(time, arguments[0], arguments[1], required, defaults);
        },
      }[name]);
    }
  }
})();
