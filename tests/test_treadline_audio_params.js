"use strict";

// Exercise the shipped SoundBank, not a second implementation, against fixed
// expected command logs (tests/fixtures/treadline-audio-params.json): every
// AudioParam command, clock read and voice state for seven envelopes per
// curve mode and capability, and digests of the longer later-play and
// restart logs. These are structural checks, not host-to-PSP timing
// estimates. After an intended change to SoundBank's commands, rewrite the
// expectations with --update and review the diff.
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const args = process.argv.slice(2);
const update = args.includes("--update");
const sourceArg = args.find(arg => arg !== "--update");
const source = fs.readFileSync(sourceArg === "-" ? 0
  : sourceArg || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
const begin = source.indexOf("  class SoundBank {");
const end = source.indexOf("  const sounds = new SoundBank();", begin);
assert(begin >= 0 && end > begin);
const body = source.slice(begin, end);
const expectedPath = path.join(__dirname, "fixtures/treadline-audio-params.json");
const expected = update ? {} : JSON.parse(fs.readFileSync(expectedPath, "utf8"));
const digest = (value) => crypto.createHash("sha256").update(JSON.stringify(value)).digest("hex");

async function fixture(text, curveMode, capability, failFrequencyGetter = false) {
  const events = [], parameters = [], getters = {gain: 0, frequency: 0};
  const state = {wallTime: 0, mode: "playing"};
  const preferences = {effects: true, music: false};
  const player = {active: true, boost: 0, command: {left: .5, right: .5}};
  let clock = 0;
  function parameter(owner, kind, value) {
    const id = parameters.length;
    function live() { assert(!owner.closed, "no writes to a closed graph"); }
    const result = {id, kind,
      get value() { return value; },
      set value(next) { live(); events.push(["value", id, next]); value = next; },
      cancelScheduledValues(when) { live(); events.push(["cancel", id, when]); return this; },
      setValueCurveAtTime(values, when, duration) {
        live(); events.push(["curve", id, Array.from(values), when, duration]); return this;
      },
      setValueAtTime(next, when) { live(); events.push(["at", id, next, when]); return this; },
      setTargetAtTime(next, when, duration) {
        live(); events.push(["target", id, next, when, duration]); return this;
      }};
    if (capability === "no-target") result.setTargetAtTime = undefined;
    if (capability === "no-curve") result.setValueCurveAtTime = undefined;
    parameters.push(result);
    return result;
  }
  function node() { return {connect(target) { return target; }, start() {}}; }
  class AudioContext {
    constructor() { this.destination = {}; this.closed = false; }
    get currentTime() { events.push(["clock", clock]); return clock++ / 1000; }
    resume() { return Promise.resolve(); }
    close() { this.closed = true; return Promise.resolve(); }
    createGain() {
      const gain = parameter(this, "gain", 1);
      return {...node(), get gain() { getters.gain++; return gain; }};
    }
    createOscillator() {
      const frequency = parameter(this, "frequency", 440);
      return {...node(), get frequency() {
        getters.frequency++;
        if (failFrequencyGetter) throw Error("AudioParam allocation refused");
        return frequency;
      }};
    }
    createBufferSource() { return node(); }
    decodeAudioData() { return Promise.resolve({}); }
  }
  const realm = vm.createContext({AudioContext, state, preferences,
    playerTank: () => player, qualificationAIActive: false,
    MENU_MELODY: [82, 98, 110, 98, 73, 82, 123, 98]});
  vm.runInContext(text + "\nthis.bank = new SoundBank();", realm);
  const result = {realm, bank: realm.bank, events, parameters, getters,
    state, preferences, player, curveMode};
  await start(result, !failFrequencyGetter);
  return result;
}

async function start(fixture, expectReady = true) {
  fixture.bank.start();
  for (let count = 0; count < 8; count++) await Promise.resolve();
  if (expectReady) {
    assert.equal(fixture.bank.voices.length, 2);
    assert(fixture.bank.engine && fixture.bank.noise, "asynchronous graph startup completed");
  }
  fixture.bank.curveMode = fixture.curveMode;
  fixture.events.length = 0;
  fixture.getters.gain = fixture.getters.frequency = 0;
}

function observe(fixture) {
  const bank = fixture.bank;
  // Normalize cross-realm arrays without dropping any command/clock arguments.
  return JSON.parse(JSON.stringify({events: fixture.events,
    voices: bank.voices.map(voice => [voice.startAt, voice.stopAt, voice.volume,
      voice.startFrequency, voice.endFrequency, voice.secondFrequency,
      voice.secondAt, voice.scheduledEnvelope, voice.lastFrequency]),
    noise: [bank.noise.startAt, bank.noise.stopAt, bank.noise.volume, bank.noise.scheduledEnvelope],
    engine: [bank.engine.frequency, bank.engine.level, bank.engineRole],
    parameters: fixture.parameters.map(parameter => parameter.value)}));
}

function sevenEnvelopes(fixture) {
  fixture.bank.play(245, .09, .14, 190);
  for (let count = 0; count < 3; count++) fixture.bank.ricochet();
}

function noHotGetters(fixture) {
  assert.deepEqual(fixture.getters, {gain: 0, frequency: 0},
    "cached AudioParams avoid native getter crossings after graph creation");
}

function laterBehavior(fixture) {
  const cues = ["hit", "kill", "damage", "ricochet", "heartbeat", "waveStart",
    "waveClear", "telegraph", "pickup", "gadget", "command"];
  for (let frame = 0; frame < 180; frame++) {
    fixture.state.wallTime = frame / 30;
    fixture.preferences.effects = frame % 17 !== 0;
    fixture.preferences.music = frame % 19 === 0;
    fixture.player.boost = frame % 23 === 0 ? 1 : 0;
    if (frame % 3 === 0) fixture.bank.shot(frame % 2 === 0);
    if (frame % 5 === 0) fixture.bank[cues[((frame / 5) | 0) % cues.length]]();
    if (frame % 7 === 0) fixture.bank.explosion([.08, .13, .055, .14, .1, .081][frame % 6]);
    if (frame % 11 === 0) fixture.bank.play(321, .063, .077, 122);
    fixture.bank.tick(fixture.state.wallTime, fixture.player, frame % 19 === 0 ? "paused" : "playing");
  }
  fixture.state.wallTime = 20;
  fixture.bank.tick(20, fixture.player, "game-over");
}

function references(bank) {
  return bank.voices.flatMap(voice => [voice.gainParameter, voice.frequencyParameter])
    .concat(bank.engine.gainParameter, bank.engine.frequencyParameter, bank.noise.gainParameter);
}

function expect(key, field, value) {
  if (update) {
    (expected[key] ||= {})[field] = value;
    return;
  }
  assert(expected[key] && field in expected[key], `expected ${key} ${field}`);
  assert.deepEqual(value, expected[key][field], `${key}: ${field} commands`);
}

(async () => {
  let cases = 0;
  for (let mode = 0; mode < 2; mode++) {
    for (const capability of ["complete", "no-target", "no-curve"]) {
      const key = `mode ${mode} ${capability}`;
      const current = await fixture(body, mode, capability);
      const context = current.bank.context;
      const params = references(current.bank);
      current.bank.start();
      for (let count = 0; count < 8; count++) await Promise.resolve();
      assert.equal(current.bank.context, context, "start is idempotent on the active graph");
      assert.deepEqual(references(current.bank), params);
      assert.equal(current.events.length, 0);
      noHotGetters(current);
      sevenEnvelopes(current);
      expect(key, "envelopes", observe(current));
      noHotGetters(current);
      if (mode === 1 && capability === "complete") {
        assert.equal(current.events.filter(event => event[0] === "curve").length, 7);
        assert.equal(current.events.filter(event => event[0] === "cancel").length, 7);
        assert.equal(current.events.filter(event => event[0] === "clock").length, 7);
      }
      laterBehavior(current);
      expect(key, "later", digest(observe(current)));
      noHotGetters(current);

      const oldReferences = references(current.bank);
      assert.equal(new Set(oldReferences).size, 7);
      await current.bank.context.close();
      // SoundBank has no reset method: a restarted page creates a fresh bank.
      vm.runInContext("this.bank = new SoundBank();", current.realm);
      current.bank = current.realm.bank;
      current.preferences.effects = true;
      await start(current);
      const fresh = references(current.bank);
      assert.equal(new Set(fresh).size, 7);
      assert(fresh.every(parameter => parameter && !oldReferences.includes(parameter)),
        "fresh graph gets fresh cached parameters");
      assert.throws(() => { oldReferences[0].value = .25; }, /closed graph/);
      sevenEnvelopes(current);
      expect(key, "restart", digest(observe(current)));
      noHotGetters(current);

      // Cache objects only: replacing their ordinary methods remains visible.
      let intercepted = 0;
      const param = current.bank.voices[0].gainParameter;
      const cancel = param.cancelScheduledValues;
      param.cancelScheduledValues = function(when) { intercepted++; return cancel.call(this, when); };
      current.bank.next = 0; current.bank.hit();
      assert.equal(intercepted, capability === "no-target" ? 0 : 1);
      cases++;
    }
  }
  // The two effects' frequency Params become live at graph creation. A
  // refused native getter must remain inside the existing startup catch path.
  const refused = await fixture(body, 1, "complete", true);
  assert.equal(refused.bank.voices.length, 0);
  assert.equal(refused.bank.engine, null);
  assert.equal(refused.bank.noise, null);
  assert.doesNotThrow(() => {
    sevenEnvelopes(refused);
    refused.bank.tick(1, refused.player, "playing");
  }, "failed startup leaves later game ticks harmless");
  if (update) {
    // One line per command, voice and digest, so a change reviews as a diff.
    const lines = (list) => list.map(item => "    " + JSON.stringify(item)).join(",\n");
    fs.writeFileSync(expectedPath, "{\n" + Object.entries(expected).map(([key, entry]) => {
      const envelopes = Object.entries(entry.envelopes).map(([field, list]) =>
        `   ${JSON.stringify(field)}: [\n${lines(list)}]`).join(",\n");
      return `${JSON.stringify(key)}: {\n  "envelopes": {\n${envelopes}},\n`
        + `  "later": ${JSON.stringify(entry.later)},\n  "restart": ${JSON.stringify(entry.restart)}}`;
    }).join(",\n") + "\n}\n");
    console.log(`Treadline AudioParams: wrote ${path.relative(process.cwd(), expectedPath)}`);
    return;
  }
  console.log(`Treadline AudioParams: ${cases} cases match the expected command logs; hot getters and stale refs rejected`);
})().catch(error => { console.error(error); process.exitCode = 1; });
