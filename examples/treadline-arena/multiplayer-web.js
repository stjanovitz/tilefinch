(() => {
  "use strict";

  if (navigator.tilefinchMultiplayer || typeof RTCPeerConnection !== "function")
    return;

  const MESSAGE_LIMIT = 512;
  const QUEUE_LIMIT = 8;
  const BUFFERED_LIMIT = MESSAGE_LIMIT * QUEUE_LIMIT;
  const TOKEN_LIMIT = 32768;
  const DESCRIPTION_LIMIT = 24576;
  const TOKEN_JSON = "TFW1J.";
  const TOKEN_DEFLATE = "TFW1Z.";
  const CONNECTION_LIMIT_MS = 30000;
  const DISCONNECT_GRACE_MS = 10000;
  const DEFAULT_ICE_SERVERS = Object.freeze([
    Object.freeze({urls: "stun:stun.cloudflare.com:3478"}),
    Object.freeze({urls: "stun:stun.l.google.com:19302"}),
  ]);

  const eventWith = (type, fields = {}) => {
    const event = new Event(type);
    for (const [name, value] of Object.entries(fields))
      Object.defineProperty(event, name, {
        value, enumerable: true, configurable: true,
      });
    return event;
  };

  function bytesToBase64Url(bytes) {
    let binary = "";
    for (let start = 0; start < bytes.length; start += 4096) {
      const end = Math.min(bytes.length, start + 4096);
      for (let at = start; at < end; at++) binary += String.fromCharCode(bytes[at]);
    }
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  function base64UrlToBytes(text) {
    if (!/^[A-Za-z0-9_-]+$/.test(text)) throw new Error("Pairing text is malformed.");
    const padded = text.replace(/-/g, "+").replace(/_/g, "/")
      + "=".repeat((4 - text.length % 4) % 4);
    const binary = atob(padded);
    if (binary.length > TOKEN_LIMIT) throw new Error("Pairing text is too large.");
    const bytes = new Uint8Array(binary.length);
    for (let at = 0; at < binary.length; at++) bytes[at] = binary.charCodeAt(at);
    return bytes;
  }

  async function readBounded(stream, limit) {
    const reader = stream.getReader();
    const chunks = [];
    let total = 0;
    try {
      for (;;) {
        const item = await reader.read();
        if (item.done) break;
        if (!(item.value instanceof Uint8Array)
            || item.value.byteLength > limit - total)
          throw new Error("Pairing text expands beyond its limit.");
        chunks.push(item.value);
        total += item.value.byteLength;
      }
    } catch (error) {
      try { await reader.cancel(error); } catch (_) {}
      throw error;
    } finally {
      reader.releaseLock();
    }
    const output = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      output.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return output;
  }

  async function encodeDescription(kind, gameId, description) {
    const source = new TextEncoder().encode(JSON.stringify({
      version: 1,
      kind,
      gameId,
      description: {type: description.type, sdp: description.sdp},
    }));
    if (source.byteLength > DESCRIPTION_LIMIT)
      throw new Error("The browser generated an oversized pairing description.");
    /*
     * Always emit the raw v1 envelope.  A sender cannot know whether the
     * receiving browser implements DecompressionStream, so choosing DEFLATE
     * opportunistically produces pairing text that some otherwise compatible
     * browsers cannot read.  Compressed v1 input remains supported for tokens
     * made by earlier Treadline builds.
     */
    const packed = TOKEN_JSON + bytesToBase64Url(source);
    if (packed.length > TOKEN_LIMIT) throw new Error("Pairing text is too large.");
    return packed;
  }

  async function decodeDescription(token, expectedKind, gameId) {
    token = String(token || "");
    if (token.length > TOKEN_LIMIT) throw new Error("Pairing text is too large.");
    /* Messaging tools may wrap a long token for display. */
    token = token.replace(/[\t\n\r ]+/g, "");
    let bytes;
    if (token.startsWith(TOKEN_DEFLATE)) {
      if (typeof DecompressionStream !== "function")
        throw new Error("This browser cannot read the compressed pairing text.");
      const compressed = base64UrlToBytes(token.slice(TOKEN_DEFLATE.length));
      bytes = await readBounded(
        new Blob([compressed]).stream().pipeThrough(new DecompressionStream("deflate")),
        DESCRIPTION_LIMIT,
      );
    } else if (token.startsWith(TOKEN_JSON)) {
      bytes = base64UrlToBytes(token.slice(TOKEN_JSON.length));
      if (bytes.byteLength > DESCRIPTION_LIMIT)
        throw new Error("Pairing text is too large.");
    } else {
      throw new Error("This is not a Treadline web pairing string.");
    }
    let value;
    try {
      value = JSON.parse(new TextDecoder("utf-8", {fatal: true}).decode(bytes));
    } catch (_) {
      throw new Error("Pairing text is malformed.");
    }
    if (!value || value.version !== 1 || value.kind !== expectedKind
        || value.gameId !== gameId || !value.description
        || value.description.type !== expectedKind
        || typeof value.description.sdp !== "string"
        || value.description.sdp.length > DESCRIPTION_LIMIT)
      throw new Error("Pairing text belongs to another game or phase.");
    return value.description;
  }

  function boundedIceServers(options) {
    const requested = options && Array.isArray(options.iceServers)
      ? options.iceServers : DEFAULT_ICE_SERVERS;
    const result = [];
    for (let at = 0; at < requested.length && result.length < 2; at++) {
      const urls = typeof requested[at] === "string"
        ? requested[at] : requested[at] && requested[at].urls;
      if (typeof urls !== "string" || urls.length > 256
          || !/^stuns?:/i.test(urls)) continue;
      result.push({urls});
    }
    return result;
  }

  function waitForIceGathering(peer) {
    if (peer.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => finish(false), 12000);
      const changed = () => {
        if (peer.iceGatheringState === "complete") finish(true);
      };
      function finish(complete) {
        clearTimeout(timer);
        peer.removeEventListener("icegatheringstatechange", changed);
        const sdp = peer.localDescription && peer.localDescription.sdp;
        if (complete || (typeof sdp === "string" && /(?:^|\r?\n)a=candidate:/m.test(sdp)))
          resolve();
        else reject(new Error("Could not finish network discovery. Try again."));
      }
      peer.addEventListener("icegatheringstatechange", changed);
    });
  }

  class WebMultiplayerChannel extends EventTarget {
    constructor(mode, code, options) {
      super();
      this.label = String(options.gameId || "").slice(0, 32);
      this.protocol = "tilefinch-game-v1";
      this.ordered = false;
      this.maxPacketLifeTime = null;
      this.maxRetransmits = 0;
      this.negotiated = false;
      this.id = null;
      this.readyState = "connecting";
      this.binaryType = "arraybuffer";
      this._bufferedAmountLowThreshold = 0;
      this.onopen = null;
      this.onmessage = null;
      this.onbufferedamountlow = null;
      this.onclose = null;
      this.onerror = null;
      this.onstatus = null;
      this.oninvitecode = null;
      this.ondiscovered = null;
      this.onpeerrequest = null;
      this._mode = mode;
      this._data = null;
      this._closed = false;
      this._remotePending = false;
      this._answerApplied = false;
      this._connectionTimer = 0;
      this._disconnectTimer = 0;
      this._peer = new RTCPeerConnection({
        iceServers: boundedIceServers(options),
        bundlePolicy: "max-bundle",
      });
      this._peer.addEventListener("connectionstatechange", () =>
        this._connectionStateChanged());
      this._peer.addEventListener("iceconnectionstatechange", () =>
        this._connectionStateChanged());
      if (mode === "host") {
        this._bindDataChannel(this._peer.createDataChannel("tilefinch-game", {
          ordered: false, maxRetransmits: 0, protocol: this.protocol,
        }));
      } else {
        this._peer.addEventListener("datachannel", event => {
          if (!this._data) this._bindDataChannel(event.channel);
          else event.channel.close();
        });
      }
      /* Let callers install setup handlers before the first status event. */
      queueMicrotask(() => {
        if (this._closed) return;
        if (mode === "host") this._startHost();
        else this._startGuest(code);
      });
    }

    get bufferedAmount() {
      return this._data ? this._data.bufferedAmount : 0;
    }

    get bufferedAmountLowThreshold() { return this._bufferedAmountLowThreshold; }

    set bufferedAmountLowThreshold(value) {
      value = Number(value);
      if (!Number.isFinite(value) || value < 0) value = 0;
      this._bufferedAmountLowThreshold = Math.min(BUFFERED_LIMIT, Math.floor(value));
      if (this._data)
        this._data.bufferedAmountLowThreshold = this._bufferedAmountLowThreshold;
    }

    _emit(event) {
      this.dispatchEvent(event);
      const handler = this["on" + event.type];
      if (typeof handler === "function") {
        try { handler.call(this, event); }
        catch (error) { queueMicrotask(() => { throw error; }); }
      }
    }

    _clearConnectionTimer() {
      if (this._connectionTimer) clearTimeout(this._connectionTimer);
      this._connectionTimer = 0;
    }

    _clearDisconnectTimer() {
      if (this._disconnectTimer) clearTimeout(this._disconnectTimer);
      this._disconnectTimer = 0;
    }

    _armConnectionTimer() {
      if (this._closed || !this._answerApplied
          || this.readyState === "open" || this._connectionTimer) return;
      this._connectionTimer = setTimeout(() => {
        this._connectionTimer = 0;
        this._fail("Direct connection timed out. A restrictive NAT may block WebRTC.");
      }, CONNECTION_LIMIT_MS);
    }

    _connectionStateChanged() {
      if (this._closed || !this._peer) return;
      const state = this._peer.connectionState;
      const ice = this._peer.iceConnectionState;
      if (state === "failed" || ice === "failed") {
        this._fail("Direct connection failed. A restrictive NAT may block WebRTC.");
        return;
      }
      if (state === "closed" || ice === "closed") {
        this._fail("The browser closed the direct connection.");
        return;
      }
      if (state === "connected" || ice === "connected" || ice === "completed") {
        this._clearDisconnectTimer();
        if (this.readyState === "connecting") this._armConnectionTimer();
        return;
      }
      if (state === "connecting" || ice === "checking") this._armConnectionTimer();
      if (state === "disconnected" || ice === "disconnected") {
        this._emit(eventWith("status", {
          detail: "Connection interrupted; trying to recover…",
          phase: "interrupted",
        }));
        if (!this._disconnectTimer) {
          this._disconnectTimer = setTimeout(() => {
            this._disconnectTimer = 0;
            this._fail("The direct connection did not recover.");
          }, DISCONNECT_GRACE_MS);
        }
      }
    }

    _bindDataChannel(channel) {
      if (!channel || channel.label !== "tilefinch-game"
          || channel.protocol !== this.protocol || channel.ordered !== false
          || channel.maxRetransmits !== 0) {
        if (channel) channel.close();
        this._fail("The peer requested an incompatible game channel.");
        return;
      }
      this._data = channel;
      channel.binaryType = "arraybuffer";
      channel.bufferedAmountLowThreshold = this._bufferedAmountLowThreshold;
      channel.addEventListener("open", () => {
        if (this._closed || this.readyState !== "connecting") return;
        this._clearConnectionTimer();
        this._clearDisconnectTimer();
        this.readyState = "open";
        this._emit(new Event("open"));
      });
      channel.addEventListener("message", event => {
        if (this._closed) return;
        let bytes = 0;
        if (typeof event.data === "string")
          bytes = new TextEncoder().encode(event.data).byteLength;
        else if (event.data instanceof ArrayBuffer) bytes = event.data.byteLength;
        else if (ArrayBuffer.isView(event.data)) bytes = event.data.byteLength;
        else return;
        if (bytes > MESSAGE_LIMIT) {
          this._emit(eventWith("error", {detail: "Peer sent an oversized message."}));
          return;
        }
        this._emit(new MessageEvent("message", {data: event.data, origin: ""}));
      });
      channel.addEventListener("bufferedamountlow", () =>
        this._emit(new Event("bufferedamountlow")));
      channel.addEventListener("close", () => this._finish(1000, "", true));
      channel.addEventListener("error", () =>
        this._fail("The browser data channel failed."));
    }

    async _startHost() {
      try {
        this._emit(eventWith("status", {
          detail: "Preparing a private offer…", phase: "preparing-offer",
        }));
        await this._peer.setLocalDescription(await this._peer.createOffer());
        await waitForIceGathering(this._peer);
        const code = await encodeDescription("offer", this.label, this._peer.localDescription);
        if (!this._closed)
          this._emit(eventWith("invitecode", {
            code, detail: "Copy this offer to the other player.",
            kind: "offer", phase: "waiting-answer",
          }));
      } catch (error) {
        this._fail(error && error.message ? error.message : "Could not create an offer.");
      }
    }

    async _startGuest(code) {
      try {
        this._emit(eventWith("status", {
          detail: "Reading the host offer…", phase: "reading-offer",
        }));
        const offer = await decodeDescription(code, "offer", this.label);
        await this._peer.setRemoteDescription(offer);
        await this._peer.setLocalDescription(await this._peer.createAnswer());
        await waitForIceGathering(this._peer);
        const answer = await encodeDescription(
          "answer", this.label, this._peer.localDescription,
        );
        if (!this._closed)
          this._emit(eventWith("invitecode", {
            code: answer, detail: "Copy this response back to the host.",
            kind: "answer", phase: "waiting-host",
          }));
      } catch (error) {
        this._fail(error && error.message ? error.message : "Could not join the host.");
      }
    }

    send(data) {
      if (this.readyState !== "open" || !this._data)
        throw new DOMException("Multiplayer channel is not open", "InvalidStateError");
      let payload = data, size;
      if (typeof data === "string") size = new TextEncoder().encode(data).byteLength;
      else if (data instanceof ArrayBuffer) {
        payload = data.slice(0); size = payload.byteLength;
      } else if (ArrayBuffer.isView(data)) {
        payload = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
        size = payload.byteLength;
      } else throw new TypeError("Unsupported multiplayer message type");
      if (size > MESSAGE_LIMIT)
        throw new DOMException("Multiplayer message is too large", "QuotaExceededError");
      if (size > BUFFERED_LIMIT - this._data.bufferedAmount)
        throw new DOMException("Multiplayer send queue is full", "QuotaExceededError");
      this._data.send(payload);
    }

    accept() { return false; }

    addRemoteCode(code) {
      if (this._mode !== "host" || this._closed
          || this._remotePending || this._peer.remoteDescription) return false;
      this._remotePending = true;
      this._applyAnswer(code);
      return true;
    }

    async _applyAnswer(code) {
      try {
        this._emit(eventWith("status", {
          detail: "Checking the guest response…", phase: "checking-answer",
        }));
        const answer = await decodeDescription(code, "answer", this.label);
        await this._peer.setRemoteDescription(answer);
        if (this._closed) return;
        this._answerApplied = true;
        if (this.readyState === "open") return;
        this._emit(eventWith("status", {
          detail: "Connecting directly to the guest…", phase: "connecting",
        }));
        this._armConnectionTimer();
      } catch (error) {
        if (this._closed) return;
        this._remotePending = false;
        const detail = error && error.message
          ? error.message : "Could not read the response.";
        this._emit(eventWith("status", {
          detail: `Response not accepted: ${detail}`, phase: "retry-answer",
        }));
      }
    }

    close(code = 1000, reason = "") {
      code = Number(code);
      reason = String(reason);
      if (!Number.isInteger(code)
          || (code !== 1000 && (code < 3000 || code > 4999)))
        throw new DOMException("Invalid close code", "InvalidAccessError");
      if (new TextEncoder().encode(reason).byteLength > 63)
        throw new DOMException("Close reason is too long", "SyntaxError");
      if (this._closed) return;
      this.readyState = "closing";
      if (this._data) this._data.close();
      this._finish(code, reason, true);
    }

    _fail(detail) {
      if (this._closed) return;
      this._emit(eventWith("error", {detail: String(detail || "")}));
      this._finish(1006, String(detail || ""), false);
    }

    _finish(code, reason, clean) {
      if (this._closed) return;
      this._closed = true;
      this._clearConnectionTimer();
      this._clearDisconnectTimer();
      this.readyState = "closed";
      const data = this._data;
      const peer = this._peer;
      this._data = null;
      this._peer = null;
      if (data && data.readyState !== "closed") data.close();
      if (peer) peer.close();
      this._emit(eventWith("close", {
        code: Number(code) || 0,
        reason: String(reason || ""),
        wasClean: !!clean,
      }));
    }
  }

  function open(mode, code, options) {
    options = options && typeof options === "object" ? options : {};
    const gameId = String(options.gameId || "");
    if (!gameId) throw new TypeError("gameId is required");
    if (gameId.length > 32) throw new RangeError("gameId is too long");
    return new WebMultiplayerChannel(mode, code, options);
  }

  Object.defineProperty(navigator, "tilefinchMultiplayer", {
    configurable: false,
    enumerable: false,
    value: Object.freeze({
      host(options) { return open("host", "", options); },
      join(code, options) { return open("guest", String(code || ""), options); },
      discover() {
        throw new DOMException("LAN discovery is available only in Tilefinch", "NotSupportedError");
      },
      supported: true,
      transport: "webrtc",
      pairingMode: "manual-offer-answer",
      maxMessageSize: MESSAGE_LIMIT,
    }),
  });
})();
