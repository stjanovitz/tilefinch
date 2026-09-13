(() => {
  const boundedAncestorPath = globalThis.__tilefinchBoundedAncestorPath,
    ancestorLimit = globalThis.__tilefinchAncestorLimit,
    tilefinchBlobBytes = globalThis.__tilefinchBlobBytes,
    tilefinchBlobForURL = globalThis.__tilefinchBlobForURL,
    tilefinchBlobType = Object.getOwnPropertyDescriptor(
      Blob.prototype,
      "type",
    ).get,
    trustedFunctionApply = Function.call.bind(Function.prototype.apply),
    TrustedObject = Object,
    TrustedString = String,
    TrustedTypeError = TypeError,
    TrustedRangeError = RangeError,
    TrustedWeakMap = WeakMap,
    TrustedWeakRef = WeakRef,
    TrustedFinalizationRegistry = FinalizationRegistry,
    trustedWeakMapGet = Function.call.bind(WeakMap.prototype.get),
    trustedWeakMapSet = Function.call.bind(WeakMap.prototype.set),
    trustedWeakMapHas = Function.call.bind(WeakMap.prototype.has),
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
    copyArrayBufferBytes = (buffer, byteOffset = 0, byteLength) => {
      const bufferLength = trustedArrayBufferByteLength(buffer);
      if (byteLength === undefined) byteLength = bufferLength - byteOffset;
      const source = new Uint8ArrayCtor(buffer, byteOffset, byteLength),
        copy = new Uint8ArrayCtor(byteLength);
      trustedUint8ArraySet(copy, source, 0);
      return copy;
    },
    copyArrayBufferViewBytes = (view) => {
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
      return copyArrayBufferBytes(buffer, byteOffset, byteLength);
    };
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
      dispatchVisibility(new globalThis.Event("visibilitychange"));
      return true;
    };
    document.onDOMContentLoaded = null;
    document.exitPointerLock = function () {};
  }
  {
    const reflect = (name) => ({
      configurable: true,
      enumerable: true,
      get() {
        return this.getAttribute(name) || "";
      },
      set(value) {
        this.setAttribute(name, String(value));
      },
    });
    Object.defineProperties(HTMLLinkElement.prototype, {
      as: reflect("as"),
      integrity: reflect("integrity"),
      referrerPolicy: reflect("referrerpolicy"),
      relList: {
        configurable: true,
        enumerable: true,
        get() {
          const owner = this,
            tokens = () =>
              String(owner.getAttribute("rel") || "")
                .trim()
                .split(/\s+/)
                .filter(Boolean);
          return {
            get length() {
              return tokens().length;
            },
            item(index) {
              return tokens()[Math.floor(Number(index) || 0)] ?? null;
            },
            contains(token) {
              return tokens().includes(String(token));
            },
            supports(token) {
              return [
                "stylesheet",
                "preload",
                "modulepreload",
                "icon",
                "alternate",
                "canonical",
                "manifest",
                "dns-prefetch",
                "preconnect",
                "prefetch",
              ].includes(String(token).toLowerCase());
            },
            toString() {
              return tokens().join(" ");
            },
            [Symbol.iterator]() {
              return tokens()[Symbol.iterator]();
            },
          };
        },
      },
    });
    Object.defineProperty(
      HTMLScriptElement.prototype,
      "integrity",
      reflect("integrity"),
    );
  }
  {
    const anchorLocation = (node) => {
      try {
        return new URL(node.getAttribute("href") || "", location.href);
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
    HTMLAnchorElement.prototype.toString = function () {
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
      if (storageAvailable) storageRemove(local, key);
      else storageFallback[local ? 1 : 0].delete(key);
    }
    clear() {
      const local = storageKinds.get(this);
      if (storageAvailable) storageClear(local);
      else storageFallback[local ? 1 : 0].clear();
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
    };
  globalThis.Event = class Event {
    constructor(type, options = {}) {
      if (arguments.length < 1)
        throw new TypeError("Event type is required");
      eventStateSet(this, {
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
      });
      Object.defineProperty(this, "isTrusted", {
        value: false,
        configurable: true,
      });
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
      state.stopped = false;
      state.immediateStopped = false;
    }
  };
  const eventField = (name, convert, writable = true) => ({
    configurable: true,
    enumerable: true,
    get() { return eventState(this)[name]; },
    set: writable ? function (value) {
      eventState(this)[name] = convert(value);
    } : undefined,
  });
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
      get() {
        return !this.defaultPrevented;
      },
      set(value) {
        if (!value) this.preventDefault();
      },
    },
    cancelBubble: {
      configurable: true,
      get() {
        return !!this.__stopped;
      },
      set(value) {
        if (value) this.stopPropagation();
      },
    },
    srcElement: {
      configurable: true,
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
    Object.defineProperty(Event, name, { value });
    Object.defineProperty(Event.prototype, name, { value });
  }
  const createEvent = (interfaceName) => {
    const name = String(interfaceName || ""),
      Constructor =
        name === "CustomEvent"
          ? CustomEvent
          : name === "ErrorEvent"
            ? ErrorEvent
          : name === "CompositionEvent"
            ? CompositionEvent
            : name === "FocusEvent"
              ? FocusEvent
              : name === "KeyboardEvent"
                ? KeyboardEvent
                : name === "MouseEvent" || name === "MouseEvents"
                  ? MouseEvent
                  : name === "UIEvent" || name === "UIEvents"
                    ? UIEvent
                    : Event,
      event = new Constructor("");
    event.__initialized = false;
    return event;
  };
  document.createEvent = createEvent;
  Document.prototype.createEvent = createEvent;
  globalThis.__tilefinchTrustedEvent = (event) => {
    Object.defineProperty(event, "isTrusted", {
      value: true,
      configurable: true,
    });
    return event;
  };
  globalThis.CustomEvent = class CustomEvent extends Event {
    constructor(type, options = {}) {
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
      super(type, options);
      this.view = options.view ?? globalThis;
      this.detail = Number(options.detail) || 0;
      this.which = Number(options.which) || 0;
    }
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
      this.relatedTarget = options.relatedTarget ?? null;
      modifierFields(this, options);
    }
  };
  globalThis.PointerEvent = class PointerEvent extends MouseEvent {
    constructor(type, options = {}) {
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
  };
  for (const [name, value] of Object.entries({
    DOM_KEY_LOCATION_STANDARD: 0,
    DOM_KEY_LOCATION_LEFT: 1,
    DOM_KEY_LOCATION_RIGHT: 2,
    DOM_KEY_LOCATION_NUMPAD: 3,
  })) {
    Object.defineProperty(KeyboardEvent, name, { value });
    Object.defineProperty(KeyboardEvent.prototype, name, { value });
  }
  globalThis.InputEvent = class InputEvent extends UIEvent {
    constructor(type, options = {}) {
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
  globalThis.FocusEvent = class FocusEvent extends UIEvent {
    constructor(type, options = {}) {
      super(type, options);
      this.relatedTarget = options.relatedTarget ?? null;
    }
  };
  globalThis.CompositionEvent = class CompositionEvent extends UIEvent {
    constructor(type, options = {}) {
      super(type, options);
      this.data = String(options.data || "");
    }
  };
  globalThis.SubmitEvent = class SubmitEvent extends Event {
    constructor(type, options = {}) {
      super(type, options);
      this.submitter = options.submitter ?? null;
    }
  };
  globalThis.ToggleEvent = class ToggleEvent extends Event {
    constructor(type, options = {}) {
      super(type, options);
      this.oldState = String(options.oldState || "closed");
      this.newState = String(options.newState || "closed");
    }
  };
  globalThis.ProgressEvent = class ProgressEvent extends Event {
    constructor(type, options = {}) {
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
  const messageEventStates = new WeakMap(),
    messageEventStateGet = messageEventStates.get.bind(messageEventStates),
    messageEventStateSet = messageEventStates.set.bind(messageEventStates),
    messageEventPorts = (ports) => {
      const result = [];
      if (ports != null)
        for (const port of ports) {
          if (result.length >= 16) break;
          result.push(port);
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
      const event = new MessageEvent(type);
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
  globalThis.CloseEvent = class CloseEvent extends Event {
    constructor(type, options = {}) {
      super(type, options);
      this.wasClean = !!options.wasClean;
      this.code = Number(options.code) || 0;
      this.reason = String(options.reason || "");
    }
  };
  globalThis.FileReader = class FileReader extends EventTarget {
    constructor() {
      super();
      this.readyState = 0;
      this.result = null;
      this.error = null;
      this._task = 0;
      for (const name of [
        "onloadstart",
        "onprogress",
        "onload",
        "onabort",
        "onerror",
        "onloadend",
      ])
        this[name] = null;
    }
    _emit(type, loaded = 0, total = 0) {
      const event = __tilefinchTrustedEvent(
        new ProgressEvent(type, {
          lengthComputable: true,
          loaded,
          total,
        }),
      );
      this.dispatchEvent(event);
      const handler = this["on" + type];
      if (typeof handler === "function")
        globalThis.__tilefinchRunTask(
          "file-reader:" + String(type),
          handler,
          this,
          [event],
        );
    }
    _read(blob, convert) {
      if (!(blob instanceof Blob))
        throw new TypeError("FileReader input must be a Blob");
      if (this.readyState === 1)
        throw new DOMException("A read is already active", "InvalidStateError");
      this.readyState = 1;
      this.result = null;
      this.error = null;
      this._emit("loadstart", 0, blob.size);
      this._task = setTimeout(() => {
        if (this.readyState !== 1) return;
        try {
          this.result = convert(tilefinchBlobBytes(blob));
          this.readyState = 2;
          this._task = 0;
          this._emit("progress", blob.size, blob.size);
          this._emit("load", blob.size, blob.size);
          this._emit("loadend", blob.size, blob.size);
        } catch (error) {
          this.error = error;
          this.result = null;
          this.readyState = 2;
          this._task = 0;
          this._emit("error", 0, blob.size);
          this._emit("loadend", 0, blob.size);
        }
      }, 0);
    }
    readAsArrayBuffer(blob) {
      this._read(blob, (bytes) => bytes.slice().buffer);
    }
    readAsText(blob, encoding = "utf-8") {
      this._read(blob, (bytes) =>
        new TextDecoder(String(encoding || "utf-8")).decode(bytes),
      );
    }
    readAsDataURL(blob) {
      this._read(blob, (bytes) => {
        let binary = "";
        for (let at = 0; at < bytes.length; at += 4096)
          binary += String.fromCharCode(...bytes.slice(at, at + 4096));
        return (
          "data:" +
          (blob.type || "application/octet-stream") +
          ";base64," +
          btoa(binary)
        );
      });
    }
    abort() {
      if (this.readyState !== 1) {
        this.result = null;
        return;
      }
      clearTimeout(this._task);
      this._task = 0;
      this.readyState = 2;
      this.result = null;
      this.error = new DOMException("The read was aborted", "AbortError");
      this._emit("abort");
      this._emit("loadend");
    }
  };
  for (const [name, value] of Object.entries({
    EMPTY: 0,
    LOADING: 1,
    DONE: 2,
  })) {
    Object.defineProperty(FileReader, name, { value });
    Object.defineProperty(FileReader.prototype, name, { value });
  }
  /* Worker teardown must cancel an owner-backed FileReader without emitting
     abort/loadend into a realm that has already been retired. */
  Object.defineProperty(globalThis, "__tilefinchAbortFileReaderForWorker", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(reader) {
      if (!(reader instanceof FileReader)) return false;
      if (reader._task) __tilefinchCancelTimer(reader._task);
      reader._task = 0;
      reader.readyState = FileReader.DONE;
      reader.result = null;
      reader.error = new DOMException("Worker is terminated", "AbortError");
      return true;
    },
  });
  Object.defineProperty(globalThis, "__tilefinchBlobTextForWorker", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(blob, encoding = "utf-8") {
      return new TextDecoder(String(encoding || "utf-8")).decode(
        tilefinchBlobBytes(blob));
    },
  });
  const markFocus = (target, on) => {
    if (!target?.__handle) return;
    if (on) __tilefinchSetAttribute(target.__handle, "data-tilefinch-focus", "");
    else __tilefinchRemoveAttribute(target.__handle, "data-tilefinch-focus");
  };
  const isFocusable = (target) => {
    if (
      target.disabled ||
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
  let focusFixupPending = false;
  globalThis.__tilefinchQueueFocusFixup = () => {
    const active = document.__activeElement;
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
      ((callback) => requestAnimationFrame(callback));
    schedule(() => {
      focusFixupPending = false;
      const current = document.__activeElement;
      if (
        current &&
        current !== document.body &&
        !isFocusable(current)
      )
        document.__activeElement = document.body;
    });
  };
  const collapseFocusSelection = (target) => {
    const type = String(target.type || "").toLowerCase();
    if (
      target instanceof HTMLInputElement &&
      (type === "text" || type === "number")
    ) {
      const parent = target.parentNode,
        offset = Array.from(parent?.childNodes || []).indexOf(target);
      if (parent && offset >= 0) getSelection().collapse(parent, offset);
    } else if (target.isContentEditable && target.firstChild) {
      getSelection().collapse(target.firstChild, 0);
    }
  };
  const focusElement = function () {
    const shadow = globalThis.__tilefinchShadowRootForHost?.(this);
    if (shadow?.delegatesFocus) {
      let delegated = null;
      for (const candidate of Array.from(shadow.querySelectorAll("*")).slice(
        0,
        128,
      ))
        if (isFocusable(candidate)) {
          delegated = candidate;
          break;
        }
      if (delegated && delegated !== this) {
        delegated.focus();
        return;
      }
    }
    if (!isFocusable(this) || document.__activeElement === this) return;
    const previous = document.__activeElement || document.body;
    if (!globalThis.__tilefinchFocusEventsObserved?.()) {
      if (previous && previous !== document.body) markFocus(previous, false);
      document.__activeElement = this;
      collapseFocusSelection(this);
      markFocus(this, true);
      return;
    }
    const fire = (target, type, options) =>
      target.dispatchEvent(
        __tilefinchTrustedEvent(new FocusEvent(type, options)),
      );
    if (previous && previous !== document.body) {
      markFocus(previous, false);
      document.__activeElement = document.body;
      fire(previous, "blur", { relatedTarget: this });
      fire(previous, "focusout", { bubbles: true, relatedTarget: this });
    }
    document.__activeElement = this;
    collapseFocusSelection(this);
    markFocus(this, true);
    fire(this, "focus", { relatedTarget: previous });
    fire(this, "focusin", { bubbles: true, relatedTarget: previous });
  };
  const blurElement = function () {
    if (document.__activeElement !== this) return;
    const next = document.body,
      fire = (type, options) =>
        this.dispatchEvent(__tilefinchTrustedEvent(new FocusEvent(type, options)));
    markFocus(this, false);
    fire("blur", { relatedTarget: next });
    fire("focusout", { bubbles: true, relatedTarget: next });
    document.__activeElement = next;
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
  HTMLDialogElement.prototype.show = function () {
    if (!this.isConnected)
      throw new DOMException("Dialog is not connected", "InvalidStateError");
    if (this.open) return;
    this.removeAttribute("data-tilefinch-modal");
    this.setAttribute("open", "");
  };
  HTMLDialogElement.prototype.showModal = function () {
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
  HTMLDialogElement.prototype.close = function (value) {
    if (!this.open) return;
    if (value !== undefined) this.returnValue = value;
    this.removeAttribute("open");
    this.removeAttribute("data-tilefinch-modal");
    this.dispatchEvent(__tilefinchTrustedEvent(new Event("close")));
  };
  HTMLDialogElement.prototype.requestClose = function (value) {
    if (!this.open) return;
    const event = __tilefinchTrustedEvent(
      new Event("cancel", { cancelable: true }),
    );
    if (this.dispatchEvent(event)) this.close(value);
  };
  Object.defineProperty(HTMLDetailsElement.prototype, "open", {
    get() {
      return this.hasAttribute("open");
    },
    set(value) {
      this.toggleAttribute("open", !!value);
    },
    configurable: true,
  });
  globalThis.__tilefinchDetailsDefault = (target) => {
    if (!(target instanceof HTMLSummaryElement)) return false;
    const details = target.parentElement;
    if (!(details instanceof HTMLDetailsElement)) return false;
    const first = details.children.find(
      (child) => child instanceof HTMLSummaryElement,
    );
    if (first !== target) return false;
    const wasOpen = details.open;
    details.open = !wasOpen;
    details.dispatchEvent(
      __tilefinchTrustedEvent(
        new ToggleEvent("toggle", {
          oldState: wasOpen ? "open" : "closed",
          newState: wasOpen ? "closed" : "open",
        }),
      ),
    );
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
          : Array.from(this.querySelectorAll("*"));
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
      const options = target.options,
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
      const old = target.checked;
      target.checked = !old;
      return {
        kind: "checkbox",
        target,
        changed: true,
        states: [[target, old]],
      };
    }
    if (type !== "radio") return null;
    if (target.checked)
      return {
        kind: "radio",
        target,
        changed: false,
        states: [[target, true]],
      };
    const form = target.closest("form"),
      name = target.name,
      peers = name
        ? Array.from(document.querySelectorAll("input"))
            .filter(
              (item) =>
                item instanceof HTMLInputElement &&
                String(item.type).toLowerCase() === "radio" &&
                item.name === name &&
                item.closest("form") === form,
            )
        : [target],
      states = peers.map((item) => [item, item.checked]);
    for (const item of peers) item.checked = item === target;
    return { kind: "radio", target, changed: true, states };
  };
  globalThis.__tilefinchFinishControlDefault = (state, accepted) => {
    if (!state) return false;
    if (!accepted) {
      if (state.kind === "select")
        for (const [option, selected] of state.states)
          option.selected = selected;
      else if (state.kind === "range") state.target.value = state.value;
      else
        for (const [item, checked] of state.states) item.checked = checked;
      return true;
    }
    if (state.kind === "reset") {
      state.target.closest("form")?.reset();
      return true;
    }
    if (state.changed) {
      state.target.dispatchEvent(
        __tilefinchTrustedEvent(new Event("input", { bubbles: true })),
      );
      state.target.dispatchEvent(
        __tilefinchTrustedEvent(new Event("change", { bubbles: true })),
      );
    }
    return true;
  };
  Object.defineProperty(HTMLElement.prototype, "popover", {
    get() {
      return this.getAttribute("popover");
    },
    set(value) {
      value === null
        ? this.removeAttribute("popover")
        : this.setAttribute("popover", String(value));
    },
    configurable: true,
  });
  const setPopoverState = (target, open) => {
    if (!target.hasAttribute("popover"))
      throw new DOMException("Element is not a popover", "NotSupportedError");
    const current = target.hasAttribute("data-tilefinch-popover-open");
    if (current === open) return current;
    const oldState = current ? "open" : "closed",
      newState = open ? "open" : "closed",
      before = __tilefinchTrustedEvent(
        new ToggleEvent("beforetoggle", {
          cancelable: open,
          oldState,
          newState,
        }),
      );
    if (!target.dispatchEvent(before)) return current;
    if (open) target.setAttribute("data-tilefinch-popover-open", "");
    else target.removeAttribute("data-tilefinch-popover-open");
    target.dispatchEvent(
      __tilefinchTrustedEvent(new ToggleEvent("toggle", { oldState, newState })),
    );
    return open;
  };
  HTMLElement.prototype.showPopover = function () {
    setPopoverState(this, true);
  };
  HTMLElement.prototype.hidePopover = function () {
    setPopoverState(this, false);
  };
  HTMLElement.prototype.togglePopover = function (force) {
    const open =
      force === undefined
        ? !this.hasAttribute("data-tilefinch-popover-open")
        : !!force;
    return setPopoverState(this, open);
  };
  globalThis.DOMException = class DOMException extends Error {
    constructor(message = "", name = "Error") {
      super(String(message));
      this.name = String(name);
      this.code =
        {
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
    const sameNode = (left, right) =>
        left === right ||
        (!!left &&
          !!right &&
          left.__handle !== undefined &&
          left.__handle === right.__handle),
      parentKind = (node) => Number(node?.nodeType),
      parentOf = (node) =>
        node?.__tilefinchDetachedParent || node?.parentNode || null,
      childrenOf = (node) => Array.from(node?.childNodes || []),
      isAncestor = (node, parent) =>
        boundedAncestorPath(parent, parentOf).some((at) =>
          sameNode(at, node),
        );
    const validatePreInsert = (
      parent,
      node,
      child,
      replacingAll = false,
    ) => {
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
        ![
          Node.DOCUMENT_FRAGMENT_NODE,
          Node.DOCUMENT_TYPE_NODE,
          Node.ELEMENT_NODE,
          Node.TEXT_NODE,
          Node.CDATA_SECTION_NODE,
          Node.PROCESSING_INSTRUCTION_NODE,
          Node.COMMENT_NODE,
        ].includes(nodeType)
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
      const payload =
          nodeType === Node.DOCUMENT_FRAGMENT_NODE ? childrenOf(node) : [node],
        current = replacingAll
          ? []
          : childrenOf(parent).filter(
              (item) => !payload.some((value) => sameNode(item, value)),
            );
      let reference = child;
      if (payload.some((value) => sameNode(value, reference))) {
        reference = null;
        for (const item of childrenOf(parent)) {
          if (payload.some((value) => sameNode(value, item))) continue;
          const original = childrenOf(parent),
            childAt = original.findIndex((value) => sameNode(value, child));
          if (original.indexOf(item) > childAt) {
            reference = item;
            break;
          }
        }
      }
      let at =
        reference === null || reference === undefined
          ? current.length
          : current.findIndex((item) => sameNode(item, reference));
      if (at < 0) at = current.length;
      current.splice(at, 0, ...payload);
      let elements = 0,
        doctypes = 0,
        elementAt = -1,
        doctypeAt = -1;
      for (let index = 0; index < current.length; index++) {
        const type = parentKind(current[index]);
        if (
          type === Node.TEXT_NODE ||
          type === Node.DOCUMENT_NODE ||
          type === Node.DOCUMENT_FRAGMENT_NODE
        )
          throw new DOMException(
            "Invalid document child",
            "HierarchyRequestError",
          );
        if (type === Node.ELEMENT_NODE) {
          elements++;
          elementAt = index;
        } else if (type === Node.DOCUMENT_TYPE_NODE) {
          doctypes++;
          doctypeAt = index;
        }
      }
      if (
        elements > 1 ||
        doctypes > 1 ||
        (doctypeAt >= 0 && elementAt >= 0 && doctypeAt > elementAt)
      )
        throw new DOMException(
          "Invalid document child order",
          "HierarchyRequestError",
        );
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
      const owner =
          parent.nodeType === Node.DOCUMENT_NODE
            ? parent
            : parent.ownerDocument || document,
        nodes = values.map((value) =>
          value instanceof Node ? value : owner.createTextNode(String(value)),
        );
      if (nodes.length === 0) return;
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
        else
          Object.defineProperty(node, "__tilefinchAdoptedOwner", {
            configurable: true,
            writable: true,
            value: owner,
          });
        for (const child of node.childNodes || []) adoptTreeOwner(child, owner);
      },
      rawDetachedInsert = (parent, node, child) => {
        if (node === child) return node;
        const values =
          node.nodeType === Node.DOCUMENT_FRAGMENT_NODE
            ? childrenOf(node)
            : [node],
          removals = values.map((value) => ({
            value,
            parent: parentOf(value),
            connected: detachedTreeConnected(value),
            owner: value.ownerDocument || null,
            previousSibling: value.previousSibling,
            nextSibling: value.nextSibling,
          }));
        for (const removal of removals)
          if (removal.connected)
            globalThis.__tilefinchCustomElementDisconnected?.(
              removal.value,
            );
        for (const value of values) {
          const old = parentOf(value);
          if (Array.isArray(old?.__detachedChildren)) {
            const oldAt = old.__detachedChildren.indexOf(value);
            if (oldAt >= 0) old.__detachedChildren.splice(oldAt, 1);
          }
          if (value.__detachedParent !== undefined)
            value.__detachedParent = null;
          value.__tilefinchDetachedParent = null;
        }
        let at =
          child === null || child === undefined
            ? parent.__detachedChildren.length
            : parent.__detachedChildren.indexOf(child);
        if (at < 0)
          throw new DOMException(
            "Reference node is not a child",
            "NotFoundError",
          );
        parent.__detachedChildren.splice(at, 0, ...values);
        for (const value of values) {
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
            removal = removals.find((item) => item.value === value);
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
            if (Array.isArray(owner?.__detachedChildren))
              owner.removeChild(this);
            else if (typeof this.__tilefinchDetachedRemove === "function")
              this.__tilefinchDetachedRemove.call(this);
          };
        }
        for (const removal of removals)
          if (removal.parent)
            globalThis.__tilefinchNotifyMutation?.(
              removal.parent,
              "childList",
              null,
              [],
              [removal.value],
              null,
              removal.previousSibling,
              removal.nextSibling,
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
          for (const value of values)
            globalThis.__tilefinchCustomElementConnected?.(value, true);
        return node;
      },
      installContainer = (container) => {
        Object.defineProperties(container, {
          addEventListener: {
            configurable: true,
            writable: true,
            value: function addEventListener(type, callback, options = false) {
              return EventTarget.prototype.addEventListener.call(
                this,
                type,
                callback,
                options,
              );
            },
          },
          removeEventListener: {
            configurable: true,
            writable: true,
            value: function removeEventListener(
              type,
              callback,
              options = false,
            ) {
              return EventTarget.prototype.removeEventListener.call(
                this,
                type,
                callback,
                options,
              );
            },
          },
          dispatchEvent: {
            configurable: true,
            writable: true,
            value: function dispatchEvent(event) {
              const path = boundedAncestorPath(
                this,
                (at) => at.__tilefinchDetachedParent || at.parentNode,
              );
              globalThis.__tilefinchPrepareEvent(event, this, path);
              for (
                let at = path.length - 1;
                at >= 1 && !event.__stopped;
                at--
              )
                globalThis.__tilefinchInvokeEventTarget(
                  path[at],
                  event,
                  true,
                  Event.CAPTURING_PHASE,
                );
              if (!event.__stopped) {
                globalThis.__tilefinchInvokeEventTarget(
                  this,
                  event,
                  true,
                  Event.AT_TARGET,
                );
                if (!event.__immediateStopped)
                  globalThis.__tilefinchInvokeEventTarget(
                    this,
                    event,
                    false,
                    Event.AT_TARGET,
                  );
              }
              if (event.bubbles && !event.__stopped)
                for (let at = 1; at < path.length && !event.__stopped; at++)
                  globalThis.__tilefinchInvokeEventTarget(
                    path[at],
                    event,
                    false,
                    Event.BUBBLING_PHASE,
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
              const owner = this.ownerDocument || this,
                nodes = values.map((value) =>
                  value instanceof Node
                    ? value
                    : owner.createTextNode(String(value)),
                ),
                insertion =
                  nodes.length === 1
                    ? nodes[0]
                    : owner.createDocumentFragment(),
                moves = [];
              for (const node of nodes) {
                const candidates =
                  node.nodeType === Node.DOCUMENT_FRAGMENT_NODE
                    ? [...node.childNodes]
                    : [node];
                for (const candidate of candidates)
                  if (candidate.parentNode)
                    moves.push({
                      node: candidate,
                      parent: candidate.parentNode,
                      previousSibling: candidate.previousSibling,
                      nextSibling: candidate.nextSibling,
                    });
              }
              validatePreInsert(this, insertion, null, true);
              globalThis.__tilefinchMutationSuppressed =
                (globalThis.__tilefinchMutationSuppressed || 0) + 1;
              let removed;
              try {
                if (nodes.length !== 1)
                  for (const node of nodes) insertion.appendChild(node);
                removed = [...this.childNodes];
                for (const child of removed) this.removeChild(child);
                if (nodes.length) this.appendChild(insertion);
              } finally {
                globalThis.__tilefinchMutationSuppressed--;
              }
              for (const move of moves)
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
                at = this.__detachedChildren.indexOf(node);
              if (connected)
                globalThis.__tilefinchCustomElementDisconnected?.(node);
              this.__detachedChildren.splice(at, 1);
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
              p.__detachedChildren[p.__detachedChildren.indexOf(this) + 1] ||
              null
            );
          },
        },
        previousSibling: {
          get() {
            const p = this.__detachedParent;
            if (!p) return null;
            return (
              p.__detachedChildren[p.__detachedChildren.indexOf(this) - 1] ||
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
    const originalHTMLDocument = document.implementation.createHTMLDocument,
      originalXMLDocument = document.implementation.createDocument,
      upgradeDocument = (doc) => {
        const upgrade = (node, parent = null) => {
          if (parent)
            Object.defineProperty(node, "__tilefinchDetachedParent", {
              configurable: true,
              writable: true,
              value: parent,
            });
          if (Array.isArray(node?.__detachedChildren)) {
            installContainer(node);
          }
          for (const child of childrenOf(node)) upgrade(child, node);
          return node;
        };
        installContainer(doc);
        for (const child of childrenOf(doc)) upgrade(child, doc);
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
            return (
              this.__detachedChildren.find(
                (node) => node.nodeType === Node.DOCUMENT_TYPE_NODE,
              ) || null
            );
          },
        });
        return doc;
      };
    const originalNewDocument = globalThis.__tilefinchNewDocument;
    if (typeof originalNewDocument === "function")
      globalThis.__tilefinchNewDocument = () =>
        upgradeDocument(originalNewDocument());
    const makeDoctype = (name = "html", publicId = "", systemId = "") => {
      const node = detachedLeaf(null, Node.DOCUMENT_TYPE_NODE, String(name));
      Object.defineProperties(node, {
        name: { value: String(name) },
        publicId: { value: String(publicId) },
        systemId: { value: String(systemId) },
      });
      node.cloneNode = () => makeDoctype(name, publicId, systemId);
      return node;
    };
    document.implementation.createDocumentType = makeDoctype;
    Object.defineProperty(Element.prototype, "outerHTML", {
      configurable: true,
      get() {
        const container = (this.ownerDocument || document).createElement("div");
        container.appendChild(this.cloneNode(true));
        return container.innerHTML;
      },
      set(value) {
        const parent = this.parentNode;
        if (!parent) return;
        const owner = this.ownerDocument || document,
          container = owner.createElement("div");
        container.innerHTML = String(value);
        const added = [...container.childNodes],
          fragment = owner.createDocumentFragment();
        for (const node of added) fragment.appendChild(node);
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
    });
    Document.prototype.cloneNode = function (deep = false) {
      const clone = new Document();
      if (deep)
        for (const child of this.childNodes || [])
          clone.appendChild(child.cloneNode(true));
      return clone;
    };
    document.implementation.createHTMLDocument = (title) => {
      const doc = upgradeDocument(originalHTMLDocument(title)),
        doctype = makeDoctype("html");
      doctype.__detachedOwner = doc;
      rawDetachedInsert(doc, doctype, doc.firstChild);
      return doc;
    };
    document.implementation.createDocument = (
      namespace,
      qualifiedName,
      doctype,
    ) => {
      const doc = upgradeDocument(
        originalXMLDocument(namespace, qualifiedName || "", doctype || null),
      );
      Object.setPrototypeOf(doc, XMLDocument.prototype);
      doc.createAttribute = (name) =>
        __tilefinchCreateAttribute(doc, String(name));
      doc.createAttributeNS = (namespace, name) =>
        __tilefinchCreateAttribute(
          doc,
          String(name),
          namespace === null ? null : String(namespace),
        );
      const cloneXMLDocument = doc.cloneNode;
      doc.cloneNode = (deep = false) => {
        const clone = cloneXMLDocument.call(doc, deep);
        Object.setPrototypeOf(clone, XMLDocument.prototype);
        return clone;
      };
      return doc;
    };
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
        parentNode: {
          configurable: true,
          get() {
            return this.__detachedParent;
          },
        },
        parentElement: { configurable: true, get: () => null },
        previousSibling: { configurable: true, get: () => null },
        nextSibling: {
          configurable: true,
          get() {
            return this.__detachedParent === document
              ? document.documentElement
              : null;
          },
        },
      });
    }
    Object.defineProperty(document, "doctype", {
      configurable: true,
      get() {
        return mainDoctype.__detachedParent === document ? mainDoctype : null;
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
  const defaultAbortReason = () =>
    new DOMException("This operation was aborted", "AbortError"),
    abortSignalToken = Object.freeze({}),
    abortSignalStates = new TrustedWeakMap(),
    abortControllerStates = new TrustedWeakMap(),
    abortSignalState = (signal) => {
      const state = trustedWeakMapGet(abortSignalStates, signal);
      if (!state) throw new TrustedTypeError("Illegal invocation");
      return state;
    },
    abortControllerState = (controller) => {
      const state = trustedWeakMapGet(abortControllerStates, controller);
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
        current.dispatchEvent(new Event("abort"));
        current = next;
      }
    };
  globalThis.AbortSignal = class AbortSignal extends EventTarget {
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
      trustedWeakMapSet(abortSignalStates, this, state);
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
        super.addEventListener("abort", state.onabortListener);
      else if (state.onabort !== null && next === null)
        super.removeEventListener("abort", state.onabortListener);
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
      setTimeout(
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
    constructor() {
      trustedWeakMapSet(abortControllerStates, this, {
        signal: new AbortSignal(abortSignalToken),
      });
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
  const normalizeXhrMethod = (method) => {
    method = String(method);
    if (!isHttpToken(method))
      throw new DOMException("Invalid HTTP method", "SyntaxError");
    if (forbiddenMethod(method))
      throw new DOMException("Forbidden HTTP method", "SecurityError");
    return method;
  };
  const normalizeXhrHeader = (name, value) => {
    name = String(name);
    value = String(value);
    if (!isHttpToken(name) || invalidHeaderValue(value))
      throw new DOMException("Invalid HTTP header", "SyntaxError");
    return [name, value];
  };
  const installXhrValidation = (XHR) => {
    const open = XHR.prototype.open,
      setRequestHeader = XHR.prototype.setRequestHeader;
    XHR.prototype.open = function (method, ...args) {
      return open.call(this, normalizeXhrMethod(method), ...args);
    };
    XHR.prototype.setRequestHeader = function (name, value) {
      [name, value] = normalizeXhrHeader(name, value);
      return setRequestHeader.call(this, name, value);
    };
  };
  const headerStorage = new TrustedWeakMap(),
    headerMap = (headers) => {
      if (!trustedWeakMapHas(headerStorage, headers))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(headerStorage, headers);
    },
    sortedHeaderEntries = (headers) => {
      const entries = [];
      for (const pair of trustedMapEntries(headerMap(headers)))
        entries.push([pair[0], pair[1]]);
      trustedArraySort(entries, (left, right) =>
        left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0);
      return entries;
    };
  class Headers {
    constructor(init = {}) {
      trustedWeakMapSet(headerStorage, this, new Map());
      if (init instanceof Headers) {
        for (const [key, value] of trustedMapEntries(headerMap(init)))
          this.set(key, value);
      } else if (typeof init === "string") {
        for (const line of init.split("\n")) {
          if (!line) continue;
          const at = line.indexOf(":");
          if (at <= 0) throw new TypeError("Invalid HTTP header block");
          this.append(line.slice(0, at), line.slice(at + 1));
        }
      } else if (init !== null
                 && typeof init?.[Symbol.iterator] === "function") {
        let count = 0;
        for (const pair of init) {
          if (++count > 256)
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
          this.append(values[0], values[1]);
        }
      } else if (init && typeof init === "object") {
        const keys = Object.keys(init);
        if (keys.length > 256)
          throw new RangeError("Header entry limit exceeded");
        for (const key of keys) this.set(key, init[key]);
      }
    }
    append(name, value) {
      name = normalizeHeaderName(name);
      value = normalizeHeaderValue(value);
      const map = headerMap(this);
      if (!trustedMapHas(map, name)
          && trustedMapSize(map) >= 256)
        throw new RangeError("Header entry limit exceeded");
      trustedMapSet(
        map,
        name,
        trustedMapHas(map, name) ? trustedMapGet(map, name) + ", " + value : value,
      );
    }
    set(name, value) {
      name = normalizeHeaderName(name);
      const map = headerMap(this);
      if (!trustedMapHas(map, name)
          && trustedMapSize(map) >= 256)
        throw new RangeError("Header entry limit exceeded");
      trustedMapSet(map, name, normalizeHeaderValue(value));
    }
    get(name) {
      return trustedMapGet(headerMap(this), normalizeHeaderName(name)) ?? null;
    }
    has(name) {
      return trustedMapHas(headerMap(this), normalizeHeaderName(name));
    }
    delete(name) {
      trustedMapDelete(headerMap(this), normalizeHeaderName(name));
    }
    forEach(callback, thisArg) {
      if (typeof callback !== "function")
        throw new TrustedTypeError("Headers callback must be a function");
      for (const [key, value] of sortedHeaderEntries(this))
        trustedFunctionApply(callback, thisArg, [value, key, this]);
    }
    entries() {
      return sortedHeaderEntries(this)[Symbol.iterator]();
    }
    keys() {
      const keys = [];
      for (const [key] of sortedHeaderEntries(this)) keys.push(key);
      return keys[Symbol.iterator]();
    }
    values() {
      const values = [];
      for (const [, value] of sortedHeaderEntries(this)) values.push(value);
      return values[Symbol.iterator]();
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
      if (headers.has("content-type")) return;
      const inferredType = inferredRequestBodyContentType(source);
      if (inferredType) headers.set("content-type", inferredType);
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
      const parsed = new URL(value, location.href);
      if (parsed.origin !== location.origin)
        throw new TypeError("Request referrer must be same-origin");
      const href = parsed.href,
        fragment = href.indexOf("#");
      return fragment < 0 ? href : href.slice(0, fragment);
    },
    snapshotRequestBody = (source, headers) => {
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
      if (source instanceof FormData) {
        const entries = [], random = new Uint8ArrayCtor(18);
        let total = 0;
        for (const [name, value] of source) {
          const bytes = value instanceof Blob
            ? tilefinchBlobBytes(value) : encoder.encode(String(value));
          if (bytes.byteLength > requestBodyByteLimit - total)
            throw new RangeError("FormData body exceeds bounded size");
          entries.push({ name: String(name), value, bytes });
          total += bytes.byteLength;
        }
        const contains = (bytes, text) => {
          const needle = encoder.encode(text);
          if (needle.byteLength > bytes.byteLength) return false;
          outer: for (let at = 0;
               at <= bytes.byteLength - needle.byteLength; at++) {
            for (let index = 0; index < needle.byteLength; index++)
              if (bytes[at + index] !== needle[index]) continue outer;
            return true;
          }
          return false;
        };
        let boundary = "";
        for (let attempt = 0; attempt < 8 && !boundary; attempt++) {
          trustedCryptoRandomFill(random);
          let suffix = "";
          for (let index = 0; index < random.length; index++)
            suffix += random[index].toString(16).padStart(2, "0");
          const candidate = "----tilefinch-" + suffix;
          if (!entries.some((entry) => contains(entry.bytes, candidate)))
            boundary = candidate;
        }
        if (!boundary)
          throw new RangeError("Unable to choose multipart boundary");
        if (!headers.has("content-type"))
          headers.set("content-type", "multipart/form-data; boundary=" + boundary);
        const chunks = [];
        total = 0;
        const append = (bytes) => {
          if (!(bytes instanceof Uint8Array)) bytes = encoder.encode(String(bytes));
          if (bytes.byteLength > requestBodyByteLimit - total)
            throw new RangeError("FormData body exceeds bounded size");
          chunks.push(bytes);
          total += bytes.byteLength;
        },
          quote = (value) => String(value)
            .replaceAll("\r", "%0D")
            .replaceAll("\n", "%0A")
            .replaceAll('"', "%22"),
          normalizeLineBreaks = (value) => String(value)
            .replace(/\r\n|\r|\n/g, "\r\n");
        for (const entry of entries) {
          const { name, value } = entry;
          append("--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"" + quote(name) + "\"");
          if (value instanceof File)
            append('; filename="' + quote(value.name) + '"');
          append("\r\n");
          if (value instanceof Blob && value.type)
            append("Content-Type: " + value.type + "\r\n");
          append("\r\n");
          append(value instanceof Blob ? entry.bytes : normalizeLineBreaks(value));
          append("\r\n");
        }
        append("--" + boundary + "--\r\n");
        const bytes = new Uint8Array(total);
        let offset = 0;
        for (const chunk of chunks) {
          bytes.set(chunk, offset);
          offset += chunk.byteLength;
        }
        return bytes;
      }
      ensureRequestBodyContentType(headers, source);
      if (source instanceof Blob)
        return admit(tilefinchBlobBytes(source).slice());
      if (isArrayBuffer(source)) return admit(copyArrayBufferBytes(source));
      if (arrayBufferIsView(source))
        return admit(copyArrayBufferViewBytes(source));
      return admit(encoder.encode(String(source)));
    };
  const requestBodyStorage = new TrustedWeakMap(),
    requestBodyState = (request) => {
      if (!trustedWeakMapHas(requestBodyStorage, request))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(requestBodyStorage, request);
    },
    consumeRequestBody = async (request, signal = null, release = false) => {
      const state = requestBodyState(request);
      if (state.body === null) return new Uint8Array();
      const bytes = await __tilefinchConsumeReadableByteStream(
        state.body, requestBodyByteLimit, signal, release);
      state.bytes = null;
      return bytes;
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
  let Response;
  class Request {
    constructor(input, init = {}) {
      const prior = input instanceof Request ? input : null;
      const initBody = init.body,
        inheritsBody = prior && (initBody === undefined || initBody === null),
        priorBody = prior ? requestBodyState(prior) : null;
      if (inheritsBody && (prior.bodyUsed || prior.body?.locked))
        throw new TypeError("Body has already been consumed");
      const url = prior
        ? prior.url
        : new URL(String(input), location.href).href;
      const method = String(
        init.method === undefined
          ? prior
            ? prior.method
            : "GET"
          : init.method,
      );
      if (!isHttpToken(method) || forbiddenMethod(method))
        throw new TypeError("Invalid HTTP method");
      const normalizedMethod = method.toUpperCase(),
        headers = new Headers(
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
          priorBody.bytes = null;
          body = priorBody.body === null
            ? null
            : bodyBytesSnapshot === null
              ? proxyRequestBody(priorBody.body)
              : null;
        }
        if (body === null && bodyBytesSnapshot !== null)
          body = new ReadableStream({
            start: (controller) => {
              controller.enqueue(bodyBytesSnapshot);
              controller.close();
            },
          });
        const requestState = {
          bytes: bodyBytesSnapshot,
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
            type: this.headers.get("content-type") || "",
          }));
    }
    formData() {
      return this.text().then((text) => {
        const type = this.headers.get("content-type") || "";
        if (!type.startsWith("application/x-www-form-urlencoded"))
          throw new TypeError("Unsupported form data encoding");
        const data = new FormData();
        for (const [name, value] of new URLSearchParams(text))
          data.append(name, value);
        return data;
      });
    }
    clone() {
      if (this.bodyUsed || this.body?.locked)
        throw new TypeError("Body has already been consumed");
      if (this.body === null) return new Request(this);
      const state = requestBodyState(this),
        branches = this.body.tee();
      state.body = branches[0];
      state.bytes = null;
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
      subtleBufferNormalizers = new WeakMap(),
      cryptoSubtleInstances = new WeakMap(),
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
    class SubtleCrypto {
      constructor(token) {
        if (token !== subtleToken) throw new TypeError("Illegal constructor");
        subtleInstances.add(this);
      }
      digest(algorithm, data) {
        if (!subtleInstances.has(this))
          throw new TypeError("Illegal invocation");
        try {
          const name =
            typeof algorithm === "string"
              ? algorithm
              : algorithm && typeof algorithm === "object"
                ? algorithm.name
                : undefined;
          if (name === undefined)
            throw new TypeError("Algorithm name is required");
          if (String(name).toUpperCase() !== "SHA-256")
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
          const result = Promise.resolve(
            __tilefinchCryptoDigestSHA256(source));
          return normalizers ? result.then(normalizers.output) : result;
        } catch (error) {
          return Promise.reject(error);
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
        return cryptoSubtleInstances.get(this);
      }
      getRandomValues(array) {
        if (!cryptoInstances.has(this))
          throw new TypeError("Illegal invocation");
        if (
          !ArrayBuffer.isView(array) ||
          !integerRandomViews.has(Object.prototype.toString.call(array))
        )
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
    globalThis.SubtleCrypto = SubtleCrypto;
    globalThis.crypto = createCrypto();
    /* Dedicated workers get a distinct platform object. Sharing the owner
       object's mutable own properties would violate realm isolation. */
    globalThis.__tilefinchCreateWorkerCrypto = createCrypto;
    globalThis.__tilefinchSetCryptoBufferNormalizer = (
      crypto, input, output,
    ) => {
      const subtle = cryptoSubtleInstances.get(crypto);
      if (!subtle || typeof input !== "function" || typeof output !== "function")
        throw new TypeError("invalid Crypto normalizer");
      subtleBufferNormalizers.set(subtle, { input, output });
    };
  }
  if (globalThis.console === undefined) {
    const emit =
      (level) =>
      (...args) =>
        __tilefinchConsoleLog(
          level,
          ...args.map((value) =>
            value && value.stack ? String(value) + "\n" + value.stack : value,
          ),
        );
    const noop = () => {};
    globalThis.console = {
      assert: noop,
      clear: noop,
      count: noop,
      countReset: noop,
      log: emit("log"),
      info: emit("info"),
      warn: emit("warn"),
      error: emit("error"),
      debug: emit("debug"),
      dir: noop,
      dirxml: noop,
      trace: emit("trace"),
      group: noop,
      groupCollapsed: noop,
      groupEnd: noop,
      table: noop,
      time: noop,
      timeEnd: noop,
      timeLog: noop,
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
  const serializeRequestBody = (request) => {
    if (request instanceof Request) {
      const state = requestBodyState(request);
      if (state.body === null) return undefined;
      const bytes = __tilefinchConsumeBufferedReadableByteStream(
        state.body, requestBodyByteLimit);
      if (bytes === null)
        throw new TypeError("Streaming body requires asynchronous consumption");
      state.bytes = null;
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
    activeNetwork = 0;
  const pendingNetwork = new Map(),
    nativeNetwork = new Map(),
    waitingNetwork = [],
    networkQueueStats = {
      admitted: 0,
      launched: 0,
      completed: 0,
      cancelled: 0,
      timedOut: 0,
      rejected: 0,
      rejectedBytes: 0,
      launchFailed: 0,
      localBlobReads: 0,
      localBlobFailures: 0,
      currentCount: 0,
      peakCount: 0,
      currentBytes: 0,
      peakBytes: 0,
      waiting: 0,
      active: 0,
    },
    networkQueueView = {};
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
  const releaseNetwork = (entry) => {
    if (!entry || !pendingNetwork.has(entry.id)) return false;
    pendingNetwork.delete(entry.id);
    if (entry.state === "waiting") {
      const at = waitingNetwork.indexOf(entry);
      if (at >= 0) waitingNetwork.splice(at, 1);
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
  const pumpNetworkQueue = () => {
    while (activeNetwork < nativeNetworkLimit && waitingNetwork.length) {
      const entry = waitingNetwork.shift();
      if (!entry || entry.state !== "waiting") continue;
      networkQueueStats.waiting = Math.max(0, networkQueueStats.waiting - 1);
      if (entry.nativeId) clearTimeout(entry.nativeId);
      entry.state = "launching";
      try {
        const nativeId = Number(entry.start());
        if (nativeId === 0) {
          /* Native policy has already admitted this immutable request.  Zero
             means the shared page scheduler is temporarily full.  Put the
             same entry back at the head so neither a later mutation nor a
             later request can overtake it, then retry from a future timer
             turn instead of spinning the single browser thread. */
          entry.state = "waiting";
          waitingNetwork.unshift(entry);
          networkQueueStats.waiting++;
          entry.nativeId = setTimeout(pumpNetworkQueue, 10);
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
    if (
      networkQueueStats.currentCount >= networkQueueLimit ||
      bytes > networkQueueByteLimit - networkQueueStats.currentBytes
    ) {
      networkQueueStats.rejected++;
      if (bytes > networkQueueByteLimit - networkQueueStats.currentBytes)
        networkQueueStats.rejectedBytes++;
      throw new RangeError("network pending queue quota exceeded");
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
  globalThis.__tilefinchDetachNetwork = () => {
    const reason = new TypeError("document detached");
    /* Snapshot the keys: cancelNetwork mutates both maps and may start a
       queued successor after each cancellation. Repeating to a fixed bound
       drains successors too without trusting author-modified iteration. */
    for (let pass = 0; pass < networkQueueLimit && pendingNetwork.size; pass++) {
      const ids = Array.from(pendingNetwork.keys()).slice(0, networkQueueLimit);
      for (const id of ids) cancelNetwork(id, reason, true);
    }
    return pendingNetwork.size === 0;
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
  Object.defineProperty(globalThis, "__tilefinchPendingNetworkRequests", {
    value: () => networkQueueStats.waiting,
    writable: false,
    configurable: false,
  });
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
  globalThis.__tilefinchXHRSendCalls = 0;
  globalThis.__tilefinchXHRLastError = "";
  const responseBrands = new WeakSet(),
    responseMetadata = new WeakMap(),
    responseBodyStorage = new TrustedWeakMap(),
    responseBodyState = (response) => {
      if (!trustedWeakMapHas(responseBodyStorage, response))
        throw new TrustedTypeError("Illegal invocation");
      return trustedWeakMapGet(responseBodyStorage, response);
    };
  Response = class Response {
    constructor(body = null, init = {}) {
      const status = Number(init.status === undefined ? 200 : init.status);
      if (
        status !== 0 &&
        (!Number.isInteger(status) || status < 200 || status > 599)
      )
        throw new RangeError("Invalid response status");
      if (body !== null && body !== undefined &&
          [101, 204, 205, 304].includes(status))
        throw new TypeError("Response status cannot have a body");
      const headers = init.headers instanceof Headers
        ? new Headers(init.headers)
        : new Headers(init.headers);
      const streamBody = body instanceof ReadableStream ? body : null;
      if (
        streamBody &&
        (streamBody.locked || __tilefinchReadableStreamDisturbed(streamBody))
      )
        throw new TypeError("Response body stream is unusable");
      const supplied = init.bodyBytes,
        suppliedBodyBytes = supplied !== undefined;
      let bytes = suppliedBodyBytes
        ? isArrayBuffer(supplied)
          ? copyArrayBufferBytes(supplied)
          : arrayBufferIsView(supplied)
            ? copyArrayBufferViewBytes(supplied)
            : null
        : streamBody ? null : snapshotRequestBody(body, headers);
      if (suppliedBodyBytes && bytes === null)
        throw new TypeError("Response bodyBytes must be a BufferSource");
      const bodyState = { body: null };
      trustedWeakMapSet(responseBodyStorage, this, bodyState);
      this.status = status;
      this.statusText = String(
        init.statusText === undefined ? "" : init.statusText,
      );
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
      let retainedBytes = bytes;
      let at = 0;
      bodyState.body =
        (body === null || body === undefined) && !suppliedBodyBytes
          ? null
          : streamBody ||
            new ReadableStream({
              pull(controller) {
                const bytes = retainedBytes || new Uint8Array();
                if (at >= bytes.length) {
                  retainedBytes = null;
                  controller.close();
                  return;
                }
                const end = Math.min(bytes.length, at + 4096);
                controller.enqueue(bytes.slice(at, end));
                at = end;
                if (at >= bytes.length) {
                  retainedBytes = null;
                  controller.close();
                }
              },
              cancel() { retainedBytes = null; },
            });
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
        const bytes = await __tilefinchConsumeReadableByteStream(
          state.body, 256 * 1024);
        return bytes;
      };
    Response.prototype.text = function () {
      return takeBytes(this).then((bytes) => new TextDecoder().decode(bytes));
    };
    Response.prototype.json = function () {
      return this.text().then(JSON.parse);
    };
    Response.prototype.arrayBuffer = function () {
      return takeBytes(this).then((bytes) => bytes.buffer);
    };
    Response.prototype.bytes = function () {
      return this.arrayBuffer().then((buffer) => new Uint8Array(buffer));
    };
    Response.prototype.blob = function () {
      return takeBytes(this).then(
        (bytes) => new Blob([bytes], {
          type: this.headers.get("content-type") || "",
        }));
    };
    Response.prototype.formData = function () {
      const type = this.headers.get("content-type") || "";
      if (!type.startsWith("application/x-www-form-urlencoded"))
        return Promise.reject(new TypeError("Unsupported form data encoding"));
      return this.text().then((text) => {
        const data = new FormData();
        for (const [name, value] of new URLSearchParams(text))
          data.append(name, value);
        return data;
      });
    };
    Response.prototype.clone = function () {
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
      if (this.body === null) return new Response(null, init);
      const [first, second] = this.body.tee();
      state.body = first;
      return new Response(second, init);
    };
    Response.error = () =>
      new Response(null, { status: 0, statusText: "", type: "error" });
    Response.redirect = (url, status = 302) => {
      status = Number(status);
      if (![301, 302, 303, 307, 308].includes(status))
        throw new RangeError("Invalid redirect status");
      return new Response(null, {
        status,
        headers: { location: new URL(String(url), location.href).href },
      });
    };
    Response.json = (data, init = {}) => {
      const headers = new Headers(init.headers);
      if (!headers.has("content-type"))
        headers.set("content-type", "application/json");
      return new Response(JSON.stringify(data), { ...init, headers });
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
      const request =
        input instanceof Request
          ? new Request(input, init)
          : new Request(input, init);
      request.signal.throwIfAborted();
      if (
        request.body !== null &&
        requestBodyState(request).bytes === null
      )
        return serializeRequestBodyAsync(request).then((bytes) =>
          fetchInternal(request, {
            body: new Uint8Array(bytes),
            duplex: "half",
          }));
      const body = serializeRequestBody(request),
        contentType = request.headers.get("content-type") || "",
        accept = request.headers.get("accept") || "*/*",
        authorHeaderBlock = nativeHeaderBlock(request.headers),
        cacheHeaderBlock = request.cache === "no-store"
          ? "cache-control: no-store"
          : request.cache === "reload"
            ? "cache-control: no-cache\npragma: no-cache"
            : request.cache === "no-cache"
              ? "cache-control: no-cache"
              : "",
        headerBlock = authorHeaderBlock && cacheHeaderBlock
          ? authorHeaderBlock + "\n" + cacheHeaderBlock
          : authorHeaderBlock || cacheHeaderBlock,
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
              new Response(method === "HEAD" ? null : local.bodyBytes, {
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
        return Promise.resolve(new Response(null, {
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
                new Response(nullBody ? null : raw.body, {
                  status: responseStatus,
                  url: raw.url,
                  headers:
                    raw.type === "opaqueredirect" || raw.type === "opaque"
                      ? new Headers()
                      : new Headers(
                          raw.headers ||
                            "content-type: " + raw.contentType + "\n",
                        ),
                  bodyBytes: nullBody ? undefined : raw.bodyBytes,
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
  globalThis.fetch = (input, init = {}) =>
    fetchInternal(input, init, __tilefinchRecordResourceTiming);
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
    const raw = __tilefinchFetchSync(
      "GET", String(url), undefined, "", "", mode, credentials, 2,
      undefined, "about:client", "");
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
      if (!(this instanceof Navigator))
        throw new TypeError("Illegal invocation");
      try {
        const target = new URL(String(url), location.href);
        if (target.protocol !== "http:" && target.protocol !== "https:")
          return false;
        const request = new Request(target.href, {
            method: "POST",
            body: data,
            credentials: "include",
            keepalive: true,
          }),
          serialized = serializeRequestBody(request),
          bytes =
            serialized === undefined
              ? 0
              : typeof serialized === "string"
                ? new TextEncoder().encode(serialized).byteLength
                : serialized.byteLength;
        if (
          bytes > 64 * 1024 ||
          networkQueueStats.currentCount >= networkQueueLimit ||
          networkQueueStats.currentBytes + bytes > networkQueueByteLimit
        )
          return false;
        fetch(request).catch(() => {});
        return true;
      } catch (_) {
        return false;
      }
    },
  });
  {
    const eventSources = new Map(),
      eventSourceLimit = 2;
    class EventSource extends EventTarget {
      constructor(url, options = {}) {
        super();
        if (arguments.length < 1)
          throw new TypeError("EventSource requires a URL");
        this.url = new URL(String(url), location.href).href;
        if (!/^https?:/.test(this.url))
          throw new DOMException(
            "EventSource requires HTTP or HTTPS",
            "SecurityError",
          );
        this.withCredentials = !!options.withCredentials;
        this.readyState = EventSource.CONNECTING;
        this.onopen = null;
        this.onmessage = null;
        this.onerror = null;
        this._nativeId = 0;
        this._closed = false;
        this._lastEventId = "";
        this._retry = 3000;
        this._decoder = new TextDecoder();
        this._text = "";
        this._data = [];
        this._dataBytes = 0;
        this._eventType = "";
        this._reconnectTask = 0;
        this._connect();
      }
      _emit(event) {
        this.dispatchEvent(event);
        const handler = this["on" + event.type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "event-source:" + String(event.type),
            handler,
            this,
            [event],
          );
      }
      _connect() {
        if (this._closed) return;
        if (eventSources.size >= eventSourceLimit) {
          this._failAndReconnect();
          return;
        }
        try {
          const id = Number(
            __tilefinchEventSourceStart(
              this.url,
              this.withCredentials,
              this._lastEventId,
            ),
          );
          if (!(id > 0)) throw new Error("EventSource admission failed");
          this._nativeId = id;
          eventSources.set(id, this);
        } catch (_) {
          this._failAndReconnect();
        }
      }
      _line(line) {
        if (line === "") {
          if (this._data.length) {
            const event = new MessageEvent(this._eventType || "message", {
              data: this._data.join("\n"),
              origin: new URL(this.url).origin,
              lastEventId: this._lastEventId,
              source: null,
            });
            this._emit(event);
          }
          this._data = [];
          this._dataBytes = 0;
          this._eventType = "";
          return;
        }
        if (line.startsWith(":")) return;
        const colon = line.indexOf(":"),
          field = colon < 0 ? line : line.slice(0, colon);
        let value = colon < 0 ? "" : line.slice(colon + 1);
        if (value.startsWith(" ")) value = value.slice(1);
        if (field === "data") {
          const separator = this._data.length ? 1 : 0;
          if (
            value.length > 64 * 1024 - separator ||
            this._dataBytes > 64 * 1024 - separator - value.length
          ) {
            this._data = [];
            this._dataBytes = 0;
            this.close();
            this._emit(new Event("error"));
            return;
          }
          this._data.push(value);
          this._dataBytes += separator + value.length;
        }
        else if (field === "event") this._eventType = value;
        else if (field === "id" && !value.includes("\0"))
          this._lastEventId = value;
        else if (field === "retry" && /^\d+$/.test(value))
          this._retry = Math.min(30000, Math.max(250, Number(value)));
      }
      _chunk(bytes) {
        this._text +=
          bytes instanceof ArrayBuffer
            ? this._decoder.decode(new Uint8Array(bytes), { stream: true })
            : String(bytes);
        for (;;) {
          let newline = -1;
          for (let index = 0; index < this._text.length; index++) {
            if (this._text[index] === "\n" || this._text[index] === "\r") {
              newline = index;
              break;
            }
          }
          if (newline < 0) {
            if (this._text.length > 64 * 1024) {
              this.close();
              this._emit(new Event("error"));
            }
            return;
          }
          if (
            this._text[newline] === "\r" &&
            newline + 1 === this._text.length
          )
            return;
          const line = this._text.slice(0, newline),
            consumed =
              this._text[newline] === "\r" &&
              this._text[newline + 1] === "\n"
                ? newline + 2
                : newline + 1;
          this._text = this._text.slice(consumed);
          this._line(line);
          if (this._closed) return;
        }
      }
      _failAndReconnect() {
        if (this._closed) return;
        this.readyState = EventSource.CONNECTING;
        this._emit(new Event("error"));
        clearTimeout(this._reconnectTask);
        this._reconnectTask = setTimeout(() => this._connect(), this._retry);
      }
      close() {
        if (this._closed) return;
        this._closed = true;
        this.readyState = EventSource.CLOSED;
        clearTimeout(this._reconnectTask);
        if (this._nativeId) {
          eventSources.delete(this._nativeId);
          __tilefinchEventSourceClose(this._nativeId);
          this._nativeId = 0;
        }
      }
    }
    for (const [name, value] of Object.entries({
      CONNECTING: 0,
      OPEN: 1,
      CLOSED: 2,
    })) {
      Object.defineProperty(EventSource, name, { value });
      Object.defineProperty(EventSource.prototype, name, { value });
    }
    globalThis.EventSource = EventSource;
    globalThis.__tilefinchDeliverEventSource = (
      id,
      kind,
      payload,
      origin,
    ) => {
      const source = eventSources.get(Number(id));
      if (!source || source._closed) return false;
      if (kind === "open") {
        source.readyState = EventSource.OPEN;
        source._emit(new Event("open"));
      } else if (kind === "chunk") {
        source._chunk(payload);
      } else {
        eventSources.delete(Number(id));
        source._nativeId = 0;
        source._failAndReconnect();
      }
      return true;
    };
  }
  {
    const webSockets = new Map(),
      webSocketLimit = 2,
      webSocketByteLimit = 64 * 1024,
      webSocketQueueLimit = 16;
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
        else setTimeout(() => this._fail("duplex transport unavailable"), 0);
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
        this.dispatchEvent(event);
        const handler = this["on" + event.type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "websocket:" + String(event.type),
            handler,
            this,
            [event],
          );
      }
      _finish(code, reason, wasClean, withError) {
        if (this.readyState === WebSocket.CLOSED) return;
        if (this._nativeId) webSockets.delete(this._nativeId);
        this._nativeId = 0;
        this.readyState = WebSocket.CLOSED;
        this._queue = [];
        this.bufferedAmount = 0;
        if (withError) this._emit(new Event("error"));
        this._emit(new CloseEvent("close", { code, reason, wasClean }));
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
        if (
          code !== undefined &&
          (!Number.isInteger(numericCode) ||
            (numericCode !== 1000 &&
              (numericCode < 3000 || numericCode > 4999)))
        )
          throw new DOMException("Invalid WebSocket close code", "InvalidAccessError");
        if (new TextEncoder().encode(String(reason)).length > 123)
          throw new DOMException("WebSocket close reason is too long", "SyntaxError");
        if (this.readyState === WebSocket.CLOSED ||
            this.readyState === WebSocket.CLOSING) return;
        this.readyState = WebSocket.CLOSING;
        this._closeRequest = { code: numericCode, reason: String(reason) };
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
        socket._emit(new Event("open"));
        socket._pump();
      } else if (kind === "text" || kind === "binary") {
        const buffer = payload instanceof ArrayBuffer
          ? payload : new Uint8Array(payload).buffer;
        let data;
        if (kind === "text") data = new TextDecoder().decode(buffer);
        else data = socket.binaryType === "arraybuffer"
          ? buffer : new Blob([buffer]);
        socket._emit(new MessageEvent("message", {
          data,
          origin: new URL(socket.url.replace(/^ws/, "http")).origin,
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
      requestPageControls(target = document.documentElement) {
        if (!target || typeof target.requestFullscreen !== "function") {
          return Promise.reject(new TypeError("Page controls need an element"));
        }
        return target.requestFullscreen();
      },
      pageControlsExitChord: "Start+Select",
    }),
  });
  {
    const channels = new Map(),
      messageLimit = 512,
      queueLimit = 8,
      bufferedLimit = messageLimit * queueLimit;
    const multiplayerEvent = (type, fields = {}) => {
      const event = new Event(type);
      for (const [name, value] of Object.entries(fields))
        Object.defineProperty(event, name, {
          value, enumerable: true, configurable: true,
        });
      return event;
    };
    class TilefinchMultiplayerChannel extends EventTarget {
      constructor(mode, options, inviteCode = "") {
        super();
        options = options && typeof options === "object" ? options : {};
        this.label = String(options.gameId || "").slice(0, 32);
        this.protocol = "tilefinch-game-v1";
        this.ordered = false;
        this.maxPacketLifeTime = null;
        this.maxRetransmits = 0;
        this.negotiated = false;
        this.id = null;
        this.readyState = "connecting";
        this.bufferedAmount = 0;
        this.bufferedAmountLowThreshold = 0;
        this.binaryType = "arraybuffer";
        this.onopen = null;
        this.onmessage = null;
        this.onbufferedamountlow = null;
        this.onclose = null;
        this.onerror = null;
        this.onstatus = null;
        this.oninvitecode = null;
        this.ondiscovered = null;
        this.onpeerrequest = null;
        this._queue = [];
        this._sending = false;
        this._closeRequest = null;
        const name = String(options.name || "Player").slice(0, 24);
        try {
          this._nativeId = Number(__tilefinchMultiplayerStart(
            String(mode), this.label, name, String(inviteCode || ""),
          ));
        } catch (_) {
          this._nativeId = 0;
        }
        if (this._nativeId > 0) channels.set(this._nativeId, this);
        else setTimeout(() => this._fail("multiplayer unavailable"), 0);
      }
      _emit(event) {
        this.dispatchEvent(event);
        const handler = this["on" + event.type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "multiplayer:" + String(event.type), handler, this, [event],
          );
      }
      _fail(detail) {
        if (this.readyState === "closed") return;
        this._emit(multiplayerEvent("error", { detail: String(detail || "") }));
        this._finish(1006, String(detail || ""), false);
      }
      _finish(code, reason, clean) {
        if (this.readyState === "closed") return;
        if (this._nativeId) channels.delete(this._nativeId);
        this._nativeId = 0;
        this.readyState = "closed";
        this._queue.length = 0;
        this.bufferedAmount = 0;
        this._emit(new CloseEvent("close", {
          code: Number(code) || 0,
          reason: String(reason || ""),
          wasClean: !!clean,
        }));
      }
      _pump() {
        if (!this._nativeId || this._sending || !this._queue.length) {
          if (!this._sending && !this._queue.length && this._closeRequest
              && this._nativeId) {
            const close = this._closeRequest;
            this._closeRequest = null;
            if (!__tilefinchMultiplayerClose(
                this._nativeId, close.code, close.reason))
              this._fail("close admission failed");
          }
          return;
        }
        if (this.readyState !== "open" && this.readyState !== "closing")
          return;
        const item = this._queue[0];
        if (__tilefinchMultiplayerSend(
            this._nativeId, item.bytes.buffer, item.binary))
          this._sending = true;
      }
      send(data) {
        if (this.readyState !== "open")
          throw new DOMException("Multiplayer channel is not open", "InvalidStateError");
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
        } else {
          throw new TypeError("Unsupported multiplayer message type");
        }
        if (bytes.byteLength > messageLimit)
          throw new DOMException("Multiplayer message is too large", "QuotaExceededError");
        if (this._queue.length >= queueLimit
            || bytes.byteLength > bufferedLimit - this.bufferedAmount)
          throw new DOMException("Multiplayer send queue is full", "QuotaExceededError");
        this._queue.push({ bytes, binary });
        this.bufferedAmount += bytes.byteLength;
        this._pump();
      }
      accept(peerIdentity, accepted = true) {
        if (!this._nativeId || this.readyState !== "connecting") return false;
        return !!__tilefinchMultiplayerAccept(
          this._nativeId, Number(peerIdentity) >>> 0, !!accepted,
        );
      }
      addRemoteCode(code) {
        if (!this._nativeId || this.readyState === "closed") return false;
        return !!__tilefinchMultiplayerAddRemoteCode(
          this._nativeId, String(code || ""),
        );
      }
      close(code = 1000, reason = "") {
        code = Number(code);
        reason = String(reason);
        if (!Number.isInteger(code)
            || (code !== 1000 && (code < 3000 || code > 4999)))
          throw new DOMException("Invalid close code", "InvalidAccessError");
        if (new TextEncoder().encode(reason).length > 63)
          throw new DOMException("Close reason is too long", "SyntaxError");
        if (this.readyState === "closed" || this.readyState === "closing") return;
        this.readyState = "closing";
        this._closeRequest = { code, reason };
        this._pump();
      }
    }
    const openChannel = (mode, code, options) => {
      options = options && typeof options === "object" ? options : {};
      if (!String(options.gameId || ""))
        throw new TypeError("gameId is required");
      return new TilefinchMultiplayerChannel(mode, options, code);
    };
    Object.defineProperty(navigator, "tilefinchMultiplayer", {
      configurable: false,
      enumerable: false,
      value: Object.freeze({
        host(options) { return openChannel("host", "", options); },
        join(code, options) { return openChannel("join", code, options); },
        discover(options) { return openChannel("discover", "", options); },
        supported: true,
        maxMessageSize: messageLimit,
      }),
    });
    globalThis.__tilefinchDeliverMultiplayer = (
      id, kind, payload, detail, code, clean, peerIdentity, inviteCode, peerName,
    ) => {
      const channel = channels.get(Number(id));
      if (!channel || channel.readyState === "closed") return false;
      if (kind === "open") {
        if (channel.readyState !== "connecting") return false;
        channel.readyState = "open";
        channel._emit(new Event("open"));
        channel._pump();
      } else if (kind === "binary" || kind === "text") {
        const buffer = payload instanceof ArrayBuffer
          ? payload : new Uint8Array(payload).buffer;
        const data = kind === "text"
          ? new TextDecoder().decode(buffer) : buffer;
        channel._emit(new MessageEvent("message", {
          data, origin: "", source: null,
        }));
      } else if (kind === "drain") {
        const before = channel.bufferedAmount;
        const item = channel._queue.shift();
        channel._sending = false;
        if (item) channel.bufferedAmount = Math.max(
          0, channel.bufferedAmount - item.bytes.byteLength,
        );
        if (before > channel.bufferedAmountLowThreshold
            && channel.bufferedAmount <= channel.bufferedAmountLowThreshold)
          channel._emit(new Event("bufferedamountlow"));
        channel._pump();
      } else if (kind === "status") {
        channel._emit(multiplayerEvent("status", { detail: String(detail || "") }));
      } else if (kind === "invitecode") {
        channel._emit(multiplayerEvent("invitecode", {
          code: String(inviteCode || ""), detail: String(detail || ""),
        }));
      } else if (kind === "discovered" || kind === "peerrequest") {
        channel._emit(multiplayerEvent(kind, {
          peerIdentity: Number(peerIdentity) >>> 0,
          code: String(inviteCode || ""),
          name: String(peerName || ""),
          detail: String(detail || ""),
        }));
      } else {
        channel._finish(code, detail, clean);
      }
      return true;
    };
  }
  const xhrEventTargetStates = new WeakMap(),
    xhrPublicStates = new WeakMap(),
    xhrPrivateStates = new WeakMap(),
    xhrEventHandlerTypes = [
      "abort",
      "error",
      "load",
      "loadend",
      "loadstart",
      "progress",
      "timeout",
    ];
  class XMLHttpRequestEventTarget extends EventTarget {
    constructor() {
      super();
      xhrEventTargetStates.set(this, {
        handlers: new Map(),
        handlerListeners: new Map(),
      });
      xhrPublicStates.set(this, Object.create(null));
    }
  }
  const xhrSetEventHandler = (target, type, value) => {
    const state = xhrEventTargetStates.get(target);
    if (!state) throw new TypeError("Illegal invocation");
    const callback = typeof value === "function" ? value : null,
      wrapper = state.handlerListeners.get(type);
    state.handlers.set(type, callback);
    if (callback && !wrapper) {
      const listener = (event) => {
        const active = xhrEventTargetStates.get(target)?.handlers.get(type);
        if (typeof active === "function") return active.call(target, event);
      };
      state.handlerListeners.set(type, listener);
      EventTarget.prototype.addEventListener.call(target, type, listener);
    } else if (!callback && wrapper) {
      EventTarget.prototype.removeEventListener.call(target, type, wrapper);
      state.handlerListeners.delete(type);
    }
  };
  for (const type of xhrEventHandlerTypes)
    Object.defineProperty(XMLHttpRequestEventTarget.prototype, "on" + type, {
      configurable: true,
      enumerable: true,
      get() {
        return xhrEventTargetStates.get(this)?.handlers.get(type) || null;
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
  class TilefinchXMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
      super();
      this.readyState = 0;
      this.status = 0;
      this.statusText = "";
      this.response = null;
      this.responseURL = "";
      this.responseXML = null;
      this.onreadystatechange = null;
      this.timeout = 0;
      this.withCredentials = false;
      this.upload = new XMLHttpRequestUpload();
      xhrPrivateStates.set(this, {
        async: true,
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
        stateTrace: [],
        timeoutId: 0,
        uploadBytes: 0,
        uploadComplete: false,
        uploadStarted: false,
        url: "",
      });
    }
    get responseType() {
      return xhrPrivateStates.get(this).responseType;
    }
    set responseType(value) {
      value = String(value || "");
      if (
        !["", "text", "json", "arraybuffer", "blob", "document"].includes(value)
      )
        throw new DOMException("Unsupported response type", "SyntaxError");
      if (this.readyState === 3 || this.readyState === 4)
        throw new DOMException(
          "Response type cannot change now",
          "InvalidStateError",
        );
      xhrPrivateStates.get(this).responseType = value;
    }
    get responseText() {
      const state = xhrPrivateStates.get(this);
      if (state.responseType !== "" && state.responseType !== "text")
        throw new DOMException(
          "responseText is unavailable for this response type",
          "InvalidStateError",
        );
      return state.responseText;
    }
    open(method, url, async = true) {
      const state = xhrPrivateStates.get(this);
      if (!state.done && state.requestId)
        cancelNetwork(
          state.requestId,
          new DOMException("Request reopened", "AbortError"),
          false,
        );
      if (state.timeoutId) clearTimeout(state.timeoutId);
      state.timeoutId = 0;
      state.generation += 1;
      state.method = String(method).toUpperCase();
      state.url = String(url);
      state.async = !!async;
      this.status = 0;
      this.statusText = "";
      this.response = null;
      this.responseURL = "";
      this.responseXML = null;
      state.responseHeaders = new Headers();
      state.headers = new Headers();
      state.responseText = "";
      state.requestId = 0;
      state.responseBytes = 0;
      state.responseLengthComputable = false;
      state.done = true;
      state.sent = false;
      state.uploadBytes = 0;
      state.uploadComplete = false;
      state.uploadStarted = false;
      this.readyState = 1;
      state.stateTrace = [1];
      xhrEmit(this, "readystatechange");
    }
    setRequestHeader(name, value) {
      const state = xhrPrivateStates.get(this);
      if (this.readyState !== 1 || state.sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      state.headers.append(name, value);
    }
    overrideMimeType(type) {
      if (this.readyState === 3 || this.readyState === 4)
        throw new DOMException("Response is loading", "InvalidStateError");
      xhrPrivateStates.get(this).mimeType = String(type);
    }
    emit(type, loaded = 0, total = 0) {
      const event = new Event(type);
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
      this.readyState = value;
      xhrEmit(this, "readystatechange");
    }
    _finish(type, generation) {
      const state = xhrPrivateStates.get(this);
      if (state.done || state.generation !== generation) return false;
      state.done = true;
      if (state.timeoutId) clearTimeout(state.timeoutId);
      state.timeoutId = 0;
      state.requestId = 0;
      const loaded = state.responseBytes,
        total = state.responseLengthComputable ? loaded : 0;
      xhrEmit(this, type, loaded, total);
      if (state.generation !== generation) return false;
      xhrEmit(this, "loadend", loaded, total);
      return state.generation === generation;
    }
    _apply(raw, generation) {
      const state = xhrPrivateStates.get(this);
      if (state.done || state.generation !== generation) return;
      if (!xhrUploadFinish(this, "load", generation)) return;
      this.status = Number(raw.status) || 0;
      this.responseURL = raw.url || state.url;
      state.responseHeaders = new Headers(
        raw.headers || "content-type: " + raw.contentType + "\n",
      );
      if (!xhrState(this, 2, generation)) return;
      const supplied =
          raw.bodyBytes instanceof ArrayBuffer
            ? new Uint8Array(raw.bodyBytes)
            : null,
        needsText =
          state.responseType === "" ||
          state.responseType === "text" ||
          state.responseType === "json" ||
          state.responseType === "document" ||
          /(?:xml|html)/i.test(
            state.mimeType ||
              state.responseHeaders.get("content-type") ||
              "",
          );
      state.responseText = needsText
        ? raw.body !== undefined
          ? String(raw.body)
          : new TextDecoder().decode(supplied || new Uint8Array())
        : "";
      if (!xhrState(this, 3, generation)) return;
      const fallbackBody = raw.body === undefined ? "" : String(raw.body),
        byteLength = Number.isFinite(Number(raw.bodyLength))
          ? Math.max(0, Number(raw.bodyLength))
          : supplied
            ? supplied.byteLength
            : new TextEncoder().encode(fallbackBody).byteLength;
      state.responseBytes = byteLength;
      state.responseLengthComputable = true;
      xhrEmit(this, "progress", byteLength, byteLength);
      if (state.generation !== generation || state.done) return;
      if (state.responseType === "" || state.responseType === "text")
        this.response = state.responseText;
      else if (state.responseType === "json") {
        try {
          this.response = JSON.parse(state.responseText);
        } catch (_) {
          this.response = null;
        }
      } else if (state.responseType === "arraybuffer") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        this.response =
          bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
            ? bytes.buffer
            : bytes.slice().buffer;
      } else if (state.responseType === "blob") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        this.response = new Blob([bytes], {
          type: state.responseHeaders.get("content-type") || "",
        });
      } else if (state.responseType === "document") {
        const mime =
          /xml/i.test(
            state.mimeType ||
              state.responseHeaders.get("content-type") ||
              "",
          )
            ? "text/xml"
            : "text/html";
        this.response = new DOMParser().parseFromString(
          state.responseText,
          mime,
        );
      } else this.response = null;
      if (
        state.responseType === "" &&
        /(?:xml|html)/i.test(
          state.mimeType ||
            state.responseHeaders.get("content-type") ||
            "",
        )
      ) {
        const mime = /xml/i.test(state.mimeType || "") ? "text/xml" : "text/html";
        this.responseXML = new DOMParser().parseFromString(
          state.responseText,
          mime,
        );
      } else if (state.responseType === "document") {
        this.responseXML = this.response;
      }
      if (!xhrState(this, 4, generation)) return;
      xhrFinish(this, "load", generation);
    }
    _fail(error, type = "error", generation) {
      const state = xhrPrivateStates.get(this);
      if (state.done || state.generation !== generation) return;
      if (!xhrUploadFinish(this, type, generation)) return;
      globalThis.__tilefinchXHRLastError = String(
        (error && error.stack) || error || type,
      );
      this.status = 0;
      this.response = null;
      state.responseText = "";
      if (!xhrState(this, 4, generation)) return;
      xhrFinish(this, type, generation);
    }
    send(body = null) {
      const state = xhrPrivateStates.get(this);
      if (this.readyState !== 1 || state.sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      globalThis.__tilefinchXHRSendCalls++;
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
          contentType = state.headers.get("content-type") || "",
          accept = state.headers.get("accept") || "*/*",
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
            setTimeout(() => xhrFail(this, error, "error", generation), 0);
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
            state.timeoutId = setTimeout(() => {
              if (state.done || state.generation !== generation) return;
              cancelNetwork(
                state.requestId,
                new DOMException("The operation timed out", "TimeoutError"),
                false,
              );
              this.status = 0;
              this.readyState = 4;
              xhrEmit(this, "readystatechange");
              if (state.generation !== generation || state.done) return;
              if (!xhrUploadFinish(this, "timeout", generation)) return;
              xhrFinish(this, "timeout", generation);
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
      const state = xhrPrivateStates.get(this);
      if (state.done) return;
      state.generation += 1;
      const generation = state.generation;
      if (state.timeoutId) clearTimeout(state.timeoutId);
      state.timeoutId = 0;
      if (state.requestId)
        cancelNetwork(
          state.requestId,
          new DOMException("This operation was aborted", "AbortError"),
          false,
        );
      this.status = 0;
      this.readyState = 0;
      if (!xhrUploadFinish(this, "abort", generation)) return;
      xhrFinish(this, "abort", generation);
    }
    getResponseHeader(name) {
      return this.readyState < 2
        ? null
        : xhrPrivateStates.get(this).responseHeaders.get(name);
    }
    getAllResponseHeaders() {
      if (this.readyState < 2) return "";
      let output = "";
      xhrPrivateStates.get(this).responseHeaders.forEach(
        (value, name) => (output += name + ": " + value + "\r\n"),
      );
      return output;
    }
  }
  Object.defineProperty(TilefinchXMLHttpRequest, "name", {
    configurable: true,
    value: "XMLHttpRequest",
  });
  globalThis.__tilefinchXHRResponseCount = 0;
  globalThis.__tilefinchXHRLastStatus = 0;
  globalThis.__tilefinchXHRLastResponseType = "";
  globalThis.__tilefinchXHRLastTextLength = 0;
  globalThis.__tilefinchXHRLastByteLength = 0;
  globalThis.__tilefinchXHRLastStates = "";
  for (const name of [
    "readyState",
    "status",
    "statusText",
    "response",
    "responseURL",
    "responseXML",
    "timeout",
    "upload",
    "withCredentials",
  ])
    Object.defineProperty(TilefinchXMLHttpRequest.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        const state = xhrPublicStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        return state[name];
      },
      set(value) {
        const state = xhrPublicStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        state[name] = value;
      },
    });
  Object.defineProperty(
    TilefinchXMLHttpRequest.prototype,
    "onreadystatechange",
    {
      configurable: true,
      enumerable: true,
      get() {
        return xhrEventTargetStates
          .get(this)
          ?.handlers.get("readystatechange") || null;
      },
      set(value) {
        xhrSetEventHandler(this, "readystatechange", value);
      },
    },
  );
  const xhrStateImpl = TilefinchXMLHttpRequest.prototype._state,
    xhrFinishImpl = TilefinchXMLHttpRequest.prototype._finish,
    xhrApplyImpl = TilefinchXMLHttpRequest.prototype._apply,
    xhrFailImpl = TilefinchXMLHttpRequest.prototype._fail,
    xhrEmitTarget = (target, type, loaded = 0, total = 0,
                     lengthComputable = total > 0) => {
      /* XMLHttpRequest is the user agent, not author script.  Its progress
         events are trusted ProgressEvents in browsers; readystatechange is
         the one plain Event in this sequence.  Challenge runtimes inspect
         both distinctions when deciding whether a response callback came
         from the transport or from an authored redispatch. */
      const event = __tilefinchTrustedEvent(
        type === "readystatechange"
          ? new Event(type)
          : new ProgressEvent(type, {
              lengthComputable,
              loaded,
              total,
            }),
      );
      target.dispatchEvent(event);
    },
    xhrEmit = (xhr, type, loaded = 0, total = 0) =>
      xhrEmitTarget(xhr, type, loaded, total),
    xhrGenerationCurrent = (xhr, generation) =>
      xhrPrivateStates.get(xhr)?.generation === generation,
    xhrUploadStart = (xhr, method, serialized, generation) => {
      const state = xhrPrivateStates.get(xhr);
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
      const state = xhrPrivateStates.get(xhr);
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
      const state = xhrPrivateStates.get(xhr);
      if (state.generation !== generation || state.done) return false;
      state.stateTrace.push(value);
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
      xhrApplyImpl.call(xhr, raw, generation);
      if (xhrGenerationCurrent(xhr, generation)
          && xhr.readyState === 4 && xhr.status !== 0) {
        const state = xhrPrivateStates.get(xhr);
        globalThis.__tilefinchXHRResponseCount++;
        globalThis.__tilefinchXHRLastStatus = xhr.status;
        globalThis.__tilefinchXHRLastResponseType = state.responseType;
        globalThis.__tilefinchXHRLastTextLength = state.responseText.length;
        globalThis.__tilefinchXHRLastByteLength = byteLength;
        globalThis.__tilefinchXHRLastStates = state.stateTrace.join(".");
      }
    },
    xhrFail = (xhr, error, type = "error", generation) => {
      if (!xhrGenerationCurrent(xhr, generation)) return;
      const previous = globalThis.__tilefinchXHRLastError;
      xhrFailImpl.call(xhr, error, type, generation);
      if (type !== "error") globalThis.__tilefinchXHRLastError = previous;
    };
  /* A dedicated worker may be terminated while one of its owner-backed XHR
     transports is still pending.  Abort that transport without dispatching
     callbacks into the retired Worker realm.  The public abort() algorithm is
     deliberately not used here because it synchronously fires abort/loadend. */
  Object.defineProperty(globalThis, "__tilefinchAbortXHRForWorker", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: (xhr) => {
      const state = xhrPrivateStates.get(xhr);
      if (!state) return false;
      state.generation += 1;
      if (state.timeoutId) clearTimeout(state.timeoutId);
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
      xhr.status = 0;
      xhr.readyState = 0;
      xhr.response = null;
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
  Object.defineProperty(globalThis.__tilefinchRootCensus, "pendingNetwork", {
    get: () => pendingNetwork.size,
  });

  /* Small device-state APIs live with the compatibility layer so platform.js
     remains below the source-fallback admission ceiling. Public objects are
     still allocated only when author code first asks for each capability. */
  const platformStatusToken = {},
    permissionNames = new Set([
      "camera",
      "clipboard-read",
      "clipboard-write",
      "geolocation",
      "microphone",
      "notifications",
      "persistent-storage",
    ]);
  let networkInformation = null,
    batteryManager = null,
    batteryPromise = null,
    permissions = null;
  const batteryStates = new TrustedWeakMap(),
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
      this._onchange = null;
    }
    get onchange() { return this._onchange; }
    set onchange(value) {
      this._onchange = typeof value === "function" ? value : null;
    }
    /* The PSP's only network interface is 802.11b Wi-Fi. Report the
       standardized connection kind and the radio's first-hop ceiling rather
       than leaving two members missing from an interface we already expose. */
    get type() { return "wifi"; }
    get downlinkMax() { return 11; }
    get effectiveType() { return "3g"; }
    get rtt() { return 300; }
    get downlink() { return 1.5; }
    get saveData() { return false; }
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
      this._name = name;
      this._onchange = null;
    }
    get name() { return this._name; }
    get state() { return "denied"; }
    get onchange() { return this._onchange; }
    set onchange(value) {
      this._onchange = typeof value === "function" ? value : null;
    }
  }
  class Permissions {
    constructor() {
      if (arguments[0] !== platformStatusToken)
        throw new TypeError("Illegal constructor");
    }
    query(descriptor) {
      if (!(this instanceof Permissions))
        throw new TypeError("Illegal invocation");
      /* Permissions.query() converts its object descriptor and resolves from
         the permissions task source.  In particular, an author getter must
         reject the returned promise rather than escape synchronously, and a
         reaction registered on the result must not overtake microtasks from
         the calling task.  Keep this bounded by the scheduler's existing
         timer quota and do not retain one status object per queried name. */
      return new Promise((resolve, reject) => {
        const task = setTimeout(() => {
          try {
            if (descriptor === null || descriptor === undefined)
              throw new TypeError("Permission descriptor is required");
            const name = String(descriptor.name);
            if (!permissionNames.has(name))
              throw new TypeError("Unsupported permission name");
            resolve(new PermissionStatus(platformStatusToken, name));
          } catch (error) {
            reject(error);
          }
        }, 0);
        if (task === 0)
          reject(new DOMException(
            "Permission query task quota exceeded", "QuotaExceededError"));
      });
    }
  }
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
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        if (networkInformation === null)
          networkInformation = new NetworkInformation(platformStatusToken);
        return networkInformation;
      },
    },
    getBattery: {
      configurable: true,
      enumerable: true,
      value() {
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        if (batteryManager === null)
          batteryManager = new BatteryManager(platformStatusToken);
        /* The Battery Status API stores one promise on each Navigator.  This
           identity is observable and prevents repeated probes from creating
           needless reactions and allocations on the PSP. */
        if (batteryPromise === null)
          batteryPromise = Promise.resolve(batteryManager);
        return batteryPromise;
      },
    },
    permissions: {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof Navigator))
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
})();
