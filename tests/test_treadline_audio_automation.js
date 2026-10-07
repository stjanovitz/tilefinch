"use strict";

/* Treadline in an ordinary browser: the shipped SoundBank (game.js) and
   music.js against a model of the Web Audio automation timeline that
   browsers enforce, instead of Tilefinch's own bootstrap subset (which
   accepts more). Desktop Chrome threw NotSupportedError from SoundBank's
   per-frame tick: a sound expires on the wall clock, the audio clock runs
   behind it, and the expiry's value write landed inside the effect's value
   curve. This replays every music scenario plus a dense effects-and-music
   session on a lagging, render-quantum audio clock and fails on the first
   automation call a browser would reject. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const h = require("./treadline_music_harness.js");
const S = require("./treadline_music_scenarios.js");

const QUANTUM = 128 / 48000;

function notSupported(message) {
  return new DOMException(message, "NotSupportedError");
}

/* The automation rules of the Web Audio specification as Chrome applies
   them (AudioParamTimeline::InsertEvent / CancelScheduledValues):
   - every time is clamped up to currentTime;
   - a setValueCurveAtTime(T, D) owns [T, T + D): any other event at a time
     in that interval, or a curve whose span would contain another event
     strictly inside (T, T + D), is a NotSupportedError;
   - assigning .value is setValueAtTime(value, currentTime);
   - cancelScheduledValues(t) removes events at or after t and any curve
     still running at t. */
class AutomationParam {
  constructor(context, value, stats) {
    this.context = context;
    this.events = [];
    this.intrinsic = value;
    this.stats = stats;
  }
  get value() { return this.intrinsic; }
  set value(next) {
    try { this.setValueAtTime(next, this.context.currentTime); }
    catch (error) {
      throw new DOMException("Failed to set the 'value' property on 'AudioParam': "
        + error.message, error.name);
    }
  }
  time(when, name) {
    when = Number(when);
    if (!Number.isFinite(when) || when < 0)
      throw new RangeError(`${name}: time ${when} is out of range`);
    return Math.max(when, this.context.currentTime);
  }
  insert(event, label) {
    this.stats.calls++;
    for (const other of this.events) {
      if (other.type === "curve" && event.time >= other.time
          && event.time < other.time + other.duration)
        throw notSupported(`${label} overlaps setValueCurveAtTime(..., ${other.time}, ${other.duration})`);
      if (event.type === "curve" && other.time > event.time
          && other.time < event.time + event.duration)
        throw notSupported(`${label} overlaps ${other.type} at ${other.time}`);
    }
    this.events.push(event);
    this.events.sort((a, b) => a.time - b.time);
    if (event.type === "set") this.intrinsic = event.value;
    return this;
  }
  setValueAtTime(value, when) {
    const time = this.time(when, "setValueAtTime");
    return this.insert({type: "set", time, value: Number(value)},
      `setValueAtTime(${value}, ${time})`);
  }
  linearRampToValueAtTime(value, when) {
    const time = this.time(when, "linearRampToValueAtTime");
    return this.insert({type: "linear", time, value: Number(value)},
      `linearRampToValueAtTime(${value}, ${time})`);
  }
  exponentialRampToValueAtTime(value, when) {
    const time = this.time(when, "exponentialRampToValueAtTime");
    return this.insert({type: "exponential", time, value: Number(value)},
      `exponentialRampToValueAtTime(${value}, ${time})`);
  }
  setTargetAtTime(value, when, constant) {
    const time = this.time(when, "setTargetAtTime");
    if (!(constant >= 0)) throw new RangeError("time constant out of range");
    return this.insert({type: "target", time, value: Number(value), constant},
      `setTargetAtTime(${value}, ${time}, ${constant})`);
  }
  setValueCurveAtTime(values, when, duration) {
    const time = this.time(when, "setValueCurveAtTime");
    if (!(duration > 0) || values.length < 2)
      throw new RangeError("curve duration or length out of range");
    return this.insert({type: "curve", time, duration, values: Float32Array.from(values)},
      `setValueCurveAtTime(..., ${time}, ${duration})`);
  }
  cancelScheduledValues(when) {
    const time = this.time(when, "cancelScheduledValues");
    this.stats.calls++;
    this.events = this.events.filter((event) => event.time < time
      && !(event.type === "curve" && event.time + event.duration > time));
    return this;
  }
}

/* An AudioContext on the harness clock. Its currentTime advances in render
   quanta behind the wall clock by an output latency that varies from frame
   to frame, the way requestAnimationFrame time leads it in a browser. */
function browserAudio(stats) {
  return (clock) => {
    let last = 0;
    class Node {
      constructor(context) { this.context = context; }
      connect(target) { return target; }
      disconnect() {}
      start() {}
      stop() {}
    }
    return class AudioContext {
      constructor() { this.destination = new Node(this); this.sampleRate = 48000; }
      get state() { return "running"; }
      get currentTime() {
        const frame = Math.round(clock.seconds * 1000);
        const latency = .012 + ((Math.imul(frame, 2654435761) >>> 0) / 2 ** 32) * .03;
        const now = Math.floor(Math.max(0, clock.seconds - latency) / QUANTUM) * QUANTUM;
        if (now > last) last = now;
        return last;
      }
      resume() { return Promise.resolve(); }
      close() { return Promise.resolve(); }
      createGain() {
        const node = new Node(this);
        node.gain = new AutomationParam(this, 1, stats);
        return node;
      }
      createOscillator() {
        const node = new Node(this);
        node.type = "sine";
        node.frequency = new AutomationParam(this, 440, stats);
        return node;
      }
      createBufferSource() {
        const node = new Node(this);
        node.buffer = null; node.loop = false;
        return node;
      }
      decodeAudioData() {
        return Promise.resolve({duration: .032, length: 256, sampleRate: 8000,
          numberOfChannels: 1});
      }
    };
  };
}

/* Model self-check: the rules reject exactly what Chrome rejected. */
function testModel() {
  const stats = {calls: 0};
  const clock = {seconds: 10};
  const Context = browserAudio(stats)(clock);
  const context = new Context();
  const now = context.currentTime;
  const gain = context.createGain().gain;
  gain.setValueCurveAtTime(new Float32Array([0, 1, 0]), now, .13);
  assert.throws(() => gain.setValueAtTime(0, now + .128), /overlaps setValueCurveAtTime/);
  assert.throws(() => gain.setTargetAtTime(0, now + .05, .01), /overlaps/);
  assert.throws(() => gain.setValueCurveAtTime([0, 1], now + .1, .1), /overlaps/);
  assert.throws(() => { gain.value = 0; }, /Failed to set the 'value' property/);
  gain.setValueAtTime(0, now + .13);
  gain.setValueCurveAtTime([0, 1], now + .13, .1);
  gain.cancelScheduledValues(now + .05);
  gain.setValueAtTime(0, now + .06);
  const pitch = context.createOscillator().frequency;
  pitch.setValueAtTime(100, now + .2);
  assert.throws(() => pitch.setValueCurveAtTime([1, 2], now + .1, .2), /overlaps set/);
  pitch.setValueCurveAtTime([1, 2], now + .2, .2);
}

/* Every cue, explosions with noise, a low-armor heartbeat, music cycled
   through Off / Menus / Full and modes switched, on jittered frames. */
async function session(options) {
  const stats = {calls: 0};
  const game = h.createGame({...options, audio: browserAudio(stats)});
  const {state, preferences, player} = game;
  state.mode = "playing";
  await h.startAudio(game);
  const foe = game.tanks[1];
  foe.x = 5;
  const cues = ["hit", "kill", "damage", "ricochet", "heartbeat", "waveStart",
    "waveClear", "telegraph", "pickup", "gadget", "command"];
  const modes = ["playing", "playing", "paused", "playing", "killcam", "playing",
    "arena-clear", "playing", "title", "playing", "victory", "playing"];
  let seed = 0x2545f491;
  const random = () => {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return (seed >>> 0) / 2 ** 32;
  };
  for (let at = 0; at < 9000; at++) {
    const dt = (at & 1 ? 1 / 60 : 1 / 30) * (.75 + random() * .5);
    if (at % 600 === 0) state.mode = modes[(at / 600) % modes.length];
    if (at % 1500 === 0) preferences.music = [2, 0, 1, 2, 0, 2][(at / 1500) % 6];
    if (at % 400 === 0) h.effect(game, "setMusic", !!preferences.music);
    preferences.effects = at % 2200 < 2000;
    player.health = at % 3000 < 900 ? 20 : 100;
    foe.cooldown = at % 700 < 300 ? 1 : 0;
    const roll = random();
    if (roll < .08) h.effect(game, "shot", roll < .04);
    else if (roll < .12) h.effect(game, cues[(roll * 1000 | 0) % cues.length]);
    else if (roll < .135) h.effect(game, "explosion", [.08, .13, .055, .14, .1][at % 5]);
    else if (roll < .14) h.effect(game, "ricochet");
    else if (roll < .145) h.effect(game, "play", 321, .063, .077, 122);
    h.frame(game, dt);
  }
  return {game, stats};
}

/* DOMException messages are read-only: wrap rather than annotate. */
function context(name, error) {
  return new Error(`${name}: ${error.name}: ${error.message}\n${error.stack}`,
    {cause: error});
}

/* music.js counts an automation call the context refused and sits that bar
   out, so a browser rejection there is silence, not an exception. */
function refusedNone(game, name) {
  if (!game.bank.music) return;
  assert.equal(game.bank.music.snapshot().stats.refused, 0,
    `${name}: no music bar refused by the browser model`);
}

(async () => {
  testModel();
  let scenarios = 0, calls = 0;
  const stats = {calls: 0};
  S.defaults.audio = browserAudio(stats);
  try {
    for (const [name, scenario] of Object.entries(S.SCENARIOS)) {
      let game;
      try { game = await scenario(); }
      catch (error) { throw context(name, error); }
      refusedNone(game, name);
      scenarios++;
    }
  } finally { S.defaults.audio = undefined; }
  calls += stats.calls;

  for (const music of [true, false]) {
    const result = await session({music}).catch((error) => {
      throw context(`session music=${music}`, error);
    });
    calls += result.stats.calls;
    if (music) {
      const snapshot = result.game.bank.music.snapshot();
      assert(snapshot.stats.segments > 50, "music scheduled bars throughout");
      assert(snapshot.stats.steals > 0, "effects took the lead voice from music");
      refusedNone(result.game, "session");
    }
  }

  // Both envelope modes: prebuilt gain curves, and target envelopes.
  for (const curveMode of [0, 1]) {
    const stats = {calls: 0};
    const game = h.createGame({audio: browserAudio(stats)});
    game.state.mode = "playing";
    await h.startAudio(game);
    game.bank.curveMode = curveMode;
    for (let at = 0; at < 1800; at++) {
      if (at % 2 === 0) h.effect(game, "shot", at % 4 === 0);
      if (at % 7 === 0) h.effect(game, "explosion");
      if (at % 5 === 0) h.effect(game, ["kill", "waveClear", "hit", "gadget"][at % 4]);
      h.frame(game, (at % 3 + 1) / 90);
    }
    calls += stats.calls;
  }

  /* Negative control: the old expiry wrote gain.value = 0 whatever drove the
     envelope. On the browser model that must throw from tickVoice. */
  const gamePath = path.join(h.ROOT, "examples/treadline-arena/game.js");
  const shipped = fs.readFileSync(gamePath, "utf8");
  const fixed = "if (voice.scheduledEnvelope !== 2) voice.gainParameter.value = 0;";
  assert.equal(shipped.split(fixed).length, 2, "expiry leaves value curves alone");
  const old = shipped.replace(fixed, "voice.gainParameter.value = 0;");
  await assert.rejects(session({music: true, gameText: old}),
    (error) => error.name === "NotSupportedError"
      && /value' property on 'AudioParam'.*overlaps setValueCurveAtTime/.test(error.message)
      && /tickVoice/.test(error.stack),
    "the pre-fix expiry is rejected by the browser model");

  console.log(`Treadline browser automation: ${scenarios} scenarios + sessions, `
    + `${calls} AudioParam automation calls accepted; pre-fix expiry rejected`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
