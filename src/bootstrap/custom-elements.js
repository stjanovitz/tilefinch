(() => {
  /* Definitions made in any registry, main or scoped. */
  let definedAnywhere = 0;
  const definitions = new Map(),
    constructors = new Map(),
    waiting = new Map(),
    scopedRegistryStates = new WeakMap(),
    /* The registry each document uses; kept off the document objects. */
    registryByDocument = new WeakMap(),
    definitionByConstructor = new WeakMap(),
    formOwnerByElement = new WeakMap(),
    disabledStateByElement = new WeakMap(),
    stack = [];
  let definitionRunning = false;
  const formAssociatedLocalNames = [];
  globalThis.__tilefinchCustomElementConstructionStack = stack;
  const reserved = new Set([
    "annotation-xml",
    "color-profile",
    "font-face",
    "font-face-src",
    "font-face-uri",
    "font-face-format",
    "font-face-name",
    "missing-glyph",
  ]);
  const validName = (name) =>
    /^[a-z][.0-9_a-z-]*-[.0-9_a-z-]*$/.test(name) && !reserved.has(name);
  /*
   * The Web IDL IsConstructor check must not read constructor.prototype.
   * Reflect.construct(..., value) does that observable read in QuickJS and
   * breaks the definition algorithm's required ordering for Proxy classes.
   * Function#toString distinguishes the only callable/non-constructible shape
   * used by web content (arrow functions) without touching page properties.
   */
  const functionToString = Function.prototype.toString;
  const isConstructor = (value) => {
    if (typeof value !== "function") return false;
    /*
     * Preserve the Proxy-observable ordering described above while rejecting
     * the overwhelmingly common callable/non-constructible form up front.
     * Function#toString is captured before page code can replace it.
     */
    try {
      return !/^(?:async\s*)?(?:\([^)]*\)|[A-Za-z_$][\w$]*)\s*=>/
        .test(functionToString.call(value).trim());
    } catch (_) {
      return true;
    }
  };
  const sequence = (value, limit) => {
    if (value === undefined) return [];
    if (
      value === null ||
      typeof value[Symbol.iterator] !== "function"
    )
      throw new TypeError("Iterable sequence required");
    return Array.from(value, String).slice(0, limit);
  };
  const callback = (prototype, name) => {
    const value = prototype[name];
    if (value !== undefined && value !== null && typeof value !== "function")
      throw new TypeError(name + " must be callable");
    return value || null;
  };
  const report = (error, context) =>
    globalThis.__tilefinchReportUncaught?.(error, context);
  const connected = (node) => {
    if (node?.isConnected) return true;
    for (
      let at = node, steps = 0;
      at && steps < 256;
      at = at instanceof ShadowRoot ? at.host : at.parentNode,
        steps++
    )
      if (at instanceof Document || at.nodeType === Node.DOCUMENT_NODE)
        return true;
    return false;
  };
  const descendants = (root) => {
    const values = [],
      pending = [];
    if (root instanceof Element) pending.push(root);
    else if (root === document && document.documentElement)
      pending.push(document.documentElement);
    else {
      const children = Array.from(root?.children || []);
      for (let index = children.length - 1; index >= 0; index--)
        pending.push(children[index]);
    }
    while (pending.length && values.length < 4096) {
      const node = pending.pop();
      values.push(node);
      const light = Array.from(node.children || []);
      for (let index = light.length - 1; index >= 0; index--)
        pending.push(light[index]);
      const shadow = globalThis.__tilefinchShadowRootForHost?.(node),
        shadowChildren = Array.from(shadow?.children || []);
      for (let index = shadowChildren.length - 1; index >= 0; index--)
        pending.push(shadowChildren[index]);
    }
    return values;
  };
  const definitionFor = (node) => {
    if (node?.__tilefinchCustomElementDefinition)
      return node.__tilefinchCustomElementDefinition;
    if (
      !node ||
      node.namespaceURI !== "http://www.w3.org/1999/xhtml"
    )
      return undefined;
    const owner = node.ownerDocument;
    if (owner && owner !== document && !owner.defaultView) return undefined;
    const registry = owner ? registryByDocument.get(owner) : undefined,
      available =
        scopedRegistryStates.get(registry)?.definitions || definitions,
      localName = String(
        node.localName || node.tagName || "",
      ).toLowerCase(),
      is = node.getAttribute?.("is");
    if (is) {
      const customized = available.get(String(is).toLowerCase());
      if (customized?.localName === localName) return customized;
    }
    const autonomous = available.get(localName);
    return autonomous?.localName === localName ? autonomous : undefined;
  };
  const definitionForRegistry = (node, available) => {
    if (
      !node ||
      node.namespaceURI !== "http://www.w3.org/1999/xhtml"
    )
      return undefined;
    const localName = String(
        node.localName || node.tagName || "",
      ).toLowerCase(),
      is = node.getAttribute?.("is");
    if (is) {
      const customized = available.get(String(is).toLowerCase());
      if (customized?.localName === localName) return customized;
    }
    const autonomous = available.get(localName);
    return autonomous?.localName === localName ? autonomous : undefined;
  };
  const invoke = (node, definition, name, args, context) => {
    const value = definition.callbacks[name];
    if (typeof value !== "function") return;
    try {
      globalThis.__tilefinchRunTask(
        "custom-element:" + String(name),
        value,
        node,
        args,
      );
    } catch (error) {
      report(error, context);
    }
  };
  const formOwnerFor = (node) => {
      if (!connected(node)) return null;
      const explicit = node.getAttribute?.("form");
      if (explicit) {
        const candidate = node.ownerDocument?.getElementById(explicit);
        return candidate instanceof HTMLFormElement ? candidate : null;
      }
      return node.closest?.("form") || null;
    },
    disabledFor = (node) => {
      if (node.hasAttribute?.("disabled")) return true;
      for (
        let at = node.parentElement, steps = 0;
        at && steps < 256;
        at = at.parentElement, steps++
      )
        if (
          String(at.localName || "").toLowerCase() === "fieldset" &&
          at.hasAttribute("disabled")
        )
          return true;
      return false;
    },
    synchronizeFormOwner = (node, definition = definitionFor(node)) => {
      if (!definition?.formAssociated) return;
      const next = formOwnerFor(node),
        hadPrevious = formOwnerByElement.has(node),
        previous = hadPrevious ? formOwnerByElement.get(node) : null;
      formOwnerByElement.set(node, next);
      if (next !== previous)
        invoke(
          node,
          definition,
          "formAssociatedCallback",
          [next],
          "custom element form association",
        );
    },
    synchronizeDisabled = (node, definition = definitionFor(node)) => {
      if (!definition?.formAssociated) return;
      const next = disabledFor(node),
        hadPrevious = disabledStateByElement.has(node),
        previous = hadPrevious ? disabledStateByElement.get(node) : false;
      disabledStateByElement.set(node, next);
      if ((!hadPrevious && next) || (hadPrevious && next !== previous))
        invoke(
          node,
          definition,
          "formDisabledCallback",
          [next],
          "custom element form disabled",
        );
    };
  const failUpgrade = (node, definition, error) => {
    node.__tilefinchCustomElementState = "failed";
    if (node.__handle !== undefined) __tilefinchSetCustomState(node.__handle, -1);
    if (globalThis.HTMLUnknownElement) {
      globalThis.__tilefinchPrepareNativePrototype?.(node);
      Object.setPrototypeOf(node, HTMLUnknownElement.prototype);
    }
    report(error, "custom element " + definition.name);
    return node;
  };
  const upgradeOne = (
    node,
    definition = definitionFor(node),
    synchronousCreation = false,
  ) => {
    if (
      !node ||
      node.__tilefinchCustomElementState === "custom" ||
      node.__tilefinchCustomElementState === "failed" ||
      node.__tilefinchCustomElementState === "precustomized" ||
      !definition
    )
      return node;
    if (
      definition.disabled?.has("shadow") &&
      globalThis.__tilefinchShadowRootForHost?.(node)
    ) {
      return failUpgrade(
        node,
        definition,
        new DOMException(
          "Shadow root is disabled for this custom element",
          "NotSupportedError",
        ),
      );
    }
    const wasConnected = connected(node),
      expectedDocument = node.ownerDocument,
      expectedLocalName = String(node.localName).toLowerCase();
    node.__tilefinchCustomElementState = "precustomized";
    Object.defineProperty(node, "__tilefinchCustomElementDefinition", {
      configurable: true,
      writable: true,
      value: definition,
    });
    stack.push(node);
    try {
      const built = Reflect.construct(definition.constructor, []);
      if (built !== node)
        throw built instanceof Element
          ? new DOMException(
              "Custom element constructor returned another element",
              "NotSupportedError",
            )
          : new TypeError(
              "custom element constructor returned another object",
            );
      const createdAttributeCount =
          node.__handle === undefined
            ? node.attributes.length
            : (__tilefinchAttributes(node.__handle) || []).length,
        allowedCustomizedBuiltinAttribute =
          definition.localName !== definition.name &&
          createdAttributeCount === 1 &&
          node.getAttribute("is") === definition.name;
      if (
        synchronousCreation &&
        ((createdAttributeCount !== 0 && !allowedCustomizedBuiltinAttribute) ||
          (node.__handle === undefined
            ? node.childNodes.length
            : (__tilefinchChildNodes(node.__handle) || []).length) !== 0 ||
          node.parentNode !== null ||
          node.ownerDocument !== expectedDocument ||
          String(node.localName).toLowerCase() !== expectedLocalName ||
          node.namespaceURI !== "http://www.w3.org/1999/xhtml")
      )
        throw new DOMException(
          "Custom element constructor changed its element",
          "NotSupportedError",
        );
      node.__tilefinchCustomElementState = "custom";
      if (node.__handle !== undefined) __tilefinchSetCustomState(node.__handle, 1);
      synchronizeFormOwner(node, definition);
      synchronizeDisabled(node, definition);
      for (const name of definition.observed) {
        const value = node.getAttribute(name);
        if (value !== null)
          invoke(
            node,
            definition,
            "attributeChangedCallback",
            [name, null, value, null],
            "custom element attribute",
          );
      }
      /*
       * Reactions caused by a constructor itself are suppressed.  Whether the
       * element was connected at entry determines the post-upgrade callback.
       */
      if (wasConnected && definition.callbacks.connectedCallback) {
        invoke(
          node,
          definition,
          "connectedCallback",
          [],
          "custom element connected",
        );
        node.__tilefinchCustomElementConnectedState = true;
      }
    } catch (error) {
      return failUpgrade(node, definition, error);
    } finally {
      stack.pop();
    }
    return node;
  };
  class CustomElementRegistry {
    define(name, constructor, options = undefined) {
      const scoped = scopedRegistryStates.get(this),
        activeDefinitions = scoped?.definitions || definitions,
        activeConstructors = scoped?.constructors || constructors,
        activeWaiting = scoped?.waiting || waiting;
      name = String(name);
      if (!isConstructor(constructor))
        throw new TypeError("Custom element constructor required");
      if (!validName(name))
        throw new DOMException("Invalid custom element name", "SyntaxError");
      if (
        activeDefinitions.has(name) ||
        activeConstructors.has(constructor)
      )
        throw new DOMException(
          "Custom element already defined",
          "NotSupportedError",
        );
      let localName = name;
      if (options?.extends !== undefined) {
        localName = String(options.extends).toLowerCase();
        if (
          validName(localName) ||
          !/^[a-z][a-z0-9-]*$/.test(localName)
        )
          throw new DOMException(
            "Invalid customized built-in name",
            "NotSupportedError",
          );
      }
      if (scoped ? scoped.definitionRunning : definitionRunning)
        throw new DOMException(
          "A custom element definition is already running",
          "NotSupportedError",
        );
      if (activeDefinitions.size >= 128)
        throw new DOMException(
          "Custom element registry limit reached",
          "QuotaExceededError",
        );
      if (scoped) scoped.definitionRunning = true;
      else definitionRunning = true;
      let definition;
      try {
        const prototype = constructor.prototype;
        if (
          (typeof prototype !== "object" &&
            typeof prototype !== "function") ||
          prototype === null
        )
          throw new TypeError("Custom element prototype must be an object");
        const callbacks = {
          connectedCallback: callback(prototype, "connectedCallback"),
          disconnectedCallback: callback(prototype, "disconnectedCallback"),
        };
        if ("moveBefore" in Element.prototype)
          callbacks.connectedMoveCallback = callback(
            prototype,
            "connectedMoveCallback",
          );
        callbacks.adoptedCallback = callback(prototype, "adoptedCallback");
        callbacks.attributeChangedCallback = callback(
          prototype,
          "attributeChangedCallback",
        );
        const observed = callbacks.attributeChangedCallback
            ? sequence(constructor.observedAttributes, 64).map((value) =>
                value.toLowerCase(),
              )
            : [],
          disabled = new Set(sequence(constructor.disabledFeatures, 8)),
          formAssociated = !!constructor.formAssociated;
        if (formAssociated)
          for (const name of [
            "formAssociatedCallback",
            "formResetCallback",
            "formDisabledCallback",
            "formStateRestoreCallback",
          ])
            callbacks[name] = callback(prototype, name);
        definition = {
          name,
          localName,
          constructor,
          prototype,
          callbacks,
          observed,
          disabled,
          formAssociated,
          createUnupgraded: scoped?.createUnupgraded || null,
        };
        if (formAssociated && !formAssociatedLocalNames.includes(localName))
          formAssociatedLocalNames.push(localName);
      } finally {
        if (scoped) scoped.definitionRunning = false;
        else definitionRunning = false;
      }
      definition.registry = this;
      activeDefinitions.set(name, definition);
      definedAnywhere++;
      activeConstructors.set(constructor, name);
      definitionByConstructor.set(constructor, definition);
      for (const node of descendants(scoped?.document || document))
        upgradeOne(node, definitionForRegistry(node, activeDefinitions));
      const waiter = activeWaiting.get(name);
      if (waiter) {
        activeWaiting.delete(name);
        waiter.resolve(constructor);
      }
    }
    get(name) {
      const scoped = scopedRegistryStates.get(this),
        activeDefinitions = scoped?.definitions || definitions;
      return activeDefinitions.get(String(name).toLowerCase())?.constructor;
    }
    getName(constructor) {
      if (typeof constructor !== "function")
        throw new TypeError("Custom element constructor required");
      const scoped = scopedRegistryStates.get(this),
        activeConstructors = scoped?.constructors || constructors;
      return activeConstructors.get(constructor) || null;
    }
    whenDefined(name) {
      const scoped = scopedRegistryStates.get(this),
        activeWaiting = scoped?.waiting || waiting;
      name = String(name).toLowerCase();
      if (!validName(name))
        return Promise.reject(
          new DOMException("Invalid custom element name", "SyntaxError"),
        );
      const found = this.get(name);
      if (found) return Promise.resolve(found);
      let waiter = activeWaiting.get(name);
      if (!waiter) {
        if (activeWaiting.size >= 128)
          return Promise.reject(
            new DOMException(
              "Custom element waiter limit reached",
              "QuotaExceededError",
            ),
          );
        let resolve;
        const promise = new Promise((done) => {
          resolve = done;
        });
        waiter = { promise, resolve };
        activeWaiting.set(name, waiter);
      }
      return waiter.promise;
    }
    upgrade(root) {
      if (!(root instanceof Node)) throw new TypeError("Node required");
      const scoped = scopedRegistryStates.get(this),
        activeDefinitions = scoped?.definitions || definitions;
      for (const node of descendants(root))
        upgradeOne(node, definitionForRegistry(node, activeDefinitions));
    }
  }
  const registry = new CustomElementRegistry();
  registryByDocument.set(document, registry);
  globalThis.CustomElementRegistry = CustomElementRegistry;
  globalThis.customElements = registry;
  Object.defineProperty(
    globalThis,
    "__tilefinchCustomElementLifecycleNeeded",
    {
      configurable: false,
      enumerable: false,
      writable: false,
      value(root) {
        if (definitions.size) return true;
        if (root?.__tilefinchCustomElementState === "custom") return true;
        const owner = root?.ownerDocument,
          ownerRegistry = owner ? registryByDocument.get(owner) : undefined,
          scoped = scopedRegistryStates.get(ownerRegistry);
        return !!scoped?.definitions?.size;
      },
    },
  );
  {
    const internalsByElement = new WeakMap(),
      customStateLimit = 32,
      validityKeys = new Set([
        "badInput",
        "customError",
        "patternMismatch",
        "rangeOverflow",
        "rangeUnderflow",
        "stepMismatch",
        "tooLong",
        "tooShort",
        "typeMismatch",
        "valueMissing",
      ]),
      definitionForInternals = (element) => {
        const definition = definitionFor(element);
        return definition?.localName === definition?.name ? definition : null;
      },
      requireFormAssociation = (element) => {
        const definition = definitionForInternals(element);
        if (!definition?.formAssociated)
          throw new DOMException(
            "Custom element is not form-associated",
            "NotSupportedError",
          );
        return definition;
      },
      associatedForm = (element) => {
        if (!connected(element)) return null;
        const explicit = element.getAttribute?.("form");
        if (explicit) {
          const candidate = element.ownerDocument?.getElementById(explicit);
          if (candidate instanceof HTMLFormElement) return candidate;
        }
        return element.closest?.("form") || null;
      },
      labelsFor = (element) => {
        const labels = [],
          id = String(element.id || "");
        for (const label of element.ownerDocument?.querySelectorAll("label") || [])
          if (
            label instanceof HTMLLabelElement &&
            (label.contains(element) || (id && label.htmlFor === id))
          )
            labels.push(label);
        const result = Object.create(NodeList.prototype);
        for (let index = 0; index < labels.length; index++)
          Object.defineProperty(result, index, {
            configurable: true,
            enumerable: true,
            value: labels[index],
          });
        Object.defineProperty(result, "length", {
          configurable: true,
          value: labels.length,
        });
        return result;
      },
      shadowIncludingDescendant = (candidate, target) => {
        for (
          let at = candidate, steps = 0;
          at && steps < 256;
          at = at instanceof ShadowRoot ? at.host : at.parentNode, steps++
        )
          if (at === target) return true;
        return false;
      };
    class CustomStateSet {
      constructor(token, state) {
        if (token !== internalsByElement)
          throw new TypeError("Illegal constructor");
        this.__state = state;
      }
      get size() {
        return this.__state.names.size;
      }
      add(value) {
        value = String(value);
        if (
          !this.__state.names.has(value) &&
          this.__state.names.size >= customStateLimit
        )
          throw new DOMException(
            "Custom state limit reached",
            "QuotaExceededError",
          );
        this.__state.names.add(value);
        return this;
      }
      delete(value) {
        return this.__state.names.delete(String(value));
      }
      has(value) {
        return this.__state.names.has(String(value));
      }
      clear() {
        this.__state.names.clear();
      }
      entries() {
        return this.__state.names.entries();
      }
      keys() {
        return this.__state.names.keys();
      }
      values() {
        return this.__state.names.values();
      }
      forEach(callback, thisArg = undefined) {
        this.__state.names.forEach((value) =>
          callback.call(thisArg, value, value, this),
        );
      }
      [Symbol.iterator]() {
        return this.values();
      }
    }
    Object.defineProperty(CustomStateSet.prototype, Symbol.toStringTag, {
      configurable: true,
      value: "CustomStateSet",
    });
    class ElementInternals {
      constructor(token, target) {
        if (token !== internalsByElement)
          throw new TypeError("Illegal constructor");
        this.__target = target;
        this.__formValue = null;
        this.__formState = null;
        this.__validationMessage = "";
        this.__validity = Object.create(null);
        this.__anchor = null;
        this.__aria = Object.create(null);
        this.__states = new CustomStateSet(internalsByElement, {
          names: new Set(),
        });
        const validityState = {};
        for (const name of validityKeys)
          Object.defineProperty(validityState, name, {
            enumerable: true,
            get: () => !!this.__validity[name],
          });
        Object.defineProperty(validityState, "valid", {
          enumerable: true,
          get: () => {
            for (const name of validityKeys)
              if (this.__validity[name]) return false;
            return true;
          },
        });
        this.__validityState = Object.freeze(validityState);
      }
      get shadowRoot() {
        return globalThis.__tilefinchShadowRootForHost?.(this.__target) || null;
      }
      get form() {
        requireFormAssociation(this.__target);
        return associatedForm(this.__target);
      }
      get labels() {
        requireFormAssociation(this.__target);
        return labelsFor(this.__target);
      }
      get willValidate() {
        requireFormAssociation(this.__target);
        if (
          this.__target.hasAttribute("disabled") ||
          this.__target.hasAttribute("readonly")
        )
          return false;
        for (
          let at = this.__target.parentElement, steps = 0;
          at && steps < 256;
          at = at.parentElement, steps++
        ) {
          const name = String(at.localName || "").toLowerCase();
          if (name === "datalist") return false;
          if (name === "fieldset" && at.hasAttribute("disabled")) return false;
        }
        return true;
      }
      get validity() {
        requireFormAssociation(this.__target);
        return this.__validityState;
      }
      get validationMessage() {
        requireFormAssociation(this.__target);
        return this.validity.valid ? "" : this.__validationMessage;
      }
      get states() {
        return this.__states;
      }
      setFormValue(value, state = value) {
        requireFormAssociation(this.__target);
        const acceptable = (candidate) =>
          candidate === null ||
          typeof candidate === "string" ||
          candidate instanceof File ||
          candidate instanceof FormData;
        if (!acceptable(value) || !acceptable(state))
          throw new TypeError("Form value must be a string, File, FormData, or null");
        this.__formValue = value;
        this.__formState = state;
      }
      setValidity(flags = {}, message = "", anchor = null) {
        requireFormAssociation(this.__target);
        if (flags === null || typeof flags !== "object")
          throw new TypeError("Validity flags must be an object");
        const next = Object.create(null);
        for (const name of validityKeys)
          if (!!flags[name]) next[name] = true;
        const invalid = Object.keys(next).length > 0;
        message = String(message);
        if (invalid && !message)
          throw new TypeError("Invalid controls require a validation message");
        if (anchor !== null && !(anchor instanceof HTMLElement))
          throw new TypeError("Validation anchor must be an HTMLElement");
        if (
          anchor !== null &&
          !shadowIncludingDescendant(anchor, this.__target)
        )
          throw new DOMException(
            "Validation anchor must be a shadow-including descendant",
            "NotFoundError",
          );
        this.__validity = next;
        this.__validationMessage = invalid ? message : "";
        this.__anchor = invalid ? anchor : null;
      }
      checkValidity() {
        if (!this.willValidate || this.validity.valid) return true;
        this.__target.dispatchEvent(
          __tilefinchTrustedEvent(
            new Event("invalid", { cancelable: true }),
          ),
        );
        return false;
      }
      reportValidity() {
        return this.checkValidity();
      }
    }
    for (const [property, attribute] of [
      ["role", "role"],
      ["ariaActiveDescendant", "aria-activedescendant"],
      ["ariaAtomic", "aria-atomic"],
      ["ariaAutoComplete", "aria-autocomplete"],
      ["ariaBrailleLabel", "aria-braillelabel"],
      ["ariaBrailleRoleDescription", "aria-brailleroledescription"],
      ["ariaBusy", "aria-busy"],
      ["ariaChecked", "aria-checked"],
      ["ariaColCount", "aria-colcount"],
      ["ariaColIndex", "aria-colindex"],
      ["ariaColIndexText", "aria-colindextext"],
      ["ariaColSpan", "aria-colspan"],
      ["ariaControls", "aria-controls"],
      ["ariaCurrent", "aria-current"],
      ["ariaDescription", "aria-description"],
      ["ariaDetails", "aria-details"],
      ["ariaDisabled", "aria-disabled"],
      ["ariaErrorMessage", "aria-errormessage"],
      ["ariaExpanded", "aria-expanded"],
      ["ariaFlowTo", "aria-flowto"],
      ["ariaHasPopup", "aria-haspopup"],
      ["ariaHidden", "aria-hidden"],
      ["ariaKeyShortcuts", "aria-keyshortcuts"],
      ["ariaLabel", "aria-label"],
      ["ariaLabelledBy", "aria-labelledby"],
      ["ariaLevel", "aria-level"],
      ["ariaLive", "aria-live"],
      ["ariaModal", "aria-modal"],
      ["ariaMultiLine", "aria-multiline"],
      ["ariaMultiSelectable", "aria-multiselectable"],
      ["ariaOrientation", "aria-orientation"],
      ["ariaOwns", "aria-owns"],
      ["ariaPlaceholder", "aria-placeholder"],
      ["ariaPosInSet", "aria-posinset"],
      ["ariaPressed", "aria-pressed"],
      ["ariaReadOnly", "aria-readonly"],
      ["ariaRelevant", "aria-relevant"],
      ["ariaRequired", "aria-required"],
      ["ariaRoleDescription", "aria-roledescription"],
      ["ariaRowCount", "aria-rowcount"],
      ["ariaRowIndex", "aria-rowindex"],
      ["ariaRowIndexText", "aria-rowindextext"],
      ["ariaRowSpan", "aria-rowspan"],
      ["ariaSelected", "aria-selected"],
      ["ariaSetSize", "aria-setsize"],
      ["ariaSort", "aria-sort"],
      ["ariaValueMax", "aria-valuemax"],
      ["ariaValueMin", "aria-valuemin"],
      ["ariaValueNow", "aria-valuenow"],
      ["ariaValueText", "aria-valuetext"],
    ])
      Object.defineProperty(ElementInternals.prototype, property, {
        configurable: true,
        enumerable: true,
        get() {
          return this.__aria[attribute] ?? null;
        },
        set(value) {
          if (value === null) delete this.__aria[attribute];
          else this.__aria[attribute] = String(value);
        },
      });
    Object.defineProperty(HTMLElement.prototype, "attachInternals", {
      configurable: true,
      enumerable: true,
      writable: true,
      value: function attachInternals() {
        const definition = definitionForInternals(this);
        if (
          !definition ||
          (this.__tilefinchCustomElementState !== "precustomized" &&
            this.__tilefinchCustomElementState !== "custom") ||
          definition.disabled?.has("internals") ||
          internalsByElement.has(this)
        )
          throw new DOMException(
            "Element internals are unavailable",
            "NotSupportedError",
          );
        const internals = new ElementInternals(internalsByElement, this);
        internalsByElement.set(this, internals);
        return internals;
      },
    });
    Object.defineProperty(globalThis, "__tilefinchElementInternalsFor", {
      configurable: false,
      enumerable: false,
      writable: false,
      value(element) {
        return internalsByElement.get(element) || null;
      },
    });
    /* Candidate tag names for form.elements' native query; each candidate
       is still checked against its own definition. */
    Object.defineProperty(globalThis, "__tilefinchFormAssociatedSelector", {
      configurable: false,
      enumerable: false,
      writable: false,
      value() {
        return formAssociatedLocalNames.join(",");
      },
    });
    Object.defineProperty(
      globalThis,
      "__tilefinchFormAssociatedCustomElement",
      {
        configurable: false,
        enumerable: false,
        writable: false,
        value(element) {
          return !!definitionFor(element)?.formAssociated;
        },
      },
    );
    globalThis.ElementInternals = ElementInternals;
    globalThis.CustomStateSet = CustomStateSet;
  }
  const constructedImportRule = (rule) =>
    /^@import(?:\s|url|\()/i.test(
      String(rule)
        .replace(/\/\*[\s\S]*?\*\//g, "")
        .trimStart(),
    ),
    /* A constructed sheet keeps at most this many characters of rule text
       (its rules joined by newlines): a memory bound, not a rule count.
       Text past it is not applied; the complete rules before it are, as
       for an oversized <link> sheet (stylesheet_complete_rules_prefix). */
    constructedSheetTextLimit = 256 * 1024,
    /* Splits CSS text into its top-level statements with the native
       tokenizer that bounds <link> sheets (stylesheet_next_statement in
       src/resources.c).  CSS has no fatal syntax errors, so this never
       throws: a block still open at the end is closed, as the CSS parser
       closes it, and a bare trailing fragment is dropped, as the parser
       drops it.  Statements are kept while their joined length fits
       `limit`; `truncated` reports that later text was left out. */
    nativeStatementEnds = globalThis.__tilefinchCssStatementEnds,
    splitConstructedRules = (text, limit) => {
      const rules = [];
      let start = 0,
        used = 0,
        scanWindow = 16 * 1024;
      const keep = (end, closing = "") => {
        /* A statement that can no longer fit is not even sliced. */
        if (end - start > limit - used) return false;
        const rule = closing
          ? text.slice(start, end).trim() + closing
          : text.slice(start, end).trim();
        start = end;
        if (!rule || constructedImportRule(rule)) return true;
        const cost = rule.length + (rules.length ? 1 : 0);
        if (cost > limit - used) return false;
        used += cost;
        rules.push(rule);
        return true;
      };
      while (start < text.length) {
        /* The output quota is not the scanner's work bound: a near-full
           sheet can still end in ignored whitespace, comments or a bare
           prelude. Scan bounded windows independently of remaining space. A
           trimmed rule or discarded @import can leave room for another
           chunk; restart at the last complete statement, never inside a
           token. This bounds the native offsets array and UTF-8 conversion
           without changing the joined-rule quota. */
        const base = start,
          scanLimit = constructedSheetTextLimit + 1,
          scanSize = Math.min(scanLimit, scanWindow),
          scanEnd = Math.min(text.length, base + scanSize),
          ends = nativeStatementEnds(text.slice(base, scanEnd)),
          openBlocks = ends.pop();
        for (const end of ends)
          if (!keep(base + end)) return { rules, truncated: true };
        if (openBlocks !== -1 && scanEnd === text.length) {
          if (openBlocks > 0 && !keep(text.length, "}".repeat(openBlocks)))
            return { rules, truncated: true };
          break;
        }
        /* Do not close a block at an artificial chunk boundary. At the
           scan ceiling, decline an unfinished large tail conservatively. */
        if (start === base) {
          if (scanSize === scanLimit) return { rules, truncated: true };
          /* One long rule may span ordinary windows. Grow this window
             geometrically, never beyond the native scan ceiling. */
          scanWindow = Math.min(scanLimit, scanWindow * 2);
        } else scanWindow = 16 * 1024;
      }
      return { rules, truncated: false };
    };
  /* Native constructed-sheet storage (document_adopted_sheets.c): a
     sheet's text lives once, in a detached <style> element made on its
     first adoption; adoptedStyleSheets lists name those elements, and the
     cascade parses each adopted sheet once however many roots adopt it. */
  const nativeSheetText = globalThis.__tilefinchConstructedSheetText,
    nativeSetAdopted = globalThis.__tilefinchSetAdoptedSheets,
    sheetQuotaError = () =>
      new DOMException("Stylesheet exceeds bounded size", "QuotaExceededError");
  delete globalThis.__tilefinchConstructedSheetText;
  delete globalThis.__tilefinchCssStatementEnds;
  delete globalThis.__tilefinchSetAdoptedSheets;
  class CSSStyleSheet {
    constructor() {
      this.__rules = [];
      this.__textLength = 0;
      this.__cssRules = [];
      this.__constructed = true;
      /* The native sheet element, once adopted. */
      this.__node = null;
      this.ownerNode = null;
      this.disabled = false;
      this.media = {
        mediaText: "",
        length: 0,
        matches: false,
        item() {
          return null;
        },
        [Symbol.iterator]() {
          return [][Symbol.iterator]();
        },
      };
    }
    /* The text is derived, not kept: the native element holds the only
       copy the cascade reads. */
    get __text() {
      return this.__rules.join("\n");
    }
    /* Makes `rules` the sheet's rules. The native copy is replaced first,
       so a refusal leaves the sheet exactly as it was. */
    __commit(rules) {
      const text = rules.join("\n");
      if (
        this.__constructed &&
        this.__node &&
        !nativeSheetText(this.__node.__handle, text)
      )
        throw sheetQuotaError();
      this.__rules = rules;
      this.__textLength = text.length;
      /* Rule objects are built once a page reads cssRules, not for every
         replaceSync: a framework sheet of thousands of rules is usually
         applied without being inspected. The list is live, so once read
         it is kept current. */
      this.__cssRulesStale = true;
      if (this.__cssRulesRead) this.cssRules;
      if (!this.__constructed && this.ownerNode) {
        this.__authorSource = text;
        if (this.ownerNode.textContent !== text)
          this.ownerNode.textContent = text;
      }
    }
    /* The native element for adoption, made on first use. */
    __adoptionNode() {
      if (!this.__node) {
        const node = document.createElement("style");
        if (!nativeSheetText(node.__handle, this.__text))
          throw sheetQuotaError();
        this.__node = node;
      }
      return this.__node;
    }
    replaceSync(text) {
      const split = splitConstructedRules(
        String(text),
        constructedSheetTextLimit,
      );
      this.__commit(split.rules);
      this.__truncated = split.truncated;
    }
    replace(text) {
      return Promise.resolve().then(() => {
        this.replaceSync(text);
        return this;
      });
    }
    get cssRules() {
      this.__cssRulesRead = true;
      if (this.__cssRulesStale) {
        this.__cssRulesStale = false;
        this.__cssRules.length = 0;
        for (const cssText of this.__rules) {
          const atRule = cssText.trimStart().startsWith("@"),
            open = cssText.indexOf("{");
          this.__cssRules.push({
            cssText,
            parentStyleSheet: this,
            type: atRule ? 4 : 1,
            selectorText: !atRule && open >= 0
              ? cssText.slice(0, open).trim()
              : undefined,
          });
        }
      }
      return this.__cssRules;
    }
    /* An author <style> past the text bound is reflected only in part;
       writing that part back would delete the rest of the element. */
    __assertWholeAuthorSheet() {
      if (!this.__constructed && this.__truncated)
        throw new DOMException(
          "Stylesheet exceeds bounded size",
          "QuotaExceededError",
        );
    }
    insertRule(rule, index = 0) {
      this.__assertWholeAuthorSheet();
      rule = String(rule);
      index = Number(index);
      if (!Number.isInteger(index) || index < 0 || index > this.__rules.length)
        throw new DOMException("Invalid rule index", "IndexSizeError");
      if (
        !rule.trim() ||
        (!rule.includes("{") && !rule.trim().endsWith(";"))
      )
        throw new DOMException("Invalid CSS rule", "SyntaxError");
      if (constructedImportRule(rule))
        throw new DOMException(
          "@import is not allowed in constructed stylesheets",
          "SyntaxError",
        );
      const split = splitConstructedRules(rule, constructedSheetTextLimit);
      if (split.truncated) throw sheetQuotaError();
      const parsed = split.rules;
      if (parsed.length !== 1)
        throw new DOMException(
          "insertRule requires exactly one rule",
          "SyntaxError",
        );
      /* The text bound is the one replaceSync applies a prefix under. One
         rule has no prefix to apply: refuse it whole so cssRules always
         lists exactly the rules the cascade holds. */
      if (
        parsed[0].length + (this.__rules.length ? 1 : 0) >
        constructedSheetTextLimit - this.__textLength
      )
        throw sheetQuotaError();
      const rules = this.__rules.slice();
      rules.splice(index, 0, parsed[0]);
      this.__commit(rules);
      return index;
    }
    deleteRule(index) {
      this.__assertWholeAuthorSheet();
      index = Number(index);
      if (!Number.isInteger(index) || index < 0 || index >= this.__rules.length)
        throw new DOMException("Invalid rule index", "IndexSizeError");
      const rules = this.__rules.slice();
      rules.splice(index, 1);
      this.__commit(rules);
    }
  }
  globalThis.CSSStyleSheet = CSSStyleSheet;
  {
    /* Author sheets are reflected lazily because native style parsing owns
       page rendering.  Keep the script-visible CSSOM bounded and avoid doing
       this work unless a page actually inspects it. */
    const authorSheets = new WeakMap(),
      authorSheet = (node, source, href) => {
        let sheet = authorSheets.get(node);
        if (!sheet) {
          sheet = new CSSStyleSheet();
          sheet.__constructed = false;
          sheet.ownerNode = node;
          sheet.href = href;
          sheet.__authorSource = null;
          authorSheets.set(node, sheet);
        }
        source = String(source || "");
        if (sheet.__authorSource !== source) {
          sheet.__constructed = true;
          sheet.replaceSync(source);
          sheet.__constructed = false;
          sheet.__authorSource = source;
        }
        return sheet;
      };
    Object.defineProperty(HTMLStyleElement.prototype, "sheet", Object.getOwnPropertyDescriptor({
      get sheet() {
        return this.isConnected
          ? authorSheet(this, this.textContent, null)
          : null;
      },
    }, "sheet"));
    Object.defineProperty(HTMLLinkElement.prototype, "sheet", Object.getOwnPropertyDescriptor({
      get sheet() {
        const rel = String(this.getAttribute("rel") || "").toLowerCase();
        return this.isConnected && rel.split(/\s+/).includes("stylesheet")
          ? authorSheet(this, "", this.href || null)
          : null;
      },
    }, "sheet"));
  }
  {
    const adoptedByTarget = new WeakMap(),
      setAdopted = (target, value) => {
        const next = Array.from(value);
        if (next.length > 16)
          throw new DOMException(
            "Adopted stylesheet limit reached",
            "QuotaExceededError",
          );
        const unique = new Set();
        for (const sheet of next) {
          if (!(sheet instanceof CSSStyleSheet) || !sheet.__constructed)
            throw new DOMException(
              "Only constructed stylesheets can be adopted",
              "NotAllowedError",
            );
          if (unique.has(sheet))
            throw new DOMException(
              "A stylesheet cannot be adopted twice",
              "NotAllowedError",
            );
          unique.add(sheet);
        }
        /* The document and connected shadow roots style the page: their
           lists go to the native registry (rootHandle 0 is the document).
           Any other document, and a shadow root without a native carrier,
           keep the list for script only. */
        const rootHandle =
          target === document
            ? 0
            : target instanceof ShadowRoot
              ? target.__handle
              : undefined;
        if (rootHandle !== undefined) {
          const handles = next.map((sheet) => sheet.__adoptionNode().__handle);
          if (!nativeSetAdopted(rootHandle, handles))
            throw new DOMException(
              "Adopted stylesheet limit reached",
              "QuotaExceededError",
            );
        }
        adoptedByTarget.set(target, next.slice());
      };
    for (const prototype of [Document.prototype, ShadowRoot.prototype])
      Object.defineProperty(prototype, "adoptedStyleSheets", {
        configurable: true,
        enumerable: true,
        get() {
          return (adoptedByTarget.get(this) || []).slice();
        },
        set(value) {
          setAdopted(this, value);
        },
      });
  }
  globalThis.__tilefinchCustomElementIsDefined = (node) => {
    if (
      node?.namespaceURI !== "http://www.w3.org/1999/xhtml"
    )
      return true;
    const localName = String(node.localName || "").toLowerCase(),
      customized = node.getAttribute?.("is");
    if (!customized && !validName(localName)) return true;
    if (
      node.__tilefinchCustomElementState === "precustomized" ||
      node.__tilefinchCustomElementState === "failed"
    )
      return false;
    return (
      node.__tilefinchCustomElementState === "custom" ||
      !!definitionFor(node)
    );
  };
  globalThis.__tilefinchRestoreCustomElement = (node) => {
    /* Until some registry defines an element, no node can be custom or
       failed; wrapping every node must not pay the registry lookup. */
    if (!definedAnywhere) return node;
    if (globalThis.__tilefinchCustomElementCreationSuppressed) return node;
    const state = __tilefinchGetCustomState(node.__handle);
    /* Only a hyphenated name or an is attribute can have a definition; an
       element neither custom (3) nor failed (1) without one has nothing to
       restore, so spare every wrapper creation the registry lookup. */
    if (
      state !== 3 &&
      state !== 1 &&
      !node.__tilefinchCustomElementDefinition &&
      !String(node.tagName || "").includes("-") &&
      !node.getAttribute?.("is")
    )
      return node;
    const definition = definitionFor(node);
    if (state === 3 && definition) {
      globalThis.__tilefinchPrepareNativePrototype?.(
        node, definition.constructor.prototype);
      Object.setPrototypeOf(node, definition.constructor.prototype);
      node.__tilefinchCustomElementState = "custom";
      node.__tilefinchCustomElementConnectedState = connected(node);
      return node;
    }
    if (state === 1) {
      node.__tilefinchCustomElementState = "failed";
      return node;
    }
    return upgradeOne(node, definition);
  };
  globalThis.__tilefinchCustomElementConnected = (root, force = false) => {
    /*
     * Ordinary pages should not pay a subtree walk on every insertion. The
     * main document cannot contain an upgraded element before its registry
     * has a definition; scoped frame registries remain eligible separately.
     */
    if (!definitions.size && root?.ownerDocument === document) return;
    for (const node of descendants(root)) {
      const definition = definitionFor(node);
      upgradeOne(node, definition);
      synchronizeFormOwner(node, definition);
      synchronizeDisabled(node, definition);
      if (
        node.__tilefinchCustomElementState === "custom" &&
        definition &&
        Object.getPrototypeOf(node) !== definition.prototype
      ) {
        globalThis.__tilefinchPrepareNativePrototype?.(
          node, definition.prototype);
        Object.setPrototypeOf(node, definition.prototype);
      }
      if (
        node.__tilefinchCustomElementState === "custom" &&
        !node.__tilefinchCustomElementConnectedState &&
        (force || connected(node)) &&
        definition?.callbacks.connectedCallback
      )
        try {
          invoke(
            node,
            definition,
            "connectedCallback",
            [],
            "custom element connected",
          );
          node.__tilefinchCustomElementConnectedState = true;
        } catch (error) {
          report(error, "custom element connected");
        }
    }
  };
  globalThis.__tilefinchCustomElementDisconnected = (root) => {
    /* Only a custom element has a disconnect reaction, and none exists
       before the first definition: skip wrapping the removed subtree. */
    if (!definedAnywhere) return;
    for (const node of descendants(root))
      if (node.__tilefinchCustomElementState === "custom") {
        const definition = definitionFor(node);
        if (node.__tilefinchCustomElementConnectedState) {
        node.__tilefinchCustomElementConnectedState = false;
        if (definition)
          invoke(
            node,
            definition,
            "disconnectedCallback",
            [],
            "custom element disconnected",
          );
        }
        synchronizeFormOwner(node, definition);
        synchronizeDisabled(node, definition);
      }
  };
  globalThis.__tilefinchCustomElementMoved = (root) => {
    if (!definedAnywhere) return;
    for (const node of descendants(root)) {
      if (node.__tilefinchCustomElementState !== "custom") continue;
      const definition = definitionFor(node);
      if (definition?.callbacks.connectedMoveCallback)
        invoke(
          node,
          definition,
          "connectedMoveCallback",
          [],
          "custom element connected move",
        );
    }
  };
  Object.defineProperty(
    globalThis,
    "__tilefinchResyncCustomElementFormState",
    {
      configurable: false,
      enumerable: false,
      writable: false,
      value(root) {
        /* Like disconnect/move, form-state resync cannot affect a node
           before any registry has defined a custom element. Detached
           construction must not wrap the whole subtree at each insertion. */
        if (!definedAnywhere) return;
        for (const node of descendants(root)) {
          const definition = definitionFor(node);
          synchronizeFormOwner(node, definition);
          synchronizeDisabled(node, definition);
        }
      },
    },
  );
  globalThis.__tilefinchCustomElementAttributeChanged = (
    node,
    name,
    oldValue,
    newValue,
    namespace = null,
  ) => {
    const lowerName = String(name).toLowerCase(),
      localName = String(node?.localName || "").toLowerCase();
    if (
      namespace === null &&
      ((localName === "form" && lowerName === "id") ||
        (localName === "fieldset" && lowerName === "disabled"))
    )
      for (const candidate of descendants(
        localName === "fieldset" ? node : document,
      )) {
        const candidateDefinition = definitionFor(candidate);
        synchronizeFormOwner(candidate, candidateDefinition);
        synchronizeDisabled(candidate, candidateDefinition);
      }
    if (oldValue === newValue || node.__tilefinchCustomElementState !== "custom")
      return;
    const pendingAdoption = pendingAdoptions.get(node);
    if (pendingAdoption) {
      pendingAdoptions.delete(node);
      deliveredAdoptions.set(node, pendingAdoption);
      deliverAdoption(node, pendingAdoption.oldDocument, pendingAdoption.newDocument);
    }
    const definition = definitionFor(node);
    if (namespace === null && lowerName === "form")
      synchronizeFormOwner(node, definition);
    if (namespace === null && lowerName === "disabled")
      synchronizeDisabled(node, definition);
    if (
      definition?.observed.includes(name) &&
      definition.callbacks.attributeChangedCallback
    )
      invoke(
        node,
        definition,
        "attributeChangedCallback",
        [
          name,
          oldValue === undefined ? null : oldValue,
          newValue === undefined ? null : newValue,
          namespace,
        ],
        "custom element attribute",
      );
  };
  const pendingAdoptions = new WeakMap(),
    deliveredAdoptions = new WeakMap(),
    deliverAdoption = (
      node,
      oldDocument,
      newDocument,
    ) => {
      if (node.__tilefinchCustomElementState !== "custom") return;
      const definition = definitionFor(node);
      if (!definition) return;
      invoke(
        node,
        definition,
        "adoptedCallback",
        [oldDocument, newDocument],
        "custom element adopted",
      );
      if (
        connected(node) &&
        !node.__tilefinchCustomElementConnectedState &&
        definition.callbacks.connectedCallback
      ) {
        invoke(
          node,
          definition,
          "connectedCallback",
          [],
          "custom element connected",
        );
        node.__tilefinchCustomElementConnectedState = true;
      }
    };
  globalThis.__tilefinchPrepareCustomElementAdoptions = (records) => {
    for (const record of records || [])
      pendingAdoptions.set(record.node, record);
  };
  globalThis.__tilefinchPrepareCustomElementAdoptionTree = (
    root,
    oldDocument,
    newDocument,
  ) => {
    for (const node of descendants(root))
      if (node.__tilefinchCustomElementState === "custom")
        pendingAdoptions.set(node, {
          node,
          oldDocument,
          newDocument,
        });
  };
  globalThis.__tilefinchFinishCustomElementAdoptions = () => {};
  globalThis.__tilefinchCustomElementAdopted = (
    node,
    oldDocument,
    newDocument,
  ) => {
    const pending = pendingAdoptions.get(node);
    if (pending) pendingAdoptions.delete(node);
    const delivered = deliveredAdoptions.get(node);
    if (
      !pending &&
      delivered?.oldDocument === oldDocument &&
      delivered?.newDocument === newDocument
    ) {
      deliveredAdoptions.delete(node);
      return;
    }
    deliverAdoption(
      node,
      pending?.oldDocument || oldDocument,
      pending?.newDocument || newDocument,
    );
  };
  globalThis.__tilefinchCustomElementAdoptedTree = (
    root,
    oldDocument,
    newDocument,
  ) => {
    for (const node of descendants(root))
      globalThis.__tilefinchCustomElementAdopted(
        node,
        oldDocument,
        newDocument,
      );
  };
  const nativeCreateElement = document.createElement.bind(document);
  const nativeCreateElementNS = document.createElementNS.bind(document);
  const createUnupgraded = (localName) => {
    globalThis.__tilefinchCustomElementCreationSuppressed =
      (globalThis.__tilefinchCustomElementCreationSuppressed || 0) + 1;
    try {
      return nativeCreateElement(localName);
    } finally {
      globalThis.__tilefinchCustomElementCreationSuppressed--;
    }
  };
  globalThis.__tilefinchConstructCustomElement = (constructor) => {
    const definition = definitionByConstructor.get(constructor);
    if (!definition)
      throw new TypeError("Illegal constructor");
    const node = (definition.createUnupgraded || createUnupgraded)(
      definition.localName,
    );
    if (definition.localName !== definition.name)
      node.setAttribute("is", definition.name);
    globalThis.__tilefinchPrepareNativePrototype?.(node, definition.prototype);
    Object.setPrototypeOf(node, definition.prototype);
    node.__tilefinchCustomElementState = "custom";
    if (node.__handle !== undefined) __tilefinchSetCustomState(node.__handle, 1);
    return node;
  };
  document.createElement = (tag, options = undefined) => {
    const localName = String(tag).toLowerCase(),
      is =
        options && typeof options === "object" && options.is !== undefined
          ? String(options.is).toLowerCase()
          : "",
      definition = is ? definitions.get(is) : definitions.get(localName);
    /*
     * Keep ordinary element creation on the native bridge. Suppressing the
     * wrapper's restore hook is needed only while constructing a known custom
     * element and needlessly changes handle/cache lifecycle for every div.
     */
    if (!definition) return nativeCreateElement(localName);
    const node = createUnupgraded(localName);
    if (is) node.setAttribute("is", is);
    return upgradeOne(node, definition, true);
  };
  document.createElementNS = (namespace, tag) => {
    const normalizedNamespace = String(namespace),
      localName = String(tag).toLowerCase(),
      definition =
        normalizedNamespace === "http://www.w3.org/1999/xhtml"
          ? definitions.get(localName)
          : undefined;
    if (!definition) return nativeCreateElementNS(namespace, tag);
    globalThis.__tilefinchCustomElementCreationSuppressed =
      (globalThis.__tilefinchCustomElementCreationSuppressed || 0) + 1;
    let node;
    try {
      node = nativeCreateElementNS(namespace, tag);
    } finally {
      globalThis.__tilefinchCustomElementCreationSuppressed--;
    }
    return upgradeOne(node, definition, true);
  };
  const installScopedRegistry = (view, frameDocument) => {
    if (!view || !frameDocument) return;
    if (
      frameDocument.documentElement &&
      frameDocument.documentElement.ownerDocument !== frameDocument
    )
      globalThis.__tilefinchAdoptNodeOwner?.(
        frameDocument.documentElement,
        frameDocument,
      );
    if (!frameDocument.implementation)
      Object.defineProperty(frameDocument, "implementation", {
        configurable: true,
        value: document.implementation,
      });
    const current = registryByDocument.get(frameDocument);
    if (current && scopedRegistryStates.has(current)) {
      view.customElements = current;
      return;
    }
    const nativeCreate = frameDocument.createElement.bind(frameDocument),
      nativeCreateNS = frameDocument.createElementNS.bind(frameDocument),
      createRaw = (localName) => nativeCreate(localName),
      frameRegistry = Object.create(CustomElementRegistry.prototype),
      state = {
        definitions: new Map(),
        constructors: new Map(),
        waiting: new Map(),
        definitionRunning: false,
        document: frameDocument,
        createUnupgraded: createRaw,
      };
    scopedRegistryStates.set(frameRegistry, state);
    registryByDocument.set(frameDocument, frameRegistry);
    frameDocument.createElement = (tag, options = undefined) => {
      const localName = String(tag).toLowerCase(),
        is =
          options && typeof options === "object" && options.is !== undefined
            ? String(options.is).toLowerCase()
            : "",
        definition = state.definitions.get(is || localName);
      if (!definition) return nativeCreate(localName);
      const node = createRaw(localName);
      if (is) node.setAttribute("is", is);
      return upgradeOne(node, definition, true);
    };
    frameDocument.createElementNS = (namespace, tag) => {
      const definition =
        String(namespace) === "http://www.w3.org/1999/xhtml"
          ? state.definitions.get(String(tag).toLowerCase())
          : undefined;
      if (!definition) return nativeCreateNS(namespace, tag);
      return upgradeOne(nativeCreateNS(namespace, tag), definition, true);
    };
    if (typeof frameDocument.write !== "function") {
      let written = "";
      frameDocument.open = () => {
        written = "";
        if (frameDocument.body) frameDocument.body.innerHTML = "";
        return frameDocument;
      };
      frameDocument.write = (...values) => {
        written += values.map(String).join("");
        if (written.length > 256 * 1024)
          throw new DOMException(
            "Document write limit reached",
            "QuotaExceededError",
          );
        if (frameDocument.body) frameDocument.body.innerHTML = written;
      };
      frameDocument.writeln = (...values) =>
        frameDocument.write(...values, "\n");
      frameDocument.close = () => {};
    }
    view.getComputedStyle = (node, pseudo = null) => {
      const base = globalThis.getComputedStyle(node, pseudo);
      let framedColor = "";
      for (const style of frameDocument.querySelectorAll?.("style") || []) {
        const source = String(style.textContent || "");
        for (const match of source.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
          const selector = match[1].trim(),
            color = match[2].match(
              /(?:^|;)\s*color\s*:\s*([^;!]+)(?:!important)?/i,
            );
          if (!color) continue;
          try {
            if (node.matches(selector)) framedColor = color[1].trim();
          } catch (_) {}
        }
      }
      return new Proxy(base, {
        get(target, name) {
          if (name === "color" && framedColor) return framedColor;
          if (name === "getPropertyValue")
            return (property) =>
              String(property).toLowerCase() === "color" && framedColor
                ? framedColor
                : target.getPropertyValue(property);
          return target[name];
        },
      });
    };
    view.customElements = frameRegistry;
  };
  const nativeFrameWindow = globalThis.__tilefinchFrameWindow;
  if (typeof nativeFrameWindow === "function")
    globalThis.__tilefinchFrameWindow = (handle) => {
      const view = nativeFrameWindow(handle);
      installScopedRegistry(view, view?.document);
      return view;
    };
})();
