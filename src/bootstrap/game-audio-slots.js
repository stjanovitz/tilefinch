((cap) => {
  // This packet stays in lexical scope. Node/Param state is exclusively C-owned.
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
  class AudioDestinationNode extends AudioNode {
    constructor() { super(); throw new TypeError("Illegal constructor"); }
  }
  class AudioBuffer {
    constructor() { throw new TypeError("Use decodeAudioData"); }
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
  function notify(receiver, type) {
    const event = new Event(type);
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
    AudioParam.prototype, AudioBuffer.prototype], notify, DOMException);
  cap.publish([AudioParam, AudioNode, GainNode, StereoPannerNode, OscillatorNode,
    AudioBufferSourceNode, AudioDestinationNode, AudioBuffer, BaseAudioContext,
    AudioContext, AudioContext]);
})
