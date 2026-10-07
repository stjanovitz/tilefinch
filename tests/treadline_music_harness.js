"use strict";

/* Shared host harness for Treadline's adaptive music. It runs the reference
   bootstrap Web Audio subset (src/bootstrap/game-audio.js; the PSP ships the
   native audio slots instead), the shipped
   SoundBank class from game.js and the shipped music.js in one vm realm.
   The native command bridge is a recorder that applies game_audio.c's
   admission rules (voices, curve/timeline ownership, horizon, sample
   ranges) and logs every command with its time, so the host renderer
   (tests/game_audio_render.c) can replay the same commands through the real
   mixer and confirm each recorded result. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ROOT = path.join(__dirname, "..");
const read = (relative) => fs.readFileSync(path.join(ROOT, relative), "utf8");
const RATE = 44100, VOICES = 4, POINTS = 64, HORIZON_FRAMES = 441000;
/* Command opcodes, by the names game-audio.js and js_game_audio_command
   share (TilefinchGameAudioCommand). */
const COMMAND = Object.fromEntries(
  [...read("include/tilefinch/game_audio.h").matchAll(
    /TILEFINCH_GAME_AUDIO_COMMAND_(\w+) = (\d+)/g)]
    .map(([, name, value]) => [name, Number(value)]));

function gameSource(file = "examples/treadline-arena/game.js", text) {
  const source = text ?? read(file);
  const begin = source.indexOf("  class SoundBank {");
  const end = source.indexOf("  const sounds = new SoundBank();", begin);
  assert(begin >= 0 && end > begin, "SoundBank located in game.js");
  const melody = source.match(/  const MENU_MELODY = \[[^\n]+\];/);
  return (melody ? melody[0] + "\n" : "") + source.slice(begin, end);
}

const isFloat32 = (value) =>
  Object.prototype.toString.call(value) === "[object Float32Array]";

/* game_audio.c admission, reduced to the observable accept/refuse result. */
class NativeModel {
  constructor(clock) {
    this.clock = clock;
    this.log = [];
    this.slots = Array.from({length: VOICES}, () => ({active: 0, generation: 0,
      kind: 0, gainCurve: 0, pitchCurve: 0, envelopes: 0}));
    this.buffers = 0;
    this.peakVoices = 0;
    this.owner = "";
    this.refusals = [];
  }
  voice(handle) {
    const slot = handle & 15, generation = handle >>> 4;
    if (!slot || slot > VOICES) return null;
    const voice = this.slots[slot - 1];
    return generation && voice.generation === generation
      && voice.active !== 0 && voice.active !== 5 ? voice : null;
  }
  claim(kind) {
    for (let at = 0; at < VOICES; at++) {
      const voice = this.slots[at];
      if (voice.active) continue;
      voice.active = 1; voice.generation = (voice.generation + 1) >>> 0 || 1;
      voice.kind = kind; voice.gainCurve = voice.pitchCurve = voice.envelopes = 0;
      const busy = this.slots.filter((entry) => entry.active).length;
      this.peakVoices = Math.max(this.peakVoices, busy);
      return (voice.generation << 4) | (at + 1);
    }
    return 0;
  }
  static gain(value) { return Number.isFinite(value) && value >= 0 && value <= 4; }
  static delay(seconds) {
    return Number.isFinite(seconds) && seconds >= 0 && seconds <= 10
      ? Math.floor(seconds * RATE + .5) : -1;
  }
  static frequency(value) {
    return Number.isFinite(value) && value >= 1 && value <= 20000;
  }
  apply(command, args) {
    switch (command) {
    case COMMAND.RESUME: case COMMAND.SUSPEND: case COMMAND.CLOSE: return true;
    case COMMAND.START_BUFFER: {
      const [, , , rate, left, right, , , , delay] = args;
      if (!NativeModel.gain(left) || !NativeModel.gain(right)
          || NativeModel.delay(delay) < 0 || !(rate > 0)) return 0;
      return this.claim(2);
    }
    case COMMAND.STOP: return true;
    case COMMAND.START_OSCILLATOR: {
      const [type, frequency, left, right, delay] = args;
      if (type < 1 || type > 4 || !NativeModel.frequency(frequency)
          || !NativeModel.gain(left) || !NativeModel.gain(right)
          || NativeModel.delay(delay) < 0) return 0;
      return this.claim(1);
    }
    case COMMAND.SET_GAIN: {
      const [handle, left, right] = args;
      return !!this.voice(handle) && NativeModel.gain(left) && NativeModel.gain(right);
    }
    case COMMAND.SET_FREQUENCY: {
      const voice = this.voice(args[0]);
      return !!voice && voice.kind === 1 && NativeModel.frequency(args[1]);
    }
    case COMMAND.GAIN_TARGET: {
      const [handle, left, right, delay, constant] = args;
      const voice = this.voice(handle);
      if (!voice || !NativeModel.gain(left) || !NativeModel.gain(right)
          || NativeModel.delay(delay) < 0 || !(constant > 0) || constant > 10)
        return false;
      if (voice.gainCurve || voice.envelopes >= 2) return false;
      voice.envelopes++;
      return true;
    }
    case COMMAND.CANCEL_GAIN: {
      const voice = this.voice(args[0]);
      if (!voice) return false;
      voice.gainCurve = 0; voice.envelopes = 0;
      return true;
    }
    case COMMAND.CANCEL_PITCH: {
      const voice = this.voice(args[0]);
      if (!voice || voice.kind !== 1) return false;
      voice.pitchCurve = 0;
      return true;
    }
    case COMMAND.CURVE: {
      const [handle, pitch, values, left, right, delay, duration] = args;
      const voice = this.voice(handle);
      const delayFrames = NativeModel.delay(delay);
      const durationFrames = NativeModel.delay(duration);
      if (!voice || !(values instanceof Float32Array) || values.length < 2
          || values.length > POINTS || (pitch && voice.kind !== 1)
          || !NativeModel.gain(left) || !NativeModel.gain(right)
          || delayFrames < 0 || durationFrames <= 0
          || durationFrames > HORIZON_FRAMES - delayFrames) return false;
      if (pitch ? voice.pitchCurve : voice.gainCurve) return false;
      if (!pitch && voice.envelopes) return false;
      for (const value of values)
        if (pitch ? !NativeModel.frequency(value) : !NativeModel.gain(value)) return false;
      if (pitch) voice.pitchCurve = values.length; else voice.gainCurve = values.length;
      return true;
    }
    default: return false;
    }
  }
  command(command, ...args) {
    // Curves arrive as realm Float32Arrays; keep a copy as native does.
    const copy = args.map((value) => isFloat32(value) ? Float32Array.from(value) : value);
    const result = this.apply(command, copy);
    this.log.push({t: this.clock.seconds, command, args: copy, result,
      owner: this.owner});
    if (!result && command >= COMMAND.START_BUFFER) this.refusals.push(this.log[this.log.length - 1]);
    return result;
  }
  decode(buffer) {
    const bytes = new Uint8Array(buffer);
    const view = new DataView(buffer);
    const channels = view.getUint16(22, true), rate = view.getUint32(24, true);
    const bits = view.getUint16(34, true), data = view.getUint32(40, true);
    const handle = (1 << 4) | ++this.buffers;
    this.log.push({t: this.clock.seconds, command: "decode",
      args: [Buffer.from(bytes).toString("hex")], result: handle, owner: ""});
    return {handle, length: data / (bits / 8) / channels, sampleRate: rate,
      numberOfChannels: channels};
  }
}

/* Commands issued by the conductor itself are logged with owner "music". */
function tagMusic(conductor, native) {
  for (const name of ["schedule", "clearVoice", "acquireBass", "releaseBass",
    "releaseLead"]) {
    const method = conductor[name];
    conductor[name] = function (...args) {
      const owner = native.owner;
      native.owner = "music";
      try { return method.apply(this, args); } finally { native.owner = owner; }
    };
  }
}

/* One realm: game-audio.js + SoundBank + (optionally) music.js. audio, when
   given, is (clock) => AudioContext class and replaces the bootstrap subset,
   so the same game code runs against another Web Audio model. gameText
   replaces the whole game.js source (negative controls). */
function createGame({music = true, gameFile, gameText, musicText, audio} = {}) {
  const clock = {seconds: 0};
  const native = new NativeModel(clock);
  const state = {wallTime: 0, mode: "title", arena: 0, gameMode: 0, kills: 0,
    damageIndicator: 0, hitConfirm: 0};
  const preferences = {music: 2, musicVolume: 1, effects: true};
  const makeTank = (id) => ({id, active: id < 2, x: id ? 9 : 0, z: 0, team: id ? 1 : 0,
    health: 100, maxHealth: 100, cooldown: 0, boss: false, inert: false, boost: 0,
    command: {left: 0, right: 0}});
  const tanks = Array.from({length: 6}, (_, id) => makeTank(id));
  const player = tanks[0];
  const campaign = {runtime: {on: false, m: null}};
  const practice = {on: false};
  const realm = vm.createContext({
    performance: {now: () => clock.seconds * 1000},
    EventTarget, Event, DOMException, console,
    __tilefinchGameAudioCommand: (command, ...args) => native.command(command, ...args),
    __tilefinchGameAudioDecode: (buffer) => native.decode(buffer),
    __tilefinchRunTask: (name, handler, self, args) => handler.apply(self, args),
    __tilefinchReportUncaught: (error) => { throw error; },
    state, preferences, playerTank: () => player, qualificationAIActive: false,
    // music.js's test tables and build helpers (the harness flag).
    __treadlineEnableDebug: true,
  });
  if (audio) realm.AudioContext = audio(clock);
  else vm.runInContext(read("src/bootstrap/game-audio.js"), realm,
    {filename: "game-audio.js"});
  vm.runInContext(gameSource(gameFile, gameText) + "\nthis.SoundBank = SoundBank;", realm,
    {filename: "game.js#SoundBank"});
  if (music)
    vm.runInContext(musicText || read("examples/treadline-arena/music.js"), realm,
      {filename: "music.js"});
  const bank = vm.runInContext("new SoundBank()", realm);
  if (music) {
    bank.music = vm.runInContext("__treadlineMusic", realm).create(bank,
      {state, tanks, preferences, playerTank: () => player, campaign, practice});
    tagMusic(bank.music, native);
  }
  return {realm, clock, native, state, preferences, tanks, player, bank,
    campaign, practice, api: music ? vm.runInContext("__treadlineMusic", realm) : null};
}

async function startAudio(game) {
  game.bank.start();
  for (let at = 0; at < 12; at++) await Promise.resolve();
  assert.equal(game.bank.voices.length, 2, "SoundBank graph started");
  assert(game.bank.engine && game.bank.noise, "engine and noise voices started");
}

/* Advance one game frame: clock, wall time, decaying indicators, tick. */
function frame(game, dt = 1 / 30) {
  game.clock.seconds += dt;
  game.state.wallTime += dt;
  game.state.damageIndicator = Math.max(0, game.state.damageIndicator - dt);
  game.state.hitConfirm = Math.max(0, game.state.hitConfirm - dt);
  for (let at = 0; at < game.tanks.length; at++) {
    const tank = game.tanks[at];
    tank.cooldown = Math.max(0, tank.cooldown - dt);
  }
  game.native.owner = "tick";
  game.bank.tick(game.state.wallTime, game.player, game.state.mode);
  game.native.owner = "";
}

function effect(game, name, ...args) {
  game.native.owner = "sfx:" + name;
  game.bank[name](...args);
  game.native.owner = "";
}

/* The recorded command log for tests/game_audio_render.c. */
function writeLog(game, file) {
  const lines = ["# treadline-music-log v1"];
  for (const entry of game.native.log) {
    const t = entry.t.toFixed(6);
    if (entry.command === "decode") {
      lines.push(`D ${t} ${entry.args[0]} = ${entry.result}`);
      continue;
    }
    const parts = [];
    for (const value of entry.args) {
      if (value instanceof Float32Array) {
        parts.push(String(value.length));
        for (const sample of value) parts.push(String(sample));
      } else parts.push(typeof value === "boolean" ? (value ? "1" : "0") : String(Number(value)));
    }
    const result = typeof entry.result === "boolean" ? (entry.result ? 1 : 0) : entry.result;
    lines.push(`C ${t} ${entry.command} ${entry.args.length} ${parts.join(" ")} = ${result}`);
  }
  lines.push(`E ${game.clock.seconds.toFixed(6)}`);
  fs.writeFileSync(file, lines.join("\n") + "\n");
}

module.exports = {NativeModel, createGame, tagMusic, startAudio, frame, effect, writeLog,
  gameSource, RATE, ROOT};
