// Site-census API probe (lab only). Loaded with
//   TILEFINCH_LAB_INIT_SCRIPT=tools/site-census/api-probe.js
// into each top-level page realm after the bootstrap and before author code.
//
// It records which properties page code reads ([[Get]]) or tests ([[Has]],
// `in`, `typeof` of a global) on platform objects and does not find. A
// transparent Proxy "sentinel" is spliced into the prototype chain just above
// each probed prototype; a lookup only reaches a sentinel after it has missed
// on the object and every prototype below, so hits on own or lower properties
// never run the trap. The sentinel forwards every operation to the original
// parent, and reports that parent from getPrototypeOf, so `instanceof`,
// inherited lookups and assignments behave as before.
//
// Blind spot: element.style is a bootstrap Proxy that answers `in` from its
// own table, so `'prop' in element.style` never reaches a sentinel.
//
// Known perturbations, which is why this runs in its own replay pass and never
// in the timing pass: lookups that walk past a sentinel are slower, and a bare
// read of an undeclared global returns undefined instead of throwing a
// ReferenceError (QuickJS forwards that lookup through the global object's
// prototype chain without a throw flag).
//
// The report is read back by the census command script:
//   js __tfApiProbe.chunk(N)    -> Nth 900-character slice of the JSON report
(() => {
  'use strict';
  const G = globalThis;
  if (G.__tfApiProbe) return;
  const R = Reflect;
  const getProto = Object.getPrototypeOf;
  const ProxyC = Proxy;
  const KEY = /^[A-Za-z][A-Za-z0-9_]{1,47}$/;
  const test = RegExp.prototype.test;
  const LIMIT = 3000;
  const counts = new Map();
  const masks = new Map();
  const statics = new Map();
  const labelled = new WeakSet();
  let depth = 0;
  let installed = 0;
  let refused = 0;
  let total = 0;
  let dropped = 0;

  // Lowercase globals that are web platform APIs (everything else lowercase
  // on window is a page global and is not recorded).
  const LOWER_GLOBALS = new Set([
    'requestIdleCallback', 'cancelIdleCallback', 'queueMicrotask',
    'structuredClone', 'matchMedia', 'fetch', 'createImageBitmap',
    'reportError', 'scheduler', 'cookieStore', 'navigation',
    'visualViewport', 'crossOriginIsolated', 'isSecureContext',
    'trustedTypes', 'caches', 'indexedDB', 'launchQueue',
    'documentPictureInPicture', 'showOpenFilePicker', 'showSaveFilePicker',
    'getScreenDetails', 'origin', 'requestAnimationFrame',
    'cancelAnimationFrame', 'speechSynthesis', 'customElements',
    'localStorage', 'sessionStorage', 'crypto', 'performance', 'screen',
    'history', 'location', 'navigator', 'clientInformation', 'external',
    'devicePixelRatio', 'getComputedStyle', 'getSelection', 'postMessage',
    'atob', 'btoa', 'setTimeout', 'setInterval', 'clearTimeout',
    'clearInterval', 'open', 'close', 'print', 'alert', 'confirm', 'prompt',
    'scrollTo', 'scrollBy', 'scroll', 'innerWidth', 'innerHeight',
    'outerWidth', 'outerHeight', 'screenX', 'screenY', 'pageXOffset',
    'pageYOffset', 'scrollX', 'scrollY', 'frames', 'parent', 'top', 'opener',
    'frameElement', 'self', 'window', 'globalThis', 'name', 'status',
    'closed', 'length', 'event', 'onerror', 'onload', 'onunhandledrejection',
    'chrome', 'opera', 'safari', 'webkitURL', 'webkitRequestAnimationFrame',
    'mozRequestAnimationFrame', 'msRequestAnimationFrame',
    'webkitIndexedDB', 'mozIndexedDB', 'msIndexedDB', 'attachEvent',
    'detachEvent', 'webkitAudioContext', 'ontouchstart', 'onpointerdown',
    'onwheel', 'onmessage', 'onpopstate', 'onhashchange', 'onbeforeunload',
    'onpagehide', 'onpageshow', 'onresize', 'onscroll', 'onfocus', 'onblur',
    'onorientationchange', 'orientation', 'ondevicemotion',
    'ondeviceorientation', 'onanimationend', 'ontransitionend',
    'webkitSpeechRecognition', 'credentialless', 'originAgentCluster',
    'sharedStorage', 'fence', 'ai', 'translation', 'webkitStorageInfo',
    'menubar', 'toolbar', 'locationbar', 'personalbar', 'scrollbars',
    'statusbar', 'getMatchedCSSRules', 'find', 'stop', 'focus', 'blur',
    'moveTo', 'moveBy', 'resizeTo', 'resizeBy', 'webkitCancelAnimationFrame',
  ]);

  // The first stack frame outside this probe: the code that did the lookup.
  // Browser bootstrap code (lazily installed features probing the global
  // before defining a name, for example) is not page code and is skipped.
  const ErrorC = Error;
  const split = String.prototype.split;
  // Frames: the Error constructor and natives first, then the code that did
  // the lookup. A lookup made by browser bootstrap code (a lazily installed
  // feature probing the global before defining a name) has no source
  // location there; page code has a URL or an inline-script name.
  const caller = () => {
    const stack = String(new ErrorC().stack || '');
    const frames = R.apply(split, stack, ['\n']);
    for (let i = 0; i < frames.length; i++) {
      const frame = frames[i].trim();
      if (frame === '' || frame === 'at Error' || frame.indexOf('<lab-init-script>') >= 0
          || frame.indexOf('(native)') >= 0) continue;
      return frame;
    }
    return '';
  };
  const where = new Map();
  let bootstrapSkipped = 0;
  const skippedSample = [];
  const PAGE_FRAME = /https?:|<inline-script>|<eval>|<loop-js>|#inline/;
  const record = (label, key, mask) => {
    if (label === undefined || !R.apply(test, KEY, [key])) return;
    if (label === 'window' && !LOWER_GLOBALS.has(key)) {
      const first = key.charCodeAt(0);
      if (first < 65 || first > 90) return;
    }
    const frame = caller();
    if (!R.apply(test, PAGE_FRAME, [frame])) {
      bootstrapSkipped++;
      if (skippedSample.length < 12) skippedSample.push(label + '.' + key + ' @ ' + frame.slice(0, 120));
      return;
    }
    total++;
    const name = label + '.' + key;
    const count = counts.get(name);
    if (count === undefined) {
      if (counts.size >= LIMIT) { dropped++; return; }
      counts.set(name, 1);
      masks.set(name, mask);
      where.set(name, frame.slice(0, 160));
    } else {
      counts.set(name, count + 1);
      masks.set(name, masks.get(name) | mask);
    }
  };

  const sentinel = (target, label, receiverLabel) => new ProxyC(target, {
    get(t, key, receiver) {
      if (depth !== 0 || typeof key !== 'string') return R.get(t, key, receiver);
      depth++;
      try {
        if (!R.has(t, key)) {
          const owner = receiverLabel ? statics.get(receiver) : label;
          record(receiverLabel && owner !== undefined ? 'static:' + owner : owner, key, 1);
        }
        return R.get(t, key, receiver);
      } finally {
        depth--;
      }
    },
    has(t, key) {
      if (depth !== 0 || typeof key !== 'string' || receiverLabel)
        return R.has(t, key);
      depth++;
      try {
        const present = R.has(t, key);
        if (!present) record(label, key, 2);
        return present;
      } finally {
        depth--;
      }
    },
    getPrototypeOf(t) { return t; },
  });

  // Splice a sentinel between `object` and its current prototype.
  const installAbove = (object, label, receiverLabel = false) => {
    if (object === null || (typeof object !== 'object' && typeof object !== 'function'))
      return;
    if (labelled.has(object)) return;
    const parent = getProto(object);
    if (parent === null) return;
    labelled.add(object);
    if (R.setPrototypeOf(object, sentinel(parent, label, receiverLabel))) installed++;
    else refused++;
  };

  const objectProto = Object.prototype;
  depth++;  // installation lookups are not page lookups
  const eventTargetProto = typeof G.EventTarget === 'function'
    ? G.EventTarget.prototype : null;
  // Family prototypes keep element misses attributed to the interface a
  // page actually uses instead of the base-most one.
  const FAMILIES = [
    'Node', 'Element', 'HTMLElement', 'SVGElement', 'Document',
    'DocumentFragment', 'ShadowRoot', 'CharacterData', 'Text', 'Attr',
    'HTMLMediaElement', 'HTMLVideoElement', 'HTMLImageElement',
    'HTMLCanvasElement', 'HTMLInputElement', 'HTMLFormElement',
    'HTMLIFrameElement', 'HTMLDialogElement', 'HTMLTemplateElement',
    'HTMLAnchorElement', 'HTMLScriptElement', 'HTMLSelectElement',
    'HTMLTextAreaElement', 'HTMLButtonElement', 'HTMLLinkElement',
    'HTMLStyleElement', 'HTMLSlotElement', 'HTMLDetailsElement',
    'UIEvent', 'MouseEvent', 'KeyboardEvent', 'PointerEvent', 'TouchEvent',
    'CustomEvent', 'MessagePort', 'Window',
  ];
  const names = Object.getOwnPropertyNames(G);
  for (const name of names) {
    let value;
    try {
      const descriptor = R.getOwnPropertyDescriptor(G, name);
      if (!descriptor || !('value' in descriptor)) continue;
      value = descriptor.value;
    } catch (_) {
      continue;
    }
    if (typeof value === 'function') {
      statics.set(value, name);
      const proto = value.prototype;
      if (proto !== null && typeof proto === 'object') {
        const parent = getProto(proto);
        if (parent === objectProto || parent === eventTargetProto)
          installAbove(proto, name);
      }
    } else if (value !== null && typeof value === 'object'
               && getProto(value) === objectProto
               && /^(CSS|Intl|Math|JSON|Reflect|Atomics|WebAssembly|console)$/.test(name)) {
      installAbove(value, name);
    }
  }
  for (const name of FAMILIES) {
    const ctor = G[name];
    if (typeof ctor === 'function' && ctor.prototype) installAbove(ctor.prototype, name);
  }
  // Singletons whose interface object may not be exposed as a global.
  for (const name of ['navigator', 'location', 'history', 'screen', 'performance',
    'document', 'visualViewport', 'localStorage', 'sessionStorage', 'crypto',
    'customElements', 'speechSynthesis', 'indexedDB', 'caches', 'scheduler',
    'navigation', 'cookieStore', 'trustedTypes', 'external']) {
    let object;
    try { object = G[name]; } catch (_) { continue; }
    if (object === null || typeof object !== 'object') continue;
    let proto = getProto(object);
    if (proto === objectProto) { installAbove(object, name); continue; }
    while (proto !== null && !labelled.has(proto)) {
      const parent = getProto(proto);
      if (parent === objectProto || parent === eventTargetProto) {
        const ctor = R.getOwnPropertyDescriptor(proto, 'constructor');
        const label = ctor && typeof ctor.value === 'function' && ctor.value.name
          ? ctor.value.name : name;
        installAbove(proto, label);
        break;
      }
      proto = parent;
    }
  }
  // Statics on constructors (Promise.withResolvers, Array.fromAsync, ...):
  // misses on any function reach Function.prototype's sentinel, which records
  // only receivers that are global constructors.
  installAbove(Function.prototype, '<function>', true);
  // Globals last, so the walk above never trips the global sentinel.
  installAbove(G, 'window');
  depth--;

  const report = () => {
    const entries = [...counts.entries()].sort((a, b) => b[1] - a[1]);
    const misses = {};
    for (const [name, count] of entries)
      misses[name] = [count, masks.get(name), where.get(name)];
    return JSON.stringify({ installed, refused, total, dropped, bootstrapSkipped, skippedSample, misses });
  };
  let frozen = null;
  Object.defineProperty(G, '__tfApiProbe', {
    configurable: true,
    enumerable: false,
    value: Object.freeze({
      chunk(index) {
        if (frozen === null) frozen = report();
        return frozen.slice(index * 900, (index + 1) * 900);
      },
      reset() { frozen = null; },
    }),
  });
})();
