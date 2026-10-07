(() => {
  /* A deliberately small Web Audio subset for game effects. Decoded PCM is
     native and budget-charged; JavaScript retains only fixed metadata and a
     four-voice graph. The context never starts audio without trusted input. */
  const nativeDecode = globalThis.__tilefinchGameAudioDecode,
    nativeCommand = globalThis.__tilefinchGameAudioCommand,
    nodeLimit = 16,
    contextStates = new WeakMap(),
    nodeMixSources = new WeakMap(),
    stateFor = (context) => contextStates.get(context),
    sourcesByVoice = new Map();
  /* Command opcodes: TilefinchGameAudioCommand in
     include/tilefinch/game_audio.h, without its prefix. */
  const RESUME = 0, SUSPEND = 1, CLOSE = 2, START_BUFFER = 3, STOP = 4,
    START_OSCILLATOR = 5, SET_GAIN = 6, SET_FREQUENCY = 7, GAIN_TARGET = 8,
    CANCEL_GAIN = 9, CANCEL_PITCH = 10, CURVE = 11;
  let activeContext = null;
  const refreshSourceMix = (source) => source._updateMix();
  const sourcesThrough = (context, node) => {
    const cache = nodeMixSources.get(node);
    const revision = stateFor(context).mixGraphRevision;
    // Retain bounded graph membership, never gain/pan values or the mix.
    // Keep this record private: exposing it would expose loop bounds too.
    if (cache.revision !== revision) {
      cache.sources.fill(null);
      cache.count = 0;
      for (const source of sourcesByVoice.values()) {
        if (source.context !== context || !source._voice) continue;
        let target = source._target, visits = 0;
        while (target && visits++ < 4) {
          if (target === node) {
            if (cache.count < cache.sources.length)
              cache.sources[cache.count++] = source;
            break;
          }
          target = target._target;
        }
      }
      cache.revision = revision;
    }
    return cache;
  };
  class AudioParam {
    constructor(value, minimum, maximum, changed = null,
                target = null, cancel = null, curve = null) {
      this._value = value;
      this._changed = changed;
      this._target = target;
      this._cancel = cancel;
      this._curve = curve;
      this.minValue = minimum;
      this.maxValue = maximum;
      this.defaultValue = value;
    }
    get value() { return this._value; }
    set value(value) {
      value = Number(value);
      if (Number.isFinite(value))
        this._value = Math.max(this.minValue, Math.min(this.maxValue, value));
      if (this._changed) this._changed();
    }
    setValueAtTime(value) { this.value = value; return this; }
    linearRampToValueAtTime(value) { this.value = value; return this; }
    exponentialRampToValueAtTime(value) { this.value = value; return this; }
    setTargetAtTime(value, when, timeConstant) {
      value = Number(value); timeConstant = Number(timeConstant);
      if (!Number.isFinite(value) || !Number.isFinite(timeConstant)
          || timeConstant <= 0) throw new TypeError(
        "Audio target and time constant must be finite");
      value = Math.max(this.minValue, Math.min(this.maxValue, value));
      if (this._target) this._target(value, when, timeConstant);
      else this.value = value;
      return this;
    }
    cancelScheduledValues(when = 0) {
      if (this._cancel) this._cancel(when);
      return this;
    }
    setValueCurveAtTime(values, when, duration) {
      when = Number(when); duration = Number(duration);
      if (!Number.isFinite(when) || when < 0
          || !Number.isFinite(duration) || duration <= 0)
        throw new RangeError("Curve time and duration are out of range");
      if (!this._curve) throw new DOMException(
        "Curves are supported on gain and oscillator frequency only",
        "NotSupportedError");
      if (!(values instanceof Float32Array)) {
        const points = [];
        for (const value of values) {
          if (points.length === 64) throw new DOMException(
            "Audio curves are limited to 64 samples", "QuotaExceededError");
          points.push(Number(value));
        }
        values = new Float32Array(points);
      }
      if (values.length < 2) throw new DOMException(
        "An audio curve needs at least two samples", "InvalidStateError");
      if (values.length > 64) throw new DOMException(
        "Audio curves are limited to 64 samples", "QuotaExceededError");
      // Native collection validates the copied float32 samples, including
      // finiteness. Do not interpret a second per-point validation loop.
      this._curve(values, when, duration);
      return this;
    }
  }

  class AudioNode extends EventTarget {
    constructor(context) {
      super();
      const state = stateFor(context);
      if (!state || state.closed) throw new DOMException(
        "AudioContext is closed", "InvalidStateError");
      if (state.nodeCount >= nodeLimit) throw new DOMException(
        "Audio node limit reached", "QuotaExceededError");
      state.nodeCount++;
      this.context = context;
      this._target = null;
      this.channelCount = 2;
      nodeMixSources.set(this, { revision: -1, count: 0,
        sources: new Array(4).fill(null) });
    }
    connect(target) {
      if (!(target instanceof AudioNode) || target.context !== this.context)
        throw new TypeError("Audio nodes must share a context");
      this._target = target;
      stateFor(this.context).mixGraphRevision++;
      this.context._refreshActive?.();
      return target;
    }
    disconnect() {
      this._target = null;
      stateFor(this.context).mixGraphRevision++;
      this.context._refreshActive?.();
    }
  }

  class AudioDestinationNode extends AudioNode {
    constructor(context) { super(context); this.maxChannelCount = 2; }
  }

  class GainNode extends AudioNode {
    constructor(context, options = {}) {
      super(context);
      this.gain = new AudioParam(
        Number(options.gain ?? 1), 0, 4,
        () => context._forSourcesThrough(this, refreshSourceMix),
        (value, when, timeConstant) =>
          context._scheduleGainTarget(this, value, when, timeConstant),
        (when) => context._cancelGainTargets(this, when),
        (values, when, duration) =>
          context._scheduleGainCurve(this, values, when, duration),
      );
    }
  }

  class StereoPannerNode extends AudioNode {
    constructor(context, options = {}) {
      super(context);
      this.pan = new AudioParam(
        Number(options.pan ?? 0), -1, 1,
        () => context._forSourcesThrough(this, refreshSourceMix),
      );
    }
  }

  const boundedDelay = (context, when) => {
    when = Number(when);
    if (!Number.isFinite(when) || when < 0) throw new TypeError(
      "Audio time must be a finite nonnegative number");
    /* currentTime never decreases, so a time at or before the last value it
       returned is due now: the delay is exactly zero without another clock
       read. An envelope scheduled "now" otherwise samples the clock three
       times, about a tenth of a millisecond each on the PSP. */
    if (when <= stateFor(context).lastTime) return 0;
    const delay = Math.max(0, when - context.currentTime);
    if (delay > 10) throw new DOMException(
      "Scheduling exceeds the bounded ten-second window", "NotSupportedError");
    return delay;
  };

  const sourceMix = (source, overrideNode = null, overrideGain = 0) => {
    let gain = 1, pan = 0, hasPanner = false;
    let target = source._target, visits = 0;
    while (target && visits++ < 4) {
      if (target instanceof GainNode)
        gain *= target === overrideNode ? overrideGain : target.gain.value;
      if (target instanceof StereoPannerNode) {
        hasPanner = true;
        pan = Math.max(-1, Math.min(1, pan + target.pan.value));
      }
      target = target._target;
    }
    gain = Math.max(0, Math.min(4, gain));
    if (!hasPanner) return [gain, gain];
    const angle = (pan + 1) * Math.PI / 4;
    return [
      Math.max(0, Math.min(4, gain * Math.cos(angle))),
      Math.max(0, Math.min(4, gain * Math.sin(angle))),
    ];
  };

  class AudioBuffer {
    constructor(token, context) {
      if (!token || !Number.isSafeInteger(Number(token.handle)))
        throw new TypeError("AudioBuffer is created by decodeAudioData");
      this._handle = Number(token.handle) >>> 0;
      this._context = context;
      this.length = Number(token.length) >>> 0;
      this.sampleRate = Number(token.sampleRate) >>> 0;
      this.numberOfChannels = Number(token.numberOfChannels) >>> 0;
      this.duration = this.sampleRate ? this.length / this.sampleRate : 0;
    }
    getChannelData() {
      throw new DOMException(
        "Decoded PCM stays in the bounded native audio pool",
        "NotSupportedError",
      );
    }
    copyFromChannel() {
      throw new DOMException("PCM readback is unavailable", "NotSupportedError");
    }
    copyToChannel() {
      throw new DOMException("PCM mutation is unavailable", "NotSupportedError");
    }
  }

  class AudioBufferSourceNode extends AudioNode {
    constructor(context, options = {}) {
      super(context);
      this.buffer = options.buffer || null;
      this.loop = !!options.loop;
      this.loopStart = Math.max(0, Number(options.loopStart ?? 0) || 0);
      this.loopEnd = Math.max(0, Number(options.loopEnd ?? 0) || 0);
      this.playbackRate = new AudioParam(
        Number(options.playbackRate ?? 1), .25, 4);
      this.detune = new AudioParam(Number(options.detune ?? 0), -1200, 1200);
      this.onended = null;
      this._started = false;
      this._voice = 0;
    }
    start(when = 0, offset = 0, duration = 0) {
      if (stateFor(this.context).closed) throw new DOMException(
        "AudioContext is closed", "InvalidStateError");
      if (this._started) throw new DOMException(
        "AudioBufferSourceNode may start only once", "InvalidStateError");
      if (!(this.buffer instanceof AudioBuffer) ||
          this.buffer._context !== this.context) throw new DOMException(
        "No decoded AudioBuffer is attached", "InvalidStateError");
      const delay = boundedDelay(this.context, when);
      const [left, right] = sourceMix(this);
      const voice = nativeCommand(
        START_BUFFER, this.buffer._handle, Number(offset), Number(duration),
        this.playbackRate.value * Math.pow(2, this.detune.value / 1200),
        left, right, this.loop, this.loopStart, this.loopEnd, delay,
      );
      if (!voice) throw new DOMException(
        "No game-audio voice is available", "QuotaExceededError");
      this._voice = Number(voice) >>> 0;
      sourcesByVoice.set(this._voice, this);
      stateFor(this.context).mixGraphRevision++;
      this._started = true;
    }
    stop(when = 0) {
      if (!this._started) throw new DOMException(
        "AudioBufferSourceNode has not started", "InvalidStateError");
      if (this._voice)
        nativeCommand(STOP, this._voice, boundedDelay(this.context, when));
    }
    _updateMix() {
      if (!this._voice) return;
      const [left, right] = sourceMix(this);
      nativeCommand(SET_GAIN, this._voice, left, right);
    }
  }

  const oscillatorTypes = ["sine", "square", "sawtooth", "triangle"];

  class OscillatorNode extends AudioNode {
    constructor(context, options = {}) {
      super(context);
      this._type = "sine";
      this.type = options.type ?? "sine";
      this.onended = null;
      this._started = false;
      this._voice = 0;
      const update = () => this._updatePitch();
      this.frequency = new AudioParam(
        Number(options.frequency ?? 440), 1, 20000, update, null,
        (when) => {
          if (boundedDelay(context, when) > .001) throw new DOMException(
            "Future automation cancellation is unavailable", "NotSupportedError");
          if (this._voice) nativeCommand(CANCEL_PITCH, this._voice);
        },
        (values, when, duration) => {
          const delay = boundedDelay(context, when);
          if (delay + duration > 10 || this.detune.value !== 0)
            throw new DOMException("Pitch curve exceeds the bounded profile",
              "NotSupportedError");
          if (!this._voice || !nativeCommand(
              CURVE, this._voice, 1, values, 1, 1, delay, duration))
            throw new DOMException("Pitch curve needs a live voice and empty timeline",
              "NotSupportedError");
        });
      this.detune = new AudioParam(
        Number(options.detune ?? 0), -1200, 1200, update);
    }
    get type() { return this._type; }
    set type(value) {
      value = String(value).toLowerCase();
      if (!oscillatorTypes.includes(value)) throw new DOMException(
        "Unsupported oscillator type", "InvalidStateError");
      this._type = value;
    }
    _effectiveFrequency() {
      const detune = this.detune.value;
      return detune === 0 ? this.frequency.value
        : this.frequency.value * Math.pow(2, detune / 1200);
    }
    start(when = 0) {
      if (this._started) throw new DOMException(
        "OscillatorNode may start only once", "InvalidStateError");
      const delay = boundedDelay(this.context, when);
      const [left, right] = sourceMix(this);
      const voice = nativeCommand(
        START_OSCILLATOR, oscillatorTypes.indexOf(this.type) + 1,
        this._effectiveFrequency(), left, right, delay,
      );
      if (!voice) throw new DOMException(
        "No game-audio voice is available", "QuotaExceededError");
      this._voice = Number(voice) >>> 0;
      sourcesByVoice.set(this._voice, this);
      stateFor(this.context).mixGraphRevision++;
      this._started = true;
    }
    stop(when = 0) {
      if (!this._started) throw new DOMException(
        "OscillatorNode has not started", "InvalidStateError");
      if (this._voice)
        nativeCommand(STOP, this._voice, boundedDelay(this.context, when));
    }
    _updateMix() {
      if (!this._voice) return;
      const [left, right] = sourceMix(this);
      nativeCommand(SET_GAIN, this._voice, left, right);
    }
    _updatePitch() {
      if (this._voice)
        nativeCommand(SET_FREQUENCY, this._voice, this._effectiveFrequency());
    }
  }

  class BaseAudioContext extends EventTarget {
    constructor() {
      super();
      contextStates.set(this, { state: "suspended", closed: false,
        nodeCount: 0, runningStarted: 0, elapsed: 0, lastTime: 0,
        mixGraphRevision: 0 });
      this.sampleRate = 44100;
      this.destination = new AudioDestinationNode(this);
      this.onstatechange = null;
    }
    get state() { return stateFor(this)?.state || "closed"; }
    get currentTime() {
      const state = stateFor(this);
      return state.lastTime = state.elapsed + (state.state === "running"
        ? (performance.now() - state.runningStarted) / 1000 : 0);
    }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createGain() { return new GainNode(this); }
    createOscillator() { return new OscillatorNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    _refreshActive() {
      for (const source of sourcesByVoice.values())
        if (source.context === this) source._updateMix();
    }
    _forSourcesThrough(node, callback) {
      const cache = sourcesThrough(this, node);
      for (let at = 0; at < cache.count; at++) callback(cache.sources[at]);
    }
    _scheduleGainTarget(node, value, when, timeConstant) {
      const delay = boundedDelay(this, when);
      const cache = sourcesThrough(this, node);
      // Automation publishes native commands directly. A per-segment closure
      // and callback dispatch add interpreted work to every attack/decay pair.
      for (let at = 0; at < cache.count; at++) {
        const source = cache.sources[at];
        const [left, right] = sourceMix(source, node, value);
        if (!nativeCommand(GAIN_TARGET, source._voice, left, right, delay, timeConstant))
          throw new DOMException("Gain target exceeds the bounded timeline",
            "NotSupportedError");
      }
    }
    _cancelGainTargets(node, when) {
      const delay = boundedDelay(this, when);
      if (delay > .001) throw new DOMException(
        "Future automation cancellation is unavailable", "NotSupportedError");
      const cache = sourcesThrough(this, node);
      for (let at = 0; at < cache.count; at++)
        nativeCommand(CANCEL_GAIN, cache.sources[at]._voice);
    }
    _scheduleGainCurve(node, values, when, duration) {
      const delay = boundedDelay(this, when);
      if (delay + duration > 10) throw new DOMException(
        "Audio curve exceeds the ten-second window", "NotSupportedError");
      const cache = sourcesThrough(this, node);
      if (!cache.count) throw new DOMException(
        "Gain curves need a live connected source", "NotSupportedError");
      for (let at = 0; at < cache.count; at++) {
        const source = cache.sources[at];
        // In a direct source -> automated gain -> destination graph the
        // overridden gain is exactly one on both channels. Do not traverse
        // the same two nodes, allocate a pair and clamp that constant again.
        // Chained gains and panners still resolve their live values below.
        let left = 1, right = 1;
        if (source._target !== node || !(node instanceof GainNode)
            || node._target !== this.destination)
          [left, right] = sourceMix(source, node, 1);
        if (!nativeCommand(CURVE, source._voice, 0, values, left, right, delay, duration))
          throw new DOMException("Gain curve needs an empty automation timeline",
            "NotSupportedError");
      }
    }
    _stateChanged() {
      const event = new Event("statechange"), handler = this.onstatechange;
      this.dispatchEvent(event);
      if (typeof handler === "function")
        try {
          globalThis.__tilefinchRunTask(
            "game-audio-statechange", handler, this, [event]);
        } catch (error) {
          globalThis.__tilefinchReportUncaught(error, "game audio statechange");
        }
    }
    decodeAudioData(buffer, success, failure) {
      const promise = Promise.resolve().then(() => {
        if (stateFor(this).closed || !(buffer instanceof ArrayBuffer))
          throw new DOMException("Invalid audio data", "EncodingError");
        const token = nativeDecode(buffer);
        if (!token) throw new DOMException(
          "Only bounded PCM WAV audio is supported", "EncodingError");
        return new AudioBuffer(token, this);
      });
      if (typeof success === "function") promise.then(success);
      if (typeof failure === "function") promise.catch(failure);
      return promise;
    }
    resume() {
      const state = stateFor(this);
      if (state.closed) return Promise.reject(new DOMException(
        "AudioContext is closed", "InvalidStateError"));
      if (state.state === "running") return Promise.resolve();
      if (!nativeCommand(RESUME)) return Promise.reject(
        new DOMException("Audio resume requires user activation", "NotAllowedError"));
      state.state = "running";
      state.runningStarted = performance.now();
      this._stateChanged();
      return Promise.resolve();
    }
    suspend() {
      const state = stateFor(this);
      if (state.closed) return Promise.reject(new DOMException(
        "AudioContext is closed", "InvalidStateError"));
      if (state.state === "suspended") return Promise.resolve();
      if (state.state === "running")
        state.elapsed += (performance.now() - state.runningStarted) / 1000;
      nativeCommand(SUSPEND);
      state.state = "suspended";
      this._stateChanged();
      return Promise.resolve();
    }
    close() {
      const state = stateFor(this);
      if (state.closed) return Promise.resolve();
      nativeCommand(CLOSE);
      sourcesByVoice.clear();
      state.mixGraphRevision++;
      state.closed = true;
      state.state = "closed";
      if (activeContext === this) activeContext = null;
      this._stateChanged();
      return Promise.resolve();
    }
  }

  class AudioContext extends BaseAudioContext {
    constructor() {
      super();
      if (activeContext !== null) throw new DOMException(
        "Only one game AudioContext is available", "QuotaExceededError");
      activeContext = this;
    }
  }
  Object.assign(globalThis, {
    AudioBuffer, AudioBufferSourceNode, AudioContext, AudioDestinationNode,
    AudioNode, AudioParam, BaseAudioContext, GainNode, OscillatorNode,
    StereoPannerNode,
    webkitAudioContext: AudioContext,
  });
  /* Native media and PSP suspend release the audio channel without running
     author callbacks on a lifecycle deadline. Keep the observable state in
     sync; a later trusted resume still dispatches the normal statechange. */
  Object.defineProperty(globalThis, "__tilefinchSuspendGameAudioFromHost", {
    configurable: true,
    writable: false,
    value: () => {
      const context = activeContext;
      if (!context) return;
      const state = stateFor(context);
      if (state.state === "running")
        state.elapsed +=
          (performance.now() - state.runningStarted) / 1000;
      state.state = "suspended";
    },
  });
  /* The audio worker publishes only an integer generation token. Native code
     invokes this captured bridge later on the browser thread, where author
     callbacks and microtasks are safe. */
  Object.defineProperty(globalThis, "__tilefinchCompleteGameAudioVoice", {
    configurable: true,
    writable: false,
    value: (handle) => {
      handle = Number(handle) >>> 0;
      const source = sourcesByVoice.get(handle);
      if (!source) return false;
      sourcesByVoice.delete(handle);
      source._voice = 0;
      stateFor(source.context).mixGraphRevision++;
      const event = new Event("ended"), handler = source.onended;
      source.dispatchEvent(event);
      if (typeof handler === "function")
        try {
          globalThis.__tilefinchRunTask(
            "game-audio-ended", handler, source, [event]);
        } catch (error) {
          globalThis.__tilefinchReportUncaught(error, "game audio ended");
        }
      return true;
    },
  });
})();
