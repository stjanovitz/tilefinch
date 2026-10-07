"use strict";

/* Scripted listening scenarios for Treadline's adaptive music. Each drives
   the shipped SoundBank + music.js through the harness at 30 frames per
   second and leaves its recorded command log on the returned game. The
   "match" scenarios feed the real intensity state machine (foe distance,
   shots, damage, kills); the tours force a level per section so every bar a
   program can play is heard once. */
const h = require("./treadline_music_harness.js");

const MISSION = {yard: "1-1", foundry: "2-1", glacier: "3-1", city: "4-1",
  fortress: "5-2", vesper: "1-5", ally: "5-3", sunset: "5-4", last: "5-5"};
const FPS = 30;

function select(game, program) {
  game.practice.on = program === "practice";
  game.campaign.runtime = program === "practice" || !MISSION[program]
    ? {on: false, m: null} : {on: true, m: {id: MISSION[program], music: program}};
}

function run(game, seconds, each) {
  const frames = Math.round(seconds * FPS);
  for (let at = 0; at < frames; at++) {
    if (each) each(at / FPS);
    h.frame(game, 1 / FPS);
  }
}

/* Seconds until the conductor's next bar begins (to cut on bar lines). */
function toBar(game) {
  const music = game.bank.music;
  return Math.max(0, music.nextStart - game.clock.seconds);
}

/* The Web Audio model every scenario starts on: undefined is the bootstrap
   subset; a test may set another (see createGame's audio option). */
const defaults = {audio: undefined};

/* program/mode are set before audio starts, so the first bar is theirs. */
async function begin({music = 2, effects = true, volume = 1, program = "",
                      mode = "title", audio = defaults.audio} = {}) {
  const game = h.createGame({audio});
  game.preferences.music = music;
  game.preferences.effects = effects;
  game.preferences.musicVolume = volume;
  if (program) select(game, program);
  game.state.mode = mode;
  // Bar timeline for the preview README: what each bar played and when.
  game.timeline = [];
  const conductor = game.bank.music;
  if (conductor) {
    const prepare = conductor.prepare;
    conductor.prepare = function (start, time) {
      prepare.call(this, start, time);
      const plan = this.plan, levels = game.api.levels, names = game.api.stingers;
      game.timeline.push({start: plan.start, program: plan.kind === 1 ? "title" : this.programId,
        part: plan.stinger >= 0 ? "stinger:" + names[plan.stinger]
          : plan.kind === 1 ? "menu" : levels[plan.level]
            + (this.bars && this.bars.kill.includes(plan.lead) ? "+kill" : "")});
    };
  }
  await h.startAudio(game);
  return game;
}

function enemyAt(game, distance) {
  const foe = game.tanks[1];
  foe.active = true; foe.x = game.player.x + distance; foe.z = game.player.z;
}

/* Force a level from the next bar onward (tours only). */
function force(game, level) {
  game.bank.music.forced = level;
}

const SCENARIOS = {
  async title() {
    const game = await begin();
    game.state.mode = "title";
    run(game, 21.5);
    return game;
  },

  /* Explore, alert, combat, danger: four bars each, then a clear stinger. */
  tour(program) {
    return async () => {
      const game = await begin({effects: false, program, mode: "playing"});
      run(game, .1);
      const max = game.api.programs[program].max ?? 3;
      for (let level = 0; level <= max; level++) {
        force(game, level);
        run(game, toBar(game) + .05);
        // Four bars at this level (bar lengths differ per level).
        for (let bar = 0; bar < 4; bar++) run(game, toBar(game) + .05);
      }
      force(game, -1);
      game.state.mode = "arena-clear";
      run(game, toBar(game) + .05);
      run(game, toBar(game) + 1.5);
      return game;
    };
  },

  async kill() {
    const game = await begin({effects: false, program: "yard", mode: "playing"});
    run(game, .1);
    force(game, 2);
    run(game, toBar(game) + .05);
    run(game, toBar(game) + .2);
    game.state.kills++;
    game.bank.music.evalAt = 0;
    for (let bar = 0; bar < 3; bar++) run(game, toBar(game) + .05);
    return game;
  },

  async clear() {
    const game = await begin({effects: false, program: "foundry", mode: "playing"});
    run(game, .1);
    force(game, 2);
    run(game, toBar(game) + .05);
    run(game, toBar(game) + .05);
    force(game, -1);
    game.state.mode = "arena-clear";
    run(game, toBar(game) + .05);
    run(game, toBar(game) + .05);
    run(game, toBar(game) + 1);
    return game;
  },

  async boss() {
    const game = await begin({effects: false, program: "city", mode: "playing"});
    enemyAt(game, 5);
    run(game, 4);
    const boss = game.tanks[5];
    boss.active = true; boss.boss = true; boss.x = 4; boss.z = 2; boss.cooldown = 1;
    run(game, toBar(game) + .3);
    for (let bar = 0; bar < 3; bar++)
      run(game, toBar(game) + .05, () => { boss.cooldown = 1; });
    return game;
  },

  async victory() {
    const game = await begin({effects: false, program: "glacier", mode: "playing"});
    enemyAt(game, 5);
    run(game, 6, () => { game.tanks[1].cooldown = 1; });
    game.state.mode = "victory";
    run(game, 14);
    return game;
  },

  async defeat() {
    const game = await begin({effects: false, program: "fortress", mode: "playing"});
    enemyAt(game, 5);
    game.player.health = 20;
    run(game, 7, () => { game.tanks[1].cooldown = 1; });
    game.state.mode = "game-over";
    run(game, 14);
    return game;
  },

  /* The real state machine both ways: a foe closes, fires, then leaves;
     the music climbs at once and calms only after the hold times. */
  async transitions() {
    const game = await begin({effects: false, program: "yard", mode: "playing"});
    const foe = game.tanks[1];
    enemyAt(game, 12);
    run(game, 7);
    run(game, 7, (t) => { foe.x = game.player.x + 12 - t * 1.2; });
    run(game, 10, () => { foe.cooldown = 1; });
    run(game, 26, (t) => { foe.x = game.player.x + 5 + t * .5; });
    return game;
  },

  /* About a minute of play through the real state machine: explore, a foe
     closes (alert), shots (combat), armor drops (danger), the last foe
     falls and the run ends in victory. Effects play from the same
     SoundBank and take voices exactly as in the game. */
  match(options = {}) {
    return async () => {
      const game = await begin({...options, program: "yard", mode: "playing"});
      const {state, player} = game;
      const foe = game.tanks[1];
      let shotAt = 0, enemyShotAt = 0, heartAt = 0;
      const time = () => state.wallTime;
      const combat = (rate) => {
        if (time() >= shotAt) {
          shotAt = time() + rate;
          h.effect(game, "shot", true); player.cooldown = .7;
          if (Math.floor(time() * 7) % 3 === 0) {
            h.effect(game, "hit"); state.hitConfirm = .18;
          }
        }
        if (time() >= enemyShotAt) {
          enemyShotAt = time() + rate * 1.3;
          h.effect(game, "shot", false); foe.cooldown = .9;
        }
        if (player.health / player.maxHealth < .3 && time() >= heartAt) {
          heartAt = time() + .82;
          h.effect(game, "heartbeat");
        }
      };
      enemyAt(game, 11);
      run(game, 12, () => { foe.x = player.x + 11 - Math.min(2, time() * .1); });
      run(game, 10, (t) => { foe.x = player.x + 8.5 - t * .4; });
      run(game, 14, (t) => {
        combat(.85);
        if (Math.abs(t - 5) < 1 / 60 || Math.abs(t - 9) < 1 / 60) {
          h.effect(game, "damage"); state.damageIndicator = .55; player.health -= 18;
        }
      });
      run(game, 14, (t) => {
        combat(.75);
        if (Math.abs(t - 1) < 1 / 60 || Math.abs(t - 4) < 1 / 60) {
          h.effect(game, "damage"); state.damageIndicator = .55; player.health -= 20;
        }
      });
      // The last foe falls.
      h.effect(game, "explosion"); h.effect(game, "kill");
      foe.active = false; state.kills++;
      run(game, 1.2);
      state.mode = "victory";
      run(game, 9);
      return game;
    };
  },
};

const PROGRAM_IDS = ["yard", "foundry", "glacier", "city", "fortress",
  "practice", "vesper", "ally", "sunset", "last"];
for (const id of PROGRAM_IDS) SCENARIOS["tour-" + id] = SCENARIOS.tour(id);
SCENARIOS["match-60s"] = SCENARIOS.match();
SCENARIOS["match-60s-sfx-only"] = SCENARIOS.match({music: 0});
SCENARIOS["match-60s-music-only"] = SCENARIOS.match({effects: false});
delete SCENARIOS.tour; delete SCENARIOS.match;

module.exports = {SCENARIOS, PROGRAM_IDS, MISSION, run, toBar, begin, select,
  enemyAt, force, defaults};
