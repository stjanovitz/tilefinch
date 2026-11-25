/* WebGPU's entry points without an adapter: GPU, WGSLLanguageFeatures and
   navigator.gpu's object, built the first time a page reads either global
   or navigator.gpu in a secure context (a ROM-backed lazy module; see
   Module boundaries in README.md). platform.js keeps navigator.gpu's
   getter and receives the object and GPU's own methods, for the Worker
   bridge, through this module's hook publisher. */
(() => {
  const Object = __tilefinchObject;
  /* Loaded after the page may have replaced these globals: build errors,
     promises and collections with what hardening.js captured. */
  const { ownKeys, Set, Promise, TypeError, DOMException, claimHooks } =
    globalThis.__tilefinchEventInterfaces;
  const publish = claimHooks?.();
  const gpuToken = {},
    wgslLanguageFeaturesToken = {};
  const webIDLString = value => {
    /* String(Symbol()) is a special ECMAScript convenience. Web IDL's
       DOMString conversion uses ToString, which rejects symbol values. */
    if (typeof value === "symbol") throw new TypeError("Symbol is not a string");
    return String(value);
  };
  const queuePlatformTask = (operation, quotaLabel = "Platform") =>
    new Promise((resolve, reject) => {
      const task = __tilefinchRealmGlobal.__tilefinchScheduleTask(() => {
        try { resolve(operation()); }
        catch (error) { reject(error); }
      });
      if (task === 0)
        reject(new DOMException(
          quotaLabel + " task quota exceeded", "QuotaExceededError"));
    });
  class WGSLLanguageFeatures {
    constructor() {
      if (arguments[0] !== wgslLanguageFeaturesToken)
        throw new TypeError("Illegal constructor");
    }
    get size() {
      if (this !== wgslLanguageFeatures)
        throw new TypeError("Illegal invocation");
      return 0;
    }
    has(value) {
      if (this !== wgslLanguageFeatures)
        throw new TypeError("Illegal invocation");
      webIDLString(value);
      return false;
    }
    entries() {
      if (this !== wgslLanguageFeatures)
        throw new TypeError("Illegal invocation");
      return emptyWGSLFeatures.entries();
    }
    values() {
      if (this !== wgslLanguageFeatures)
        throw new TypeError("Illegal invocation");
      return emptyWGSLFeatures.values();
    }
    forEach(callback, thisArg = undefined) {
      if (this !== wgslLanguageFeatures)
        throw new TypeError("Illegal invocation");
      if (typeof callback !== "function")
        throw new TypeError("callback must be callable");
      emptyWGSLFeatures.forEach(callback, thisArg);
    }
  }
  const emptyWGSLFeatures = new Set();
  Object.defineProperties(WGSLLanguageFeatures.prototype, {
    keys: {
      configurable: true,
      enumerable: true,
      writable: true,
      value: WGSLLanguageFeatures.prototype.values,
    },
    [Symbol.iterator]: {
      configurable: true,
      enumerable: false,
      writable: true,
      value: WGSLLanguageFeatures.prototype.values,
    },
  });
  for (const name of ["has", "entries", "values", "forEach"])
    Object.defineProperty(WGSLLanguageFeatures.prototype, name, {
      ...Object.getOwnPropertyDescriptor(WGSLLanguageFeatures.prototype, name),
      enumerable: true,
    });
  Object.defineProperty(
    WGSLLanguageFeatures.prototype,
    Symbol.toStringTag,
    { configurable: true, value: "WGSLLanguageFeatures" },
  );
  class GPU {
    constructor() {
      if (arguments[0] !== gpuToken)
        throw new TypeError("Illegal constructor");
    }
    get wgslLanguageFeatures() {
      if (this !== gpu) throw new TypeError("Illegal invocation");
      return wgslLanguageFeatures;
    }
    getPreferredCanvasFormat() {
      if (this !== gpu) throw new TypeError("Illegal invocation");
      return "bgra8unorm";
    }
    requestAdapter(options = {}) {
      /* Even without an admitted WebGPU adapter, Web IDL still converts the
         complete options dictionary. Sites use those ordered conversions
         to distinguish an implementation from a placeholder. Conversion
         failures reject this promise; an unavailable adapter resolves to
         null from the WebGPU task timeline. */
      try {
        if (this !== gpu) throw new TypeError("Illegal invocation");
        if (options === null || options === undefined) options = {};
        else if (typeof options !== "object" && typeof options !== "function")
          throw new TypeError("GPURequestAdapterOptions must be a dictionary");
        const featureValue = options.featureLevel,
          featureLevel = featureValue === undefined
            ? "core" : webIDLString(featureValue);
        const forceFallbackAdapter = Boolean(options.forceFallbackAdapter),
          powerValue = options.powerPreference,
          powerPreference = powerValue === undefined
            ? undefined : webIDLString(powerValue);
        if (powerPreference !== undefined
            && powerPreference !== "low-power"
            && powerPreference !== "high-performance")
          throw new TypeError("Invalid GPU powerPreference");
        const xrCompatible = Boolean(options.xrCompatible);
        /* Preserve all observable conversions while making the PSP's lack
           of a WebGPU adapter explicit. */
        void featureLevel;
        void forceFallbackAdapter;
        void xrCompatible;
      } catch (error) {
        return Promise.reject(error);
      }
      return queuePlatformTask(() => null, "WebGPU");
    }
  }
  Object.defineProperty(GPU.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "GPU",
  });
  const wgslLanguageFeatures = new WGSLLanguageFeatures(
      wgslLanguageFeaturesToken,
    ),
    gpu = new GPU(gpuToken);
  /* Web IDL exposes interface members as enumerable; class syntax defines
     them non-enumerable (platform.js's enumerateInterfaceMembers). */
  for (const constructor of [GPU, WGSLLanguageFeatures]) {
    const prototype = constructor.prototype;
    for (const key of ownKeys(prototype)) {
      if (key === "constructor" || key === Symbol.toStringTag) continue;
      const descriptor = Object.getOwnPropertyDescriptor(prototype, key);
      if (descriptor)
        Object.defineProperty(prototype, key, {
          ...descriptor,
          enumerable: true,
        });
    }
  }
  globalThis.GPU = GPU;
  globalThis.WGSLLanguageFeatures = WGSLLanguageFeatures;
  publish?.("__tilefinchGPUHooks", {
    __proto__: null,
    gpu,
    GPU,
    WGSLLanguageFeatures,
    requestAdapter: GPU.prototype.requestAdapter,
    getPreferredCanvasFormat: GPU.prototype.getPreferredCanvasFormat,
  });
})();
