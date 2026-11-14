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
  /* compat.js's frozen, null-prototype table of the event interfaces it
     captured before author code: Event, EventTarget and the Event
     subclasses it defines, keyed by global name. Lazy modules evaluate
     after the page may have replaced those globals, so they extend these
     instead (`const { Event, EventTarget } = __tilefinchEventInterfaces`).
     Every entry is the object the page itself received at that global, so
     the table grants a page nothing it lacked. */
  defineProperty(globalThis, "__tilefinchEventInterfaces", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: eventInterfaces,
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
