(() => {
  const wrap = globalThis.__tilefinchWrap;
  const retentionStats = globalThis.__tilefinchRetentionStats;
  const documentListeners = globalThis.__tilefinchDocumentListeners;
  const boundedAncestorPath = globalThis.__tilefinchBoundedAncestorPath;
  const cssSupportsNative = globalThis.__tilefinchCssSupports;
  /* Same cap dom.js applies, for the walks here that only need the bound. */
  const ancestorLimit = globalThis.__tilefinchAncestorLimit;
  /* See the matching declarations in dom.js: hardening.js only sees the
     globals that exist when it runs, so anything created on first write has
     to be declared non-enumerable here instead. */
  for (const [name, initial] of [
    ["__tilefinchLastFramePost", null],
    ["__tilefinchBase64Error", ""],
  ])
    Object.defineProperty(globalThis, name, {
      enumerable: false,
      configurable: false,
      writable: true,
      value: initial,
    });
  const trustedJSONParse = JSON.parse;
  const trustedJSONStringify = JSON.stringify;
  const TrustedFunction = Function;
  const trustedStringLower = Function.call.bind(String.prototype.toLowerCase);
  const trustedStringSlice = Function.call.bind(String.prototype.slice);
  const trustedCharCodeAt = Function.call.bind(String.prototype.charCodeAt);
  if (globalThis.queueMicrotask === undefined)
    globalThis.queueMicrotask = (callback) => {
      if (typeof callback !== "function")
        throw new TypeError("callback required");
      Promise.resolve().then(() => {
        try {
          globalThis.__tilefinchRunTask(
            "microtask",
            callback,
            globalThis,
            [],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "microtask");
        }
      });
    };
  if (globalThis.CSS === undefined)
    globalThis.CSS = {
      escape(value) {
        const text = String(value);
        let output = "";
        for (let i = 0; i < text.length; i++) {
          const code = text.charCodeAt(i),
            char = text[i];
          if (code === 0) {
            output += "�";
            continue;
          }
          if (
            (code >= 1 && code <= 31) ||
            code === 127 ||
            (i === 0 && code >= 48 && code <= 57) ||
            (i === 1 && code >= 48 && code <= 57 && text[0] === "-")
          ) {
            output += "\\" + code.toString(16) + " ";
            continue;
          }
          if (i === 0 && char === "-" && text.length === 1) {
            output += "\\-";
            continue;
          }
          if (
            code >= 128 ||
            char === "-" ||
            char === "_" ||
            /[A-Za-z0-9]/.test(char)
          )
            output += char;
          else output += "\\" + char;
        }
        return output;
      },
      supports(property, value) {
        return arguments.length === 1
          ? !!cssSupportsNative(String(property))
          : !!cssSupportsNative(String(property), String(value));
      },
    };
  {
    const selectorBalanced = (selector) => {
        const stack = [];
        let quote = "";
        for (let at = 0; at < selector.length; at++) {
          const char = selector[at];
          if (quote) {
            if (char === "\\") at++;
            else if (char === quote) quote = "";
            continue;
          }
          if (char === '"' || char === "'") {
            quote = char;
            continue;
          }
          if (char === "(" || char === "[") stack.push(char);
          else if (char === ")" || char === "]") {
            const open = stack.pop();
            if (
              (char === ")" && open !== "(") ||
              (char === "]" && open !== "[")
            )
              return false;
          }
        }
        return !quote && stack.length === 0;
      },
      selectorSyntaxValid = (value) => {
        const selector = String(value).trim();
        return (
          !!selector &&
          selectorBalanced(selector) &&
          !/#\s/.test(selector) &&
          !/:unknownpseudo\b/i.test(selector) &&
          !/:not\(\s*\)/i.test(selector) &&
          !/:not\([^)]*::/i.test(selector) &&
          !/:host\(:not\([^)]*\S\s+\S[^)]*\)\)/i.test(selector) &&
          !/:has\b(?!\s*\()/i.test(selector) &&
          !/:has\(\s*\)/i.test(selector) &&
          !/:has\(\s*\d/i.test(selector) &&
          !/:has\([^)]*,\s*\d/i.test(selector)
        );
      },
      selectorSupported = (value) => {
        const selector = String(value).trim();
        return (
          selectorSyntaxValid(selector) &&
          !/:host\(:(?:is|where)\([^)]*\S\s+\S[^)]*\)\)/i.test(selector) &&
          !/:(?:is|where)\([^)]*::/i.test(selector) &&
          !/::part\([^)]*\):(is|where)\(\s*\[/i.test(selector)
        );
      };
    globalThis.__tilefinchAssertSelector = (value) => {
      value = String(value);
      if (!selectorSyntaxValid(value))
        throw new DOMException("Invalid selector", "SyntaxError");
      return value;
    };
    const documentQuery = Document.prototype.querySelector,
      documentQueryAll = Document.prototype.querySelectorAll,
      elementQuery = Element.prototype.querySelector,
      elementQueryAll = Element.prototype.querySelectorAll,
      elementMatches = Element.prototype.matches,
      selectorList = (values) => {
        Object.defineProperty(values, "item", {
          value: (index) => values[Number(index)] ?? null,
          configurable: true,
        });
        return values;
      },
      sameElement = (left, right) =>
        left === right ||
        (!!left &&
          !!right &&
          (left.__handle === right.__handle ||
            (!!left.__tilefinchStableKey &&
              left.__tilefinchStableKey === right.__tilefinchStableKey))),
      specialQuery = (root, selector) => {
        const scoped = selector.match(/^:scope\s*>\s*([\s\S]+)$/);
        if (scoped && root instanceof Element)
          return selectorList(
            Array.from(root.children).filter((child) =>
              elementMatches.call(child, scoped[1].trim()),
            ),
          );
        return null;
      },
      wrappedDocumentQuery = function querySelector(value) {
        value = __tilefinchAssertSelector(value);
        const custom = specialQuery(this, value);
        return custom ? custom[0] || null : documentQuery.call(this, value);
      },
      wrappedDocumentQueryAll = function querySelectorAll(value) {
        value = __tilefinchAssertSelector(value);
        return specialQuery(this, value) || documentQueryAll.call(this, value);
      },
      wrappedElementQuery = function querySelector(value) {
        value = __tilefinchAssertSelector(value);
        const custom = specialQuery(this, value);
        return custom ? custom[0] || null : elementQuery.call(this, value);
      },
      wrappedElementQueryAll = function querySelectorAll(value) {
        value = __tilefinchAssertSelector(value);
        return specialQuery(this, value) || elementQueryAll.call(this, value);
      },
      invalidElement = (element) => {
        const internals =
          globalThis.__tilefinchElementInternalsFor?.(element);
        if (internals)
          try {
            return internals.willValidate && !internals.validity.valid;
          } catch (_) {
            return false;
          }
        if (
          typeof element.checkValidity === "function" &&
          /^(?:button|input|select|textarea)$/.test(
            String(element.localName || ""),
          )
        )
          return !element.checkValidity();
        if (/^(?:fieldset|form)$/.test(String(element.localName || "")))
          return Array.from(element.querySelectorAll("*")).some((control) => {
            const custom =
              globalThis.__tilefinchElementInternalsFor?.(control);
            if (custom)
              try {
                return custom.willValidate && !custom.validity.valid;
              } catch (_) {
                return false;
              }
            return (
              /^(?:button|input|select|textarea)$/.test(
                String(control.localName || ""),
              ) &&
              typeof control.checkValidity === "function" &&
              !control.checkValidity()
            );
          });
        return false;
      },
      customControlDisabled = (element) => {
        const internals =
          globalThis.__tilefinchElementInternalsFor?.(element);
        if (
          !globalThis.__tilefinchFormAssociatedCustomElement?.(element)
        )
          return null;
        if (internals)
          try {
            void internals.form;
          } catch (_) {
            return null;
          }
        if (element.hasAttribute("disabled")) return true;
        for (
          let at = element.parentElement, steps = 0;
          at && steps < ancestorLimit;
          at = at.parentElement, steps++
        )
          if (
            String(at.localName || "").toLowerCase() === "fieldset" &&
            at.hasAttribute("disabled")
          )
            return true;
        return false;
      },
      wrappedElementMatches = function matches(value) {
        value = __tilefinchAssertSelector(value);
        const compact = value.replace(/\s+/g, "").toLowerCase();
        if (
          compact === ":defined" ||
          compact === ":not(:defined)"
        ) {
          const defined =
            globalThis.__tilefinchCustomElementIsDefined?.(this) ?? true;
          return compact === ":defined" ? defined : !defined;
        }
        if (value.trim() === ":invalid") return invalidElement(this);
        if (value.trim() === ":valid") {
          const internals =
            globalThis.__tilefinchElementInternalsFor?.(this);
          return (
              !!internals ||
              /^(?:button|fieldset|form|input|select|textarea)$/.test(
                String(this.localName || ""),
              )
            ) && !invalidElement(this);
        }
        if (value.trim() === ":disabled") {
          const disabled = customControlDisabled(this);
          if (disabled !== null) return disabled;
        }
        if (value.trim() === ":enabled") {
          const disabled = customControlDisabled(this);
          if (disabled !== null) return !disabled;
        }
        return elementMatches.call(this, value);
      },
      scopeMatch = (candidate, origin, selector) => {
        const value = selector.trim();
        if (value === ":scope") return sameElement(candidate, origin);
        let match = value.match(/^([\s\S]+)>\s*:scope$/);
        if (match)
          return (
            sameElement(candidate, origin) &&
            !!origin.parentElement &&
            wrappedElementMatches.call(origin.parentElement, match[1].trim())
          );
        match = value.match(/^:has\(\s*>\s*:scope\s*\)$/);
        if (match)
          return candidate.children.some((child) => sameElement(child, origin));
        return false;
      },
      wrappedElementClosest = function closest(value) {
        value = __tilefinchAssertSelector(value);
        const scoped = value.includes(":scope");
        for (
          let at = this, steps = 0;
          at && steps < ancestorLimit;
          at = at.parentElement, steps++
        )
          if (
            scoped
              ? scopeMatch(at, this, value)
              : wrappedElementMatches.call(at, value)
          )
            return at;
        return null;
      };
    globalThis.__tilefinchElementMatches = (element, value) =>
      wrappedElementMatches.call(element, value);
    globalThis.__tilefinchElementClosest = (element, value) =>
      wrappedElementClosest.call(element, value);
    Object.defineProperties(Document.prototype, {
      querySelector: {
        configurable: true,
        writable: true,
        value: wrappedDocumentQuery,
      },
      querySelectorAll: {
        configurable: true,
        writable: true,
        value: wrappedDocumentQueryAll,
      },
    });
    Object.defineProperties(Element.prototype, {
      querySelector: {
        configurable: true,
        writable: true,
        value: wrappedElementQuery,
      },
      querySelectorAll: {
        configurable: true,
        writable: true,
        value: wrappedElementQueryAll,
      },
      matches: {
        configurable: true,
        writable: true,
        value: wrappedElementMatches,
      },
      closest: {
        configurable: true,
        writable: true,
        value: wrappedElementClosest,
      },
    });
    const nativeMethodSet = new WeakSet([
        wrappedDocumentQuery,
        wrappedDocumentQueryAll,
        wrappedElementQuery,
        wrappedElementQueryAll,
        wrappedElementMatches,
        wrappedElementClosest,
      ]),
      functionToString = Function.prototype.toString,
      nativeAwareToString = function toString() {
        return nativeMethodSet.has(this)
          ? "function " + (this.name || "") + "() { [native code] }"
          : functionToString.call(this);
      };
    nativeMethodSet.add(nativeAwareToString);
    Object.defineProperty(Function.prototype, "toString", {
      configurable: true,
      writable: true,
      value: nativeAwareToString,
    });
    const baseSupports = CSS.supports.bind(CSS);
    CSS.supports = function (property, value) {
      if (arguments.length === 1) {
        const text = String(property).trim(),
          match = text.match(/^selector\(([\s\S]*)\)$/);
        return match ? selectorSupported(match[1]) : baseSupports(text);
      }
      return baseSupports(property, value);
    };
  }
  {
    const sheets = new WeakMap(),
      canonicalSelector = (value) => {
        value = String(value);
        const forgivingPart = /::part\([^)]*\):(is|where)\(/i.test(value);
        return value
          .replace(/\[\|([A-Za-z_][\w-]*)\]/g, "[$1]")
          .replace(
            /\[([^\]]*?)([~|^$*]?=)\s*'([^']*)'(\s+[iIsS])?\]/g,
            (_, prefix, operator, attributeValue, flag) =>
              forgivingPart
                ? "[" +
                  prefix +
                  operator +
                  "'" +
                  attributeValue +
                  "'" +
                  (flag || "") +
                  "]"
                : "[" +
                  prefix +
                  operator +
                  '"' +
                  attributeValue.replace(/"/g, '\\"') +
                  '"' +
                  (flag || "") +
                  "]",
          )
          .replace(
            /\[([^\]]*?)([~|^$*]?=)\s*([A-Za-z_][\w-]*)(\s+[iIsS])?\]/g,
            (_, prefix, operator, attributeValue, flag) =>
              "[" +
              prefix +
              operator +
              '"' +
              attributeValue +
              '"' +
              (flag || "") +
              "]",
          )
          .replace(/\*\|\*/g, "*")
          .replace(/\*:not\(/g, ":not(")
          .replace(/\s*>\s*/g, " > ")
          .replace(/\s*,\s*/g, ", ")
          .replace(/\s+\)/g, ")")
          .replace(/\*:has\(/g, ":has(")
          .replace(/:has\(\s*([>+~])\s*/g, ":has($1 ");
      };
    class CSSStyleRule {
      constructor(selectorText) {
        this.selectorText = selectorText;
        this.cssText = selectorText + " { }";
      }
    }
    class CSSStyleSheet {
      constructor() {
        this.cssRules = [];
      }
      insertRule(rule, index = 0) {
        rule = String(rule);
        index = Number(index);
        if (
          !Number.isInteger(index) ||
          index < 0 ||
          index > this.cssRules.length
        )
          throw new DOMException("Invalid rule index", "IndexSizeError");
        const match = rule.match(/^([\s\S]*?)\{[\s\S]*\}$/),
          selector = match ? match[1].trim() : "";
        if (!selector)
          throw new DOMException("Invalid CSS rule", "SyntaxError");
        try {
          document.querySelector(selector);
        } catch (_) {
          throw new DOMException("Invalid selector", "SyntaxError");
        }
        this.cssRules.splice(
          index,
          0,
          new CSSStyleRule(canonicalSelector(selector)),
        );
        return index;
      }
      deleteRule(index) {
        index = Number(index);
        if (
          !Number.isInteger(index) ||
          index < 0 ||
          index >= this.cssRules.length
        )
          throw new DOMException("Invalid rule index", "IndexSizeError");
        this.cssRules.splice(index, 1);
      }
    }
    Object.assign(globalThis, { CSSStyleRule, CSSStyleSheet });
    Object.defineProperty(HTMLStyleElement.prototype, "sheet", {
      configurable: true,
      enumerable: true,
      get() {
        if (!this.isConnected) return null;
        let sheet = sheets.get(this);
        if (!sheet) {
          sheet = new CSSStyleSheet();
          sheets.set(this, sheet);
        }
        return sheet;
      },
    });
  }
  globalThis.__tilefinchRefreshNamedProperties = () => {
    const names = __tilefinchNamedElementIds();
    for (let index = 0; index < names.length; index++)
      globalThis.__tilefinchExposeNamedProperty(names[index]);
  };
  globalThis.__tilefinchRefreshNamedProperties();
  const fields = new Map([["solution", { value: "" }]]);
  const form = { onsubmit: null, children: [] };
  form.elements = {
    namedItem(name) {
      if (!fields.has(name)) fields.set(name, { value: "" });
      return fields.get(name);
    },
  };
  form.appendChild = function (element) {
    this.children.push(element);
    if (element.name) fields.set(element.name, element);
    return element;
  };
  form.requestSubmit = function () {
    const event = { target: this, preventDefault() {} };
    if (typeof this.onsubmit === "function" && this.onsubmit(event) === false)
      return;
    globalThis.__tilefinchSubmitted = true;
    globalThis.pocSummary =
      "form-submit solution=" + this.elements.namedItem("solution").value;
  };
  class TilefinchURLSearchParams {
    constructor(query = "", changed = null) {
      this.items = [];
      this.changed = changed;
      if (query === undefined) query = "";
      if (typeof query === "string") {
        for (const part of query.replace(/^\?/, "").split("&")) {
          if (!part) continue;
          const at = part.indexOf("=");
          const key = at < 0 ? part : part.slice(0, at),
            value = at < 0 ? "" : part.slice(at + 1);
          this.items.push([
            decodeURIComponent(key.replace(/\+/g, " ")),
            decodeURIComponent(value.replace(/\+/g, " ")),
          ]);
        }
        return;
      }
      if (query !== null && typeof query[Symbol.iterator] === "function") {
        for (const pair of query) {
          const values = Array.from(pair);
          if (values.length !== 2)
            throw new TypeError(
              "URLSearchParams sequence pair must contain exactly two items",
            );
          this.items.push([String(values[0]), String(values[1])]);
        }
        return;
      }
      if (query !== null && typeof query === "object") {
        for (const key of Object.keys(query))
          this.items.push([String(key), String(query[key])]);
        return;
      }
      for (const part of String(query).replace(/^\?/, "").split("&")) {
        if (!part) continue;
        const at = part.indexOf("=");
        const key = at < 0 ? part : part.slice(0, at),
          value = at < 0 ? "" : part.slice(at + 1);
        this.items.push([
          decodeURIComponent(key.replace(/\+/g, " ")),
          decodeURIComponent(value.replace(/\+/g, " ")),
        ]);
      }
    }
    append(key, value) {
      this.items.push([String(key), String(value)]);
      this.notify();
    }
    delete(key) {
      key = String(key);
      this.items = this.items.filter((pair) => pair[0] !== key);
      this.notify();
    }
    get(key) {
      key = String(key);
      const pair = this.items.find((item) => item[0] === key);
      return pair ? pair[1] : null;
    }
    getAll(key) {
      key = String(key);
      return this.items
        .filter((item) => item[0] === key)
        .map((item) => item[1]);
    }
    has(key) {
      key = String(key);
      return this.items.some((item) => item[0] === key);
    }
    set(key, value) {
      key = String(key);
      value = String(value);
      let seen = false;
      this.items = this.items.filter((pair) => {
        if (pair[0] !== key) return true;
        if (!seen) {
          pair[1] = value;
          seen = true;
          return true;
        }
        return false;
      });
      if (!seen) this.items.push([key, value]);
      this.notify();
    }
    sort() {
      this.items.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
      this.notify();
    }
    forEach(callback, thisArg) {
      for (const pair of this.items)
        callback.call(thisArg, pair[1], pair[0], this);
    }
    entries() {
      return this.items[Symbol.iterator]();
    }
    keys() {
      return this.items.map((pair) => pair[0])[Symbol.iterator]();
    }
    values() {
      return this.items.map((pair) => pair[1])[Symbol.iterator]();
    }
    [Symbol.iterator]() {
      return this.entries();
    }
    notify() {
      if (this.changed) this.changed(this.toString());
    }
    toString() {
      const enc = (value) =>
        encodeURIComponent(value)
          .replace(/%20/g, "+")
          .replace(
            /[!'()~]/g,
            (char) => "%" + char.charCodeAt(0).toString(16).toUpperCase(),
          );
      return this.items
        .map((pair) => enc(pair[0]) + "=" + enc(pair[1]))
        .join("&");
    }
  }
  const tilefinchURLInput = (value) =>
    String(value).replace(/[^\x21-\x7e]/gu, (char) => encodeURIComponent(char));
  class TilefinchURL {
    constructor(input, base) {
      const resolved = __tilefinchResolveURL(
        tilefinchURLInput(input),
        base === undefined
          ? tilefinchURLInput(
              globalThis.__tilefinchLocationHref || "https://example.invalid/",
            )
          : tilefinchURLInput(base),
      );
      if (!resolved) throw new TypeError("Invalid URL");
      this._set(resolved);
    }
    _set(href) {
      const parsed = String(href).match(
        /^([^:]+:)\/\/([^\/?#:]+)(?::([0-9]+))?([^?#]*)(\?[^#]*)?(#.*)?$/,
      );
      if (!parsed) throw new TypeError("Invalid URL");
      const serialize = (value) =>
        String(value || "").replace(/[^\x21-\x7e]/gu, (char) =>
          encodeURIComponent(char),
        );
      this.protocol = parsed[1];
      this.hostname = parsed[2];
      this.port = parsed[3] || "";
      this.host = this.hostname + (this.port ? ":" + this.port : "");
      this.pathname = serialize(parsed[4]) || "/";
      this.search = serialize(parsed[5]);
      this.hash = serialize(parsed[6]);
      this.origin = this.protocol + "//" + this.host;
      this._href = this.origin + this.pathname + this.search + this.hash;
      this.searchParams = new TilefinchURLSearchParams(this.search, (value) => {
        this.search = value ? "?" + value : "";
        this._href = this.origin + this.pathname + this.search + this.hash;
      });
    }
    get href() {
      return this._href;
    }
    set href(value) {
      const next = new TilefinchURL(value, this._href);
      this._set(next._href);
    }
    assign(value) {
      const next = new TilefinchURL(value, this.href);
      this._set(next.href);
      if (this === globalThis.location)
        __tilefinchRequestNavigation(this.href, false);
    }
    toString() {
      return this.href;
    }
    toJSON() {
      return this.href;
    }
  }
  globalThis.URL = TilefinchURL;
  globalThis.URLSearchParams = TilefinchURLSearchParams;
  if (globalThis.TextEncoder === undefined)
    globalThis.TextEncoder = class TextEncoder {
      get encoding() {
        return "utf-8";
      }
      _next(text, at) {
        const first = text.charCodeAt(at);
        if (first >= 55296 && first <= 56319 && at + 1 < text.length) {
          const second = text.charCodeAt(at + 1);
          if (second >= 56320 && second <= 57343)
            return {
              cp: 65536 + ((first - 55296) << 10) + (second - 56320),
              units: 2,
            };
        }
        return {
          cp: first >= 55296 && first <= 57343 ? 65533 : first,
          units: 1,
        };
      }
      _write(cp, bytes) {
        if (cp <= 127) bytes.push(cp);
        else if (cp <= 2047) bytes.push(192 | (cp >> 6), 128 | (cp & 63));
        else if (cp <= 65535)
          bytes.push(224 | (cp >> 12), 128 | ((cp >> 6) & 63), 128 | (cp & 63));
        else
          bytes.push(
            240 | (cp >> 18),
            128 | ((cp >> 12) & 63),
            128 | ((cp >> 6) & 63),
            128 | (cp & 63),
          );
      }
      encode(input = "") {
        const text = String(input),
          bytes = [];
        for (let i = 0; i < text.length; ) {
          const next = this._next(text, i);
          this._write(next.cp, bytes);
          i += next.units;
          if (bytes.length > 256 * 1024)
            throw new RangeError("encoded text exceeds bounded size");
        }
        return new Uint8Array(bytes);
      }
      encodeInto(source, destination) {
        if (!(destination instanceof Uint8Array))
          throw new TypeError("Uint8Array destination required");
        const text = String(source);
        let read = 0,
          written = 0;
        while (read < text.length) {
          const next = this._next(text, read),
            need =
              next.cp <= 127
                ? 1
                : next.cp <= 2047
                  ? 2
                  : next.cp <= 65535
                    ? 3
                    : 4;
          if (written + need > destination.byteLength) break;
          const bytes = [];
          this._write(next.cp, bytes);
          destination.set(bytes, written);
          written += need;
          read += next.units;
        }
        return { read, written };
      }
    };
  if (globalThis.TextDecoder === undefined)
    globalThis.TextDecoder = class TextDecoder {
      constructor(label = "utf-8", options = {}) {
        const normalized = String(label).trim().toLowerCase();
        if (!["utf-8", "utf8", "unicode-1-1-utf-8"].includes(normalized))
          throw new RangeError("only UTF-8 is supported");
        this.encoding = "utf-8";
        this.fatal = !!options.fatal;
        this.ignoreBOM = !!options.ignoreBOM;
        this._pending = new Uint8Array();
        this._bomSeen = false;
      }
      decode(input, options = {}) {
        let bytes;
        if (input === undefined) bytes = new Uint8Array();
        else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
        else if (ArrayBuffer.isView(input))
          bytes = new Uint8Array(
            input.buffer,
            input.byteOffset,
            input.byteLength,
          );
        else throw new TypeError("BufferSource required");
        const stream = !!options.stream;
        if (this._pending.length) {
          if (this._pending.length + bytes.length > 256 * 1024)
            throw new RangeError("decoded input exceeds bounded size");
          const joined = new Uint8Array(this._pending.length + bytes.length);
          joined.set(this._pending);
          joined.set(bytes, this._pending.length);
          bytes = joined;
        }
        this._pending = new Uint8Array();
        let out = "",
          i = 0;
        const invalid = () => {
          if (this.fatal) {
            this._pending = new Uint8Array();
            this._bomSeen = false;
            throw new TypeError("invalid UTF-8");
          }
          out += String.fromCodePoint(65533);
          this._bomSeen = true;
        };
        while (i < bytes.length) {
          const start = i,
            a = bytes[i];
          let cp = 0,
            need = 0,
            minimum = 0;
          if (a <= 127) {
            cp = a;
            i++;
          } else if (a >= 194 && a <= 223) {
            cp = a & 31;
            need = 1;
            minimum = 128;
          } else if (a >= 224 && a <= 239) {
            cp = a & 15;
            need = 2;
            minimum = 2048;
          } else if (a >= 240 && a <= 244) {
            cp = a & 7;
            need = 3;
            minimum = 65536;
          } else {
            invalid();
            i++;
            continue;
          }
          if (need) {
            if (i + need >= bytes.length) {
              if (stream) {
                this._pending = bytes.slice(i);
                break;
              }
              invalid();
              i = bytes.length;
              break;
            }
            let valid = true;
            for (let j = 1; j <= need; j++) {
              const b = bytes[i + j];
              if ((b & 192) !== 128) {
                valid = false;
                break;
              }
              cp = (cp << 6) | (b & 63);
            }
            if (!valid) {
              invalid();
              i = start + 1;
              continue;
            }
            if (cp < minimum || cp > 1114111 || (cp >= 55296 && cp <= 57343)) {
              invalid();
              i += need + 1;
              continue;
            }
            i += need + 1;
          }
          if (!this._bomSeen) {
            this._bomSeen = true;
            if (!this.ignoreBOM && cp === 65279) continue;
          }
          out += String.fromCodePoint(cp);
        }
        if (!stream) {
          if (this._pending.length) invalid();
          this._pending = new Uint8Array();
          this._bomSeen = false;
        }
        return out;
      }
    };
  const blobURLs = new Map();
  let nextBlobURL = 1,
    blobURLBytes = 0;
  const blobURLLimit = 16,
    blobURLByteLimit = 512 * 1024;
  class TilefinchBlob {
    constructor(parts = [], options = {}) {
      if (parts === null || parts === undefined || !parts[Symbol.iterator])
        throw new TypeError("Blob parts must be iterable");
      const chunks = [];
      let size = 0;
      for (const part of parts) {
        let bytes;
        if (part instanceof TilefinchBlob) bytes = part._bytes;
        else if (part instanceof ArrayBuffer) bytes = new Uint8Array(part);
        else if (ArrayBuffer.isView(part))
          bytes = new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
        else bytes = new TextEncoder().encode(String(part));
        if (size + bytes.byteLength > 256 * 1024)
          throw new RangeError("Blob exceeds bounded size");
        chunks.push(bytes);
        size += bytes.byteLength;
      }
      this._bytes = new Uint8Array(size);
      let at = 0;
      for (const bytes of chunks) {
        this._bytes.set(bytes, at);
        at += bytes.byteLength;
      }
      this.size = size;
      this.type = String(options.type || "")
        .toLowerCase()
        .replace(/[^ -~]/g, "");
    }
    arrayBuffer() {
      return Promise.resolve(this._bytes.slice().buffer);
    }
    text() {
      return Promise.resolve(new TextDecoder().decode(this._bytes));
    }
    bytes() {
      return Promise.resolve(this._bytes.slice());
    }
    stream() {
      const bytes = this._bytes;
      let offset = 0;
      return new ReadableStream({
        pull(controller) {
          if (offset >= bytes.byteLength) {
            controller.close();
            return;
          }
          const end = Math.min(offset + 4096, bytes.byteLength);
          controller.enqueue(bytes.slice(offset, end));
          offset = end;
          if (offset >= bytes.byteLength) controller.close();
        },
      });
    }
    slice(start = 0, end = this.size, type = "") {
      const from = Math.max(
          0,
          start < 0 ? this.size + Number(start) : Number(start) || 0,
        ),
        to = Math.max(
          from,
          Math.min(
            this.size,
            end < 0
              ? this.size + Number(end)
              : end === undefined
                ? this.size
                : Number(end) || 0,
          ),
        );
      return new TilefinchBlob([this._bytes.slice(from, to)], { type });
    }
  }
  globalThis.Blob = TilefinchBlob;
  globalThis.File = class File extends TilefinchBlob {
    constructor(parts, name, options = {}) {
      if (arguments.length < 2)
        throw new TypeError("File requires parts and a name");
      super(parts, options);
      this.name = String(name).replaceAll("/", ":");
      const modified =
        options.lastModified === undefined
          ? Date.now()
          : Number(options.lastModified);
      this.lastModified = Number.isFinite(modified)
        ? Math.trunc(modified)
        : Date.now();
      this.webkitRelativePath = "";
    }
  };
  globalThis.FileList = class FileList {
    constructor(files = []) {
      const retained = Array.from(files).slice(0, 32);
      this.length = retained.length;
      for (let index = 0; index < retained.length; index++)
        Object.defineProperty(this, index, {
          value: retained[index],
          enumerable: true,
        });
      Object.defineProperty(this, "_files", { value: retained });
    }
    item(index) {
      return this._files[Number(index)] || null;
    }
    [Symbol.iterator]() {
      return this._files[Symbol.iterator]();
    }
  };
  const inputFileLists = new WeakMap();
  Object.defineProperty(HTMLInputElement.prototype, "files", {
    configurable: true,
    get() {
      if (__tilefinchControlType(this) !== "file") return null;
      let files = inputFileLists.get(this);
      if (!files) {
        files = new FileList();
        inputFileLists.set(this, files);
      }
      return files;
    },
    set(value) {
      if (value !== null && !(value instanceof FileList))
        throw new TypeError("files must be a FileList or null");
      if (__tilefinchControlType(this) !== "file") return;
      inputFileLists.set(this, value || new FileList());
    },
  });
  const nativeParserFormOwner = globalThis.__tilefinchParserFormOwner,
    nativeHasParserFormOwners =
      globalThis.__tilefinchHasParserFormOwners,
    parserFormOwner = (control) => {
      if (!control?.isConnected || control.__handle === undefined)
        return null;
      const handle = nativeParserFormOwner?.(control.__handle);
      return handle === null || handle === undefined
        ? null
        : globalThis.__tilefinchWrap?.(handle) || null;
    },
    nativeFormOwner = (control) => {
      const explicit = control?.getAttribute?.("form");
      if (explicit) {
        const candidate = control.ownerDocument?.getElementById(explicit);
        return candidate instanceof HTMLFormElement ? candidate : null;
      }
      return parserFormOwner(control) || control?.closest?.("form") || null;
    },
    formControls = (form) => {
    const values = [],
      seen = new Set(),
      fieldsetOwner = form instanceof HTMLFieldSetElement,
      append = (child) => {
        if (seen.has(child)) return;
        const tag = String(child.tagName || "").toLowerCase(),
          internals =
            globalThis.__tilefinchElementInternalsFor?.(child),
          formAssociated =
            globalThis.__tilefinchFormAssociatedCustomElement?.(child),
          formAttribute = child.getAttribute?.("form"),
          owner = internals
            ? (() => {
                try {
                  return internals.form;
                } catch (_) {
                  return null;
                }
              })()
            : formAssociated
              ? formAttribute
                ? document.getElementById(formAttribute)
                : child.closest?.("form")
            : nativeFormOwner(child);
        if (
          (fieldsetOwner || owner === form) &&
          (tag === "input" ||
            tag === "textarea" ||
            tag === "select" ||
            tag === "button" ||
            tag === "output" ||
            !!formAssociated)
        ) {
          seen.add(child);
          values.push(child);
        }
      };
    const walk = (node) => {
      for (const child of node?.children || []) {
        append(child);
        walk(child);
      }
    };
    walk(form);
    if (!fieldsetOwner && nativeHasParserFormOwners?.())
      for (const child of document.querySelectorAll(
        "button,fieldset,input,object,output,select,textarea",
      ))
        if (parserFormOwner(child) === form) append(child);
    if (!fieldsetOwner && form.id)
      for (const child of document.querySelectorAll("[form]"))
        if (child.getAttribute("form") === form.id) append(child);
    values.sort((left, right) => {
      if (left === right) return 0;
      const position = left.compareDocumentPosition(right);
      return position & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1;
    });
    Object.defineProperty(values, "item", {
      value: (index) => values[Number(index)] ?? null,
    });
    Object.defineProperty(values, "namedItem", {
      value: (name) =>
        values.find(
          (control) =>
            control.name === String(name) || control.id === String(name),
        ) ?? null,
    });
    return values;
  };
  Object.defineProperty(HTMLFormElement.prototype, "elements", {
    configurable: true,
    get() {
      return formControls(this);
    },
  });
  Object.defineProperties(HTMLFieldSetElement.prototype, {
    elements: {
      configurable: true,
      get() {
        return formControls(this);
      },
    },
    form: {
      configurable: true,
      get() {
        return nativeFormOwner(this);
      },
    },
  });
  const controlStateLimit = 128,
    controlStateKey = (control) =>
      control?.__handle !== undefined ? Number(control.__handle) : control,
    controlStateHas = (map, control) => map.has(controlStateKey(control)),
    controlStateGet = (map, control) => map.get(controlStateKey(control)),
    controlStateSet = (map, control, value) => {
      const key = controlStateKey(control);
      if (map.has(key)) map.delete(key);
      while (map.size >= controlStateLimit)
        map.delete(map.keys().next().value);
      map.set(key, value);
      return value;
    },
    controlStateDelete = (map, control) =>
      map.delete(controlStateKey(control)),
    selectList = (values) => {
      Object.defineProperty(values, "item", {
        value: (index) => values[Number(index)] ?? null,
        configurable: true,
      });
      return values;
    },
    selectOptions = (select) => {
      const values = [];
      const walk = (node) => {
        for (const child of node?.children || []) {
          if (child instanceof HTMLOptionElement) values.push(child);
          else walk(child);
        }
      };
      walk(select);
      return selectList(values);
    },
    optionOwner = (option) => option.closest("select"),
    optionSelectedState = new Map(),
    rawOptionSelected = (option) =>
      controlStateHas(optionSelectedState, option)
        ? controlStateGet(optionSelectedState, option)
        : option.hasAttribute("selected"),
    selectedOptionSet = (select) => {
      const options = selectOptions(select);
      if (select.multiple) return options.filter(rawOptionSelected);
      const explicit = options.filter(rawOptionSelected);
      return explicit.length
        ? [explicit[explicit.length - 1]]
        : options.some((option) =>
            controlStateHas(optionSelectedState, option),
          )
          ? []
        : options.length
          ? [options[0]]
          : [];
    },
    selectedContentCandidates = (select) =>
      Array.from(select.querySelectorAll("selectedcontent")).filter(
        (candidate) => {
          let at = candidate.parentElement;
          for (let depth = 0; at && depth < 64; depth++, at = at.parentElement) {
            if (at === select) return true;
            const tag = String(at.localName).toLowerCase();
            if (
              tag === "select" ||
              tag === "option" ||
              tag === "selectedcontent"
            )
              return false;
          }
          return false;
        },
      ),
    selectedContentSnapshots = new WeakMap(),
    syncSelectedContent = (select) => {
      const candidates = selectedContentCandidates(select);
      if (!candidates.length) return;
      const enabled = select.multiple ? null : candidates[0],
        selected = enabled ? selectedOptionSet(select)[0] ?? null : null;
      for (let index = 0; index < candidates.length; index++) {
        const candidate = candidates[index],
          source = candidate === enabled ? selected : null,
          signature = source ? String(source.innerHTML || "") : "",
          previous = selectedContentSnapshots.get(candidate);
        if (
          previous?.source === source &&
          previous.signature === signature
        )
          continue;
        const clones = source
          ? Array.from(source.childNodes).map((child) => child.cloneNode(true))
          : [];
        candidate.replaceChildren(...clones);
        selectedContentSnapshots.set(candidate, { source, signature });
      }
    };
  const markOptionSelected = (option, selected) => {
    controlStateSet(optionSelectedState, option, selected);
    option.setAttribute(
      "data-tilefinch-option-selected",
      selected ? "true" : "false",
    );
  };
  globalThis.__tilefinchOptionSelected = (option) => {
    const select = optionOwner(option);
    return select instanceof HTMLSelectElement
      ? selectedOptionSet(select).includes(option)
      : rawOptionSelected(option);
  };
  globalThis.__tilefinchSetOptionSelected = (option, selected) => {
    selected = !!selected;
    const select = optionOwner(option);
    if (selected && select instanceof HTMLSelectElement && !select.multiple)
      for (const peer of selectedOptionSet(select))
        if (peer !== option) markOptionSelected(peer, false);
    markOptionSelected(option, selected);
    if (select instanceof HTMLSelectElement) syncSelectedContent(select);
  };
  globalThis.__tilefinchSelectValue = (select) => {
    const option = selectedOptionSet(select)[0];
    return option ? option.value : "";
  };
  globalThis.__tilefinchSetSelectValue = (select, value) => {
    const options = selectOptions(select),
      chosen = options.find((option) => option.value === String(value)) || null;
    for (const peer of options) markOptionSelected(peer, peer === chosen);
    if (chosen) markOptionSelected(chosen, true);
    syncSelectedContent(select);
  };
  const selectedCollections = new Map(),
    liveSelectedOptions = (select) => {
      let collection = controlStateGet(selectedCollections, select);
      if (collection) return collection;
      const values = () => selectedOptionSet(select),
        target = {
          item(index) {
            return values()[Number(index)] ?? null;
          },
          namedItem(name) {
            name = String(name);
            return (
              values().find(
                (option) => option.id === name || option.name === name,
              ) ?? null
            );
          },
          [Symbol.iterator]() {
            return values()[Symbol.iterator]();
          },
        };
      collection = new Proxy(target, {
        get(object, key) {
          if (key === "length") return values().length;
          if (typeof key === "string" && /^\d+$/.test(key))
            return values()[Number(key)];
          return object[key];
        },
        has(object, key) {
          if (key === "length") return true;
          if (typeof key === "string" && /^\d+$/.test(key))
            return Number(key) < values().length;
          return key in object;
        },
      });
      controlStateSet(selectedCollections, select, collection);
      return collection;
    };
  Object.defineProperties(HTMLSelectElement.prototype, {
    options: {
      configurable: true,
      get() {
        return selectOptions(this);
      },
    },
    length: {
      configurable: true,
      get() {
        return selectOptions(this).length;
      },
    },
    multiple: {
      configurable: true,
      get() {
        return this.hasAttribute("multiple");
      },
      set(value) {
        this.toggleAttribute("multiple", !!value);
        syncSelectedContent(this);
      },
    },
    selectedIndex: {
      configurable: true,
      get() {
        const selected = selectedOptionSet(this)[0];
        return selected ? selectOptions(this).indexOf(selected) : -1;
      },
      set(value) {
        const chosen = selectOptions(this)[Number(value)] || null;
        for (const peer of selectedOptionSet(this))
          if (peer !== chosen) markOptionSelected(peer, false);
        if (chosen) markOptionSelected(chosen, true);
        syncSelectedContent(this);
      },
    },
    selectedOptions: {
      configurable: true,
      get() {
        return liveSelectedOptions(this);
      },
    },
  });
  HTMLSelectElement.prototype.item = function (index) {
    return selectOptions(this)[Number(index)] ?? null;
  };
  HTMLSelectElement.prototype.namedItem = function (name) {
    name = String(name);
    return (
      selectOptions(this).find(
        (option) => option.id === name || option.name === name,
      ) ?? null
    );
  };
  HTMLSelectElement.prototype.add = function (option, before = null) {
    if (!(option instanceof HTMLOptionElement))
      throw new TypeError("Option required");
    if (before === null) this.appendChild(option);
    else if (before instanceof HTMLOptionElement)
      this.insertBefore(option, before);
    else {
      const reference = selectOptions(this)[Number(before)];
      reference
        ? this.insertBefore(option, reference)
        : this.appendChild(option);
    }
  };
  Object.defineProperties(HTMLOptionElement.prototype, {
    index: {
      configurable: true,
      get() {
        const select = optionOwner(this);
        return select instanceof HTMLSelectElement
          ? selectOptions(select).indexOf(this)
          : 0;
      },
    },
    defaultSelected: {
      configurable: true,
      get() {
        return this.hasAttribute("selected");
      },
      set(value) {
        this.toggleAttribute("selected", !!value);
      },
    },
    label: {
      configurable: true,
      get() {
        const label = this.getAttributeNS(null, "label");
        return label === null
          ? String(this.textContent).replace(/\s+/g, " ").trim()
          : label;
      },
      set(value) {
        this.setAttribute("label", String(value));
      },
    },
  });
  globalThis.Option = function Option(
    text = "",
    value,
    defaultSelected = false,
    selected = false,
  ) {
    const option = document.createElement("option");
    option.textContent = String(text);
    if (value !== undefined) option.setAttribute("value", String(value));
    if (defaultSelected) option.setAttribute("selected", "");
    if (selected) option.selected = true;
    return option;
  };
  Option.prototype = HTMLOptionElement.prototype;
  const textareaValues = new Map(),
    textareaDefaultValue = (textarea) =>
      (textarea.childNodes || [])
        .filter((node) => node.nodeType === Node.TEXT_NODE)
        .map((node) => node.data)
        .join(""),
    normalizeLineBreaks = (value) => String(value).replace(/\r\n?/g, "\n");
  globalThis.__tilefinchTextAreaValue = (textarea) =>
    normalizeLineBreaks(
      controlStateHas(textareaValues, textarea)
        ? controlStateGet(textareaValues, textarea)
        : textareaDefaultValue(textarea),
    );
  globalThis.__tilefinchSetTextAreaValue = (textarea, value) => {
    const text = value === null ? "" : String(value);
    controlStateSet(textareaValues, textarea, text);
    __tilefinchSetControlValue(textarea.__handle, text);
  };
  Object.defineProperty(HTMLTextAreaElement.prototype, "defaultValue", {
    configurable: true,
    get() {
      return textareaDefaultValue(this);
    },
    set(value) {
      this.textContent = String(value);
    },
  });
  const inputTypes = new Set([
      "hidden",
      "text",
      "search",
      "tel",
      "url",
      "email",
      "password",
      "date",
      "month",
      "week",
      "time",
      "datetime-local",
      "number",
      "range",
      "color",
      "checkbox",
      "radio",
      "file",
      "submit",
      "image",
      "reset",
      "button",
    ]),
    textValueTypes = new Set([
      "text",
      "search",
      "tel",
      "url",
      "email",
      "password",
    ]),
    dateValuePatterns = {
      date: /^\d{4,}-\d{2}-\d{2}$/,
      month: /^\d{4,}-\d{2}$/,
      week: /^\d{4,}-W\d{2}$/,
      time: /^\d{2}:\d{2}(?::\d{2}(?:\.\d+)?)?$/,
      "datetime-local": /^\d{4,}-\d{2}-\d{2}T\d{2}:\d{2}(?::\d{2}(?:\.\d+)?)?$/,
    };
  globalThis.__tilefinchControlType = (control) => {
    const tag = String(control.tagName).toLowerCase(),
      raw = String(control.getAttribute("type") || "").toLowerCase();
    if (tag === "button")
      return ["submit", "reset", "button"].includes(raw) ? raw : "submit";
    return inputTypes.has(raw) ? raw : "text";
  };
  const inputValues = new Map(),
    nativeControlValues = new Map(),
    nativeControlKeys = (control) => {
      const keys = [],
        stable = String(control?.__tilefinchStableKey || ""),
        id = control?.id ? "i:" + String(control.id) : "";
      if (stable) keys.push(stable);
      if (id && id !== stable) keys.push(id);
      return keys;
    };
  globalThis.__tilefinchInputValue = (input) => {
    const type = __tilefinchControlType(input),
      attribute = input.getAttribute("value");
    if (type === "file") return "";
    if (type === "checkbox" || type === "radio")
      return attribute === null ? "on" : attribute;
    if (controlStateHas(inputValues, input))
      return controlStateGet(inputValues, input);
    if (type === "range" && attribute === null) return "50";
    if (type === "color" && attribute === null) return "#000000";
    return attribute || "";
  };
  globalThis.__tilefinchSetInputValue = (input, value) => {
    const type = __tilefinchControlType(input);
    if (type === "checkbox" || type === "radio") {
      input.setAttribute("value", value === null ? "" : String(value));
      return;
    }
    if (type === "file") {
      if (value === null || String(value) === "") return;
      if (value !== null && String(value) !== "")
        throw new DOMException(
          "File input values may only be cleared",
          "InvalidStateError",
        );
    }
    let text = value === null ? "" : String(value);
    if (textValueTypes.has(type)) text = text.replace(/[\r\n]/g, "");
    else if (dateValuePatterns[type] && !dateValuePatterns[type].test(text))
      text = "";
    else if (
      type === "number" &&
      (text.trim() === "" || !Number.isFinite(Number(text)))
    )
      text = "";
    else if (type === "range") {
      const numeric = Number(text),
        minimum = Number(input.getAttribute("min")),
        maximum = Number(input.getAttribute("max")),
        min =
          Number.isFinite(minimum) && input.getAttribute("min") !== null
            ? minimum
            : 0,
        max =
          Number.isFinite(maximum) && input.getAttribute("max") !== null
            ? maximum
            : 100;
      text = String(
        Number.isFinite(numeric)
          ? Math.min(max, Math.max(min, numeric))
          : (min + max) / 2,
      );
    } else if (type === "color" && !/^#[0-9a-f]{6}$/i.test(text))
      text = "#000000";
    controlStateSet(inputValues, input, text);
    __tilefinchSetControlValue(input.__handle, text);
  };
  globalThis.__tilefinchResetInputValue = (input) => {
    controlStateDelete(inputValues, input);
    __tilefinchSetControlValue(input.__handle, __tilefinchInputValue(input));
  };
  globalThis.__tilefinchSyncNativeControlValue = (control, value) => {
    const tag = String(control?.localName || control?.tagName).toLowerCase();
    for (const key of nativeControlKeys(control)) {
      if (nativeControlValues.has(key)) nativeControlValues.delete(key);
      while (nativeControlValues.size >= 256)
        nativeControlValues.delete(nativeControlValues.keys().next().value);
      nativeControlValues.set(key, String(value ?? ""));
    }
    if (tag === "input")
      controlStateSet(inputValues, control, String(value ?? ""));
    else if (tag === "textarea")
      controlStateSet(textareaValues, control, String(value ?? ""));
    if (tag === "input" || tag === "textarea")
      __tilefinchSetControlValue(control.__handle, String(value ?? ""));
  };
  const indeterminateState = new Map(),
    checkedDefaultState = new Map();
  globalThis.__tilefinchRegisterNativeNodeStateCleanup?.((handle) => {
    handle = Number(handle);
    optionSelectedState.delete(handle);
    selectedCollections.delete(handle);
    textareaValues.delete(handle);
    inputValues.delete(handle);
    indeterminateState.delete(handle);
    checkedDefaultState.delete(handle);
  });
  delete globalThis.__tilefinchRegisterNativeNodeStateCleanup;
  Object.defineProperty(HTMLInputElement.prototype, "indeterminate", {
    configurable: true,
    get() {
      return controlStateGet(indeterminateState, this) || false;
    },
    set(value) {
      value = !!value;
      controlStateSet(indeterminateState, this, value);
      this.toggleAttribute("data-tilefinch-indeterminate", value);
    },
  });
  globalThis.__tilefinchCopyFormCloneState = (source, clone, deep = false) => {
    const copyControl = (from, to) => {
      if (!from || !to) return;
      const tag = String(from.localName || "").toLowerCase();
      if (tag === "input") {
        to.value = from.value;
        to.checked = from.checked;
        to.indeterminate = from.indeterminate;
      } else if (tag === "textarea") {
        to.value = from.value;
      } else if (tag === "option") {
        to.selected = from.selected;
      }
    };
    copyControl(source, clone);
    if (!deep) return;
    for (const tag of ["input", "textarea", "option"]) {
      const fromControls = source.getElementsByTagName?.(tag) || [],
        toControls = clone?.getElementsByTagName?.(tag) || [],
        count = Math.min(fromControls.length, toControls.length);
      for (let index = 0; index < count; index++)
        copyControl(fromControls[index], toControls[index]);
    }
  };
  globalThis.__tilefinchSetChecked = (input, checked) => {
    if (!(input instanceof HTMLInputElement)) {
      input.toggleAttribute("checked", checked);
      return;
    }
    if (!controlStateHas(checkedDefaultState, input))
      controlStateSet(
        checkedDefaultState,
        input,
        input.hasAttribute("checked"),
      );
    if (checked && __tilefinchControlType(input) === "radio") {
      const form = input.closest("form"),
        name = input.name;
      if (name)
        for (const peer of document.querySelectorAll("input"))
          if (
            peer !== input &&
            peer instanceof HTMLInputElement &&
            __tilefinchControlType(peer) === "radio" &&
            peer.name === name &&
            peer.closest("form") === form
          )
            peer.removeAttribute("checked");
    }
    input.toggleAttribute("checked", checked);
  };
  globalThis.__tilefinchResetChecked = (input) => {
    const checked = controlStateHas(checkedDefaultState, input)
      ? !!controlStateGet(checkedDefaultState, input)
      : input.hasAttribute("checked");
    input.checked = checked;
    controlStateDelete(checkedDefaultState, input);
  };
  globalThis.ValidityState = class ValidityState {
    constructor(fields) {
      Object.assign(this, fields);
    }
  };
  const reflectedBoolean = (name) => ({
      configurable: true,
      get() {
        return this.hasAttribute(name);
      },
      set(value) {
        this.toggleAttribute(name, !!value);
      },
    }),
    reflectedInteger = (name, fallback) => ({
      configurable: true,
      get() {
        const value = this.getAttribute(name);
        return value === null ? fallback : Number.parseInt(value, 10);
      },
      set(value) {
        this.setAttribute(name, String(Number(value)));
      },
    }),
    controlWillValidate = (control) => {
      if (control.disabled || control.readOnly) return false;
      const type = String(control.type || "text").toLowerCase();
      return !["hidden", "button", "reset", "submit", "image"].includes(type);
    },
    controlValidity = (control) => {
      const value = String(control.value || ""),
        type = String(control.type || "text").toLowerCase(),
        required = control.required;
      let valueMissing = false;
      if (required) {
        if (type === "checkbox") valueMissing = !control.checked;
        else if (type === "radio") {
          const form = control.closest("form"),
            name = control.name;
          valueMissing = !document
            .querySelectorAll("input")
            .some(
              (item) =>
                item instanceof HTMLInputElement &&
                String(item.type).toLowerCase() === "radio" &&
                item.name === name &&
                item.closest("form") === form &&
                item.checked,
            );
        } else valueMissing = value === "";
      }
      let patternMismatch = false;
      const pattern = control.getAttribute("pattern");
      if (value && pattern !== null) {
        try {
          patternMismatch = !new RegExp("^(?:" + pattern + ")$", "u").test(
            value,
          );
        } catch (_) {}
      }
      const minimumLength = control.minLength,
        maximumLength = control.maxLength,
        tooShort =
          value !== "" && minimumLength >= 0 && value.length < minimumLength,
        tooLong = maximumLength >= 0 && value.length > maximumLength;
      let typeMismatch = false;
      if (value && type === "email")
        typeMismatch = !/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(value);
      else if (value && type === "url") {
        try {
          new URL(value);
        } catch (_) {
          typeMismatch = true;
        }
      }
      const numeric = Number(value),
        minimum = Number(control.getAttribute("min")),
        maximum = Number(control.getAttribute("max")),
        hasMinimum =
          control.getAttribute("min") !== null && Number.isFinite(minimum),
        hasMaximum =
          control.getAttribute("max") !== null && Number.isFinite(maximum),
        rangeUnderflow =
          value !== "" &&
          Number.isFinite(numeric) &&
          hasMinimum &&
          numeric < minimum,
        rangeOverflow =
          value !== "" &&
          Number.isFinite(numeric) &&
          hasMaximum &&
          numeric > maximum;
      let stepMismatch = false;
      const step = Number(control.getAttribute("step"));
      if (
        value !== "" &&
        Number.isFinite(numeric) &&
        Number.isFinite(step) &&
        step > 0
      ) {
        const valueAttribute = Number(control.getAttribute("value")),
          hasValueBase =
            control.getAttribute("value") !== null &&
            Number.isFinite(valueAttribute),
          base = hasMinimum ? minimum : hasValueBase ? valueAttribute : 0;
        stepMismatch =
          Math.abs(
            (numeric - base) / step - Math.round((numeric - base) / step),
          ) > 1e-9;
      }
      const customError = !!control.__customValidity,
        badInput = false,
        valid = !(
          valueMissing ||
          patternMismatch ||
          tooShort ||
          tooLong ||
          typeMismatch ||
          rangeUnderflow ||
          rangeOverflow ||
          stepMismatch ||
          customError ||
          badInput
        );
      return new ValidityState({
        valueMissing,
        typeMismatch,
        patternMismatch,
        tooLong,
        tooShort,
        rangeUnderflow,
        rangeOverflow,
        stepMismatch,
        badInput,
        customError,
        valid,
      });
    },
    validationDescriptors = {
      form: {
        configurable: true,
        get() {
          return nativeFormOwner(this);
        },
      },
      required: reflectedBoolean("required"),
      readOnly: reflectedBoolean("readonly"),
      minLength: reflectedInteger("minlength", -1),
      maxLength: reflectedInteger("maxlength", -1),
      pattern: {
        configurable: true,
        get() {
          return this.getAttribute("pattern") || "";
        },
        set(value) {
          this.setAttribute("pattern", String(value));
        },
      },
      min: {
        configurable: true,
        get() {
          return this.getAttribute("min") || "";
        },
        set(value) {
          this.setAttribute("min", String(value));
        },
      },
      max: {
        configurable: true,
        get() {
          return this.getAttribute("max") || "";
        },
        set(value) {
          this.setAttribute("max", String(value));
        },
      },
      step: {
        configurable: true,
        get() {
          return this.getAttribute("step") || "";
        },
        set(value) {
          this.setAttribute("step", String(value));
        },
      },
      willValidate: {
        configurable: true,
        get() {
          return controlWillValidate(this);
        },
      },
      validity: {
        configurable: true,
        get() {
          return controlValidity(this);
        },
      },
      validationMessage: {
        configurable: true,
        get() {
          const validity = controlValidity(this);
          if (validity.valid) return "";
          if (validity.customError) return this.__customValidity;
          return validity.valueMissing
            ? "Please fill out this field."
            : validity.typeMismatch
              ? "Please enter a valid value."
              : validity.patternMismatch
                ? "Please match the requested format."
                : validity.tooShort
                  ? "Please lengthen this text."
                  : validity.tooLong
                    ? "Please shorten this text."
                    : validity.rangeUnderflow
                      ? "Value is below the minimum."
                      : validity.rangeOverflow
                        ? "Value is above the maximum."
                        : validity.stepMismatch
                          ? "Please enter a valid step value."
                          : "Invalid value.";
        },
      },
    };
  for (const prototype of [
    HTMLInputElement.prototype,
    HTMLTextAreaElement.prototype,
    HTMLSelectElement.prototype,
  ]) {
    Object.defineProperties(prototype, validationDescriptors);
    prototype.setCustomValidity = function (message) {
      this.__customValidity = String(message);
    };
    prototype.checkValidity = function () {
      if (!this.willValidate || this.validity.valid) return true;
      this.dispatchEvent(
        __tilefinchTrustedEvent(new Event("invalid", { cancelable: true })),
      );
      return false;
    };
    prototype.reportValidity = prototype.checkValidity;
  }
  Object.defineProperties(HTMLFieldSetElement.prototype, {
    validity: {
      configurable: true,
      get() {
        return controlValidity(this);
      },
    },
    validationMessage: validationDescriptors.validationMessage,
    willValidate: {
      configurable: true,
      get() {
        return false;
      },
    },
  });
  HTMLFieldSetElement.prototype.setCustomValidity = function (message) {
    this.__customValidity = String(message);
  };
  HTMLFieldSetElement.prototype.checkValidity = function () {
    return true;
  };
  HTMLFieldSetElement.prototype.reportValidity =
    HTMLFieldSetElement.prototype.checkValidity;
  const labelableControl = (element) => {
    if (
      /^(?:button|input|meter|output|progress|select|textarea)$/.test(
        String(element?.localName || "").toLowerCase(),
      )
    )
      return true;
    const internals =
      globalThis.__tilefinchElementInternalsFor?.(element);
    if (
      !internals &&
      !globalThis.__tilefinchFormAssociatedCustomElement?.(element)
    )
      return false;
    if (!internals) return true;
    try {
      void internals.form;
      return true;
    } catch (_) {
      return false;
    }
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
        const target = this.htmlFor
          ? this.ownerDocument?.getElementById(this.htmlFor)
          : Array.from(this.querySelectorAll("*")).find(labelableControl);
        return labelableControl(target) ? target : null;
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
  const labelsForControl = (control) => {
    const labels = [],
      id = String(control.id || "");
    for (const label of document.querySelectorAll("label")) {
      if (!(label instanceof HTMLLabelElement)) continue;
      if (label.control === control || (id && label.htmlFor === id))
        labels.push(label);
    }
    return selectList(labels);
  };
  for (const prototype of [
    HTMLInputElement.prototype,
    HTMLTextAreaElement.prototype,
    HTMLSelectElement.prototype,
    HTMLButtonElement.prototype,
    HTMLOutputElement.prototype,
  ])
    Object.defineProperty(prototype, "labels", {
      configurable: true,
      get() {
        return labelsForControl(this);
      },
    });
  const outputValues = new WeakMap();
  Object.defineProperties(HTMLOutputElement.prototype, {
    form: {
      configurable: true,
      get() {
        return nativeFormOwner(this);
      },
    },
    defaultValue: {
      configurable: true,
      get() {
        return this.textContent;
      },
      set(value) {
        this.textContent = String(value);
        if (!outputValues.has(this)) outputValues.set(this, String(value));
      },
    },
    value: {
      configurable: true,
      get() {
        return outputValues.has(this)
          ? outputValues.get(this)
          : this.textContent;
      },
      set(value) {
        outputValues.set(this, String(value));
      },
    },
    willValidate: {
      configurable: true,
      get() {
        return false;
      },
    },
  });
  const numberValueTypes = new Set([
      "date",
      "month",
      "week",
      "time",
      "datetime-local",
      "number",
      "range",
    ]),
    dateValueTypes = new Set(["date", "month", "week", "time"]);
  Object.defineProperties(HTMLInputElement.prototype, {
    valueAsNumber: {
      configurable: true,
      get() {
        const type = __tilefinchControlType(this);
        if (!numberValueTypes.has(type) || this.value === "") return NaN;
        const number = Number(this.value);
        return Number.isFinite(number) ? number : NaN;
      },
      set(value) {
        const number = Number(value);
        if (!Number.isFinite(number))
          throw new TypeError("valueAsNumber requires a finite number");
        if (!numberValueTypes.has(__tilefinchControlType(this)))
          throw new DOMException(
            "valueAsNumber does not apply to this input type",
            "InvalidStateError",
          );
        this.value = String(number);
      },
    },
    valueAsDate: {
      configurable: true,
      get() {
        if (!dateValueTypes.has(__tilefinchControlType(this)) || !this.value)
          return null;
        const date = new Date(this.value);
        return Number.isFinite(date.getTime()) ? date : null;
      },
      set(value) {
        if (value !== null && !(value instanceof Date))
          throw new TypeError("valueAsDate requires a Date or null");
        if (!dateValueTypes.has(__tilefinchControlType(this)))
          throw new DOMException(
            "valueAsDate does not apply to this input type",
            "InvalidStateError",
          );
        if (value === null || !Number.isFinite(value.getTime())) {
          this.value = "";
          return;
        }
        const iso = value.toISOString();
        this.value =
          __tilefinchControlType(this) === "time"
            ? iso.slice(11, 19)
            : __tilefinchControlType(this) === "month"
              ? iso.slice(0, 7)
              : __tilefinchControlType(this) === "week"
                ? ""
                : iso.slice(0, 10);
      },
    },
  });
  const decimalPlaces = (value) => {
    const text = String(value).toLowerCase(),
      exponentAt = text.indexOf("e"),
      exponent =
        exponentAt < 0 ? 0 : Number.parseInt(text.slice(exponentAt + 1), 10),
      mantissa = exponentAt < 0 ? text : text.slice(0, exponentAt),
      point = mantissa.indexOf("."),
      fraction = point < 0 ? 0 : mantissa.length - point - 1;
    return Math.min(9, Math.max(0, fraction - (exponent || 0)));
  };
  const stepInput = (input, count) => {
    const type = __tilefinchControlType(input);
    if (type !== "number" && type !== "range")
      throw new DOMException(
        "Stepping does not apply to this input type",
        "InvalidStateError",
      );
    const stepAttribute = input.getAttribute("step");
    if (String(stepAttribute).toLowerCase() === "any")
      throw new DOMException(
        "Stepping is unavailable when step is any",
        "InvalidStateError",
      );
    const parsedStep = Number(stepAttribute),
      step =
        stepAttribute !== null && Number.isFinite(parsedStep) && parsedStep > 0
          ? parsedStep
          : 1,
      parsedMin = Number(input.getAttribute("min")),
      parsedMax = Number(input.getAttribute("max")),
      hasMin =
        input.getAttribute("min") !== null && Number.isFinite(parsedMin),
      hasMax =
        input.getAttribute("max") !== null && Number.isFinite(parsedMax),
      valueAttribute = Number(input.getAttribute("value")),
      hasValueBase =
        input.getAttribute("value") !== null &&
        Number.isFinite(valueAttribute),
      base = hasMin ? parsedMin : hasValueBase ? valueAttribute : 0;
    count = Math.trunc(Number(count) || 0);
    let value = Number(input.value);
    if (!Number.isFinite(value)) value = hasMin ? parsedMin : base;
    const precision = Math.max(
        decimalPlaces(step),
        decimalPlaces(base),
        decimalPlaces(value),
        hasMin ? decimalPlaces(parsedMin) : 0,
        hasMax ? decimalPlaces(parsedMax) : 0,
      ),
      scale = 10 ** precision,
      stepUnits = Math.max(1, Math.round(step * scale)),
      baseUnits = Math.round(base * scale);
    let valueUnits = Math.round(value * scale),
      remainder = (valueUnits - baseUnits) % stepUnits;
    if (remainder < 0) remainder += stepUnits;
    if (count > 0 && remainder !== 0) {
      valueUnits += stepUnits - remainder;
      count--;
    } else if (count < 0 && remainder !== 0) {
      valueUnits -= remainder;
      count++;
    }
    valueUnits += count * stepUnits;
    value = valueUnits / scale;
    if (hasMin && value < parsedMin) value = parsedMin;
    if (hasMax && value > parsedMax) value = parsedMax;
    input.value = String(value);
  };
  HTMLInputElement.prototype.stepUp = function (count = 1) {
    stepInput(this, count);
  };
  HTMLInputElement.prototype.stepDown = function (count = 1) {
    stepInput(this, -Number(count));
  };
  Object.defineProperties(HTMLFormElement.prototype, {
    noValidate: reflectedBoolean("novalidate"),
  });
  HTMLFormElement.prototype.checkValidity = function () {
    let valid = true;
    for (const control of formControls(this)) {
      const internals =
        globalThis.__tilefinchElementInternalsFor?.(control);
      if (
        (typeof control.checkValidity === "function" &&
          !control.checkValidity()) ||
        (internals && !internals.checkValidity())
      )
        valid = false;
    }
    return valid;
  };
  HTMLFormElement.prototype.reportValidity =
    HTMLFormElement.prototype.checkValidity;
  const resetForm = (form, deferCustomCallbacks = false) => {
    const event = __tilefinchTrustedEvent(
      new Event("reset", { bubbles: true, cancelable: true }),
    );
    if (!form.dispatchEvent(event)) return false;
    const resetCallbacks = [];
    for (const control of formControls(form)) {
      if (control instanceof HTMLInputElement) {
        __tilefinchResetInputValue(control);
        if (["checkbox", "radio"].includes(control.type))
          __tilefinchResetChecked(control);
      } else if (control instanceof HTMLTextAreaElement)
        control.value = control.defaultValue;
      else if (control instanceof HTMLSelectElement) {
        const options = selectOptions(control);
        for (const option of options)
          markOptionSelected(option, option.hasAttribute("selected"));
      } else if (control instanceof HTMLOutputElement) {
        control.value = control.defaultValue;
      } else {
        const definition = control.__tilefinchCustomElementDefinition,
          callback = definition?.callbacks?.formResetCallback;
        if (typeof callback === "function")
          resetCallbacks.push([control, callback]);
      }
    }
    const invokeCustomCallbacks = () => {
      for (const [control, callback] of resetCallbacks)
        try {
          globalThis.__tilefinchRunTask(
            "custom-element:formResetCallback",
            callback,
            control,
            [],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "custom element form reset");
        }
    };
    if (deferCustomCallbacks && resetCallbacks.length)
      queueMicrotask(invokeCustomCallbacks);
    else invokeCustomCallbacks();
    return true;
  };
  HTMLFormElement.prototype.reset = function () {
    resetForm(this);
  };
  globalThis.__tilefinchResetFormFromActivation = (form) =>
    resetForm(form, true);
  Object.defineProperties(HTMLButtonElement.prototype, {
    form: {
      configurable: true,
      get() {
        return nativeFormOwner(this);
      },
    },
    formNoValidate: reflectedBoolean("formnovalidate"),
  });
  Object.defineProperty(
    HTMLInputElement.prototype,
    "formNoValidate",
    reflectedBoolean("formnovalidate"),
  );
  globalThis.FormData = class FormData {
    constructor(form) {
      this.items = [];
      if (form !== undefined) {
        if (!(form instanceof HTMLFormElement))
          throw new TypeError("FormData constructor argument must be a form");
        for (const control of formControls(form)) {
          const name = String(
            control.name || control.getAttribute?.("name") || "",
          );
          if (
            !name ||
            control.disabled ||
            control.hasAttribute?.("disabled")
          )
            continue;
          const internals =
            globalThis.__tilefinchElementInternalsFor?.(control);
          if (internals) {
            const value = internals.__formValue;
            if (value instanceof FormData)
              for (const [entryName, entryValue] of value)
                this.append(entryName, entryValue);
            else if (value !== null) this.append(name, value);
            continue;
          }
          const tag = String(control.tagName || "").toLowerCase(),
            type = String(control.type || "").toLowerCase();
          if (
            tag === "button" ||
            tag === "output" ||
            type === "submit" ||
            type === "button" ||
            type === "reset" ||
            type === "image" ||
            type === "file"
          )
            continue;
          if ((type === "checkbox" || type === "radio") && !control.checked)
            continue;
          if (tag === "select") {
            const options = [];
            const collect = (node) => {
              for (const child of node.children || []) {
                if (String(child.tagName || "").toLowerCase() === "option")
                  options.push(child);
                collect(child);
              }
            };
            collect(control);
            const hasExplicit = options.some((option) => option.selected),
              selected = options.filter(
                (option) => option.selected && !option.disabled,
              );
            if (!selected.length && !hasExplicit) {
              const fallback = options.find((option) => !option.disabled);
              if (fallback) selected.push(fallback);
            }
            for (const option of selected)
              this.append(
                name,
                option.hasAttribute("value")
                  ? option.value
                  : option.textContent,
              );
            continue;
          }
          this.append(name, control.value);
        }
      }
    }
    append(name, value) {
      if (this.items.length >= 256)
        throw new RangeError("FormData entry limit exceeded");
      this.items.push([
        String(name),
        value instanceof Blob ? value : String(value),
      ]);
    }
    delete(name) {
      name = String(name);
      this.items = this.items.filter((pair) => pair[0] !== name);
    }
    get(name) {
      name = String(name);
      const pair = this.items.find((item) => item[0] === name);
      return pair ? pair[1] : null;
    }
    getAll(name) {
      name = String(name);
      return this.items
        .filter((item) => item[0] === name)
        .map((item) => item[1]);
    }
    has(name) {
      name = String(name);
      return this.items.some((item) => item[0] === name);
    }
    set(name, value) {
      name = String(name);
      value = value instanceof Blob ? value : String(value);
      let found = false;
      this.items = this.items.filter((pair) => {
        if (pair[0] !== name) return true;
        if (found) return false;
        pair[1] = value;
        found = true;
        return true;
      });
      if (!found) this.append(name, value);
    }
    entries() {
      return this.items[Symbol.iterator]();
    }
    keys() {
      return this.items.map((pair) => pair[0])[Symbol.iterator]();
    }
    values() {
      return this.items.map((pair) => pair[1])[Symbol.iterator]();
    }
    forEach(callback, thisArg) {
      for (const [name, value] of this.items)
        callback.call(thisArg, value, name, this);
    }
    [Symbol.iterator]() {
      return this.entries();
    }
  };
  globalThis.__tilefinchQueueFormSubmission = (form, submitter) => {
    if (!(form instanceof HTMLFormElement)) return false;
    const target = String(
      submitter?.getAttribute?.("formtarget") ??
        form.getAttribute("target") ??
        "",
    );
    if (!target || /^_(?:self|parent|top|blank)$/i.test(target))
      return false;
    let frame = null;
    for (const candidate of document.querySelectorAll("iframe"))
      if (candidate.getAttribute("name") === target) {
        frame = candidate;
        break;
      }
    if (!(frame instanceof HTMLIFrameElement)) return false;
    const method = String(
      submitter?.getAttribute?.("formmethod") ??
        form.getAttribute("method") ??
        "get",
    ).toLowerCase();
    /* Remote child-frame POST needs a body-bearing frame scheduler request.
       GET can use the existing dynamic iframe lifecycle without turning a
       named target into a top-level navigation. */
    if (method !== "get") return false;
    const action = String(
      submitter?.getAttribute?.("formaction") ??
        form.getAttribute("action") ??
        location.href,
    );
    let url;
    try {
      url = new URL(action || location.href, location.href);
    } catch (_) {
      return false;
    }
    const query = new URLSearchParams();
    for (const [name, value] of new FormData(form)) {
      if (value instanceof Blob) continue;
      query.append(name, value);
    }
    const submitterName = String(
      submitter?.name || submitter?.getAttribute?.("name") || "",
    );
    if (submitterName && !submitter.disabled)
      query.append(submitterName, String(submitter.value || ""));
    const serialized = query.toString();
    frame.src =
      url.origin +
      url.pathname +
      (serialized ? "?" + serialized : "") +
      url.hash;
    return true;
  };
  TilefinchURL.createObjectURL = (blob) => {
    if (!(blob instanceof Blob)) throw new TypeError("Blob required");
    if (
      blobURLs.size >= blobURLLimit ||
      blobURLBytes + blob.size > blobURLByteLimit
    )
      throw new RangeError("blob URL quota exceeded");
    const url = "blob:" + location.origin + "/" + nextBlobURL++;
    blobURLs.set(url, blob);
    blobURLBytes += blob.size;
    return url;
  };
  TilefinchURL.revokeObjectURL = (url) => {
    url = String(url);
    const blob = blobURLs.get(url);
    if (blob) {
      blobURLBytes -= blob.size;
      blobURLs.delete(url);
    }
  };
  globalThis.__tilefinchBlobForURL = (url) => blobURLs.get(String(url)) || null;
  const cloneWorkerValue = (value, depth = 0) => {
    if (depth > 8) throw new RangeError("worker message nesting limit");
    if (
      value === null ||
      typeof value === "string" ||
      typeof value === "number" ||
      typeof value === "boolean" ||
      typeof value === "undefined"
    )
      return value;
    if (value instanceof ArrayBuffer) return value.slice(0);
    if (ArrayBuffer.isView(value)) return new value.constructor(value);
    if (Array.isArray(value)) {
      if (value.length > 1024)
        throw new RangeError("worker message item limit");
      return value.map((item) => cloneWorkerValue(item, depth + 1));
    }
    if (Object.getPrototypeOf(value) === Object.prototype) {
      const copy = {};
      const keys = Object.keys(value);
      if (keys.length > 128) throw new RangeError("worker message key limit");
      for (const key of keys)
        copy[key] = cloneWorkerValue(value[key], depth + 1);
      return copy;
    }
    throw new TypeError("unsupported worker message value");
  };
  if (globalThis.structuredClone === undefined)
    globalThis.structuredClone = (value) => cloneWorkerValue(value);
  /* Capture the native compiler in this trusted closure. Native code removes
     its temporary global property before author script runs, so page code
     cannot turn the worker compiler into an unsafe-eval bypass. */
  const runWorkerNative = globalThis.__tilefinchRunWorker;
  let activeWorkers = 0;
  globalThis.Worker = class Worker {
    constructor(url) {
      const blob = blobURLs.get(String(url));
      if (!blob) throw new TypeError("only retained blob URLs are supported");
      if (activeWorkers >= 2) throw new RangeError("worker quota exceeded");
      activeWorkers++;
      this.active = true;
      this.onmessage = null;
      this.onerror = null;
      this.listeners = new Map();
      const owner = this,
        scope = { onmessage: null, onerror: null };
      scope.self = scope;
      scope.globalThis = scope;
      scope.postMessage = (value) => {
        const copied = cloneWorkerValue(value);
        setTimeout(() => owner.emit("message", { data: copied }), 0);
      };
      scope.addEventListener = (type, callback) => {
        if (typeof callback !== "function") return;
        const key = String(type);
        if (!scope._listeners) scope._listeners = new Map();
        if (!scope._listeners.has(key)) scope._listeners.set(key, []);
        scope._listeners.get(key).push(callback);
      };
      scope.removeEventListener = (type, callback) => {
        const list = scope._listeners?.get(String(type));
        if (!list) return;
        const at = list.indexOf(callback);
        if (at >= 0) list.splice(at, 1);
      };
      scope.crypto = crypto;
      scope.performance = performance;
      scope.setTimeout = setTimeout;
      scope.clearTimeout = clearTimeout;
      scope.TextEncoder = TextEncoder;
      scope.TextDecoder = TextDecoder;
      scope.Uint8Array = Uint8Array;
      scope.ArrayBuffer = ArrayBuffer;
      scope.DataView = DataView;
      scope.Blob = Blob;
      const proxy = new Proxy(scope, {
        has() {
          return true;
        },
        get(target, key) {
          return key in target ? target[key] : globalThis[key];
        },
        set(target, key, value) {
          target[key] = value;
          return true;
        },
      });
      this.scope = proxy;
      try {
        let source = new TextDecoder().decode(blob._bytes);
        source = source.split("import.meta").join("__tilefinchWorkerMeta");
        source = source.replace(/\bimport\(/g, "__tilefinchWorkerImport(");
        source = source.replace(/export\s*\{[^}]*\}\s*;?/g, ";");
        scope.__tilefinchWorkerMeta = { url: String(url) };
        runWorkerNative(proxy, source, String(url));
      } catch (error) {
        if (globalThis.console && console.log)
          console.log(
            "tilefinch-worker-error: " +
              String(error) +
              " || " +
              String((error && error.stack) || ""),
          );
        /* Construction did not produce a live worker. Release its bounded
           slot before surfacing the CSP/compile failure to the caller. */
        this.active = false;
        activeWorkers--;
        this.listeners.clear();
        this.scope = null;
        throw error;
      }
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
    emit(type, event = {}) {
      if (!this.active) return;
      event.type = type;
      event.target = this;
      const handler = this["on" + type];
      if (typeof handler === "function")
        globalThis.__tilefinchRunTask(
          "worker:" + String(type),
          handler,
          this,
          [event],
        );
      for (const callback of this.listeners.get(type) || [])
        globalThis.__tilefinchRunTask(
          "worker-listener:" + String(type),
          callback,
          this,
          [event],
        );
    }
    postMessage(value) {
      if (!this.active) throw new Error("Worker is terminated");
      const copied = cloneWorkerValue(value);
      setTimeout(() => {
        if (!this.active) return;
        const event = { type: "message", data: copied, target: this.scope };
        const handler = this.scope.onmessage;
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "worker-scope-message",
            handler,
            this.scope,
            [event],
          );
        for (const callback of this.scope._listeners?.get("message") || [])
          globalThis.__tilefinchRunTask(
            "worker-scope-listener",
            callback,
            this.scope,
            [event],
          );
      }, 0);
    }
    terminate() {
      if (this.active) {
        this.active = false;
        activeWorkers--;
        this.listeners.clear();
        this.scope = null;
      }
    }
  };
  const location = new TilefinchURL(
    String(globalThis.__tilefinchLocationHref || "https://example.invalid/"),
  );
  Object.defineProperty(location, "href", {
    configurable: false,
    enumerable: true,
    get() {
      return this._href;
    },
    set(value) {
      this.assign(value);
    },
  });
  Object.defineProperty(document, "forms", {
    configurable: true,
    enumerable: true,
    get() {
      return document.querySelectorAll("form");
    },
  });
  globalThis.__tilefinchQualifiedElementName = (namespace, qualifiedName) => {
    const namespaceURI =
        namespace === null || namespace === undefined || namespace === ""
          ? null
          : String(namespace),
      name = String(qualifiedName),
      match = name.match(
        /^([A-Za-z_][A-Za-z0-9_.-]*)(?::([A-Za-z_][A-Za-z0-9_.-]*))?$/,
      );
    if (!match)
      throw new DOMException(
        "Invalid qualified name",
        "InvalidCharacterError",
      );
    const prefix = match[2] === undefined ? null : match[1],
      xml = "http://www.w3.org/XML/1998/namespace",
      xmlns = "http://www.w3.org/2000/xmlns/";
    if (
      (prefix !== null && namespaceURI === null) ||
      (prefix === "xml" && namespaceURI !== xml) ||
      ((name === "xmlns" || prefix === "xmlns") &&
        namespaceURI !== xmlns) ||
      (namespaceURI === xmlns && name !== "xmlns" && prefix !== "xmlns")
    )
      throw new DOMException("Invalid namespace", "NamespaceError");
    return { namespaceURI, name };
  };
  {
    const htmlNamespace = "http://www.w3.org/1999/xhtml",
      detachedPrototypes = new Map(),
      collectAdoption = (node, owner, records) => {
        if (!node || node.nodeType === Node.DOCUMENT_NODE) return;
        const previousOwner = node.ownerDocument || null;
        if ("__detachedOwner" in node) node.__detachedOwner = owner;
        else
          Object.defineProperty(node, "__tilefinchAdoptedOwner", {
            configurable: true,
            writable: true,
            value: owner,
          });
        for (const attribute of node.attributes || [])
          attribute.__tilefinchAttributeOwnerDocument = owner;
        if (previousOwner && previousOwner !== owner)
          records.push({ node, oldDocument: previousOwner, newDocument: owner });
        const shadow = globalThis.__tilefinchShadowRootForHost?.(node);
        if (shadow) {
          if ("__detachedOwner" in shadow) shadow.__detachedOwner = owner;
          else
            Object.defineProperty(shadow, "__tilefinchAdoptedOwner", {
              configurable: true,
              writable: true,
              value: owner,
            });
          for (const child of shadow.childNodes || [])
            collectAdoption(child, owner, records);
        }
        for (const child of node.childNodes || [])
          collectAdoption(child, owner, records);
      },
      adoptOwner = (node, owner) => {
        const records = [];
        collectAdoption(node, owner, records);
        globalThis.__tilefinchPrepareCustomElementAdoptions?.(records);
        try {
          for (const record of records)
            globalThis.__tilefinchCustomElementAdopted?.(
              record.node,
              record.oldDocument,
              record.newDocument,
            );
        } finally {
          globalThis.__tilefinchFinishCustomElementAdoptions?.();
        }
      },
      detach = (node) => {
        const parent = node?.__detachedParent;
        if (parent) {
          let connected = !!parent.isConnected;
          for (let at = parent, steps = 0; at && steps < 64; steps++) {
            if (at.nodeType === Node.DOCUMENT_NODE) {
              connected = true;
              break;
            }
            at = at.__detachedParent;
          }
          const at = parent.__detachedChildren.indexOf(node);
          if (at >= 0) parent.__detachedChildren.splice(at, 1);
          node.__detachedParent = null;
          node.__tilefinchDetachedParent = null;
          if (connected)
            globalThis.__tilefinchCustomElementDisconnected?.(node);
          else
            globalThis.__tilefinchResyncCustomElementFormState?.(node);
        }
        return node;
      },
      insert = (parent, node, before = null) => {
        if (!node || !Array.isArray(parent.__detachedChildren))
          throw new TypeError("Node required");
        if (node.nodeType === Node.DOCUMENT_FRAGMENT_NODE) {
          const children = Array.from(node.childNodes || []);
          for (const child of children) insert(parent, child, before);
          return node;
        }
        const liveParent =
          node.__detachedParent === undefined ? node.parentNode : null;
        if (liveParent?.removeChild) liveParent.removeChild(node);
        detach(node);
        const at =
          before === null
            ? parent.__detachedChildren.length
            : parent.__detachedChildren.indexOf(before);
        if (at < 0)
          throw new DOMException(
            "Reference node is not a child",
            "NotFoundError",
          );
        parent.__detachedChildren.splice(at, 0, node);
        node.__detachedParent = parent;
        node.__tilefinchDetachedParent = parent;
        adoptOwner(
          node,
          parent.nodeType === Node.DOCUMENT_NODE
            ? parent
            : parent.ownerDocument,
        );
        if (node instanceof HTMLIFrameElement)
          globalThis.__tilefinchLoadLocalFrame?.(node);
        let connected = !!parent.isConnected;
        for (let at = parent, steps = 0; at && steps < 64; steps++) {
          if (at.nodeType === Node.DOCUMENT_NODE) {
            connected = true;
            break;
          }
          at = at.__detachedParent;
        }
        if (connected)
          globalThis.__tilefinchCustomElementConnected?.(node, true);
        else
          globalThis.__tilefinchResyncCustomElementFormState?.(node);
        return node;
      },
      sibling = (node, step) => {
        const parent = node.__detachedParent;
        if (!parent) return null;
        const at = parent.__detachedChildren.indexOf(node) + step;
        return at >= 0 && at < parent.__detachedChildren.length
          ? parent.__detachedChildren[at]
          : null;
      },
      cloneParsedNode = (owner, source, state, depth = 0) => {
        if (depth > 64 || ++state.nodes > 4096)
          throw new DOMException(
            "Detached HTML fragment exceeds implementation limits",
            "NotSupportedError",
          );
        let clone;
        if (source.nodeType === Node.TEXT_NODE)
          clone = owner.createTextNode(source.data);
        else if (source.nodeType === Node.COMMENT_NODE)
          clone = owner.createComment(source.data);
        else if (source.nodeType === Node.ELEMENT_NODE) {
          clone = owner.createElementNS(source.namespaceURI, source.localName);
          for (const attribute of source.attributes || [])
            clone.setAttributeNS(
              attribute.namespaceURI,
              attribute.name,
              attribute.value,
            );
          for (const child of source.childNodes || [])
            clone.appendChild(cloneParsedNode(owner, child, state, depth + 1));
        } else {
          return null;
        }
        return clone;
      },
      replaceDetachedHTML = (target, value) => {
        const source = String(value);
        if (source.length > 256 * 1024)
          throw new RangeError("detached HTML fragment exceeds bounded size");
        // Reuse the native fragment parser in an inert, disconnected
        // container, then copy into the detached document's ownership model.
        // This keeps createHTMLDocument useful without retaining a second
        // parser or exposing live-document nodes through the detached tree.
        const parsed = document.createElement("div");
        parsed.innerHTML = source;
        const replacements = [],
          state = { nodes: 0 };
        for (const child of parsed.childNodes || []) {
          const clone = cloneParsedNode(
            target.ownerDocument,
            child,
            state,
          );
          if (clone) replacements.push(clone);
        }
        for (const child of [...target.__detachedChildren]) detach(child);
        for (const child of replacements) insert(target, child);
      },
      elementPrototype = (base) => {
        let proto = detachedPrototypes.get(base);
        if (proto) return proto;
        proto = Object.create(base);
        Object.defineProperties(proto, {
          nodeType: { configurable: true, value: Node.ELEMENT_NODE },
          tagName: {
            get() {
              return this.__detachedNamespace === htmlNamespace
                ? this.__detachedTag.toUpperCase()
                : this.__detachedTag;
            },
          },
          nodeName: {
            get() {
              return this.tagName;
            },
          },
          localName: {
            get() {
              return this.__detachedTag;
            },
          },
          namespaceURI: {
            get() {
              return this.__detachedNamespace;
            },
          },
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
          childNodes: {
            get() {
              return this.__detachedChildren.slice();
            },
          },
          children: {
            get() {
              if (!this.__detachedChildrenCollection)
                Object.defineProperty(
                  this,
                  "__detachedChildrenCollection",
                  {
                    value: __tilefinchLiveHTMLCollection(() =>
                      this.__detachedChildren.filter(
                        (node) => node.nodeType === Node.ELEMENT_NODE,
                      ),
                    ),
                  },
                );
              return this.__detachedChildrenCollection;
            },
          },
          attributes: {
            get() {
              return globalThis.__tilefinchNamedNodeMapFor(
                this,
                () => this.__detachedAttributeNodes.slice(),
              );
            },
          },
          firstChild: {
            get() {
              return this.__detachedChildren[0] || null;
            },
          },
          lastChild: {
            get() {
              return (
                this.__detachedChildren[this.__detachedChildren.length - 1] ||
                null
              );
            },
          },
          nextSibling: {
            get() {
              return sibling(this, 1);
            },
          },
          previousSibling: {
            get() {
              return sibling(this, -1);
            },
          },
          textContent: {
            get() {
              return this.__detachedChildren
                .map((node) => node.textContent)
                .join("");
            },
            set(value) {
              for (const child of this.__detachedChildren)
                child.__detachedParent = null;
              this.__detachedChildren = [];
              const text = this.__detachedOwner.createTextNode(String(value));
              if (text.data) this.appendChild(text);
            },
          },
          innerText: {
            get() {
              return this.textContent;
            },
            set(value) {
              this.textContent = value;
            },
          },
          innerHTML: {
            set(value) {
              replaceDetachedHTML(this, value);
            },
          },
        });
        Object.assign(proto, {
          addEventListener(type, callback, options = false) {
            return EventTarget.prototype.addEventListener.call(
              this,
              type,
              callback,
              options,
            );
          },
          removeEventListener(type, callback, options = false) {
            return EventTarget.prototype.removeEventListener.call(
              this,
              type,
              callback,
              options,
            );
          },
          dispatchEvent(event) {
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
          appendChild(node) {
            return insert(this, node);
          },
          insertBefore(node, before) {
            return insert(this, node, before ?? null);
          },
          removeChild(node) {
            if (node?.__detachedParent !== this)
              throw new DOMException("Node is not a child", "NotFoundError");
            return detach(node);
          },
          replaceChild(node, old) {
            if (old?.__detachedParent !== this)
              throw new DOMException("Node is not a child", "NotFoundError");
            const next = old.nextSibling;
            detach(old);
            insert(this, node, next);
            return old;
          },
          remove() {
            detach(this);
          },
          cloneNode(deep = false) {
            const clone = this.__detachedOwner.createElementNS(
              this.namespaceURI,
              this.localName,
            );
            for (const attribute of this.attributes)
              clone.setAttributeNS(
                attribute.namespaceURI,
                attribute.name,
                attribute.value,
              );
            if (deep)
              for (const child of this.__detachedChildren)
                clone.appendChild(child.cloneNode(true));
            return clone;
          },
          getAttribute(name) {
            name = String(name);
            if (
              this.namespaceURI === htmlNamespace &&
              this.ownerDocument?.contentType === "text/html"
            )
              name = name.toLowerCase();
            const attribute = this.__detachedAttributeNodes.find(
              (item) => item.name === name,
            );
            return attribute ? attribute.value : null;
          },
          setAttribute(name, value) {
            name = String(name);
            if (
              this.namespaceURI === htmlNamespace &&
              this.ownerDocument?.contentType === "text/html"
            )
              name = name.toLowerCase();
            let attribute = this.__detachedAttributeNodes.find(
              (item) => item.namespaceURI === null && item.name === name,
            ),
              oldValue = attribute?.value ?? null;
            if (!attribute) {
              attribute = globalThis.__tilefinchCreateAttribute(
                this.ownerDocument,
                name,
              );
              attribute.__tilefinchAttributeOwner = this;
              this.__detachedAttributeNodes.push(attribute);
            }
            attribute.__tilefinchAttributeValue = String(value);
            globalThis.__tilefinchCustomElementAttributeChanged?.(
              this,
              name,
              oldValue,
              String(value),
              null,
            );
          },
          getAttributeNS(namespace, localName) {
            namespace =
              namespace === null || namespace === undefined || namespace === ""
                ? null
                : String(namespace);
            localName = String(localName);
            const attribute = this.__detachedAttributeNodes.find(
              (item) =>
                item.namespaceURI === namespace &&
                item.localName === localName,
            );
            return attribute ? attribute.value : null;
          },
          setAttributeNS(namespace, qualifiedName, value) {
            namespace =
              namespace === null || namespace === undefined || namespace === ""
                ? null
                : String(namespace);
            qualifiedName = String(qualifiedName);
            const colon = qualifiedName.indexOf(":"),
              prefix = colon < 0 ? null : qualifiedName.slice(0, colon),
              localName =
                colon < 0 ? qualifiedName : qualifiedName.slice(colon + 1);
            let attribute = this.__detachedAttributeNodes.find(
              (item) =>
                item.namespaceURI === namespace &&
                item.localName === localName,
            ),
              oldValue = attribute?.value ?? null;
            if (!attribute) {
              attribute = globalThis.__tilefinchCreateAttribute(
                this.ownerDocument,
                qualifiedName,
                namespace,
              );
              attribute.__tilefinchAttributeOwner = this;
              this.__detachedAttributeNodes.push(attribute);
            }
            attribute.__tilefinchAttributeName = qualifiedName;
            attribute.__tilefinchAttributeLocalName = localName;
            attribute.__tilefinchAttributePrefix = prefix;
            attribute.__tilefinchAttributeValue = String(value);
            globalThis.__tilefinchCustomElementAttributeChanged?.(
              this,
              localName,
              oldValue,
              String(value),
              namespace,
            );
          },
          hasAttribute(name) {
            return this.getAttribute(name) !== null;
          },
          hasAttributeNS(namespace, localName) {
            return this.getAttributeNS(namespace, localName) !== null;
          },
          removeAttribute(name) {
            name = String(name);
            if (
              this.namespaceURI === htmlNamespace &&
              this.ownerDocument?.contentType === "text/html"
            )
              name = name.toLowerCase();
            const at = this.__detachedAttributeNodes.findIndex(
              (item) => item.name === name,
            );
            if (at >= 0) {
              const [attribute] = this.__detachedAttributeNodes.splice(at, 1);
              attribute.__tilefinchAttributeOwner = null;
              globalThis.__tilefinchCustomElementAttributeChanged?.(
                this,
                attribute.localName,
                attribute.value,
                null,
                attribute.namespaceURI,
              );
            }
          },
          removeAttributeNS(namespace, localName) {
            namespace =
              namespace === null || namespace === undefined || namespace === ""
                ? null
                : String(namespace);
            localName = String(localName);
            const at = this.__detachedAttributeNodes.findIndex(
              (item) =>
                item.namespaceURI === namespace &&
                item.localName === localName,
            );
            if (at >= 0) {
              const [attribute] = this.__detachedAttributeNodes.splice(at, 1);
              attribute.__tilefinchAttributeOwner = null;
              globalThis.__tilefinchCustomElementAttributeChanged?.(
                this,
                attribute.localName,
                attribute.value,
                null,
                namespace,
              );
            }
          },
        });
        detachedPrototypes.set(base, proto);
        return proto;
      },
      textPrototype = (() => {
        const proto = Object.create(Text.prototype);
        Object.defineProperties(proto, {
          nodeType: { value: Node.TEXT_NODE },
          nodeName: { value: "#text" },
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
              return sibling(this, 1);
            },
          },
          previousSibling: {
            get() {
              return sibling(this, -1);
            },
          },
          textContent: {
            get() {
              return this.data;
            },
            set(value) {
              this.data = String(value);
            },
          },
          nodeValue: {
            get() {
              return this.data;
            },
            set(value) {
              this.data = String(value);
            },
          },
        });
        proto.cloneNode = function () {
          return this.__detachedOwner.createTextNode(this.data);
        };
        return proto;
      })();
    const detachedDocument = (
      title,
      xml = false,
      suppliedDoctype = null,
      namespace = null,
      qualifiedName = "",
      initialize = true,
      compatMode = "CSS1Compat",
    ) => {
      const doc = Object.create(
          xml ? XMLDocument.prototype : Document.prototype,
        ),
        makeElement = (tag, namespace = htmlNamespace) => {
          tag = String(tag).toLowerCase();
          const normalizedNamespace =
              namespace === null || namespace === ""
                ? null
                : String(namespace),
            base =
              normalizedNamespace === htmlNamespace
                ? tag === "body"
                  ? HTMLBodyElement.prototype
                  : tag === "frameset"
                    ? HTMLFrameSetElement.prototype
                    : HTMLElement.prototype
                : normalizedNamespace === "http://www.w3.org/2000/svg"
                  ? SVGElement.prototype
                  : Element.prototype,
            detachedPrototype = elementPrototype(base),
            /*
             * Detached documents have no browsing context and therefore do
             * not own a second set of interface prototypes. Keep the
             * platform operations as own descriptors while exposing the
             * standard interface prototype directly, as the DOM requires.
             */
            node = Object.create(base);
          const detachedDescriptors =
            Object.getOwnPropertyDescriptors(detachedPrototype);
          for (const descriptor of Object.values(detachedDescriptors))
            descriptor.configurable = true;
          Object.defineProperties(node, detachedDescriptors);
          Object.defineProperties(node, {
            __detachedOwner: { value: doc, writable: true },
            __detachedTag: { value: tag },
            __detachedNamespace: {
              value: normalizedNamespace,
              writable: true,
            },
            __detachedChildren: { value: [], writable: true },
            __detachedParent: { value: null, writable: true },
            __detachedAttributes: { value: new Map() },
            __detachedAttributeNodes: { value: [], writable: true },
            style: { value: {} },
          });
          return node;
        },
        makeData = (value, type, name) => {
          const node = Object.create(textPrototype);
          Object.defineProperties(node, {
            __detachedOwner: { value: doc, writable: true },
            __detachedParent: { value: null, writable: true },
            data: { value: String(value), writable: true },
            nodeType: { value: type },
            nodeName: { value: name },
          });
          node.cloneNode = () => makeData(node.data, type, name);
          return node;
        },
        walk = (root, selector, output) => {
          for (const child of Array.from(
            root.__detachedChildren || root.childNodes || [],
          )) {
            if (child.nodeType !== Node.ELEMENT_NODE) continue;
            const match =
              selector[0] === "#"
                ? child.getAttribute("id") === selector.slice(1)
                : child.localName === selector.toLowerCase();
            if (match) output.push(child);
            walk(child, selector, output);
          }
        };
      Object.defineProperty(doc, "__detachedChildren", {
        value: [],
        writable: true,
      });
      const rootElement = () =>
          doc.__detachedChildren.find(
            (node) => node.nodeType === Node.ELEMENT_NODE,
          ) || null,
        directElement = (root, names) => {
          if (
            !root ||
            root.namespaceURI !== htmlNamespace ||
            root.localName !== "html"
          )
            return null;
          return (
            root.children.find(
              (child) =>
                child.namespaceURI === htmlNamespace &&
                names.includes(child.localName),
            ) || null
          );
        };
      Object.defineProperties(doc, {
        nodeType: { value: Node.DOCUMENT_NODE },
        nodeName: { value: "#document" },
        ownerDocument: { value: null },
        contentType: { value: xml ? "application/xml" : "text/html" },
        compatMode: { value: compatMode },
        defaultView: { value: null, configurable: true },
        documentElement: { get: rootElement },
        doctype: {
          configurable: true,
          get() {
            return (
              this.__detachedChildren.find(
                (node) => node.nodeType === Node.DOCUMENT_TYPE_NODE,
              ) || null
            );
          },
        },
        head: {
          get() {
            return directElement(rootElement(), ["head"]);
          },
        },
        body: {
          get() {
            return directElement(rootElement(), ["body", "frameset"]);
          },
          set(value) {
            if (!(value instanceof Element))
              throw new TypeError("Body must be an element");
            if (
              !(value instanceof HTMLBodyElement) &&
              !(value instanceof HTMLFrameSetElement)
            )
              throw new DOMException(
                "Body must be body or frameset",
                "HierarchyRequestError",
              );
            if (value.namespaceURI !== htmlNamespace)
              throw new DOMException(
                "Body must be in the HTML namespace",
                "HierarchyRequestError",
              );
            const root = rootElement();
            if (!root)
              throw new DOMException(
                "Document has no root element",
                "HierarchyRequestError",
              );
            const current = this.body;
            if (current) root.replaceChild(value, current);
            else root.insertBefore(value, root.firstChild);
          },
        },
        scrollingElement: {
          get() {
            const root = this.documentElement;
            if (!root) return null;
            if (this.compatMode !== "BackCompat") return root;
            if (
              root.namespaceURI !== htmlNamespace ||
              root.localName !== "html"
            )
              return null;
            const body = this.body;
            if (!body) return null;
            const rootOverflow = String(
                root.style?.overflow || "visible",
              ).toLowerCase(),
              bodyOverflow = String(
                body.style?.overflow || "visible",
              ).toLowerCase();
            return rootOverflow === "visible" ||
              bodyOverflow === "visible" ||
              bodyOverflow === "clip"
              ? body
              : null;
          },
        },
        title: {
          get() {
            const item = this.querySelector("title");
            return item
              ? String(item.textContent).replace(/\s+/g, " ").trim()
              : "";
          },
          set(value) {
            let item = this.querySelector("title");
            if (!item) {
              const head = this.head;
              if (!head) return;
              item = this.createElement("title");
              head.appendChild(item);
            }
            item.textContent = String(value);
          },
        },
        childNodes: {
          get() {
            return this.__detachedChildren.slice();
          },
        },
        children: {
          get() {
            if (!this.__detachedChildrenCollection)
              Object.defineProperty(this, "__detachedChildrenCollection", {
                value: __tilefinchLiveHTMLCollection(() =>
                  this.__detachedChildren.filter(
                    (node) => node.nodeType === Node.ELEMENT_NODE,
                  ),
                ),
              });
            return this.__detachedChildrenCollection;
          },
        },
        firstChild: {
          get() {
            return this.__detachedChildren[0] || null;
          },
        },
        lastChild: {
          get() {
            return (
              this.__detachedChildren[this.__detachedChildren.length - 1] ||
              null
            );
          },
        },
      });
      doc.createElement = (tag) => makeElement(tag);
      doc.createElementNS = (namespace, tag) => {
        const qualified = globalThis.__tilefinchQualifiedElementName(
          namespace,
          tag,
        );
        return makeElement(qualified.name, qualified.namespaceURI);
      };
      doc.createAttribute = (name) =>
        __tilefinchCreateAttribute(
          doc,
          xml ? String(name) : String(name).toLowerCase(),
        );
      doc.createAttributeNS = (namespace, name) =>
        __tilefinchCreateAttribute(
          doc,
          String(name),
          namespace === null ? null : String(namespace),
        );
      doc.createTextNode = (value) => {
        const node = Object.create(textPrototype);
        Object.defineProperties(node, {
          __detachedOwner: { value: doc, writable: true },
          __detachedParent: { value: null, writable: true },
          data: { value: String(value), writable: true },
        });
        return node;
      };
      doc.createComment = (value) =>
        makeData(value, Node.COMMENT_NODE, "#comment");
      doc.createCDATASection = (value) =>
        makeData(value, Node.CDATA_SECTION_NODE, "#cdata-section");
      doc.createProcessingInstruction = (target, value) =>
        makeData(value, Node.PROCESSING_INSTRUCTION_NODE, String(target));
      doc.createDocumentType = (name, publicId = "", systemId = "") => {
        const node = Object.create(DocumentType.prototype);
        Object.defineProperties(node, {
          __detachedOwner: { value: doc, writable: true },
          __detachedParent: { value: null, writable: true },
          nodeType: { value: Node.DOCUMENT_TYPE_NODE },
          nodeName: { get() { return this.name; } },
          name: { value: String(name) },
          publicId: { value: String(publicId) },
          systemId: { value: String(systemId) },
          ownerDocument: { get() { return this.__detachedOwner; } },
          parentNode: { get() { return this.__detachedParent; } },
          childNodes: { get() { return []; } },
        });
        node.cloneNode = function () {
          return this.__detachedOwner.createDocumentType(
            this.name,
            this.publicId,
            this.systemId,
          );
        };
        return node;
      };
      doc.createDocumentFragment = () => {
        const node = makeElement("fragment", "");
        Object.defineProperty(node, "nodeType", {
          value: Node.DOCUMENT_FRAGMENT_NODE,
        });
        return node;
      };
      doc.appendChild = (node) => insert(doc, node);
      doc.removeChild = (node) => {
        if (node?.__detachedParent !== doc)
          throw new DOMException("Node is not a child", "NotFoundError");
        return detach(node);
      };
      doc.insertBefore = (node, child) => insert(doc, node, child ?? null);
      doc.replaceChild = (node, child) => {
        const next = child.nextSibling;
        doc.removeChild(child);
        insert(doc, node, next);
        return child;
      };
      doc.querySelector = (selector) => {
        const output = [];
        walk(doc, String(selector), output);
        return output[0] || null;
      };
      doc.querySelectorAll = (selector) => {
        const output = [];
        walk(doc, String(selector), output);
        return output;
      };
      doc.getElementById = (id) => doc.querySelector("#" + String(id));
      doc.getElementsByTagName = (tag) => doc.querySelectorAll(String(tag));
      doc.adoptNode = (node) => {
        if (node instanceof Document)
          throw new DOMException("Documents cannot be adopted", "NotSupportedError");
        if (node?.parentNode?.removeChild) node.parentNode.removeChild(node);
        else detach(node);
        adoptOwner(node, doc);
        return node;
      };
      doc.append = (...values) => {
        const nodes = values.map((value) =>
          value instanceof Node ? value : doc.createTextNode(String(value)),
        );
        const elements =
          doc.children.length +
          nodes.filter((node) => node.nodeType === Node.ELEMENT_NODE).length;
        if (
          nodes.some(
            (node) =>
              node.nodeType !== Node.ELEMENT_NODE &&
              node.nodeType !== Node.DOCUMENT_TYPE_NODE,
          ) ||
          elements > 1
        )
          throw new DOMException(
            "Invalid document child sequence",
            "HierarchyRequestError",
          );
        for (const node of nodes) doc.appendChild(node);
      };
      doc.cloneNode = (deep = false) => {
        const clone = detachedDocument(
          undefined,
          xml,
          null,
          null,
          "",
          false,
        );
        if (deep)
          for (const child of doc.__detachedChildren)
            clone.appendChild(child.cloneNode(true));
        return clone;
      };
      if (suppliedDoctype) doc.appendChild(suppliedDoctype);
      if (xml) {
        if (qualifiedName)
          doc.appendChild(makeElement(qualifiedName, namespace || ""));
      } else if (initialize) {
        const html = doc.appendChild(doc.createElement("html")),
          head = html.appendChild(doc.createElement("head"));
        if (title !== undefined) {
          const item = head.appendChild(doc.createElement("title"));
          item.textContent = String(title);
        }
        html.appendChild(doc.createElement("body"));
      }
      return doc;
    };
    globalThis.__tilefinchCreateFrameDocument = (standards = true) =>
      detachedDocument(
        undefined,
        false,
        null,
        null,
        "",
        true,
        standards ? "CSS1Compat" : "BackCompat",
      );
    globalThis.__tilefinchNewDocument = () =>
      detachedDocument(undefined, false, null, null, "", false);
    document.implementation = {
      createHTMLDocument: (title) => detachedDocument(title),
      createDocument: (namespace, qualifiedName, doctype = null) =>
        detachedDocument(
          undefined,
          true,
          doctype,
          namespace,
          qualifiedName || "",
        ),
      createDocumentType: (name, publicId, systemId) =>
        detachedDocument(undefined, true).createDocumentType(
          name,
          publicId,
          systemId,
        ),
      hasFeature: () => true,
    };
    globalThis.__tilefinchAdoptNodeOwner = adoptOwner;
    globalThis.__tilefinchDetachNode = detach;
  }
  document.adoptNode = (node) => {
    if (node instanceof Document)
      throw new DOMException("Documents cannot be adopted", "NotSupportedError");
    if (!(node instanceof Node)) throw new TypeError("Node required");
    if (node === document.doctype && "__detachedParent" in node)
      node.__detachedParent = null;
    else if (node.parentNode?.removeChild) node.parentNode.removeChild(node);
    else __tilefinchDetachNode(node);
    __tilefinchAdoptNodeOwner(node, document);
    return node;
  };
  Document.prototype.importNode = function (node, deep = false) {
    if (!(node instanceof Node)) throw new TypeError("Node required");
    if (node instanceof Document)
      throw new DOMException(
        "Documents cannot be imported",
        "NotSupportedError",
      );
    const clone = node.cloneNode(!!deep);
    globalThis.__tilefinchAdoptNodeOwner?.(clone, this);
    return clone;
  };
  Object.defineProperties(document, {
    nodeType: { configurable: true, enumerable: true, value: 9 },
    nodeName: { configurable: true, enumerable: true, value: "#document" },
    ownerDocument: { configurable: true, enumerable: true, value: null },
    contentType: { configurable: true, enumerable: true, value: "text/html" },
    compatMode: { configurable: true, enumerable: true, value: "CSS1Compat" },
    defaultView: { configurable: true, enumerable: true, value: globalThis },
  });
  document.location = location;
  Object.defineProperties(Document.prototype, {
    images: {
      configurable: true,
      enumerable: true,
      get() {
        if (!this.__tilefinchImagesCollection)
          Object.defineProperty(this, "__tilefinchImagesCollection", {
            configurable: true,
            value: globalThis.__tilefinchLiveHTMLCollection(() =>
              Array.from(this.querySelectorAll("img")),
            ),
          });
        return this.__tilefinchImagesCollection;
      },
    },
  });
  Object.defineProperties(document, {
    URL: {
      configurable: true,
      enumerable: true,
      get() {
        return location.href;
      },
    },
    documentURI: {
      configurable: true,
      enumerable: true,
      get() {
        return location.href;
      },
    },
    baseURI: {
      configurable: true,
      enumerable: true,
      get() {
        return __tilefinchDocumentBaseURI();
      },
    },
    domain: {
      configurable: true,
      enumerable: true,
      get() {
        return location.hostname;
      },
      set(value) {
        if (
          String(value).toLowerCase() !==
          String(location.hostname).toLowerCase()
        )
          throw new DOMException(
            "Origin relaxation is not supported",
            "SecurityError",
          );
      },
    },
  });
  {
    const stats = { writes: 0, text: "" };
    globalThis.__tilefinchClipboardStats = stats;
    const write = (value) => {
      value = String(value);
      if (value.length > 16384)
        throw new RangeError("clipboard text exceeds bounded size");
      stats.text = value;
      stats.writes++;
      return value;
    };
    document.execCommand = (command) => {
      command = String(command).toLowerCase();
      if (command !== "copy" && command !== "cut") return false;
      const control = globalThis.__tilefinchSelectedControl;
      if (!control || control.selectionStart === null) return false;
      write(
        String(control.value).slice(
          control.selectionStart,
          control.selectionEnd,
        ),
      );
      if (command === "cut")
        control.value =
          String(control.value).slice(0, control.selectionStart) +
          String(control.value).slice(control.selectionEnd);
      return true;
    };
    document.queryCommandSupported = (command) =>
      ["copy", "cut"].includes(String(command).toLowerCase());
    document.queryCommandEnabled = document.queryCommandSupported;
    globalThis.__tilefinchClipboardWrite = write;
  }
  Object.defineProperty(document, "lang", {
    get() {
      return document.documentElement?.getAttribute("lang") || "";
    },
    set(value) {
      document.documentElement?.setAttribute("lang", String(value));
    },
  });
  Object.defineProperty(document, "dir", {
    get() {
      const value = String(
        document.documentElement?.getAttribute("dir") || "",
      ).toLowerCase();
      return value === "ltr" || value === "rtl" ? value : "";
    },
    set(value) {
      document.documentElement?.setAttribute("dir", String(value));
    },
  });
  document.readyState = "loading";
  document.__tilefinchDocumentElementValue = wrap(__tilefinchDocumentElement());
  Object.defineProperty(document, "childNodes", {
    configurable: true,
    get() {
      return __tilefinchDocumentChildNodes().map(wrap);
    },
  });
  Object.defineProperty(document, "documentElement", {
    configurable: true,
    enumerable: true,
    get() {
      const next = wrap(__tilefinchDocumentElement());
      if (next) this.__tilefinchDocumentElementValue = next;
      return next;
    },
    set(value) {
      this.__tilefinchDocumentElementValue = value || null;
    },
  });
  document.__tilefinchHeadValue = wrap(__tilefinchQuery("head"));
  Object.defineProperty(document, "head", {
    configurable: true,
    enumerable: true,
    get() {
      const next = wrap(__tilefinchQuery("head"));
      if (next) this.__tilefinchHeadValue = next;
      return next;
    },
    set(value) {
      this.__tilefinchHeadValue = value || null;
    },
  });
  document.__tilefinchBodyValue = wrap(__tilefinchBody());
  Object.defineProperty(document, "body", {
    configurable: true,
    enumerable: true,
    get() {
      const next = wrap(__tilefinchBody());
      if (next) this.__tilefinchBodyValue = next;
      return next;
    },
    set(value) {
      if (!(value instanceof Element))
        throw new TypeError("Body must be an element");
      if (
        !(value instanceof HTMLBodyElement) &&
        !(value instanceof HTMLFrameSetElement)
      )
        throw new DOMException(
          "Body must be body or frameset",
          "HierarchyRequestError",
        );
      if (
        value.namespaceURI !== "http://www.w3.org/1999/xhtml" ||
        !this.documentElement
      )
        throw new DOMException(
          "Invalid document body",
          "HierarchyRequestError",
        );
      const current = wrap(__tilefinchBody());
      if (current) this.documentElement.replaceChild(value, current);
      else
        this.documentElement.insertBefore(
          value,
          this.documentElement.firstChild,
        );
      this.__tilefinchBodyValue = value;
    },
  });
  Object.defineProperty(document, "title", {
    configurable: true,
    enumerable: true,
    get() {
      const item = this.querySelector("title");
      return item ? String(item.textContent).replace(/\s+/g, " ").trim() : "";
    },
    set(value) {
      let item = this.querySelector("title");
      if (!item) {
        const head = this.head;
        if (!head) return;
        item = this.createElement("title");
        head.appendChild(item);
      }
      item.textContent = String(value);
    },
  });
  let activeElementHandle = Number(document.body?.__handle) || 0,
    activeElementValue = document.body;
  Object.defineProperty(document, "__activeElement", {
    configurable: true,
    get() {
      if (
        activeElementValue &&
        Number(activeElementValue.__handle) === activeElementHandle
      )
        return activeElementValue;
      activeElementValue = wrap(activeElementHandle) || document.body;
      return activeElementValue;
    },
    set(value) {
      activeElementValue = value || document.body;
      activeElementHandle = Number(activeElementValue?.__handle) || 0;
    },
  });
  globalThis.__tilefinchSetFocusHandle = (handle) => {
    handle = Number(handle) || 0;
    if (!handle) return false;
    if (activeElementHandle && activeElementHandle !== handle)
      __tilefinchRemoveAttribute(
        activeElementHandle,
        "data-tilefinch-focus",
      );
    activeElementHandle = handle;
    activeElementValue = null;
    __tilefinchSetAttribute(handle, "data-tilefinch-focus", "");
    return true;
  };
  Object.defineProperty(document, "activeElement", {
    get() {
      const active = document.__activeElement || document.body;
      return (
        globalThis.__tilefinchRetargetShadowEvent?.(active, document) ||
        active
      );
    },
  });
  document.hasFocus = () => true;
  const nodeExtent = (node) =>
    node?.nodeType === Node.TEXT_NODE || node?.nodeType === Node.COMMENT_NODE
      ? node.data.length
      : node?.childNodes?.length || 0;
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
  globalThis.Range = class Range {
    constructor() {
      this.startContainer = document;
      this.startOffset = 0;
      this.endContainer = document;
      this.endOffset = 0;
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
    setStart(node, offset) {
      this.startContainer = node;
      this.startOffset = boundedOffset(node, offset);
      if (this.endContainer === document) {
        this.endContainer = node;
        this.endOffset = this.startOffset;
      }
    }
    setEnd(node, offset) {
      this.endContainer = node;
      this.endOffset = boundedOffset(node, offset);
    }
    setStartBefore(node) {
      this.setStart(node.parentNode, node.parentNode.childNodes.indexOf(node));
    }
    setStartAfter(node) {
      this.setStart(
        node.parentNode,
        node.parentNode.childNodes.indexOf(node) + 1,
      );
    }
    setEndBefore(node) {
      this.setEnd(node.parentNode, node.parentNode.childNodes.indexOf(node));
    }
    setEndAfter(node) {
      this.setEnd(
        node.parentNode,
        node.parentNode.childNodes.indexOf(node) + 1,
      );
    }
    collapse(toStart = false) {
      if (toStart) {
        this.endContainer = this.startContainer;
        this.endOffset = this.startOffset;
      } else {
        this.startContainer = this.endContainer;
        this.startOffset = this.endOffset;
      }
    }
    selectNodeContents(node) {
      this.startContainer = node;
      this.startOffset = 0;
      this.endContainer = node;
      this.endOffset = nodeExtent(node);
    }
    cloneRange() {
      const copy = new Range();
      copy.startContainer = this.startContainer;
      copy.startOffset = this.startOffset;
      copy.endContainer = this.endContainer;
      copy.endOffset = this.endOffset;
      return copy;
    }
    extractContents() {
      const fragment = document.createDocumentFragment(),
        start = this.startContainer,
        end = this.endContainer,
        startOffset = this.startOffset,
        endOffset = this.endOffset,
        isCharacterData = (node) =>
          node?.nodeType === Node.TEXT_NODE ||
          node?.nodeType === Node.CDATA_SECTION_NODE ||
          node?.nodeType === Node.COMMENT_NODE ||
          node?.nodeType === Node.PROCESSING_INSTRUCTION_NODE;
      if (start === end) {
        if (isCharacterData(start)) {
          const removed = start.data.slice(startOffset, endOffset);
          start.deleteData(startOffset, endOffset - startOffset);
          if (removed) fragment.appendChild(document.createTextNode(removed));
        } else {
          const selected = [...start.childNodes].slice(startOffset, endOffset);
          for (const node of selected) fragment.appendChild(node);
        }
        this.setEnd(start, startOffset);
        return fragment;
      }
      if (
        isCharacterData(start) &&
        isCharacterData(end) &&
        start.parentNode === end.parentNode
      ) {
        const parent = start.parentNode,
          children = [...parent.childNodes],
          startIndex = children.indexOf(start),
          endIndex = children.indexOf(end),
          startText = start.data.slice(startOffset),
          endText = end.data.slice(0, endOffset);
        start.deleteData(startOffset, start.data.length - startOffset);
        if (startText) fragment.appendChild(document.createTextNode(startText));
        for (let at = startIndex + 1; at < endIndex; at++)
          fragment.appendChild(children[at]);
        end.deleteData(0, endOffset);
        if (endText) fragment.appendChild(document.createTextNode(endText));
        this.setEnd(start, startOffset);
        return fragment;
      }
      throw new DOMException(
        "Complex range extraction is not supported",
        "NotSupportedError",
      );
    }
    deleteContents() {
      this.extractContents();
    }
    insertNode(node) {
      if (!(node instanceof Node)) throw new TypeError("Node required");
      const container = this.startContainer,
        offset = this.startOffset,
        isCharacterData =
          container?.nodeType === Node.TEXT_NODE ||
          container?.nodeType === Node.CDATA_SECTION_NODE ||
          container?.nodeType === Node.COMMENT_NODE ||
          container?.nodeType === Node.PROCESSING_INSTRUCTION_NODE;
      if (isCharacterData) {
        const parent = container.parentNode;
        if (!parent)
          throw new DOMException(
            "Range boundary has no parent",
            "HierarchyRequestError",
          );
        const suffix = container.data.slice(offset);
        container.deleteData(offset, container.data.length - offset);
        const split = container.ownerDocument.createTextNode(suffix);
        parent.insertBefore(split, container.nextSibling);
        parent.insertBefore(node, split);
      } else {
        container.insertBefore(node, container.childNodes[offset] || null);
      }
    }
    surroundContents(newParent) {
      if (!(newParent instanceof Node)) throw new TypeError("Node required");
      const fragment = this.extractContents();
      this.insertNode(newParent);
      newParent.appendChild(fragment);
      this.selectNodeContents(newParent);
    }
    toString() {
      if (this.startContainer === this.endContainer) {
        const node = this.startContainer,
          text =
            node.nodeType === Node.TEXT_NODE ||
            node.nodeType === Node.COMMENT_NODE
              ? node.data
              : node.textContent;
        return String(text || "").slice(
          this.startOffset,
          node.nodeType === Node.TEXT_NODE ||
            node.nodeType === Node.COMMENT_NODE
            ? this.endOffset
            : undefined,
        );
      }
      return String(this.commonAncestorContainer?.textContent || "");
    }
    getBoundingClientRect() {
      const node = this.commonAncestorContainer;
      return node instanceof Element
        ? node.getBoundingClientRect()
        : node?.parentElement?.getBoundingClientRect() || new DOMRect();
    }
    getClientRects() {
      const rect = this.getBoundingClientRect();
      return rect.width && rect.height ? [rect] : [];
    }
    detach() {}
  };
  const selection = {
    _range: null,
    get rangeCount() {
      return this._range ? 1 : 0;
    },
    get anchorNode() {
      return this._range?.startContainer || null;
    },
    get anchorOffset() {
      return this._range?.startOffset || 0;
    },
    get focusNode() {
      return this._range?.endContainer || null;
    },
    get focusOffset() {
      return this._range?.endOffset || 0;
    },
    get isCollapsed() {
      return !this._range || this._range.collapsed;
    },
    get type() {
      return this._range ? (this._range.collapsed ? "Caret" : "Range") : "None";
    },
    addRange(range) {
      if (!(range instanceof Range)) throw new TypeError("Range required");
      this._range = range;
      document.__tilefinchSelectionChanged();
    },
    getRangeAt(index) {
      if (Number(index) !== 0 || !this._range)
        throw new DOMException("No range", "IndexSizeError");
      return this._range;
    },
    removeAllRanges() {
      this._range = null;
      document.__tilefinchSelectionChanged();
    },
    empty() {
      this.removeAllRanges();
    },
    collapse(node, offset = 0) {
      const range = new Range();
      range.setStart(node, offset);
      range.collapse(true);
      this._range = range;
      document.__tilefinchSelectionChanged();
    },
    setPosition(node, offset = 0) {
      this.collapse(node, offset);
    },
    selectAllChildren(node) {
      const range = new Range();
      range.selectNodeContents(node);
      this._range = range;
      document.__tilefinchSelectionChanged();
    },
    extend(node, offset = 0) {
      if (!this._range) this.collapse(node, offset);
      else {
        this._range.setEnd(node, offset);
        document.__tilefinchSelectionChanged();
      }
    },
    containsNode(node, allowPartial = false) {
      if (!this._range) return false;
      const root = this._range.commonAncestorContainer;
      return (
        root === node ||
        (root instanceof Element && root.contains(node)) ||
        (!allowPartial && node instanceof Element && node.contains(root))
      );
    },
    toString() {
      return this._range?.toString() || "";
    },
  };
  document.__tilefinchSelectionChanged = () =>
    document.dispatchEvent(new Event("selectionchange"));
  document.createRange = () => new Range();
  document.getSelection = globalThis.getSelection = () => selection;
  Object.setPrototypeOf(document, Document.prototype);
  document.createElement = (tag) => {
    tag = String(tag);
    let node = wrap(__tilefinchCreate(tag, "http://www.w3.org/1999/xhtml"));
    /*
     * Lexbor rejects a few valid HTML local names which its XML-oriented
     * constructor cannot represent (for example ":good:times:"). Preserve
     * their DOM identity in the bounded detached-node implementation even
     * though such names cannot participate in native layout.
     */
    if (!node && globalThis.__tilefinchNewDocument) {
      node = globalThis.__tilefinchNewDocument().createElement(tag);
      globalThis.__tilefinchAdoptNodeOwner?.(node, document);
    }
    if (node) node.__tilefinchProgrammatic = true;
    return node;
  };
  function Image(width, height) {
    const image = document.createElement("img");
    if (width !== undefined) image.width = Number(width) >>> 0;
    if (height !== undefined) image.height = Number(height) >>> 0;
    return image;
  }
  Image.prototype = HTMLImageElement.prototype;
  globalThis.Image = Image;
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
    Object.defineProperties(HTMLImageElement.prototype, {
      srcset: reflect("srcset"),
      sizes: reflect("sizes"),
      currentSrc: {
        configurable: true,
        enumerable: true,
        get() {
          const selected = __tilefinchImageProperty(this.__handle, 0);
          try {
            return selected ? new URL(selected, document.baseURI).href : "";
          } catch {
            return String(selected || "");
          }
        },
      },
      naturalWidth: {
        configurable: true,
        enumerable: true,
        get() {
          return Number(__tilefinchImageProperty(this.__handle, 1)) || 0;
        },
      },
      naturalHeight: {
        configurable: true,
        enumerable: true,
        get() {
          return Number(__tilefinchImageProperty(this.__handle, 2)) || 0;
        },
      },
      complete: {
        configurable: true,
        enumerable: true,
        get() {
          return !!__tilefinchImageProperty(this.__handle, 3);
        },
      },
    });
    Object.defineProperties(HTMLSourceElement.prototype, {
      srcset: reflect("srcset"),
      sizes: reflect("sizes"),
      media: reflect("media"),
      type: reflect("type"),
    });
    Object.defineProperties(HTMLIFrameElement.prototype, {
      srcdoc: reflect("srcdoc"),
      loading: reflect("loading"),
      referrerPolicy: reflect("referrerpolicy"),
    });
    Object.defineProperty(HTMLVideoElement.prototype, "poster", reflect("poster"));
  }
  {
    const states = new WeakMap(),
      stateFor = (node) => {
        let state = states.get(node);
        if (!state) {
          state = {
            paused: true,
            ended: false,
            seeking: false,
            currentTime: 0,
            duration: NaN,
            readyState: HTMLMediaElement.HAVE_NOTHING,
            networkState: HTMLMediaElement.NETWORK_EMPTY,
            volume: 1,
            muted: false,
            defaultMuted: false,
            playbackRate: 1,
            defaultPlaybackRate: 1,
          };
          states.set(node, state);
        }
        return state;
      },
      reflectString = (name) => ({
        configurable: true,
        enumerable: true,
        get() {
          return this.getAttribute(name) || "";
        },
        set(value) {
          this.setAttribute(name, String(value));
        },
      }),
      reflectBoolean = (name) => ({
        configurable: true,
        enumerable: true,
        get() {
          return this.hasAttribute(name);
        },
        set(value) {
          this.toggleAttribute(name, !!value);
        },
      });
    Object.defineProperties(HTMLMediaElement.prototype, {
      src: reflectString("src"),
      currentSrc: {
        configurable: true,
        enumerable: true,
        get() {
          const own = this.getAttribute("src");
          if (own) return new URL(own, location.href).href;
          const source = this.querySelector("source[src]"),
            value = source?.getAttribute("src") || "";
          try {
            return value ? new URL(value, location.href).href : "";
          } catch {
            return value;
          }
        },
      },
      crossOrigin: { ...reflectString("crossorigin") },
      preload: { ...reflectString("preload") },
      autoplay: reflectBoolean("autoplay"),
      loop: reflectBoolean("loop"),
      controls: reflectBoolean("controls"),
      playsInline: reflectBoolean("playsinline"),
      paused: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).paused;
        },
      },
      ended: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).ended;
        },
      },
      seeking: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).seeking;
        },
      },
      currentTime: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).currentTime;
        },
        set(value) {
          value = Number(value);
          if (!Number.isFinite(value) || value < 0)
            throw new TypeError("Invalid media time");
          const state = stateFor(this);
          state.seeking = true;
          state.currentTime = value;
          state.ended = false;
          this.dispatchEvent(new Event("seeking"));
          if (
            !__tilefinchRequestMedia(
              this.__handle,
              3,
              this.currentSrc,
              value,
            )
          ) {
            state.seeking = false;
            this.dispatchEvent(new Event("seeked"));
          } else {
            globalThis.__tilefinchActiveMediaNode = this;
            globalThis.__tilefinchActiveMediaState = state;
          }
        },
      },
      duration: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).duration;
        },
      },
      volume: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).volume;
        },
        set(value) {
          value = Number(value);
          if (!Number.isFinite(value) || value < 0 || value > 1)
            throw new DOMException(
              "Volume must be between 0 and 1",
              "IndexSizeError",
            );
          stateFor(this).volume = value;
          this.dispatchEvent(new Event("volumechange"));
        },
      },
      muted: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).muted;
        },
        set(value) {
          stateFor(this).muted = !!value;
          this.dispatchEvent(new Event("volumechange"));
        },
      },
      defaultMuted: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).defaultMuted || this.hasAttribute("muted");
        },
        set(value) {
          const state = stateFor(this);
          state.defaultMuted = !!value;
          this.toggleAttribute("muted", !!value);
        },
      },
      playbackRate: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).playbackRate;
        },
        set(value) {
          value = Number(value);
          if (!Number.isFinite(value) || value === 0)
            throw new DOMException(
              "Invalid playback rate",
              "NotSupportedError",
            );
          stateFor(this).playbackRate = value;
          this.dispatchEvent(new Event("ratechange"));
        },
      },
      defaultPlaybackRate: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).defaultPlaybackRate;
        },
        set(value) {
          value = Number(value);
          if (!Number.isFinite(value) || value === 0)
            throw new DOMException(
              "Invalid playback rate",
              "NotSupportedError",
            );
          stateFor(this).defaultPlaybackRate = value;
        },
      },
      readyState: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).readyState;
        },
      },
      networkState: {
        configurable: true,
        enumerable: true,
        get() {
          return stateFor(this).networkState;
        },
      },
    });
    Object.assign(HTMLMediaElement, {
      NETWORK_EMPTY: 0,
      NETWORK_IDLE: 1,
      NETWORK_LOADING: 2,
      NETWORK_NO_SOURCE: 3,
      HAVE_NOTHING: 0,
      HAVE_METADATA: 1,
      HAVE_CURRENT_DATA: 2,
      HAVE_FUTURE_DATA: 3,
      HAVE_ENOUGH_DATA: 4,
    });
    for (const name of [
      "NETWORK_EMPTY",
      "NETWORK_IDLE",
      "NETWORK_LOADING",
      "NETWORK_NO_SOURCE",
      "HAVE_NOTHING",
      "HAVE_METADATA",
      "HAVE_CURRENT_DATA",
      "HAVE_FUTURE_DATA",
      "HAVE_ENOUGH_DATA",
    ])
      Object.defineProperty(HTMLMediaElement.prototype, name, {
        value: HTMLMediaElement[name],
        enumerable: true,
      });
    HTMLMediaElement.prototype.canPlayType = function (type) {
      type = String(type || "").toLowerCase();
      if (
        type.startsWith("video/mp4") &&
        (!type.includes("codecs=") ||
          type.includes("avc1") ||
          type.includes("mp4a"))
      )
        return "probably";
      if (type.startsWith("audio/mp4") && type.includes("mp4a"))
        return "probably";
      return "";
    };
    HTMLMediaElement.prototype.load = function () {
      const state = stateFor(this);
      state.paused = true;
      state.ended = false;
      state.seeking = false;
      state.currentTime = 0;
      this.dispatchEvent(new Event("emptied"));
      const source = this.currentSrc;
      if (source && __tilefinchRequestMedia(this.__handle, 0, source, 0)) {
        state.networkState = HTMLMediaElement.NETWORK_LOADING;
        globalThis.__tilefinchActiveMediaNode = this;
        globalThis.__tilefinchActiveMediaState = state;
      } else {
        state.networkState = source
          ? HTMLMediaElement.NETWORK_NO_SOURCE
          : HTMLMediaElement.NETWORK_EMPTY;
      }
    };
    HTMLMediaElement.prototype.play = function () {
      const state = stateFor(this),
        source = this.currentSrc;
      if (
        !source ||
        !__tilefinchRequestMedia(this.__handle, 1, source, 0)
      ) {
        state.paused = true;
        return Promise.reject(
          new DOMException("No supported media source", "NotSupportedError"),
        );
      }
      if (state.paused) {
        state.paused = false;
        state.ended = false;
        state.networkState = HTMLMediaElement.NETWORK_LOADING;
        this.dispatchEvent(new Event("play"));
        this.dispatchEvent(new Event("waiting"));
      }
      globalThis.__tilefinchActiveMediaNode = this;
      globalThis.__tilefinchActiveMediaState = state;
      return Promise.resolve();
    };
    HTMLMediaElement.prototype.pause = function () {
      const state = stateFor(this);
      if (__tilefinchRequestMedia(this.__handle, 2, this.currentSrc, 0)) {
        globalThis.__tilefinchActiveMediaNode = this;
        globalThis.__tilefinchActiveMediaState = state;
      }
      if (!state.paused) {
        state.paused = true;
        this.dispatchEvent(new Event("pause"));
      }
    };
    HTMLMediaElement.prototype.fastSeek = function (time) {
      this.currentTime = time;
    };
    function Audio(src) {
      const audio = document.createElement("audio");
      audio.preload = "auto";
      if (src !== undefined) audio.src = String(src);
      return audio;
    }
    Audio.prototype = HTMLAudioElement.prototype;
    globalThis.Audio = Audio;
  }
  document.createDocumentFragment = () => {
    const node = wrap(__tilefinchCreateFragment());
    if (node) Object.setPrototypeOf(node, DocumentFragment.prototype);
    return node;
  };
  document.createTextNode = (value) => {
    const node = wrap(__tilefinchCreateText(String(value)));
    if (node) node.__tilefinchProgrammatic = true;
    return node;
  };
  document.createComment = (value) => {
    const node = wrap(__tilefinchCreateComment(String(value)));
    if (node) node.__tilefinchProgrammatic = true;
    return node;
  };
  document.createAttribute = (name) =>
    __tilefinchCreateAttribute(document, String(name).toLowerCase());
  document.createAttributeNS = (namespace, name) =>
    __tilefinchCreateAttribute(
      document,
      String(name),
      namespace === null ? null : String(namespace),
    );
  document.createElementNS = (namespace, tag) => {
    const qualified = globalThis.__tilefinchQualifiedElementName(namespace, tag);
    const node = wrap(
      __tilefinchCreate(qualified.name, qualified.namespaceURI),
    );
    if (node) {
      node.__tilefinchProgrammatic = true;
      node.__namespaceURI = qualified.namespaceURI;
      Object.setPrototypeOf(
        node,
        globalThis.__tilefinchElementPrototype(
          node.tagName,
          node.nodeType,
          qualified.namespaceURI,
        ),
      );
    }
    return node;
  };
  globalThis.__tilefinchMaterializeDetachedNode = (node) => {
    if (!node || node.__handle !== undefined) return node;
    const owner = node.ownerDocument,
      parent = node.parentNode,
      logicalType = node.nodeType,
      logicalName = node.nodeName,
      connected =
        node.nodeType === Node.TEXT_NODE
          ? document.createTextNode(node.data)
          : node.nodeType === Node.COMMENT_NODE
            ? document.createComment(node.data)
            : document.createElementNS(node.namespaceURI, node.localName);
    if (
      logicalType !== Node.ELEMENT_NODE &&
      logicalType !== Node.TEXT_NODE &&
      logicalType !== Node.COMMENT_NODE
    )
      Object.defineProperties(connected, {
        nodeType: {
          configurable: true,
          enumerable: true,
          value: logicalType,
        },
        nodeName: {
          configurable: true,
          enumerable: true,
          value: logicalName,
        },
      });
    if (node.nodeType === Node.ELEMENT_NODE) {
      for (const [name, value] of node.__detachedAttributes || [])
        connected.setAttribute(name, value);
      for (const child of [...(node.childNodes || [])])
        connected.appendChild(__tilefinchMaterializeDetachedNode(child));
    }
    if (parent?.removeChild) parent.removeChild(node);
    for (const [key, descriptor] of Object.entries(
      Object.getOwnPropertyDescriptors(connected),
    )) {
      const existing = Object.getOwnPropertyDescriptor(node, key);
      if (!existing || existing.configurable)
        Object.defineProperty(node, key, descriptor);
    }
    Object.setPrototypeOf(node, Object.getPrototypeOf(connected));
    Object.defineProperty(node, "__tilefinchAdoptedOwner", {
      configurable: true,
      writable: true,
      value: document,
    });
    return node;
  };
  globalThis.DOMParser = class DOMParser {
    parseFromString(input, type) {
      const source = String(input),
        mime = String(type).trim().toLowerCase();
      if (
        mime === "text/xml" ||
        mime === "application/xml" ||
        mime === "application/xhtml+xml" ||
        mime === "image/svg+xml"
      ) {
        const match = source.match(
          /<\s*([A-Za-z_][A-Za-z0-9_.:-]*)\b[^>]*\/?\s*>/,
        );
        return document.implementation.createDocument(
          mime === "image/svg+xml"
            ? "http://www.w3.org/2000/svg"
            : null,
          match ? match[1] : "parsererror",
          null,
        );
      }
      if (mime !== "text/html")
        throw new TypeError("Only text/html parsing is supported");
      if (source.length > 256 * 1024)
        throw new RangeError("parsed document exceeds bounded size");
      const html = document.createElement("html"),
        head = document.createElement("head"),
        body = document.createElement("body"),
        doctype = document.implementation.createDocumentType("html", "", "");
      html.appendChild(head);
      html.appendChild(body);
      body.innerHTML = source;
      const parsed = Object.create(Document.prototype);
      doctype.__detachedOwner = parsed;
      Object.defineProperties(parsed, {
        nodeType: { value: Node.DOCUMENT_NODE },
        nodeName: { value: "#document" },
        documentElement: { value: html, enumerable: true },
        head: { value: head, enumerable: true },
        body: { value: body, enumerable: true },
        contentType: { value: "text/html", enumerable: true },
        URL: { value: "about:blank", enumerable: true },
        documentURI: { value: "about:blank", enumerable: true },
        characterSet: { value: "UTF-8", enumerable: true },
        compatMode: { value: "CSS1Compat", enumerable: true },
        readyState: { value: "complete", enumerable: true },
        defaultView: {
          configurable: true,
          value: null,
          enumerable: true,
        },
        doctype: { value: doctype, enumerable: true },
        childNodes: {
          get() {
            return [doctype, html];
          },
        },
      });
      parsed.querySelector = (selector) => html.querySelector(String(selector));
      parsed.querySelectorAll = (selector) =>
        html.querySelectorAll(String(selector));
      parsed.getElementById = (id) =>
        html.querySelector("#" + CSS.escape(String(id)));
      parsed.createElement = document.createElement;
      parsed.createElementNS = document.createElementNS;
      parsed.createTextNode = document.createTextNode;
      parsed.createComment = document.createComment;
      parsed.createDocumentFragment = document.createDocumentFragment;
      parsed.cloneNode = (deep = false) =>
        deep
          ? document.implementation.createHTMLDocument()
          : new Document();
      return parsed;
    }
  };
  const listenerCallable = (callback) =>
    typeof callback === "function" ||
    (callback !== null && typeof callback === "object");
  const listenerCapture = (options) =>
    options !== null && typeof options === "object"
      ? !!options.capture
      : !!options;
  const focusEventTypes = new Set(["blur", "focus", "focusin", "focusout"]);
  let focusObserverCount = 0;
  globalThis.__tilefinchFocusObserverDelta = (type, delta) => {
    if (!focusEventTypes.has(String(type))) return;
    focusObserverCount = Math.max(0, focusObserverCount + Number(delta || 0));
  };
  globalThis.__tilefinchFocusEventsObserved = () => focusObserverCount !== 0;
  globalThis.__tilefinchAddEventListener = (
    map,
    type,
    callback,
    options = false,
  ) => {
    const key = String(type),
      capture = listenerCapture(options),
      once = typeof options === "object" && !!options?.once,
      passive = typeof options === "object" && !!options?.passive,
      signal = typeof options === "object" ? options?.signal : null;
    if (!listenerCallable(callback)) return;
    if (signal?.aborted) return;
    if (!map.has(key)) map.set(key, []);
    const list = map.get(key);
    if (
      list.some(
        (item) => item.callback === callback && item.capture === capture,
      )
    )
      return;
    const item = { callback, capture, once, passive, signal, abort: null };
    if (signal && typeof signal.addEventListener === "function") {
      item.abort = () =>
        globalThis.__tilefinchRemoveEventListener(map, key, callback, capture);
      signal.addEventListener("abort", item.abort, { once: true });
    }
    list.push(item);
    globalThis.__tilefinchFocusObserverDelta(key, 1);
  };
  globalThis.__tilefinchRemoveEventListener = (
    map,
    type,
    callback,
    options = false,
  ) => {
    const capture = listenerCapture(options);
    const list = map.get(String(type));
    if (!list) return;
    const at = list.findIndex(
        (item) => item.callback === callback && item.capture === capture,
      );
    if (at < 0) return;
    const [item] = list.splice(at, 1);
    globalThis.__tilefinchFocusObserverDelta(type, -1);
    if (item.signal && item.abort)
      try {
        item.signal.removeEventListener("abort", item.abort);
      } catch (_) {}
  };
  globalThis.__tilefinchInvokeListenerList = (map, target, event, capture) => {
    const list = map.get(String(event.type)) || [];
    for (const item of [...list]) {
      if (item.capture !== capture) continue;
      event.__passive = item.passive;
      try {
        if (typeof item.callback === "function")
          globalThis.__tilefinchRunTask(
            "event:" + String(event.type),
            item.callback,
            target,
            [event],
          );
        else if (typeof item.callback.handleEvent === "function")
          globalThis.__tilefinchRunTask(
            "event-object:" + String(event.type),
            item.callback.handleEvent,
            item.callback,
            [event],
          );
      } catch (error) {
        __tilefinchReportUncaught(error, "event " + event.type);
      } finally {
        event.__passive = false;
        if (item.once)
          globalThis.__tilefinchRemoveEventListener(
            map,
            event.type,
            item.callback,
            item.capture,
          );
      }
      if (event.__immediateStopped) break;
    }
  };
  globalThis.__tilefinchPrepareEvent = (event, target, path) => {
    if (!(event instanceof Event)) throw new TypeError("Event required");
    if (!event.__initialized || !String(event.type))
      throw new DOMException("Event is not initialized", "InvalidStateError");
    if (event.__dispatching)
      throw new DOMException("Event is already dispatching", "InvalidStateError");
    event.__dispatching = true;
    event.type = String(event.type);
    event.target = target;
    event.currentTarget = null;
    event.eventPhase = 0;
    event.defaultPrevented = !!event.defaultPrevented;
    event.__passive = false;
    event.__path = path;
    event.composedPath = () =>
      globalThis.__tilefinchVisibleShadowEventPath
        ? globalThis.__tilefinchVisibleShadowEventPath(
            path,
            event.currentTarget,
          )
        : event.currentTarget
          ? path.slice()
          : [];
    event.preventDefault = () => {
      if (event.cancelable !== false && !event.__passive)
        event.defaultPrevented = true;
    };
    event.stopPropagation = () => {
      event.__stopped = true;
    };
    event.stopImmediatePropagation = () => {
      event.__stopped = true;
      event.__immediateStopped = true;
    };
  };
  document.addEventListener = (type, callback, options = false) =>
    globalThis.__tilefinchAddEventListener(
      documentListeners,
      type,
      callback,
      options,
    );
  document.removeEventListener = (type, callback, options = false) =>
    globalThis.__tilefinchRemoveEventListener(
      documentListeners,
      type,
      callback,
      options,
    );
  globalThis.__tilefinchInvokeDocumentEvent = (event, capture) => {
    event.currentTarget = document;
    event.eventPhase = document === event.target ? 2 : capture ? 1 : 3;
    if (!capture) {
      const handler = document["on" + event.type];
      if (typeof handler === "function")
        try {
          globalThis.__tilefinchRunTask(
            "document-handler:" + String(event.type),
            handler,
            document,
            [event],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "document event " + event.type);
        }
    }
    globalThis.__tilefinchInvokeListenerList(
      documentListeners,
      document,
      event,
      capture,
    );
  };
  document.dispatchEvent = (event) => {
    const value = event,
      path = [document, globalThis];
    globalThis.__tilefinchPrepareEvent(value, document, path);
    if (!value.__stopped)
      globalThis.__tilefinchInvokeWindowEvent?.(value, true);
    if (!value.__stopped) {
      globalThis.__tilefinchInvokeDocumentEvent(value, true);
      if (!value.__immediateStopped)
        globalThis.__tilefinchInvokeDocumentEvent(value, false);
    }
    if (value.bubbles && !value.__stopped)
      globalThis.__tilefinchInvokeWindowEvent?.(value, false);
    value.currentTarget = null;
    value.eventPhase = 0;
    value.__dispatching = false;
    __tilefinchRecordEvent();
    return !value.defaultPrevented;
  };
  {
    const maps = new WeakMap(),
      mapFor = (target) => {
        let map = maps.get(target);
        if (!map) {
          map = new Map();
          maps.set(target, map);
        }
        return map;
      };
    globalThis.__tilefinchInvokeEventTarget = (
      target,
      event,
      capture,
      phase,
    ) => {
      event.currentTarget = target;
      event.eventPhase = phase;
      globalThis.__tilefinchInvokeListenerList(
        mapFor(target),
        target,
        event,
        capture,
      );
    };
    EventTarget.prototype.addEventListener = function (
      type,
      callback,
      options = false,
    ) {
      return globalThis.__tilefinchAddEventListener(
        mapFor(this),
        type,
        callback,
        options,
      );
    };
    EventTarget.prototype.removeEventListener = function (
      type,
      callback,
      options = false,
    ) {
      return globalThis.__tilefinchRemoveEventListener(
        mapFor(this),
        type,
        callback,
        options,
      );
    };
    EventTarget.prototype.dispatchEvent = function (event) {
      const value = event,
        map = mapFor(this);
      globalThis.__tilefinchPrepareEvent(value, this, [this]);
      if (!value.__stopped) {
        value.currentTarget = this;
        value.eventPhase = 2;
        globalThis.__tilefinchInvokeListenerList(map, this, value, true);
        if (!value.__immediateStopped)
          globalThis.__tilefinchInvokeListenerList(map, this, value, false);
      }
      value.currentTarget = null;
      value.eventPhase = 0;
      value.__dispatching = false;
      __tilefinchRecordEvent();
      return !value.defaultPrevented;
    };
    Node.prototype.addEventListener = EventTarget.prototype.addEventListener;
    Node.prototype.removeEventListener =
      EventTarget.prototype.removeEventListener;
    Node.prototype.dispatchEvent = function (event) {
      const path = boundedAncestorPath(
        this,
        (at) => at.__tilefinchDetachedParent || at.parentNode,
      );
      globalThis.__tilefinchPrepareEvent(event, this, path);
      for (let at = path.length - 1; at >= 1 && !event.__stopped; at--)
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
    };
  }
  document.head = wrap(__tilefinchQuery("head"));
  globalThis.__tilefinchRebindDocument = () => {
    globalThis.__tilefinchClearNodeCache();
    __tilefinchSuppressRemoteLookup(true);
    try {
      document.documentElement = wrap(__tilefinchDocumentElement());
      /* Rebinding an existing runtime is not an authored body replacement.
         Bypass the public setter: it correctly performs replaceChild(), but
         that traversal belongs to the retired document whose remote reader
         has already been detached. */
      document.__tilefinchBodyValue = wrap(__tilefinchBody());
      document.head = document.querySelector("head");
    } finally {
      __tilefinchSuppressRemoteLookup(false);
    }
    globalThis.__tilefinchRebindStableNodes();
    document.__activeElement = document.body;
    selection._range = null;
  };
  {
    const states = new Map(),
      limit = 32,
      dirtyIds = new Set();
    const controlSelector = "input,textarea,select,option";
    const sectionControlValue = (item) => {
      const tag = String(item?.localName || item?.tagName).toLowerCase();
      for (const key of nativeControlKeys(item))
        if (nativeControlValues.has(key)) return nativeControlValues.get(key);
      if (tag === "input") {
        const native = String(
            __tilefinchGetAttribute(item.__handle, "value") ?? "",
          ),
          reflected = String(item.getAttribute("value") ?? "");
        if (native !== reflected) return native;
      } else if (tag === "textarea") {
        const native = String(__tilefinchGetText(item.__handle) ?? ""),
          reflected = String(item.textContent ?? "");
        if (native !== reflected) return native;
      }
      return item.value === undefined || item.value === null ? "" : item.value;
    };
    const controls = () => {
      __tilefinchSuppressRemoteLookup(true);
      try {
        /* Avoid materializing an unbounded public NodeList just to preserve
           section state. Sixty-four controls still covers dense forms while
           bounding wrapper/object retention to a practical PSP footprint.
           The 65th-match probe records that truncation occurred without
           walking or allocating the rest of a hostile form. */
        const count = __tilefinchQueryCount(controlSelector, 65);
        if (count > 64) retentionStats.controlDrops += count - 64;
        /* Do not turn state preservation into the allocation which kills a
           nearly exhausted page. The current section remains rendered and
           usable; only this optional restore snapshot is skipped. */
        if (__tilefinchHeapRemaining() < 512 * 1024) {
          retentionStats.controlDrops += Math.min(count, 64);
          return [];
        }
        const handles = __tilefinchQueryAll(controlSelector, 0, 64);
        return handles.map(__tilefinchWrap);
      } finally {
        __tilefinchSuppressRemoteLookup(false);
      }
    };
    globalThis.__tilefinchDirtyNodeIds = dirtyIds;
    globalThis.__tilefinchRestoringSection = false;
    globalThis.__tilefinchSaveSectionState = (key) => {
      key = Number(key);
      const list = controls(),
        saved = [];
      for (let index = 0; index < list.length; index++) {
        const item = list[index],
          raw = sectionControlValue(item),
          value = String(raw).slice(0, 512),
          selectionStart = Number.isInteger(item.selectionStart)
            ? item.selectionStart
            : null,
          selectionEnd = Number.isInteger(item.selectionEnd)
            ? item.selectionEnd
            : null,
          selectionDirection =
            selectionStart === null
              ? null
              : String(item.selectionDirection || "none"),
          stableKey = String(item.__tilefinchStableKey || "").slice(0, 192);
        saved.push({
          stableKey,
          index,
          value,
          checked: !!item.checked,
          selected: !!item.selected,
          selectionStart,
          selectionEnd,
          selectionDirection,
        });
      }
      const nodes = [];
      for (const stableKey of dirtyIds) {
        if (nodes.length >= 16) {
          retentionStats.dirtyDrops++;
          continue;
        }
        const item = __tilefinchNodeForStableKey(stableKey);
        if (!item) continue;
        const attributes = item.attributes.slice(0, 16).map((attribute) => ({
          name: String(attribute.name).slice(0, 64),
          value: String(attribute.value).slice(0, 256),
        }));
        let html = null;
        try {
          const serialized = String(item.innerHTML);
          if (serialized.length <= 512) html = serialized;
          else retentionStats.dirtyDrops++;
        } catch (_) {
          retentionStats.dirtyDrops++;
        }
        nodes.push({
          stableKey: String(stableKey).slice(0, 192),
          attributes,
          html,
        });
      }
      dirtyIds.clear();
      if (states.has(key)) states.delete(key);
      while (states.size >= limit) {
        states.delete(states.keys().next().value);
        retentionStats.stateEvictions++;
      }
      states.set(key, { controls: saved, nodes });
      nativeControlValues.clear();
      return saved.length + nodes.length;
    };
    globalThis.__tilefinchRestoreSectionState = (key) => {
      const state = states.get(Number(key));
      if (!state) return 0;
      globalThis.__tilefinchRestoringSection = true;
      let restored = 0;
      try {
        for (const saved of state.nodes || []) {
          const item = __tilefinchNodeForStableKey(saved.stableKey);
          if (!item) continue;
          const names = new Set(
            saved.attributes.map((attribute) => attribute.name),
          );
          for (const attribute of [...item.attributes])
            if (!names.has(attribute.name))
              item.removeAttribute(attribute.name);
          for (const attribute of saved.attributes)
            item.setAttribute(attribute.name, attribute.value);
          if (saved.html !== null) item.innerHTML = saved.html;
          restored++;
        }
        const list = controls();
        for (const saved of state.controls || []) {
          const item = saved.stableKey
            ? __tilefinchNodeForStableKey(saved.stableKey)
            : list[saved.index];
          if (!item) continue;
          item.value = saved.value;
          const tag = String(
            item.localName || item.tagName,
          ).toLowerCase();
          if (tag === "input")
            __tilefinchSetAttribute(item.__handle, "value", saved.value);
          else if (tag === "textarea")
            __tilefinchSetText(item.__handle, saved.value);
          if ("checked" in item) item.checked = saved.checked;
          if ("selected" in item) item.selected = saved.selected;
          if (
            saved.selectionStart !== null &&
            typeof item.setSelectionRange === "function"
          )
            item.setSelectionRange(
              saved.selectionStart,
              saved.selectionEnd,
              saved.selectionDirection,
            );
          restored++;
        }
      } finally {
        globalThis.__tilefinchRestoringSection = false;
        dirtyIds.clear();
      }
      return restored;
    };
  }
  document.getElementById = __tilefinchDocumentGetElementById;
  document.getElementsByTagName = (tag) => {
    tag = String(tag);
    if (tag !== "*" && !/^[a-z][\w-]*$/i.test(tag))
      return Array.from(document.querySelectorAll("*")).filter(
        (node) => String(node.localName).toLowerCase() === tag.toLowerCase(),
      );
    return document.querySelectorAll(tag);
  };
  {
    let writeBuffer = "",
      writePending = false;
    const flush = () => {
      writePending = false;
      if (!writeBuffer) return;
      const source = writeBuffer;
      writeBuffer = "";
      const container = document.createElement("div");
      container.innerHTML = source;
      const target = document.body || document.documentElement;
      while (container.firstChild) target.appendChild(container.firstChild);
    };
    document.write = (...parts) => {
      const addition = parts.map(String).join("");
      if (writeBuffer.length + addition.length > 256 * 1024)
        throw new RangeError("document.write buffer exceeds bounded size");
      writeBuffer += addition;
      if (!writePending) {
        writePending = true;
        queueMicrotask(flush);
      }
    };
    document.writeln = (...parts) => document.write(...parts, "\n");
    document.close = () => {
      if (writePending) flush();
    };
  }
  Object.defineProperty(document, "scripts", {
    get() {
      return document.querySelectorAll("script");
    },
  });
  Object.defineProperty(document, "styleSheets", {
    get() {
      return document.querySelectorAll("style,link[rel=stylesheet]");
    },
  });
  document.referrer = "";
  globalThis.NodeFilter = {
    SHOW_ALL: 0xffffffff,
    SHOW_ELEMENT: 1,
    SHOW_TEXT: 4,
    SHOW_COMMENT: 128,
    FILTER_ACCEPT: 1,
    FILTER_REJECT: 2,
    FILTER_SKIP: 3,
  };
  const traversalHandle = (root) =>
      root === document
        ? document.documentElement.__handle
        : root?.__handle || 0,
    traversalNodes = (root, whatToShow) => (
      globalThis.__tilefinchBeginTraversal?.(),
      __tilefinchDescendants(traversalHandle(root), Number(whatToShow)).map(
        (value) => (value instanceof Node ? value : wrap(value)),
      )
    );
  document.createNodeIterator = (root, whatToShow = NodeFilter.SHOW_ALL) => {
    const nodes = traversalNodes(root, whatToShow);
    let index = 0;
    return {
      root,
      whatToShow,
      nextNode() {
        return index < nodes.length ? nodes[index++] : null;
      },
      previousNode() {
        return index > 0 ? nodes[--index] : null;
      },
      detach() {},
    };
  };
  document.createTreeWalker = (root, whatToShow = NodeFilter.SHOW_ALL) => {
    const nodes = traversalNodes(root, whatToShow).filter(
      (node) => node !== root,
    );
    let index = -1;
    return {
      root,
      whatToShow,
      currentNode: root,
      nextNode() {
        if (index + 1 >= nodes.length) return null;
        this.currentNode = nodes[++index];
        return this.currentNode;
      },
      previousNode() {
        if (index <= 0) return null;
        this.currentNode = nodes[--index];
        return this.currentNode;
      },
    };
  };
  globalThis.window = globalThis;
  globalThis.self = globalThis;
  globalThis.top = globalThis;
  globalThis.parent = globalThis;
  globalThis.frames = globalThis;
  globalThis.length = 0;
  Object.defineProperty(globalThis, "opener", {
    configurable: false,
    enumerable: true,
    writable: false,
    value: null,
  });
  /* Tilefinch does not create auxiliary browsing contexts. Explicitly
     returning null keeps links/forms from acquiring an opener relationship
     through a partial window.open implementation. */
  Object.defineProperty(globalThis, "open", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: () => null,
  });
  Object.defineProperty(globalThis, "location", {
    configurable: false,
    enumerable: true,
    get() {
      return location;
    },
    set(value) {
      location.assign(value);
    },
  });
  const diagnosticMobileSafari = !!globalThis.__tilefinchDiagnosticMobileSafari,
    tilefinchInnerWidth = Number(globalThis.__tilefinchViewportWidth) || 480,
    tilefinchInnerHeight = Number(globalThis.__tilefinchViewportHeight) || 272,
    tilefinchDeviceWidth = Number(globalThis.__tilefinchDeviceWidth) || 480,
    tilefinchDeviceHeight = Number(globalThis.__tilefinchDeviceHeight) || 272;
  globalThis.innerWidth = diagnosticMobileSafari ? 390 : tilefinchInnerWidth;
  globalThis.innerHeight = diagnosticMobileSafari ? 844 : tilefinchInnerHeight;
  globalThis.outerWidth = diagnosticMobileSafari ? 390 : tilefinchDeviceWidth;
  globalThis.outerHeight = diagnosticMobileSafari ? 844 : tilefinchDeviceHeight;
  globalThis.devicePixelRatio = diagnosticMobileSafari ? 3 : 1;
  const orientationLandscape = innerWidth >= innerHeight;
  globalThis.screen = {
    width: diagnosticMobileSafari ? innerWidth : tilefinchDeviceWidth,
    height: diagnosticMobileSafari ? innerHeight : tilefinchDeviceHeight,
    availWidth: diagnosticMobileSafari ? innerWidth : tilefinchDeviceWidth,
    availHeight: diagnosticMobileSafari ? innerHeight : tilefinchDeviceHeight,
    availLeft: 0,
    availTop: 0,
    colorDepth: 24,
    pixelDepth: 24,
    orientation: {
      type: orientationLandscape ? "landscape-primary" : "portrait-primary",
      angle: orientationLandscape ? 90 : 0,
      addEventListener() {},
      removeEventListener() {},
    },
  };
  globalThis.scrollX = globalThis.pageXOffset = 0;
  globalThis.scrollY = globalThis.pageYOffset = 0;
  globalThis.__tilefinchFrameEvalTelemetry = "none";
  const normalizePostMessageTarget = (value, senderOrigin) => {
      if (value === undefined || value === "/") return String(senderOrigin);
      const text = String(value);
      if (text === "*") return text;
      let parsed;
      try {
        parsed = new TilefinchURL(text, location.href);
      } catch (_) {
        throw new DOMException("Invalid target origin", "SyntaxError");
      }
      if (!parsed.origin || parsed.origin === "null")
        throw new DOMException("Invalid target origin", "SyntaxError");
      return parsed.origin;
    };
  Object.defineProperty(globalThis, "__tilefinchNormalizeTargetOrigin", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: normalizePostMessageTarget,
  });
  Object.defineProperty(globalThis, "__tilefinchPostParentMessage", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(data, targetOrigin = "/") {
      const normalizedTarget = normalizePostMessageTarget(
        targetOrigin,
        location.origin,
      );
      const json = trustedJSONStringify(data);
      __tilefinchPostMessage(
        0,
        json === undefined ? "null" : json,
        normalizedTarget,
      );
    },
  });
  const frameSandboxPolicy = (element) => {
      const value = element?.getAttribute?.("sandbox");
      if (value === null || value === undefined)
        return { present: false, scripts: true, sameOrigin: true };
      const raw = String(value);
      if (raw.length > 1024)
        return { present: true, scripts: false, sameOrigin: false };
      const text = trustedStringLower(raw);
      let scripts = false,
        sameOrigin = false,
        at = 0;
      const whitespace = (index) => {
        const code = trustedCharCodeAt(text, index);
        return code === 9 || code === 10 || code === 12 || code === 13 || code === 32;
      };
      while (at < text.length) {
        while (at < text.length && whitespace(at)) at++;
        const start = at;
        while (at < text.length && !whitespace(at)) at++;
        const token = trustedStringSlice(text, start, at);
        if (token === "allow-scripts") scripts = true;
        else if (token === "allow-same-origin") sameOrigin = true;
      }
      return {
        present: true,
        scripts,
        sameOrigin,
      };
    },
    /* Compile this trusted scope evaluator while the bootstrap is being
       installed. CSP arms QuickJS's native dynamic-code gate before author
       script begins, so constructing it lazily during iframe setup would
       incorrectly break even script-disabled frames. The native wrapper
       still checks the page's non-writable CSP decision on every call. */
    trustedFrameEvaluator = new Function(
      "scope",
      "source",
      "with(scope){return eval(source)}",
    ),
    frameWindows = new Map(),
    frameWindowLimit = 16,
    evictFrameWindow = () => {
      let candidate = null;
      for (const entry of frameWindows) {
        if (candidate === null) candidate = entry;
        if (!entry[1].active) {
          candidate = entry;
          break;
        }
      }
      if (candidate === null) return;
      candidate[1].active = false;
      candidate[1].scope.document = null;
      candidate[1].scope.location = null;
      frameWindows.delete(candidate[0]);
    };
  globalThis.__tilefinchFrameWindow = (handle) => {
    handle = Number(handle);
    let state = frameWindows.get(handle);
    const element = wrap(handle);
    if (!state) {
      if (frameWindows.size >= frameWindowLimit) evictFrameWindow();
      const scope = {};
      state = {
        active: !!element?.isConnected,
        sameOrigin: true,
        opaqueOrigin: false,
        scriptsAllowed: true,
        managed: false,
        loadGeneration: 0,
        localSource: null,
        localSrcdoc: null,
        localSandboxScripts: false,
        localSandboxSameOrigin: false,
        proxy: null,
        scope,
      };
      scope.document = globalThis.__tilefinchCreateFrameDocument(true);
      scope.location = {
        href: "about:blank",
        protocol: "about:",
        origin: location.origin,
      };
      let proxy;
      proxy = new Proxy(scope, {
        has(target, key) {
          if (key === "source" || key === "eval") return false;
          if (state.sameOrigin) return true;
          return (
            key === "closed" ||
            key === "window" ||
            key === "self" ||
            key === "frames" ||
            key === "postMessage" ||
            key === "parent" ||
            key === "top" ||
            key === "opener" ||
            key === "length"
          );
        },
        get(target, key) {
          if (key === "closed") return !state.active;
          if (key === "document" || key === "location")
            return state.sameOrigin
              ? key in target
                ? target[key]
                : globalThis[key]
              : undefined;
          if (key === "eval")
            return state.sameOrigin && state.scriptsAllowed
              ? target.eval
              : undefined;
          if (!state.sameOrigin) {
            if (key === "window" || key === "self" || key === "frames")
              return proxy;
            if (
              key === "postMessage" ||
              key === "parent" ||
              key === "top" ||
              key === "opener" ||
              key === "length"
            )
              return target[key];
            return undefined;
          }
          if (key in target) return target[key];
          return state.sameOrigin ? globalThis[key] : undefined;
        },
        set(target, key, value) {
          if (!state.sameOrigin) return false;
          target[key] = value;
          return true;
        },
      });
      state.proxy = proxy;
      Object.defineProperty(scope.document, "defaultView", {
        configurable: true,
        value: proxy,
      });
      scope.window = proxy;
      scope.self = proxy;
      scope.globalThis = proxy;
      scope.parent = globalThis;
      scope.top = globalThis;
      Object.defineProperty(scope, "opener", {
        configurable: false,
        enumerable: true,
        writable: false,
        value: null,
      });
      scope.frames = proxy;
      scope.length = 0;
      scope.eval = __tilefinchCreateFrameEval(trustedFrameEvaluator, proxy);
      scope.postMessage = function (data, targetOrigin = "/") {
        const current = wrap(handle);
        if (!state.active || !current?.isConnected) return;
        const normalizedTarget = normalizePostMessageTarget(
          targetOrigin,
          location.origin,
        );
        const json = trustedJSONStringify(data),
          ancestors = [];
        for (
          let at = current;
          at && ancestors.length < 8;
          at = at.parentElement
        )
          ancestors.push(
            String(at.tagName || at.nodeName || "") +
              ":" +
              String(at.__handle || 0),
          );
        globalThis.__tilefinchLastFramePost = {
          handle,
          connected: true,
          ancestors,
          src: String(current.src || ""),
          targetOrigin: normalizedTarget,
        };
        __tilefinchPostMessage(
          handle,
          json === undefined ? "null" : json,
          normalizedTarget,
        );
      };
      frameWindows.set(handle, state);
    } else if (!state.managed) state.active = !!element?.isConnected;
    return state.proxy;
  };
  globalThis.__tilefinchLoadLocalFrame = (element) => {
    if (
      !(element instanceof HTMLIFrameElement) ||
      !element.isConnected
    )
      return;
    const srcdoc = element.getAttribute("srcdoc"),
      source = String(element.src || ""),
      blob = blobURLs.get(source),
      local =
        srcdoc !== null ||
        source === "" ||
        source === "about:blank" ||
        source.startsWith("blob:");
    const proxy = globalThis.__tilefinchFrameWindow(element.__handle),
      state = frameWindows.get(Number(element.__handle));
    if (!local || (source.startsWith("blob:") && !blob)) {
      /* A queued initial about:blank load must not win a race with a
         synchronous navigation assigned before its microtask runs. */
      state.localSource = null;
      state.localSrcdoc = null;
      state.loadGeneration++;
      return;
    }
    const sandboxPolicy = frameSandboxPolicy(element),
      normalizedSrcdoc = srcdoc === null ? null : String(srcdoc);
    if (
      state.localSource === source &&
      state.localSrcdoc === normalizedSrcdoc &&
      state.localSandboxScripts === sandboxPolicy.scripts &&
      state.localSandboxSameOrigin === sandboxPolicy.sameOrigin
    )
      return;
    state.localSource = source;
    state.localSrcdoc = normalizedSrcdoc;
    state.localSandboxScripts = sandboxPolicy.scripts;
    state.localSandboxSameOrigin = sandboxPolicy.sameOrigin;
    state.scriptsAllowed = sandboxPolicy.scripts;
    state.opaqueOrigin = sandboxPolicy.present && !sandboxPolicy.sameOrigin;
    state.sameOrigin = !state.opaqueOrigin;
    const text =
        normalizedSrcdoc !== null ? normalizedSrcdoc
        : blob ? new TextDecoder().decode(blob._bytes) : "",
      standards = /^\s*<!doctype\s+html(?:\s|>)/i.test(text),
      frameDocument = text
        ? new DOMParser().parseFromString(text, "text/html")
        : globalThis.__tilefinchCreateFrameDocument(standards),
      generation = ++state.loadGeneration;
    state.scope.document = frameDocument;
    const frameHref =
        srcdoc !== null ? "about:srcdoc" : source || "about:blank",
      protocolMatch = frameHref.match(/^([A-Za-z][A-Za-z0-9+.-]*:)/);
    state.scope.location = {
      href: frameHref,
      protocol: protocolMatch ? protocolMatch[1].toLowerCase() : "",
      origin: state.opaqueOrigin ? "null" : location.origin,
    };
    frameDocument.location = state.scope.location;
    Object.defineProperty(frameDocument, "defaultView", {
      configurable: true,
      value: proxy,
    });
    queueMicrotask(() => {
      if (element.isConnected && state.loadGeneration === generation)
        element.dispatchEvent(__tilefinchTrustedEvent(new Event("load")));
    });
  };
  globalThis.__tilefinchSetFrameWindowState = (
    handle,
    active,
    sameOrigin,
    opaqueOrigin,
    committedURL = null,
  ) => {
    handle = Number(handle);
    const proxy = globalThis.__tilefinchFrameWindow(handle),
      state = frameWindows.get(handle);
    state.active = !!active;
    state.sameOrigin = !!sameOrigin;
    state.opaqueOrigin = !!opaqueOrigin;
    state.managed = true;
    if (!state.active) {
      state.scope.document = null;
      state.scope.location = null;
      state.localSource = null;
      state.localSrcdoc = null;
      state.loadGeneration++;
      return proxy;
    }
    if (state.active && state.sameOrigin && committedURL !== null) {
      try {
        const parsed = new URL(String(committedURL), location.href);
        state.scope.location = {
          href: parsed.href,
          protocol: parsed.protocol,
          origin: state.opaqueOrigin ? "null" : parsed.origin,
          host: parsed.host,
          hostname: parsed.hostname,
          port: parsed.port,
          pathname: parsed.pathname,
          search: parsed.search,
          hash: parsed.hash,
        };
        if (state.scope.document)
          state.scope.document.location = state.scope.location;
      } catch (_) {}
    }
    return proxy;
  };
  globalThis.__tilefinchReceiveMessage = (json, origin, sourceHandle) =>
    globalThis.__tilefinchRunTask(
      "window-message:source=" + String(sourceHandle),
      () => {
        const event = new Event("message");
        Object.defineProperty(event, "isTrusted", { value: true });
        event.data = trustedJSONParse(String(json));
        event.origin = String(origin);
        event.source =
          Number(sourceHandle) === -1
            ? globalThis
            : Number(sourceHandle) === 0
            ? globalThis.parent
            : __tilefinchFrameWindow(Number(sourceHandle));
        globalThis.dispatchEvent(event);
      },
    );
  globalThis.postMessage = (data, targetOrigin = "/") => {
    const normalizedTarget = normalizePostMessageTarget(
      targetOrigin,
      location.origin,
    );
    if (
      normalizedTarget !== "*" &&
      normalizedTarget !== String(location.origin)
    )
      return;
    const json = trustedJSONStringify(data);
    setTimeout(
      () =>
        __tilefinchReceiveMessage(
          json === undefined ? "null" : json,
          location.origin,
          -1,
        ),
      0,
    );
  };
  location.replace = (value) => {
    const next = new TilefinchURL(value, location.href);
    location._set(next.href);
    __tilefinchRequestNavigation(location.href, true);
  };
  location.reload = () => __tilefinchRequestNavigation(location.href, true);
  let historyState = null,
    historyLength = 1;
  globalThis.history = {
    get state() {
      return historyState;
    },
    get length() {
      return historyLength;
    },
    scrollRestoration: "auto",
    replaceState(state, title, url) {
      if (url !== undefined && url !== null) {
        const next = new TilefinchURL(url, location.href);
        if (
          next.origin !== location.origin ||
          !__tilefinchSetDocumentURL(next.href)
        )
          throw new Error("SecurityError");
        location._set(next.href);
      }
      historyState = state;
    },
    pushState(state, title, url) {
      this.replaceState(state, title, url);
      historyLength++;
    },
    back() {},
    forward() {},
    go() {},
  };
  globalThis.__tilefinchCommitSameDocument = (url, oldURL) => {
    location._set(String(url));
    historyLength++;
    const event = new Event("hashchange");
    event.oldURL = String(oldURL);
    event.newURL = location.href;
    globalThis.dispatchEvent(event);
  };
  globalThis.__tilefinchRestoreSameDocument = (url, oldURL) => {
    location._set(String(url));
    const pop = new Event("popstate");
    pop.state = historyState;
    globalThis.dispatchEvent(pop);
    const event = new Event("hashchange");
    event.oldURL = String(oldURL);
    event.newURL = location.href;
    globalThis.dispatchEvent(event);
  };
  const windowListeners = new Map();
  globalThis.addEventListener = (type, callback, options = false) =>
    globalThis.__tilefinchAddEventListener(
      windowListeners,
      type,
      callback,
      options,
    );
  globalThis.removeEventListener = (type, callback, options = false) =>
    globalThis.__tilefinchRemoveEventListener(
      windowListeners,
      type,
      callback,
      options,
    );
  globalThis.__tilefinchInvokeWindowEvent = (event, capture) => {
    event.currentTarget = globalThis;
    event.eventPhase = globalThis === event.target ? 2 : capture ? 1 : 3;
    if (!capture) {
      const handler = globalThis["on" + event.type];
      if (typeof handler === "function")
        try {
          globalThis.__tilefinchRunTask(
            "window-handler:" + String(event.type),
            handler,
            globalThis,
            [event],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "window event " + event.type);
        }
    }
    globalThis.__tilefinchInvokeListenerList(
      windowListeners,
      globalThis,
      event,
      capture,
    );
    globalThis.__tilefinchInvokeEventTarget(
      globalThis,
      event,
      capture,
      event.eventPhase,
    );
  };
  globalThis.dispatchEvent = (event) => {
    const value = event,
      path = [globalThis];
    globalThis.__tilefinchPrepareEvent(value, globalThis, path);
    if (!value.__stopped) {
      globalThis.__tilefinchInvokeWindowEvent(value, true);
      if (!value.__immediateStopped)
        globalThis.__tilefinchInvokeWindowEvent(value, false);
    }
    value.currentTarget = null;
    value.eventPhase = 0;
    value.__dispatching = false;
    __tilefinchRecordEvent();
    return !value.defaultPrevented;
  };
  globalThis.navigator = diagnosticMobileSafari
    ? {
        userAgent:
          "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1",
        language: "en-US",
        languages: ["en-US"],
        platform: "iPhone",
        maxTouchPoints: 5,
        cookieEnabled: true,
        onLine: true,
        hardwareConcurrency: 6,
      }
    : {
        userAgent: String(globalThis.__tilefinchBrowserUserAgent),
        language: "en-US",
        languages: ["en-US"],
        platform: "PSP",
        maxTouchPoints: 0,
        cookieEnabled: true,
        onLine: true,
        hardwareConcurrency: 1,
        deviceMemory: 0.25,
        userAgentData: {
          brands: [
            { brand: "Tilefinch", version: "0.1" },
            { brand: "Not.A/Brand", version: "99" },
          ],
          mobile: true,
          platform: "PlayStation Portable",
          getHighEntropyValues(hints) {
            const all = {
              architecture: "MIPS",
              bitness: "32",
              model: "PSP-3000",
              platform: "PlayStation Portable",
              platformVersion: "6.61",
              uaFullVersion: "0.1.2",
              fullVersionList: [
                { brand: "Tilefinch", version: "0.1.2" },
                { brand: "Not.A/Brand", version: "99.0.0.0" },
              ],
            };
            const value = {
              brands: this.brands,
              mobile: this.mobile,
              platform: this.platform,
            };
            for (const hint of hints || [])
              if (hint in all) value[hint] = all[hint];
            return Promise.resolve(value);
          },
          toJSON() {
            return {
              brands: this.brands,
              mobile: this.mobile,
              platform: this.platform,
            };
          },
        },
      };
  navigator.clipboard = {
    writeText(value) {
      try {
        globalThis.__tilefinchClipboardWrite(value);
        return Promise.resolve();
      } catch (error) {
        return Promise.reject(error);
      }
    },
    readText() {
      return Promise.resolve(globalThis.__tilefinchClipboardStats.text);
    },
  };
  {
    const invalidBase64 = (detail) => {
      detail = String(detail);
      globalThis.__tilefinchBase64Error = detail;
      const message = "The string is not correctly encoded. [" + detail + "]",
        error = globalThis.DOMException
          ? new DOMException(message, "InvalidCharacterError")
          : new TypeError("InvalidCharacterError: " + message);
      throw error;
    };
    globalThis.btoa = (input) => {
      const text = String(input);
      if (text.length > 256 * 1024)
        throw new RangeError("base64 input exceeds bounded size");
      const bytes = new Uint8Array(text.length);
      for (let at = 0; at < text.length; at++) {
        const value = text.charCodeAt(at);
        if (value > 255)
          invalidBase64(
            "btoa length=" + text.length + " index=" + at + " code=" + value,
          );
        bytes[at] = value;
      }
      return __tilefinchBase64EncodeBytes(bytes);
    };
    globalThis.atob = (input) => {
      let text = String(input).replace(/[\t\n\f\r ]/g, "");
      if (text.length > 768 * 1024)
        throw new RangeError("base64 input exceeds bounded size");
      if (text.length % 4 === 0) text = text.replace(/==?$/, "");
      const remainder = text.length % 4,
        bad = text.search(/[^A-Za-z0-9+/]/);
      if (remainder === 1 || bad >= 0)
        invalidBase64(
          "atob length=" +
            text.length +
            " remainder=" +
            remainder +
            " bad-index=" +
            bad +
            " bad-code=" +
            (bad < 0 ? -1 : text.charCodeAt(bad)),
        );
      return __tilefinchBase64DecodeString(text);
    };
  }
  {
    const decode = globalThis.atob,
      history = [];
    let calls = 0;
    globalThis.atob = (input) => {
      const text = String(input),
        codes = [];
      for (let i = 0; i < Math.min(12, text.length); i++)
        codes.push(text.charCodeAt(i));
      const call = ++calls,
        brief =
          call +
          ":" +
          text.length +
          ":" +
          (text.length ? text.charCodeAt(0) : -1),
        detail =
          "call=" +
          call +
          " type=" +
          typeof input +
          " tag=" +
          Object.prototype.toString.call(input) +
          " length=" +
          text.length +
          " prefix=" +
          codes.join(",");
      try {
        const output = decode(input),
          out = [];
        for (let i = 0; i < Math.min(4, output.length); i++)
          out.push(output.charCodeAt(i));
        history.push(brief + ">" + output.length + ":" + out.join(","));
        if (history.length > 8) history.shift();
        return output;
      } catch (error) {
        const diagnostic = detail + " prior=" + history.join("|");
        globalThis.__tilefinchBase64Error =
          (
            String(globalThis.__tilefinchBase64Error || "").slice(-1024) +
            " " +
            diagnostic
          ).slice(-2048);
        if (error && typeof error === "object")
          error.message =
            String(error.message || error) + " [" + diagnostic + "]";
        throw error;
      }
    };
  }
  globalThis.DOMRect = class DOMRect {
    constructor(x = 0, y = 0, width = 0, height = 0) {
      this.x = Number(x);
      this.y = Number(y);
      this.width = Number(width);
      this.height = Number(height);
      this.left = this.x;
      this.top = this.y;
      this.right = this.x + this.width;
      this.bottom = this.y + this.height;
    }
    toJSON() {
      return {
        x: this.x,
        y: this.y,
        width: this.width,
        height: this.height,
        top: this.top,
        right: this.right,
        bottom: this.bottom,
        left: this.left,
      };
    }
  };
  const sparseComputedProperties = new Set([
    "cursor",
    "overscroll-behavior",
    "overscroll-behavior-x",
    "overscroll-behavior-y",
    "overscroll-behavior-inline",
    "overscroll-behavior-block",
    "scroll-behavior",
    "scroll-margin",
    "scroll-margin-top",
    "scroll-margin-right",
    "scroll-margin-bottom",
    "scroll-margin-left",
    "scroll-padding",
    "scroll-padding-top",
    "scroll-padding-right",
    "scroll-padding-bottom",
    "scroll-padding-left",
    "scroll-snap-align",
    "scroll-snap-stop",
    "scroll-snap-type",
    "scrollbar-color",
    "scrollbar-width",
    "user-select",
    "-webkit-user-select",
    "touch-action",
    "text-size-adjust",
    "-webkit-text-size-adjust",
    "resize",
    "text-wrap",
    "text-wrap-style",
    "translate",
    "rotate",
    "scale",
    "isolation",
    "flex-basis",
    "content-visibility",
    "-webkit-line-clamp",
    "border-start-start-radius",
    "border-start-end-radius",
    "border-end-start-radius",
    "border-end-end-radius",
  ]),
    computedSparseValue = (node, name, value) => {
      if (name === "flex-basis") {
        const text = String(value).trim(),
          fontSize =
            parseFloat(
              __tilefinchComputedStyleGet(node.__handle, "font-size", ""),
            ) || 16;
        let match = text.match(
          /^calc\(\s*(-?(?:\d+(?:\.\d*)?|\.\d+))px\s*([+-])\s*((?:\d+(?:\.\d*)?|\.\d+))em\s*\)$/i,
        );
        if (match) {
          const result = Math.max(
            0,
            Number(match[1]) +
              (match[2] === "-" ? -1 : 1) * Number(match[3]) * fontSize,
          );
          return String(Math.round(result * 1000) / 1000) + "px";
        }
        match = text.match(
          /^calc\(\s*(-?(?:\d+(?:\.\d*)?|\.\d+))%\s*\)$/i,
        );
        if (match) return String(Number(match[1])) + "%";
        match = text.match(
          /^calc\(\s*(-?(?:\d+(?:\.\d*)?|\.\d+))%\s*\+\s*0px\s*\)$/i,
        );
        if (match) return String(Number(match[1])) + "%";
        return value;
      }
      if (
        !name.startsWith("scroll-margin") &&
        !name.startsWith("scroll-padding")
      )
        return value;
      const fontSize =
          parseFloat(
            __tilefinchComputedStyleGet(node.__handle, "font-size", ""),
          ) || 16,
        padding = name.startsWith("scroll-padding");
      return String(value).replace(
        /calc\(\s*(-?(?:\d+(?:\.\d*)?|\.\d+))px\s*([+-])\s*((?:\d+(?:\.\d*)?|\.\d+))em\s*\)/gi,
        (_match, pixels, operator, ems) => {
          let result =
            Number(pixels) +
            (operator === "-" ? -1 : 1) * Number(ems) * fontSize;
          if (padding && result < 0) result = 0;
          return String(Math.round(result * 1000) / 1000) + "px";
        },
      );
    };
  globalThis.getComputedStyle = (node, pseudo = null) => {
    if (!node || !node.style) return {};
    const defaults = {
      display: "block",
      visibility: "visible",
      opacity: "1",
      contentVisibility: "visible",
      position: "static",
      transform: "none",
      perspective: "none",
      filter: "none",
      contain: "none",
      overflow: "visible",
    };
    return new Proxy(
      {
        getPropertyValue(name) {
          name = __tilefinchCssName(name);
          const inline = node.style.getPropertyValue(name),
            computed = __tilefinchComputedStyleGet(
              node.__handle,
              name,
              pseudo === null ? "" : String(pseudo),
            );
          // The host resolves custom properties across inline declarations,
          // the author cascade, inheritance, and var() substitution.  An
          // empty result is meaningful here: it is also the computed
          // serialization of a guaranteed-invalid value, so do not revive
          // an unresolved token stream through the ordinary inline fallback.
          if (name.startsWith("--")) return computed;
          return computedSparseValue(node, name, (
            computed ||
            inline ||
            defaults[name] ||
            ""
          ));
        },
      },
      {
        get(target, name) {
          return name in target ? target[name] : target.getPropertyValue(name);
        },
        has(target, name) {
          return (
            name in target ||
            (typeof name === "string" &&
              sparseComputedProperties.has(__tilefinchCssName(name)))
          );
        },
      },
    );
  };
  {
    const mediaLength = (value) => {
        const match = String(value)
          .trim()
          .match(/^(-?[0-9.]+)(px|em|rem)?$/);
        if (!match) return NaN;
        return (
          Number(match[1]) * (match[2] === "em" || match[2] === "rem" ? 16 : 1)
        );
      },
      canonicalCalc = (text) =>
        text
          .replace(
            /calc\(\s*(-?[0-9.]+)x\s*([+*\/-])\s*(-?[0-9.]+)(x)?\s*\)/gi,
            (all, left, operator, right, rightUnit) => {
              left = Number(left);
              right = Number(right);
              if ((operator === "+" || operator === "-") && !rightUnit)
                return all;
              const result =
                operator === "+"
                  ? left + right
                  : operator === "-"
                    ? left - right
                    : operator === "*"
                      ? left * right
                      : left / right;
              return Number.isFinite(result)
                ? "calc(" + String(result) + "dppx)"
                : all;
            },
          )
          .replace(
            /calc\(\s*(-?[0-9.]+)x\s*\)/gi,
            (_, value) => "calc(" + String(Number(value)) + "dppx)",
          ),
      canonicalMediaQuery = (query) => {
        const original = String(query);
        if (original.length > 4096) return "not all";
        if (original.trim() === "") return "";
        return original
          .split(",")
          .slice(0, 32)
          .map((part) => {
            let text = part.trim();
            if (!text) return "not all";
            const opens = (text.match(/\(/g) || []).length,
              closes = (text.match(/\)/g) || []).length;
            if (opens !== closes) return "not all";
            text = canonicalCalc(text);
            const simple = text.match(/^\(\s*([\w-]+)\s*\)$/);
            if (simple) return "(" + simple[1].toLowerCase() + ")";
            return text
              .toLowerCase()
              .replace(/\s+/g, " ")
              .replace(/\(\s*/g, "(")
              .replace(/\s*\)/g, ")")
              .replace(/\s*:\s*/g, ": ");
          })
          .join(", ");
      };
    const compareMedia = (left, operator, right) =>
        operator === "<"
          ? left < right
          : operator === "<="
            ? left <= right
            : operator === ">"
              ? left > right
              : operator === ">="
                ? left >= right
                : left === right,
      mediaRatio = (value) => {
        const parts = String(value).trim().split("/");
        if (parts.length === 1) return Number(parts[0]);
        const numerator = Number(parts[0]),
          denominator = Number(parts[1]);
        return denominator ? numerator / denominator : NaN;
      },
      mediaResolution = (value) => {
        const match = String(value)
          .trim()
          .match(/^(-?[0-9.]+)(dppx|dpi|dpcm)$/);
        if (!match) return NaN;
        const number = Number(match[1]);
        return match[2] === "dpi"
          ? number / 96
          : match[2] === "dpcm"
            ? number / 37.8
            : number;
      },
      mediaDimension = (name) =>
        name === "width" || name === "device-width"
          ? innerWidth
          : name === "height" || name === "device-height"
            ? innerHeight
            : NaN,
      mediaOperand = (text) => {
        text = String(text).trim();
        const dimension = mediaDimension(text);
        return Number.isFinite(dimension) ? dimension : mediaLength(text);
      },
      rangeMatches = (text) => {
        const match = String(text)
          .trim()
          .match(/^(.+?)\s*(<=|>=|<|>|=)\s*(.+?)(?:\s*(<=|>=|<|>)\s*(.+))?$/);
        if (!match) return null;
        const left = mediaOperand(match[1]),
          middle = mediaOperand(match[3]);
        if (!Number.isFinite(left) || !Number.isFinite(middle)) return false;
        let result = compareMedia(left, match[2], middle);
        if (match[4]) {
          const right = mediaOperand(match[5]);
          result =
            result &&
            Number.isFinite(right) &&
            compareMedia(middle, match[4], right);
        }
        return result;
      },
      featureMatches = (source) => {
        const text = String(source).trim().toLowerCase(),
          ranged = rangeMatches(text);
        if (ranged !== null) return ranged;
        const at = text.indexOf(":"),
          name = (at < 0 ? text : text.slice(0, at)).trim(),
          value = (at < 0 ? "" : text.slice(at + 1)).trim();
        if (
          name === "min-width" ||
          name === "max-width" ||
          name === "width" ||
          name === "min-height" ||
          name === "max-height" ||
          name === "height" ||
          name === "min-device-width" ||
          name === "max-device-width" ||
          name === "device-width" ||
          name === "min-device-height" ||
          name === "max-device-height" ||
          name === "device-height"
        ) {
          const prefix = name.startsWith("min-")
              ? "min"
              : name.startsWith("max-")
                ? "max"
                : "exact",
            base = name.replace(/^(min|max)-/, ""),
            actual = mediaDimension(base),
            numeric = mediaLength(value);
          return Number.isFinite(numeric) &&
            (prefix === "min"
              ? actual >= numeric
              : prefix === "max"
                ? actual <= numeric
                : actual === numeric);
        }
        if (name === "orientation")
          return value === (innerWidth >= innerHeight ? "landscape" : "portrait");
        if (name === "aspect-ratio" || name === "device-aspect-ratio")
          return mediaRatio(value) === innerWidth / innerHeight;
        if (name === "min-aspect-ratio" || name === "max-aspect-ratio") {
          const wanted = mediaRatio(value),
            actual = innerWidth / innerHeight;
          return Number.isFinite(wanted) &&
            (name.startsWith("min-") ? actual >= wanted : actual <= wanted);
        }
        if (
          name === "resolution" ||
          name === "min-resolution" ||
          name === "max-resolution"
        ) {
          const wanted = mediaResolution(value),
            actual = Number(devicePixelRatio) || 1;
          return Number.isFinite(wanted) &&
            (name.startsWith("min-")
              ? actual >= wanted
              : name.startsWith("max-")
                ? actual <= wanted
                : actual === wanted);
        }
        if (name === "hover" || name === "any-hover")
          return value === "none" || (value === "" && false);
        if (name === "pointer" || name === "any-pointer")
          return value === "coarse" || value === "";
        if (name === "prefers-color-scheme") return value === "light";
        if (name === "prefers-reduced-motion")
          return value === "no-preference";
        if (name === "prefers-contrast") return value === "no-preference";
        if (name === "color") return value === "" || Number(value) <= 8;
        if (name === "monochrome") return value === "0";
        if (name === "color-gamut") return value === "srgb";
        return false;
      },
      clauseMatches = (source) => {
        let text = String(source).trim().toLowerCase(),
          negated = false,
          typeMatches = true;
        if (text.startsWith("only ")) text = text.slice(5).trim();
        if (text.startsWith("not ")) {
          negated = true;
          text = text.slice(4).trim();
        }
        if (/^print(?:\s|$)/.test(text)) {
          typeMatches = false;
          text = text.replace(/^print(?:\s+and\s+)?/, "");
        } else if (/^screen(?:\s|$)/.test(text)) {
          text = text.replace(/^screen(?:\s+and\s+)?/, "");
        } else if (/^all(?:\s|$)/.test(text)) {
          text = text.replace(/^all(?:\s+and\s+)?/, "");
        }
        let result = typeMatches,
          seen = false;
        const pattern = /\(([^()]+)\)/g;
        for (let match; (match = pattern.exec(text)); ) {
          seen = true;
          result = result && featureMatches(match[1]);
        }
        if (!seen && text !== "") result = false;
        return negated ? !result : result;
      };
    class MediaQueryListEvent {
      constructor(type, init = {}) {
        if (
          globalThis.Event &&
          Object.getPrototypeOf(MediaQueryListEvent.prototype) !==
            globalThis.Event.prototype
        )
          Object.setPrototypeOf(
            MediaQueryListEvent.prototype,
            globalThis.Event.prototype,
          );
        this.type = String(type);
        this.defaultPrevented = false;
        this.media = String(init.media ?? "");
        this.matches = !!init.matches;
      }
      preventDefault() {
        this.defaultPrevented = true;
      }
    }
    const liveMediaQueries = new Set(),
      registerMediaQuery = (query) => {
        if (query._registered) return;
        if (liveMediaQueries.size < 128) {
          liveMediaQueries.add(query);
          query._registered = true;
        } else retentionStats.observerDrops++;
      },
      unregisterMediaQuery = (query) => {
        if (query._listeners.size || query._onchange) return;
        liveMediaQueries.delete(query);
        query._registered = false;
      };
    class MediaQueryList {
      constructor(query) {
        if (
          globalThis.EventTarget &&
          Object.getPrototypeOf(MediaQueryList.prototype) !==
            globalThis.EventTarget.prototype
        )
          Object.setPrototypeOf(
            MediaQueryList.prototype,
            globalThis.EventTarget.prototype,
          );
        this.media = canonicalMediaQuery(query);
        this._listeners = new Set();
        this._onchange = null;
        this._registered = false;
        this._lastMatch = this.matches;
      }
      get matches() {
        return this.media.split(",").some(clauseMatches);
      }
      get onchange() {
        return this._onchange;
      }
      set onchange(callback) {
        this._onchange = typeof callback === "function" ? callback : null;
        if (this._onchange) registerMediaQuery(this);
        else unregisterMediaQuery(this);
      }
      addListener(callback) {
        this.addEventListener("change", callback);
      }
      removeListener(callback) {
        this.removeEventListener("change", callback);
      }
      addEventListener(type, callback) {
        if (
          String(type) !== "change" ||
          (typeof callback !== "function" &&
            typeof callback?.handleEvent !== "function")
        )
          return;
        if (this._listeners.size >= 64 && !this._listeners.has(callback)) {
          retentionStats.observerDrops++;
          return;
        }
        this._listeners.add(callback);
        registerMediaQuery(this);
      }
      removeEventListener(type, callback) {
        if (String(type) === "change") this._listeners.delete(callback);
        unregisterMediaQuery(this);
      }
      dispatchEvent(event) {
        if (String(event?.type) !== "change") return true;
        for (const callback of this._listeners)
          try {
            if (typeof callback === "function")
              globalThis.__tilefinchRunTask(
                "media-query-change",
                callback,
                this,
                [event],
              );
            else
              globalThis.__tilefinchRunTask(
                "media-query-change-object",
                callback.handleEvent,
                callback,
                [event],
              );
          } catch (error) {
            __tilefinchReportUncaught(error, "matchMedia change");
          }
        if (this._onchange)
          try {
            globalThis.__tilefinchRunTask(
              "media-query-onchange",
              this._onchange,
              this,
              [event],
            );
          } catch (error) {
            __tilefinchReportUncaught(error, "matchMedia onchange");
          }
        return !event.defaultPrevented;
      }
    }
    globalThis.__tilefinchMediaRecheck = () => {
      for (const query of liveMediaQueries) {
        const matches = query.matches;
        if (matches === query._lastMatch) continue;
        query._lastMatch = matches;
        query.dispatchEvent(
          new MediaQueryListEvent("change", {
            media: query.media,
            matches,
          }),
        );
      }
    };
    globalThis.MediaQueryList = MediaQueryList;
    globalThis.MediaQueryListEvent = MediaQueryListEvent;
    globalThis.matchMedia = (query) => new MediaQueryList(query);
  }
  class PerformanceEntry {
    constructor(name, type, startTime = 0, duration = 0) {
      this.name = String(name);
      this.entryType = String(type);
      this.startTime = Number(startTime);
      this.duration = Number(duration);
    }
    toJSON() {
      return {
        name: this.name,
        entryType: this.entryType,
        startTime: this.startTime,
        duration: this.duration,
      };
    }
  }
  class PerformanceResourceTiming extends PerformanceEntry {
    constructor(name, initiatorType = "other") {
      super(name, "resource", 0, 0);
      this.initiatorType = String(initiatorType);
      this.nextHopProtocol = "h2";
      this.transferSize = 0;
      this.encodedBodySize = 0;
      this.decodedBodySize = 0;
    }
  }
  class PerformanceNavigationTiming extends PerformanceResourceTiming {
    constructor(name) {
      super(name, "navigation");
      this.entryType = "navigation";
      this.type = "navigate";
      this.redirectCount = 0;
      this.domInteractive = 0;
      this.domContentLoadedEventStart = 0;
      this.domContentLoadedEventEnd = 0;
      this.loadEventStart = 0;
      this.loadEventEnd = 0;
    }
  }
  Object.assign(globalThis, {
    PerformanceEntry,
    PerformanceResourceTiming,
    PerformanceNavigationTiming,
  });
  if (globalThis.__tilefinchDeterministicDateFacade !== Date)
    Date.now = () => __tilefinchDateNow(0);
  const performanceEntries = [new PerformanceNavigationTiming(location.href)],
    appendPerformanceEntry = (entry) => {
      if (performanceEntries.length >= 128) {
        const at = performanceEntries.findIndex(
          (value) => value.entryType !== "navigation",
        );
        if (at < 0) return entry;
        performanceEntries.splice(at, 1);
      }
      performanceEntries.push(entry);
      return entry;
    },
    performanceMarkTime = (name) => {
      name = String(name);
      for (let at = performanceEntries.length - 1; at >= 0; at--) {
        const entry = performanceEntries[at];
        if (entry.entryType === "mark" && entry.name === name)
          return entry.startTime;
      }
      throw new DOMException(
        "The mark " + name + " does not exist",
        "SyntaxError",
      );
    },
    performanceTimestamp = (value) =>
      typeof value === "string" ? performanceMarkTime(value) : Number(value);
  globalThis.__tilefinchRecordResourceTiming = (name, initiatorType) =>
    appendPerformanceEntry(
      new PerformanceResourceTiming(
        String(name),
        String(initiatorType || "other"),
      ),
    );
  const performanceTimeOrigin = Date.now();
  globalThis.performance = {
    timeOrigin: performanceTimeOrigin,
    /* Deprecated, but still read by bootstrap/telemetry code on major sites.
       Keep the bounded navigation-zero surface rather than forcing those
       scripts down exception paths before their actual UI initialization. */
    timing: Object.freeze({
      navigationStart: performanceTimeOrigin,
      fetchStart: performanceTimeOrigin,
      domainLookupStart: performanceTimeOrigin,
      domainLookupEnd: performanceTimeOrigin,
      connectStart: performanceTimeOrigin,
      connectEnd: performanceTimeOrigin,
      requestStart: performanceTimeOrigin,
      responseStart: performanceTimeOrigin,
      responseEnd: performanceTimeOrigin,
      domLoading: performanceTimeOrigin,
      domInteractive: performanceTimeOrigin,
      domContentLoadedEventStart: performanceTimeOrigin,
      domContentLoadedEventEnd: performanceTimeOrigin,
      domComplete: performanceTimeOrigin,
      loadEventStart: performanceTimeOrigin,
      loadEventEnd: performanceTimeOrigin,
      redirectStart: 0,
      redirectEnd: 0,
      unloadEventStart: 0,
      unloadEventEnd: 0,
    }),
    navigation: Object.freeze({ type: 0, redirectCount: 0 }),
    now: () => __tilefinchPerformanceNow(3),
    mark(name, options = {}) {
      const start =
        options.startTime === undefined
          ? __tilefinchPerformanceNow(4)
          : Number(options.startTime);
      if (!Number.isFinite(start) || start < 0)
        throw new TypeError("startTime must be a finite nonnegative number");
      const entry = new PerformanceEntry(String(name), "mark", start, 0);
      entry.detail = options.detail;
      return appendPerformanceEntry(entry);
    },
    measure(name, startOrOptions, endMark) {
      let start = 0,
        end,
        detail;
      if (typeof startOrOptions === "object" && startOrOptions !== null) {
        const hasStart = startOrOptions.start !== undefined,
          hasEnd = startOrOptions.end !== undefined,
          hasDuration = startOrOptions.duration !== undefined;
        if ((hasStart && hasEnd && hasDuration) || (!hasStart && !hasEnd))
          throw new TypeError("Invalid measure options");
        start = hasStart ? performanceTimestamp(startOrOptions.start) : NaN;
        end = hasEnd ? performanceTimestamp(startOrOptions.end) : NaN;
        const duration = hasDuration ? Number(startOrOptions.duration) : NaN;
        if (hasDuration && !Number.isFinite(duration))
          throw new TypeError("duration must be finite");
        if (!hasStart) start = end - duration;
        else if (!hasEnd)
          end = hasDuration ? start + duration : __tilefinchPerformanceNow(5);
        detail = startOrOptions.detail;
      } else {
        if (startOrOptions !== undefined)
          start = performanceTimestamp(startOrOptions);
        end =
          endMark !== undefined
            ? performanceTimestamp(endMark)
            : __tilefinchPerformanceNow(5);
      }
      if (!Number.isFinite(start) || !Number.isFinite(end))
        throw new TypeError("measure timestamps must be finite");
      const entry = new PerformanceEntry(
        String(name),
        "measure",
        start,
        Math.max(0, end - start),
      );
      entry.detail = detail;
      return appendPerformanceEntry(entry);
    },
    getEntries() {
      return performanceEntries.slice();
    },
    getEntriesByType(type) {
      return performanceEntries.filter(
        (entry) => entry.entryType === String(type),
      );
    },
    getEntriesByName(name, type) {
      return performanceEntries.filter(
        (entry) =>
          entry.name === String(name) &&
          (type === undefined || entry.entryType === String(type)),
      );
    },
    clearMarks(name) {
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (
          performanceEntries[i].entryType === "mark" &&
          (name === undefined || performanceEntries[i].name === String(name))
        )
          performanceEntries.splice(i, 1);
    },
    clearMeasures(name) {
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (
          performanceEntries[i].entryType === "measure" &&
          (name === undefined || performanceEntries[i].name === String(name))
        )
          performanceEntries.splice(i, 1);
    },
    clearResourceTimings() {
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (performanceEntries[i].entryType === "resource")
          performanceEntries.splice(i, 1);
    },
    setResourceTimingBufferSize() {},
  };
  globalThis.PerformanceObserver = class {
    static supportedEntryTypes = [
      "mark",
      "measure",
      "resource",
      "navigation",
      "paint",
    ];
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
  };
  globalThis.r = {
    config: {},
    setup(value) {
      Object.assign(this.config, value);
    },
  };
  globalThis.mw = {
    config: {
      set() {},
      get() {
        return undefined;
      },
    },
    loader: { state() {}, implement() {}, load() {} },
  };
  globalThis.__tilefinchDispatchDOMContentLoaded = () => {
    if (document.readyState === "complete") return;
    document.readyState = "interactive";
    document.dispatchEvent(new Event("DOMContentLoaded", { bubbles: true }));
    globalThis.__tilefinchMaybeStartMotion?.();
    document.readyState = "complete";
    const loadEvent = new Event("load");
    const bodyLoad = document.body?.getAttribute?.("onload");
    if (
      globalThis.__tilefinchCspAllowsInlineEventHandlers !== false &&
      typeof bodyLoad === "string" &&
      bodyLoad.length <= 65536
    )
      try {
        TrustedFunction("event", bodyLoad).call(globalThis, loadEvent);
      } catch (error) {
        __tilefinchReportUncaught(error, "body onload");
      }
    /*
     * HTMLBodyElement's onload handler reflects the Window load handler
     * surface.  Programmatic `document.body.onload = ...` assignments are
     * stored on the wrapped body, so invoke that property before dispatching
     * the Window event.  Markup handlers are evaluated by the branch above.
     */
    const bodyOnload = document.body?.onload;
    if (typeof bodyOnload === "function")
      try {
        bodyOnload.call(globalThis, loadEvent);
      } catch (error) {
        __tilefinchReportUncaught(error, "body onload");
      }
    globalThis.dispatchEvent(loadEvent);
  };
  Object.defineProperty(globalThis.__tilefinchRootCensus, "frameWindows", {
    get: () => frameWindows.size,
  });
})();
