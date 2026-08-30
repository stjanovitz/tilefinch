"use strict";

const assert = require("node:assert/strict");
const path = require("node:path");

Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  value: {},
});

let nextPeerId = 1;
const peers = new Map();

class FakeDataChannel extends EventTarget {
  constructor(label, options = {}) {
    super();
    this.label = label;
    this.protocol = options.protocol || "";
    this.ordered = options.ordered !== false;
    this.maxRetransmits = options.maxRetransmits ?? null;
    this.readyState = "connecting";
    this.binaryType = "arraybuffer";
    this.bufferedAmount = 0;
    this.bufferedAmountLowThreshold = 0;
    this.remote = null;
  }

  send(data) {
    if (this.readyState !== "open") throw new Error("channel not open");
    const payload = data instanceof ArrayBuffer ? data.slice(0) : data;
    queueMicrotask(() => {
      if (this.remote && this.remote.readyState === "open")
        this.remote.dispatchEvent(new MessageEvent("message", {data: payload}));
    });
  }

  close() {
    if (this.readyState === "closed") return;
    this.readyState = "closed";
    queueMicrotask(() => this.dispatchEvent(new Event("close")));
    if (this.remote && this.remote.readyState !== "closed") {
      this.remote.readyState = "closed";
      queueMicrotask(() => this.remote.dispatchEvent(new Event("close")));
    }
  }
}

class FakePeerConnection extends EventTarget {
  constructor(configuration) {
    super();
    this.id = nextPeerId++;
    this.configuration = configuration;
    this.iceGatheringState = "complete";
    this.iceConnectionState = "new";
    this.connectionState = "new";
    this.localDescription = null;
    this.remoteDescription = null;
    this.channel = null;
    this.hostId = 0;
    peers.set(this.id, this);
  }

  createDataChannel(label, options) {
    this.channel = new FakeDataChannel(label, options);
    return this.channel;
  }

  async createOffer() { return {type: "offer", sdp: `fake-offer:${this.id}`}; }
  async createAnswer() {
    return {type: "answer", sdp: `fake-answer:${this.id}:${this.hostId}`};
  }

  async setLocalDescription(description) {
    this.localDescription = description;
  }

  async setRemoteDescription(description) {
    this.remoteDescription = description;
    if (description.type === "offer") {
      const match = /^fake-offer:(\d+)$/.exec(description.sdp);
      if (!match) throw new Error("bad offer");
      this.hostId = Number(match[1]);
      return;
    }
    const match = /^fake-answer:(\d+):(\d+)$/.exec(description.sdp);
    if (!match || Number(match[2]) !== this.id) throw new Error("bad answer");
    const guest = peers.get(Number(match[1]));
    if (!guest || !this.channel) throw new Error("missing peer");
    const guestChannel = new FakeDataChannel(this.channel.label, {
      ordered: this.channel.ordered,
      maxRetransmits: this.channel.maxRetransmits,
      protocol: this.channel.protocol,
    });
    this.channel.remote = guestChannel;
    guestChannel.remote = this.channel;
    guest.channel = guestChannel;
    /* Event has no channel field; expose the WebRTC event shape. */
    const event = new Event("datachannel");
    Object.defineProperty(event, "channel", {value: guestChannel});
    guest.dispatchEvent(event);
    this.connectionState = guest.connectionState = "connected";
    this.iceConnectionState = guest.iceConnectionState = "connected";
    this.dispatchEvent(new Event("connectionstatechange"));
    guest.dispatchEvent(new Event("connectionstatechange"));
    this.channel.readyState = guestChannel.readyState = "open";
    queueMicrotask(() => {
      this.channel.dispatchEvent(new Event("open"));
      guestChannel.dispatchEvent(new Event("open"));
    });
  }

  close() {
    this.connectionState = "closed";
    peers.delete(this.id);
  }
}

globalThis.RTCPeerConnection = FakePeerConnection;
require(path.resolve(__dirname, "../examples/treadline-arena/multiplayer-web.js"));

function once(target, type) {
  return new Promise(resolve => target.addEventListener(type, resolve, {once: true}));
}

function onceWhere(target, type, predicate) {
  return new Promise(resolve => {
    const listener = event => {
      if (!predicate(event)) return;
      target.removeEventListener(type, listener);
      resolve(event);
    };
    target.addEventListener(type, listener);
  });
}

function wrapToken(token) {
  return token.replace(/.{64}/g, "$&\n");
}

async function legacyCompressedToken(rawToken) {
  assert.match(rawToken, /^TFW1J\./);
  const encoded = rawToken.slice(6).replace(/-/g, "+").replace(/_/g, "/");
  const binary = atob(encoded + "=".repeat((4 - encoded.length % 4) % 4));
  const source = Uint8Array.from(binary, character => character.charCodeAt(0));
  const stream = new Blob([source]).stream()
    .pipeThrough(new CompressionStream("deflate"));
  const reader = stream.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const item = await reader.read();
    if (item.done) break;
    chunks.push(item.value);
    total += item.value.byteLength;
  }
  const compressed = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    compressed.set(chunk, offset);
    offset += chunk.byteLength;
  }
  let packed = "";
  for (const byte of compressed) packed += String.fromCharCode(byte);
  return "TFW1Z." + btoa(packed).replace(/\+/g, "-")
    .replace(/\//g, "_").replace(/=+$/, "");
}

async function connectPair(api, options, offerTransform = value => value) {
  const host = api.host(options);
  const firstStatus = await once(host, "status");
  assert.equal(firstStatus.phase, "preparing-offer");
  const hostInvite = (await once(host, "invitecode")).code;
  assert.match(hostInvite, /^TFW1J\./);
  const guest = api.join(await offerTransform(hostInvite), options);
  const guestAnswer = (await once(guest, "invitecode")).code;
  assert.match(guestAnswer, /^TFW1J\./);
  return {host, guest, hostInvite, guestAnswer};
}

async function main() {
  const api = navigator.tilefinchMultiplayer;
  assert.equal(api.transport, "webrtc");
  assert.equal(api.pairingMode, "manual-offer-answer");
  assert.equal(api.maxMessageSize, 512);
  assert.throws(() => api.discover(), error => error.name === "NotSupportedError");
  assert.throws(
    () => api.host({gameId: "x".repeat(33)}),
    error => error instanceof RangeError,
  );

  const options = {gameId: "treadline-arena-v1", name: "Host"};
  const savedDecompressionStream = globalThis.DecompressionStream;
  globalThis.DecompressionStream = undefined;
  let host, guest, hostInvite, guestAnswer;
  try {
    ({host, guest, hostInvite, guestAnswer} = await connectPair(
      api, options, value => wrapToken(value),
    ));
  } finally {
    globalThis.DecompressionStream = savedDecompressionStream;
  }
  assert.equal(host.readyState, "connecting");
  let statusAfterOpen = false;
  host.addEventListener("status", () => {
    if (host.readyState === "open") statusAfterOpen = true;
  });

  const retry = onceWhere(host, "status", event => event.phase === "retry-answer");
  assert.equal(host.addRemoteCode("TFW1J.not-valid-base64"), true);
  assert.match((await retry).detail, /not accepted/i);
  assert.equal(host.readyState, "connecting");
  const hostOpen = once(host, "open");
  const guestOpen = once(guest, "open");
  assert.equal(host.addRemoteCode(guestAnswer), true);
  await Promise.all([hostOpen, guestOpen]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(statusAfterOpen, false);
  assert.equal(host.readyState, "open");
  assert.equal(guest.readyState, "open");
  assert.equal(host.ordered, false);
  assert.equal(host.maxRetransmits, 0);

  const received = once(guest, "message");
  host.send(new Uint8Array([7, 8, 9]));
  assert.deepEqual(Array.from(new Uint8Array((await received).data)), [7, 8, 9]);
  assert.throws(
    () => host.send(new Uint8Array(513)),
    error => error.name === "QuotaExceededError",
  );

  let hostMessages = 0, guestMessages = 0;
  let hostChecksum = 0, guestChecksum = 0;
  host.addEventListener("message", event => {
    hostMessages++;
    hostChecksum = (hostChecksum + new Uint8Array(event.data)[0]) >>> 0;
  });
  guest.addEventListener("message", event => {
    guestMessages++;
    guestChecksum = (guestChecksum + new Uint8Array(event.data)[0]) >>> 0;
  });
  /* Ten minutes at the game's 20 Hz input and 15 Hz snapshot rates. */
  for (let second = 0; second < 600; second++) {
    for (let at = 0; at < 20; at++) guest.send(new Uint8Array([1, at, second & 255]));
    for (let at = 0; at < 15; at++) host.send(new Uint8Array([2, at, second & 255]));
    await new Promise(resolve => setImmediate(resolve));
  }
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(hostMessages, 12000);
  assert.equal(guestMessages, 9000);
  assert.equal(hostChecksum, 12000);
  assert.equal(guestChecksum, 9000 * 2);

  const closed = once(guest, "close");
  host.close(1000, "done");
  assert.equal((await closed).wasClean, true);
  assert.equal(guest.readyState, "closed");
  assert.equal(host._peer, null);
  assert.equal(host._data, null);
  assert.equal(guest._peer, null);
  assert.equal(guest._data, null);

  if (typeof CompressionStream === "function"
      && typeof DecompressionStream === "function") {
    const legacy = await connectPair(api, options, legacyCompressedToken);
    const legacyHostOpen = once(legacy.host, "open");
    const legacyGuestOpen = once(legacy.guest, "open");
    assert.equal(legacy.host.addRemoteCode(legacy.guestAnswer), true);
    await Promise.all([legacyHostOpen, legacyGuestOpen]);
    legacy.host.close(1000, "legacy compressed input accepted");
  }

  const wrongGame = api.join(hostInvite, {gameId: "another-game", name: "Guest"});
  const failed = await once(wrongGame, "error");
  assert.match(failed.detail, /another game|phase/i);
  assert.equal(wrongGame.readyState, "closed");

  const failedHost = api.host(options);
  await once(failedHost, "invitecode");
  const connectionError = once(failedHost, "error");
  failedHost._peer.connectionState = "failed";
  failedHost._peer.dispatchEvent(new Event("connectionstatechange"));
  assert.match((await connectionError).detail, /direct connection failed/i);
  assert.equal(failedHost.readyState, "closed");

  const cancelledHost = api.host(options);
  let cancelledInvite = false;
  cancelledHost.addEventListener("invitecode", () => { cancelledInvite = true; });
  cancelledHost.close(1000, "cancelled");
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(cancelledInvite, false);

  await new Promise(resolve => setImmediate(resolve));
  assert.equal(peers.size, 0);

  console.log("treadline web multiplayer adapter: ok; 10-minute packet soak passed");
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
