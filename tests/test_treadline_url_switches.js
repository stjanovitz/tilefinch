"use strict";

/* Treadline's page-URL switches, run from game.js's own parsing block:
   qualification runs read seed= (Onslaught's pinned arena seed), ordinary
   play never does, and a switch matches only as a whole query name. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
const begin = source.indexOf("  let repeatedFrameCaptures = 0;\n");
const last = source.indexOf("  const qualificationTooling", begin);
assert(begin >= 0 && last > begin, "game.js's URL switch block");
const block = source.slice(begin, source.indexOf("\n", last) + 1);

function parse(href, search = href.slice(href.indexOf("?"))) {
  const context = vm.createContext({location: {href, search}, globalThis: {}});
  vm.runInContext(block + "\nthis.out = {requestedArenaSeed, qualificationRun,"
    + " qualificationSoak, qualificationLongSoak, qualificationTooling};", context);
  return context.out;
}

const page = "https://games.test/examples/treadline-arena/index.html";
assert.equal(parse(`${page}?seed=5`).requestedArenaSeed, 0,
  "ordinary play ignores seed=");
assert.equal(parse(`${page}?daily=20261001&seed=5`).qualificationTooling, false);
assert.equal(parse(`${page}?qualification=soak&seed=5`).requestedArenaSeed, 5);
assert.equal(parse(`${page}?qualification=soak&foo_seed=7`).requestedArenaSeed, 0,
  "a name ending in seed is not seed=");
assert.equal(parse(`${page}?qualification=soak&seed=12x`).requestedArenaSeed, 0);
const longSoak = parse(`${page}?qualification=long-soak&seed=12345&mode=onslaught`);
assert(longSoak.qualificationLongSoak && !longSoak.qualificationSoak
  && longSoak.requestedArenaSeed === 12345, JSON.stringify(longSoak));
// The PSP can publish the committed query in href before search.
assert.equal(parse(`${page}?qualification=soak&seed=9`, "").requestedArenaSeed, 9);
assert.equal(parse(`${page}?qualification=input#seed=4`).requestedArenaSeed, 0,
  "a fragment is not the query");
console.log("Treadline URL switches: seed= in qualification runs only, whole names");
