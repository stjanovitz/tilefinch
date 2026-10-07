"use strict";

/* Treadline adaptive music: the shipped music.js, SoundBank (game.js) and
   bootstrap Web Audio subset, with native admission modelled by the
   harness. Run with --expose-gc --no-concurrent-recompilation (the second
   makes the allocation measurement independent of machine load; see
   testAllocation). Structural counts only; host timings are
   reported by tilefinch-treadline-music-render-tests, not asserted here. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const v8 = require("node:v8");
const h = require("./treadline_music_harness.js");
const S = require("./treadline_music_scenarios.js");

const ROOT = h.ROOT;
const read = (relative) => fs.readFileSync(path.join(ROOT, relative), "utf8");
const musicSource = read("examples/treadline-arena/music.js");
let checks = 0;
const ok = (value, message) => { assert(value, message); checks++; };

/* Record every bar the conductor prepares. */
function watchPlans(game) {
  const music = game.bank.music, plans = [];
  plans.logFrom = game.native.log.length;
  const prepare = music.prepare;
  music.prepare = function (start, time) {
    prepare.call(this, start, time);
    const plan = this.plan;
    plans.push({start: plan.start, end: plan.end, stepDur: plan.stepDur,
      kind: plan.kind, level: plan.level, stinger: plan.stinger,
      lead: plan.lead, bass: plan.bass, at: game.clock.seconds,
      committed: this.level});
  };
  return plans;
}

function curveCommands(game, owner = "music") {
  return game.native.log.filter((entry) => entry.command === 11
    && (owner === null || entry.owner === owner));
}

// ------------------------------------------------------- built curves --
function testBuiltCurves() {
  const game = h.createGame();
  const api = game.api;
  const segments = [];
  const collect = (value) => {
    if (!value || typeof value !== "object") return;
    if (value.gain && typeof value.n === "number") { segments.push(value); return; }
    for (const key of Object.keys(value)) collect(value[key]);
  };
  for (const id of S.PROGRAM_IDS) for (const volume of [.6, 1, 1.45])
    collect(api.buildProgram(id, volume));
  for (const volume of [.6, 1, 1.45]) collect(api.buildGlobal(volume));
  ok(segments.length > 300, "every program and stinger builds");
  for (const seg of segments) {
    ok(seg.n >= 2 && seg.n <= 64 && seg.gain.length === seg.n, "2-64 gain samples");
    ok(!seg.pitch || seg.pitch.length === seg.n, "pitch curve matches the gain grid");
    ok(seg.gain[0] === 0 && seg.gain[seg.n - 1] === 0, "a bar starts and ends silent");
    ok(seg.end <= 15, "every bar leaves at least one silent step to schedule the next");
    let peak = 0;
    for (let at = 0; at < seg.n; at++) {
      ok(Number.isFinite(seg.gain[at]) && seg.gain[at] >= 0 && seg.gain[at] <= .2,
        "gain samples are small and admitted");
      peak = Math.max(peak, seg.gain[at]);
      if (seg.pitch) {
        ok(seg.pitch[at] >= 60 && seg.pitch[at] <= 2100, "pitch stays musical and admitted");
        // Pitch may move only while the gain is silent at both ends.
        if (at > 0 && seg.pitch[at] !== seg.pitch[at - 1])
          ok(seg.gain[at] === 0 && seg.gain[at - 1] === 0, "pitch glides only in silence");
      } else ok(seg.hz >= 60 && seg.hz <= 2100, "single-pitch bar sets a base frequency");
    }
    ok(peak > 0, "a bar is audible");
  }
  // Pools are sized for the largest program at once.
  const conductor = api.create(game.bank, {state: game.state, preferences:
    game.preferences, tanks: game.tanks, playerTank: () => game.player,
    campaign: game.campaign, practice: game.practice});
  for (const id of S.PROGRAM_IDS) {
    conductor.programId = id; conductor.builtVolume = -1; conductor.build();
    ok(conductor.gamePool.used <= conductor.gamePool.data.length, "game pool fits " + id);
  }
  ok(conductor.globalPool.used <= conductor.globalPool.data.length, "global pool fits");
}

// --------------------------------------------- state machine, hysteresis --
async function testHysteresis() {
  const game = await S.begin({effects: false});
  const plans = watchPlans(game);
  const music = game.bank.music, foe = game.tanks[1];
  S.select(game, "foundry");
  game.state.mode = "playing";
  S.enemyAt(game, 12);
  S.run(game, 6);
  ok(music.level === 0 && music.desired === 0, "far foe: explore");
  // Hover around the alert entry radius: the latch must not flap.
  let flips = 0, last = music.alert;
  S.run(game, 20, (t) => {
    foe.x = game.player.x + 6.5 + Math.sin(t * 9) * .7;
    if (music.alert !== last) { flips++; last = music.alert; }
  });
  ok(flips <= 1, `alert latch held at the boundary (${flips} flips)`);
  ok(music.level === 1, "near foe: alert");
  // Retreat only to 8: still inside the exit radius.
  foe.x = game.player.x + 8;
  S.run(game, 8);
  ok(music.level === 1, "alert holds inside the exit radius");
  // Combat: shots near the player escalate at the next bar.
  const before = plans.length;
  foe.x = game.player.x + 5; foe.cooldown = 1;
  let desiredAt = -1;
  S.run(game, 6, (t) => {
    foe.cooldown = 1;
    if (desiredAt < 0 && music.desired === 2) desiredAt = game.clock.seconds;
  });
  ok(desiredAt > 0, "combat desired within one evaluation");
  const escalation = plans.slice(before).find((plan) => plan.level === 2);
  ok(escalation && escalation.start >= desiredAt
     && escalation.start - desiredAt <= escalation.end - escalation.start + 1,
     "combat begins on the next bar line");
  // Calm down: needs COMBAT_HOLD + CALM_HOLD and MIN_BARS bars.
  foe.x = game.player.x + 12; foe.cooldown = 0;
  const calmFrom = game.clock.seconds;
  let calmedAt = -1;
  S.run(game, 20, () => {
    if (calmedAt < 0 && music.level < 2) calmedAt = game.clock.seconds;
  });
  const {COMBAT_HOLD, CALM_HOLD} = game.api.constants;
  ok(calmedAt - calmFrom >= COMBAT_HOLD + CALM_HOLD - .5,
    `calming waited ${(calmedAt - calmFrom).toFixed(2)} s`);
  ok(music.level === 0, "explore again once the foe left");
  // Danger latch: enters below .3, holds to .42.
  foe.x = game.player.x + 5;
  game.player.health = 29;
  S.run(game, 5);
  ok(music.level === 3, "low armor: danger");
  game.player.health = 38;
  S.run(game, 10);
  ok(music.level === 3 && music.danger, "danger holds below the exit threshold");
  game.player.health = 60;
  S.run(game, 12);
  ok(music.level < 3, "danger released after repair");
  // Every level change happened on a prepared bar; bars are contiguous.
  for (let at = 1; at < plans.length; at++) {
    const gap = plans[at].start - plans[at - 1].end;
    ok(Math.abs(gap) < 1e-9 || gap > game.api.constants.PREPARE_LEAD,
      "bars follow each other without overlap");
  }
  return game;
}

/* Every music curve starts exactly on its prepared bar's step grid. */
function checkQuantized(game, plans) {
  const curves = curveCommands(game).filter((entry) =>
    game.native.log.indexOf(entry) >= plans.logFrom);
  ok(curves.length > 20, "music scheduled curves");
  for (const entry of curves) {
    const start = entry.t + entry.args[5];
    const plan = plans.find((p) => start >= p.start - 1e-6 && start < p.end);
    ok(plan, "curve belongs to a bar");
    const steps = (start - plan.start) / plan.stepDur;
    ok(Math.abs(steps - Math.round(steps)) < 1e-6, "curve starts on a sixteenth");
    ok(entry.args[5] <= game.api.constants.PREPARE_LEAD + 1e-9,
      "scheduled no more than PREPARE_LEAD ahead");
    ok(entry.args[5] + entry.args[6] < 5, "well inside the ten-second horizon");
    ok(entry.args[2].length >= 2 && entry.args[2].length <= 64, "2-64 samples");
  }
}

// -------------------------------------------------- scenarios end to end --
async function testScenarios() {
  for (const name of Object.keys(S.SCENARIOS)) {
    const game = await S.SCENARIOS[name]();
    ok(game.native.refusals.length === 0,
      `${name}: no refused native command ${JSON.stringify(game.native.refusals[0]
        && [game.native.refusals[0].command, game.native.refusals[0].owner])}`);
    ok(game.native.peakVoices <= 4, `${name}: four voices at most`);
    const starts = game.native.log.filter((entry) => entry.command === 3
      || entry.command === 5);
    ok(starts.length === 4, `${name}: music starts no voice of its own`);
    if (game.bank.music) {
      const stats = game.bank.music.snapshot().stats;
      ok(stats.refused === 0, `${name}: no music command refused`);
      ok(stats.maxPoints <= 64, `${name}: 64-sample limit`);
      // PREPARE_LEAD plus the slowest bar (54 bpm): far inside ten seconds.
      ok(game.bank.music.maxHorizon < 5, `${name}: horizon ${game.bank.music.maxHorizon}`);
    }
  }
  // Quantization and horizon on a long, state-driven run.
  const game = await S.begin();
  const plans = watchPlans(game);
  const run = S.SCENARIOS["match-60s"];
  void run;
  S.select(game, "city");
  game.state.mode = "playing";
  S.enemyAt(game, 5);
  S.run(game, 40, (t) => {
    game.tanks[1].cooldown = t % 9 < 5 ? 1 : 0;
    game.tanks[1].x = game.player.x + (t % 20 < 10 ? 5 : 12);
  });
  checkQuantized(game, plans);
}

// --------------------------------------------------- effects keep priority --
async function testEffectPriority() {
  const game = await S.begin();
  const {bank, native, state} = game;
  const lead = bank.voices[1], leadHandle = lead.oscillator._voice;
  let checked = 0;
  const command = native.command.bind(native);
  native.command = (id, ...args) => {
    if (native.owner === "music" && args[0] === leadHandle && id >= 6 && id <= 11) {
      ok(!(lead.stopAt > state.wallTime), "music never touches a sounding effect");
      checked++;
    }
    return command(id, ...args);
  };
  S.select(game, "yard");
  state.mode = "playing";
  S.enemyAt(game, 5);
  let effects = 0;
  S.run(game, 45, (t) => {
    game.tanks[1].cooldown = 1;
    const frameAt = Math.round(t * 30);
    if (frameAt % 7 === 0) { h.effect(game, "shot", true); effects++; }
    if (frameAt % 11 === 0) { h.effect(game, "explosion"); effects++; }
    if (frameAt % 13 === 0) { h.effect(game, "hit"); effects++; }
  });
  ok(checked > 10, "lead-voice music commands were observed");
  const sfx = native.log.filter((entry) => entry.owner.startsWith("sfx:"));
  ok(sfx.length > effects, "effects issued commands");
  ok(sfx.every((entry) => entry.result), "every effect command was admitted");
  ok(bank.music.snapshot().stats.steals > 3, "effects took the lead voice when needed");
  // With the first oscillator free, an effect never takes the lead voice.
  const v0 = bank.voices[0];
  S.run(game, 1);
  ok(!(v0.stopAt > state.wallTime), "first effect voice idle");
  const steals = bank.music.snapshot().stats.steals;
  h.effect(game, "shot", true);
  ok(bank.music.snapshot().stats.steals === steals && v0.stopAt > state.wallTime,
    "a lone effect uses the first oscillator");
  // Heartbeat: the danger bass replaces the effect; without music it plays.
  game.player.health = 20;
  S.run(game, 8);
  ok(bank.music.level === 3 && bank.music.bassOwned, "danger with bass");
  let count = native.log.length;
  h.effect(game, "heartbeat");
  ok(native.log.length === count, "danger bass covers the heartbeat effect");
  game.preferences.music = 0;
  S.run(game, .2);
  count = native.log.length;
  h.effect(game, "heartbeat");
  ok(native.log.length > count, "heartbeat effect plays with music off");
  ok(bank.engineRole === "hum", "engine hum returns when music is off");
}

// ---------------------------------------------------- music off == legacy --
async function testOffMatchesLegacy() {
  const drive = async (music) => {
    const game = h.createGame({music});
    game.preferences.music = 0;
    await h.startAudio(game);
    game.state.mode = "title";
    S.run(game, 3);
    game.state.mode = "playing";
    S.enemyAt(game, 5);
    S.run(game, 20, (t) => {
      const at = Math.round(t * 30);
      game.player.command.left = (at % 90) / 90;
      if (at % 9 === 0) h.effect(game, "shot", at % 18 === 0);
      if (at % 31 === 0) h.effect(game, "explosion");
      if (at % 37 === 0) h.effect(game, "heartbeat");
    });
    game.state.mode = "paused";
    S.run(game, 3);
    return game.native.log.map((entry) => [entry.t, entry.command,
      entry.args.map((value) => value instanceof Float32Array ? Array.from(value) : value),
      entry.result]);
  };
  const withMusic = await drive(true), legacy = await drive(false);
  assert.deepEqual(withMusic, legacy,
    "music Off issues exactly the legacy SoundBank commands (league, sweep, Off)");
  checks++;
}

// ------------------------------------------------ menu-only and toggling --
async function testMenuOnlyAndToggle() {
  const game = await S.begin({music: 1});
  const {bank, native, state} = game;
  state.mode = "title";
  S.run(game, 6);
  ok(curveCommands(game).length > 0 && bank.engineRole === "music", "menus play the theme");
  state.mode = "playing";
  S.run(game, 3);
  const mark = native.log.length;
  S.enemyAt(game, 5);
  S.run(game, 15, () => { game.tanks[1].cooldown = 1; });
  ok(native.log.slice(mark).every((entry) => entry.owner !== "music"),
    "no in-game music in Menus mode (hum only)");
  ok(bank.engineRole === "hum", "engine hum in game with Menus");
  state.mode = "arena-clear";
  S.run(game, 3);
  ok(bank.music.queued === 0, "no clear stinger in Menus mode");
  // Toggle Full mid-game, then Off, then Full again: clean hand-overs.
  state.mode = "playing";
  game.preferences.music = 2;
  S.run(game, 8, () => { game.tanks[1].cooldown = 1; });
  ok(bank.music.kind === 2 && bank.music.level >= 1, "Full mid-game starts music");
  game.preferences.music = 0;
  S.run(game, 2);
  ok(!bank.music.leadOwned && !bank.music.bassOwned, "Off releases both voices");
  game.preferences.music = 2;
  S.run(game, 8, () => { game.tanks[1].cooldown = 1; });
  ok(native.refusals.length === 0, "toggling never leaves a refused command");
  // Volume: rebuilds once and scales the curves.
  const builds = bank.music.stats[11];
  const peak = () => Math.max(...curveCommands(game).slice(-6)
    .filter((entry) => !entry.args[1]).map((entry) => Math.max(...entry.args[2])));
  const before = peak();
  game.preferences.musicVolume = 2;
  S.run(game, 8, () => { game.tanks[1].cooldown = 1; });
  ok(bank.music.stats[11] === builds + 1, "volume change rebuilds once");
  ok(peak() > before * 1.2, "High is louder than Mid");
}

// ------------------------------------------------------ program selection --
async function testPrograms() {
  const game = await S.begin({effects: false});
  const music = game.bank.music;
  /* A mission plays its own program (campaign.js MISSIONS' music field;
     the feature fixture checks the real table), else its theater's. */
  const expect = [[{id: "1-1"}, "yard"], [{id: "2-3"}, "foundry"], [{id: "3-4"}, "glacier"],
    [{id: "4-2"}, "city"], [{id: "5-2"}, "fortress"], [{id: "1-5", music: "vesper"}, "vesper"],
    [{id: "5-3", music: "ally"}, "ally"], [{id: "5-4", music: "sunset"}, "sunset"]];
  for (const [m, program] of expect) {
    game.campaign.runtime = {on: true, m};
    ok(music.programFor() === program, `${m.id} -> ${program}`);
  }
  game.campaign.runtime = {on: false, m: null};
  game.practice.on = true;
  ok(music.programFor() === "practice", "practice range");
  game.practice.on = false;
  for (const [mode, arena, program] of [[0, 0, "yard"], [1, 1, "foundry"],
    [2, 2, "glacier"], [0, 3, "city"], [3, 0, "fortress"], [4, 1, "city"], [6, 0, "vesper"]]) {
    game.state.gameMode = mode; game.state.arena = arena;
    ok(music.programFor() === program, `mode ${mode} arena ${arena} -> ${program}`);
  }
  // Practice and the last match never leave alert.
  S.select(game, "practice");
  game.state.gameMode = 0;
  game.state.mode = "playing";
  S.enemyAt(game, 4);
  game.player.health = 10;
  S.run(game, 12, () => { game.tanks[1].cooldown = 1; });
  ok(music.level === 1, "practice range stays calm");
  game.tanks[1].active = false;
  S.run(game, 12);
  ok(music.level === 1, "practice range keeps its calm loop with no target near");
  // Stealth missions (Whiteout, Lights Out): a closing, silent foe is not announced.
  for (const id of ["3-4", "4-1"]) {
    const stealth = await S.begin({effects: false});
    stealth.campaign.runtime = {on: true, m: {id, stealth: true}};
    stealth.state.mode = "playing";
    S.enemyAt(stealth, 3);
    S.run(stealth, 12);
    ok(stealth.bank.music.level === 0, `${id}: a hidden foe does not raise the music`);
    S.run(stealth, 6, () => { stealth.tanks[1].cooldown = 1; });
    ok(stealth.bank.music.level === 2, `${id}: a foe that fires does`);
  }
}

// ------------------------------------------- a program change mid-run --
/* Survival's next arena changes the program inside the run. The new one is
   built a segment per tick into the second pool while the old one keeps
   playing; it used to be built whole in one tick (about 100 ms on a PSP). */
async function testSlicedProgramChange() {
  const game = await S.begin({effects: false});
  const music = game.bank.music;
  game.campaign.runtime = {on: false, m: null};
  game.state.gameMode = 0; game.state.arena = 0; game.state.mode = "playing";
  S.run(game, 3);
  ok(music.programId === "yard" && music.bars, "Survival arena 1 plays the yard");
  const builds = music.stats[11];
  game.state.mode = "arena-clear";
  S.run(game, 1.4);
  game.state.arena = 1; game.state.mode = "playing";
  let largest = 0, ticks = 0;
  while (music.programId === "yard" && ticks < 120) {
    const back = music.gamePools[music.front ^ 1], before = back.used;
    h.frame(game, 1 / 30); ticks++;
    if (music.programId === "yard") largest = Math.max(largest, back.used - before);
  }
  ok(music.programId === "foundry" && music.stats[11] === builds + 1,
    "the next arena's program takes over once built");
  ok(ticks > 30 && largest <= 2 * 64, `built a segment per tick (${ticks} ticks, `
    + `at most ${largest} floats a tick)`);
  ok(music.bars.lead[1][0].gain.buffer === music.gamePool.data.buffer,
    "the new program plays from the pool it was built in");
  S.run(game, 4, () => { game.tanks[1].cooldown = 1; });
  ok(game.native.refusals.length === 0, "no command refused across the change");
  // A new run (from the title) still builds at once.
  game.state.mode = "title"; S.run(game, .5);
  game.state.arena = 2; game.state.mode = "playing";
  h.frame(game, 1 / 30);
  ok(music.programId === "glacier" && !music.pending, "a new run builds its program at once");
}

// ------------------------------------------------- output-only determinism --
async function testOutputOnly() {
  const game = h.createGame();
  const deny = (label) => ({set() { throw new Error("music wrote " + label); },
    defineProperty() { throw new Error("music defined " + label); },
    deleteProperty() { throw new Error("music deleted " + label); }});
  const tanks = game.tanks.map((tank, at) => new Proxy(tank, deny("tank " + at)));
  const env = {state: new Proxy(game.state, deny("state")), tanks,
    preferences: new Proxy(game.preferences, deny("preferences")),
    playerTank: () => tanks[0], campaign: new Proxy(game.campaign, deny("campaign")),
    practice: new Proxy(game.practice, deny("practice"))};
  game.bank.music = game.api.create(game.bank, env);
  h.tagMusic(game.bank.music, game.native);
  const random = Math.random;
  vm.runInContext("Math.random = () => { throw new Error('music drew a random number'); }",
    game.realm);
  await h.startAudio(game);
  for (const mode of ["title", "playing", "paused", "playing", "killcam", "playing",
    "arena-clear", "playing", "victory", "title", "playing", "game-over", "title"]) {
    game.state.mode = mode;
    S.enemyAt(game, mode === "playing" ? 4 : 12);
    game.player.health = mode === "killcam" ? 10 : 70;
    game.tanks[1].cooldown = 1; game.state.kills++;
    S.run(game, 5, () => { if (Math.random !== random) game.tanks[1].cooldown = 1; });
  }
  ok(game.bank.music.snapshot().stats.segments > 20, "music ran under read-only state");
  // The game source never hands music anything writable beyond its bank.
  const game_js = read("examples/treadline-arena/game.js");
  ok(/sounds\.music = globalThis\.__treadlineMusic\.create\(sounds,\s*\{state, tanks, preferences, playerTank, campaign, practice\}\)/
    .test(game_js), "music receives read-only game views");
  ok(!/\brandom\(|Math\.random|localStorage|sessionStorage|[Rr]eplay[A-Z(]/.test(
    musicSource.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "")),
    "music.js draws no randomness and touches no save or replay");
}

// ------------------------------------------------ timing runs stay silent --
function testTimingGates() {
  const game_js = read("examples/treadline-arena/game.js");
  ok(game_js.includes(`if (!qualificationAutoStart || urlSwitch("music") === "on")
    sounds.music = `), "qualification runs keep the effects-only audio baseline");
  // The league lives in the harness tooling (qualification.js).
  ok(/beginLeague[\s\S]{0,1600}preferences\.effects = false; preferences\.music = 0;/
    .test(read("examples/treadline-arena/qualification.js")),
    "the bot league (and campaign sweep) turn music off");
  const html = read("examples/treadline-arena/index.html");
  ok(html.indexOf('<script defer src="music.js"></script>')
     < html.indexOf('<script defer src="game.js"></script>')
     && html.includes('<script defer src="music.js"></script>'), "music.js loads before game.js");
  /* That music.js is packaged and runs from bytecode offline is checked by
     tilefinch-offline-app-launch-tests itself, not by reading its text. */
}

// ---------------------------------------------------- per-frame allocation --
/* QuickJS keeps numbers immediate, so the conductor's per-frame and per-bar
   paths allocate nothing exactly when they contain no allocating syntax.
   Check that statically, then confirm dynamically under V8 (whose double
   boxing adds a small constant) against a negative control. */
function functionBody(source, name) {
  const at = source.indexOf(`\n    ${name}(`);
  assert(at >= 0, name);
  const open = source.indexOf("{", at);
  let depth = 0, end = open;
  for (; end < source.length; end++) {
    if (source[end] === "{") depth++;
    else if (source[end] === "}" && --depth === 0) break;
  }
  return source.slice(open, end + 1).replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\/\/[^\n]*/g, "");
}

/* Array/object literals, closures, templates, spreads, `new`, string
   concatenation and allocating array/string helpers. Indexing (a[b]) and
   blocks after `)`/`else`/`try` are not literals. */
const ALLOCATING = [/\bnew\s/, /=>/, /`/, /\.\.\./, /(^|[=(,:?]|\breturn)\s*\[/,
  /(=|\(|,|\breturn|:)\s*\{/, /\.(map|filter|slice|concat|join|split|bind)\(/,
  /\b(Array|Object)\.\w+\(/, /"\s*\+|\+\s*"/, /\bfunction\b/];
const allocatingSyntax = (body) => {
  for (const pattern of ALLOCATING) {
    const found = body.match(pattern);
    if (found) return found[0];
  }
  return null;
};

function testStaticAllocation() {
  for (const snippet of ["{ const a = [time]; }", "{ this.x = {time}; }",
    "{ return new Error(); }", "{ list.map(f); }", "{ f(() => 1); }",
    "{ const s = `x`; }", "{ g(...args); }", "{ t = \"a\" + b; }",
    "{ return [a, b]; }", "{ f(a, {b}); }"])
    ok(allocatingSyntax(snippet), `negative control: ${snippet}`);
  for (const snippet of ["{ a[b] = c[d]; if (x) { y(); } else { z(); } }",
    "{ try { f(); } catch (_) { g(); } }"])
    ok(!allocatingSyntax(snippet), `control without allocation: ${snippet}`);
  for (const name of ["tick", "claimsEngine", "menuMode", "evaluate", "commitLevel",
    "prepare", "service", "voiceFree", "schedule", "sfxVoice", "heartbeat",
    "clearVoice", "acquireBass", "releaseBass", "releaseLead", "enqueue"]) {
    const found = allocatingSyntax(functionBody(musicSource, name));
    ok(!found, `${name} has no allocating syntax (${found})`);
  }
}

async function allocationPerFrame(musicText, mutate) {
  const game = h.createGame({musicText});
  await h.startAudio(game);
  S.select(game, "city");
  game.state.mode = "playing";
  S.enemyAt(game, 5);
  const foe = game.tanks[1], context = game.bank.context;
  // A plain clock field instead of the bootstrap getter (V8 boxes its result).
  Object.defineProperty(context, "currentTime", {value: 0, writable: true});
  if (mutate) mutate(game);
  // Admission without logging: no per-command arrays from the recorder.
  game.native.command = (id) => id !== 5 && id !== 3;
  const step = (frames) => {
    for (let at = 0; at < frames; at++) {
      foe.cooldown = (at % 600) < 300 ? 1 : 0;
      h.frame(game, 1 / 30);
      context.currentTime = game.clock.seconds;
    }
  };
  step(12000);
  const space = () => v8.getHeapSpaceStatistics()
    .find((entry) => entry.space_name === "new_space").space_used_size;
  let best = Infinity;
  for (let attempt = 0; attempt < 6; attempt++) {
    global.gc();
    const before = space();
    step(3000);
    const after = space();
    if (after >= before) best = Math.min(best, (after - before) / 3000);
  }
  return best;
}

async function testAllocation() {
  testStaticAllocation();
  if (typeof global.gc !== "function") throw new Error("run with --expose-gc");
  if (!process.execArgv.includes("--no-concurrent-recompilation"))
    throw new Error("run with --no-concurrent-recompilation");
  const idle = await allocationPerFrame(undefined, (game) => {
    game.bank.music.tick = () => false;
  });
  const full = await allocationPerFrame();
  // A modest per-frame buffer, as a careless curve rebuild would make.
  const mutant = musicSource.replace("const preference = this.musicMode;",
    "const preference = this.musicMode; this.scratch = new Float32Array(64);");
  assert.notEqual(mutant, musicSource);
  const control = await allocationPerFrame(mutant);
  console.log(`V8 young-generation bytes per frame: SoundBank without music ` +
    `${idle.toFixed(1)}, with music ${full.toFixed(1)}, negative control ${control.toFixed(1)}`);
  /* The static check above is exact. V8's own figure includes double
     boxing until the conductor is optimized. With concurrent recompilation
     (V8's default) optimized code arrives late on a loaded machine, and the
     difference swung from 25 B to over 150 B per frame under a parallel
     CTest; compiling synchronously makes it repeatable (25.1-25.5 B across
     twelve parallel runs, 2026-10). The bound sits well above that and well
     below the control: it rejects a per-frame buffer or a small per-frame
     object, with room for a V8 upgrade to box a little differently. */
  ok(control - idle > 150, "negative control (one 64-sample buffer per frame) is detected");
  ok(full - idle < 80, `music allocates no buffers per frame (${(full - idle).toFixed(1)} B)`);
}

(async () => {
  testBuiltCurves();
  await testHysteresis();
  await testScenarios();
  await testEffectPriority();
  await testOffMatchesLegacy();
  await testMenuOnlyAndToggle();
  await testPrograms();
  await testSlicedProgramChange();
  await testOutputOnly();
  testTimingGates();
  await testAllocation();
  console.log(`Treadline music: ${checks} checks`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
