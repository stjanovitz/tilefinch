(() => {
  "use strict";
  /* Date's clock is deterministic already. Keep the same bounded Intl
     implementation used by ordinary pages and the Chrome replay contract. */
  const empty = () => [];
  Object.defineProperties(performance, {
    getEntries: {
      value: empty,
      writable: true,
      enumerable: true,
      configurable: true,
    },
    getEntriesByType: {
      value: empty,
      writable: true,
      enumerable: true,
      configurable: true,
    },
    getEntriesByName: {
      value: empty,
      writable: true,
      enumerable: true,
      configurable: true,
    },
  });
  globalThis.__tilefinchRecordResourceTiming = () => {};
  globalThis.__tilefinchRecordNavigationTiming = () => {};
  class ReplayPerformanceObserver {
    constructor(callback) {
      if (typeof callback !== "function")
        throw new TypeError("callback required");
      this.callback = callback;
    }
    observe() {}
    disconnect() {}
    takeRecords() {
      return [];
    }
  }
  Object.defineProperty(ReplayPerformanceObserver, "supportedEntryTypes", {
    value: [],
    writable: false,
    enumerable: true,
    configurable: true,
  });
  globalThis.PerformanceObserver = ReplayPerformanceObserver;
  delete globalThis.__tilefinchDeterministicDateFacade;
})();
