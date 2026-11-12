/* Intl polyfill: QuickJS has no Intl, so pages that touch it receive this
   bounded en-US surface (the "bounded-en-us-utc" replay contract; other
   locales format as en-US, and a named time zone other than the host's
   formats as UTC). Installed lazily on first access to the Intl global
   (see lazy_bootstrap_properties), or on the first Number or BigInt
   toLocaleString, or Date toLocale*String with locales or options, which
   compat.js routes here. benchmarks/capture-reference.js keeps a copy of
   NumberFormat in lockstep for reference captures. */
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
    /* en-US number formatting (the bounded en-US contract): grouping,
       ECMA-402 fraction-digit defaults, currency symbols and minor units,
       percent, compact and scientific notation, significant digits and
       sign display. Other locales format as en-US. */
    const currencyDigits = (code) =>
        /^(?:BIF|CLP|DJF|GNF|ISK|JPY|KMF|KRW|PYG|RWF|UGX|UYI|VND|VUV|XAF|XOF|XPF)$/
          .test(code) ? 0
          : /^(?:BHD|IQD|JOD|KWD|LYD|OMR|TND)$/.test(code) ? 3 : 2,
      currencySymbols = {
        USD: "$", EUR: "€", GBP: "£", JPY: "¥", INR: "₹", CNY: "CN¥",
        KRW: "₩", CAD: "CA$", AUD: "A$", MXN: "MX$", BRL: "R$", HKD: "HK$",
        NZD: "NZ$", ILS: "₪", VND: "₫", TWD: "NT$", PHP: "₱", XAF: "FCFA",
      },
      narrowSymbols = {
        CAD: "$", AUD: "$", MXN: "$", HKD: "$", NZD: "$", TWD: "$",
        CNY: "¥", RUB: "₽", UAH: "₴", TRY: "₺", PLN: "zł", THB: "฿",
        NGN: "₦", KZT: "₸", SEK: "kr", NOK: "kr", DKK: "kr",
      },
      compactShort = ["", "K", "M", "B", "T"],
      compactLong = ["", "thousand", "million", "billion", "trillion"],
      /* Short unit labels; "" joins the label to the number. */
      unitLabels = {
        percent: "%", celsius: "°C", fahrenheit: "°F", degree: "deg",
        kilometer: " km", meter: " m", centimeter: " cm", millimeter: " mm",
        mile: " mi", foot: " ft", inch: " in", kilogram: " kg", gram: " g",
        pound: " lb", liter: " L", milliliter: " mL", second: " sec",
        minute: " min", hour: " hr", millisecond: " ms", byte: " byte",
        kilobyte: " kB", megabyte: " MB", gigabyte: " GB",
        "kilometer-per-hour": " km/h", "mile-per-hour": " mph",
      },
      choice = (options, key, allowed, fallback) => {
        const value = options[key];
        if (value === undefined) return fallback;
        const text = String(value);
        if (!allowed.includes(text))
          throw new RangeError("Value " + text + " out of range for " + key);
        return text;
      },
      digitOption = (options, key, minimum, maximum, fallback) => {
        const value = options[key];
        if (value === undefined) return fallback;
        const number = Math.floor(Number(value));
        if (!(number >= minimum && number <= maximum))
          throw new RangeError(key + " value is out of range.");
        return number;
      },
      /* A decimal as [digits, exponent], value = 0.digits x 10^exponent,
         from the shortest round-trip text of a number (as ICU formats) or
         a BigInt's digits; zero is ["", 1]. */
      decimalOf = (value) => {
        let text = String(value), exponent = 0;
        const e = text.indexOf("e");
        if (e >= 0) {
          exponent = Number(text.slice(e + 1));
          text = text.slice(0, e);
        }
        const point = text.indexOf(".");
        let digits = text.replace(".", "");
        exponent += point < 0 ? text.length : point;
        const leading = digits.length - digits.replace(/^0+/, "").length;
        digits = digits.slice(leading).replace(/0+$/, "");
        return digits ? [digits, exponent - leading] : ["", 1];
      },
      /* Keeps `keep` leading digits, rounding half away from zero. */
      roundDecimal = ([digits, exponent], keep) => {
        if (keep >= digits.length) return [digits, exponent];
        if (keep < 0) return ["", 1];
        let kept = digits.slice(0, keep);
        if (digits.charCodeAt(keep) >= 53) {
          let at = kept.length - 1;
          while (at >= 0 && kept[at] === "9") at--;
          if (at < 0) {
            kept = "1";
            exponent++;
          } else
            kept = kept.slice(0, at) +
              String.fromCharCode(kept.charCodeAt(at) + 1);
        }
        kept = kept.replace(/0+$/, "");
        return kept ? [kept, exponent] : ["", 1];
      },
      /* Plain digits with at least `fraction` fraction digits. */
      plainDecimal = ([digits, exponent], fraction) => {
        const integer = exponent <= 0 ? "0"
            : digits.slice(0, exponent).padEnd(exponent, "0"),
          shown = (exponent < 0 ? "0".repeat(-exponent) + digits
            : digits.slice(exponent)).padEnd(fraction, "0");
        return shown ? integer + "." + shown : integer;
      };
    class NumberFormat {
      constructor(locales, options = {}) {
        this.locale = localeTag(Array.isArray(locales) ? locales[0] : locales);
        options = options === undefined ? {} : Object(options);
        const r = (this.resolved = {});
        r.style = choice(options, "style",
          ["decimal", "percent", "currency", "unit"], "decimal");
        if (options.currency !== undefined) {
          r.currency = String(options.currency).toUpperCase();
          if (!/^[A-Z]{3}$/.test(r.currency))
            throw new RangeError("Invalid currency code : " + options.currency);
        } else if (r.style === "currency")
          throw new TypeError("Currency code is required with currency style.");
        if (r.style === "currency") {
          r.currencyDisplay = choice(options, "currencyDisplay",
            ["symbol", "narrowSymbol", "code", "name"], "symbol");
          r.currencySign = choice(options, "currencySign",
            ["standard", "accounting"], "standard");
        }
        if (r.style === "unit") {
          if (options.unit === undefined)
            throw new TypeError("Unit is required with unit style.");
          r.unit = String(options.unit);
          r.unitDisplay = choice(options, "unitDisplay",
            ["short", "narrow", "long"], "short");
        }
        r.notation = choice(options, "notation",
          ["standard", "scientific", "engineering", "compact"], "standard");
        if (r.notation === "compact")
          r.compactDisplay = choice(options, "compactDisplay",
            ["short", "long"], "short");
        r.minimumIntegerDigits =
          digitOption(options, "minimumIntegerDigits", 1, 21, 1);
        const significant = options.minimumSignificantDigits !== undefined ||
            options.maximumSignificantDigits !== undefined,
          fractional = options.minimumFractionDigits !== undefined ||
            options.maximumFractionDigits !== undefined,
          defaultMinimum = r.style === "currency" && r.notation !== "compact"
            ? currencyDigits(r.currency) : 0,
          defaultMaximum = r.style === "currency" && r.notation !== "compact"
            ? defaultMinimum : r.style === "percent" ? 0 : 3;
        if (significant) {
          r.minimumSignificantDigits =
            digitOption(options, "minimumSignificantDigits", 1, 21, 1);
          r.maximumSignificantDigits = digitOption(options,
            "maximumSignificantDigits", r.minimumSignificantDigits, 21, 21);
        } else if (fractional || r.notation !== "compact") {
          let minimum = digitOption(options, "minimumFractionDigits", 0, 100,
              undefined),
            maximum = digitOption(options, "maximumFractionDigits", 0, 100,
              undefined);
          if (minimum === undefined)
            minimum = Math.min(defaultMinimum, maximum ?? defaultMinimum);
          else if (maximum === undefined)
            maximum = Math.max(defaultMaximum, minimum);
          else if (minimum > maximum)
            throw new RangeError("maximumFractionDigits value is out of range.");
          r.minimumFractionDigits = minimum;
          r.maximumFractionDigits = maximum ?? Math.max(defaultMaximum, minimum);
        } else r.compactRounding = true;
        const grouping = options.useGrouping;
        r.useGrouping = grouping === undefined
          ? (r.notation === "compact" ? "min2" : "auto")
          : grouping === true || grouping === "true" ? "always"
          : grouping === false || grouping === "false" ? false
          : choice({ grouping }, "grouping", ["min2", "auto", "always"]);
        r.signDisplay = choice(options, "signDisplay",
          ["auto", "never", "always", "exceptZero", "negative"], "auto");
      }
      /* The digits of a decimal per the rounding options. */
      _digits(decimal) {
        const r = this.resolved;
        let minimum = r.minimumSignificantDigits,
          maximum = r.maximumSignificantDigits;
        if (r.compactRounding) {
          if (decimal[1] >= 3)
            return plainDecimal(roundDecimal(decimal, decimal[1]), 0);
          minimum = 1;
          maximum = 2;
        }
        if (maximum === undefined)
          return plainDecimal(roundDecimal(decimal,
            decimal[1] + r.maximumFractionDigits), r.minimumFractionDigits);
        const rounded = roundDecimal(decimal, maximum);
        return plainDecimal(rounded, minimum - rounded[1]);
      }
      formatToParts(value) {
        const r = this.resolved,
          parts = [],
          add = (type, text) => parts.push({ type, value: text });
        let number = typeof value === "bigint" ? value : Number(value),
          negative, digits = null, exponent = 0, scale = 0, decimal = null;
        if (typeof number === "bigint") {
          negative = number < 0n;
          decimal = decimalOf(negative ? -number : number);
        } else {
          negative = number < 0 || Object.is(number, -0);
          if (Number.isFinite(number)) decimal = decimalOf(Math.abs(number));
        }
        if (decimal) {
          if (r.style === "percent" && decimal[0]) decimal[1] += 2;
          const shifted = (by) => [decimal[0], decimal[0] ? decimal[1] - by : 1];
          if (r.notation === "compact" && decimal[0] && decimal[1] >= 4) {
            scale = Math.min(4, Math.floor((decimal[1] - 1) / 3));
            digits = this._digits(shifted(scale * 3));
            if (scale < 4 && digits.length >= 4 && !digits.includes(".")) {
              scale++;
              digits = this._digits(shifted(scale * 3));
            }
          } else if ((r.notation === "scientific" ||
                      r.notation === "engineering") && decimal[0]) {
            exponent = decimal[1] - 1;
            if (r.notation === "engineering")
              exponent -= ((exponent % 3) + 3) % 3;
            digits = this._digits(shifted(exponent));
            if (digits.indexOf(".") > (r.notation === "engineering" ? 3 : 1) ||
                (!digits.includes(".") &&
                 digits.length > (r.notation === "engineering" ? 3 : 1))) {
              exponent += r.notation === "engineering" ? 3 : 1;
              digits = this._digits(shifted(exponent));
            }
          } else digits = this._digits(decimal);
        }
        const zero = digits !== null && !/[1-9]/.test(digits),
          sign = Number.isNaN(number) ? ""
            : r.signDisplay === "never" ? ""
            : r.signDisplay === "always" ? (negative ? "-" : "+")
            : r.signDisplay === "exceptZero"
              ? (zero ? "" : negative ? "-" : "+")
            : r.signDisplay === "negative" ? (negative && !zero ? "-" : "")
            : negative ? "-" : "",
          accounting = sign === "-" && r.currencySign === "accounting",
          code = r.currency,
          display = r.currencyDisplay,
          symbol = r.style !== "currency" ? ""
            : display === "code" ? code
            : display === "name" ? ""
            : (display === "narrowSymbol" && narrowSymbols[code]) ||
              currencySymbols[code] || code;
        if (accounting) add("literal", "(");
        else if (sign) add(sign === "-" ? "minusSign" : "plusSign", sign);
        if (symbol) {
          add("currency", symbol);
          if (/[A-Za-z]$/.test(symbol)) add("literal", " ");
        }
        if (digits === null)
          add(Number.isNaN(number) ? "nan" : "infinity",
            Number.isNaN(number) ? "NaN" : "∞");
        else {
          const point = digits.indexOf("."),
            integer = (point < 0 ? digits : digits.slice(0, point))
              .padStart(r.minimumIntegerDigits, "0"),
            groups = r.useGrouping === false ||
              (r.useGrouping === "min2" && integer.length < 5)
              ? [integer] : integer.replace(/\B(?=(\d{3})+$)/g, ",").split(",");
          groups.forEach((group, at) => {
            if (at) add("group", ",");
            add("integer", group);
          });
          if (point >= 0) {
            add("decimal", ".");
            add("fraction", digits.slice(point + 1));
          }
        }
        if (scale && r.compactDisplay === "long")
          add("literal", " "), add("compact", compactLong[scale]);
        else if (scale) add("compact", compactShort[scale]);
        if (r.notation === "scientific" || r.notation === "engineering") {
          add("exponentSeparator", "E");
          if (exponent < 0) add("exponentMinusSign", "-");
          add("exponentInteger", String(Math.abs(exponent)));
        }
        if (r.style === "percent") add("percentSign", "%");
        if (r.style === "currency" && display === "name")
          add("literal", " "), add("currency", code);
        if (r.style === "unit") {
          const label = unitLabels[r.unit] ?? " " + r.unit;
          if (label[0] === " ") add("literal", " ");
          add("unit", label.trimStart());
        }
        if (accounting) add("literal", ")");
        return parts;
      }
      /* ECMA-402: format is an accessor returning a bound function. */
      get format() {
        let bound = boundFormats.get(this);
        if (!bound) {
          bound = (value) => {
            let text = "";
            for (const part of this.formatToParts(value)) text += part.value;
            return text;
          };
          boundFormats.set(this, bound);
        }
        return bound;
      }
      resolvedOptions() {
        const r = this.resolved,
          compact = r.compactRounding,
          result = { locale: this.locale, numberingSystem: "latn" };
        for (const key of [
          "style", "currency", "currencyDisplay", "currencySign", "unit",
          "unitDisplay", "minimumIntegerDigits", "minimumFractionDigits",
          "maximumFractionDigits", "minimumSignificantDigits",
          "maximumSignificantDigits", "useGrouping", "notation",
          "compactDisplay", "signDisplay",
        ]) {
          const value = compact && key.endsWith("FractionDigits") ? 0
            : compact && key === "minimumSignificantDigits" ? 1
            : compact && key === "maximumSignificantDigits" ? 2 : r[key];
          if (value !== undefined) result[key] = value;
        }
        result.roundingIncrement = 1;
        result.roundingMode = "halfExpand";
        result.roundingPriority = compact ? "morePrecision" : "auto";
        result.trailingZeroDisplay = "auto";
        return result;
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
    /* The default time zone is the one Date's local-time methods use; the
       host hands it over through a temporary global (runtime_creation.inc).
       Local fields come from Date itself, so formatting agrees with Date for
       every instant, DST included. */
    const hostZone =
        typeof globalThis.__tilefinchDefaultTimeZone === "string"
          ? globalThis.__tilefinchDefaultTimeZone
          : "UTC",
      dateProto = Date.prototype,
      localOffset = dateProto.getTimezoneOffset,
      utcTime = dateProto.getTime,
      weekdayNames = [
        "Sunday",
        "Monday",
        "Tuesday",
        "Wednesday",
        "Thursday",
        "Friday",
        "Saturday",
      ],
      monthNames = [
        "January",
        "February",
        "March",
        "April",
        "May",
        "June",
        "July",
        "August",
        "September",
        "October",
        "November",
        "December",
      ],
      utcZones = /^(?:etc\/)?(?:utc|uct|gmt|gmt[+-]?0|universal|zulu|greenwich)$/i,
      offsetZone = /^([+-])(\d{2})(?::?(\d{2}))?$/,
      resolveZone = (value) => {
        if (value === undefined) return hostZone;
        const text = String(value);
        if (utcZones.test(text)) return "UTC";
        const offset = offsetZone.exec(text);
        if (offset) {
          if (Number(offset[2]) > 23 || Number(offset[3] || 0) > 59)
            throw new RangeError("Invalid time zone specified: " + text);
          return offset[1] + offset[2] + ":" + (offset[3] || "00");
        }
        if (text.toLowerCase() === hostZone.toLowerCase()) return hostZone;
        if (!/^[A-Za-z][A-Za-z0-9_+-]*(?:\/[A-Za-z0-9_+-]+)*$/.test(text))
          throw new RangeError("Invalid time zone specified: " + text);
        return text;
      },
      /* Minutes east of UTC for this instant in a resolved zone. A named
         zone other than the host's has no rules here and formats as UTC. */
      zoneOffset = (zone, time) => {
        if (zone === hostZone) return -localOffset.call(new Date(time));
        const offset = offsetZone.exec(zone);
        return offset
          ? (offset[1] === "-" ? -1 : 1) *
              (Number(offset[2]) * 60 + Number(offset[3]))
          : 0;
      },
      dateFields = ["weekday", "year", "month", "day"],
      timeFields = ["hour", "minute", "second"];
    const boundFormats = new WeakMap(),
      two = (value) => String(value).padStart(2, "0"),
      named = (names, index, style) =>
        style === "long"
          ? names[index]
          : style === "narrow"
            ? names[index][0]
            : names[index].slice(0, 3),
      numeric = (value, style) => (style === "2-digit" ? two(value) : value);
    class DateTimeFormat {
      constructor(locales, options = {}) {
        this.locale = localeTag(Array.isArray(locales) ? locales[0] : locales);
        options = options || {};
        const resolved = {};
        for (const key of [
          ...dateFields,
          ...timeFields,
          "timeZoneName",
          "dateStyle",
          "timeStyle",
        ])
          if (options[key] !== undefined) resolved[key] = String(options[key]);
        if (
          !resolved.dateStyle &&
          !resolved.timeStyle &&
          ![...dateFields, ...timeFields].some((key) => resolved[key])
        )
          resolved.year = resolved.month = resolved.day = "numeric";
        this.hour12 =
          options.hour12 !== undefined
            ? !!options.hour12
            : options.hourCycle === undefined ||
              options.hourCycle === "h12" ||
              options.hourCycle === "h11";
        this.timeZone = resolveZone(options.timeZone);
        this.options = resolved;
      }
      formatToParts(value) {
        const time = utcTime.call(
          value === undefined
            ? new Date()
            : value instanceof Date
              ? value
              : new Date(Number(value)),
        );
        if (!Number.isFinite(time)) throw new RangeError("Invalid time value");
        const offset = zoneOffset(this.timeZone, time),
          shifted = new Date(time + offset * 60000),
          o = this.options,
          parts = [],
          add = (type, value) => parts.push({ type, value: String(value) }),
          list = (fields, separator) =>
            fields.forEach((field, at) => {
              if (at) add("literal", separator);
              add(field[0], field[1]);
            }),
          month = shifted.getUTCMonth(),
          hour = shifted.getUTCHours();
        let { weekday, month: monthStyle, day, year, hour: hourStyle } = o,
          { minute, second, timeZoneName } = o,
          joiner = ", ";
        if (o.dateStyle) {
          const style = o.dateStyle;
          weekday = style === "full" ? "long" : undefined;
          monthStyle =
            style === "short" ? "numeric" : style === "medium" ? "short" : "long";
          day = "numeric";
          year = style === "short" ? "2-digit" : "numeric";
          if (style === "full" || style === "long") joiner = " at ";
        }
        if (o.timeStyle) {
          hourStyle = minute = "numeric";
          second = o.timeStyle === "short" ? undefined : "numeric";
          timeZoneName =
            o.timeStyle === "full" || o.timeStyle === "long"
              ? "short"
              : undefined;
        }
        const weekdayField = weekday && [
            "weekday",
            named(weekdayNames, shifted.getUTCDay(), weekday),
          ],
          dayField = day && ["day", numeric(shifted.getUTCDate(), day)],
          yearValue = shifted.getUTCFullYear(),
          yearField = year && [
            "year",
            year === "2-digit" ? two(yearValue % 100) : yearValue,
          ];
        if (monthStyle === "numeric" || monthStyle === "2-digit") {
          /* en-US numeric dates: "Thu, 10/1/2026". */
          if (weekdayField) list([weekdayField], ""), add("literal", ", ");
          list(
            [["month", numeric(month + 1, monthStyle)], dayField, yearField].filter(
              Boolean,
            ),
            "/",
          );
        } else if (monthStyle) {
          /* en-US textual dates: "Thursday, October 1, 2026". */
          if (weekdayField) list([weekdayField], ""), add("literal", ", ");
          add("month", named(monthNames, month, monthStyle));
          if (dayField) add("literal", " "), add(dayField[0], dayField[1]);
          if (yearField)
            add("literal", dayField ? ", " : " "), add(yearField[0], yearField[1]);
        } else list([weekdayField, dayField, yearField].filter(Boolean), " ");
        if (hourStyle || minute || second) {
          if (parts.length) add("literal", joiner);
          const shown = this.hour12 ? hour % 12 || 12 : hour,
            paired = (style, other) => (other ? "2-digit" : style);
          list(
            [
              hourStyle && [
                "hour",
                numeric(
                  shown,
                  !this.hour12 && (minute || second) ? "2-digit" : hourStyle,
                ),
              ],
              minute && [
                "minute",
                numeric(
                  shifted.getUTCMinutes(),
                  paired(minute, hourStyle || second),
                ),
              ],
              second && [
                "second",
                numeric(
                  shifted.getUTCSeconds(),
                  paired(second, hourStyle || minute),
                ),
              ],
            ].filter(Boolean),
            ":",
          );
          if (hourStyle && this.hour12)
            add("literal", " "), add("dayPeriod", hour < 12 ? "AM" : "PM");
        }
        if (timeZoneName) {
          if (parts.length) add("literal", " ");
          const magnitude = Math.abs(offset);
          add(
            "timeZoneName",
            this.timeZone === "UTC"
              ? "UTC"
              : "GMT" +
                  (offset === 0
                    ? ""
                    : (offset < 0 ? "-" : "+") +
                      Math.floor(magnitude / 60) +
                      (magnitude % 60 ? ":" + two(magnitude % 60) : "")),
          );
        }
        return parts;
      }
      /* ECMA-402: format is an accessor returning a function bound to this
         formatter, so `dates.map(formatter.format)` works. */
      get format() {
        let bound = boundFormats.get(this);
        if (!bound) {
          bound = (value) =>
            this.formatToParts(value)
              .map((part) => part.value)
              .join("");
          boundFormats.set(this, bound);
        }
        return bound;
      }
      resolvedOptions() {
        const result = {
            locale: this.locale,
            calendar: "gregory",
            numberingSystem: "latn",
            timeZone: this.timeZone,
          },
          o = this.options;
        if (o.hour !== undefined || o.timeStyle !== undefined) {
          result.hourCycle = this.hour12 ? "h12" : "h23";
          result.hour12 = this.hour12;
        }
        for (const key in o) result[key] = o[key];
        return result;
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
    /* Number and Date toLocale*String format through these classes, not
       through whatever the page's Intl global holds (compat.js). Date's
       options default as ECMA-402 ToDateTimeOptions does. */
    const dateKeys = ["weekday", "year", "month", "day"],
      timeKeys = ["dayPeriod", "hour", "minute", "second",
        "fractionalSecondDigits"];
    globalThis.__tilefinchRegisterLocaleFormat?.(
      (value, locales, options) =>
        new NumberFormat(locales, options).format(value),
      (time, locales, options, required, defaults) => {
        if (!Number.isFinite(time)) return "Invalid Date";
        const source = options === undefined ? {} : Object(options),
          merged = {};
        for (const key of [...dateKeys, ...timeKeys, "timeZoneName",
          "dateStyle", "timeStyle", "hour12", "hourCycle", "timeZone"])
          if (source[key] !== undefined) merged[key] = source[key];
        let needDefaults = merged.dateStyle === undefined &&
          merged.timeStyle === undefined;
        if (required !== "time" && dateKeys.some((key) => key in merged))
          needDefaults = false;
        if (required !== "date" && timeKeys.some((key) => key in merged))
          needDefaults = false;
        if (needDefaults && defaults !== "time")
          merged.year = merged.month = merged.day = "numeric";
        if (needDefaults && defaults !== "date")
          merged.hour = merged.minute = merged.second = "numeric";
        return new DateTimeFormat(locales, merged).format(time);
      });
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
})();
