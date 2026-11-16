(() => {
  /* Loaded after the page may have replaced these globals: build
     errors, promises and platform objects with the constructors
     hardening.js captured, and publish private hooks with its
     defineProperty. */
  const { defineProperty, Error, TypeError, Promise, Set, Array } =
    globalThis.__tilefinchEventInterfaces;
  /* Media Capture and Streams for the PSP microphone: one mono audio input,
     no camera. Every admission decision (secure context, frame policy,
     visibility, the user's stored choice and the browser-owned prompt) is
     native (src/js_media_capture.c); this module keeps the bounded track and
     stream objects. Audio never passes through JavaScript except when a page
     asks Web Audio for the newest samples. Loaded only in secure contexts,
     the first time a page names one of these interfaces. */
  const command = globalThis.__tilefinchMediaCaptureCommand,
    createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap,
    createEvent = globalThis.__tilefinchCreateEvent,
    /* Loaded after the page may have replaced these globals: extend the
       interfaces hardening.js captured, and add mediaDevices to its
       Navigator. Handler attributes register through its listener pair. */
    { Event, EventTarget, DOMException, Navigator: NavigatorInterface,
      addListener, removeListener } = globalThis.__tilefinchEventInterfaces;
  if (typeof command !== "function" || typeof createPrivateWeakMap !== "function" ||
      typeof createEvent !== "function")
    throw new Error("Media capture bridge is unavailable");
  /* Command opcodes: js_media_capture.c. */
  const REQUEST = 0, RELEASE = 1, CLONE = 2, PERMISSION = 3, DEVICES = 4,
    ENABLE = 6;
  const token = {},
    tracks = createPrivateWeakMap(),
    streams = createPrivateWeakMap(),
    devices = createPrivateWeakMap(),
    handlers = createPrivateWeakMap(),
    liveTracks = new Set();
  let mediaDevices = null;
  const newId = () => crypto.randomUUID();
  const handlerAttribute = (prototype, type) =>
    defineProperty(prototype, "on" + type, {
      configurable: true,
      enumerable: true,
      get() {
        return handlers.get(this)?.[type] ?? null;
      },
      set(value) {
        let slots = handlers.get(this);
        if (!slots) handlers.set(this, (slots = Object.create(null)));
        if (slots[type]) removeListener(this, type, slots[type]);
        slots[type] = typeof value === "function" ? value : null;
        if (slots[type]) addListener(this, type, slots[type]);
      },
    });
  const trackState = (value) => {
    const state = tracks.get(value);
    if (!state) throw new TypeError("Illegal invocation");
    return state;
  };
  const streamState = (value) => {
    const state = streams.get(value);
    if (!state) throw new TypeError("Illegal invocation");
    return state;
  };

  /* ---- constraints ---------------------------------------------------- */
  const supported = Object.freeze({
    deviceId: true, groupId: true, sampleRate: true, sampleSize: true,
    channelCount: true, latency: true, echoCancellation: true,
    autoGainControl: true, noiseSuppression: true,
  });
  const capabilitiesFor = (format) => ({
    deviceId: "default",
    groupId: "default",
    sampleRate: { min: format.sampleRate, max: format.sampleRate },
    sampleSize: { min: 16, max: 16 },
    channelCount: { min: format.channelCount, max: format.channelCount },
    latency: { min: format.latency, max: format.latency },
    echoCancellation: [false],
    autoGainControl: [false],
    noiseSuppression: [false],
  });
  const settingsFor = (format) => ({
    deviceId: "default",
    groupId: "default",
    sampleRate: format.sampleRate,
    sampleSize: 16,
    channelCount: format.channelCount,
    latency: format.latency,
    echoCancellation: false,
    autoGainControl: false,
    noiseSuppression: false,
  });
  /* The PSP input's fixed format, before a session reports its own. */
  const nominal = { sampleRate: 44100, channelCount: 1, latency: 1024 / 44100 };
  const overconstrained = (name) =>
    new OverconstrainedError(name, "Constraint " + name + " cannot be satisfied");
  /* Whether VALUE (a bare value or a constraint dictionary) leaves SETTING
     possible. Bare values are ideals except under "advanced". */
  const satisfies = (setting, value, bareRequired) => {
    if (value === undefined || value === null) return true;
    if (typeof value !== "object") {
      if (!bareRequired) return true;
      return Array.isArray(setting) ? true : setting === value
        || (typeof setting === "number" && Number(value) === setting);
    }
    if (Array.isArray(value)) return true;
    const test = (wanted) => {
      if (typeof setting === "number") return Number(wanted) === setting;
      if (Array.isArray(wanted)) return wanted.some((item) => item === setting);
      return wanted === setting;
    };
    if ("exact" in value && !test(value.exact)) return false;
    if (typeof setting === "number") {
      if ("min" in value && Number(value.min) > setting + 1e-9) return false;
      if ("max" in value && Number(value.max) < setting - 1e-9) return false;
    }
    return true;
  };
  /* The first required constraint the input cannot meet, or "". */
  const unmet = (constraints, settings) => {
    if (constraints === null || typeof constraints !== "object") return "";
    for (const name of Object.keys(constraints)) {
      if (name === "advanced" || !supported[name]) continue;
      if (!satisfies(settings[name], constraints[name], false)) return name;
    }
    return "";
  };
  const copyConstraints = (constraints) => {
    const copy = {};
    if (constraints === null || typeof constraints !== "object") return copy;
    for (const name of Object.keys(constraints)) {
      if (!supported[name] && name !== "advanced") continue;
      const value = constraints[name];
      copy[name] = value !== null && typeof value === "object"
        ? JSON.parse(JSON.stringify(value)) : value;
    }
    return copy;
  };

  /* ---- tracks and streams --------------------------------------------- */
  /* Ends TRACK as stop() does, without page-visible lookups. */
  const endTrack = (track) => {
    const state = tracks.get(track);
    if (!state || state.readyState === "ended") return;
    state.readyState = "ended";
    liveTracks.delete(track);
    command(RELEASE, state.serial);
  };
  /* A new track for the native SERIAL (0: an ended track). Building it is
     all or nothing: when it throws (an id or an allocation), SERIAL's share
     of the microphone is given back before the error propagates, so a
     track the page never received cannot keep capture running. */
  const makeTrack = (generation, serial, format, constraints) => {
    let track = null;
    try {
      track = new MediaStreamTrack(token, generation, serial, format,
        constraints);
      if (!serial) {
        tracks.get(track).readyState = "ended";
        liveTracks.delete(track);
      }
      return track;
    } catch (error) {
      if (track !== null) endTrack(track);
      else if (serial) command(RELEASE, serial);
      throw error;
    }
  };
  const cloneTrack = (state) => {
    const serial = state.readyState === "live" ? command(CLONE, state.serial) : 0;
    let copy = null;
    try {
      copy = makeTrack(serial ? state.generation : 0, serial, state.format,
        copyConstraints(state.constraints));
    } catch (error) {
      /* Arguments that failed before makeTrack ran (a repeat is ignored). */
      if (serial) command(RELEASE, serial);
      throw error;
    }
    const copied = tracks.get(copy);
    copied.contentHint = state.contentHint;
    copied.enabled = state.enabled;
    if (serial && !state.enabled) command(ENABLE, serial, false);
    return copy;
  };
  /* A track's native serial names it to the realm's capture bookkeeping
     (js_media_capture.c): stop() releases it, enabled reaches the audio a
     MediaStreamAudioSourceNode of it plays. 0 is an ended track. */
  class MediaStreamTrack extends EventTarget {
    constructor(value, generation, serial, format, constraints) {
      if (value !== token) throw new TypeError("Illegal constructor");
      super();
      tracks.set(this, {
        id: newId(), enabled: true, readyState: "live", generation, serial,
        format, constraints, contentHint: "",
      });
      liveTracks.add(this);
    }
    get kind() { trackState(this); return "audio"; }
    get id() { return trackState(this).id; }
    get label() { trackState(this); return "PSP microphone"; }
    get enabled() { return trackState(this).enabled; }
    set enabled(value) {
      const state = trackState(this);
      state.enabled = !!value;
      if (state.readyState === "live") command(ENABLE, state.serial, state.enabled);
    }
    get muted() { trackState(this); return false; }
    get readyState() { return trackState(this).readyState; }
    get contentHint() { return trackState(this).contentHint; }
    set contentHint(value) {
      value = String(value);
      if (value === "" || value === "speech" || value === "speech-recognition"
          || value === "music")
        trackState(this).contentHint = value;
    }
    stop() {
      trackState(this);
      endTrack(this);
    }
    clone() {
      return cloneTrack(trackState(this));
    }
    getCapabilities() { return capabilitiesFor(trackState(this).format); }
    getConstraints() { return copyConstraints(trackState(this).constraints); }
    getSettings() { return settingsFor(trackState(this).format); }
    applyConstraints(constraints = {}) {
      let state;
      try {
        state = trackState(this);
      } catch (error) {
        return Promise.reject(error);
      }
      const name = unmet(constraints, settingsFor(state.format));
      if (name) return Promise.reject(overconstrained(name));
      state.constraints = copyConstraints(constraints);
      return Promise.resolve();
    }
  }
  for (const type of ["ended", "mute", "unmute"])
    handlerAttribute(MediaStreamTrack.prototype, type);

  class MediaStream extends EventTarget {
    constructor(init = undefined) {
      super();
      const own = new Set();
      if (init instanceof MediaStream) {
        for (const track of streamState(init).tracks) own.add(track);
      } else if (init !== undefined) {
        if (init === null || typeof init[Symbol.iterator] !== "function")
          throw new TypeError("MediaStream needs a stream or a track sequence");
        for (const track of init) {
          trackState(track);
          own.add(track);
        }
      }
      streams.set(this, { id: newId(), tracks: own });
    }
    get id() { return streamState(this).id; }
    get active() {
      for (const track of streamState(this).tracks)
        if (tracks.get(track).readyState === "live") return true;
      return false;
    }
    getTracks() { return [...streamState(this).tracks]; }
    getAudioTracks() { return [...streamState(this).tracks]; }
    getVideoTracks() { streamState(this); return []; }
    getTrackById(id) {
      id = String(id);
      for (const track of streamState(this).tracks)
        if (tracks.get(track).id === id) return track;
      return null;
    }
    addTrack(track) {
      const state = streamState(this);
      trackState(track);
      state.tracks.add(track);
    }
    removeTrack(track) {
      const state = streamState(this);
      trackState(track);
      state.tracks.delete(track);
    }
    clone() {
      const state = streamState(this);
      const copy = new MediaStream();
      const own = streamState(copy).tracks;
      /* All or nothing, as getUserMedia(): a clone that fails ends the
         clones made before it. */
      try {
        for (const track of state.tracks) {
          const clone = cloneTrack(trackState(track));
          try {
            own.add(clone);
          } catch (error) {
            endTrack(clone);
            throw error;
          }
        }
      } catch (error) {
        for (const track of own) endTrack(track);
        throw error;
      }
      return copy;
    }
  }
  for (const type of ["addtrack", "removetrack"])
    handlerAttribute(MediaStream.prototype, type);

  class MediaStreamTrackEvent extends Event {
    constructor(type, init) {
      if (init === null || typeof init !== "object")
        throw new TypeError("MediaStreamTrackEvent needs a track");
      const track = init.track;
      trackState(track);
      super(type, init);
      defineProperty(this, "track", { enumerable: true, value: track });
    }
  }

  class OverconstrainedError extends DOMException {
    constructor(constraint, message = "") {
      super(String(message), "OverconstrainedError");
      defineProperty(this, "constraint", {
        enumerable: true, value: String(constraint),
      });
    }
  }

  /* ---- devices ---------------------------------------------------------- */
  class MediaDeviceInfo {
    constructor(value, deviceId, label) {
      if (value !== token) throw new TypeError("Illegal constructor");
      devices.set(this, { deviceId, label });
    }
    get deviceId() { return devices.get(this).deviceId; }
    get kind() { devices.get(this); return "audioinput"; }
    get label() { return devices.get(this).label; }
    get groupId() { return devices.get(this).deviceId; }
    toJSON() {
      return { deviceId: this.deviceId, kind: this.kind, label: this.label,
        groupId: this.groupId };
    }
  }
  class InputDeviceInfo extends MediaDeviceInfo {
    getCapabilities() {
      return devices.get(this).deviceId ? capabilitiesFor(nominal) : {};
    }
  }

  const reject = (name, message) =>
    Promise.reject(new DOMException(message, name));
  const audioRequested = (value) =>
    value !== undefined && value !== false && value !== null;
  class MediaDevices extends EventTarget {
    constructor(value) {
      if (value !== token) throw new TypeError("Illegal constructor");
      super();
    }
    getSupportedConstraints() {
      return { ...supported };
    }
    enumerateDevices() {
      if (this !== mediaDevices)
        return Promise.reject(new TypeError("Illegal invocation"));
      return new Promise((resolve) => {
        const list = [];
        if (command(DEVICES)) {
          /* Identifiers and labels only once the page may hear the input:
             a grant or a live capture. */
          const exposed = liveTracks.size !== 0 || command(PERMISSION) === 1;
          list.push(new InputDeviceInfo(token, exposed ? "default" : "",
            exposed ? "PSP microphone" : ""));
        }
        resolve(list);
      });
    }
    getUserMedia(constraints = {}) {
      if (this !== mediaDevices)
        return Promise.reject(new TypeError("Illegal invocation"));
      if (constraints === null || typeof constraints !== "object")
        return Promise.reject(new TypeError("Constraints must be an object"));
      const audio = constraints.audio, video = constraints.video;
      if (!audioRequested(audio) && !audioRequested(video))
        return Promise.reject(new TypeError(
          "At least one of audio and video must be requested"));
      if (audioRequested(video))
        return reject("NotFoundError", "Requested device not found");
      const wanted = typeof audio === "object" ? copyConstraints(audio) : {};
      const name = unmet(wanted, settingsFor(nominal));
      if (name) return Promise.reject(overconstrained(name));
      const result = command(REQUEST);
      if (typeof result === "string") {
        const split = result.indexOf(":");
        return reject(result.slice(0, split), result.slice(split + 1));
      }
      /* The microphone is on for this request: until the page holds the
         stream, any failure gives it back and rejects. Releasing a serial
         that was already given back is ignored. */
      let track = null;
      try {
        track = makeTrack(result.generation, result.track, {
          sampleRate: result.sampleRate, channelCount: result.channelCount,
          latency: result.latency,
        }, wanted);
        return Promise.resolve(new MediaStream([track]));
      } catch (error) {
        if (track !== null) endTrack(track);
        else command(RELEASE, result.track);
        return Promise.reject(error);
      }
    }
  }
  handlerAttribute(MediaDevices.prototype, "devicechange");

  for (const [constructor, tag] of [
    [MediaStreamTrack, "MediaStreamTrack"],
    [MediaStream, "MediaStream"],
    [MediaStreamTrackEvent, "MediaStreamTrackEvent"],
    [OverconstrainedError, "OverconstrainedError"],
    [MediaDeviceInfo, "MediaDeviceInfo"],
    [InputDeviceInfo, "InputDeviceInfo"],
    [MediaDevices, "MediaDevices"],
  ])
    defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true,
      value: tag,
    });
  Object.assign(globalThis, {
    MediaStreamTrack, MediaStream, MediaStreamTrackEvent, OverconstrainedError,
    MediaDeviceInfo, InputDeviceInfo, MediaDevices,
  });
  if (typeof NavigatorInterface === "function")
    defineProperty(NavigatorInterface.prototype, "mediaDevices", {
      configurable: true,
      enumerable: true,
      get() {
        if (!(this instanceof NavigatorInterface))
          throw new TypeError("Illegal invocation");
        return (mediaDevices ??= new MediaDevices(token));
      },
    });

  /* Native code captures this before author code can see it, and calls it
     at a task boundary, with the trusted-event marker, when the browser
     ended capture (Stop microphone, sleep, voice typing, native media):
     every live track of that session ends with a trusted "ended". */
  /* Native code captures this too: the serial of the track a
     MediaStreamAudioSourceNode of STREAM plays (the first audio track by id,
     as Web Audio chooses), or 0 when that track is not live. */
  defineProperty(globalThis, "__tilefinchMediaCaptureStreamTrack", { __proto__: null,
    configurable: true,
    writable: false,
    value: (stream) => {
      const state = streams.get(stream);
      if (!state) return 0;
      let chosen = null;
      for (const track of state.tracks) {
        const candidate = tracks.get(track);
        if (chosen === null || candidate.id < chosen.id) chosen = candidate;
      }
      return chosen !== null && chosen.readyState === "live" ? chosen.serial : 0;
    },
  });
  defineProperty(globalThis, "__tilefinchMediaCaptureHostEvent", { __proto__: null,
    configurable: true,
    writable: false,
    value: (generation, trusted) => {
      for (const track of [...liveTracks]) {
        const state = tracks.get(track);
        if (state.generation !== generation) continue;
        liveTracks.delete(track);
        state.readyState = "ended";
        track.dispatchEvent(trusted(createEvent("ended")));
      }
    },
  });
})();
