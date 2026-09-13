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
  const trustedString = String,
    trustedStringFromCodePoint = Function.call.bind(String.fromCodePoint),
    TrustedUint8Array = Uint8Array;
  const trustedMapGet = Function.call.bind(Map.prototype.get),
    trustedMapSet = Function.call.bind(Map.prototype.set),
    trustedMapDelete = Function.call.bind(Map.prototype.delete),
    trustedMapEntries = Function.call.bind(Map.prototype.entries),
    trustedSetValues = Function.call.bind(Set.prototype.values),
    trustedSetAdd = Function.call.bind(Set.prototype.add),
    trustedDateTime = Function.call.bind(Date.prototype.getTime),
    trustedRegExpSource = Function.call.bind(
      Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get,
    ),
    trustedRegExpFlags = Function.call.bind(
      Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").get,
    ),
    trustedArrayBufferByteLength = Function.call.bind(
      Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get,
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
    trustedTypedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype),
    trustedTypedArrayName = Function.call.bind(
      Object.getOwnPropertyDescriptor(
        trustedTypedArrayPrototype, Symbol.toStringTag,
      ).get,
    ),
    trustedTypedArrayBuffer = Function.call.bind(
      Object.getOwnPropertyDescriptor(trustedTypedArrayPrototype, "buffer").get,
    ),
    trustedTypedArrayByteOffset = Function.call.bind(
      Object.getOwnPropertyDescriptor(
        trustedTypedArrayPrototype, "byteOffset",
      ).get,
    ),
    trustedTypedArrayLength = Function.call.bind(
      Object.getOwnPropertyDescriptor(trustedTypedArrayPrototype, "length").get,
    ),
    trustedBooleanValue = Function.call.bind(Boolean.prototype.valueOf),
    trustedNumberValue = Function.call.bind(Number.prototype.valueOf),
    trustedStringValue = Function.call.bind(String.prototype.valueOf),
    trustedMapSize = Function.call.bind(
      Object.getOwnPropertyDescriptor(Map.prototype, "size").get,
    ),
    trustedSetSize = Function.call.bind(
      Object.getOwnPropertyDescriptor(Set.prototype, "size").get,
    ),
    trustedWeakMapGet = Function.call.bind(WeakMap.prototype.get),
    trustedWeakMapSet = Function.call.bind(WeakMap.prototype.set),
    trustedUint8ArraySet = Function.call.bind(Uint8Array.prototype.set),
    trustedObjectKeys = Object.keys,
    trustedObjectPrototype = Object.getPrototypeOf,
    trustedObjectDescriptor = Object.getOwnPropertyDescriptor,
    trustedDefineProperty = Object.defineProperty,
    trustedObjectTag = Function.call.bind(Object.prototype.toString),
    trustedObjectIs = Object.is,
    trustedArrayIsArray = Array.isArray,
    trustedArrayFrom = Array.from,
    trustedArrayBufferIsView = ArrayBuffer.isView,
    trustedNumberIsNaN = Number.isNaN,
    trustedNumberIsInteger = Number.isInteger,
    trustedStringSplit = Function.call.bind(String.prototype.split),
    trustedStringIndexOf = Function.call.bind(String.prototype.indexOf),
    trustedArrayJoin = Function.call.bind(Array.prototype.join),
    trustedFunctionApply = Function.call.bind(Function.prototype.apply);
  const trustedStringLower = Function.call.bind(String.prototype.toLowerCase);
  const trustedStringSlice = Function.call.bind(String.prototype.slice);
  const trustedCharCodeAt = Function.call.bind(String.prototype.charCodeAt);
  const trustedPromiseResolve = Function.call.bind(Promise.resolve, Promise);
  const trustedPromiseThen = Function.call.bind(Promise.prototype.then);
  if (globalThis.queueMicrotask === undefined)
    globalThis.queueMicrotask = (callback) => {
      if (typeof callback !== "function")
        throw new TypeError("callback required");
      trustedPromiseThen(trustedPromiseResolve(), () => {
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
  delete globalThis.__tilefinchQueueCheckpointContinuation;
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
    const selectorPseudoClasses = new Set([
        "active", "any-link", "autofill", "blank", "buffering", "checked",
        "current", "default", "defined", "dir", "disabled", "empty",
        "enabled", "first", "first-child", "first-of-type", "focus",
        "focus-visible", "focus-within", "fullscreen", "future", "has",
        "heading", "host", "host-context", "hover", "indeterminate",
        "in-range", "invalid", "is", "lang", "last-child", "last-of-type",
        "left", "link", "local-link", "modal", "muted", "not",
        "nth-child", "nth-col", "nth-last-child", "nth-last-col",
        "nth-last-of-type", "nth-of-type", "only-child", "only-of-type",
        "open", "optional", "out-of-range", "past", "paused",
        "picture-in-picture", "placeholder-shown", "playing", "popover-open",
        "read-only", "read-write", "required", "right", "root", "scope",
        "seeking", "stalled", "state", "target", "target-current",
        "user-invalid", "user-valid", "valid", "visited", "volume-locked",
        "where", "-webkit-any-link", "-webkit-autofill",
        "-webkit-full-screen",
      ]),
      selectorPseudoElements = new Set([
        "after", "backdrop", "before", "cue", "cue-region",
        "file-selector-button", "first-letter", "first-line", "marker",
        "part", "placeholder", "selection", "slotted",
      ]),
      selectorBalanced = (selector) => {
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
          if (char === "\\") {
            at++;
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
      selectorListSegmentsValid = (selector) => {
        let quote = "", squareDepth = 0, roundDepth = 0, segmentStart = 0;
        for (let at = 0; at <= selector.length; at++) {
          const char = selector[at] || ",";
          if (quote) {
            if (char === "\\") at++;
            else if (char === quote) quote = "";
            continue;
          }
          if (char === '"' || char === "'") {
            quote = char;
            continue;
          }
          if (char === "\\") {
            at++;
            continue;
          }
          if (char === "[") squareDepth++;
          else if (char === "]") squareDepth--;
          else if (char === "(") roundDepth++;
          else if (char === ")") roundDepth--;
          else if (char === "," && squareDepth === 0 && roundDepth === 0) {
            if (!selector.slice(segmentStart, at).trim()) return false;
            segmentStart = at + 1;
          }
        }
        return true;
      },
      selectorPseudosValid = (selector) => {
        let quote = "", squareDepth = 0;
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
          if (char === "\\") {
            at++;
            continue;
          }
          if (char === "[") {
            squareDepth++;
            continue;
          }
          if (char === "]") {
            squareDepth--;
            continue;
          }
          if (char !== ":" || squareDepth > 0) continue;
          const pseudoElement = selector[at + 1] === ":";
          if (pseudoElement) at++;
          const start = at + 1;
          if (selector[start] === "\\") continue;
          let end = start;
          while (end < selector.length && /[A-Za-z0-9_-]/.test(selector[end]))
            end++;
          if (end === start) return false;
          const name = selector.slice(start, end).toLowerCase(),
            known = pseudoElement
              ? selectorPseudoElements.has(name)
              : selectorPseudoClasses.has(name);
          if (!known) return false;
          at = end - 1;
        }
        return true;
      },
      selectorSyntaxValid = (value) => {
        const selector = String(value).trim();
        return (
          !!selector &&
          selectorBalanced(selector) &&
          selectorListSegmentsValid(selector) &&
          selectorPseudosValid(selector) &&
          !/#\s/.test(selector) &&
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
        const result = Object.create(NodeList.prototype),
          length = Math.min(Number(values?.length) >>> 0, 16384);
        for (let index = 0; index < length; index++)
          Object.defineProperty(result, index, {
            configurable: true,
            enumerable: true,
            value: values[index],
          });
        Object.defineProperty(result, "length", {
          configurable: true,
          value: length,
        });
        return result;
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
    globalThis.__tilefinchMarkNativeFunction = (value) => {
      if (typeof value === "function") nativeMethodSet.add(value);
      return value;
    };
    Object.defineProperty(globalThis, "__tilefinchIsNativeFunction", {
      configurable: false,
      enumerable: false,
      writable: false,
      value: (value) =>
        typeof value === "function" && nativeMethodSet.has(value),
    });
    Object.defineProperty(Function.prototype, "toString", {
      configurable: true,
      writable: true,
      value: nativeAwareToString,
    });
    /*
     * QuickJS records the JavaScript implementation of browser callbacks in
     * Error.stack.  Those frames are an implementation detail: Chromium stops
     * the public stack at the author callback, while exposing our
     * <browser-compat>/<browser-bootstrap> frames both fingerprints Tilefinch
     * and gives pages a non-standard view of the event machinery.
     *
     * Keep the engine's useful author frames, but interpose the public Error
     * constructors so errors created by page script receive a bounded stack
     * with browser-private and QuickJS call-adapter frames removed.  Native
     * errors thrown by the engine retain their original stack.  The facades
     * share the native prototypes and are marked native below, preserving
     * instanceof, subclassing, constructor identity and function reflection.
     */
    const sanitizePublicStack = (value) => {
        if (typeof value !== "string" || value.length > 65536) return value;
        const lines = value.split("\n"), kept = [];
        for (let index = 0; index < lines.length; index++) {
          const line = lines[index];
          if (
            line.includes("<browser-") ||
            /^\s*at (?:call|apply|construct) \(native\)\s*$/.test(line)
          ) continue;
          kept.push(line);
        }
        return kept.join("\n");
      },
      installPublicError = (name) => {
        const NativeError = globalThis[name];
        if (typeof NativeError !== "function" || !NativeError.prototype)
          return;
        const PublicError = {
          [name]: function (...args) {
            const value = new.target
              ? Reflect.construct(
                  NativeError,
                  args,
                  new.target === PublicError ? NativeError : new.target,
                )
              : Reflect.apply(NativeError, undefined, args);
            const stack = value && value.stack;
            if (typeof stack === "string")
              Object.defineProperty(value, "stack", {
                configurable: true,
                writable: true,
                value: sanitizePublicStack(stack),
              });
            return value;
          },
        }[name];
        Object.defineProperty(PublicError, "length", {
          configurable: true,
          value: NativeError.length,
        });
        Object.defineProperty(PublicError, "prototype", {
          writable: false,
          value: NativeError.prototype,
        });
        Object.defineProperty(NativeError.prototype, "constructor", {
          configurable: true,
          writable: true,
          value: PublicError,
        });
        for (const key of Reflect.ownKeys(NativeError)) {
          if (key === "length" || key === "name" || key === "prototype")
            continue;
          const descriptor = Object.getOwnPropertyDescriptor(NativeError, key);
          if (descriptor) Object.defineProperty(PublicError, key, descriptor);
        }
        nativeMethodSet.add(PublicError);
        globalThis[name] = PublicError;
      };
    for (const name of [
      "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError",
      "TypeError", "URIError", "AggregateError",
    ]) installPublicError(name);
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
  const urlSearchParamLimit = 8192,
    tilefinchUSVString = (value) => {
      const text = String(value);
      let output = "",
        start = 0;
      for (let index = 0; index < text.length; index++) {
        const unit = text.charCodeAt(index);
        if (unit >= 0xd800 && unit <= 0xdbff) {
          const next = index + 1 < text.length
            ? text.charCodeAt(index + 1) : 0;
          if (next >= 0xdc00 && next <= 0xdfff) {
            index++;
            continue;
          }
        } else if (unit < 0xdc00 || unit > 0xdfff) continue;
        output += text.slice(start, index) + "\ufffd";
        start = index + 1;
      }
      return start ? output + text.slice(start) : text;
    },
    appendURLParamUTF8 = (bytes, codePoint) => {
      if (codePoint <= 0x7f) bytes.push(codePoint);
      else if (codePoint <= 0x7ff)
        bytes.push(0xc0 | (codePoint >> 6), 0x80 | (codePoint & 0x3f));
      else if (codePoint <= 0xffff)
        bytes.push(
          0xe0 | (codePoint >> 12),
          0x80 | ((codePoint >> 6) & 0x3f),
          0x80 | (codePoint & 0x3f),
        );
      else
        bytes.push(
          0xf0 | (codePoint >> 18),
          0x80 | ((codePoint >> 12) & 0x3f),
          0x80 | ((codePoint >> 6) & 0x3f),
          0x80 | (codePoint & 0x3f),
        );
    },
    urlParamHex = (unit) =>
      unit >= 0x30 && unit <= 0x39 ? unit - 0x30
        : unit >= 0x41 && unit <= 0x46 ? unit - 0x41 + 10
          : unit >= 0x61 && unit <= 0x66 ? unit - 0x61 + 10 : -1,
    decodeURLParamBytes = (bytes) => {
      let output = "";
      for (let index = 0; index < bytes.length;) {
        const first = bytes[index];
        let codePoint, count, firstLower = 0x80, firstUpper = 0xbf;
        if (first <= 0x7f) {
          codePoint = first;
          count = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
          codePoint = first & 0x1f;
          count = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
          codePoint = first & 0x0f;
          count = 3;
          if (first === 0xe0) firstLower = 0xa0;
          else if (first === 0xed) firstUpper = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
          codePoint = first & 0x07;
          count = 4;
          if (first === 0xf0) firstLower = 0x90;
          else if (first === 0xf4) firstUpper = 0x8f;
        } else {
          output += "\ufffd";
          index++;
          continue;
        }
        let invalidAt = 0;
        for (let offset = 1;
             offset < count && index + offset < bytes.length; offset++) {
          const next = bytes[index + offset];
          if (next < (offset === 1 ? firstLower : 0x80)
              || next > (offset === 1 ? firstUpper : 0xbf)) {
            invalidAt = offset;
            break;
          }
          codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (invalidAt) {
          output += "\ufffd";
          index += invalidAt;
          continue;
        }
        if (index + count > bytes.length) {
          output += "\ufffd";
          index = bytes.length;
          continue;
        }
        output += trustedStringFromCodePoint(null, codePoint);
        index += count;
      }
      return output;
    },
    decodeURLParam = (value) => {
      const text = tilefinchUSVString(value).replace(/\+/g, " "),
        bytes = [];
      for (let index = 0; index < text.length;) {
        const firstHex = text.charCodeAt(index) === 0x25 && index + 2 < text.length
            ? urlParamHex(text.charCodeAt(index + 1)) : -1,
          secondHex = firstHex >= 0 ? urlParamHex(text.charCodeAt(index + 2)) : -1;
        if (secondHex >= 0) {
          bytes.push((firstHex << 4) | secondHex);
          index += 3;
        } else {
          const first = text.charCodeAt(index);
          let codePoint = first,
            units = 1;
          if (first >= 0xd800 && first <= 0xdbff && index + 1 < text.length) {
            const second = text.charCodeAt(index + 1);
            if (second >= 0xdc00 && second <= 0xdfff) {
              codePoint = 0x10000 + ((first - 0xd800) << 10)
                + (second - 0xdc00);
              units = 2;
            }
          }
          appendURLParamUTF8(bytes, codePoint);
          index += units;
        }
        if (bytes.length > 256 * 1024)
          throw new RangeError("URLSearchParams input exceeds bounded size");
      }
      return decodeURLParamBytes(bytes);
    },
    urlParamIterator = (owner, kind) => {
      let index = 0;
      return {
        next() {
          if (index >= owner.items.length) return { done: true, value: undefined };
          const pair = owner.items[index++];
          return {
            done: false,
            value: kind === 0 ? [pair[0], pair[1]]
              : kind === 1 ? pair[0] : pair[1],
          };
        },
        [Symbol.iterator]() { return this; },
      };
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
          this._appendDecoded(key, value);
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
          this._append(tilefinchUSVString(values[0]), tilefinchUSVString(values[1]));
        }
        return;
      }
      if (query !== null && typeof query === "object") {
        for (const key of Object.keys(query))
          this._append(tilefinchUSVString(key), tilefinchUSVString(query[key]));
        return;
      }
      for (const part of String(query).replace(/^\?/, "").split("&")) {
        if (!part) continue;
        const at = part.indexOf("=");
        const key = at < 0 ? part : part.slice(0, at),
          value = at < 0 ? "" : part.slice(at + 1);
        this._appendDecoded(key, value);
      }
    }
    _append(key, value) {
      if (this.items.length >= urlSearchParamLimit)
        throw new RangeError("URLSearchParams entry limit exceeded");
      this.items.push([key, value]);
    }
    _appendDecoded(key, value) {
      this._append(decodeURLParam(key), decodeURLParam(value));
    }
    _replace(query) {
      this.items.length = 0;
      for (const part of String(query).replace(/^\?/, "").split("&")) {
        if (!part) continue;
        const at = part.indexOf("=");
        this._appendDecoded(
          at < 0 ? part : part.slice(0, at),
          at < 0 ? "" : part.slice(at + 1),
        );
      }
    }
    append(key, value) {
      this._append(tilefinchUSVString(key), tilefinchUSVString(value));
      this.notify();
    }
    delete(key, value) {
      key = tilefinchUSVString(key);
      const matchValue = arguments.length > 1,
        wanted = matchValue ? tilefinchUSVString(value) : "";
      for (let index = this.items.length - 1; index >= 0; index--)
        if (this.items[index][0] === key
            && (!matchValue || this.items[index][1] === wanted))
          this.items.splice(index, 1);
      this.notify();
    }
    get(key) {
      key = tilefinchUSVString(key);
      const pair = this.items.find((item) => item[0] === key);
      return pair ? pair[1] : null;
    }
    getAll(key) {
      key = tilefinchUSVString(key);
      return this.items
        .filter((item) => item[0] === key)
        .map((item) => item[1]);
    }
    has(key, value) {
      key = tilefinchUSVString(key);
      const matchValue = arguments.length > 1,
        wanted = matchValue ? tilefinchUSVString(value) : "";
      return this.items.some((item) => item[0] === key
        && (!matchValue || item[1] === wanted));
    }
    set(key, value) {
      key = tilefinchUSVString(key);
      value = tilefinchUSVString(value);
      let seen = false;
      for (let index = 0; index < this.items.length;) {
        const pair = this.items[index];
        if (pair[0] !== key) {
          index++;
        } else if (!seen) {
          pair[1] = value;
          seen = true;
          index++;
        } else this.items.splice(index, 1);
      }
      if (!seen) this._append(key, value);
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
      return urlParamIterator(this, 0);
    }
    keys() {
      return urlParamIterator(this, 1);
    }
    values() {
      return urlParamIterator(this, 2);
    }
    [Symbol.iterator]() {
      return this.entries();
    }
    notify() {
      if (this.changed) this.changed(this.toString());
    }
    toString() {
      const enc = (value) =>
        encodeURIComponent(tilefinchUSVString(value))
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
    tilefinchUSVString(value).replace(
      /[^\x21-\x7e]/gu, (char) => encodeURIComponent(char));
  const tilefinchURLPath = (value) => {
    value = tilefinchUSVString(value);
    if (!value.startsWith("/")) value = "/" + value;
    return value.replace(/[^\x21-\x7e]|[?#]/gu,
      (char) => encodeURIComponent(char));
  },
    tilefinchURLHash = (value) => {
      value = tilefinchUSVString(value);
      if (!value) return "";
      if (value[0] === "#") value = value.slice(1);
      return "#" + value.replace(/[^\x21-\x7e]/gu,
        (char) => encodeURIComponent(char));
    };
  const tilefinchURLSearch = (value) => {
    value = tilefinchUSVString(value);
    if (!value) return "";
    if (value[0] === "?") value = value.slice(1);
    return "?" + value.replace(
      /[^\x21-\x7e]|[ #]/gu, (char) => encodeURIComponent(char));
  };
  class TilefinchURL {
    constructor(input, base) {
      const text = tilefinchURLInput(input);
      if (/^blob:/i.test(text)) {
        this._setBlob(text);
        return;
      }
      const resolved = __tilefinchResolveURL(
        text,
        base === undefined
          ? tilefinchURLInput(
              globalThis.__tilefinchLocationHref || "https://example.invalid/",
            )
          : tilefinchURLInput(base),
      );
      if (!resolved) throw new TypeError("Invalid URL");
      this._set(resolved);
    }
    _setSearchParams(search) {
      if (this._searchParams) this._searchParams._replace(search);
      else
        this._searchParams = new TilefinchURLSearchParams(search, (value) => {
          this.search = value ? "?" + value : "";
          this._href = this.protocol === "blob:"
            ? "blob:" + this.pathname + this.search + this.hash
            : this.origin + this.pathname + this.search + this.hash;
        });
    }
    _setBlob(href) {
      const parsed = String(href).match(/^blob:([^?#]+)(\?[^#]*)?(#.*)?$/i);
      if (!parsed) throw new TypeError("Invalid URL");
      const pathname = parsed[1],
        search = parsed[2] || "",
        hash = parsed[3] || "",
        embedded = pathname.match(
          /^([A-Za-z][A-Za-z0-9+.-]*:)\/\/([^\/?#:]+)(?::([0-9]+))?/,
        ),
        origin = embedded
          ? embedded[1].toLowerCase() + "//" + embedded[2] +
            (embedded[3] ? ":" + embedded[3] : "")
          : "null";
      this._protocol = "blob:";
      this._hostname = "";
      this._port = "";
      this._host = "";
      this._pathname = pathname;
      this._search = search;
      this._hash = hash;
      this._origin = origin;
      this._href = "blob:" + pathname + search + hash;
      this._setSearchParams(search);
    }
    _set(href) {
      if (/^blob:/i.test(String(href))) {
        this._setBlob(href);
        return;
      }
      const parsed = String(href).match(
        /^([^:]+:)\/\/([^\/?#:]+)(?::([0-9]+))?([^?#]*)(\?[^#]*)?(#.*)?$/,
      );
      if (!parsed) throw new TypeError("Invalid URL");
      const serialize = (value) =>
        String(value || "").replace(/[^\x21-\x7e]/gu, (char) =>
          encodeURIComponent(char),
        ),
        protocol = parsed[1],
        hostname = parsed[2],
        port = parsed[3] || "",
        host = hostname + (port ? ":" + port : ""),
        pathname = serialize(parsed[4]) || "/",
        search = serialize(parsed[5]),
        hash = serialize(parsed[6]),
        origin = protocol + "//" + host;
      this._protocol = protocol;
      this._hostname = hostname;
      this._port = port;
      this._host = host;
      this._pathname = pathname;
      this._search = search;
      this._hash = hash;
      this._origin = origin;
      this._href = origin + pathname + search + hash;
      this._setSearchParams(search);
    }
    _setCandidate(value) {
      try {
        const next = new TilefinchURL(value, this._href);
        this._set(next._href);
        return true;
      } catch (_) {
        return false;
      }
    }
    get href() {
      return this._href;
    }
    set href(value) {
      const next = new TilefinchURL(value, this._href);
      this._set(next._href);
    }
    get protocol() { return this._protocol; }
    set protocol(value) {
      if (this._protocol === "blob:") return;
      value = tilefinchUSVString(value);
      if (!value.endsWith(":")) value += ":";
      if (!/^[A-Za-z][A-Za-z0-9+.-]*:$/.test(value)) return;
      this._setCandidate(value + "//" + this.host + this.pathname +
        this.search + this.hash);
    }
    get hostname() { return this._hostname; }
    set hostname(value) {
      if (this._protocol === "blob:") return;
      value = tilefinchUSVString(value);
      if (!value || /[\s\/?#@]/.test(value)) return;
      this._setCandidate(this.protocol + "//" + value +
        (this.port ? ":" + this.port : "") + this.pathname +
        this.search + this.hash);
    }
    get port() { return this._port; }
    set port(value) {
      if (this._protocol === "blob:") return;
      value = tilefinchUSVString(value);
      if (value && (!/^[0-9]+$/.test(value) || Number(value) > 65535)) return;
      if ((this.protocol === "http:" && value === "80") ||
          (this.protocol === "https:" && value === "443")) value = "";
      this._setCandidate(this.protocol + "//" + this.hostname +
        (value ? ":" + value : "") + this.pathname +
        this.search + this.hash);
    }
    get host() { return this._host; }
    set host(value) {
      if (this._protocol === "blob:") return;
      value = tilefinchUSVString(value);
      if (!value || /[\s\/?#@]/.test(value)) return;
      this._setCandidate(this.protocol + "//" + value + this.pathname +
        this.search + this.hash);
    }
    get pathname() { return this._pathname; }
    set pathname(value) {
      if (this._protocol === "blob:") return;
      this._setCandidate(this.origin + tilefinchURLPath(value) +
        this.search + this.hash);
    }
    get hash() { return this._hash || ""; }
    set hash(value) {
      const hash = tilefinchURLHash(value);
      this._hash = hash;
      if (this._href)
        this._href = this.protocol === "blob:"
          ? "blob:" + this.pathname + this.search + hash
          : this.origin + this.pathname + this.search + hash;
    }
    get origin() { return this._origin; }
    get searchParams() {
      return this._searchParams;
    }
    get search() {
      return this._search || "";
    }
    set search(value) {
      const search = tilefinchURLSearch(value);
      this._search = search;
      if (this._searchParams) this._searchParams._replace(search);
      if (this._href)
        this._href = this.protocol === "blob:"
          ? "blob:" + this.pathname + search + this.hash
          : this.origin + this.pathname + search + this.hash;
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
          out += trustedStringFromCodePoint(null, 65533);
          this._bomSeen = true;
        };
        while (i < bytes.length) {
          const start = i,
            a = bytes[i];
          let cp = 0,
            need = 0,
            firstLower = 128,
            firstUpper = 191;
          if (a <= 127) {
            cp = a;
            i++;
          } else if (a >= 194 && a <= 223) {
            cp = a & 31;
            need = 1;
          } else if (a >= 224 && a <= 239) {
            cp = a & 15;
            need = 2;
            if (a === 224) firstLower = 160;
            else if (a === 237) firstUpper = 159;
          } else if (a >= 240 && a <= 244) {
            cp = a & 7;
            need = 3;
            if (a === 240) firstLower = 144;
            else if (a === 244) firstUpper = 143;
          } else {
            invalid();
            i++;
            continue;
          }
          if (need) {
            let valid = true;
            let invalidAt = 0;
            for (let j = 1; j <= need && start + j < bytes.length; j++) {
              const b = bytes[i + j];
              if (
                b < (j === 1 ? firstLower : 128) ||
                b > (j === 1 ? firstUpper : 191)
              ) {
                valid = false;
                invalidAt = j;
                break;
              }
              cp = (cp << 6) | (b & 63);
            }
            if (!valid) {
              invalid();
              /* Consume the valid prefix of the malformed sequence, then
                 reconsume the first non-continuation byte as required by
                 the Encoding Standard's UTF-8 decoder state machine. */
              i = start + invalidAt;
              continue;
            }
            if (start + need >= bytes.length) {
              if (stream) {
                this._pending = bytes.slice(start);
                break;
              }
              invalid();
              i = bytes.length;
              break;
            }
            i += need + 1;
          }
          if (!this._bomSeen) {
            this._bomSeen = true;
            if (!this.ignoreBOM && cp === 65279) continue;
          }
          out += trustedStringFromCodePoint(null, cp);
        }
        if (!stream) {
          if (this._pending.length) invalid();
          this._pending = new Uint8Array();
          this._bomSeen = false;
        }
        return out;
      }
    };
  const blobURLs = new Map(),
    blobStates = new WeakMap(),
    fileStates = new WeakMap(),
    blobBytes = (blob) => {
      const state = trustedWeakMapGet(blobStates, blob);
      if (!state)
        throw new TypeError("Blob method called on incompatible receiver");
      return state.bytes;
    },
    blobBytesCopy = (blob) => {
      const state = trustedWeakMapGet(blobStates, blob);
      if (!state)
        throw new TypeError("Blob method called on incompatible receiver");
      const copy = new TrustedUint8Array(state.length);
      trustedUint8ArraySet(copy, state.bytes);
      return copy;
    };
  let nextBlobURL = 1,
    blobURLBytes = 0;
  const blobURLLimit = 16,
    blobURLByteLimit = 512 * 1024;
  const TilefinchBlob = class Blob {
    constructor(parts = [], options = {}) {
      if (parts === null || parts === undefined || !parts[Symbol.iterator])
        throw new TypeError("Blob parts must be iterable");
      const chunks = [];
      let size = 0;
      for (const part of parts) {
        let bytes, byteLength;
        const blobState = trustedWeakMapGet(blobStates, part);
        if (blobState) {
          bytes = blobState.bytes;
          byteLength = blobState.length;
        } else if (part instanceof ArrayBuffer) {
          bytes = new Uint8Array(part);
          byteLength = bytes.byteLength;
        } else if (ArrayBuffer.isView(part)) {
          bytes = new Uint8Array(
            part.buffer, part.byteOffset, part.byteLength);
          byteLength = bytes.byteLength;
        } else {
          bytes = new TextEncoder().encode(String(part));
          byteLength = bytes.byteLength;
        }
        if (byteLength > 256 * 1024 - size)
          throw new RangeError("Blob exceeds bounded size");
        chunks[chunks.length] = { bytes, length: byteLength };
        size += byteLength;
      }
      const retained = new TrustedUint8Array(size);
      let at = 0;
      for (let i = 0; i < chunks.length; i++) {
        trustedUint8ArraySet(retained, chunks[i].bytes, at);
        at += chunks[i].length;
      }
      trustedWeakMapSet(blobStates, this, {
        bytes: retained,
        length: size,
        type: String(options.type || "")
          .toLowerCase()
          .replace(/[^ -~]/g, ""),
      });
    }
    get size() {
      return blobBytes(this).byteLength;
    }
    get type() {
      const state = trustedWeakMapGet(blobStates, this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.type;
    }
    arrayBuffer() {
      return Promise.resolve(blobBytesCopy(this).buffer);
    }
    text() {
      return Promise.resolve(new TextDecoder().decode(blobBytesCopy(this)));
    }
    bytes() {
      return Promise.resolve(blobBytesCopy(this));
    }
    stream() {
      const bytes = blobBytesCopy(this);
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
      return new TilefinchBlob([blobBytesCopy(this).slice(from, to)], { type });
    }
  };
  globalThis.Blob = TilefinchBlob;
  globalThis.__tilefinchBlobBytes = blobBytesCopy;
  const TilefinchFile = globalThis.File = class File extends TilefinchBlob {
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
      trustedWeakMapSet(fileStates, this, {
        name: this.name,
        lastModified: this.lastModified,
      });
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
  /* A page fullscreen state is deliberately singular and presentation-only.
     Native code owns the viewport/chrome transition; JavaScript retains the
     standards-facing element identity and events. */
  let fullscreenElement = null;
  const dispatchFullscreenChange = (element) => {
      const target = element?.isConnected ? element : document;
      target.dispatchEvent(new Event("fullscreenchange", { bubbles: true }));
    },
    leaveFullscreen = (fromHost = false) => {
      const previous = fullscreenElement;
      if (!previous) return true;
      if (!fromHost && !__tilefinchSetFullscreen(0, 0)) return false;
      fullscreenElement = null;
      dispatchFullscreenChange(previous);
      return true;
    };
  Object.defineProperties(document, {
    fullscreenElement: {
      configurable: true,
      enumerable: true,
      get() {
        if (fullscreenElement && !fullscreenElement.isConnected)
          leaveFullscreen(false);
        return fullscreenElement;
      },
    },
    fullscreenEnabled: {
      configurable: true,
      enumerable: true,
      get: () => true,
    },
    webkitFullscreenElement: {
      configurable: true,
      get() { return document.fullscreenElement; },
    },
  });
  Element.prototype.requestFullscreen = function () {
    return new Promise((resolve, reject) => {
      if (!this.isConnected || !__tilefinchSetFullscreen(this.__handle, 1)) {
        const error = new DOMException(
          "Fullscreen requires a connected element and user activation",
          "NotAllowedError",
        );
        this.dispatchEvent(new Event("fullscreenerror", { bubbles: true }));
        reject(error);
        return;
      }
      const previous = fullscreenElement;
      fullscreenElement = this;
      dispatchFullscreenChange(previous || this);
      resolve();
    });
  };
  Element.prototype.webkitRequestFullscreen =
    Element.prototype.requestFullscreen;
  document.exitFullscreen = () => new Promise((resolve, reject) => {
    if (!fullscreenElement) { resolve(); return; }
    if (!leaveFullscreen(false)) {
      reject(new DOMException("Could not leave fullscreen", "InvalidStateError"));
      return;
    }
    resolve();
  });
  document.webkitExitFullscreen = document.exitFullscreen;
  globalThis.__tilefinchExitFullscreenFromHost = () => leaveFullscreen(true);
  globalThis.__tilefinchRegisterNativeNodeStateCleanup?.((handle) => {
    if (fullscreenElement?.__handle === Number(handle)) leaveFullscreen(true);
  });

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
      if (name) {
        const peers = document.querySelectorAll("input");
        for (let index = 0; index < peers.length; index++) {
          const peer = peers[index];
          if (
            peer !== input &&
            peer instanceof HTMLInputElement &&
            __tilefinchControlType(peer) === "radio" &&
            peer.name === name &&
            peer.closest("form") === form
          )
            peer.removeAttribute("checked");
        }
      }
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
            name = control.name,
            peers = document.querySelectorAll("input");
          let groupChecked = false;
          for (let index = 0; index < peers.length; index++) {
            const item = peers[index];
            if (
              item instanceof HTMLInputElement &&
              String(item.type).toLowerCase() === "radio" &&
              item.name === name &&
              item.closest("form") === form &&
              item.checked
            ) {
              groupChecked = true;
              break;
            }
          }
          valueMissing = !groupChecked;
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
        if (!control.multiple && !selectedOptionSet(control).length
            && options.length) markOptionSelected(options[0], true);
        syncSelectedContent(control);
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
    append(name, value, filename) {
      if (this.items.length >= 256)
        throw new RangeError("FormData entry limit exceeded");
      if (value instanceof Blob && (!(value instanceof File) || filename !== undefined))
        value = new File([value], filename === undefined ? "blob" : String(filename), {
          type: value.type,
          lastModified: value instanceof File ? value.lastModified : Date.now(),
        });
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
    set(name, value, filename) {
      name = String(name);
      if (value instanceof Blob && (!(value instanceof File) || filename !== undefined))
        value = new File([value], filename === undefined ? "blob" : String(filename), {
          type: value.type,
          lastModified: value instanceof File ? value.lastModified : Date.now(),
        });
      else value = value instanceof Blob ? value : String(value);
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
  TilefinchURL.createObjectURL = function createObjectURL(blob) {
    const state = trustedWeakMapGet(blobStates, blob);
    if (!state) throw new TypeError("Blob required");
    if (
      trustedMapSize(blobURLs) >= blobURLLimit ||
      blobURLBytes + state.bytes.byteLength > blobURLByteLimit
    )
      throw new RangeError("blob URL quota exceeded");
    const url = "blob:" + location.origin + "/" + nextBlobURL++;
    trustedMapSet(blobURLs, url, blob);
    blobURLBytes += state.bytes.byteLength;
    return url;
  };
  TilefinchURL.revokeObjectURL = function revokeObjectURL(url) {
    url = trustedString(url);
    const blob = trustedMapGet(blobURLs, url);
    if (blob) {
      blobURLBytes -= blobBytes(blob).byteLength;
      trustedMapDelete(blobURLs, url);
    }
  };
  /* Worker binds policy and retained source to one WebIDL conversion. Keep
     the conversion intrinsic private and make lookup accept only its already
     converted primitive so author String replacement cannot split identity. */
  globalThis.__tilefinchTrustedString = (value) => trustedString(value);
  globalThis.__tilefinchBlobForURL = (url) =>
    typeof url === "string" ? trustedMapGet(blobURLs, url) || null : null;
  const workerSourceForBlob = (blob) => {
    const state = trustedWeakMapGet(blobStates, blob);
    if (!state) throw new TypeError("Blob required");
    const bytes = state.bytes, length = state.length;
    let out = "", i = 0;
    while (i < length) {
      const a = bytes[i++];
      let cp, need = 0, minimum = 0;
      if (a <= 127) cp = a;
      else if (a >= 194 && a <= 223) {
        cp = a & 31; need = 1; minimum = 128;
      } else if (a >= 224 && a <= 239) {
        cp = a & 15; need = 2; minimum = 2048;
      } else if (a >= 240 && a <= 244) {
        cp = a & 7; need = 3; minimum = 65536;
      } else {
        out += trustedStringFromCodePoint(null, 65533);
        continue;
      }
      if (i + need > length) {
        out += trustedStringFromCodePoint(null, 65533);
        break;
      }
      let valid = true;
      for (let j = 0; j < need; j++) {
        const b = bytes[i + j];
        if ((b & 192) !== 128) { valid = false; break; }
        cp = (cp << 6) | (b & 63);
      }
      if (!valid || cp < minimum || cp > 1114111
          || (cp >= 55296 && cp <= 57343)) {
        out += trustedStringFromCodePoint(null, 65533);
        continue;
      }
      i += need;
      out += trustedStringFromCodePoint(null, cp);
    }
    return out;
  };
  Object.defineProperty(globalThis, "__tilefinchWorkerIntrinsics", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: Object.freeze({
      apply: trustedFunctionApply,
      charCodeAt: trustedCharCodeAt,
      indexOf: trustedStringIndexOf,
      join: trustedArrayJoin,
      slice: trustedStringSlice,
      sourceForBlob: workerSourceForBlob,
      split: trustedStringSplit,
    }),
  });
  /* Structured clone with a memory map: shared references stay shared,
     cycles are preserved, and every unsupported value is a DataCloneError.
     Bounds are on the total number of values and the number of distinct
     objects, so a wide shallow message and a deep narrow one both fail
     predictably instead of exhausting the 5 MiB realm. */
  const cloneError = (message) => {
    const DOMExceptionType = globalThis.DOMException;
    return typeof DOMExceptionType === "function"
      ? new DOMExceptionType(message, "DataCloneError")
      : new TypeError(message);
  };
  const workerCloneConstructorNames = Object.freeze([
      "Object", "Array", "ArrayBuffer", "DataView", "Date", "RegExp",
      "Blob", "File", "MessagePort",
      "Map", "Set", "Error", "EvalError", "RangeError", "ReferenceError",
      "SyntaxError", "TypeError", "URIError", "Boolean", "Number", "String",
      "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
      "Uint16Array", "Int32Array", "Uint32Array", "Float32Array",
      "Float64Array", "BigInt64Array", "BigUint64Array",
    ]),
    captureWorkerCloneIntrinsics = (realm) => {
      const intrinsics = Object.create(null);
      for (const name of workerCloneConstructorNames)
        if (typeof realm[name] === "function") intrinsics[name] = realm[name];
      return Object.freeze(intrinsics);
    },
    ownerWorkerCloneIntrinsics = captureWorkerCloneIntrinsics(globalThis);
  Object.defineProperty(globalThis, "__tilefinchWorkerCloneIntrinsics", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: Object.freeze({
      capture: captureWorkerCloneIntrinsics,
      owner: ownerWorkerCloneIntrinsics,
    }),
  });
  const cloneErrorTypes = (intrinsics) => ({
    EvalError: intrinsics.EvalError,
    RangeError: intrinsics.RangeError,
    ReferenceError: intrinsics.ReferenceError,
    SyntaxError: intrinsics.SyntaxError,
    TypeError: intrinsics.TypeError,
    URIError: intrinsics.URIError,
  }),
    ordinaryClonePrototypeKind = (first) => {
      let userTag = false;
      for (let at = first, steps = 0; steps < 32; steps++) {
        if (at === null) return userTag ? 2 : 1;
        if (at === EventTarget.prototype) return 0;
        const tag = trustedObjectDescriptor(at, Symbol.toStringTag);
        if (tag) {
          const constructor = trustedObjectDescriptor(at, "constructor");
          if (globalThis.__tilefinchIsNativeFunction?.(constructor?.value))
            return 0;
          userTag = true;
        }
        at = trustedObjectPrototype(at);
      }
      return 0;
    };
  const cloneWorkerValueInternal = (value, state, depth) => {
    if (state.remaining-- <= 0)
      throw cloneError("worker message value limit");
    if (
      value === null ||
      typeof value === "string" ||
      typeof value === "number" ||
      typeof value === "boolean" ||
      typeof value === "undefined" ||
      typeof value === "bigint"
    )
      return value;
    if (typeof value === "function" || typeof value === "symbol")
      throw cloneError("value could not be cloned");
    /* Proxy is never structured-cloneable. Reject by native brand before
       prototype/key inspection so no getPrototypeOf, ownKeys, get or getter
       trap can run as a side effect of postMessage/structuredClone. */
    if (globalThis.__tilefinchIsProxy(value))
      throw cloneError("Proxy values could not be cloned");
    const memory = state.memory;
    if (memory.has(value)) return memory.get(value);
    if (depth > 64) throw cloneError("worker message nesting limit");
    if (memory.size >= 16384) throw cloneError("worker message object limit");
    const remember = (copy) => {
      memory.set(value, copy);
      return copy;
    };
    if (state.transferMap?.has(value))
      return remember(state.transferMap.get(value));
    const clone = (item) => cloneWorkerValueInternal(item, state, depth + 1);
    const intrinsics = state.intrinsics;
    const blobState = trustedWeakMapGet(blobStates, value);
    if (blobState) {
      const bytes = new TrustedUint8Array(blobState.length);
      trustedUint8ArraySet(bytes, blobState.bytes);
      const fileState = trustedWeakMapGet(fileStates, value);
      if (fileState && typeof intrinsics.File === "function")
        return remember(new intrinsics.File([bytes], fileState.name, {
          type: blobState.type,
          lastModified: fileState.lastModified,
        }));
      if (typeof intrinsics.Blob !== "function")
        throw cloneError("unsupported worker message blob");
      return remember(new intrinsics.Blob([bytes], { type: blobState.type }));
    }
    let arrayBufferLength;
    try {
      arrayBufferLength = trustedArrayBufferByteLength(value);
    } catch (_) {}
    if (arrayBufferLength !== undefined) {
      const copy = remember(new intrinsics.ArrayBuffer(arrayBufferLength));
      trustedUint8ArraySet(
        new intrinsics.Uint8Array(copy), new TrustedUint8Array(value));
      return copy;
    }
    let dataViewBuffer, dataViewOffset, dataViewLength;
    try {
      dataViewBuffer = trustedDataViewBuffer(value);
      dataViewOffset = trustedDataViewByteOffset(value);
      dataViewLength = trustedDataViewByteLength(value);
    } catch (_) {}
    if (dataViewBuffer !== undefined) {
      const buffer = clone(dataViewBuffer);
      return remember(new intrinsics.DataView(
        buffer, dataViewOffset, dataViewLength));
    }
    if (trustedArrayBufferIsView(value)) {
      /* Views over one buffer keep sharing the cloned buffer. */
      const name = trustedTypedArrayName(value),
        buffer = clone(trustedTypedArrayBuffer(value)),
        constructor = intrinsics[name];
      if (typeof constructor !== "function")
        throw cloneError("unsupported worker message view");
      return remember(
        new constructor(
          buffer, trustedTypedArrayByteOffset(value),
          trustedTypedArrayLength(value),
        ),
      );
    }
    const cloneWasmModule = globalThis.__tilefinchCloneWasmModule;
    if (typeof cloneWasmModule === "function") {
      const module = cloneWasmModule(value);
      if (module !== null) return remember(module);
    }
    let dateTime;
    try {
      dateTime = trustedDateTime(value);
    } catch (_) {}
    if (dateTime !== undefined)
      return remember(new intrinsics.Date(dateTime));
    let regexpSource, regexpFlags;
    try {
      regexpSource = trustedRegExpSource(value);
      regexpFlags = trustedRegExpFlags(value);
    } catch (_) {}
    if (regexpSource !== undefined)
      return remember(new intrinsics.RegExp(regexpSource, regexpFlags));
    try {
      return remember(new intrinsics.Boolean(trustedBooleanValue(value)));
    } catch (_) {}
    try {
      return remember(new intrinsics.Number(trustedNumberValue(value)));
    } catch (_) {}
    try {
      return remember(new intrinsics.String(trustedStringValue(value)));
    } catch (_) {}
    let mapSize;
    try {
      mapSize = trustedMapSize(value);
    } catch (_) {}
    if (mapSize !== undefined) {
      if (mapSize > 1024) throw cloneError("worker message item limit");
      const copy = remember(new intrinsics.Map());
      let count = 0;
      for (const [key, item] of trustedMapEntries(value)) {
        if (++count > 1024) throw cloneError("worker message item limit");
        trustedMapSet(copy, clone(key), clone(item));
      }
      return copy;
    }
    let setSize;
    try {
      setSize = trustedSetSize(value);
    } catch (_) {}
    if (setSize !== undefined) {
      if (setSize > 1024) throw cloneError("worker message item limit");
      const copy = remember(new intrinsics.Set());
      let count = 0;
      for (const item of trustedSetValues(value)) {
        if (++count > 1024) throw cloneError("worker message item limit");
        trustedSetAdd(copy, clone(item));
      }
      return copy;
    }
    if (trustedArrayIsArray(value)) {
      if (value.length > 1024) throw cloneError("worker message item limit");
      const copy = remember(new intrinsics.Array(value.length));
      /* Structured serialization visits enumerable own properties.  Using
         `index in value` both admitted inherited indexes and discarded named
         properties carried by framework message arrays. */
      const keys = trustedObjectKeys(value);
      if (keys.length > 1152)
        throw cloneError("worker message item limit");
      for (const key of keys) {
        trustedDefineProperty(copy, key, {
          configurable: true,
          enumerable: true,
          writable: true,
          value: clone(value[key]),
        });
      }
      return copy;
    }
    const prototypeKind = ordinaryClonePrototypeKind(
      trustedObjectPrototype(value),
    );
    /* Error has no dedicated public brand getter. Object#toString is safe
       only when no page-controlled @@toStringTag exists in the chain. */
    if (prototypeKind === 1 && trustedObjectTag(value) === "[object Error]") {
      const ErrorType = cloneErrorTypes(intrinsics)[String(value.name)]
        || intrinsics.Error;
      const copy = new ErrorType(String(value.message));
      try {
        if (typeof value.stack === "string") copy.stack = value.stack;
      } catch (_) {}
      return remember(copy);
    }
    if (prototypeKind !== 0) {
      const copy = remember(new intrinsics.Object());
      const keys = trustedObjectKeys(value);
      if (keys.length > 128) throw cloneError("worker message key limit");
      /* Structured serialization creates ordinary own data properties. An
         assignment here would invoke an inherited setter in the receiving
         realm and gives __proto__ its legacy mutation semantics, allowing a
         valid Worker message to arrive with fields missing or redirected. */
      for (const key of keys) {
        trustedDefineProperty(copy, key, {
          configurable: true,
          enumerable: true,
          writable: true,
          value: clone(value[key]),
        });
      }
      return copy;
    }
    throw cloneError("unsupported worker message value");
  };
  const detachArrayBuffer = globalThis.__tilefinchDetachArrayBuffer,
    collectWorkerTransferList = (sequence) => {
      if (sequence === undefined) return [];
      let values;
      try {
        values = trustedArrayFrom(sequence);
      } catch (_) {
        throw cloneError("transfer list is not iterable");
      }
      if (values.length > 16)
        throw cloneError("worker transfer list limit");
      const seen = new Set(), entries = [];
      for (const value of values) {
        let kind = 0;
        try {
          trustedArrayBufferByteLength(value);
        } catch (_) {
          kind = 1;
        }
        if (kind === 1 && typeof globalThis
            .__tilefinchPrepareMessagePortTransfer !== "function")
          throw cloneError("unsupported transferable object");
        if (seen.has(value))
          throw cloneError("duplicate transferable object");
        seen.add(value);
        entries.push({ value, kind });
      }
      return entries;
    },
    cloneWorkerValue = (
      value, intrinsics = ownerWorkerCloneIntrinsics, transfer, portsOut,
    ) => {
      const transferList = collectWorkerTransferList(transfer),
        transferMap = new Map(),
        preparedPorts = [];
      for (const entry of transferList) {
        if (entry.kind !== 1) continue;
        const prepared = globalThis.__tilefinchPrepareMessagePortTransfer(
          entry.value, intrinsics.MessagePort?.prototype);
        if (!prepared) throw cloneError("unsupported transferable object");
        if (preparedPorts.some((item) =>
            item.original === prepared.peer || item.peer === prepared.original))
          throw cloneError("entangled message ports cannot transfer together");
        preparedPorts.push(prepared);
        transferMap.set(entry.value, prepared.copy);
      }
      const cloneState = {
          memory: new Map(), remaining: 262144, intrinsics, transferMap,
        },
        copy = cloneWorkerValueInternal(
          value, cloneState, 0);
      /* Clone is transactional: detach only after every reachable value has
         serialized successfully. The native sink repeats the class check. */
      if (preparedPorts.some((prepared) => !prepared.canCommit()))
        throw cloneError("message port transfer could not commit");
      for (const entry of transferList)
        if (entry.kind === 0) detachArrayBuffer(entry.value);
      for (const prepared of preparedPorts) {
        if (!prepared.commit())
          throw cloneError("message port transfer could not commit");
        if (portsOut) portsOut.push(prepared.copy);
      }
      return copy;
    };
  globalThis.__tilefinchCloneWorkerValue = cloneWorkerValue;
  if (globalThis.structuredClone === undefined)
    globalThis.structuredClone = (value, options = undefined) =>
      cloneWorkerValue(value, ownerWorkerCloneIntrinsics, options?.transfer);
  const tilefinchCurrentDocumentURL =
      globalThis.__tilefinchCurrentDocumentURL,
    tilefinchDocumentURLRevision =
      globalThis.__tilefinchDocumentURLRevision,
    location = new TilefinchURL(
      String(globalThis.__tilefinchLocationHref || "https://example.invalid/"),
    ),
    locationPartNames = [
      "protocol",
      "hostname",
      "port",
      "host",
      "pathname",
      "search",
      "hash",
      "origin",
      "searchParams",
    ];
  let locationRevision = Number(tilefinchDocumentURLRevision()) >>> 0,
    locationSynchronizing = false;
  const synchronizeLocation = () => {
      if (locationSynchronizing) return;
      const revision = Number(tilefinchDocumentURLRevision()) >>> 0;
      if (revision === locationRevision) return;
      const href = tilefinchCurrentDocumentURL();
      locationSynchronizing = true;
      try {
        location._set(String(href));
        locationRevision = revision;
      } finally {
        locationSynchronizing = false;
      }
    },
    setSynchronizedLocation = (href) => {
      locationSynchronizing = true;
      try {
        location._set(String(href));
        locationRevision = Number(tilefinchDocumentURLRevision()) >>> 0;
      } finally {
        locationSynchronizing = false;
      }
    };
  for (const name of locationPartNames) {
    const descriptor = trustedObjectDescriptor(
      TilefinchURL.prototype, name);
    Object.defineProperty(location, name, {
      configurable: true,
      enumerable: true,
      get() {
        synchronizeLocation();
        return descriptor.get.call(location);
      },
      set: descriptor.set === undefined ? undefined : function (value) {
        synchronizeLocation();
        const previous = location._href;
        descriptor.set.call(location, value);
        if (!locationSynchronizing && location._href !== previous)
          __tilefinchRequestNavigation(location._href, false);
      },
    });
  }
  Object.defineProperty(location, "href", {
    configurable: false,
    enumerable: true,
    get() {
      synchronizeLocation();
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
            globalThis.__tilefinchFinishEventDispatch(event);
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
            if (name === "style")
              globalThis.__tilefinchInvalidateDetachedStyle?.(this);
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
            if (namespace === null && localName === "style")
              globalThis.__tilefinchInvalidateDetachedStyle?.(this);
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
              if (name === "style")
                globalThis.__tilefinchInvalidateDetachedStyle?.(this);
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
              if (namespace === null && localName === "style")
                globalThis.__tilefinchInvalidateDetachedStyle?.(this);
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
          xml ? XMLDocument.prototype : HTMLDocument.prototype,
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
    globalThis.DOMImplementation = class DOMImplementation {
      constructor(token) {
        if (token !== implementationToken)
          throw new TypeError("Illegal constructor");
      }
      createHTMLDocument(title) {
        return detachedDocument(title);
      }
      createDocument(namespace, qualifiedName, doctype = null) {
        return detachedDocument(
          undefined,
          true,
          doctype,
          namespace,
          qualifiedName || "",
        );
      }
      createDocumentType(name, publicId, systemId) {
        return detachedDocument(undefined, true).createDocumentType(
          name,
          publicId,
          systemId,
        );
      }
      hasFeature() {
        return true;
      }
    };
    const implementationToken = {},
      implementations = new WeakMap();
    Object.defineProperty(Document.prototype, "implementation", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof Document)) throw new TypeError("Illegal invocation");
        let value = implementations.get(this);
        if (!value) {
          value = new globalThis.DOMImplementation(implementationToken);
          implementations.set(this, value);
        }
        return value;
      },
    });
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
      this.setStart(
        node.parentNode,
        Array.prototype.indexOf.call(node.parentNode.childNodes, node),
      );
    }
    setStartAfter(node) {
      this.setStart(
        node.parentNode,
        Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1,
      );
    }
    setEndBefore(node) {
      this.setEnd(
        node.parentNode,
        Array.prototype.indexOf.call(node.parentNode.childNodes, node),
      );
    }
    setEndAfter(node) {
      this.setEnd(
        node.parentNode,
        Array.prototype.indexOf.call(node.parentNode.childNodes, node) + 1,
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
  Object.setPrototypeOf(document, HTMLDocument.prototype);
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
    let pendingImageDecodes = 0;
    HTMLImageElement.prototype.decode = function () {
      const image = this;
      return new Promise((resolve, reject) => {
        if (pendingImageDecodes >= 16) {
          reject(new DOMException(
            "Too many pending image decodes", "QuotaExceededError"));
          return;
        }
        pendingImageDecodes++;
        let settled = false, polls = 0;
        const finish = (error = null) => {
          if (settled) return;
          settled = true;
          pendingImageDecodes--;
          image.removeEventListener("load", loaded);
          image.removeEventListener("error", failed);
          if (error) reject(error); else resolve();
        },
        loaded = () => finish(),
        failed = () => finish(new DOMException(
          "The image could not be decoded", "EncodingError")),
        poll = () => {
          if (settled) return;
          if (!image.src) { failed(); return; }
          if (image.complete) {
            if (image.naturalWidth > 0 && image.naturalHeight > 0) loaded();
            else failed();
            return;
          }
          if (++polls >= 300) { failed(); return; }
          setTimeout(poll, 100);
        };
        image.addEventListener("load", loaded, { once: true });
        image.addEventListener("error", failed, { once: true });
        queueMicrotask(poll);
      });
    };
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
      selectedSource = (node) => {
        const own = node.getAttribute("src");
        if (own) return own;
        const children = node.children || [],
          limit = Math.min(Number(children.length) || 0, 16);
        for (let index = 0; index < limit; index++) {
          const source = children[index];
          if (!(source instanceof HTMLSourceElement)) continue;
          const value = source.getAttribute("src");
          if (!value) continue;
          const media = source.getAttribute("media");
          if (media && !matchMedia(media).matches) continue;
          const type = source.getAttribute("type");
          if (type && !node.canPlayType(type)) continue;
          return value;
        }
        return "";
      },
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
            nativeState: -1,
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
    /* Native controls can activate a video without calling the page-visible
       play() method. The runtime captures and removes this bootstrap bridge
       before author code runs, so native state delivery can still obtain the
       same WeakMap record without trusting a mutable Window property. */
    globalThis.__tilefinchMediaStateFor = (node) => {
      if (!(node instanceof HTMLMediaElement)) return null;
      return stateFor(node);
    };
    Object.defineProperties(HTMLMediaElement.prototype, {
      src: reflectString("src"),
      currentSrc: {
        configurable: true,
        enumerable: true,
        get() {
          const value = selectedSource(this);
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
      if (type.startsWith("audio/mp4")) {
        if (!type.includes("codecs=")) return "maybe";
        if (type.includes("mp4a")) return "probably";
      }
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
      return Promise.resolve();
    };
    HTMLMediaElement.prototype.pause = function () {
      const state = stateFor(this);
      __tilefinchRequestMedia(this.__handle, 2, this.currentSrc, 0);
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
    // Detached shim nodes have own accessors. Materialize the native receiver
    // descriptors before copying so those accessors cannot shadow native state.
    globalThis.__tilefinchPrepareNativePrototype?.(connected);
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
  const focusEventTypes = new Set(["blur", "focus", "focusin", "focusout"]),
    pointerMoveEventTypes = new Set(["mousemove", "pointermove"]),
    pointerHoverEventTypes = new Set([
      "mouseenter",
      "mouseleave",
      "mouseover",
      "mouseout",
      "pointerenter",
      "pointerleave",
      "pointerover",
      "pointerout",
    ]);
  let focusObserverCount = 0,
    pointerMoveObserverCount = 0,
    pointerHoverObserverCount = 0;
  globalThis.__tilefinchEventObserverDelta = (type, delta) => {
    const key = String(type),
      amount = Number(delta || 0);
    if (focusEventTypes.has(key))
      focusObserverCount = Math.max(0, focusObserverCount + amount);
    if (pointerMoveEventTypes.has(key))
      pointerMoveObserverCount = Math.max(
        0,
        pointerMoveObserverCount + amount,
      );
    if (pointerHoverEventTypes.has(key))
      pointerHoverObserverCount = Math.max(
        0,
        pointerHoverObserverCount + amount,
      );
  };
  globalThis.__tilefinchFocusObserverDelta = (type, delta) =>
    globalThis.__tilefinchEventObserverDelta(type, delta);
  globalThis.__tilefinchFocusEventsObserved = () => focusObserverCount !== 0;
  globalThis.__tilefinchPointerMoveEventsObserved = () =>
    pointerMoveObserverCount !== 0;
  globalThis.__tilefinchPointerHoverEventsObserved = () =>
    pointerHoverObserverCount !== 0;
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
    if (signal !== null && signal !== undefined) {
      const inspectSignal = globalThis.__tilefinchAbortSignalBrand;
      if (typeof inspectSignal !== "function")
        throw new TypeError("AbortSignal required");
      if (inspectSignal(signal)) return;
    }
    if (!map.has(key)) map.set(key, []);
    const list = map.get(key);
    if (
      list.some(
        (item) => item.callback === callback && item.capture === capture,
      )
    )
      return;
    const item = {
      callback,
      capture,
      once,
      passive,
      signal,
      abort: null,
      active: true,
    };
    if (signal && typeof signal.addEventListener === "function") {
      item.abort = () =>
        globalThis.__tilefinchRemoveEventListener(map, key, callback, capture);
      globalThis.__tilefinchAddAbortAlgorithm(signal, item.abort);
    }
    list.push(item);
    globalThis.__tilefinchEventObserverDelta(key, 1);
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
    /* The DOM dispatch algorithm marks a removed listener before continuing
       the current event's cloned listener list.  Without this retained bit a
       listener removed by an earlier callback still runs once from our
       snapshot, which is observably wrong and can call stale teardown state. */
    item.active = false;
    globalThis.__tilefinchEventObserverDelta(type, -1);
    if (item.signal && item.abort)
      try {
        globalThis.__tilefinchRemoveAbortAlgorithm(item.signal, item.abort);
      } catch (_) {}
  };
  const invokeListenerItem = (
    map,
    list,
    target,
    event,
    item,
    errorObserver,
  ) => {
    /* DOM removes a once listener before calling it. This matters when the
       callback dispatches recursively or registers itself again: the new
       registration belongs to the next event and must not be removed by
       cleanup for the current invocation. */
    if (item.once)
      globalThis.__tilefinchRemoveEventListener(
        map,
        event.type,
        item.callback,
        item.capture,
      );
    event.__passive = item.passive;
    try {
      if (typeof item.callback === "function") {
        globalThis.__tilefinchRecordEventHandler();
        globalThis.__tilefinchRunTask(
          "event:" + String(event.type),
          item.callback,
          target,
          [event],
        );
      } else {
        /* Web IDL resolves an event-listener object's operation once per
           invocation. A getter may be stateful; reading it for both the type
           check and call can invoke a different value or side effect. */
        const handleEvent = item.callback.handleEvent;
        if (typeof handleEvent !== "function")
          throw new TypeError("event listener handleEvent is not callable");
        globalThis.__tilefinchRecordEventHandler();
        globalThis.__tilefinchRunTask(
          "event-object:" + String(event.type),
          handleEvent,
          item.callback,
          [event],
        );
      }
    } catch (error) {
      if (typeof errorObserver === "function")
        try { errorObserver(error, item, list); } catch (_) {}
      __tilefinchReportUncaught(error, "event " + event.type);
    } finally {
      event.__passive = false;
    }
  };
  globalThis.__tilefinchInvokeListenerList = (
    map,
    target,
    event,
    capture,
    errorObserver = null,
  ) => {
    const list = map.get(String(event.type)) || [];
    for (const item of [...list]) {
      if (!item.active || item.capture !== capture) continue;
      invokeListenerItem(map, list, target, event, item, errorObserver);
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
    event.__passive = false;
    event.__path = path;
  };
  globalThis.__tilefinchFinishEventDispatch = (event) => {
    event.currentTarget = null;
    event.eventPhase = Event.NONE;
    event.__dispatching = false;
    /* Propagation flags belong to one dispatch. Keep defaultPrevented, which
       remains observable when the same Event is dispatched again. */
    event.__stopped = false;
    event.__immediateStopped = false;
    event.__passive = false;
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
    globalThis.__tilefinchFinishEventDispatch(value);
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
      errorObserver = null,
    ) => {
      event.currentTarget = target;
      event.eventPhase = phase;
      globalThis.__tilefinchInvokeListenerList(
        mapFor(target),
        target,
        event,
        capture,
        errorObserver,
      );
    };
    /* A native event is one task and its dispatch is synchronous.  Running a
       microtask checkpoint between listeners exposes half-updated listener
       state and disagrees with dispatchEvent(); the host drains jobs after
       this complete dispatch returns. */
    globalThis.__tilefinchInvokeEventTargetCheckpointed = (
      target,
      event,
      phase,
      errorObserver,
      betweenPhases,
      complete,
    ) => {
      const map = mapFor(target);
      let completed = false;
      const finish = () => {
          if (completed) return;
          completed = true;
          if (typeof complete === "function") complete();
        };
      try {
        event.currentTarget = target;
        event.eventPhase = phase;
        globalThis.__tilefinchInvokeListenerList(
          map, target, event, true, errorObserver);
        if (!event.__immediateStopped && typeof betweenPhases === "function")
          betweenPhases();
        if (!event.__immediateStopped) {
          event.currentTarget = target;
          event.eventPhase = phase;
          globalThis.__tilefinchInvokeListenerList(
            map, target, event, false, errorObserver);
        }
      } finally {
        finish();
      }
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
      globalThis.__tilefinchFinishEventDispatch(value);
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
      globalThis.__tilefinchFinishEventDispatch(event);
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
    const documentWriteStates = new WeakMap(),
      stateFor = (receiver) => {
        if (!(receiver instanceof Document))
          throw new TypeError("Illegal invocation");
        let state = documentWriteStates.get(receiver);
        if (!state) {
          state = { buffer: "", pending: false };
          documentWriteStates.set(receiver, state);
        }
        return state;
      },
      flush = (receiver, state) => {
        state.pending = false;
        if (!state.buffer) return;
        const source = state.buffer;
        state.buffer = "";
        const container = receiver.createElement("div");
        container.innerHTML = source;
        const target = receiver.body || receiver.documentElement;
        if (!target) return;
        while (container.firstChild) target.appendChild(container.firstChild);
      };
    Object.defineProperties(Document.prototype, {
      write: {
        configurable: true,
        writable: true,
        value: function write(...parts) {
          const state = stateFor(this),
            addition = parts.map(String).join("");
          if (state.buffer.length + addition.length > 256 * 1024)
            throw new RangeError("document.write buffer exceeds bounded size");
          state.buffer += addition;
          if (!state.pending) {
            state.pending = true;
            queueMicrotask(() => flush(this, state));
          }
        },
      },
      writeln: {
        configurable: true,
        writable: true,
        value: function writeln(...parts) {
          this.write(...parts, "\n");
        },
      },
      close: {
        configurable: true,
        writable: true,
        value: function close() {
          const state = stateFor(this);
          if (state.pending) flush(this, state);
        },
      },
    });
  }
  Object.defineProperty(document, "scripts", {
    get() {
      return document.querySelectorAll("script");
    },
  });
  let cachedDocumentStyleSheets = null,
    cachedDocumentStyleSheetsGeneration = -1;
  Object.defineProperty(document, "styleSheets", {
    get() {
      const generation = Number(
        globalThis.__tilefinchStyleSheetGeneration?.() || 0,
      );
      if (
        cachedDocumentStyleSheets &&
        cachedDocumentStyleSheetsGeneration === generation
      )
        return cachedDocumentStyleSheets;
      const nodes = document.querySelectorAll("style,link"),
        sheets = [];
      for (let index = 0; index < nodes.length; index++) {
        if (nodes[index].hasAttribute("data-tilefinch-constructed")) continue;
        const sheet = nodes[index].sheet;
        if (sheet) sheets.push(sheet);
      }
      Object.defineProperty(sheets, "item", {
        value(index) { return this[index] || null; },
      });
      cachedDocumentStyleSheets = sheets;
      cachedDocumentStyleSheetsGeneration = generation;
      return cachedDocumentStyleSheets;
    },
  });
  const documentReferrer = String(
    globalThis.__tilefinchDocumentReferrer || "",
  );
  delete globalThis.__tilefinchDocumentReferrer;
  Object.defineProperty(Document.prototype, "referrer", {
    configurable: true,
    enumerable: true,
    get() {
      if (!(this instanceof Document)) throw new TypeError("Illegal invocation");
      return this === document ? documentReferrer : "";
    },
  });
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
  Object.defineProperty(globalThis, "frameElement", {
    configurable: false,
    enumerable: true,
    get() {
      return null;
    },
  });
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
  const orientationLandscape = innerWidth >= innerHeight,
    screenToken = {},
    screenOrientationToken = {};
  let screenHandler = null,
    screenOrientationHandler = null;
  class ScreenOrientation extends EventTarget {
    constructor(token) {
      super();
      if (token !== screenOrientationToken)
        throw new TypeError("Illegal constructor");
    }
    get type() {
      return orientationLandscape ? "landscape-primary" : "portrait-primary";
    }
    /* The PSP's natural orientation is landscape, so landscape-primary is
       unrotated. The diagnostic mobile profile likewise represents its
       natural portrait orientation rather than a rotated device. */
    get angle() { return 0; }
    get onchange() { return screenOrientationHandler; }
    set onchange(value) {
      screenOrientationHandler = typeof value === "function" ? value : null;
    }
    lock() {
      return Promise.reject(
        new DOMException("Screen orientation cannot be changed", "NotSupportedError"),
      );
    }
    unlock() {}
  }
  Object.defineProperty(ScreenOrientation.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "ScreenOrientation",
  });
  const screenOrientation = new ScreenOrientation(screenOrientationToken);
  class Screen extends EventTarget {
    constructor(token) {
      super();
      if (token !== screenToken) throw new TypeError("Illegal constructor");
    }
    get width() {
      return diagnosticMobileSafari ? innerWidth : tilefinchDeviceWidth;
    }
    get height() {
      return diagnosticMobileSafari ? innerHeight : tilefinchDeviceHeight;
    }
    get availWidth() { return this.width; }
    get availHeight() { return this.height; }
    get availLeft() { return 0; }
    get availTop() { return 0; }
    get colorDepth() { return 24; }
    get pixelDepth() { return 24; }
    get isExtended() { return false; }
    get onchange() { return screenHandler; }
    set onchange(value) {
      screenHandler = typeof value === "function" ? value : null;
    }
    get orientation() { return screenOrientation; }
  }
  Object.defineProperty(Screen.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Screen",
  });
  for (const constructor of [Screen, ScreenOrientation])
    for (const key of Reflect.ownKeys(constructor.prototype)) {
      if (key === "constructor" || key === Symbol.toStringTag) continue;
      const descriptor = Object.getOwnPropertyDescriptor(
        constructor.prototype, key);
      Object.defineProperty(constructor.prototype, key, {
        ...descriptor,
        enumerable: true,
      });
    }
  const screenValue = new Screen(screenToken);
  globalThis.Screen = Screen;
  globalThis.ScreenOrientation = ScreenOrientation;
  /* `screen` is a [Replaceable] Window attribute: assigning shadows the
     getter with an ordinary own data property for that realm. */
  Object.defineProperty(globalThis, "screen", {
    configurable: true,
    enumerable: true,
    get() { return screenValue; },
    set(value) {
      Object.defineProperty(globalThis, "screen", {
        configurable: true,
        enumerable: true,
        writable: true,
        value,
      });
    },
  });
  globalThis.scrollX = globalThis.pageXOffset = 0;
  globalThis.scrollY = globalThis.pageYOffset = 0;
  globalThis.__tilefinchFrameEvalTelemetry = "none";
  /* Cross-document postMessage crosses independent QuickJS runtimes. JSON is
     not a structured-clone transport: it drops undefined and non-finite
     numbers, destroys aliases/cycles, and turns typed arrays into records.
     Keep a small, versioned graph wire instead. The final 64 KiB native queue
     bound remains authoritative; the limits here stop hostile graphs before
     serialization creates proportional intermediate state. */
  const frameCloneMagic = "tilefinch-clone-v1",
    frameCloneObjectLimit = 512,
    frameClonePropertyLimit = 4096,
    frameCloneDepthLimit = 64,
    frameCloneBinaryLimit = 48 * 1024,
    frameCloneWireLimit = 64 * 1024,
    frameCloneViewTypes = Object.freeze({
      Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
      Int32Array, Uint32Array, Float32Array, Float64Array,
      BigInt64Array: globalThis.BigInt64Array,
      BigUint64Array: globalThis.BigUint64Array,
      DataView,
    }),
    frameCloneError = (message) =>
      new DOMException(String(message), "DataCloneError"),
    frameCloneBytesToBase64 = (bytes) => {
      if (bytes.byteLength > frameCloneBinaryLimit)
        throw frameCloneError("message binary data exceeds bounded size");
      return __tilefinchBase64EncodeBytes(bytes);
    },
    frameCloneBytesFromBase64 = (text) => {
      const binary = __tilefinchBase64DecodeString(String(text));
      if (binary.length > frameCloneBinaryLimit)
        throw frameCloneError("message binary data exceeds bounded size");
      const bytes = new TrustedUint8Array(binary.length);
      for (let at = 0; at < binary.length; at++)
        bytes[at] = trustedCharCodeAt(binary, at);
      return bytes;
    },
    encodeFrameMessage = (value) => {
      const memory = new Map(),
        nodes = [];
      let propertyCount = 0,
        diagnosticEvent = "";
      const encode = (item, depth) => {
        if (depth > frameCloneDepthLimit)
          throw frameCloneError("message nesting exceeds bounded depth");
        if (item === null || typeof item === "string" ||
            typeof item === "boolean") return item;
        if (item === undefined) return ["u"];
        if (typeof item === "number") {
          if (trustedNumberIsNaN(item)) return ["n", "nan"];
          if (item === Infinity) return ["n", "+inf"];
          if (item === -Infinity) return ["n", "-inf"];
          if (trustedObjectIs(item, -0)) return ["n", "-0"];
          return item;
        }
        if (typeof item === "bigint") return ["bi", String(item)];
        if (typeof item === "function" || typeof item === "symbol")
          throw frameCloneError("message contains an unsupported value");
        /* A Proxy is never structured-cloneable. Reject it before tag,
           prototype, or key inspection so page traps cannot run as a side
           effect of cross-realm postMessage. */
        if (globalThis.__tilefinchIsProxy(item))
          throw frameCloneError("Proxy values could not be cloned");
        if (memory.has(item)) return ["r", memory.get(item)];
        if (nodes.length >= frameCloneObjectLimit)
          throw frameCloneError("message object count exceeds bounded size");
        const id = nodes.length,
          tag = trustedObjectTag(item);
        memory.set(item, id);
        nodes.push(null);
        if (tag === "[object ArrayBuffer]") {
          nodes[id] = {
            t: "b",
            v: frameCloneBytesToBase64(new TrustedUint8Array(item)),
          };
          return ["r", id];
        }
        if (tag === "[object DataView]") {
          nodes[id] = {
            t: "v", c: "DataView", b: encode(item.buffer, depth + 1),
            o: item.byteOffset, l: item.byteLength,
          };
          return ["r", id];
        }
        if (trustedArrayBufferIsView(item)) {
          nodes[id] = {
            t: "v", c: tag.slice(8, -1),
            b: encode(item.buffer, depth + 1),
            o: item.byteOffset, l: item.length,
          };
          return ["r", id];
        }
        if (tag === "[object Date]") {
          nodes[id] = { t: "d", v: trustedDateTime(item) };
          return ["r", id];
        }
        if (tag === "[object RegExp]") {
          nodes[id] = {
            t: "x", s: trustedRegExpSource(item), f: trustedRegExpFlags(item),
          };
          return ["r", id];
        }
        if (tag === "[object Map]") {
          const entries = [];
          for (const pair of trustedMapEntries(item)) {
            if (++propertyCount > frameClonePropertyLimit)
              throw frameCloneError("message item count exceeds bounded size");
            entries.push([
              encode(pair[0], depth + 1), encode(pair[1], depth + 1),
            ]);
          }
          nodes[id] = { t: "m", e: entries };
          return ["r", id];
        }
        if (tag === "[object Set]") {
          const entries = [];
          for (const entry of trustedSetValues(item)) {
            if (++propertyCount > frameClonePropertyLimit)
              throw frameCloneError("message item count exceeds bounded size");
            entries.push(encode(entry, depth + 1));
          }
          nodes[id] = { t: "s", e: entries };
          return ["r", id];
        }
        if (tag.endsWith("Error]")) {
          nodes[id] = {
            t: "e", n: String(item.name || "Error"),
            m: String(item.message || ""), s: String(item.stack || ""),
          };
          return ["r", id];
        }
        const isArray = trustedArrayIsArray(item),
          prototype = trustedObjectPrototype(item);
        /* Structured clone serializes ordinary class instances as ordinary
           objects in the receiving realm. Requiring Object.prototype or a
           null prototype incorrectly rejected those instances even though
           their enumerable data is cloneable. Keep platform/native objects
           outside the bounded profile through the same prototype classifier
           used by the Worker clone path. */
        if (!isArray && !(tag === "[object Object]" &&
                          ordinaryClonePrototypeKind(prototype) !== 0))
          throw frameCloneError("message contains an unsupported object");
        if (isArray && item.length > frameClonePropertyLimit)
          throw frameCloneError("message array length exceeds bounded size");
        const properties = [],
          keys = trustedObjectKeys(item);
        if (propertyCount + keys.length > frameClonePropertyLimit)
          throw frameCloneError("message property count exceeds bounded size");
        propertyCount += keys.length;
        for (const key of keys) {
          const propertyValue = item[key],
            encoded = encode(propertyValue, depth + 1);
          properties.push([key, encoded]);
          if (depth === 0 && key === "event" &&
              typeof propertyValue === "string")
            diagnosticEvent = trustedStringSlice(propertyValue, 0, 64);
        }
        nodes[id] = {
          t: isArray ? "a" : "o",
          l: isArray ? item.length : 0,
          p: properties,
        };
        return ["r", id];
      };
      const root = encode(value, 0),
        envelope = {
          tilefinchClone: frameCloneMagic,
          event: diagnosticEvent,
          root,
          nodes,
        },
        wire = trustedJSONStringify(envelope);
      if (wire.length > frameCloneWireLimit)
        throw frameCloneError("message exceeds bounded wire size");
      return wire;
    },
    decodeFrameMessage = (wire) => {
      const text = String(wire);
      if (text.length > frameCloneWireLimit)
        throw frameCloneError("message exceeds bounded wire size");
      const envelope = trustedJSONParse(text);
      if (!envelope || envelope.tilefinchClone !== frameCloneMagic ||
          !trustedArrayIsArray(envelope.nodes))
        return envelope;
      if (envelope.nodes.length > frameCloneObjectLimit)
        throw frameCloneError("message object count exceeds bounded size");
      const nodes = new Array(envelope.nodes.length),
        source = envelope.nodes,
        errorTypes = {
          Error, EvalError, RangeError, ReferenceError, SyntaxError, TypeError,
          URIError,
        };
      let decodedItemCount = 0;
      for (let at = 0; at < source.length; at++) {
        const node = source[at];
        if (!node || typeof node.t !== "string")
          throw frameCloneError("message contains an invalid node");
        if (node.t === "a") {
          const length = Number(node.l);
          if (!trustedNumberIsInteger(length) || length < 0 ||
              length > frameClonePropertyLimit)
            throw frameCloneError("message array length exceeds bounded size");
          nodes[at] = new Array(length);
        }
        else if (node.t === "o") nodes[at] = {};
        else if (node.t === "b")
          nodes[at] = frameCloneBytesFromBase64(node.v).buffer;
        else if (node.t === "d") nodes[at] = new Date(Number(node.v));
        else if (node.t === "x") nodes[at] = new RegExp(node.s, node.f);
        else if (node.t === "m") nodes[at] = new Map();
        else if (node.t === "s") nodes[at] = new Set();
        else if (node.t === "e") {
          const ErrorType = errorTypes[String(node.n)] || Error;
          nodes[at] = new ErrorType(String(node.m || ""));
          try { nodes[at].stack = String(node.s || ""); } catch (_) {}
        } else if (node.t !== "v")
          throw frameCloneError("message contains an invalid node type");
      }
      const decode = (item) => {
        if (!trustedArrayIsArray(item)) return item;
        if (item[0] === "u") return undefined;
        if (item[0] === "bi") return BigInt(item[1]);
        if (item[0] === "n") {
          if (item[1] === "nan") return NaN;
          if (item[1] === "+inf") return Infinity;
          if (item[1] === "-inf") return -Infinity;
          if (item[1] === "-0") return -0;
        }
        if (item[0] !== "r" || !trustedNumberIsInteger(item[1]) ||
            item[1] < 0 || item[1] >= nodes.length)
          throw frameCloneError("message contains an invalid reference");
        return nodes[item[1]];
      };
      for (let at = 0; at < source.length; at++) {
        const node = source[at];
        if (node.t === "v") {
          const buffer = decode(node.b),
            constructor = frameCloneViewTypes[node.c];
          if (!(buffer instanceof ArrayBuffer) || typeof constructor !== "function")
            throw frameCloneError("message contains an invalid view");
          nodes[at] = node.c === "DataView"
            ? new constructor(buffer, Number(node.o), Number(node.l))
            : new constructor(buffer, Number(node.o), Number(node.l));
        }
      }
      for (let at = 0; at < source.length; at++) {
        const node = source[at], target = nodes[at];
        if (node.t === "a" || node.t === "o") {
          if (!trustedArrayIsArray(node.p) ||
              node.p.length > frameClonePropertyLimit ||
              decodedItemCount > frameClonePropertyLimit - node.p.length)
            throw frameCloneError("message contains invalid properties");
          decodedItemCount += node.p.length;
          for (const pair of node.p) {
            if (!trustedArrayIsArray(pair) || pair.length !== 2)
              throw frameCloneError("message contains an invalid property");
            trustedDefineProperty(target, String(pair[0]), {
              configurable: true, enumerable: true, writable: true,
              value: decode(pair[1]),
            });
          }
        } else if (node.t === "m") {
          if (!trustedArrayIsArray(node.e) ||
              decodedItemCount > frameClonePropertyLimit - node.e.length)
            throw frameCloneError("message contains invalid map entries");
          decodedItemCount += node.e.length;
          for (const pair of node.e) {
            if (!trustedArrayIsArray(pair) || pair.length !== 2)
              throw frameCloneError("message contains an invalid map entry");
            trustedMapSet(target, decode(pair[0]), decode(pair[1]));
          }
        } else if (node.t === "s") {
          if (!trustedArrayIsArray(node.e) ||
              decodedItemCount > frameClonePropertyLimit - node.e.length)
            throw frameCloneError("message contains invalid set entries");
          decodedItemCount += node.e.length;
          for (const entry of node.e) trustedSetAdd(target, decode(entry));
        }
      }
      return decode(envelope.root);
    };
  const normalizePostMessageTarget = (value, senderOrigin) => {
      /* HTML exposes both the legacy postMessage(message, targetOrigin)
         signature and the options-dictionary overload.  Resolve the overload
         here so top-level, parent and nested-frame senders all observe the
         same single targetOrigin getter. Transferables remain outside the
         bounded frame-message profile; an options object with no transfer
         list is still fully useful and is the form used by current web apps. */
      if (value !== null && typeof value === "object")
        value = value.targetOrigin;
      if (value === undefined || value === "/") return String(senderOrigin);
      const text = String(value);
      if (text === "*") return text;
      if (!/^[A-Za-z][A-Za-z0-9+.-]*:/.test(text))
        throw new DOMException("Invalid target origin", "SyntaxError");
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
  Object.defineProperty(globalThis, "__tilefinchPostParentMessage", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(data, targetOrigin = "/") {
      const normalizedTarget = normalizePostMessageTarget(
        targetOrigin,
        location.origin,
      );
      const wire = encodeFrameMessage(data);
      __tilefinchPostMessage(
        0,
        wire,
        normalizedTarget,
        globalThis.__tilefinchActiveTaskKind,
        globalThis.__tilefinchActiveTaskSequence,
      );
    },
  });
  Object.defineProperty(globalThis, "__tilefinchPostTopMessage", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(data, targetOrigin = "/") {
      const normalizedTarget = normalizePostMessageTarget(
        targetOrigin,
        location.origin,
      );
      const wire = encodeFrameMessage(data);
      __tilefinchPostMessage(
        -1,
        wire,
        normalizedTarget,
        globalThis.__tilefinchActiveTaskKind,
        globalThis.__tilefinchActiveTaskSequence,
      );
    },
  });
  /* The nested-frame WindowProxy subsystem lives in frames.js. It is a
     first-use-free eager module installed here with this module's private
     helpers; the installer is removed so no privileged bridge remains on
     the global object. */
  const frames = globalThis.__tilefinchInstallFrames({
    wrap,
    encodeFrameMessage,
    decodeFrameMessage,
    trustedStringLower,
    trustedCharCodeAt,
    trustedStringSlice,
    blobForURL: (url) => blobURLs.get(url),
    blobBytes,
    normalizePostMessageTarget,
    location,
  });
  delete globalThis.__tilefinchInstallFrames;
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
        setSynchronizedLocation(next.href);
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
    setSynchronizedLocation(url);
    historyLength++;
    const event = new Event("hashchange");
    event.oldURL = String(oldURL);
    event.newURL = location.href;
    globalThis.dispatchEvent(event);
  };
  globalThis.__tilefinchRestoreSameDocument = (url, oldURL) => {
    setSynchronizedLocation(url);
    const pop = new Event("popstate");
    pop.state = historyState;
    globalThis.dispatchEvent(pop);
    const event = new Event("hashchange");
    event.oldURL = String(oldURL);
    event.newURL = location.href;
    globalThis.dispatchEvent(event);
  };
  globalThis.addEventListener = (type, callback, options = false) =>
    EventTarget.prototype.addEventListener.call(
      globalThis, type, callback, options);
  globalThis.removeEventListener = (type, callback, options = false) =>
    EventTarget.prototype.removeEventListener.call(
      globalThis, type, callback, options);
  const windowMessageHandlers = new Map(),
    setWindowMessageHandler = (type, value) => {
      let slot = windowMessageHandlers.get(type);
      if (!slot) {
        slot = { value: null, wrapper: null };
        windowMessageHandlers.set(type, slot);
      }
      const callback = typeof value === "function" ? value : null;
      if (callback === slot.value) return;
      slot.value = callback;
      if (callback !== null && slot.wrapper === null) {
        /* Event-handler attributes occupy one stable position in the shared
           listener list. Replacing a handler preserves that position; a
           null/re-add pair registers a new position at the end. */
        slot.wrapper = function (event) {
          const current = slot.value;
          if (typeof current === "function")
            return trustedFunctionApply(current, this, [event]);
        };
        EventTarget.prototype.addEventListener.call(
          globalThis, type, slot.wrapper, false);
      } else if (callback === null && slot.wrapper !== null) {
        EventTarget.prototype.removeEventListener.call(
          globalThis, type, slot.wrapper, false);
        slot.wrapper = null;
      }
    };
  for (const type of ["message", "messageerror"])
    Object.defineProperty(globalThis, "on" + type, {
      configurable: true,
      enumerable: true,
      get() { return windowMessageHandlers.get(type)?.value || null; },
      set(value) { setWindowMessageHandler(type, value); },
    });
  globalThis.__tilefinchInvokeWindowEvent = (event, capture) => {
    event.currentTarget = globalThis;
    event.eventPhase = globalThis === event.target ? 2 : capture ? 1 : 3;
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
    globalThis.__tilefinchFinishEventDispatch(value);
    __tilefinchRecordEvent();
    return !value.defaultPrevented;
  };
  globalThis.__tilefinchDispatchWindowEventCheckpointed = (event) => {
    const value = event,
      path = [globalThis];
    globalThis.__tilefinchPrepareEvent(value, globalThis, path);
    const finish = () => {
        globalThis.__tilefinchFinishEventDispatch(value);
        __tilefinchRecordEvent();
      },
      invokeHandler = () => {
        if (windowMessageHandlers.has(String(value.type))) return;
        const handler = globalThis["on" + value.type];
        if (typeof handler !== "function") return;
        value.currentTarget = globalThis;
        value.eventPhase = Event.AT_TARGET;
        try {
          globalThis.__tilefinchRecordEventHandler();
          globalThis.__tilefinchRunTask(
            "window-handler:" + String(value.type),
            handler,
            globalThis,
            [value],
          );
        } catch (error) {
          __tilefinchReportUncaught(error, "window event " + value.type);
        }
      };
    if (value.__stopped) finish();
    else globalThis.__tilefinchInvokeEventTargetCheckpointed(
      globalThis,
      value,
      Event.AT_TARGET,
      null,
      invokeHandler,
      finish,
    );
  };
  {
    /* Navigator is a platform object, not a mutable record. Keeping its
       values in closures preserves Tilefinch's honest device identity while
       matching the prototype/brand surface feature-detection code expects. */
    const navigatorToken = {},
      uaDataToken = {},
      gpuToken = {},
      wgslLanguageFeaturesToken = {},
      pluginArrayToken = {},
      mimeTypeArrayToken = {},
      pluginToken = {},
      mimeTypeToken = {},
      languages = Object.freeze(["en-US", "en"]),
      brands = Object.freeze([
        Object.freeze({
          brand: String(globalThis.__tilefinchBrowserBrand),
          version: String(globalThis.__tilefinchBrowserBrandVersion),
        }),
        Object.freeze({ brand: "Not.A/Brand", version: "99" }),
      ]),
      fullVersionList = Object.freeze([
        Object.freeze({
          brand: String(globalThis.__tilefinchBrowserBrand),
          version: String(globalThis.__tilefinchBrowserFullVersion),
        }),
        Object.freeze({ brand: "Not.A/Brand", version: "99.0.0.0" }),
      ]);
    const queuePlatformTask = (operation, quotaLabel = "Platform") =>
      new Promise((resolve, reject) => {
        const task = setTimeout(() => {
          try { resolve(operation()); }
          catch (error) { reject(error); }
        }, 0);
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
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
        return 0;
      }
      has() {
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
        return false;
      }
      entries() {
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
        return [][Symbol.iterator]();
      }
      keys() {
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
        return [][Symbol.iterator]();
      }
      values() {
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
        return [][Symbol.iterator]();
      }
      forEach() {
        if (!(this instanceof WGSLLanguageFeatures))
          throw new TypeError("Illegal invocation");
      }
      [Symbol.iterator]() { return this.values(); }
    }
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
        if (!(this instanceof GPU)) throw new TypeError("Illegal invocation");
        return wgslLanguageFeatures;
      }
      getPreferredCanvasFormat() {
        if (!(this instanceof GPU)) throw new TypeError("Illegal invocation");
        return "bgra8unorm";
      }
      requestAdapter(options = {}) {
        if (!(this instanceof GPU)) throw new TypeError("Illegal invocation");
        /* Even without an admitted WebGPU adapter, Web IDL still converts the
           complete options dictionary. Sites use those ordered conversions
           to distinguish an implementation from a placeholder. Conversion
           failures reject this promise; an unavailable adapter resolves to
           null from the WebGPU task timeline. */
        try {
          if (options === null || options === undefined) options = {};
          const featureValue = options.featureLevel,
            featureLevel = featureValue === undefined
              ? "core" : String(featureValue),
            forceFallbackAdapter = Boolean(options.forceFallbackAdapter),
            powerValue = options.powerPreference,
            powerPreference = powerValue === undefined
              ? undefined : String(powerValue),
            xrCompatible = Boolean(options.xrCompatible);
          if (powerPreference !== undefined
              && powerPreference !== "low-power"
              && powerPreference !== "high-performance")
            throw new TypeError("Invalid GPU powerPreference");
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
    class PluginArray {
      constructor() {
        if (arguments[0] !== pluginArrayToken)
          throw new TypeError("Illegal constructor");
      }
      refresh() {}
      get length() { return 0; }
      item() { return null; }
      namedItem() { return null; }
    }
    class MimeTypeArray {
      constructor() {
        if (arguments[0] !== mimeTypeArrayToken)
          throw new TypeError("Illegal constructor");
      }
      get length() { return 0; }
      item() { return null; }
      namedItem() { return null; }
    }
    /* These constructors are required to exist even when the browser has no
       plug-ins or PDF viewer, but no page-created instances are admitted. */
    class Plugin {
      constructor() {
        if (arguments[0] !== pluginToken)
          throw new TypeError("Illegal constructor");
      }
    }
    class MimeType {
      constructor() {
        if (arguments[0] !== mimeTypeToken)
          throw new TypeError("Illegal constructor");
      }
    }
    for (const [constructor, tag] of [
      [PluginArray, "PluginArray"],
      [MimeTypeArray, "MimeTypeArray"],
      [Plugin, "Plugin"],
      [MimeType, "MimeType"],
    ])
      Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
        configurable: true,
        value: tag,
      });
    const plugins = new PluginArray(pluginArrayToken),
      mimeTypes = new MimeTypeArray(mimeTypeArrayToken);
    class NavigatorUAData {
      constructor() {
        if (arguments[0] !== uaDataToken)
          throw new TypeError("Illegal constructor");
      }
      get brands() {
        if (!(this instanceof NavigatorUAData))
          throw new TypeError("Illegal invocation");
        return brands;
      }
      get mobile() {
        if (!(this instanceof NavigatorUAData))
          throw new TypeError("Illegal invocation");
        return true;
      }
      get platform() {
        if (!(this instanceof NavigatorUAData))
          throw new TypeError("Illegal invocation");
        return "PlayStation Portable";
      }
      getHighEntropyValues(hints) {
        if (!(this instanceof NavigatorUAData))
          throw new TypeError("Illegal invocation");
        /* Web IDL converts the complete sequence before the operation runs.
           Keep the observable iterator/string order while bounding hostile
           iterables independently of the number of hints we recognize. */
        const requested = [];
        for (const hint of hints) {
          if (requested.length >= 64)
            throw new RangeError("UA client hint quota exceeded");
          requested.push(String(hint));
        }
        const all = Object.assign(Object.create(null), {
            architecture: "MIPS",
            bitness: "32",
            formFactors: Object.freeze(["Mobile"]),
            model: "PSP-3000",
            platform: "PlayStation Portable",
            platformVersion: "6.61",
            uaFullVersion: String(globalThis.__tilefinchBrowserFullVersion),
            fullVersionList,
            wow64: false,
          }),
          value = {
            brands,
            mobile: true,
            platform: "PlayStation Portable",
          };
        for (const hint of requested)
          if (Object.prototype.hasOwnProperty.call(all, hint))
            value[hint] = all[hint];
        return Promise.resolve(value);
      }
      toJSON() {
        if (!(this instanceof NavigatorUAData))
          throw new TypeError("Illegal invocation");
        return { brands, mobile: true, platform: "PlayStation Portable" };
      }
    }
    Object.defineProperty(NavigatorUAData.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "NavigatorUAData",
    });
    const uaData = diagnosticMobileSafari
      ? undefined
      : new NavigatorUAData(uaDataToken),
      values = diagnosticMobileSafari
        ? {
            userAgent:
              "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1",
            platform: "iPhone",
            maxTouchPoints: 5,
            hardwareConcurrency: 6,
            deviceMemory: undefined,
          }
        : {
            userAgent: String(globalThis.__tilefinchBrowserUserAgent),
            platform: "PSP",
            maxTouchPoints: 0,
            hardwareConcurrency: 1,
            deviceMemory: 0.25,
          };
    class Navigator {
      constructor() {
        if (arguments[0] !== navigatorToken)
          throw new TypeError("Illegal constructor");
      }
      get userAgent() { return values.userAgent; }
      get appCodeName() { return "Mozilla"; }
      get appName() { return "Netscape"; }
      get appVersion() {
        return values.userAgent.startsWith("Mozilla/")
          ? values.userAgent.slice(8)
          : "";
      }
      get language() { return languages[0]; }
      get languages() { return languages; }
      get platform() { return values.platform; }
      get product() { return "Gecko"; }
      get productSub() { return "20030107"; }
      get vendor() { return ""; }
      get vendorSub() { return ""; }
      get maxTouchPoints() { return values.maxTouchPoints; }
      get cookieEnabled() { return true; }
      get onLine() { return true; }
      get hardwareConcurrency() { return values.hardwareConcurrency; }
      get deviceMemory() { return values.deviceMemory; }
      get userAgentData() { return uaData; }
      get gpu() { return globalThis.isSecureContext ? gpu : undefined; }
      get plugins() { return plugins; }
      get mimeTypes() { return mimeTypes; }
      get pdfViewerEnabled() { return false; }
      get webdriver() { return false; }
      javaEnabled() { return false; }
    }
    Object.defineProperty(Navigator.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "Navigator",
    });
    for (const constructor of [
      Navigator,
      NavigatorUAData,
      GPU,
      WGSLLanguageFeatures,
      PluginArray,
      MimeTypeArray,
      Plugin,
      MimeType,
    ])
      for (const key of Reflect.ownKeys(constructor.prototype)) {
        const descriptor = Object.getOwnPropertyDescriptor(
          constructor.prototype,
          key,
        );
        if (descriptor && key !== "constructor" && key !== Symbol.toStringTag)
          Object.defineProperty(constructor.prototype, key, {
            ...descriptor,
            enumerable: true,
          });
      }
    globalThis.Navigator = Navigator;
    globalThis.NavigatorUAData = NavigatorUAData;
    globalThis.GPU = GPU;
    globalThis.WGSLLanguageFeatures = WGSLLanguageFeatures;
    globalThis.PluginArray = PluginArray;
    globalThis.MimeTypeArray = MimeTypeArray;
    globalThis.Plugin = Plugin;
    globalThis.MimeType = MimeType;
    globalThis.navigator = new Navigator(navigatorToken);
    Object.defineProperty(globalThis, "clientInformation", {
      configurable: true,
      enumerable: true,
      get() { return globalThis.navigator; },
    });
    const storageManagerToken = {};
    class StorageManager {
      constructor() {
        if (arguments[0] !== storageManagerToken)
          throw new TypeError("Illegal constructor");
      }
      estimate() {
        if (!(this instanceof StorageManager))
          throw new TypeError("Illegal invocation");
        return queuePlatformTask(() => {
          const values = globalThis.__tilefinchStorageEstimate();
          if (!values)
            throw new TypeError("Storage is unavailable for this origin");
          return {
            usage: Math.max(0, Number(values[0]) || 0),
            quota: Math.max(0, Number(values[1]) || 0),
          };
        }, "Storage");
      }
      persisted() {
        if (!(this instanceof StorageManager))
          throw new TypeError("Illegal invocation");
        return queuePlatformTask(() => {
          if (!globalThis.__tilefinchStorageAvailable())
            throw new TypeError("Storage is unavailable for this origin");
          return false;
        }, "Storage");
      }
      persist() {
        if (!(this instanceof StorageManager))
          throw new TypeError("Illegal invocation");
        return queuePlatformTask(() => {
          if (!globalThis.__tilefinchStorageAvailable())
            throw new TypeError("Storage is unavailable for this origin");
          return false;
        }, "Storage");
      }
    }
    Object.defineProperty(StorageManager.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "StorageManager",
    });
    for (const key of ["estimate", "persisted", "persist"])
      Object.defineProperty(StorageManager.prototype, key, {
        ...Object.getOwnPropertyDescriptor(StorageManager.prototype, key),
        enumerable: true,
      });
    const storageManager = new StorageManager(storageManagerToken);
    Object.defineProperty(Navigator.prototype, "storage", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        return storageManager;
      },
    });
    globalThis.StorageManager = StorageManager;

    /* The Keyboard Map API is exposed by Chromium even when the platform
       cannot disclose a layout: getLayoutMap() resolves to a stable empty
       map.  That is also the honest PSP result.  Instantiate the two platform
       objects only if a page asks for them so ordinary navigation pays no
       object-allocation cost. */
    const keyboardToken = {},
      keyboardLayoutMapToken = {},
      emptyKeyboardMap = new Map();
    let keyboard = null,
      keyboardLayoutMap = null;
    class KeyboardLayoutMap {
      constructor() {
        if (arguments[0] !== keyboardLayoutMapToken)
          throw new TypeError("Illegal constructor");
      }
      get size() {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return 0;
      }
      entries() {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return emptyKeyboardMap.entries();
      }
      forEach(callback, thisArg) {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        if (typeof callback !== "function")
          throw new TypeError("callback must be a function");
        emptyKeyboardMap.forEach(callback, thisArg);
      }
      get(key) {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return emptyKeyboardMap.get(String(key));
      }
      has(key) {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return emptyKeyboardMap.has(String(key));
      }
      keys() {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return emptyKeyboardMap.keys();
      }
      values() {
        if (!(this instanceof KeyboardLayoutMap))
          throw new TypeError("Illegal invocation");
        return emptyKeyboardMap.values();
      }
      [Symbol.iterator]() { return this.entries(); }
    }
    class Keyboard {
      constructor() {
        if (arguments[0] !== keyboardToken)
          throw new TypeError("Illegal constructor");
      }
      getLayoutMap() {
        if (!(this instanceof Keyboard))
          throw new TypeError("Illegal invocation");
        return queuePlatformTask(() => {
          if (keyboardLayoutMap === null)
            keyboardLayoutMap = new KeyboardLayoutMap(keyboardLayoutMapToken);
          return keyboardLayoutMap;
        });
      }
      lock() {
        if (!(this instanceof Keyboard))
          throw new TypeError("Illegal invocation");
        if (!document.fullscreenElement)
          return Promise.reject(
            new DOMException(
              "Keyboard lock requires fullscreen",
              "InvalidStateError",
            ),
          );
        return Promise.resolve();
      }
      unlock() {
        if (!(this instanceof Keyboard))
          throw new TypeError("Illegal invocation");
      }
    }
    for (const [constructor, tag] of [
      [Keyboard, "Keyboard"],
      [KeyboardLayoutMap, "KeyboardLayoutMap"],
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
    Object.defineProperty(Navigator.prototype, "keyboard", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        if (keyboard === null) keyboard = new Keyboard(keyboardToken);
        return keyboard;
      },
    });
    globalThis.Keyboard = Keyboard;
    globalThis.KeyboardLayoutMap = KeyboardLayoutMap;

  }
  {
    /* One fixed built-in controller is exposed only while the user has
       explicitly handed page input to the document. Keep the Gamepad graph
       stable and mutate bounded closure state; only the sequence snapshot
       returned by getGamepads() is fresh, as Web IDL requires. */
    const buttonValues = new Array(17).fill(0),
      axisValues = [0, 0, 0, 0],
      buttons = buttonValues.map((_, index) =>
        Object.freeze({
          get pressed() {
            return buttonValues[index] !== 0;
          },
          get touched() {
            return buttonValues[index] !== 0;
          },
          get value() {
            return buttonValues[index];
          },
          [Symbol.toStringTag]: "GamepadButton",
        }),
      ),
      axes = [];
    for (let index = 0; index < 4; index++)
      Object.defineProperty(axes, index, {
        enumerable: true,
        get: () => axisValues[index],
      });
    Object.defineProperty(axes, "length", { value: 4 });
    Object.freeze(axes);
    Object.freeze(buttons);
    let connected = false,
      hasGamepadGesture = false,
      timestamp = 0,
      currentButtonBits = 0;
    /* PSP analog input can change on every sampled frame. Keep that hot
       publication path allocation-free: defining this helper inside the
       host callback created a fresh closure for every nub sample, and
       rewriting all 17 buttons made axis-only movement pay unrelated work. */
    const normalizeGamepadAxis = (value) =>
      Math.max(-1, Math.min(1, Number(value) / 32767));
    const gamepad = Object.freeze({
        id: "PSP Built-in Controller",
        index: 0,
        mapping: "standard",
        get connected() {
          return connected;
        },
        get timestamp() {
          return timestamp;
        },
        axes,
        buttons,
        vibrationActuator: null,
        [Symbol.toStringTag]: "Gamepad",
      });
    class GamepadEvent {
      constructor(type, init = {}) {
        /* Event is installed by the later compatibility bootstrap. Controller
           publication begins only after the complete bootstrap, so resolve
           it at construction time rather than making platform.js depend on
           source ordering. */
        if (typeof globalThis.Event === "function") {
          const event = new globalThis.Event(type, init);
          Object.setPrototypeOf(GamepadEvent.prototype, Event.prototype);
          Object.setPrototypeOf(event, GamepadEvent.prototype);
          event.gamepad = init.gamepad || null;
          return event;
        }
        this.type = String(type);
        this.gamepad = init.gamepad || null;
      }
    }
    globalThis.GamepadEvent = GamepadEvent;
    Object.defineProperty(Navigator.prototype, "getGamepads", {
      configurable: true,
      enumerable: true,
      writable: true,
      value: function getGamepads() {
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        /* Before an explicit page-controls handoff, exposing even a null slot
           reveals the presence of the built-in controller. Afterwards retain
           its assigned index across disconnects, but return a new sequence so
           author mutation cannot poison later snapshots. */
        if (!hasGamepadGesture) return [];
        return [connected ? gamepad : null];
      },
    });
    globalThis.__tilefinchUpdateGamepad = (
      nextConnected,
      buttonBits,
      axisX,
      axisY,
      nextTimestamp,
    ) => {
      nextConnected = !!nextConnected;
      if (nextConnected) hasGamepadGesture = true;
      buttonBits = Number(buttonBits) >>> 0;
      if (buttonBits !== currentButtonBits) {
        currentButtonBits = buttonBits;
        for (let index = 0; index < 17; index++)
          buttonValues[index] = buttonBits & (1 << index) ? 1 : 0;
      }
      axisValues[0] = normalizeGamepadAxis(axisX);
      axisValues[1] = normalizeGamepadAxis(axisY);
      axisValues[2] = 0;
      axisValues[3] = 0;
      timestamp = Math.max(0, Number(nextTimestamp) || 0);
      if (connected === nextConnected) return;
      connected = nextConnected;
      const type = connected ? "gamepadconnected" : "gamepaddisconnected";
      queueMicrotask(() => dispatchEvent(new GamepadEvent(type, { gamepad })));
    };
  }
  {
    const clipboardToken = {};
    class Clipboard extends EventTarget {
      constructor() {
        super();
        if (arguments[0] !== clipboardToken)
          throw new TypeError("Illegal constructor");
      }
      writeText(value) {
      try {
        globalThis.__tilefinchClipboardWrite(value);
        return Promise.resolve();
      } catch (error) {
        return Promise.reject(error);
      }
      }
      readText() {
        return Promise.resolve(globalThis.__tilefinchClipboardStats.text);
      }
    }
    Object.defineProperty(Clipboard.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "Clipboard",
    });
    for (const key of ["writeText", "readText"])
      Object.defineProperty(Clipboard.prototype, key, {
        ...Object.getOwnPropertyDescriptor(Clipboard.prototype, key),
        enumerable: true,
      });
    const clipboard = new Clipboard(clipboardToken);
    Object.defineProperty(Navigator.prototype, "clipboard", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof Navigator))
          throw new TypeError("Illegal invocation");
        return clipboard;
      },
    });
    globalThis.Clipboard = Clipboard;
  }
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
      const text = String(input);
      if (text.length > 1024 * 1024)
        throw new RangeError("base64 input exceeds bounded size");
      try {
        return __tilefinchBase64DecodeString(text);
      } catch (error) {
        if (
          !(error instanceof TypeError) ||
          String(error.message || error) !== "invalid base64 input"
        )
          throw error;
        const bad = text.search(/[^A-Za-z0-9+/=\t\n\f\r ]/);
        invalidBase64(
          "atob length=" +
            text.length +
            " bad-index=" +
            bad +
            " bad-code=" +
            (bad < 0 ? -1 : text.charCodeAt(bad)),
        );
      }
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
    "flex",
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
  const computedStyleToken = {},
    computedStyleStates = new WeakMap(),
    computedStyleDefaults = {
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
    },
    computedStyleProperties = Array.from(new Set([
      ...Object.keys(computedStyleDefaults).map(__tilefinchCssName),
      ...sparseComputedProperties,
    ])).sort(),
    computedStylePropertyNames = new Set(computedStyleProperties),
    computedStyleState = (value) => {
      const state = computedStyleStates.get(value);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    },
    computedStyleRead = (state, name) => {
      name = __tilefinchCssName(name);
      const inline = state.node.style.getPropertyValue(name),
        computed = state.connected
          ? __tilefinchComputedStyleGet(
              state.node.__handle, name, state.pseudo)
          : "";
      /* The host resolves custom properties across inline declarations,
         the author cascade, inheritance, and var() substitution. An empty
         result is meaningful for a guaranteed-invalid custom value. */
      if (name.startsWith("--")) return computed;
      return computedSparseValue(
        state.node, name,
        computed || inline || computedStyleDefaults[name] || "");
    },
    computedStyleReadonly = () => {
      throw new DOMException(
        "Computed styles are read-only", "NoModificationAllowedError");
    };
  class CSSStyleDeclaration {
    constructor(token) {
      if (token !== computedStyleToken)
        throw new TypeError("Illegal constructor");
    }
    get cssText() { return ""; }
    set cssText(_value) { computedStyleReadonly(); }
    get length() { computedStyleState(this); return computedStyleProperties.length; }
    get parentRule() { computedStyleState(this); return null; }
    item(index) {
      computedStyleState(this);
      index = Number(index) >>> 0;
      return computedStyleProperties[index] || "";
    }
    getPropertyValue(name) {
      return computedStyleRead(computedStyleState(this), name);
    }
    getPropertyPriority(_name) { computedStyleState(this); return ""; }
    [Symbol.iterator]() {
      computedStyleState(this);
      return computedStyleProperties[Symbol.iterator]();
    }
    setProperty(_name, _value, _priority) { computedStyleReadonly(); }
    removeProperty(_name) { computedStyleReadonly(); }
  }
  Object.defineProperty(CSSStyleDeclaration.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "CSSStyleDeclaration",
  });
  globalThis.CSSStyleDeclaration = CSSStyleDeclaration;
  globalThis.getComputedStyle = (node, pseudo = null) => {
    if (!node || !node.style)
      throw new TypeError("getComputedStyle requires an Element");
    const target = new CSSStyleDeclaration(computedStyleToken),
      state = {
        node,
        connected: Number.isInteger(node.__handle) && node.__handle > 0,
        pseudo: pseudo === null ? "" : String(pseudo),
      };
    computedStyleStates.set(target, state);
    const proxy = new Proxy(
      target,
      {
        get(object, name, receiver) {
          if (typeof name === "string" && /^\d+$/.test(name))
            return object.item(Number(name));
          return name in object
            ? Reflect.get(object, name, receiver)
            : typeof name === "string"
              ? object.getPropertyValue(name) : undefined;
        },
        set() { computedStyleReadonly(); },
        deleteProperty() { computedStyleReadonly(); },
        has(object, name) {
          if (typeof name === "string" && /^\d+$/.test(name))
            return Number(name) < computedStyleProperties.length;
          return (
            name in object ||
            (typeof name === "string" &&
              computedStylePropertyNames.has(__tilefinchCssName(name)))
          );
        },
        ownKeys() {
          return computedStyleProperties.map((_name, index) => String(index));
        },
        getOwnPropertyDescriptor(_object, name) {
          if (typeof name === "string" && /^\d+$/.test(name)
              && Number(name) < computedStyleProperties.length) {
            return {
              configurable: true,
              enumerable: true,
              writable: false,
              value: computedStyleProperties[Number(name)],
            };
          }
          return undefined;
        },
      },
    );
    computedStyleStates.set(proxy, state);
    return proxy;
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
        if (name === "prefers-reduced-motion") return value === "reduce";
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
        if (typeof globalThis.Event === "function") {
          const event = new globalThis.Event(type, init);
          Object.setPrototypeOf(
            MediaQueryListEvent.prototype,
            globalThis.Event.prototype,
          );
          Object.setPrototypeOf(event, MediaQueryListEvent.prototype);
          event.media = String(init.media ?? "");
          event.matches = !!init.matches;
          return event;
        }
        this.type = String(type);
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
  const performanceEntryState = new WeakMap(),
    requirePerformanceEntryState = (value) => {
      const state = performanceEntryState.get(value);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    };
  class PerformanceEntry {
    constructor(name, type, startTime = 0, duration = 0) {
      performanceEntryState.set(this, {
        base: [String(name), String(type), Number(startTime), Number(duration)],
        resource: null,
        navigation: null,
        detail: null,
      });
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
  const performanceMeasureToken = {};
  class PerformanceMark extends PerformanceEntry {
    constructor(name, options = {}) {
      options = Object(options);
      const start = options.startTime === undefined
        ? __tilefinchPerformanceNow(4) : Number(options.startTime);
      if (!Number.isFinite(start) || start < 0)
        throw new TypeError("startTime must be a finite nonnegative number");
      super(String(name), "mark", start, 0);
      requirePerformanceEntryState(this).detail = cloneWorkerValue(
        options.detail === undefined ? null : options.detail,
        ownerWorkerCloneIntrinsics,
      );
    }
    get detail() {
      return requirePerformanceEntryState(this).detail;
    }
    toJSON() {
      return { ...super.toJSON(), detail: this.detail };
    }
  }
  class PerformanceMeasure extends PerformanceEntry {
    constructor(token, name, start, duration, detail) {
      if (token !== performanceMeasureToken)
        throw new TypeError("Illegal constructor");
      super(name, "measure", start, duration);
      requirePerformanceEntryState(this).detail = detail;
    }
    get detail() {
      return requirePerformanceEntryState(this).detail;
    }
    toJSON() {
      return { ...super.toJSON(), detail: this.detail };
    }
  }
  for (const [name, index] of [
    ["name", 0], ["entryType", 1], ["startTime", 2], ["duration", 3],
  ])
    Object.defineProperty(PerformanceEntry.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        return requirePerformanceEntryState(this).base[index];
      },
    });
  const visibilityStateEntryToken = {};
  class VisibilityStateEntry extends PerformanceEntry {
    constructor(token, state, startTime) {
      if (token !== visibilityStateEntryToken)
        throw new TypeError("Illegal constructor");
      super(state === "hidden" ? "hidden" : "visible",
        "visibility-state", startTime, 0);
    }
  }
  const performanceFiniteTiming = (value, fallback) => {
    if (value === undefined) return fallback;
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
  };
  const performanceEmptyServerTiming = Object.freeze([]);
  class PerformanceResourceTiming extends PerformanceEntry {
    constructor(name, initiatorType = "other", timing = {}) {
      const startTime = performanceFiniteTiming(timing.startTime, 0),
        responseEnd = performanceFiniteTiming(timing.responseEnd, startTime);
      super(name, "resource", startTime, Math.max(0, responseEnd - startTime));
      requirePerformanceEntryState(this).resource = {
        initiatorType: String(initiatorType),
        deliveryType: String(timing.deliveryType || ""),
        nextHopProtocol: String(timing.nextHopProtocol || ""),
        workerStart: 0,
        redirectStart: 0,
        redirectEnd: 0,
        fetchStart: startTime,
        domainLookupStart:
          performanceFiniteTiming(timing.domainLookupStart, startTime),
        domainLookupEnd:
          performanceFiniteTiming(timing.domainLookupEnd, startTime),
        connectStart: performanceFiniteTiming(timing.connectStart, startTime),
        secureConnectionStart:
          performanceFiniteTiming(timing.secureConnectionStart, 0),
        connectEnd: performanceFiniteTiming(timing.connectEnd, startTime),
        requestStart: performanceFiniteTiming(timing.requestStart, startTime),
        finalResponseHeadersStart:
          performanceFiniteTiming(timing.finalResponseHeadersStart, 0),
        firstInterimResponseStart: 0,
        responseStart: performanceFiniteTiming(timing.responseStart, startTime),
        responseEnd,
        workerRouterEvaluationStart: 0,
        workerCacheLookupStart: 0,
        workerMatchedRouterSource: "",
        workerFinalRouterSource: "",
        transferSize: Number(timing.transferSize) || 0,
        encodedBodySize: Number(timing.encodedBodySize) || 0,
        decodedBodySize: Number(timing.decodedBodySize) || 0,
        responseStatus: Math.max(0, Math.min(65535,
          Math.trunc(Number(timing.responseStatus) || 0))),
        renderBlockingStatus: timing.renderBlockingStatus === "blocking"
          ? "blocking" : "non-blocking",
        contentType: String(timing.contentType || ""),
        contentEncoding: "",
        serverTiming: performanceEmptyServerTiming,
      };
    }
    toJSON() {
      return {
        ...super.toJSON(),
        initiatorType: this.initiatorType,
        deliveryType: this.deliveryType,
        nextHopProtocol: this.nextHopProtocol,
        workerStart: this.workerStart,
        redirectStart: this.redirectStart,
        redirectEnd: this.redirectEnd,
        fetchStart: this.fetchStart,
        domainLookupStart: this.domainLookupStart,
        domainLookupEnd: this.domainLookupEnd,
        connectStart: this.connectStart,
        secureConnectionStart: this.secureConnectionStart,
        connectEnd: this.connectEnd,
        requestStart: this.requestStart,
        finalResponseHeadersStart: this.finalResponseHeadersStart,
        firstInterimResponseStart: this.firstInterimResponseStart,
        responseStart: this.responseStart,
        responseEnd: this.responseEnd,
        workerRouterEvaluationStart: this.workerRouterEvaluationStart,
        workerCacheLookupStart: this.workerCacheLookupStart,
        workerMatchedRouterSource: this.workerMatchedRouterSource,
        workerFinalRouterSource: this.workerFinalRouterSource,
        transferSize: this.transferSize,
        encodedBodySize: this.encodedBodySize,
        decodedBodySize: this.decodedBodySize,
        responseStatus: this.responseStatus,
        renderBlockingStatus: this.renderBlockingStatus,
        contentType: this.contentType,
        contentEncoding: this.contentEncoding,
        serverTiming: this.serverTiming,
      };
    }
  }
  for (const name of [
    "initiatorType", "deliveryType", "nextHopProtocol", "workerStart",
    "redirectStart", "redirectEnd", "fetchStart", "domainLookupStart",
    "domainLookupEnd", "connectStart", "secureConnectionStart", "connectEnd",
    "requestStart", "finalResponseHeadersStart", "firstInterimResponseStart",
    "responseStart", "responseEnd", "workerRouterEvaluationStart",
    "workerCacheLookupStart", "workerMatchedRouterSource",
    "workerFinalRouterSource", "transferSize", "encodedBodySize",
    "decodedBodySize", "responseStatus", "renderBlockingStatus", "contentType",
    "contentEncoding", "serverTiming",
  ])
    Object.defineProperty(PerformanceResourceTiming.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        const state = requirePerformanceEntryState(this).resource;
        if (!state) throw new TypeError("Illegal invocation");
        return state[name];
      },
    });
  class PerformanceNavigationTiming extends PerformanceResourceTiming {
    constructor(name) {
      super(name, "navigation");
      const state = requirePerformanceEntryState(this);
      state.base[1] = "navigation";
      state.navigation = {
        type: "navigate",
        redirectCount: 0,
        unloadEventStart: 0,
        unloadEventEnd: 0,
        domInteractive: 0,
        domContentLoadedEventStart: 0,
        domContentLoadedEventEnd: 0,
        domComplete: 0,
        loadEventStart: 0,
        loadEventEnd: 0,
      };
    }
    toJSON() {
      return {
        ...super.toJSON(),
        type: this.type,
        redirectCount: this.redirectCount,
        unloadEventStart: this.unloadEventStart,
        unloadEventEnd: this.unloadEventEnd,
        domInteractive: this.domInteractive,
        domContentLoadedEventStart: this.domContentLoadedEventStart,
        domContentLoadedEventEnd: this.domContentLoadedEventEnd,
        domComplete: this.domComplete,
        loadEventStart: this.loadEventStart,
        loadEventEnd: this.loadEventEnd,
      };
    }
  }
  for (const name of [
    "type", "redirectCount", "unloadEventStart", "unloadEventEnd",
    "domInteractive", "domContentLoadedEventStart",
    "domContentLoadedEventEnd", "domComplete", "loadEventStart",
    "loadEventEnd",
  ])
    Object.defineProperty(PerformanceNavigationTiming.prototype, name, {
      configurable: true,
      enumerable: true,
      get() {
        const state = requirePerformanceEntryState(this).navigation;
        if (!state) throw new TypeError("Illegal invocation");
        return state[name];
      },
    });
  for (const [constructor, name] of [
    [PerformanceEntry, "PerformanceEntry"],
    [PerformanceMark, "PerformanceMark"],
    [PerformanceMeasure, "PerformanceMeasure"],
    [PerformanceResourceTiming, "PerformanceResourceTiming"],
    [PerformanceNavigationTiming, "PerformanceNavigationTiming"],
    [VisibilityStateEntry, "VisibilityStateEntry"],
  ]) {
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: name,
    });
  }
  const sortPerformanceEntries = entries => entries.slice().sort(
      (left, right) => left.startTime - right.startTime),
    performanceObserverEntryListState = new WeakMap();
  class PerformanceObserverEntryList {
    constructor(entries) {
      performanceObserverEntryListState.set(this, entries.slice());
    }
    getEntries() {
      const entries = performanceObserverEntryListState.get(this);
      if (!entries) throw new TypeError("Illegal invocation");
      return sortPerformanceEntries(entries);
    }
    getEntriesByType(type) {
      const entries = performanceObserverEntryListState.get(this);
      if (!entries) throw new TypeError("Illegal invocation");
      type = String(type);
      return sortPerformanceEntries(
        entries.filter((entry) => entry.entryType === type));
    }
    getEntriesByName(name, type) {
      const entries = performanceObserverEntryListState.get(this);
      if (!entries) throw new TypeError("Illegal invocation");
      name = String(name);
      if (type !== undefined) type = String(type);
      return sortPerformanceEntries(entries.filter(
        (entry) => entry.name === name &&
          (type === undefined || entry.entryType === type)));
    }
  }
  Object.assign(globalThis, {
    PerformanceEntry,
    PerformanceMark,
    PerformanceMeasure,
    PerformanceResourceTiming,
    PerformanceNavigationTiming,
    PerformanceObserverEntryList,
  });
  globalThis.VisibilityStateEntry = VisibilityStateEntry;
  if (globalThis.__tilefinchDeterministicDateFacade !== Date)
    Date.now = () => __tilefinchDateNow(0);
  const performanceObserverState = new WeakMap(),
    performanceObservers = new Set(),
    performanceObserverLimit = 16,
    performanceObserverRecordLimit = 64,
    supportedPerformanceEntryTypes = Object.freeze([
      "mark",
      "measure",
      "resource",
      "navigation",
      "paint",
      "visibility-state",
    ]),
    schedulePerformanceObserver = (observer, state) => {
      if (state.pending || state.records.length === 0) return;
      state.pending = true;
      setTimeout(() => {
        state.pending = false;
        if (!performanceObservers.has(observer) || state.records.length === 0)
          return;
        const records = state.records.splice(0),
          entries = new PerformanceObserverEntryList(records);
        try {
          state.callback(entries, observer, {
            droppedEntriesCount: state.dropped,
          });
        } catch (error) {
          globalThis.__tilefinchReportUncaught?.(
            error,
            "PerformanceObserver",
          );
        }
        state.dropped = 0;
      }, 0);
    },
    notifyPerformanceObservers = (entry) => {
      for (const observer of performanceObservers) {
        const state = performanceObserverState.get(observer);
        if (!state || !state.types.has(entry.entryType)) continue;
        if (state.records.length >= performanceObserverRecordLimit) {
          state.records.shift();
          state.dropped++;
        }
        state.records.push(entry);
        schedulePerformanceObserver(observer, state);
      }
    },
    navigationPerformanceEntry =
      new PerformanceNavigationTiming(location.href),
    performanceEntries = [
      navigationPerformanceEntry,
      new VisibilityStateEntry(
        visibilityStateEntryToken, document.visibilityState, 0),
    ],
    appendPerformanceEntry = (entry) => {
      if (entry.entryType === "visibility-state") {
        let visibilityEntries = 0;
        for (let at = performanceEntries.length - 1; at >= 0; at--) {
          if (performanceEntries[at].entryType !== "visibility-state")
            continue;
          visibilityEntries++;
          if (visibilityEntries >= 50) {
            performanceEntries.splice(at, 1);
            break;
          }
        }
      }
      if (performanceEntries.length >= 128) {
        const at = performanceEntries.findIndex(
          (value) => value.entryType !== "navigation",
        );
        if (at >= 0) performanceEntries.splice(at, 1);
      }
      if (performanceEntries.length < 128) performanceEntries.push(entry);
      notifyPerformanceObservers(entry);
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
  globalThis.__tilefinchRecordResourceTiming = (
    name,
    initiatorType,
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
    const end = __tilefinchPerformanceNow(6),
      duration = measured ? Math.max(0, Number(totalUs) / 1000) : 0,
      start = Math.max(0, end - duration),
      at = (microseconds) =>
        measured
          ? Math.min(end, start + Math.max(0, Number(microseconds) / 1000))
          : start,
      encodedBytes = Math.max(0, Number(encodedBodyBytes) || 0),
      decodedBytes = Math.max(0, Number(decodedBodyBytes) || 0),
      exposedEncodedBytes = timingAllowed ? encodedBytes : 0,
      exposedDecodedBytes = timingAllowed ? decodedBytes : 0;
    return appendPerformanceEntry(
      new PerformanceResourceTiming(
        String(name),
        String(initiatorType || "other"),
        {
          startTime: start,
          nextHopProtocol: timingAllowed
            ? ["", "http/1.0", "http/1.1", "h2"][nextHopProtocol] || ""
            : "",
          deliveryType: timingAllowed && (cacheHit || cacheValidated)
            ? "cache" : "",
          domainLookupStart: timingAllowed ? start : 0,
          domainLookupEnd: timingAllowed ? at(nameLookupUs) : 0,
          connectStart: timingAllowed ? at(nameLookupUs) : 0,
          secureConnectionStart: timingAllowed ? at(connectUs) : 0,
          connectEnd: timingAllowed ? at(appconnectUs || connectUs) : 0,
          requestStart: timingAllowed ? at(appconnectUs || connectUs) : 0,
          responseStart: timingAllowed ? at(firstByteUs) : 0,
          finalResponseHeadersStart: timingAllowed ? at(firstByteUs) : 0,
          responseEnd: end,
          transferSize: !timingAllowed
            ? 0
            : cacheHit
              ? 0
              : cacheValidated
                ? 300
                : exposedEncodedBytes + 300,
          encodedBodySize: exposedEncodedBytes,
          decodedBodySize: exposedDecodedBytes,
          responseStatus,
          /* The native side has already applied Fetch's filtered-response
             rules.  TAO masks connection timing independently of status and
             response body metadata. */
          contentType,
        },
      ),
    );
  };
  globalThis.__tilefinchRecordNavigationTiming = (
    nameLookupUs = 0,
    connectUs = 0,
    appconnectUs = 0,
    firstByteUs = 0,
    totalUs = 0,
    responseStartUs = 0,
    responseEndUs = 0,
    encodedBodyBytes = 0,
    decodedBodyBytes = 0,
    measured = false,
    responseComplete = false,
    encodedBodyBytesMeasured = false,
    responseStatus = 0,
    nextHopProtocol = "",
    contentType = "",
  ) => {
    const state = requirePerformanceEntryState(navigationPerformanceEntry),
      resource = state.resource,
      responseStart = Math.max(0, Number(responseStartUs) / 1000 || 0),
      responseEnd = responseComplete
        ? Math.max(responseStart, Number(responseEndUs) / 1000 || 0)
        : 0,
      finalHopStart = measured && responseComplete
        ? Math.max(0, responseEnd - Math.max(0, Number(totalUs) / 1000))
        : 0,
      at = microseconds => measured && responseComplete
        ? Math.min(responseEnd,
          finalHopStart + Math.max(0, Number(microseconds) / 1000))
        : 0;
    state.base[2] = 0;
    resource.fetchStart = 0;
    if (measured && responseComplete) {
      resource.domainLookupStart = finalHopStart;
      resource.domainLookupEnd = at(nameLookupUs);
      resource.connectStart = at(nameLookupUs);
      resource.secureConnectionStart = appconnectUs > connectUs
        ? at(connectUs) : 0;
      resource.connectEnd = at(appconnectUs || connectUs);
      resource.requestStart = at(appconnectUs || connectUs);
      resource.responseStart = responseStart || at(firstByteUs);
    } else if (responseStart > 0) {
      resource.responseStart = responseStart;
      resource.finalResponseHeadersStart = responseStart;
    }
    if (responseComplete) {
      resource.responseEnd = responseEnd;
      resource.finalResponseHeadersStart = resource.responseStart;
      const encoded = encodedBodyBytesMeasured
        ? Math.max(0, Number(encodedBodyBytes) || 0) : 0,
        decoded = Math.max(0, Number(decodedBodyBytes) || 0);
      resource.encodedBodySize = encoded;
      resource.decodedBodySize = decoded;
      resource.transferSize = encodedBodyBytesMeasured ? encoded + 300 : 0;
      resource.responseStatus = Math.max(0, Math.min(65535,
        Math.trunc(Number(responseStatus) || 0)));
      resource.nextHopProtocol = String(nextHopProtocol || "");
      resource.contentType = String(contentType || "");
    }
    const epoch = performanceTimeOrigin;
    performanceTimingState.fetchStart = epoch;
    if (measured && responseComplete) {
      performanceTimingState.domainLookupStart = epoch + finalHopStart;
      performanceTimingState.domainLookupEnd =
        epoch + resource.domainLookupEnd;
      performanceTimingState.connectStart = epoch + resource.connectStart;
      performanceTimingState.connectEnd = epoch + resource.connectEnd;
      performanceTimingState.requestStart = epoch + resource.requestStart;
    }
    if (resource.responseStart > 0)
      performanceTimingState.responseStart = epoch + resource.responseStart;
    if (responseComplete)
      performanceTimingState.responseEnd = epoch + resource.responseEnd;
    return navigationPerformanceEntry;
  };
  globalThis.__tilefinchRecordVisibilityPerformance = state =>
    appendPerformanceEntry(
      new VisibilityStateEntry(
        visibilityStateEntryToken,
        state,
        __tilefinchPerformanceNow(6),
      ),
    );
  const performanceTimeOrigin = Number(
    globalThis.__tilefinchPerformanceTimeOrigin,
  );
  delete globalThis.__tilefinchPerformanceTimeOrigin;
  const performanceValueState = new WeakMap();
  class Performance extends EventTarget {
    constructor() {
      super();
      throw new TypeError("Illegal constructor");
    }
  }
  const requirePerformanceValue = value => {
    const state = performanceValueState.get(value);
    if (!state) throw new TypeError("Illegal invocation");
    return state;
  };
  Object.defineProperty(Performance.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "Performance",
  });
  /* Tilefinch has no process-isolated browsing-context group and therefore
     exposes the reduced-resolution timing surface.  Keep the capability fact
     explicit instead of leaving feature detection to infer it from missing
     SharedArrayBuffer APIs. */
  Object.defineProperty(globalThis, "crossOriginIsolated", {
    value: false,
    writable: false,
    enumerable: true,
    configurable: true,
  });
  const performanceTimingState = {
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
    },
    performanceTiming = {};
  for (const name of Object.keys(performanceTimingState))
    Object.defineProperty(performanceTiming, name, {
      enumerable: true,
      get: () => performanceTimingState[name],
    });
  Object.freeze(performanceTiming);
  const performanceNavigation = Object.freeze({ type: 0, redirectCount: 0 });
  Object.defineProperties(Performance.prototype, {
    timeOrigin: {
      configurable: true,
      enumerable: true,
      get() { return requirePerformanceValue(this).timeOrigin; },
    },
    timing: {
      configurable: true,
      enumerable: true,
      get() { requirePerformanceValue(this); return performanceTiming; },
    },
    navigation: {
      configurable: true,
      enumerable: true,
      get() { requirePerformanceValue(this); return performanceNavigation; },
    },
  });
  const performanceOperations = {
    now() {
      requirePerformanceValue(this);
      return __tilefinchPerformanceNow(3);
    },
    mark(name, options = {}) {
      requirePerformanceValue(this);
      return appendPerformanceEntry(new PerformanceMark(name, options));
    },
    measure(name, startOrOptions, endMark) {
      requirePerformanceValue(this);
      let start = 0,
        end,
        detail = null;
      if (typeof startOrOptions === "object" && startOrOptions !== null) {
        const hasStart = startOrOptions.start !== undefined,
          hasEnd = startOrOptions.end !== undefined,
          hasDuration = startOrOptions.duration !== undefined;
        if ((hasStart && hasEnd && hasDuration)
            || (hasDuration && !hasStart && !hasEnd))
          throw new TypeError("Invalid measure options");
        start = hasStart ? performanceTimestamp(startOrOptions.start) : 0;
        end = hasEnd ? performanceTimestamp(startOrOptions.end) : undefined;
        const duration = hasDuration ? Number(startOrOptions.duration) : NaN;
        if (hasDuration && (!Number.isFinite(duration) || duration < 0))
          throw new TypeError("duration must be finite and nonnegative");
        if (!hasStart && hasDuration) start = end - duration;
        else if (!hasEnd)
          end = hasDuration ? start + duration : __tilefinchPerformanceNow(5);
        detail = cloneWorkerValue(
          startOrOptions.detail === undefined ? null : startOrOptions.detail,
          ownerWorkerCloneIntrinsics,
        );
      } else {
        if (startOrOptions !== undefined)
          start = performanceTimestamp(startOrOptions);
        end =
          endMark !== undefined
            ? performanceTimestamp(endMark)
            : __tilefinchPerformanceNow(5);
      }
      if (!Number.isFinite(start) || !Number.isFinite(end)
          || start < 0 || end < start)
        throw new TypeError("measure timestamps must be ordered and nonnegative");
      const entry = new PerformanceMeasure(
        performanceMeasureToken, String(name), start, end - start, detail,
      );
      return appendPerformanceEntry(entry);
    },
    getEntries() {
      requirePerformanceValue(this);
      return sortPerformanceEntries(performanceEntries);
    },
    getEntriesByType(type) {
      requirePerformanceValue(this);
      type = String(type);
      return sortPerformanceEntries(
        performanceEntries.filter((entry) => entry.entryType === type));
    },
    getEntriesByName(name, type) {
      requirePerformanceValue(this);
      name = String(name);
      if (type !== undefined) type = String(type);
      return sortPerformanceEntries(performanceEntries.filter(
        (entry) => entry.name === name &&
          (type === undefined || entry.entryType === type)));
    },
    clearMarks(name) {
      requirePerformanceValue(this);
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (
          performanceEntries[i].entryType === "mark" &&
          (name === undefined || performanceEntries[i].name === String(name))
        )
          performanceEntries.splice(i, 1);
    },
    clearMeasures(name) {
      requirePerformanceValue(this);
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (
          performanceEntries[i].entryType === "measure" &&
          (name === undefined || performanceEntries[i].name === String(name))
        )
          performanceEntries.splice(i, 1);
    },
    clearResourceTimings() {
      requirePerformanceValue(this);
      for (let i = performanceEntries.length - 1; i >= 0; i--)
        if (performanceEntries[i].entryType === "resource")
          performanceEntries.splice(i, 1);
    },
    setResourceTimingBufferSize() { requirePerformanceValue(this); },
      toJSON() {
        return { timeOrigin: requirePerformanceValue(this).timeOrigin };
      },
  };
  for (const name of Object.keys(performanceOperations))
    Object.defineProperty(Performance.prototype, name, {
      configurable: true,
      writable: true,
      value: performanceOperations[name],
    });
  const performanceValue = Object.create(Performance.prototype);
  performanceValueState.set(performanceValue, {
    timeOrigin: performanceTimeOrigin,
  });
  globalThis.Performance = Performance;
  globalThis.performance = performanceValue;
  /* A dedicated worker has its own performance time origin and monotonic zero.
     Keep the implementation bounded by sharing the immutable interface shape,
     while rebasing now() for the worker lifetime.  The lazy Worker bootstrap
     wraps this timing source in its own Worker-realm Performance prototype. */
  globalThis.__tilefinchCreateWorkerPerformance = () => {
    const monotonicOrigin = __tilefinchPerformanceNow(3);
    return {
      timeOrigin: Date.now(),
      now: () => Math.max(
        0,
        __tilefinchPerformanceNow(3) - monotonicOrigin,
      ),
    };
  };
  const memoryInfoState = new WeakMap(),
    memoryInfoPrototype = Object.create(Object.prototype),
    memoryInfoValue = (object, index) => {
      const values = memoryInfoState.get(object);
      if (!values) throw new TypeError("Illegal invocation");
      return values[index];
    };
  Object.defineProperties(memoryInfoPrototype, {
    totalJSHeapSize: {
      configurable: true,
      enumerable: true,
      get() { return memoryInfoValue(this, 1); },
    },
    usedJSHeapSize: {
      configurable: true,
      enumerable: true,
      get() { return memoryInfoValue(this, 2); },
    },
    jsHeapSizeLimit: {
      configurable: true,
      enumerable: true,
      get() { return memoryInfoValue(this, 0); },
    },
    [Symbol.toStringTag]: {
      configurable: true,
      value: "MemoryInfo",
    },
  });
  Object.defineProperty(globalThis.performance, "memory", {
    configurable: true,
    enumerable: true,
    get() {
      const values = globalThis.__tilefinchHeapMemorySnapshot();
      const object = Object.create(memoryInfoPrototype);
      memoryInfoState.set(object, [
        Math.max(0, Number(values?.[0]) || 0),
        Math.max(0, Number(values?.[1]) || 0),
        Math.max(0, Number(values?.[2]) || 0),
      ]);
      return object;
    },
  });
  globalThis.PerformanceObserver = class PerformanceObserver {
    constructor(callback) {
      if (typeof callback !== "function")
        throw new TypeError("callback required");
      performanceObserverState.set(this, {
        callback,
        types: new Set(),
        mode: null,
        records: [],
        pending: false,
        dropped: 0,
      });
    }
    observe(options = {}) {
      const state = performanceObserverState.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      options = Object(options);
      const hasEntryTypes = options.entryTypes !== undefined,
        hasType = options.type !== undefined;
      if (hasEntryTypes === hasType)
        throw new TypeError("Specify entryTypes or type");
      const requestedMode = hasType ? "single" : "multiple";
      if (state.mode !== null && state.mode !== requestedMode)
        throw new DOMException(
          "PerformanceObserver registration mode cannot change",
          "InvalidModificationError",
        );
      state.mode = requestedMode;
      const requested = hasType
          ? [String(options.type)]
          : Array.from(options.entryTypes, String),
        types = new Set(
          requested.filter((type) =>
            supportedPerformanceEntryTypes.includes(type),
          ),
        );
      if (performanceObservers.size >= performanceObserverLimit &&
          !performanceObservers.has(this))
        throw new RangeError("PerformanceObserver quota exceeded");
      if (hasType) {
        for (const type of types) state.types.add(type);
      } else state.types = types;
      performanceObservers.add(this);
      if (hasType && options.buffered && types.has(requested[0])) {
        for (const entry of performanceEntries) {
          if (entry.entryType !== requested[0]) continue;
          if (state.records.length >= performanceObserverRecordLimit) {
            state.records.shift();
            state.dropped++;
          }
          state.records.push(entry);
        }
        schedulePerformanceObserver(this, state);
      }
    }
    disconnect() {
      const state = performanceObserverState.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      state.types.clear();
      state.records.length = 0;
      state.pending = false;
      state.dropped = 0;
      performanceObservers.delete(this);
    }
    takeRecords() {
      const state = performanceObserverState.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return sortPerformanceEntries(state.records.splice(0));
    }
  };
  Object.defineProperty(
    globalThis.PerformanceObserver.prototype,
    Symbol.toStringTag,
    { configurable: true, value: "PerformanceObserver" },
  );
  Object.defineProperty(PerformanceObserver, "supportedEntryTypes", {
    value: supportedPerformanceEntryTypes,
    writable: false,
    enumerable: true,
    configurable: true,
  });
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
    const navigationState =
      requirePerformanceEntryState(navigationPerformanceEntry),
      navigationTiming = navigationState.navigation,
      domInteractive = __tilefinchPerformanceNow(3);
    document.readyState = "interactive";
    navigationTiming.domInteractive = domInteractive;
    performanceTimingState.domInteractive =
      performanceTimeOrigin + domInteractive;
    navigationTiming.domContentLoadedEventStart =
      __tilefinchPerformanceNow(3);
    performanceTimingState.domContentLoadedEventStart =
      performanceTimeOrigin + navigationTiming.domContentLoadedEventStart;
    document.dispatchEvent(new Event("DOMContentLoaded", { bubbles: true }));
    globalThis.__tilefinchMaybeStartMotion?.();
    const domContentLoadedEventEnd = __tilefinchPerformanceNow(3);
    document.readyState = "complete";
    const domComplete = __tilefinchPerformanceNow(3),
      loadEventStart = __tilefinchPerformanceNow(3);
    navigationTiming.domContentLoadedEventEnd = domContentLoadedEventEnd;
    navigationTiming.domComplete = domComplete;
    navigationTiming.loadEventStart = loadEventStart;
    performanceTimingState.domContentLoadedEventEnd =
      performanceTimeOrigin + domContentLoadedEventEnd;
    performanceTimingState.domComplete = performanceTimeOrigin + domComplete;
    performanceTimingState.loadEventStart =
      performanceTimeOrigin + loadEventStart;
    const loadEvent = new Event("load");
    /*
     * HTMLBodyElement's onload handler reflects the Window load handler
     * surface.  Programmatic `document.body.onload = ...` assignments are
     * stored on the wrapped body, so invoke that property before dispatching
     * the Window event. Markup and programmatic handlers share the same lazy,
     * CSP-gated event-handler property path.
     */
    const bodyOnload = document.body?.onload;
    if (typeof bodyOnload === "function")
      try {
        bodyOnload.call(globalThis, loadEvent);
      } catch (error) {
        __tilefinchReportUncaught(error, "body onload");
      }
    globalThis.dispatchEvent(loadEvent);
    const loadEventEnd = __tilefinchPerformanceNow(3);
    navigationState.base[3] = Math.max(
      0, loadEventEnd - navigationPerformanceEntry.startTime);
    navigationTiming.loadEventEnd = loadEventEnd;
    performanceTimingState.loadEventEnd = performanceTimeOrigin + loadEventEnd;
    /* Navigation Timing queues the already-buffered navigation entry only
       after the load lifecycle has completed. Observers registered while the
       document was loading therefore receive exactly one finalized record. */
    notifyPerformanceObservers(navigationPerformanceEntry);
  };
  Object.defineProperty(globalThis.__tilefinchRootCensus, "frameWindows", {
    get: () => frames.frameWindowCount(),
  });
})();
