/* EventSource, built the first time a page reads the global (a ROM-backed
   lazy module; see Module boundaries in README.md). The host lends the
   dom.js share for this evaluation only: a source's reconnect tasks are
   cancelled with the cancel that reaches every timer. Native code delivers
   a stream's open, chunks and end through the hook this publishes. */
(() => {
  /* Loaded after the page may have replaced these globals: build errors,
     events and platform objects with what hardening.js captured. URL is
     the platform's: its _origin field is not a page-replaceable getter. */
  const { defineProperty, dispatch, EventTarget, DOMException, URL, Map, Set,
    TextDecoder, Uint8Array, ArrayBuffer, Error, TypeError, claimHooks } =
    globalThis.__tilefinchEventInterfaces;
  const publish = claimHooks?.(),
    domShared = globalThis.__tilefinchDomShared,
    createEvent = globalThis.__tilefinchCreateEvent;
  const eventSources = new Map(),
    eventSourceLimit = 2;
  let deferredSources = null;
  const scheduleSourceReconnect = (source) => {
    if (source._closed || source._nativeId || source._reconnectTask) return true;
    source._reconnectTask = __tilefinchRealmGlobal.__tilefinchScheduleTask(() => {
      source._reconnectTask = 0;
      source._connect();
    }, Math.max(0, source._reconnectAt - __tilefinchRealmGlobal.__tilefinchSchedulerTime()));
    return !!source._reconnectTask;
  };
  const deferSourceReconnect = (source) => {
    if (!deferredSources) {
      deferredSources = new Set();
      __tilefinchTimerCapacity[5] = () => {
        let remaining = 128;
        for (const pending of deferredSources) {
          if (!remaining--) break;
          if (scheduleSourceReconnect(pending)) deferredSources.delete(pending);
        }
        if (deferredSources.size) __tilefinchTimerCapacity[4] |= 16;
      };
    }
    /* Retain bounded owners, not another unbounded task queue. */
    if (deferredSources.size >= 128 && !deferredSources.has(source)) {
      source.close();
      return;
    }
    deferredSources.add(source);
    __tilefinchTimerCapacity[4] |= 16;
  };
  class EventSource extends EventTarget {
    constructor(url, options = {}) {
      super();
      if (arguments.length < 1)
        throw new TypeError("EventSource requires a URL");
      this.url = new URL(String(url), location.href).href;
      if (!/^https?:/.test(this.url))
        throw new DOMException(
          "EventSource requires HTTP or HTTPS",
          "SecurityError",
        );
      this.withCredentials = !!options.withCredentials;
      this.readyState = EventSource.CONNECTING;
      this.onopen = null;
      this.onmessage = null;
      this.onerror = null;
      this._nativeId = 0;
      this._closed = false;
      this._lastEventId = "";
      this._retry = 3000;
      this._decoder = null;
      this._text = "";
      this._data = [];
      this._dataBytes = 0;
      this._eventType = "";
      this._idBuffer = "";
      this._skipLF = false;
      this._reconnectTask = 0;
      this._reconnectAt = 0;
      this._connect();
    }
    _emit(event) {
      dispatch(this, event);
      try {
        const handler = this["on" + event.type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "event-source:" + String(event.type), handler, this, [event],
          );
      } catch (error) {
        globalThis.__tilefinchReportUncaught(error, "event-source:" + event.type);
      }
    }
    _resetParser() {
      this._decoder = null;
      this._text = "";
      this._data = [];
      this._dataBytes = 0;
      this._eventType = "";
      this._idBuffer = "";
      this._skipLF = false;
    }
    _connect() {
      if (this._closed || this._nativeId) return;
      deferredSources?.delete(this);
      const task = this._reconnectTask;
      this._reconnectTask = 0;
      domShared.cancelTimer(task);
      this._resetParser();
      if (eventSources.size >= eventSourceLimit) {
        this._failAndReconnect();
        return;
      }
      try {
        this._decoder = new TextDecoder();
        const id = Number(
          __tilefinchEventSourceStart(
            this.url,
            this.withCredentials,
            this._lastEventId,
          ),
        );
        if (!(id > 0)) throw new Error("EventSource admission failed");
        this._nativeId = id;
        eventSources.set(id, this);
      } catch (_) {
        this._failAndReconnect();
      }
    }
    _line(line) {
      if (line === "") {
        this._lastEventId = this._idBuffer;
        const data = this._data, type = this._eventType;
        /* Consume the event before author code can throw or reenter. */
        this._data = [];
        this._dataBytes = 0;
        this._eventType = "";
        if (data.length) {
          const event = createEvent(type || "message", {
            data: data.join("\n"),
            origin: new URL(this.url)._origin,
            lastEventId: this._lastEventId,
            source: null,
          }, "MessageEvent");
          this._emit(event);
        }
        return;
      }
      if (line.startsWith(":")) return;
      const colon = line.indexOf(":"),
        field = colon < 0 ? line : line.slice(0, colon);
      let value = colon < 0 ? "" : line.slice(colon + 1);
      if (value.startsWith(" ")) value = value.slice(1);
      if (field === "data") {
        const separator = this._data.length ? 1 : 0;
        if (
          value.length > 64 * 1024 - separator ||
          this._dataBytes > 64 * 1024 - separator - value.length
        ) {
          this._data = [];
          this._dataBytes = 0;
          this.close();
          this._emit(createEvent("error"));
          return;
        }
        this._data.push(value);
        this._dataBytes += separator + value.length;
      }
      else if (field === "event") this._eventType = value;
      else if (field === "id" && !value.includes("\0"))
        this._idBuffer = value;
      else if (field === "retry" && /^\d+$/.test(value))
        this._retry = Math.min(30000, Math.max(250, Number(value)));
    }
    _chunk(bytes) {
      let text =
        bytes instanceof ArrayBuffer
          ? this._decoder.decode(new Uint8Array(bytes), { stream: true })
          : String(bytes);
      if (this._skipLF && text.length) {
        if (text[0] === "\n") text = text.slice(1);
        this._skipLF = false;
      }
      this._text += text;
      for (;;) {
        let newline = -1;
        for (let index = 0; index < this._text.length; index++) {
          if (this._text[index] === "\n" || this._text[index] === "\r") {
            newline = index;
            break;
          }
        }
        if (newline < 0) {
          if (this._text.length > 64 * 1024) {
            this.close();
            this._emit(createEvent("error"));
          }
          return;
        }
        this._skipLF = this._text[newline] === "\r" &&
          newline + 1 === this._text.length;
        const line = this._text.slice(0, newline),
          consumed =
            this._text[newline] === "\r" &&
            this._text[newline + 1] === "\n"
              ? newline + 2
              : newline + 1;
        this._text = this._text.slice(consumed);
        this._line(line);
        if (this._closed) return;
      }
    }
    _failAndReconnect() {
      if (this._closed) return;
      this._resetParser();
      this.readyState = EventSource.CONNECTING;
      this._reconnectAt = __tilefinchRealmGlobal.__tilefinchSchedulerTime() + this._retry;
      if (!scheduleSourceReconnect(this)) deferSourceReconnect(this);
      this._emit(createEvent("error"));
    }
    close() {
      if (this._closed) return;
      this._closed = true;
      this.readyState = EventSource.CLOSED;
      deferredSources?.delete(this);
      const task = this._reconnectTask;
      this._reconnectTask = 0;
      this._resetParser();
      domShared.cancelTimer(task);
      if (this._nativeId) {
        eventSources.delete(this._nativeId);
        __tilefinchEventSourceClose(this._nativeId);
        this._nativeId = 0;
      }
    }
  }
  for (const [name, value] of [["CONNECTING", 0], ["OPEN", 1], ["CLOSED", 2]]) {
    defineProperty(EventSource, name, { __proto__: null, value });
    defineProperty(EventSource.prototype, name, { __proto__: null, value });
  }
  globalThis.EventSource = EventSource;
  publish?.("__tilefinchEventSourceInterface", EventSource);
  publish?.("__tilefinchDeliverEventSource", (
    id,
    kind,
    payload,
    origin,
  ) => {
    const source = eventSources.get(Number(id));
    if (!source || source._closed) return false;
    if (kind === "open") {
      source.readyState = EventSource.OPEN;
      source._emit(createEvent("open"));
    } else if (kind === "chunk") {
      source._chunk(payload);
    } else {
      eventSources.delete(Number(id));
      source._nativeId = 0;
      if (kind === "terminal") {
        source.close();
        source._emit(createEvent("error"));
      } else source._failAndReconnect();
    }
    return true;
  });
})();
