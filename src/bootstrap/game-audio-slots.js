((cap) => {
  // This packet stays in lexical scope. Node/Param state is exclusively C-owned.
  /* Loaded on first use, after the page may have replaced EventTarget:
     extend the interface hardening.js captured. */
  const { EventTarget } = globalThis.__tilefinchEventInterfaces;
  class AudioParam {
    constructor() { throw new TypeError("Illegal constructor"); }
  }
  class AudioNode extends EventTarget {
    constructor() {
      super();
      if (new.target === AudioNode) throw new TypeError("Illegal constructor");
    }
  }
  class GainNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 1, options);
    }
  }
  class StereoPannerNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 2, options);
    }
  }
  class OscillatorNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 3, options);
    }
  }
  class AudioBufferSourceNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 4, options);
    }
  }
  class BiquadFilterNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 5, options);
    }
  }
  class MediaStreamAudioSourceNode extends AudioNode {
    constructor(context, options) {
      super();
      const stream = options === null || typeof options !== "object"
        ? undefined : options.mediaStream;
      if (typeof MediaStream !== "function" || !(stream instanceof MediaStream))
        throw new TypeError("MediaStreamAudioSourceNode needs options.mediaStream");
      if (stream.getAudioTracks().length === 0)
        throw new DOMException("The MediaStream has no audio track", "InvalidStateError");
      return cap.node(context, new.target.prototype, 6, { mediaStream: stream });
    }
  }
  class AnalyserNode extends AudioNode {
    constructor(context, options = {}) {
      super();
      return cap.node(context, new.target.prototype, 7, options);
    }
  }
  class AudioDestinationNode extends AudioNode {
    constructor() { super(); throw new TypeError("Illegal constructor"); }
  }
  class AudioBuffer {
    constructor(options) {
      if (options === null || typeof options !== "object")
        throw new TypeError("AudioBuffer options are required");
      const channels = options.numberOfChannels, length = options.length,
        sampleRate = options.sampleRate;
      if (length === undefined || sampleRate === undefined)
        throw new TypeError("AudioBuffer length and sampleRate are required");
      return cap.newBuffer(new.target.prototype, null,
        channels === undefined ? 1 : channels, length, sampleRate);
    }
  }
  const contextToken = {};
  class BaseAudioContext extends EventTarget {
    constructor(token) {
      super();
      if (token !== contextToken) throw new TypeError("Illegal constructor");
      return cap.context(new.target.prototype, true);
    }
    createGain() { return new GainNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    createOscillator() { return new OscillatorNode(this); }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createBiquadFilter() { return new BiquadFilterNode(this); }
    createMediaStreamSource(mediaStream) {
      return new MediaStreamAudioSourceNode(this, { mediaStream });
    }
    createAnalyser() { return new AnalyserNode(this); }
    createBuffer(numberOfChannels, length, sampleRate) {
      if (arguments.length < 3) throw new TypeError("createBuffer needs three arguments");
      return cap.newBuffer(AudioBuffer.prototype, this, numberOfChannels, length, sampleRate);
    }
    decodeAudioData(buffer, success, failure) {
      const promise = Promise.resolve().then(() => cap.buffer(this, buffer));
      if (typeof success === "function") promise.then(success);
      if (typeof failure === "function") promise.catch(failure);
      return promise;
    }
  }
  class AudioContext extends BaseAudioContext {
    constructor() { super(contextToken); }
  }
  /* Loaded on first use, after author code may have replaced Event. */
  const createEvent = globalThis.__tilefinchCreateEvent;
  function notify(receiver, type) {
    const event = createEvent(type);
    let handler;
    try { handler = receiver["on" + type]; }
    catch (error) {
      globalThis.__tilefinchReportUncaught(error,
        type === "ended" ? "game audio ended getter" : "game audio statechange getter");
    }
    receiver.dispatchEvent(event);
    if (typeof handler === "function") try {
      globalThis.__tilefinchRunTask(
        type === "ended" ? "game-audio-ended" : "game-audio-statechange",
        handler, receiver, [event]);
    } catch (error) {
      globalThis.__tilefinchReportUncaught(error,
        type === "ended" ? "game audio ended" : "game audio statechange");
    }
  }
  cap.install([BaseAudioContext.prototype, AudioContext.prototype, AudioNode.prototype,
    GainNode.prototype, StereoPannerNode.prototype, OscillatorNode.prototype,
    AudioBufferSourceNode.prototype, AudioDestinationNode.prototype,
    AudioParam.prototype, AudioBuffer.prototype, BiquadFilterNode.prototype,
    MediaStreamAudioSourceNode.prototype, AnalyserNode.prototype], notify,
    DOMException);
  cap.publish([AudioParam, AudioNode, GainNode, StereoPannerNode, OscillatorNode,
    AudioBufferSourceNode, AudioDestinationNode, AudioBuffer, BaseAudioContext,
    AudioContext, AudioContext, BiquadFilterNode, MediaStreamAudioSourceNode, AnalyserNode]);
})
