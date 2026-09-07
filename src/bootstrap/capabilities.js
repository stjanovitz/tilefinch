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
})();
