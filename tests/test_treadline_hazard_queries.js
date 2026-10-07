"use strict";

// Terrain admission is conservative; selection and the circle predicate remain
// identical to the original highest-slot-first scan. Counts are not PSP timings.
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
function fn(name) {
  const at = source.indexOf("  function " + name + "(");
  assert(at >= 0, name);
  const end = source.indexOf("\n  }", at + 5);
  assert(end > at, name);
  return source.slice(at, end + 4);
}
const records = Array.from({length: 8}, () => ({active: false, type: 0,
  x: 0, z: 0, radius: 1, life: 0, renderY: 0, renderDiameter: 2, level: 0}));
let candidateVisits = 0, referenceVisits = 0;
const c = {
  COLLISION_GRID_CELLS: 1024, COLLISION_GRID_SIDE: 32, COLLISION_GRID_SCALE: 2,
  COLLISION_GRID_ORIGIN: 8, COLLISION_GRID_LAST: 31, MAX_HAZARDS: 8, MAX_CRATES: 4,
  hazards: new Proxy(records, {get(target, key) {
    if (typeof key === "string" && /^\d+$/.test(key)) candidateVisits++;
    return target[key];
  }}),
  hazardGrid: new Uint8Array(1024), hazardGridFallbackMask: 0, hazardCursor: 0,
  crates: Array.from({length: 4}, () => ({active: false})),
  state: {arena: 0}, surfaceHeightAt(x, z) { return (x + z) / 32; },
  // Ground-mark layers: spawnHazard stacks overlapping marks (no decals here).
  MAX_DECALS: 0, decals: [], decalCursor: 0,
  FLAT_STEP: Number(source.match(/const FLAT_STEP = ([\d.]+),/)[1]),
  MARK_TOP: Number(source.match(/MARK_TOP = ([\d.]+),/)[1]),
  MARK_LEVELS: Number(source.match(/MARK_LEVELS = (\d+);/)[1]),
};
vm.createContext(c);
for (const name of ["gridFirst", "gridLast", "updateHazardGrid", "spawnHazard", "hazardAt",
  "hazardAtFallback", "updateHazards", "groundMarkLevel"]) vm.runInContext(fn(name), c);
vm.runInContext(fn("resetHazards"), c);
// An arena start (beginArena, so replays and online snapshots too) clears
// every hazard, crate, grid bit and the cursor: run it on a dirty state.
// That moveTank reads terrain through hazardAt is the conformance
// fixtures' business (oil, ice and mud change travel there).
for (let at = 0; at < 3; at++) c.spawnHazard(at, at - 2, 2 - at, 1.2, 5);
for (const crate of c.crates) crate.active = true;
c.hazardGridFallbackMask = 3;
assert(c.hazardGrid.some(Boolean) && c.hazardCursor !== 0);
c.resetHazards();
assert(c.hazardGrid.every((value) => value === 0) && c.hazardGridFallbackMask === 0
  && c.hazardCursor === 0 && records.every((hazard) => !hazard.active)
  && c.crates.every((crate) => !crate.active), "an arena start resets hazards");
// Independent literal oracle: no shared candidate calculation or fallback.
function reference(x, z) {
  for (let at = records.length - 1; at >= 0; at--) {
    referenceVisits++;
    const hazard = records[at];
    if (!hazard.active) continue;
    const dx = x - hazard.x, dz = z - hazard.z;
    if (dx * dx + dz * dz < hazard.radius * hazard.radius) return hazard.type;
  }
  return -1;
}
let checks = 0, hits = 0, oldVisits = 0, newVisits = 0;
function compare(x, z) {
  candidateVisits = referenceVisits = 0;
  const expected = reference(x, z), actual = c.hazardAt(x, z);
  assert.equal(actual, expected, JSON.stringify({arena: c.state.arena, x, z}));
  oldVisits += referenceVisits; newVisits += candidateVisits;
  checks++; hits += expected !== -1;
  return actual;
}
let randomState = 0x346fd;
function random() {
  randomState ^= randomState << 13; randomState ^= randomState >>> 17;
  randomState ^= randomState << 5;
  return (randomState >>> 0) / 4294967296;
}
const storage = c.hazardGrid;
vm.runInContext(fs.readFileSync(path.join(__dirname,
  "../examples/treadline-arena/arena-generator.js"), "utf8"), c);
const arenaData = c.__treadlineArenaData;
const generator = c.__treadlineCreateArenaGenerator(arenaData.arenas,
  arenaData.generatedArena);
for (let scene = 0; scene < 103; scene++) {
  c.state.arena = scene < 3 ? scene : 3;
  if (scene >= 3) {
    // Terrain replacement reuses the generated slot; beginning its new run
    // executes the same production reset as authored arenas and replay.
    generator.materialize(Math.imul(scene, 0x45d9f3b) >>> 0);
    arenaData.spatial.fillSpatialData(3);
  }
  c.resetHazards();
  assert.equal(c.hazardGrid, storage);
  assert.equal(c.hazardGridFallbackMask, 0);
  assert(c.hazardGrid.every(value => value === 0));
  for (let at = 0; at < 8; at++)
    c.spawnHazard(at % 3, random() * 18 - 9, random() * 18 - 9,
      at & 1 ? .85 : 1.25, at & 2 ? -1 : .2);
  for (let q = 0; q < 4096; q++) {
    if (q % 71 === 0) c.spawnHazard(q % 3,
      random() * 18 - 9, random() * 18 - 9, .25 + random() * 1.75, .15);
    if (q % 37 === 0) c.updateHazards(.04);
    if (q % 53 === 0) records[q % 8].active = !records[q % 8].active;
    let x = random() * 20 - 10, z = random() * 20 - 10;
    const h = records[q % 8], epsilon = (q % 3 - 1) * 1e-12;
    if (q % 4 === 0) { x = h.x + h.radius + epsilon; z = h.z; }
    if (q % 4 === 1) { x = h.x; z = h.z - h.radius + epsilon; }
    if (q % 4 === 2) { x = (q % 33) * .5 - 8 + epsilon; z = h.z; }
    compare(x, z);
  }
}
// Highest numeric slot wins even when the ring has wrapped: not newest spawn.
c.resetHazards();
for (let i = 0; i < 8; i++) c.spawnHazard(i % 3, 0, 0, 1, -1);
assert.equal(compare(0, 0), 1);
c.spawnHazard(2, 0, 0, 1, -1);
assert.equal(compare(0, 0), 1);
records[7].active = false;
assert.equal(compare(0, 0), 0);
// Expiry is observed immediately without rebuilding membership.
c.resetHazards(); c.spawnHazard(2, .5, .5, .85, .01);
assert.equal(compare(.5, .5), 2); c.updateHazards(.02);
assert.equal(compare(.5, .5), -1);
// Slot recycling clears old membership, not merely correctness via a new center.
c.resetHazards();
for (let i = 0; i < 8; i++) c.spawnHazard(i % 3, -6, -6, .25, -1);
for (let i = 0; i < 8; i++) c.spawnHazard(i % 3, 6, 6, .25, -1);
assert.equal(c.hazardGrid[4 * 32 + 4], 0);
assert.equal(compare(-6, -6), -1); assert.equal(compare(6, 6), 1);
// Invalid/custom geometry and positions must retain the old scan semantics.
for (const radius of [-1, 0, 2, 2.00001, 20, NaN, Infinity]) {
  c.resetHazards(); c.spawnHazard(2, 0, 0, radius, -1);
  for (const x of [-Infinity, -20, -8, -7.999999999, 0, 7.999999999, 8, 20, NaN, Infinity])
    for (const z of [-8, -.5, 0, .5, 8, NaN]) compare(x, z);
  c.resetHazards(); assert.equal(c.hazardGridFallbackMask, 0);
}
for (const x of [NaN, Infinity, -Infinity, Number.MAX_VALUE]) {
  c.spawnHazard(1, x, 0, 1, -1);
  compare(0, 0); compare(x, 0);
}
// Mutation guards: missing a positive cell and reversing precedence both fail.
c.resetHazards(); c.spawnHazard(2, 0, 0, 1, -1);
const center = 16 * 32 + 16, saved = c.hazardGrid[center];
c.hazardGrid[center] = 0;
assert.notEqual(c.hazardAt(0, 0), reference(0, 0));
c.hazardGrid[center] = saved;
c.spawnHazard(1, 0, 0, 1, -1);
const reverse = fn("hazardAt").replace("31 - Math.clz32(mask)",
  "31 - Math.clz32((mask & -mask) >>> 0)");
assert.notEqual(reverse, fn("hazardAt")); vm.runInContext(reverse, c);
assert.notEqual(c.hazardAt(0, 0), reference(0, 0));
vm.runInContext(fn("hazardAt"), c);
// Empty queries must avoid all record visits, not just return the right type.
candidateVisits = 0;
assert.equal(c.hazardAt(6, 6), -1); assert.equal(candidateVisits, 0);
assert(newVisits < oldVisits / 2, "conservative grid removes record scans");
console.log(JSON.stringify({checks,hits,oldVisits,newVisits,
  generatedReplacements:100,fixedStorageBytes:storage.byteLength,
  omittedMemberAndOrderNegativeControls:true}));
