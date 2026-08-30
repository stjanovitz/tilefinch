# Direct multiplayer for offline games

Tilefinch gives installed offline games one bounded, direct peer channel. Its
surface is shaped like an `RTCDataChannel`, allowing the same game protocol to
use Tilefinch's direct UDP transport on PSP and a small WebRTC adapter in an
ordinary browser. The transports do not cross-connect: PSP players play other
PSP players, and web players play other web players.

The feature needs no Tilefinch account, companion application, or game
service. It works automatically on the same Wi-Fi and can often connect over
the Internet with a short numeric invite. Direct connectivity is not
guaranteed: symmetric NAT and carrier-grade NAT can require a relay, and this
profile deliberately has no relay.

## Player flow

Treadline Arena demonstrates all three bounded paths:

1. **Find LAN** discovers hosts for the same game on the local network.
2. **Host online** displays a 12-digit code in the usual fixed-port case. The
   other player enters it with a PSP-sized numeric keypad.
3. If the first code cannot establish a path, the joining player can share
   the response code shown on their screen. The host chooses **Enter response
   code** so both peers send bounded UDP probes through their NATs.

Codes contain an IPv4 endpoint and a six-bit transcription check. They are
grouped for reading but use digits only. A less common 17-digit form includes
an alternate mapped port. Codes are invitations, not secrets; the host must
still approve the named peer before gameplay opens.

Tilefinch contacts the configured public STUN endpoint only after the user
starts multiplayer. STUN observes the user's public IP address and UDP port,
as any direct peer would, but receives no game payload. LAN discovery uses one
small broadcast per second while its screen is active. Neither path writes to
the Memory Stick.

## Page API

The API exists for feature detection as `navigator.tilefinchMultiplayer`, but
native admission requires all of the following:

- the document is an installed Tilefinch offline app;
- the call occurs synchronously from a user activation;
- no other multiplayer session is active in the process; and
- the fixed page and network budgets admit the session.

```js
const channel = navigator.tilefinchMultiplayer.host({
  gameId: "my-game-v1",
  name: "Player",
});

channel.oninvitecode = event => showCode(event.code);
channel.onpeerrequest = event => {
  showApproval(event.name, accepted =>
    channel.accept(event.peerIdentity, accepted));
};
channel.onopen = () => channel.send(new Uint8Array([1, 2, 3]));
channel.onmessage = event => receive(new Uint8Array(event.data));
channel.onclose = event => returnToLobby(event.reason);
```

Join and LAN discovery use the same channel object:

```js
const joined = navigator.tilefinchMultiplayer.join(code, options);
const browser = navigator.tilefinchMultiplayer.discover(options);

browser.ondiscovered = event => {
  // After the player chooses this result, reuse the live UDP session.
  browser.addRemoteCode(event.code);
};
```

The channel deliberately mirrors the useful `RTCDataChannel` surface:
`readyState`, `label`, `protocol`, `ordered`, `maxRetransmits`, `binaryType`,
`bufferedAmount`, `bufferedAmountLowThreshold`, `send()`, `close()`, and the
ordinary `open`, `message`, `bufferedamountlow`, `error`, and `close` events.
Tilefinch-specific setup events are `status`, `invitecode`, `discovered`, and
`peerrequest`; setup methods are `accept()` and `addRemoteCode()`.

This is an unordered, zero-retransmit datagram channel. A host-authoritative
game should send small input records repeatedly, publish replaceable snapshots,
ignore stale sequence numbers, and keep menus usable if packets disappear.
Treadline sends 12-byte input records at 20 Hz and snapshots of at most 320
bytes at 15 Hz. Its version-2 snapshot header repeats the authoritative arena
seed and a layout checksum for reconnect and late join, plus a 16-bit active
barrier mask so destroyed cover cannot reappear on a client. An incompatible
packet version is refused rather than partially applied.

## Bounds and lifecycle

- One direct channel per Tilefinch process.
- 512 bytes per message.
- Eight queued outgoing messages and eight ordinary pending events.
- Six retained LAN discoveries.
- A 16 KiB worker-stack reservation plus fixed command/event storage, all
  charged to the page `Budget`.
- A 30-second connection deadline, one-second keepalive, and 15-second peer
  timeout.
- Navigation, runtime destruction, suspension cleanup, or explicit close
  cancels the worker, releases the network-supervisor lease, and returns all
  page ownership.

The PSP worker owns DNS, STUN, UDP receive, and handshake work. The browser
thread only moves fixed records through single-producer/single-consumer queues,
so a slow network cannot block rendering or input.

## Web-to-web play

The Treadline example conditionally loads `multiplayer-web.js` only when the
native Tilefinch API is absent. It installs the same channel-shaped surface on
`navigator.tilefinchMultiplayer`, backed by an unordered WebRTC DataChannel
with zero retransmits and the same 512-byte message and eight-message buffering
limits. Tilefinch never requests or parses this adapter on PSP.

Web pairing is service-free and deliberately manual:

1. The host copies the generated `TFW1` offer to the guest through any ordinary
   messaging channel.
2. The guest pastes it, then copies the generated response back to the host.
3. The host pastes the response and the browsers establish the encrypted direct
   DataChannel.

Each bounded pairing string contains a complete, non-trickle ICE description.
New offers use a Base64url-encoded JSON v1 envelope so they remain readable in
browsers without `DecompressionStream`; the reader also accepts the bounded
DEFLATE form emitted by earlier builds. Whitespace inserted while wrapping a
token is ignored. The adapter uses public STUN servers but no TURN server,
relay, account, lobby, or matchmaking service. STUN and the other player learn
the public endpoint, and restrictive or symmetric NATs can prevent a
connection. Pairing strings use a separate format from PSP numeric codes, so
accidental cross-platform attempts fail immediately instead of timing out.

The adapter adds no short deadline while the host waits for a response, though
the response should be returned promptly because browsers also enforce their
own ICE lifetime. Once the host applies a complete response, the adapter bounds
connection setup to 30 seconds and gives an interrupted live connection ten
seconds to recover. A malformed response leaves the host offer alive so the
response can be corrected rather than restarting both players.

## What this is not

The native PSP profile is not a standards claim for WebRTC,
`RTCPeerConnection`, or `RTCDataChannel`. It has no encryption beyond the
Wi-Fi/network path, no relay, no Internet lobby, no matchmaking identity, no
spectator fan-out, and no host migration. Do not send credentials or private
chat through it. The example's browser adapter uses standard WebRTC encryption,
but its intentionally similar API does not make the two transports wire
compatible.
