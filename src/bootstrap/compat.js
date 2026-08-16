(() => {
  const boundedAncestorPath = globalThis.__tilefinchBoundedAncestorPath,
    ancestorLimit = globalThis.__tilefinchAncestorLimit;
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
  if (globalThis.Intl === undefined) {
    const localeTag = (value) =>
      String(value === undefined ? "en-US" : value).replace(/_/g, "-");
    class Locale {
      constructor(tag) {
        const parts = localeTag(tag).split("-");
        if (!/^[A-Za-z]{2,8}$/.test(parts[0] || ""))
          throw new RangeError("Invalid language tag");
        this.language = parts[0].toLowerCase();
        this.script = "";
        this.region = "";
        for (const part of parts.slice(1)) {
          if (!this.script && /^[A-Za-z]{4}$/.test(part))
            this.script = part[0].toUpperCase() + part.slice(1).toLowerCase();
          else if (!this.region && /^([A-Za-z]{2}|[0-9]{3})$/.test(part))
            this.region = part.toUpperCase();
        }
        this.baseName = [this.language, this.script, this.region]
          .filter(Boolean)
          .join("-");
      }
      toString() {
        return this.baseName;
      }
      maximize() {
        return new Locale(this.baseName);
      }
      minimize() {
        return new Locale(this.baseName);
      }
    }
    class NumberFormat {
      constructor(locales, options = {}) {
        this.locale = localeTag(Array.isArray(locales) ? locales[0] : locales);
        this.options = options || {};
      }
      format(value) {
        let number = Number(value);
        if (this.options.style === "percent") number *= 100;
        const minimum =
            this.options.minimumFractionDigits === undefined
              ? 0
              : Number(this.options.minimumFractionDigits),
          maximum =
            this.options.maximumFractionDigits === undefined
              ? Math.max(minimum, 3)
              : Number(this.options.maximumFractionDigits);
        let text = Number.isFinite(number)
          ? number.toFixed(Math.min(20, maximum))
          : "NaN";
        if (maximum > minimum && text.includes(".")) {
          while (text.endsWith("0") && text.split(".")[1].length > minimum)
            text = text.slice(0, -1);
          if (text.endsWith(".")) text = text.slice(0, -1);
        }
        if (this.options.useGrouping !== false) {
          const pair = text.split(".");
          pair[0] = pair[0].replace(/\B(?=(\d{3})+(?!\d))/g, ",");
          text = pair.join(".");
        }
        if (this.options.style === "percent") text += "%";
        if (this.options.style === "currency")
          text = (this.options.currency || "USD") + " " + text;
        return text;
      }
      formatToParts(value) {
        return [{ type: "integer", value: this.format(value) }];
      }
      resolvedOptions() {
        return {
          locale: this.locale,
          numberingSystem: "latn",
          style: this.options.style || "decimal",
          minimumFractionDigits: this.options.minimumFractionDigits || 0,
          maximumFractionDigits:
            this.options.maximumFractionDigits === undefined
              ? 3
              : this.options.maximumFractionDigits,
          useGrouping: this.options.useGrouping !== false,
          notation: "standard",
          signDisplay: "auto",
        };
      }
      static supportedLocalesOf(locales) {
        return (Array.isArray(locales) ? locales : [locales])
          .filter((value) => value !== undefined)
          .map(localeTag);
      }
    }
    class PluralRules {
      constructor(locales, options = {}) {
        this.locale = localeTag(Array.isArray(locales) ? locales[0] : locales);
        this.options = options || {};
      }
      select(value) {
        const n = Math.abs(Number(value));
        if (this.options.type === "ordinal") {
          const mod10 = n % 10,
            mod100 = n % 100;
          return mod10 === 1 && mod100 !== 11
            ? "one"
            : mod10 === 2 && mod100 !== 12
              ? "two"
              : mod10 === 3 && mod100 !== 13
                ? "few"
                : "other";
        }
        const minF =
            this.options.minimumFractionDigits === undefined
              ? 0
              : Number(this.options.minimumFractionDigits),
          maxF =
            this.options.maximumFractionDigits === undefined
              ? Math.max(minF, 3)
              : Number(this.options.maximumFractionDigits);
        let fraction = Number.isFinite(n)
          ? n.toFixed(Math.min(20, Math.max(0, maxF))).split(".")[1] || ""
          : "";
        while (fraction.length > minF && fraction.endsWith("0"))
          fraction = fraction.slice(0, -1);
        const visible = fraction.length;
        return n === Math.trunc(n) && Math.trunc(n) === 1 && visible === 0
          ? "one"
          : "other";
      }
      resolvedOptions() {
        return {
          locale: this.locale,
          type: this.options.type || "cardinal",
          pluralCategories:
            this.options.type === "ordinal"
              ? ["few", "one", "two", "other"]
              : ["one", "other"],
        };
      }
      static supportedLocalesOf(locales) {
        return NumberFormat.supportedLocalesOf(locales);
      }
    }
    class DateTimeFormat {
      constructor(locales, options = {}) {
        this.locale = localeTag(Array.isArray(locales) ? locales[0] : locales);
        this.options = options || {};
      }
      format(value) {
        const date = value === undefined ? new Date() : new Date(value);
        if (!Number.isFinite(date.getTime()))
          throw new RangeError("Invalid time value");
        if (this.options.timeStyle || this.options.hour !== undefined)
          return date.toLocaleTimeString
            ? date.toLocaleTimeString()
            : date.toISOString().slice(11, 19);
        return date.toISOString().slice(0, 10);
      }
      formatToParts(value) {
        return [{ type: "literal", value: this.format(value) }];
      }
      resolvedOptions() {
        return {
          locale: this.locale,
          calendar: "gregory",
          numberingSystem: "latn",
          timeZone: "UTC",
        };
      }
      static supportedLocalesOf(locales) {
        return NumberFormat.supportedLocalesOf(locales);
      }
    }
    const callable = (Ctor) => {
      const value = function (...args) {
        return new Ctor(...args);
      };
      value.prototype = Ctor.prototype;
      Object.setPrototypeOf(value, Ctor);
      return value;
    };
    globalThis.Intl = {
      Locale,
      NumberFormat: callable(NumberFormat),
      PluralRules,
      DateTimeFormat: callable(DateTimeFormat),
      getCanonicalLocales(locales) {
        return NumberFormat.supportedLocalesOf(locales);
      },
    };
  }
  if (typeof globalThis.Intl.Collator !== "function") {
    const canonicalLocale = (value) => {
        const text = String(value === undefined ? "en-US" : value).replace(
          /_/g,
          "-",
        );
        if (!/^[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*$/.test(text))
          throw new RangeError("Invalid language tag");
        return text;
      },
      supportedLocales = (locales) =>
        (Array.isArray(locales) ? locales : [locales])
          .filter((value) => value !== undefined)
          .map(canonicalLocale);
    class TilefinchCollator {
      constructor(locales, options = {}) {
        this.locale = canonicalLocale(
          Array.isArray(locales) ? locales[0] : locales,
        );
        this.usage = options.usage === "search" ? "search" : "sort";
        this.sensitivity = ["base", "accent", "case", "variant"].includes(
          options.sensitivity,
        )
          ? options.sensitivity
          : "variant";
        this.ignorePunctuation = !!options.ignorePunctuation;
        this.numeric = !!options.numeric;
        this.caseFirst = ["upper", "lower", "false"].includes(options.caseFirst)
          ? options.caseFirst
          : "false";
        this._boundCompare = null;
      }
      get compare() {
        if (!this._boundCompare)
          this._boundCompare = (left, right) => this._compare(left, right);
        return this._boundCompare;
      }
      _fold(value) {
        let text = String(value);
        if (this.sensitivity === "base" || this.sensitivity === "case")
          text = text.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
        if (this.sensitivity === "base" || this.sensitivity === "accent")
          text = text.toLowerCase();
        if (this.ignorePunctuation)
          text = text.replace(/[\s!"#$%&'()*+,./:;<=>?@[\\\]^_`{|}~-]+/g, "");
        return text;
      }
      _compare(left, right) {
        const a = this._fold(left),
          b = this._fold(right);
        if (a === b) return 0;
        if (this.numeric) {
          const aa = a.match(/\d+|\D+/g) || [],
            bb = b.match(/\d+|\D+/g) || [],
            length = Math.min(aa.length, bb.length);
          for (let at = 0; at < length; at++) {
            if (aa[at] === bb[at]) continue;
            if (/^\d+$/.test(aa[at]) && /^\d+$/.test(bb[at])) {
              const av = Number(aa[at]),
                bv = Number(bb[at]);
              if (av !== bv) return av < bv ? -1 : 1;
            }
            return aa[at] < bb[at] ? -1 : 1;
          }
          if (aa.length !== bb.length) return aa.length < bb.length ? -1 : 1;
        }
        return a < b ? -1 : 1;
      }
      resolvedOptions() {
        return {
          locale: this.locale,
          usage: this.usage,
          sensitivity: this.sensitivity,
          ignorePunctuation: this.ignorePunctuation,
          collation: "default",
          numeric: this.numeric,
          caseFirst: this.caseFirst,
        };
      }
      static supportedLocalesOf(locales) {
        return supportedLocales(locales);
      }
    }
    const Collator = function (...args) {
      return new TilefinchCollator(...args);
    };
    Collator.prototype = TilefinchCollator.prototype;
    Object.setPrototypeOf(Collator, TilefinchCollator);
    globalThis.Intl.Collator = Collator;
  }
  {
    const canonicalLocale = (value) =>
        String(value === undefined ? "en-US" : value).replace(/_/g, "-"),
      supportedLocales = (locales) =>
        (Array.isArray(locales) ? locales : [locales])
          .filter((value) => value !== undefined)
          .map(canonicalLocale),
      localeOf = (locales) =>
        canonicalLocale(Array.isArray(locales) ? locales[0] : locales);
    if (typeof Intl.RelativeTimeFormat !== "function") {
      const units = {
          second: "second",
          seconds: "second",
          minute: "minute",
          minutes: "minute",
          hour: "hour",
          hours: "hour",
          day: "day",
          days: "day",
          week: "week",
          weeks: "week",
          month: "month",
          months: "month",
          quarter: "quarter",
          quarters: "quarter",
          year: "year",
          years: "year",
        },
        auto = {
          second: { 0: "now" },
          day: { "-1": "yesterday", 0: "today", 1: "tomorrow" },
          week: { "-1": "last week", 0: "this week", 1: "next week" },
          month: { "-1": "last month", 0: "this month", 1: "next month" },
          quarter: {
            "-1": "last quarter",
            0: "this quarter",
            1: "next quarter",
          },
          year: { "-1": "last year", 0: "this year", 1: "next year" },
        };
      class RelativeTimeFormat {
        constructor(locales, options = {}) {
          this.locale = localeOf(locales);
          this.style = ["long", "short", "narrow"].includes(options.style)
            ? options.style
            : "long";
          this.numeric = options.numeric === "auto" ? "auto" : "always";
        }
        format(value, unit) {
          const number = Number(value),
            name = units[String(unit)];
          if (!name) throw new RangeError("Invalid unit");
          if (!Number.isFinite(number)) throw new RangeError("Invalid value");
          const automatic =
            this.numeric === "auto" ? auto[name]?.[String(number)] : undefined;
          if (automatic !== undefined) return automatic;
          const magnitude = Math.abs(number),
            label =
              this.style === "narrow"
                ? {
                    second: "s",
                    minute: "m",
                    hour: "h",
                    day: "d",
                    week: "w",
                    month: "mo",
                    quarter: "q",
                    year: "y",
                  }[name]
                : name + (magnitude === 1 ? "" : "s");
          return number < 0
            ? magnitude + " " + label + " ago"
            : "in " + magnitude + " " + label;
        }
        formatToParts(value, unit) {
          const text = this.format(value, unit),
            number = String(Math.abs(Number(value))),
            at = text.indexOf(number);
          return at < 0
            ? [{ type: "literal", value: text }]
            : [
                { type: "literal", value: text.slice(0, at) },
                { type: "integer", value: number, unit: units[String(unit)] },
                { type: "literal", value: text.slice(at + number.length) },
              ];
        }
        resolvedOptions() {
          return {
            locale: this.locale,
            style: this.style,
            numeric: this.numeric,
            numberingSystem: "latn",
          };
        }
        static supportedLocalesOf(locales) {
          return supportedLocales(locales);
        }
      }
      Intl.RelativeTimeFormat = RelativeTimeFormat;
    }
    if (typeof Intl.ListFormat !== "function") {
      class ListFormat {
        constructor(locales, options = {}) {
          this.locale = localeOf(locales);
          this.type = ["conjunction", "disjunction", "unit"].includes(
            options.type,
          )
            ? options.type
            : "conjunction";
          this.style = ["long", "short", "narrow"].includes(options.style)
            ? options.style
            : "long";
        }
        _items(values) {
          const items = [];
          for (const value of values) {
            if (typeof value !== "string")
              throw new TypeError("List items must be strings");
            if (items.length >= 4096)
              throw new RangeError("List item limit exceeded");
            items.push(value);
          }
          return items;
        }
        _separator(final = false) {
          if (this.type === "unit")
            return this.style === "long"
              ? ", "
              : this.style === "short"
                ? ", "
                : " ";
          if (this.type === "disjunction") return final ? " or " : ", ";
          return final ? " and " : ", ";
        }
        format(values) {
          const items = this._items(values);
          if (items.length < 2) return items[0] || "";
          if (items.length === 2)
            return items[0] + this._separator(true) + items[1];
          return (
            items.slice(0, -1).join(this._separator(false)) +
            "," +
            this._separator(true) +
            items[items.length - 1]
          );
        }
        formatToParts(values) {
          const items = this._items(values),
            parts = [];
          for (let at = 0; at < items.length; at++) {
            if (at)
              parts.push({
                type: "literal",
                value:
                  items.length === 2
                    ? this._separator(true)
                    : at === items.length - 1
                      ? "," + this._separator(true)
                      : this._separator(false),
              });
            parts.push({ type: "element", value: items[at] });
          }
          return parts;
        }
        resolvedOptions() {
          return { locale: this.locale, type: this.type, style: this.style };
        }
        static supportedLocalesOf(locales) {
          return supportedLocales(locales);
        }
      }
      Intl.ListFormat = ListFormat;
    }
    if (typeof Intl.DisplayNames !== "function") {
      const languages = {
          en: "English",
          es: "Spanish",
          fr: "French",
          de: "German",
          it: "Italian",
          ja: "Japanese",
          ko: "Korean",
          pt: "Portuguese",
          ru: "Russian",
          zh: "Chinese",
          ar: "Arabic",
          hi: "Hindi",
        },
        regions = {
          US: "United States",
          GB: "United Kingdom",
          CA: "Canada",
          DE: "Germany",
          FR: "France",
          JP: "Japan",
          CN: "China",
          IN: "India",
        };
      class DisplayNames {
        constructor(locales, options = {}) {
          if (!options || !options.type)
            throw new TypeError("type is required");
          this.locale = localeOf(locales);
          this.type = String(options.type);
          this.style = ["long", "short", "narrow"].includes(options.style)
            ? options.style
            : "long";
          this.fallback = options.fallback === "none" ? "none" : "code";
          this.languageDisplay =
            options.languageDisplay === "dialect" ? "dialect" : "standard";
        }
        of(code) {
          code = String(code);
          let value;
          if (this.type === "language")
            value = languages[code.toLowerCase().split("-")[0]];
          else if (this.type === "region") value = regions[code.toUpperCase()];
          else if (this.type === "currency")
            value = {
              USD: "US Dollar",
              EUR: "Euro",
              GBP: "British Pound",
              JPY: "Japanese Yen",
            }[code.toUpperCase()];
          else if (this.type === "dateTimeField")
            value = {
              year: "year",
              month: "month",
              week: "week",
              day: "day",
              hour: "hour",
              minute: "minute",
              second: "second",
            }[code];
          else if (this.type === "script" || this.type === "calendar")
            value = undefined;
          else throw new RangeError("Invalid type");
          return value === undefined
            ? this.fallback === "none"
              ? undefined
              : code
            : value;
        }
        resolvedOptions() {
          return {
            locale: this.locale,
            style: this.style,
            type: this.type,
            fallback: this.fallback,
            languageDisplay: this.languageDisplay,
          };
        }
        static supportedLocalesOf(locales) {
          return supportedLocales(locales);
        }
      }
      Intl.DisplayNames = DisplayNames;
    }
  }
  if (typeof globalThis.Intl.Segmenter !== "function") {
    const segLocale = (value) =>
      String(
        value === undefined ? "en-US" : Array.isArray(value) ? value[0] : value,
      ).replace(/_/g, "-");
    class TilefinchSegmenter {
      constructor(locales, options = {}) {
        this.locale = segLocale(locales);
        this.granularity = ["grapheme", "word", "sentence"].includes(
          options.granularity,
        )
          ? options.granularity
          : "grapheme";
      }
      segment(input) {
        const text = String(input),
          segments = [];
        if (this.granularity === "grapheme") {
          let index = 0;
          for (const value of text) {
            segments.push({ segment: value, index, input: text });
            index += value.length;
          }
        } else if (this.granularity === "word") {
          const pattern = /([\p{L}\p{N}_'’]+|\s+|[^\s\p{L}\p{N}_'’]+)/gu;
          let match;
          while ((match = pattern.exec(text)) !== null) {
            segments.push({
              segment: match[0],
              index: match.index,
              input: text,
              isWordLike: /[\p{L}\p{N}_]/u.test(match[0]),
            });
          }
        } else {
          const pattern = /[^.!?\n]*(?:[.!?\n]+\s*|$)/g;
          let match;
          while ((match = pattern.exec(text)) !== null && match[0] !== "") {
            segments.push({
              segment: match[0],
              index: match.index,
              input: text,
            });
            if (pattern.lastIndex === match.index) pattern.lastIndex++;
          }
        }
        const containing = (at) => {
          at = Math.floor(Number(at) || 0);
          for (const entry of segments) {
            if (at >= entry.index && at < entry.index + entry.segment.length)
              return entry;
          }
          return undefined;
        };
        return {
          [Symbol.iterator]() {
            return segments[Symbol.iterator]();
          },
          containing,
        };
      }
      resolvedOptions() {
        return { locale: this.locale, granularity: this.granularity };
      }
      static supportedLocalesOf(locales) {
        return (Array.isArray(locales) ? locales : [locales])
          .filter((value) => value !== undefined)
          .map(segLocale);
      }
    }
    globalThis.Intl.Segmenter = TilefinchSegmenter;
  }
  {
    Object.defineProperty(document, "visibilityState", {
      configurable: true,
      enumerable: true,
      get() {
        return "visible";
      },
    });
    Object.defineProperty(document, "hidden", {
      configurable: true,
      enumerable: true,
      get() {
        return false;
      },
    });
    document.onDOMContentLoaded = null;
    document.exitPointerLock = function () {};
    navigator.product = "Gecko";
    navigator.vendor = "";
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
  globalThis.Event = class Event {
    constructor(type, options = {}) {
      if (arguments.length < 1)
        throw new TypeError("Event type is required");
      this.type = String(type);
      this.bubbles = !!options.bubbles;
      this.cancelable = !!options.cancelable;
      this.composed = !!options.composed;
      this.defaultPrevented = false;
      this.eventPhase = 0;
      this.currentTarget = null;
      this.target = null;
      this.timeStamp = __tilefinchPerformanceSample(9);
      this.__initialized = true;
      this.__dispatching = false;
      this.__stopped = false;
      this.__immediateStopped = false;
      Object.defineProperty(this, "isTrusted", {
        value: false,
        configurable: true,
      });
    }
    preventDefault() {
      if (this.cancelable) this.defaultPrevented = true;
    }
    stopPropagation() {
      this.__stopped = true;
    }
    stopImmediatePropagation() {
      this.__stopped = true;
      this.__immediateStopped = true;
    }
    composedPath() {
      return this.__path?.slice() || [];
    }
    initEvent(type, bubbles = false, cancelable = false) {
      if (arguments.length < 1)
        throw new TypeError("Event type is required");
      if (this.__dispatching) return;
      this.type = String(type);
      this.bubbles = !!bubbles;
      this.cancelable = !!cancelable;
      this.defaultPrevented = false;
      this.__initialized = true;
      this.__stopped = false;
      this.__immediateStopped = false;
    }
  };
  Object.defineProperties(Event.prototype, {
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
        options.pointerId === undefined ? 1 : options.pointerId,
      );
      this.width = Number(options.width === undefined ? 1 : options.width);
      this.height = Number(options.height === undefined ? 1 : options.height);
      this.pressure = Number(options.pressure) || 0;
      this.tangentialPressure = Number(options.tangentialPressure) || 0;
      this.tiltX = Number(options.tiltX) || 0;
      this.tiltY = Number(options.tiltY) || 0;
      this.twist = Number(options.twist) || 0;
      this.pointerType = String(options.pointerType || "mouse");
      this.isPrimary = options.isPrimary !== false;
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
      this.lengthComputable = !!options.lengthComputable;
      this.loaded = Math.max(0, Number(options.loaded) || 0);
      this.total = Math.max(0, Number(options.total) || 0);
    }
  };
  globalThis.MessageEvent = class MessageEvent extends Event {
    constructor(type, options = {}) {
      super(type, options);
      this.data = options.data === undefined ? null : options.data;
      this.origin = String(options.origin || "");
      this.lastEventId = String(options.lastEventId || "");
      this.source = options.source ?? null;
      this.ports = Array.from(options.ports || []);
    }
  };
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
      const event = new ProgressEvent(type, {
        lengthComputable: true,
        loaded,
        total,
      });
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
          this.result = convert(blob._bytes);
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
  globalThis.__tilefinchIsFocusable = isFocusable;
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
        ? document
            .querySelectorAll("input")
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
      const own = Object.prototype.hasOwnProperty.call(this, "insertBefore")
        ? this.insertBefore
        : null;
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
        Object.prototype.hasOwnProperty.call(parent, "append")
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
              event.currentTarget = null;
              event.eventPhase = Event.NONE;
              event.__dispatching = false;
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
  globalThis.AbortSignal = class AbortSignal {
    constructor() {
      this.aborted = false;
      this.reason = undefined;
      this.onabort = null;
      this._listeners = [];
    }
    addEventListener(type, callback, options = {}) {
      if (String(type) !== "abort" || typeof callback !== "function") return;
      if (!this._listeners.some((item) => item.callback === callback))
        this._listeners.push({ callback, once: !!options?.once });
    }
    removeEventListener(type, callback) {
      if (String(type) !== "abort") return;
      this._listeners = this._listeners.filter(
        (item) => item.callback !== callback,
      );
    }
    dispatchEvent(event) {
      if (String(event?.type) !== "abort") return true;
      event.target = this;
      if (typeof this.onabort === "function")
        globalThis.__tilefinchRunTask(
          "abort-handler",
          this.onabort,
          this,
          [event],
        );
      for (const item of [...this._listeners]) {
        globalThis.__tilefinchRunTask(
          "abort-listener",
          item.callback,
          this,
          [event],
        );
        if (item.once) this.removeEventListener("abort", item.callback);
      }
      return true;
    }
    throwIfAborted() {
      if (this.aborted) throw this.reason;
    }
    static abort(
      reason = new DOMException("This operation was aborted", "AbortError"),
    ) {
      const signal = new AbortSignal();
      signal._abort(reason);
      return signal;
    }
    static timeout(milliseconds) {
      const signal = new AbortSignal();
      setTimeout(
        () =>
          signal._abort(
            new DOMException("The operation timed out", "TimeoutError"),
          ),
        Math.max(0, Number(milliseconds) || 0),
      );
      return signal;
    }
    _abort(reason) {
      if (this.aborted) return;
      this.aborted = true;
      this.reason =
        reason === undefined
          ? new DOMException("This operation was aborted", "AbortError")
          : reason;
      this.dispatchEvent(new Event("abort"));
    }
  };
  globalThis.AbortController = class AbortController {
    constructor() {
      this.signal = new AbortSignal();
    }
    abort(reason) {
      this.signal._abort(reason);
    }
  };
  const httpTokenPattern = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
  const isHttpToken = (value) => httpTokenPattern.test(String(value));
  const invalidHeaderValue = (value) => /[\x00-\x1f\x7f]/.test(String(value));
  const normalizeHeaderName = (name) => {
    name = String(name);
    if (!isHttpToken(name)) throw new TypeError("Invalid HTTP header name");
    return name.toLowerCase();
  };
  const normalizeHeaderValue = (value) => {
    value = String(value);
    if (invalidHeaderValue(value))
      throw new TypeError("Invalid HTTP header value");
    return value.replace(/^ +| +$/g, "");
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
      if (this.readyState !== 1 || this._sent || !this._done)
        throw new DOMException("Request is not open", "InvalidStateError");
      [name, value] = normalizeXhrHeader(name, value);
      return setRequestHeader.call(this, name, value);
    };
  };
  class Headers {
    constructor(init = {}) {
      this.map = new Map();
      if (init instanceof Headers) {
        init.forEach((v, k) => this.set(k, v));
      } else if (typeof init === "string") {
        for (const line of init.split("\n")) {
          if (!line) continue;
          const at = line.indexOf(":");
          if (at <= 0) throw new TypeError("Invalid HTTP header block");
          this.append(line.slice(0, at), line.slice(at + 1));
        }
      } else if (Array.isArray(init)) {
        for (const pair of init) {
          if (!pair || pair.length !== 2)
            throw new TypeError("Header pair must contain two values");
          this.append(pair[0], pair[1]);
        }
      } else if (init && typeof init === "object") {
        for (const key of Object.keys(init)) this.set(key, init[key]);
      }
    }
    append(name, value) {
      name = normalizeHeaderName(name);
      value = normalizeHeaderValue(value);
      this.map.set(
        name,
        this.map.has(name) ? this.map.get(name) + ", " + value : value,
      );
    }
    set(name, value) {
      this.map.set(normalizeHeaderName(name), normalizeHeaderValue(value));
    }
    get(name) {
      return this.map.get(normalizeHeaderName(name)) ?? null;
    }
    has(name) {
      return this.map.has(normalizeHeaderName(name));
    }
    delete(name) {
      this.map.delete(normalizeHeaderName(name));
    }
    forEach(callback, thisArg) {
      for (const [key, value] of this.map)
        callback.call(thisArg, value, key, this);
    }
    entries() {
      return this.map.entries();
    }
    [Symbol.iterator]() {
      return this.entries();
    }
  }
  let Response;
  class Request {
    constructor(input, init = {}) {
      const prior = input instanceof Request ? input : null;
      if (prior && prior.bodyUsed && init.body === undefined)
        throw new TypeError("Body has already been consumed");
      this.url = prior
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
      this.method = method.toUpperCase();
      this.headers = new Headers(
        init.headers === undefined
          ? prior
            ? prior.headers
            : {}
          : init.headers,
      );
      this._bodySource =
        init.body === undefined
          ? prior
            ? prior._bodySource
            : null
          : init.body;
      if (
        (this.method === "GET" || this.method === "HEAD") &&
        this._bodySource !== null &&
        this._bodySource !== undefined
      )
        throw new TypeError("GET and HEAD requests cannot have a body");
      this.bodyUsed = false;
      this._bodyBytes = () => {
        const body = this._bodySource;
        if (body === null || body === undefined) return new Uint8Array();
        if (body instanceof Blob) return body._bytes.slice();
        if (body instanceof ArrayBuffer) return new Uint8Array(body.slice(0));
        if (ArrayBuffer.isView(body))
          return new Uint8Array(
            body.buffer.slice(
              body.byteOffset,
              body.byteOffset + body.byteLength,
            ),
          );
        return new TextEncoder().encode(String(body));
      };
      this.body =
        this._bodySource === null || this._bodySource === undefined
          ? null
          : new ReadableStream({
              start: (controller) => {
                controller.enqueue(this._bodyBytes());
                controller.close();
              },
            });
      this.mode = String(
        init.mode === undefined ? (prior ? prior.mode : "cors") : init.mode,
      );
      this.credentials = String(
        init.credentials === undefined
          ? prior
            ? prior.credentials
            : "same-origin"
          : init.credentials,
      );
      if (!["same-origin", "cors", "no-cors"].includes(this.mode))
        throw new TypeError("Invalid request mode");
      if (!["omit", "same-origin", "include"].includes(this.credentials))
        throw new TypeError("Invalid credentials mode");
      this.cache = String(
        init.cache === undefined ? (prior ? prior.cache : "default") : init.cache,
      );
      if (
        ![
          "default",
          "no-store",
          "reload",
          "no-cache",
          "force-cache",
          "only-if-cached",
        ].includes(this.cache)
      )
        throw new TypeError("Invalid request cache mode");
      if (this.cache === "only-if-cached" && this.mode !== "same-origin")
        throw new TypeError("only-if-cached requires same-origin mode");
      this.redirect = String(
        init.redirect === undefined
          ? prior
            ? prior.redirect
            : "follow"
          : init.redirect,
      );
      if (!["follow", "error", "manual"].includes(this.redirect))
        throw new TypeError("Invalid redirect mode");
      this.referrer = String(
        init.referrer === undefined
          ? prior
            ? prior.referrer
            : "about:client"
          : init.referrer,
      );
      this.referrerPolicy = String(
        init.referrerPolicy === undefined
          ? prior
            ? prior.referrerPolicy
            : ""
          : init.referrerPolicy,
      );
      this.integrity = String(
        init.integrity === undefined
          ? prior
            ? prior.integrity
            : ""
          : init.integrity,
      );
      this.keepalive =
        init.keepalive === undefined
          ? prior
            ? prior.keepalive
            : false
          : !!init.keepalive;
      this.destination = "";
      this.signal =
        init.signal ||
        (prior ? prior.signal : null) ||
        new AbortController().signal;
    }
    _consume() {
      if (this.bodyUsed) throw new TypeError("Body has already been consumed");
      this.bodyUsed = true;
      const serialized = serializeRequestBody(this);
      if (serialized === undefined) return new Uint8Array();
      if (typeof serialized === "string")
        return new TextEncoder().encode(serialized);
      return new Uint8Array(serialized);
    }
    arrayBuffer() {
      try {
        return Promise.resolve(this._consume().buffer);
      } catch (error) {
        return Promise.reject(error);
      }
    }
    bytes() {
      try {
        return Promise.resolve(this._consume());
      } catch (error) {
        return Promise.reject(error);
      }
    }
    text() {
      try {
        return Promise.resolve(new TextDecoder().decode(this._consume()));
      } catch (error) {
        return Promise.reject(error);
      }
    }
    json() {
      return this.text().then(JSON.parse);
    }
    blob() {
      try {
        return Promise.resolve(
          new Blob([this._consume()], {
            type: this.headers.get("content-type") || "",
          }),
        );
      } catch (error) {
        return Promise.reject(error);
      }
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
      if (this.bodyUsed) throw new TypeError("Body has already been consumed");
      return new Request(this);
    }
  }
  globalThis.Headers = Headers;
  globalThis.Request = Request;
  {
    const subtle = {
      digest(algorithm, data) {
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
          let bytes;
          if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
          else if (ArrayBuffer.isView(data)) {
            if (
              typeof SharedArrayBuffer !== "undefined" &&
              data.buffer instanceof SharedArrayBuffer
            )
              throw new TypeError("Shared BufferSource is not supported");
            bytes = new Uint8Array(
              data.buffer,
              data.byteOffset,
              data.byteLength,
            );
          } else throw new TypeError("BufferSource required");
          return Promise.resolve(__tilefinchCryptoDigestSHA256(bytes));
        } catch (error) {
          return Promise.reject(error);
        }
      },
    };
    const integerRandomViews = new Set([
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
    globalThis.crypto = {
      getRandomValues(array) {
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
      },
      randomUUID() {
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
      },
      subtle,
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
      log: emit("log"),
      info: emit("info"),
      warn: emit("warn"),
      error: emit("error"),
      debug: emit("debug"),
      trace: emit("trace"),
      group: noop,
      groupEnd: noop,
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
    "if-modified-since",
    "if-none-match",
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
    headers.forEach((value, name) => {
      name = String(name).toLowerCase();
      value = String(value);
      if (!nativeForbiddenHeader(name)) lines.push(name + ": " + value);
    });
    return lines.join("\n");
  };
  const serializeRequestBody = (request) => {
    const source = request._bodySource;
    if (source === null || source === undefined) return undefined;
    if (source instanceof URLSearchParams) {
      if (!request.headers.has("content-type"))
        request.headers.set(
          "content-type",
          "application/x-www-form-urlencoded;charset=UTF-8",
        );
      return source.toString();
    }
    if (source instanceof FormData) {
      const boundary = "----tilefinch-form-boundary",
        chunks = [],
        encoder = new TextEncoder();
      let total = 0;
      const append = (bytes) => {
        if (!(bytes instanceof Uint8Array)) bytes = encoder.encode(String(bytes));
        if (total + bytes.byteLength > 256 * 1024)
          throw new RangeError("FormData body exceeds bounded size");
        chunks.push(bytes);
        total += bytes.byteLength;
      };
      const quote = (value) =>
        String(value).replaceAll("\\", "\\\\").replaceAll('"', '\\"');
      for (const [name, value] of source) {
        append("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + quote(name) + "\"");
        if (value instanceof File) append('; filename="' + quote(value.name) + '"');
        append("\r\n");
        if (value instanceof Blob && value.type)
          append("Content-Type: " + value.type + "\r\n");
        append("\r\n");
        append(value instanceof Blob ? value._bytes : String(value));
        append("\r\n");
      }
      append("--" + boundary + "--\r\n");
      const bytes = new Uint8Array(total);
      let offset = 0;
      for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.byteLength;
      }
      request.headers.set(
        "content-type",
        "multipart/form-data; boundary=" + boundary,
      );
      return bytes.buffer;
    }
    if (source instanceof Blob) {
      if (source.type && !request.headers.has("content-type"))
        request.headers.set("content-type", source.type);
      return source._bytes.buffer.slice(
        source._bytes.byteOffset,
        source._bytes.byteOffset + source._bytes.byteLength,
      );
    }
    if (source instanceof ArrayBuffer) return source;
    if (ArrayBuffer.isView(source))
      return source.buffer.slice(
        source.byteOffset,
        source.byteOffset + source.byteLength,
      );
    return String(source);
  };
  const networkQueueLimit = 128,
    networkQueueByteLimit = 256 * 1024,
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
      entry.state = "launching";
      try {
        const nativeId = Number(entry.start());
        if (!Number.isSafeInteger(nativeId) || nativeId <= 0)
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
  const queueNetwork = (start, bytes, resolve, reject) => {
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
  globalThis.__tilefinchDeliverNetwork = (id, ok, value) => {
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
  globalThis.__tilefinchXHRSendCalls = 0;
  globalThis.__tilefinchXHRLastError = "";
  Response = class Response {
    constructor(body = null, init = {}) {
      const streamBody = body instanceof ReadableStream ? body : null;
      let supplied = init.bodyBytes;
      if (supplied === undefined && body instanceof Blob)
        supplied = body._bytes.slice().buffer;
      else if (supplied === undefined && body instanceof ArrayBuffer)
        supplied = body.slice(0);
      else if (supplied === undefined && ArrayBuffer.isView(body))
        supplied = body.buffer.slice(
          body.byteOffset,
          body.byteOffset + body.byteLength,
        );
      this._bytes =
        supplied instanceof ArrayBuffer ? new Uint8Array(supplied) : null;
      this._body = this._bytes || streamBody
        ? null
        : String(body === undefined || body === null ? "" : body);
      this._streamBody = !!streamBody;
      this._bodyText = () => {
        if (this._streamBody)
          throw new TypeError("Streaming body requires asynchronous consumption");
        if (this._body === null) {
          this._body = new TextDecoder().decode(
            this._bytes || new Uint8Array(),
          );
          this._bytes = null;
        }
        return this._body;
      };
      this._bodyBytes = () => {
        if (this._streamBody)
          throw new TypeError("Streaming body requires asynchronous consumption");
        /* Direct text()/arrayBuffer()/blob() consumption releases both
           retained representations. A ReadableStream pull already queued by
           the runtime may run afterward; it must not recreate an empty
           retained byte buffer and pin it for the Response lifetime. */
        if (this.bodyUsed && this._bytes === null && this._body === null)
          return new Uint8Array();
        if (!this._bytes) {
          this._bytes = new TextEncoder().encode(this._body || "");
          if (this.bodyUsed) this._body = null;
        }
        return this._bytes;
      };
      this.status = Number(init.status === undefined ? 200 : init.status);
      if (
        this.status !== 0 &&
        (!Number.isInteger(this.status) ||
          this.status < 200 ||
          this.status > 599)
      )
        throw new RangeError("Invalid response status");
      if (
        body !== null &&
        body !== undefined &&
        [101, 204, 205, 304].includes(this.status)
      )
        throw new TypeError("Response status cannot have a body");
      this.statusText = String(
        init.statusText === undefined ? "" : init.statusText,
      );
      this.url = String(init.url === undefined ? "" : init.url);
      this.headers =
        init.headers instanceof Headers
          ? new Headers(init.headers)
          : new Headers(init.headers);
      this.ok = this.status >= 200 && this.status < 300;
      this.redirected = !!init.redirected;
      this.type = String(init.type || "default");
      this.bodyUsed = false;
      const bytesOf = this._bodyBytes;
      let at = 0;
      this.body =
        body === null || body === undefined
          ? null
          : streamBody ||
            new ReadableStream({
              pull(controller) {
                const bytes = bytesOf();
                if (at >= bytes.length) {
                  controller.close();
                  return;
                }
                const end = Math.min(bytes.length, at + 4096);
                controller.enqueue(bytes.slice(at, end));
                at = end;
                if (at >= bytes.length) controller.close();
              },
            });
      if (!this.body) return;
      const getReader = this.body.getReader.bind(this.body),
        cancel = this.body.cancel.bind(this.body);
      this._bodyGetReader = getReader;
      this.body.getReader = () => {
        if (this.bodyUsed)
          throw new TypeError("Body has already been consumed");
        this.bodyUsed = true;
        return getReader();
      };
      this.body.cancel = (reason) => {
        if (this.bodyUsed)
          return Promise.reject(
            new TypeError("Body has already been consumed"),
          );
        this.bodyUsed = true;
        return cancel(reason);
      };
    }
  };
  globalThis.Response = Response;
  {
    const consume = (response) => {
        if (response.bodyUsed)
          throw new TypeError("Body has already been consumed");
        response.bodyUsed = true;
      },
      takeText = (response) => {
        const text = response._bodyText();
        response._bytes = null;
        response._body = null;
        return text;
      },
      takeBuffer = (response) => {
        const view = response._bodyBytes(),
          buffer =
            view.byteOffset === 0 && view.byteLength === view.buffer.byteLength
              ? view.buffer
              : view.slice().buffer;
        response._bytes = null;
        response._body = null;
        return buffer;
      },
      takeBytes = async (response) => {
        if (!response._streamBody)
          return new Uint8Array(takeBuffer(response));
        const reader = response._bodyGetReader(),
          chunks = [];
        let total = 0;
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          const bytes =
            value instanceof Uint8Array
              ? value
              : value instanceof ArrayBuffer
                ? new Uint8Array(value)
                : new TextEncoder().encode(String(value));
          if (total + bytes.byteLength > 256 * 1024)
            throw new RangeError("Response body exceeds bounded size");
          chunks.push(bytes);
          total += bytes.byteLength;
        }
        const joined = new Uint8Array(total);
        let offset = 0;
        for (const chunk of chunks) {
          joined.set(chunk, offset);
          offset += chunk.byteLength;
        }
        response._streamBody = false;
        response._bytes = null;
        response._body = null;
        return joined;
      };
    Response.prototype.text = function () {
      try {
        consume(this);
        if (!this._streamBody) return Promise.resolve(takeText(this));
        return takeBytes(this).then((bytes) => new TextDecoder().decode(bytes));
      } catch (error) {
        return Promise.reject(error);
      }
    };
    Response.prototype.json = function () {
      return this.text().then(JSON.parse);
    };
    Response.prototype.arrayBuffer = function () {
      try {
        consume(this);
        if (!this._streamBody) return Promise.resolve(takeBuffer(this));
        return takeBytes(this).then((bytes) => bytes.buffer);
      } catch (error) {
        return Promise.reject(error);
      }
    };
    Response.prototype.bytes = function () {
      return this.arrayBuffer().then((buffer) => new Uint8Array(buffer));
    };
    Response.prototype.blob = function () {
      try {
        consume(this);
        if (!this._streamBody) {
          const blob = new Blob([this._bodyBytes()], {
            type: this.headers.get("content-type") || "",
          });
          this._bytes = null;
          this._body = null;
          return Promise.resolve(blob);
        }
        return takeBytes(this).then(
          (bytes) =>
            new Blob([bytes], {
              type: this.headers.get("content-type") || "",
            }),
        );
      } catch (error) {
        return Promise.reject(error);
      }
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
      if (this.bodyUsed) throw new TypeError("Body has already been consumed");
      const init = {
        status: this.status,
        statusText: this.statusText,
        url: this.url,
        headers: this.headers,
        redirected: this.redirected,
        type: this.type,
      };
      if (this._streamBody) {
        const [first, second] = this.body.tee();
        this.body = first;
        const firstGetReader = first.getReader.bind(first),
          firstCancel = first.cancel.bind(first);
        this._bodyGetReader = firstGetReader;
        first.getReader = () => {
          if (this.bodyUsed)
            throw new TypeError("Body has already been consumed");
          this.bodyUsed = true;
          return firstGetReader();
        };
        first.cancel = (reason) => {
          if (this.bodyUsed)
            return Promise.reject(
              new TypeError("Body has already been consumed"),
            );
          this.bodyUsed = true;
          return firstCancel(reason);
        };
        return new Response(second, init);
      }
      if (this._bytes) init.bodyBytes = this._bytes.slice().buffer;
      return new Response(
        this.body === null ? null : this._bytes ? undefined : this._body,
        init,
      );
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
  }
  globalThis.fetch = (input, init = {}) => {
    try {
      const request =
        input instanceof Request
          ? new Request(input, init)
          : new Request(input, init);
      request.signal.throwIfAborted();
      const body = serializeRequestBody(request),
        contentType = request.headers.get("content-type") || "",
        headerBlock = nativeHeaderBlock(request.headers),
        signal = request.signal,
        method = request.method,
        url = request.url,
        mode = request.mode,
        credentials = request.credentials;
      return new Promise((resolve, reject) => {
        let id = 0;
        const abort = () =>
          cancelNetwork(
            id,
            signal.reason ||
              new DOMException("This operation was aborted", "AbortError"),
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
            signal.removeEventListener("abort", abort);
            try {
              signal.throwIfAborted();
              resolve(
                new Response(raw.body, {
                  status: raw.status,
                  url: raw.url,
                  headers: new Headers(
                    raw.headers || "content-type: " + raw.contentType + "\n",
                  ),
                  bodyBytes: raw.bodyBytes,
                }),
              );
            } catch (error) {
              reject(error);
            }
          },
          (error) => {
            signal.removeEventListener("abort", abort);
            reject(error);
          },
        );
        if (pendingNetwork.has(id)) {
          signal.addEventListener("abort", abort, { once: true });
          if (signal.aborted) abort();
        }
      });
    } catch (error) {
      return Promise.reject(error);
    }
  };
  navigator.sendBeacon = (url, data = null) => {
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
  };
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
    /*
     * The PSP SDK's libcurl 7.64 has no WebSocket framing API. Expose the
     * standard object and deterministic asynchronous failure lifecycle, but
     * never claim OPEN or send data until a native duplex transport is
     * installed. This is safer than a polling protocol disguised as a socket.
     */
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
        const list =
          typeof protocols === "string" ? [protocols] : Array.from(protocols);
        if (
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
        this.binaryType = "blob";
        this.onopen = null;
        this.onmessage = null;
        this.onerror = null;
        this.onclose = null;
        this._task = setTimeout(() => {
          if (this.readyState !== WebSocket.CONNECTING) return;
          this.readyState = WebSocket.CLOSED;
          const error = new Event("error");
          this.dispatchEvent(error);
          if (typeof this.onerror === "function") this.onerror(error);
          const close = new CloseEvent("close", {
            code: 1006,
            reason: "duplex transport unavailable",
            wasClean: false,
          });
          this.dispatchEvent(close);
          if (typeof this.onclose === "function") this.onclose(close);
        }, 0);
      }
      send() {
        if (this.readyState === WebSocket.CONNECTING)
          throw new DOMException("WebSocket is connecting", "InvalidStateError");
        if (this.readyState !== WebSocket.OPEN) return;
      }
      close(code, reason = "") {
        if (
          code !== undefined &&
          code !== 1000 &&
          (Number(code) < 3000 || Number(code) > 4999)
        )
          throw new DOMException("Invalid WebSocket close code", "InvalidAccessError");
        if (new TextEncoder().encode(String(reason)).length > 123)
          throw new DOMException("WebSocket close reason is too long", "SyntaxError");
        clearTimeout(this._task);
        this.readyState = WebSocket.CLOSED;
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
  }
  class TilefinchXMLHttpRequest {
    constructor() {
      this.readyState = 0;
      this.status = 0;
      this.statusText = "";
      this.response = null;
      this.responseURL = "";
      this.responseXML = null;
      this.responseHeaders = new Headers();
      this.onreadystatechange = null;
      this.onloadstart = null;
      this.onprogress = null;
      this.onload = null;
      this.onerror = null;
      this.onabort = null;
      this.onloadend = null;
      this.ontimeout = null;
      this.timeout = 0;
      this.withCredentials = false;
      this.upload = { addEventListener() {}, removeEventListener() {} };
      this.method = "GET";
      this.url = "";
      this.async = true;
      this.headers = new Headers();
      this.listeners = new Map();
      this._responseType = "";
      this._responseText = "";
      this._requestId = 0;
      this._timeoutId = 0;
      this._done = true;
      this._sent = false;
    }
    get responseType() {
      return this._responseType;
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
      this._responseType = value;
    }
    get responseText() {
      if (this._responseType !== "" && this._responseType !== "text")
        throw new DOMException(
          "responseText is unavailable for this response type",
          "InvalidStateError",
        );
      return this._responseText;
    }
    open(method, url, async = true) {
      if (!this._done && this._requestId)
        cancelNetwork(
          this._requestId,
          new DOMException("Request reopened", "AbortError"),
          false,
        );
      this.method = String(method).toUpperCase();
      this.url = String(url);
      this.async = !!async;
      this.status = 0;
      this.statusText = "";
      this.response = null;
      this.responseURL = "";
      this.responseXML = null;
      this.responseHeaders = new Headers();
      this.headers = new Headers();
      this._responseText = "";
      this._requestId = 0;
      this._done = true;
      this._sent = false;
      this.readyState = 1;
      this.emit("readystatechange");
    }
    setRequestHeader(name, value) {
      if (this.readyState !== 1 || this._sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      this.headers.append(name, value);
    }
    overrideMimeType(type) {
      if (this.readyState === 3 || this.readyState === 4)
        throw new DOMException("Response is loading", "InvalidStateError");
      this._mimeType = String(type);
    }
    addEventListener(type, callback) {
      if (typeof callback !== "function") return;
      const key = String(type);
      if (!this.listeners.has(key)) this.listeners.set(key, []);
      this.listeners.get(key).push(callback);
    }
    removeEventListener(type, callback) {
      const list = this.listeners.get(String(type));
      if (!list) return;
      const at = list.indexOf(callback);
      if (at >= 0) list.splice(at, 1);
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
      this.emit("readystatechange");
    }
    _finish(type) {
      if (this._done) return;
      this._done = true;
      if (this._timeoutId) clearTimeout(this._timeoutId);
      this._timeoutId = 0;
      this._requestId = 0;
      this.emit(type);
      this.emit("loadend");
    }
    _apply(raw) {
      if (this._done) return;
      this.status = Number(raw.status) || 0;
      this.responseURL = raw.url || this.url;
      this.responseHeaders = new Headers(
        raw.headers || "content-type: " + raw.contentType + "\n",
      );
      this._state(2);
      const supplied =
          raw.bodyBytes instanceof ArrayBuffer
            ? new Uint8Array(raw.bodyBytes)
            : null,
        needsText =
          this._responseType === "" ||
          this._responseType === "text" ||
          this._responseType === "json" ||
          this._responseType === "document" ||
          /(?:xml|html)/i.test(
            this._mimeType ||
              this.responseHeaders.get("content-type") ||
              "",
          );
      this._responseText = needsText
        ? raw.body !== undefined
          ? String(raw.body)
          : new TextDecoder().decode(supplied || new Uint8Array())
        : "";
      this._state(3);
      const fallbackBody = raw.body === undefined ? "" : String(raw.body),
        byteLength = Number.isFinite(Number(raw.bodyLength))
          ? Math.max(0, Number(raw.bodyLength))
          : supplied
            ? supplied.byteLength
            : new TextEncoder().encode(fallbackBody).byteLength;
      this.emit("progress", byteLength, byteLength);
      if (this._responseType === "" || this._responseType === "text")
        this.response = this._responseText;
      else if (this._responseType === "json") {
        try {
          this.response = JSON.parse(this._responseText);
        } catch (_) {
          this.response = null;
        }
      } else if (this._responseType === "arraybuffer") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        this.response =
          bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
            ? bytes.buffer
            : bytes.slice().buffer;
      } else if (this._responseType === "blob") {
        const bytes = supplied || new TextEncoder().encode(fallbackBody);
        this.response = new Blob([bytes], {
          type: this.responseHeaders.get("content-type") || "",
        });
      } else if (this._responseType === "document") {
        const mime =
          /xml/i.test(
            this._mimeType ||
              this.responseHeaders.get("content-type") ||
              "",
          )
            ? "text/xml"
            : "text/html";
        this.response = new DOMParser().parseFromString(
          this._responseText,
          mime,
        );
      } else this.response = null;
      if (
        this._responseType === "" &&
        /(?:xml|html)/i.test(
          this._mimeType ||
            this.responseHeaders.get("content-type") ||
            "",
        )
      ) {
        const mime = /xml/i.test(this._mimeType || "") ? "text/xml" : "text/html";
        this.responseXML = new DOMParser().parseFromString(
          this._responseText,
          mime,
        );
      } else if (this._responseType === "document") {
        this.responseXML = this.response;
      }
      this._state(4);
      this._finish("load");
    }
    _fail(error, type = "error") {
      if (this._done) return;
      globalThis.__tilefinchXHRLastError = String(
        (error && error.stack) || error || type,
      );
      this.status = 0;
      this.response = null;
      this._responseText = "";
      this._state(4);
      this._finish(type);
    }
    send(body = null) {
      if (this.readyState !== 1 || this._sent)
        throw new DOMException("Request is not open", "InvalidStateError");
      globalThis.__tilefinchXHRSendCalls++;
      this._sent = true;
      this._done = false;
      this.emit("loadstart");
      try {
        const request = { _bodySource: body, headers: this.headers },
          serialized = serializeRequestBody(request),
          method = this.method || "GET",
          url = this.url,
          contentType = this.headers.get("content-type") || "",
          headerBlock = nativeHeaderBlock(this.headers),
          credentials = this.withCredentials ? "include" : "same-origin",
          timeout = Math.max(0, Number(this.timeout) || 0);
        if (this.async) {
          const id = queueNetwork(
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
            (raw) => this._apply(raw),
            (error) => {
              const timedOut =
                timeout > 0 && /timeout|timed out/i.test(String(error));
              this._fail(error, timedOut ? "timeout" : "error");
            },
          );
          this._requestId = pendingNetwork.has(id) ? Number(id) : 0;
          if (!this._done && timeout > 0)
            this._timeoutId = setTimeout(() => {
              if (this._done) return;
              cancelNetwork(
                this._requestId,
                new DOMException("The operation timed out", "TimeoutError"),
                false,
              );
              this.status = 0;
              this.readyState = 4;
              this.emit("readystatechange");
              this._finish("timeout");
            }, timeout);
        } else
          this._apply(
            __tilefinchFetchSync(
              method,
              url,
              serialized,
              contentType,
              headerBlock,
              "cors",
              credentials,
            ),
          );
      } catch (error) {
        this._fail(error);
      }
    }
    abort() {
      if (this._done) return;
      if (this._requestId)
        cancelNetwork(
          this._requestId,
          new DOMException("This operation was aborted", "AbortError"),
          false,
        );
      this.status = 0;
      this.readyState = 0;
      this._finish("abort");
    }
    getResponseHeader(name) {
      return this.readyState < 2 ? null : this.responseHeaders.get(name);
    }
    getAllResponseHeaders() {
      if (this.readyState < 2) return "";
      let output = "";
      this.responseHeaders.forEach(
        (value, name) => (output += name + ": " + value + "\r\n"),
      );
      return output;
    }
  }
  installXhrValidation(TilefinchXMLHttpRequest);
  globalThis.__tilefinchXHRResponseCount = 0;
  globalThis.__tilefinchXHRLastStatus = 0;
  globalThis.__tilefinchXHRLastResponseType = "";
  globalThis.__tilefinchXHRLastTextLength = 0;
  globalThis.__tilefinchXHRLastByteLength = 0;
  globalThis.__tilefinchXHRLastStates = "";
  {
    const open = TilefinchXMLHttpRequest.prototype.open,
      state = TilefinchXMLHttpRequest.prototype._state,
      apply = TilefinchXMLHttpRequest.prototype._apply;
    TilefinchXMLHttpRequest.prototype.open = function (...args) {
      this._stateTrace = [1];
      return open.apply(this, args);
    };
    TilefinchXMLHttpRequest.prototype._state = function (value) {
      this._stateTrace.push(value);
      return state.call(this, value);
    };
    TilefinchXMLHttpRequest.prototype._apply = function (raw) {
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
      apply.call(this, raw);
      if (this.readyState === 4 && this.status !== 0) {
        globalThis.__tilefinchXHRResponseCount++;
        globalThis.__tilefinchXHRLastStatus = this.status;
        globalThis.__tilefinchXHRLastResponseType = this._responseType;
        globalThis.__tilefinchXHRLastTextLength = this._responseText.length;
        globalThis.__tilefinchXHRLastByteLength = byteLength;
        globalThis.__tilefinchXHRLastStates = this._stateTrace.join(".");
      }
    };
  }
  {
    const fail = TilefinchXMLHttpRequest.prototype._fail;
    TilefinchXMLHttpRequest.prototype._fail = function (error, type = "error") {
      const previous = globalThis.__tilefinchXHRLastError;
      fail.call(this, error, type);
      if (type !== "error") globalThis.__tilefinchXHRLastError = previous;
    };
  }
  TilefinchXMLHttpRequest.prototype.emit = function (type, loaded = 0, total = 0) {
    const event = new Event(type);
    event.target = this;
    event.loaded = loaded;
    event.total = total;
    event.lengthComputable = total > 0;
    const invoke = (callback) =>
      globalThis.__tilefinchRunTask(
        "xhr:" + String(type) + ":state=" + String(this.readyState),
        () => {
          try {
            callback.call(this, event);
          } catch (error) {
            __tilefinchReportUncaught(error, "XMLHttpRequest " + type);
          }
        },
      );
    const handler = this["on" + type];
    if (typeof handler === "function") invoke(handler);
    for (const callback of [...(this.listeners.get(type) || [])])
      invoke(callback);
  };
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
  globalThis.XMLHttpRequest = TilefinchXMLHttpRequest;
  Object.defineProperty(globalThis.__tilefinchRootCensus, "pendingNetwork", {
    get: () => pendingNetwork.size,
  });
})();
