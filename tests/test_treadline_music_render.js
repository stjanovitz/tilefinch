"use strict";

/* Replay recorded Treadline music command logs through the real native
   mixer (tests/game_audio_render.c): every recorded admission result must
   match src/game_audio.c, nothing clips, and music is audible but stays
   under the effects. Host CPU per mixed block is printed for comparison
   only; it is not a PSP cost and is not asserted.

   node tests/test_treadline_music_render.js RENDERER OUT_DIR */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {execFileSync} = require("node:child_process");
const h = require("./treadline_music_harness.js");
const {SCENARIOS} = require("./treadline_music_scenarios.js");

const [renderer, out] = process.argv.slice(2);
assert(renderer && out, "usage: test_treadline_music_render.js RENDERER OUT_DIR");

(async () => {
  fs.mkdirSync(out, {recursive: true});
  const results = {};
  for (const name of ["title", "kill", "boss", "victory", "defeat", "tour-glacier",
    "match-60s", "match-60s-sfx-only", "match-60s-music-only"]) {
    const game = await SCENARIOS[name]();
    const log = path.join(out, name + ".log");
    h.writeLog(game, log);
    const summary = JSON.parse(execFileSync(renderer, [log, "--repeat", "3"],
      {encoding: "utf8"}));
    results[name] = summary;
    assert.equal(summary.mismatches, 0,
      `${name}: native result differs from the recorded one: ${summary.firstMismatch}`);
    assert(summary.commands > 10, `${name}: commands replayed`);
    assert.equal(summary.clipped, 0, `${name}: no clipping`);
    if (name !== "match-60s-sfx-only")
      assert(summary.rms > 150, `${name}: music is audible (rms ${summary.rms})`);
    if (!name.startsWith("match") || name.endsWith("music-only"))
      assert(summary.peak < 9000, `${name}: music stays well under full scale`);
  }
  const on = results["match-60s"], off = results["match-60s-sfx-only"];
  assert(on.peak < 20000, "effects and music together leave headroom");
  console.log(`host mixer CPU per 512-frame block, 60 s match: effects only ` +
    `${off.hostMixMeanUs.toFixed(2)} us, effects + music ${on.hostMixMeanUs.toFixed(2)} us, ` +
    `music only ${results["match-60s-music-only"].hostMixMeanUs.toFixed(2)} us ` +
    `(host numbers, not PSP)`);
  console.log(`Treadline music render: ${Object.keys(results).length} logs replayed`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
