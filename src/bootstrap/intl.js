/* Intl polyfill: QuickJS has no Intl, so pages that touch it receive this
   bounded surface. Installed lazily on first access to the Intl global
   (see lazy_bootstrap_properties); nothing in the eager bootstrap needs it. */
(() => {
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
  /* A deterministic replay realm registered its DateTimeFormat facade
     before this module loaded (see deterministic-replay-surface.js). */
  const replayDateTimeFormat = globalThis.__tilefinchDeterministicDateTimeFormat;
  if (typeof replayDateTimeFormat === "function")
    globalThis.Intl.DateTimeFormat = replayDateTimeFormat;
})();
