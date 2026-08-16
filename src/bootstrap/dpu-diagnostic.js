(() => {
  const NativeWritableStream = globalThis.WritableStream;
  globalThis.WritableStream = class extends NativeWritableStream {
    constructor(sink = {}, ...rest) {
      const wrapped = { ...sink };
      for (const name of ["write", "close", "abort"])
        if (typeof sink[name] === "function")
          wrapped[name] = function (...args) {
            try {
              return sink[name].apply(sink, args);
            } catch (error) {
              console.error(
                "writable-stream-" + name + "-error",
                (error && error.stack) || error,
              );
              throw error;
            }
          };
      super(wrapped, ...rest);
    }
  };
  let current;
  Object.defineProperty(globalThis, "__webMobileDeclarativePartialUpdates", {
    configurable: true,
    get() {
      return current;
    },
    set(value) {
      if (value && typeof value.apply === "function") {
        const original = value.apply;
        value.apply = function (...args) {
          try {
            return original.apply(this, args);
          } catch (error) {
            console.error("dpu-apply-error", (error && error.stack) || error);
            throw error;
          }
        };
      }
      current = value;
    },
  });
})();
