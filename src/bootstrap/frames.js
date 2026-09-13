/* Nested-frame WindowProxy machinery: per-frame window proxies, the
   same-origin frame evaluator (persistent global bindings with the child
   WindowProxy receiver, with top-level declarations shared across a frame's
   scripts), local srcdoc/blob/about:blank frame loads, frame window
   state driven by native navigation, and window message delivery.

   platform.js installs this module with its private helpers immediately
   after defining them and then deletes the installer, so author code never
   observes a privileged bridge on the global object. This module must be
   evaluated after dom.js and before platform.js. */
globalThis.__tilefinchInstallFrames = (support) => {
  const reflectDescriptor = Reflect.getOwnPropertyDescriptor,
    reflectKeys = Reflect.ownKeys,
    reflectDefine = Reflect.defineProperty,
    reflectDelete = Reflect.deleteProperty,
    reflectPrototype = Reflect.getPrototypeOf,
    reflectSetPrototype = Reflect.setPrototypeOf,
    FrameTypeError = TypeError;
  const {
    wrap,
    encodeFrameMessage,
    decodeFrameMessage,
    trustedStringLower,
    trustedCharCodeAt,
    trustedStringSlice,
    blobForURL,
    blobBytes,
    normalizePostMessageTarget,
    location,
  } = support;
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
    frameScope = globalThis.__tilefinchFrameScope,
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
      frameScope(candidate[1].scope);
      frameWindows.delete(candidate[0]);
    },
    initializeFrameRealm = (
      state,
      handle,
      proxy,
      frameDocument = null,
      frameLocation = null,
    ) => {
      const scope = frameScope();
      /* Publish ownership before any author-replaceable intrinsic runs so the
       * caller can retire a partially initialized realm on failure. */
      state.scope = scope;
      Object.setPrototypeOf(scope, new Proxy(Object.create(null), {
        get: (_target, key) => globalThis[key],
        has: (_target, key) => key in globalThis,
      }));
      scope.document = frameDocument ||
        globalThis.__tilefinchCreateFrameDocument(true);
      scope.location = frameLocation || {
        href: "about:blank",
        protocol: "about:",
        origin: location.origin,
      };
      scope.document.location = scope.location;
      Object.defineProperty(scope.document, "defaultView", {
        configurable: true,
        value: proxy,
      });
      scope.window = proxy;
      scope.self = proxy;
      scope.globalThis = proxy;
      Object.defineProperty(scope, Symbol.toStringTag, {
        configurable: true,
        value: "Window",
      });
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
      Object.defineProperty(scope, "frameElement", {
        configurable: true,
        enumerable: true,
        get() {
          const current = wrap(handle);
          return state.active && state.sameOrigin && current?.isConnected
            ? current
            : null;
        },
      });
      scope.eval = __tilefinchCreateFrameEval(scope, proxy);
      scope.postMessage = function (data, targetOrigin = "/") {
        const current = wrap(handle);
        if (!state.active || !current?.isConnected) return;
        const normalizedTarget = normalizePostMessageTarget(
          targetOrigin,
          location.origin,
        );
        const wire = encodeFrameMessage(data),
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
          wire,
          normalizedTarget,
          globalThis.__tilefinchActiveTaskKind,
          globalThis.__tilefinchActiveTaskSequence,
        );
      };
      return scope;
    },
    replaceFrameRealm = (state, handle, proxy, documentValue, locationValue) => {
      const previous = state.scope;
      if (previous) {
        frameScope(previous);
        state.scope = null;
      }
      return initializeFrameRealm(
        state, handle, proxy, documentValue, locationValue);
    },
    adoptInitialFrameDocument = (state, proxy, documentValue, locationValue) => {
      state.scope.document = documentValue;
      state.scope.location = locationValue;
      documentValue.location = locationValue;
      Object.defineProperty(documentValue, "defaultView", {
        configurable: true,
        value: proxy,
      });
    };
  globalThis.__tilefinchFrameWindow = (handle) => {
    handle = Number(handle);
    let state = frameWindows.get(handle);
    const element = wrap(handle);
    if (!state) {
      if (frameWindows.size >= frameWindowLimit) evictFrameWindow();
      try {
      state = {
        active: !!element?.isConnected,
        sameOrigin: true,
        opaqueOrigin: false,
        managed: false,
        initialAboutBlank: true,
        loadGeneration: 0,
        localSource: null,
        localSrcdoc: null,
        localSandboxScripts: false,
        localSandboxSameOrigin: false,
        proxy: null,
        scope: null,
      };
      let proxy;
      proxy = new Proxy(Object.create(null), {
        has(target, key) {
          if (key === "source" || key === "execute") return false;
          if (state.sameOrigin) return true;
          return (
            key === "closed" ||
            key === "window" ||
            key === "self" ||
            key === "frames" ||
            key === "frameElement" ||
            key === "postMessage" ||
            key === "parent" ||
            key === "top" ||
            key === "opener" ||
            key === "length"
          );
        },
        get(target, key) {
          if (key === "closed") return !state.active;
          if (key === "frameElement") {
            const current = wrap(handle);
            return state.active && state.sameOrigin && current?.isConnected
              ? current
              : null;
          }
          if (key === "document" || key === "location")
            return state.sameOrigin
              ? key in state.scope
                ? state.scope[key]
                : globalThis[key]
              : undefined;
          if (key === "eval")
            /* The sandboxed-scripts flag suppresses scripts owned by the
               child document; it does not hide Window.eval from a
               same-origin parent. Chromium exposes and permits this call
               for sandbox="allow-same-origin". Opaque-origin frames remain
               inaccessible through the same-origin gate below. */
            return state.sameOrigin ? state.scope.eval : undefined;
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
              return state.scope[key];
            return undefined;
          }
          // Window/document identity and messaging do not require a full
          // set of ECMAScript intrinsics. Everything else, including identity,
          // observes the fully initialized original global before reading it.
          if (key !== "window" && key !== "self" && key !== "globalThis" &&
              key !== "parent" && key !== "top" && key !== "opener" &&
              key !== "frames" && key !== "length" && key !== "postMessage" &&
              key !== Symbol.toStringTag)
            frameScope(state.scope, true);
          if (key in state.scope) return state.scope[key];
          return state.sameOrigin ? globalThis[key] : undefined;
        },
        set(target, key, value) {
          if (!state.sameOrigin) return false;
          // The document adapter installs these host bindings without running
          // child script. Neither collides with a deferred ECMAScript global.
          if (key !== "getComputedStyle" && key !== "customElements")
            frameScope(state.scope, true);
          state.scope[key] = value;
          return true;
        },
        getOwnPropertyDescriptor(target, key) {
          if (!state.sameOrigin) throw new FrameTypeError("cross-origin frame");
          frameScope(state.scope, true);
          const descriptor = reflectDescriptor(state.scope, key);
          return descriptor ? { ...descriptor, configurable: true } : undefined;
        },
        ownKeys(target) {
          if (!state.sameOrigin) throw new FrameTypeError("cross-origin frame");
          frameScope(state.scope, true);
          return reflectKeys(state.scope);
        },
        defineProperty(target, key, descriptor) {
          if (!state.sameOrigin) return false;
          frameScope(state.scope, true);
          return reflectDefine(state.scope, key, {
            ...descriptor,
            configurable: true,
          });
        },
        deleteProperty(target, key) {
          if (!state.sameOrigin) return false;
          frameScope(state.scope, true);
          return reflectDelete(state.scope, key);
        },
        getPrototypeOf(target) {
          if (!state.sameOrigin) throw new FrameTypeError("cross-origin frame");
          frameScope(state.scope, true);
          return reflectPrototype(state.scope);
        },
        setPrototypeOf(target, prototype) {
          if (!state.sameOrigin) return false;
          frameScope(state.scope, true);
          return reflectSetPrototype(state.scope, prototype);
        },
        preventExtensions(target) {
          return false;
        },
      });
      state.proxy = proxy;
      initializeFrameRealm(state, handle, proxy);
      frameWindows.set(handle, state);
      } catch (error) {
        if (state?.scope) frameScope(state.scope);
        throw error;
      }
    } else if (!state.managed) state.active = !!element?.isConnected;
    return state.proxy;
  };
  globalThis.__tilefinchLoadLocalFrame = (element, explicitNavigation = false) => {
    if (
      !(element instanceof HTMLIFrameElement) ||
      !element.isConnected
    )
      return;
    const srcdoc = element.getAttribute("srcdoc"),
      fallbackSource = String(element.src || ""),
      source = srcdoc === null ? fallbackSource : "",
      blob = blobForURL(source),
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
    if (!explicitNavigation &&
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
    state.opaqueOrigin = sandboxPolicy.present && !sandboxPolicy.sameOrigin;
    state.sameOrigin = !state.opaqueOrigin;
    const text =
        normalizedSrcdoc !== null ? normalizedSrcdoc
        : blob ? new TextDecoder().decode(blobBytes(blob)) : "",
      standards = /^\s*<!doctype\s+html(?:\s|>)/i.test(text),
      frameDocument = text
        ? new DOMParser().parseFromString(text, "text/html")
        : globalThis.__tilefinchCreateFrameDocument(standards),
      generation = ++state.loadGeneration;
    const frameHref =
        srcdoc !== null ? "about:srcdoc" : source || "about:blank",
      protocolMatch = frameHref.match(/^([A-Za-z][A-Za-z0-9+.-]*:)/);
    const frameLocation = {
      href: frameHref,
      protocol: protocolMatch ? protocolMatch[1].toLowerCase() : "",
      origin: state.opaqueOrigin ? "null" : location.origin,
    };
    if (state.initialAboutBlank && state.sameOrigin)
      adoptInitialFrameDocument(state, proxy, frameDocument, frameLocation);
    else
      replaceFrameRealm(state, Number(element.__handle), proxy,
        frameDocument, frameLocation);
    /* Connecting an iframe without a src merely materializes its initial
     * about:blank Document; it does not consume the first navigation which
     * is required to retain the initial Window global. */
    const initialMaterialization = !explicitNavigation &&
      normalizedSrcdoc === null &&
      (source === "" || source === "about:blank");
    if (!initialMaterialization)
      state.initialAboutBlank = false;
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
    documentCommitted = false,
  ) => {
    handle = Number(handle);
    const proxy = globalThis.__tilefinchFrameWindow(handle),
      state = frameWindows.get(handle);
    state.active = !!active;
    state.managed = true;
    if (!state.active) {
      state.scope.document = null;
      state.scope.location = null;
      state.localSource = null;
      state.localSrcdoc = null;
      state.loadGeneration++;
      return proxy;
    }
    if (state.active && documentCommitted) {
      state.sameOrigin = !!sameOrigin;
      state.opaqueOrigin = !!opaqueOrigin;
    }
    if (state.active && documentCommitted && committedURL !== null) {
      try {
        const parsed = new URL(String(committedURL), location.href);
        const frameLocation = {
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
        if (state.sameOrigin) {
          const frameDocument =
            globalThis.__tilefinchCreateFrameDocument(true);
          if (state.initialAboutBlank)
            adoptInitialFrameDocument(
              state, proxy, frameDocument, frameLocation);
          else
            replaceFrameRealm(
              state, handle, proxy, frameDocument, frameLocation);
        }
        state.initialAboutBlank = false;
      } catch (_) {}
    }
    return proxy;
  };
  globalThis.__tilefinchReceiveMessage = (wire, origin, sourceHandle) =>
    globalThis.__tilefinchRunTask(
      "window-message:source=" + String(sourceHandle),
      () => {
        const source =
          Number(sourceHandle) === -1
            ? globalThis
            : Number(sourceHandle) === -2
            ? null
            : Number(sourceHandle) === 0
            ? globalThis.parent
            : __tilefinchFrameWindow(Number(sourceHandle));
        /* compat.js installs this private factory after frames.js has been
           initialized, but before any author task can deliver a message. */
        const event = globalThis.__tilefinchTrustedEvent(
          globalThis.__tilefinchCreateMessageEvent(
          "message", decodeFrameMessage(wire), String(origin), source, []));
        globalThis.__tilefinchDispatchWindowEventCheckpointed(event);
      },
    );
  globalThis.postMessage = (data, targetOrigin = "/") => {
    const normalizedTarget = normalizePostMessageTarget(
      targetOrigin,
      location.origin,
    ),
      wire = encodeFrameMessage(data);
    if (
      normalizedTarget !== "*" &&
      normalizedTarget !== String(location.origin)
    )
      return;
    setTimeout(
      () =>
        __tilefinchReceiveMessage(
          wire,
          location.origin,
          -1,
        ),
      0,
    );
  };
  return Object.freeze({
    frameWindowCount: () => frameWindows.size,
  });
};
