/* The multiplayer channel behind navigator.tilefinchMultiplayer, built on
   the first host(), join() or discover() (a ROM-backed lazy module; see
   Module boundaries in README.md). compat.js keeps the frozen
   navigator.tilefinchMultiplayer object and loads this through a private
   native, reading the channel factory off the global in the same call.
   Native code delivers a channel's events through the hook this
   publishes. */
(() => {
  const Object = __tilefinchObject;
  /* Loaded after the page may have replaced these globals: build errors,
     events and platform objects with what hardening.js captured. */
  const { defineProperty, dispatch, EventTarget, DOMException, Map,
    ArrayBuffer, Uint8Array, TextEncoder, TextDecoder, TypeError,
    claimHooks } = globalThis.__tilefinchEventInterfaces;
  const publish = claimHooks?.(),
    createEvent = globalThis.__tilefinchCreateEvent;
  const channels = new Map(),
    messageLimit = 512,
    queueLimit = 8,
    bufferedLimit = messageLimit * queueLimit;
  const multiplayerEvent = (type, fields = {}) => {
    const event = createEvent(type);
    for (const [name, value] of Object.entries(fields))
      Object.defineProperty(event, name, {
        value, enumerable: true, configurable: true,
      });
    return event;
  };
  class TilefinchMultiplayerChannel extends EventTarget {
    constructor(mode, options, inviteCode = "") {
      super();
      options = options && typeof options === "object" ? options : {};
      this.label = String(options.gameId || "").slice(0, 32);
      this.protocol = "tilefinch-game-v1";
      this.ordered = false;
      this.maxPacketLifeTime = null;
      this.maxRetransmits = 0;
      this.negotiated = false;
      this.id = null;
      this.readyState = "connecting";
      this.bufferedAmount = 0;
      this.bufferedAmountLowThreshold = 0;
      this.binaryType = "arraybuffer";
      this.onopen = null;
      this.onmessage = null;
      this.onbufferedamountlow = null;
      this.onclose = null;
      this.onerror = null;
      this.onstatus = null;
      this.oninvitecode = null;
      this.ondiscovered = null;
      this.onpeerrequest = null;
      this._queue = [];
      this._sending = false;
      this._closeRequest = null;
      const name = String(options.name || "Player").slice(0, 24);
      try {
        this._nativeId = Number(__tilefinchMultiplayerStart(
          String(mode), this.label, name, String(inviteCode || ""),
        ));
      } catch (_) {
        this._nativeId = 0;
      }
      if (this._nativeId > 0) channels.set(this._nativeId, this);
      else if (!__tilefinchRealmGlobal.__tilefinchScheduleTask(
        () => this._fail("multiplayer unavailable"), 0)) {
        this.readyState = "closed";
        throw new DOMException("Multiplayer task queue is full", "QuotaExceededError");
      }
    }
    _emit(event) {
      dispatch(this, event);
      try {
        const handler = this["on" + event.type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask(
            "multiplayer:" + String(event.type), handler, this, [event],
          );
      } catch (error) {
        globalThis.__tilefinchReportUncaught(error, "multiplayer:" + event.type);
      }
    }
    _fail(detail) {
      if (this.readyState === "closed") return;
      this._finish(1006, String(detail || ""), false, true);
    }
    _finish(code, reason, clean, withError = false) {
      if (this.readyState === "closed") return;
      if (this._nativeId) channels.delete(this._nativeId);
      this._nativeId = 0;
      this.readyState = "closed";
      this._queue.length = 0;
      this._sending = false;
      this._closeRequest = null;
      this.bufferedAmount = 0;
      if (withError)
        this._emit(multiplayerEvent("error", { detail: String(reason || "") }));
      this._emit(createEvent("close", {
        code: Number(code) || 0,
        reason: String(reason || ""),
        wasClean: !!clean,
      }, "CloseEvent"));
    }
    _pump() {
      if (!this._nativeId || this._sending || !this._queue.length) {
        if (!this._sending && !this._queue.length && this._closeRequest
            && this._nativeId) {
          const close = this._closeRequest;
          this._closeRequest = null;
          if (!__tilefinchMultiplayerClose(
              this._nativeId, close.code, close.reason))
            this._fail("close admission failed");
        }
        return;
      }
      if (this.readyState !== "open" && this.readyState !== "closing")
        return;
      const item = this._queue[0];
      if (__tilefinchMultiplayerSend(
          this._nativeId, item.bytes.buffer, item.binary))
        this._sending = true;
    }
    send(data) {
      if (this.readyState !== "open")
        throw new DOMException("Multiplayer channel is not open", "InvalidStateError");
      let bytes, binary = true;
      if (typeof data === "string") {
        bytes = new TextEncoder().encode(data);
        binary = false;
      } else if (data instanceof ArrayBuffer) {
        bytes = new Uint8Array(data.slice(0));
      } else if (ArrayBuffer.isView(data)) {
        bytes = new Uint8Array(
          data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
        );
      } else {
        throw new TypeError("Unsupported multiplayer message type");
      }
      if (bytes.byteLength > messageLimit)
        throw new DOMException("Multiplayer message is too large", "QuotaExceededError");
      if (this._queue.length >= queueLimit
          || bytes.byteLength > bufferedLimit - this.bufferedAmount)
        throw new DOMException("Multiplayer send queue is full", "QuotaExceededError");
      this._queue.push({ bytes, binary });
      this.bufferedAmount += bytes.byteLength;
      this._pump();
    }
    accept(peerIdentity, accepted = true) {
      if (!this._nativeId || this.readyState !== "connecting") return false;
      return !!__tilefinchMultiplayerAccept(
        this._nativeId, Number(peerIdentity) >>> 0, !!accepted,
      );
    }
    addRemoteCode(code) {
      if (!this._nativeId || this.readyState !== "connecting") return false;
      return !!__tilefinchMultiplayerAddRemoteCode(
        this._nativeId, String(code || ""),
      );
    }
    close(code = 1000, reason = "") {
      code = Number(code);
      reason = String(reason);
      if (!Number.isInteger(code)
          || (code !== 1000 && (code < 3000 || code > 4999)))
        throw new DOMException("Invalid close code", "InvalidAccessError");
      if (new TextEncoder().encode(reason).length > 63)
        throw new DOMException("Close reason is too long", "SyntaxError");
      if (this.readyState === "closed" || this.readyState === "closing") return;
      this.readyState = "closing";
      this._closeRequest = { code, reason };
      this._pump();
    }
  }
  const openChannel = (mode, code, options) => {
    options = options && typeof options === "object" ? options : {};
    if (!String(options.gameId || ""))
      throw new TypeError("gameId is required");
    return new TilefinchMultiplayerChannel(mode, options, code);
  };
  /* compat.js reads and deletes this in the statement after the load. */
  defineProperty(globalThis, "__tilefinchCreateMultiplayer", {
    __proto__: null,
    configurable: true,
    value: openChannel,
  });
  publish?.("__tilefinchDeliverMultiplayer", (
    id, kind, payload, detail, code, clean, peerIdentity, inviteCode, peerName,
  ) => {
    const channel = channels.get(Number(id));
    if (!channel || channel.readyState === "closed") return false;
    if (kind === "open") {
      if (channel.readyState !== "connecting") return false;
      channel.readyState = "open";
      channel._emit(createEvent("open"));
      channel._pump();
    } else if (kind === "binary" || kind === "text") {
      if (channel.readyState !== "open") return false;
      const buffer = payload instanceof ArrayBuffer
        ? payload : new Uint8Array(payload).buffer;
      const data = kind === "text"
        ? new TextDecoder().decode(buffer) : buffer;
      channel._emit(createEvent("message", {
        data, origin: "", source: null,
      }, "MessageEvent"));
    } else if (kind === "drain") {
      const before = channel.bufferedAmount;
      const item = channel._queue.shift();
      channel._sending = false;
      if (item) channel.bufferedAmount = Math.max(
        0, channel.bufferedAmount - item.bytes.byteLength,
      );
      if (before > channel.bufferedAmountLowThreshold
          && channel.bufferedAmount <= channel.bufferedAmountLowThreshold)
        channel._emit(createEvent("bufferedamountlow"));
      channel._pump();
    } else if (kind === "status") {
      channel._emit(multiplayerEvent("status", { detail: String(detail || "") }));
    } else if (kind === "invitecode") {
      channel._emit(multiplayerEvent("invitecode", {
        code: String(inviteCode || ""), detail: String(detail || ""),
      }));
    } else if (kind === "discovered" || kind === "peerrequest") {
      channel._emit(multiplayerEvent(kind, {
        peerIdentity: Number(peerIdentity) >>> 0,
        code: String(inviteCode || ""),
        name: String(peerName || ""),
        detail: String(detail || ""),
      }));
    } else {
      channel._finish(code, detail, clean);
    }
    return true;
  });
})();
