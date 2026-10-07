"use strict";

// Test the shipped SoundBank, including asynchronous startup. Count native
// publications instead of treating host microseconds as PSP frame savings.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(process.argv[2] === "-" ? 0
  : process.argv[2] || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
const begin = source.indexOf("  class SoundBank {");
const end = source.indexOf("  const sounds = new SoundBank();", begin);
assert(begin >= 0 && end > begin);
const body = source.slice(begin, end);

function parameter(value) {
  return {writes: 0, get value() { return value; },
    set value(next) { this.writes++; value = next; }};
}
function node() {
  return {connect(target) { return target; }, start() {}};
}
class FakeContext {
  constructor() { this.destination = {}; this.currentTime = 0; }
  resume() { return Promise.resolve(); }
  createGain() { return {...node(), gain: parameter(1)}; }
  createOscillator() { return {...node(), frequency: parameter(440)}; }
  createBufferSource() { return node(); }
  decodeAudioData() { return Promise.resolve({}); }
}
function fixture(text = body, AudioContext = FakeContext) {
  const state = {wallTime: 0, mode: "title"};
  const preferences = {music: true, effects: true};
  const player = {active: true, boost: 0, command: {left: 0, right: 0}};
  const context = vm.createContext({AudioContext, state, preferences,
    playerTank: () => player, qualificationAIActive: false});
  vm.runInContext(text + "\nthis.bank = new SoundBank();", context);
  return {bank: context.bank, state, preferences, player};
}

async function check(text = body) {
  const {bank, state, preferences, player} = fixture(text);
  bank.start();
  for (let at = 0; at < 6; at++) await Promise.resolve();
  assert.equal(bank.voices.length, 2);
  assert(bank.engine && bank.noise, "startup completed rather than swallowing an error");
  const frequency = bank.engine.oscillator.frequency, gain = bank.engine.gain.gain;
  let expectedFrequency = 82, expectedGain = 0, expectedStep = 0;
  let frames = 0;
  function tick(time, mode, reset = false) {
    state.wallTime = time; state.mode = mode;
    // Menu music moved to music.js (its own conductor); without one the
    // engine voice is the hum in play and silent elsewhere, whatever the
    // music preference.
    const active = mode === "playing" && player.active;
    const step = (time * 10) | 0;
    if (reset) expectedStep = -1;
    if (step !== expectedStep) {
      expectedStep = step;
      if (active) {
        const moving = Math.min(1, (Math.abs(player.command.left)
          + Math.abs(player.command.right)) * .5);
        const boosted = player.boost > 0;
        expectedFrequency = 64 + moving * 34 + (boosted ? 25 : 0);
        expectedGain = preferences.effects ? .009 + moving * .012 + (boosted ? .006 : 0) : 0;
      } else expectedGain = 0;
    }
    if (reset) bank.setMusic(preferences.music);
    else bank.tick(time, player, mode);
    assert.equal(frequency.value, expectedFrequency);
    assert.equal(gain.value, expectedGain);
    assert.equal(bank.engineRole, active ? "hum" : "off");
    frames++;
  }
  tick(.2, "playing");
  const before = frequency.writes + gain.writes;
  for (let at = 1; at <= 300; at++) tick(.2 + at / 30, "playing");
  assert.equal(frequency.writes + gain.writes, before,
    "unchanged engine hum must not republish AudioParam values");

  let time = 11;
  for (const mode of ["title", "playing", "paused", "game-over", "victory", "generating"])
    for (const music of [false, true]) for (const effects of [false, true])
      for (const active of [false, true]) for (const boost of [0, 1]) {
        preferences.music = music; preferences.effects = effects;
        player.active = active; player.boost = boost;
        for (const direction of [-1, -.25, 0, .5, 1]) {
          player.command.left = direction; player.command.right = -direction * .5;
          tick(time, mode); tick(time + .001, mode);
          tick(time + .002, mode, true);
          time += .17;
        }
      }
  // Music reset updates immediately even within the existing ten-Hz step.
  preferences.music = true; tick(time, "paused", true);
  preferences.music = false; tick(time, "paused", true);
  assert.equal(gain.value, 0);
  return frames;
}

(async () => {
  const frames = await check();
  const mutant = body.replace("if (frequency !== this.engine.frequency)", "if (true)")
    .replace("if (level !== this.engine.level)", "if (true)");
  assert.notEqual(mutant, body, "negative control replaced the publication guards");
  await assert.rejects(check(mutant), /unchanged engine hum must not republish/);
  const silent = fixture(body, null);
  silent.bank.start();
  silent.bank.tick(1, silent.player, "playing");
  assert.equal(silent.bank.context, null);
  console.log(`Treadline engine voice: ${frames} equivalent states; redundant writes rejected`);
})().catch(error => { console.error(error); process.exitCode = 1; });
