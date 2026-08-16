(() => {
  "use strict";
  const pad = (value) => String(value).padStart(2, "0");
  class ReplayDateTimeFormat {
    constructor(locales, options = {}) {
      this.locale = String(
        Array.isArray(locales)
          ? locales[0]
          : locales === undefined
            ? "en-US"
            : locales,
      ).replace(/_/g, "-");
      this.options = options || {};
    }
    _date(value) {
      const date = value === undefined ? new Date() : new Date(value);
      if (!Number.isFinite(date.getTime()))
        throw new RangeError("Invalid time value");
      return date;
    }
    format(value) {
      const date = this._date(value),
        options = this.options;
      if (options.timeStyle || options.hour !== undefined) {
        const hour = date.getUTCHours();
        return (
          (hour % 12 || 12) +
          ":" +
          pad(date.getUTCMinutes()) +
          ":" +
          pad(date.getUTCSeconds()) +
          " " +
          (hour < 12 ? "AM" : "PM")
        );
      }
      return (
        date.getUTCMonth() +
        1 +
        "/" +
        date.getUTCDate() +
        "/" +
        date.getUTCFullYear()
      );
    }
    formatToParts(value) {
      const date = this._date(value),
        options = this.options;
      if (options.timeStyle || options.hour !== undefined)
        return [{ type: "literal", value: this.format(date) }];
      return [
        { type: "month", value: String(date.getUTCMonth() + 1) },
        { type: "literal", value: "/" },
        { type: "day", value: String(date.getUTCDate()) },
        { type: "literal", value: "/" },
        { type: "year", value: String(date.getUTCFullYear()) },
      ];
    }
    resolvedOptions() {
      return {
        locale: this.locale,
        calendar: "gregory",
        numberingSystem: "latn",
        timeZone: "UTC",
        year: "numeric",
        month: "numeric",
        day: "numeric",
      };
    }
    static supportedLocalesOf(locales) {
      return (Array.isArray(locales) ? locales : [locales])
        .filter((value) => value !== undefined)
        .map((value) => String(value).replace(/_/g, "-"));
    }
  }
  const DateTimeFormat = function (...args) {
    return new ReplayDateTimeFormat(...args);
  };
  DateTimeFormat.prototype = ReplayDateTimeFormat.prototype;
  Object.setPrototypeOf(DateTimeFormat, ReplayDateTimeFormat);
  Intl.DateTimeFormat = DateTimeFormat;
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
