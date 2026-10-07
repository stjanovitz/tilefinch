(() => {
  const createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap;
  if (typeof createPrivateWeakMap !== "function")
    throw new Error("Private runtime storage is unavailable");

  /* PSP has no speech synthesizer, but exposing the bounded Web Speech shape
     lets feature detection observe an empty engine instead of failing while
     walking an otherwise standard browser surface. */
  const speechToken = {};
  const handlerSlots = createPrivateWeakMap();
  class SpeechSynthesisVoice {
    constructor(value) {
      if (value !== speechToken) throw new TypeError("Illegal constructor");
    }
  }
  class SpeechSynthesisEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.utterance = init.utterance || null;
      this.charIndex = Math.max(0, Number(init.charIndex) || 0);
      this.charLength = Math.max(0, Number(init.charLength) || 0);
      this.elapsedTime = Math.max(0, Number(init.elapsedTime) || 0);
      this.name = String(init.name || "");
    }
  }
  class SpeechSynthesisErrorEvent extends SpeechSynthesisEvent {
    constructor(type, init = {}) {
      super(type, init);
      this.error = String(init.error || "synthesis-unavailable");
    }
  }
  const eventAttributes = [
    "start",
    "end",
    "error",
    "pause",
    "resume",
    "mark",
    "boundary",
  ];
  class SpeechSynthesisUtterance extends EventTarget {
    constructor(text = "") {
      super();
      this.text = String(text);
      this.lang = "";
      this.voice = null;
      this.volume = 1;
      this.rate = 1;
      this.pitch = 1;
      const slots = Object.create(null);
      handlerSlots.set(this, slots);
      for (const type of eventAttributes) slots[type] = null;
    }
  }
  for (const type of eventAttributes)
    Object.defineProperty(SpeechSynthesisUtterance.prototype, "on" + type, {
      configurable: true,
      enumerable: true,
      get() {
        return handlerSlots.get(this)?.[type] || null;
      },
      set(value) {
        const slots = handlerSlots.get(this);
        if (!slots) throw new TypeError("Illegal invocation");
        const prior = slots[type];
        if (prior) this.removeEventListener(type, prior);
        slots[type] = typeof value === "function" ? value : null;
        if (slots[type]) this.addEventListener(type, slots[type]);
      },
    });
  class SpeechSynthesis extends EventTarget {
    constructor(value) {
      if (value !== speechToken) throw new TypeError("Illegal constructor");
      super();
      this.onvoiceschanged = null;
    }
    get pending() {
      return false;
    }
    get speaking() {
      return false;
    }
    get paused() {
      return false;
    }
    getVoices() {
      return [];
    }
    cancel() {}
    pause() {}
    resume() {}
    speak(utterance) {
      if (!(utterance instanceof SpeechSynthesisUtterance))
        throw new TypeError("SpeechSynthesisUtterance required");
      setTimeout(
        () =>
          utterance.dispatchEvent(
            new SpeechSynthesisErrorEvent("error", {
              utterance,
              error: "synthesis-unavailable",
            }),
          ),
        0,
      );
    }
  }
  for (const [constructor, tag] of [
    [SpeechSynthesis, "SpeechSynthesis"],
    [SpeechSynthesisVoice, "SpeechSynthesisVoice"],
    [SpeechSynthesisEvent, "SpeechSynthesisEvent"],
    [SpeechSynthesisErrorEvent, "SpeechSynthesisErrorEvent"],
    [SpeechSynthesisUtterance, "SpeechSynthesisUtterance"],
  ])
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: tag,
    });
  globalThis.SpeechSynthesis = SpeechSynthesis;
  globalThis.SpeechSynthesisVoice = SpeechSynthesisVoice;
  globalThis.SpeechSynthesisEvent = SpeechSynthesisEvent;
  globalThis.SpeechSynthesisErrorEvent = SpeechSynthesisErrorEvent;
  globalThis.SpeechSynthesisUtterance = SpeechSynthesisUtterance;
  Object.defineProperty(globalThis, "speechSynthesis", {
    configurable: true,
    enumerable: true,
    value: new SpeechSynthesis(speechToken),
  });

  /* Tilefinch does not expose the Media Source byte-stream pipeline. Keep
     standards capability probes well-formed without advertising a type that
     addSourceBuffer cannot actually accept. */
  const mediaToken = {};
  const mediaSourceState = createPrivateWeakMap();
  const requireMediaSource = (source) => {
    if (mediaSourceState.get(source) !== true)
      throw new TypeError("Illegal invocation");
  };
  class SourceBuffer extends EventTarget {
    constructor(value) {
      if (value !== mediaToken) throw new TypeError("Illegal constructor");
      super();
    }
  }
  class SourceBufferList extends EventTarget {
    constructor(value) {
      if (value !== mediaToken) throw new TypeError("Illegal constructor");
      super();
    }
    get length() {
      return 0;
    }
  }
  const emptyBuffers = new SourceBufferList(mediaToken);
  class MediaSource extends EventTarget {
    constructor() {
      super();
      mediaSourceState.set(this, true);
    }
    get sourceBuffers() {
      requireMediaSource(this);
      return emptyBuffers;
    }
    get activeSourceBuffers() {
      requireMediaSource(this);
      return emptyBuffers;
    }
    get readyState() {
      requireMediaSource(this);
      return "closed";
    }
    get duration() {
      requireMediaSource(this);
      return NaN;
    }
    set duration(_) {
      requireMediaSource(this);
      throw new DOMException("MediaSource is not open", "InvalidStateError");
    }
    addSourceBuffer(type) {
      requireMediaSource(this);
      String(type);
      throw new DOMException("MediaSource is not open", "InvalidStateError");
    }
    removeSourceBuffer(_) {
      requireMediaSource(this);
      throw new DOMException("SourceBuffer was not found", "NotFoundError");
    }
    endOfStream() {
      requireMediaSource(this);
      throw new DOMException("MediaSource is not open", "InvalidStateError");
    }
    setLiveSeekableRange() {
      requireMediaSource(this);
      throw new DOMException("MediaSource is not open", "InvalidStateError");
    }
    clearLiveSeekableRange() {
      requireMediaSource(this);
      throw new DOMException("MediaSource is not open", "InvalidStateError");
    }
    static isTypeSupported(type) {
      String(type);
      return false;
    }
  }
  Object.defineProperty(MediaSource, "canConstructInDedicatedWorker", {
    enumerable: true,
    value: false,
  });
  for (const [constructor, tag] of [
    [MediaSource, "MediaSource"],
    [SourceBuffer, "SourceBuffer"],
    [SourceBufferList, "SourceBufferList"],
  ])
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: tag,
    });
  globalThis.MediaSource = MediaSource;
  globalThis.SourceBuffer = SourceBuffer;
  globalThis.SourceBufferList = SourceBufferList;

  /* document.fonts. Pages cannot create FontFace objects here, so every
     document's set is empty and settled: ready is resolved, check() is true
     (no face in the set needs loading) and load() resolves with no faces.
     Page @font-face rules still load natively; they are just not exposed as
     FontFace objects. The loading events never fire. */
  const fontSetToken = {},
    fontSets = createPrivateWeakMap(),
    fontSetSlots = createPrivateWeakMap(),
    /* The cascade's own font shorthand grammar. */
    fontShorthandValid = globalThis.__tilefinchFontShorthandValid;
  const requireFontSet = (value) => {
    const slots = fontSetSlots.get(value);
    if (!slots) throw new TypeError("Illegal invocation");
    return slots;
  };
  const parseFont = (font) => {
    if (!fontShorthandValid(String(font)))
      throw new DOMException("Could not parse font", "SyntaxError");
  };
  class FontFaceSet extends EventTarget {
    constructor(token) {
      if (token !== fontSetToken) throw new TypeError("Illegal constructor");
      super();
      const slots = { loading: null, loadingdone: null, loadingerror: null };
      fontSetSlots.set(this, slots);
      slots.ready = Promise.resolve(this);
    }
    get ready() { return requireFontSet(this).ready; }
    get status() { requireFontSet(this); return "loaded"; }
    get size() { requireFontSet(this); return 0; }
    add(face) {
      requireFontSet(this);
      throw new TypeError("Argument 1 is not a FontFace");
    }
    delete(face) { requireFontSet(this); return false; }
    has(face) { requireFontSet(this); return false; }
    clear() { requireFontSet(this); }
    check(font, text) { requireFontSet(this); parseFont(font); return true; }
    load(font, text) {
      try {
        requireFontSet(this);
        parseFont(font);
      } catch (error) {
        return Promise.reject(error);
      }
      return Promise.resolve([]);
    }
    forEach(callback, thisArg) {
      requireFontSet(this);
      if (typeof callback !== "function")
        throw new TypeError("callback is not a function");
    }
    values() { requireFontSet(this); return [].values(); }
  }
  FontFaceSet.prototype.keys = FontFaceSet.prototype.values;
  FontFaceSet.prototype.entries = FontFaceSet.prototype.values;
  FontFaceSet.prototype[Symbol.iterator] = FontFaceSet.prototype.values;
  for (const type of ["loading", "loadingdone", "loadingerror"])
    Object.defineProperty(FontFaceSet.prototype, "on" + type, {
      configurable: true,
      enumerable: true,
      get() { return requireFontSet(this)[type]; },
      set(value) {
        requireFontSet(this)[type] = typeof value === "function" ? value : null;
      },
    });
  const DocumentInterface = globalThis.Document;
  if (typeof DocumentInterface === "function")
    Object.defineProperty(DocumentInterface.prototype, "fonts", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof DocumentInterface))
          throw new TypeError("Illegal invocation");
        let set = fontSets.get(this);
        if (!set) fontSets.set(this, (set = new FontFaceSet(fontSetToken)));
        return set;
      },
    });

  /* Event Timing's interface, for feature detection. Tilefinch records no
     event or first-input entries, so PerformanceObserver.supportedEntryTypes
     does not list them and no instance ever exists. */
  class PerformanceEventTiming extends PerformanceEntry {
    constructor() {
      throw new TypeError("Illegal constructor");
    }
  }
  const noEventTiming = function () {
    throw new TypeError("Illegal invocation");
  };
  for (const name of [
    "processingStart", "processingEnd", "cancelable", "target",
    "interactionId",
  ])
    Object.defineProperty(PerformanceEventTiming.prototype, name, {
      configurable: true,
      enumerable: true,
      get: noEventTiming,
    });
  PerformanceEventTiming.prototype.toJSON = noEventTiming;
  for (const [constructor, tag] of [
    [FontFaceSet, "FontFaceSet"],
    [PerformanceEventTiming, "PerformanceEventTiming"],
  ])
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: tag,
    });
  globalThis.FontFaceSet = FontFaceSet;
  globalThis.PerformanceEventTiming = PerformanceEventTiming;
})();
