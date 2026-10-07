#!/usr/bin/env node
"use strict";

/* Render Treadline's adaptive-music listening previews on the host.

   node scripts/render-treadline-music-previews.js \
     build-preset-release/tilefinch-game-audio-render OUT_DIR [name...]

   Every scenario in tests/treadline_music_scenarios.js runs the reference
   game-audio.js, the shipped SoundBank and music.js; its recorded native commands are
   replayed through src/game_audio.c by the renderer, which writes 16-bit
   44.1 kHz stereo WAV files. Nothing is played aloud. */
const fs = require("node:fs");
const path = require("node:path");
const {execFileSync} = require("node:child_process");
const h = require("../tests/treadline_music_harness.js");
const {SCENARIOS} = require("../tests/treadline_music_scenarios.js");

const [renderer, out, ...only] = process.argv.slice(2);
if (!renderer || !out) {
  console.error("usage: render-treadline-music-previews.js RENDERER OUT_DIR [name...]");
  process.exit(2);
}

(async () => {
  fs.mkdirSync(path.join(out, "logs"), {recursive: true});
  const results = {};
  for (const name of Object.keys(SCENARIOS)) {
    if (only.length && !only.includes(name)) continue;
    const game = await SCENARIOS[name]();
    const log = path.join(out, "logs", name + ".log");
    h.writeLog(game, log);
    const json = execFileSync(renderer, [log, "--wav", path.join(out, name + ".wav"),
      "--repeat", "5"], {encoding: "utf8"});
    const summary = JSON.parse(json);
    summary.music = game.bank.music ? game.bank.music.snapshot().stats : null;
    summary.refusals = game.native.refusals.length;
    // Collapse the bar timeline into sections: [start seconds, program, part].
    summary.sections = [];
    for (const bar of game.timeline || []) {
      const last = summary.sections[summary.sections.length - 1];
      if (!last || last[1] !== bar.program || last[2] !== bar.part)
        summary.sections.push([Number(bar.start.toFixed(2)), bar.program, bar.part]);
    }
    summary.peakVoices = game.native.peakVoices;
    results[name] = summary;
    console.log(name, JSON.stringify({seconds: summary.seconds, peak: summary.peak,
      rms: summary.rms, mismatches: summary.mismatches,
      hostMixMeanUs: summary.hostMixMeanUs, misses: summary.music?.misses,
      steals: summary.music?.steals}));
  }
  fs.writeFileSync(path.join(out, "summary.json"), JSON.stringify(results, null, 1) + "\n");
})().catch((error) => { console.error(error); process.exitCode = 1; });
