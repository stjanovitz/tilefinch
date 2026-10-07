"use strict";

// Exact answers and work counts, not noisy host timings. The reference below
// retains the pre-index category walks and intersection arithmetic separately
// from both production paths. Each query is checked, not just a final digest.
const fs = require("node:fs"), vm = require("node:vm");
const assert = require("node:assert/strict"), path = require("node:path");
const args = process.argv.slice(2);
assert(args.every(arg => !arg.startsWith("--") || arg === "--mutant-drop-crate"));
const sourcePath = args.find(arg => !arg.startsWith("--"))
  || path.join(__dirname, "../examples/treadline-arena/game.js");
// Route and bank planning live in bots.js beside game.js.
const source = fs.readFileSync(sourcePath, "utf8")
  + fs.readFileSync(path.join(path.dirname(sourcePath), "bots.js"), "utf8");
function fn(name) {
  const start = source.indexOf("  function " + name + "(");
  assert(start >= 0, name + " is present");
  const end = source.indexOf("\n  }", start + 5);
  assert(end > start, name + " is complete");
  return source.slice(start, end + 4);
}
const c = {Math, Uint8Array, Uint16Array, Uint32Array, Int8Array, Int16Array,
  Float32Array, Float64Array, MAX_TANKS: 6, MAX_BARRIERS: 6,
  COLLISION_GRID_SIDE: 32, COLLISION_GRID_SCALE: 2, COLLISION_GRID_CELLS: 1024,
  COLLISION_GRID_ORIGIN: 8, COLLISION_GRID_LAST: 31, MAX_HAZARDS: 8, MAX_CRATES: 4,
  ARENA_EDGE: 7.85, HULL_EDGE: 7.65, CRATE_SHELL_HALF: .32,
  CRATE_PLACE_EDGE: 6.5,
  state: {arena: 0, gateOpen: false}, online: {active: false},
  crateGridReady: true, tinyCircleArena: -1, rayGeometryArena: -1, aimGeometryEpoch: 0,
  rayActiveMask: 0, rayGateMask: 0, RAY_BARRIER_MASK: 0x03f00000,
  rayTestVisits: 0, rayTestExact: 0,
  rayBounds: new Float64Array(128), rayAxisMasks: new Uint32Array(64),
  RAY_AIM_PADDING: .08, rayAimBoxes: new Float64Array(256),
  rayIntervalMasks: new Uint32Array(2048), rayBlockers: new Int16Array(72).fill(-1),
  shellCellMasks: new Uint32Array(1024),
  tinyCircleOccupied: new Uint8Array(1024), barrierCircleGrid: new Uint8Array(1024),
  crateCircleGrid: new Uint8Array(1024),
  barriers: Array.from({length: 6}, () => ({active: false, present: false,
    left: 0, right: 0, top: 0, bottom: 0})),
  crates: Array.from({length: 4}, () => ({active: false, x: 0, z: 0})),
  tanks: Array.from({length: 6}, () => ({active: false, x: 0, z: 0})),
  refreshSceneryHeights() {}, surfaceHeightAt() { return 0; }, spawnHazard() {},
  restartBotNavigation() {}, prepareNavigationGrids() {}};
vm.createContext(c);
vm.runInContext(fs.readFileSync(path.join(__dirname,
  "../examples/treadline-arena/arena-generator.js"), "utf8"), c);
const data = c.__treadlineArenaData, spatial = data.spatial;
// arena-generator.js's shared sizes, and game.js's shell-grid padding.
Object.assign(c, {arenaSpatial: spatial, CRATE_HALF: spatial.CRATE_HALF,
  CIRCLE_GRID_RADIUS: spatial.CIRCLE_GRID_RADIUS,
  SHELL_GRID_PADDING: Number(source.match(/const SHELL_GRID_PADDING = ([\d.]+);/)[1]),
  ARENA_OBSTACLE_BOUNDS: spatial.obstacleBounds, ARENA_GATE_BOUNDS: spatial.gateBounds,
  ARENA_OBSTACLE_COUNTS: spatial.obstacleCounts, ARENA_GATE_COUNTS: spatial.gateCounts,
  ARENA_BULLET_OBSTACLE_GRIDS: spatial.bulletObstacleGrids,
  ARENA_BULLET_GATE_GRIDS: spatial.bulletGateGrids,
  ARENA_CIRCLE_OBSTACLE_GRIDS: spatial.circleObstacleGrids,
  ARENA_CIRCLE_GATE_GRIDS: spatial.circleGateGrids});
for (const name of ["gridFirst", "gridLast", "segmentHitsBox", "lineCrossesWallsFallback",
  "setRayGeometryRect",
  "updateRayActiveMask", "fillRayGeometry", "rayCandidateMask", "lineCrossesWalls",
  "fillArenaSpatialData", "fillCircleCandidateRect", "fillBarrierCircleGrid",
  "fillTinyCircleOccupiedRect", "fillTinyCircleOccupied", "circleHitsObstacle",
  "initializeArenaHazards", "invalidateBotNavigation"]) {
  let body = fn(name);
  if (name === "lineCrossesWalls") {
    assert(body.includes("      const at = id * 4;"));
    assert(body.includes("      let hit;"));
    body = body.replace("      const at = id * 4;", "      rayTestVisits++; const at = id * 4;")
      .replace("      let hit;", "      rayTestExact++; let hit;");
  }
  vm.runInContext(body, c);
}

let oldVisits = 0, oldHelperCalls = 0, checks = 0, positive = 0, fallbackChecks = 0;
function oldBox(ax, az, bx, bz, left, right, top, bottom, padding) {
  oldHelperCalls++;
  left -= padding; right += padding; top -= padding; bottom += padding;
  if ((ax < left && bx < left) || (ax > right && bx > right)
      || (az < top && bz < top) || (az > bottom && bz > bottom)) return false;
  const dx = bx - ax, dz = bz - az;
  const absDx = dx < 0 ? -dx : dx, absDz = dz < 0 ? -dz : dz;
  if (absDx < .00001) return ax >= left && ax <= right
    && (absDz >= .00001 || (az >= top && az <= bottom));
  if (absDz < .00001) return az >= top && az <= bottom;
  const centerX = left + right - ax - bx, centerZ = top + bottom - az - bz;
  const cross = dx * centerZ - dz * centerX;
  return (cross < 0 ? -cross : cross)
    <= absDz * (right - left) + absDx * (bottom - top);
}
function reference(ax, az, bx, bz, overCover = false, padding = .08) {
  const minX = ax < bx ? ax : bx, maxX = ax > bx ? ax : bx;
  const minZ = az < bz ? az : bz, maxZ = az > bz ? az : bz;
  for (const crate of c.crates) {
    oldVisits++;
    if (crate.active && maxX >= crate.x - .22 - padding
        && minX <= crate.x + .22 + padding && maxZ >= crate.z - .22 - padding
        && minZ <= crate.z + .22 + padding
        && oldBox(ax, az, bx, bz, crate.x - .22, crate.x + .22,
          crate.z - .22, crate.z + .22, padding)) return true;
  }
  const bounds = spatial.obstacleBounds[c.state.arena];
  for (let at = 0; at < spatial.obstacleCounts[c.state.arena] * 4; at += 4) {
    oldVisits++;
    if (maxX >= bounds[at] - padding && minX <= bounds[at + 1] + padding
        && maxZ >= bounds[at + 2] - padding && minZ <= bounds[at + 3] + padding
        && oldBox(ax, az, bx, bz, bounds[at], bounds[at + 1],
          bounds[at + 2], bounds[at + 3], padding)) return true;
  }
  if (!overCover) for (const barrier of c.barriers) {
    oldVisits++;
    if (barrier.active && maxX >= barrier.left - padding
        && minX <= barrier.right + padding && maxZ >= barrier.top - padding
        && minZ <= barrier.bottom + padding
        && oldBox(ax, az, bx, bz, barrier.left, barrier.right,
          barrier.top, barrier.bottom, padding)) return true;
  }
  if (!c.state.gateOpen) {
    const gates = spatial.gateBounds[c.state.arena];
    for (let at = 0; at < spatial.gateCounts[c.state.arena] * 4; at += 4) {
      oldVisits++;
      if (maxX >= gates[at] - padding && minX <= gates[at + 1] + padding
          && maxZ >= gates[at + 2] - padding && minZ <= gates[at + 3] + padding
          && oldBox(ax, az, bx, bz, gates[at], gates[at + 1],
            gates[at + 2], gates[at + 3], padding)) return true;
    }
  }
  return false;
}
function compare(ax, az, bx, bz, overCover = false, padding = .08, slot = -1) {
  const visitsBefore = oldVisits, helpersBefore = oldHelperCalls;
  const fallback = c.rayGeometryArena !== c.state.arena
    || Number.isNaN(bx - ax) || Number.isNaN(bz - az) || Number.isNaN(padding);
  const expected = reference(ax, az, bx, bz, overCover, padding);
  const detail = () => JSON.stringify({arena: c.state.arena,
    ax, az, bx, bz, overCover, padding, slot, ready: c.rayGeometryArena});
  assert.equal(c.lineCrossesWallsFallback(ax, az, bx, bz, overCover, padding), expected, detail());
  assert.equal(c.lineCrossesWalls(ax, az, bx, bz, overCover, padding, slot), expected, detail());
  if (fallback) {
    // Both paths execute the same old walk here. Keep these correctness
    // probes out of the optimized-corpus counts on both sides.
    oldVisits = visitsBefore; oldHelperCalls = helpersBefore; fallbackChecks++;
  }
  checks++; positive += Number(expected);
  return expected;
}
let seed = 918273;
function random() { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return (seed >>> 0) / 4294967296; }
function placeArena() {
  const arena = data.arenas[c.state.arena];
  for (let at = 0; at < 6; at++) {
    const barrier = c.barriers[at], rect = arena.barriers[at];
    barrier.present = barrier.active = at < spatial.itemCount(arena, "barriers");
    if (barrier.present) Object.assign(barrier, {left: rect[0] - rect[2] * .5,
      right: rect[0] + rect[2] * .5, top: rect[1] - rect[3] * .5,
      bottom: rect[1] + rect[3] * .5});
  }
  c.fillBarrierCircleGrid();
  assert.equal(c.rayGeometryArena, -1);
  for (const crate of c.crates) crate.active = false;
  c.initializeArenaHazards(); // real offline placement/final-publication path
  assert.equal(c.rayGeometryArena, c.state.arena);
}
const storage = [c.rayBounds, c.rayAxisMasks, c.rayIntervalMasks, c.rayBlockers,
  c.shellCellMasks, c.rayAimBoxes];
const generator = c.__treadlineCreateArenaGenerator(data.arenas, data.generatedArena);
const pads = [0, .08, .1, .32, .5, -.08, -.3, 2];
const deltas = [0, .000009999, .00001, .000010001, -.000009999, -.00001, -.000010001];
for (let scene = 0; scene < 103; scene++) {
  c.state.arena = scene < 3 ? scene : 3;
  if (scene >= 3) {
    generator.materialize(Math.imul(scene, 0x45d9f3b) >>> 0);
    c.fillArenaSpatialData(3); // same index, new generated rectangle placement
    assert.equal(c.rayGeometryArena, -1, "replacement uses fallback until final placement");
    for (let q = 0; q < 64; q++) compare(random()*16-8, random()*16-8,
      random()*16-8, random()*16-8, false, .08, q % 72);
  }
  placeArena();
  const intervals = Array.from(c.rayIntervalMasks);
  for (let q = 0; q < 3072; q++) {
    if (q % 31 === 0) { c.barriers[q % 6].active = !c.barriers[q % 6].active; c.invalidateBotNavigation(); }
    if (q % 47 === 0) { c.crates[q % 4].active = !c.crates[q % 4].active; c.invalidateBotNavigation(); }
    c.state.gateOpen = !!(q & 1); // gate applicability must be immediate
    let ax = random()*20-10, az = random()*20-10;
    let bx = random()*20-10, bz = random()*20-10;
    const padding = pads[q % pads.length];
    const bounds = spatial.obstacleBounds[c.state.arena];
    const at = (q % spatial.obstacleCounts[c.state.arena]) * 4;
    if (q % 5 === 0) { ax = bounds[at] - padding; az = bounds[at + 2]; bx = ax + deltas[q % 7]; }
    if (q % 5 === 1) { az = bounds[at + 2] - padding; ax = bounds[at]; bz = az + deltas[q % 7]; }
    if (q % 5 === 2) { ax = (q % 32) * .5 - 8 + (q % 3 - 1) * 1e-12; bx = ax; }
    if (q % 5 === 3) { ax = bx = c.crates[q % 4].x; az = bz = c.crates[q % 4].z; }
    compare(ax, az, bx, bz, q % 3 === 0, padding, q % 73 - 1);
  }
  assert.deepEqual(Array.from(c.rayIntervalMasks), intervals,
    "destruction/reactivation/gate changes must not rebuild or edit membership");
}
const corpusWork = {optimizedQueries: checks - fallbackChecks, fallbackQueries: fallbackChecks,
  oldRectangleVisits: oldVisits, oldHelperCalls,
  newRectangleVisits: c.rayTestVisits, newExactTests: c.rayTestExact};
assert(corpusWork.newRectangleVisits < corpusWork.oldRectangleVisits * .75,
  "the same world queries eliminate substantial category walks");
// Unsupported/not-ready states must retain the general predicate.
for (const padding of [NaN, Infinity, -Infinity, -.8, 100])
  for (const value of [NaN, Infinity, -Infinity, 0]) compare(value, 1, 0, 1, false, padding);
c.crateGridReady = false; c.fillRayGeometry();
assert.equal(c.rayGeometryArena, -1); compare(0, 0, 2, 2);
c.crateGridReady = true;
const savedCount = spatial.obstacleCounts[c.state.arena];
spatial.obstacleCounts[c.state.arena] = 17; c.fillRayGeometry();
assert.equal(c.rayGeometryArena, -1); compare(0, 0, 2, 2);
spatial.obstacleCounts[c.state.arena] = savedCount;
c.fillRayGeometry();

// Exercise every supported bit, especially the signed high bit for gate 5.
const savedObstacles = spatial.obstacleBounds[c.state.arena].slice();
const savedGates = spatial.gateBounds[c.state.arena];
const savedGateCount = spatial.gateCounts[c.state.arena];
spatial.obstacleCounts[c.state.arena] = 16;
spatial.gateCounts[c.state.arena] = 6;
spatial.gateBounds[c.state.arena] = new Float32Array(24);
for (let id = 0; id < 32; id++) {
  const x = (id & 7) * 2 - 7.5, z = (id >> 3) * 2 - 7.5;
  if (id < 4) Object.assign(c.crates[id], {active: true, x, z});
  else if (id < 20) spatial.obstacleBounds[c.state.arena].set(
    [x - .1, x + .1, z - .1, z + .1], (id - 4) * 4);
  else if (id < 26) Object.assign(c.barriers[id - 20], {active: true,
    left: x - .1, right: x + .1, top: z - .1, bottom: z + .1});
  else spatial.gateBounds[c.state.arena].set(
    [x - .1, x + .1, z - .1, z + .1], (id - 26) * 4);
}
c.state.gateOpen = false; c.fillRayGeometry();
for (let id = 0; id < 32; id++) {
  const x = (id & 7) * 2 - 7.5, z = (id >> 3) * 2 - 7.5;
  assert(compare(x, z, x, z, false, 0, id));
  assert.equal(c.rayBlockers[id], id);
  assert.equal(compare(x, z, x, z, true, 0, id), !(id >= 20 && id < 26));
}
spatial.obstacleBounds[c.state.arena].set(savedObstacles);
spatial.obstacleCounts[c.state.arena] = savedCount;
spatial.gateBounds[c.state.arena] = savedGates;
spatial.gateCounts[c.state.arena] = savedGateCount;

// Isolate a remembered blocker, change the current ray, destroy/reactivate it,
// and toggle the gate. These positive tests catch stale visibility answers.
spatial.obstacleCounts[c.state.arena] = 0;
for (const barrier of c.barriers) barrier.active = false;
for (const crate of c.crates) crate.active = false;
Object.assign(c.crates[0], {active: true, x: -7.5, z: -7.5});
c.state.gateOpen = true; c.fillRayGeometry();
if (args.includes("--mutant-drop-crate")) c.rayIntervalMasks.fill(0);
assert(compare(-7.5, -7.5, -7.5, -7.5, false, .08, 0));
assert.equal(c.rayBlockers[0], 0);
assert(!compare(6, 6, 6.1, 6.1, false, .08, 0));
assert(compare(-7.5, -7.5, -7.5, -7.5, false, .08, 0));
c.crates[0].active = false; c.invalidateBotNavigation();
assert(!compare(-7.5, -7.5, -7.5, -7.5, false, .08, 0));
c.crates[0].active = true; c.invalidateBotNavigation();
assert(compare(-7.5, -7.5, -7.5, -7.5, false, .08, 0));
const gates = spatial.gateBounds[c.state.arena];
const gateX = (gates[0] + gates[1]) * .5, gateZ = (gates[2] + gates[3]) * .5;
c.state.gateOpen = false; assert(compare(gateX, gateZ, gateX, gateZ, false, 0, 1));
c.state.gateOpen = true; assert(!compare(gateX, gateZ, gateX, gateZ, false, 0, 1));
// Negative control must expose a lost interval member, even with fresh active bits.
c.rayBlockers.fill(-1); c.rayIntervalMasks.fill(0);
assert(reference(-7.5, -7.5, -7.5, -7.5));
assert(!c.lineCrossesWalls(-7.5, -7.5, -7.5, -7.5));
c.fillRayGeometry();
const originalBounds = c.rayBounds;
let optimizedVisits = 0;
c.rayBounds = new Proxy(originalBounds, {get(target, key) {
  if (typeof key === "string" && /^\d+$/.test(key) && Number(key) % 4 === 0) optimizedVisits++;
  return Reflect.get(target, key, target);
}});
oldVisits = oldHelperCalls = 0;
for (let q = 0; q < 1024; q++) {
  reference(5.5, 5.5, 6.5, 6.5);
  assert(!c.lineCrossesWalls(5.5, 5.5, 6.5, 6.5));
}
assert(oldVisits >= 10240);
assert.equal(optimizedVisits, 0, "empty interval skips every rectangle visit");
c.rayBounds = originalBounds;
c.online.active = true;
for (const crate of c.crates) crate.active = false;
c.initializeArenaHazards(); // real online early-return publication path
assert.equal(c.rayGeometryArena, c.state.arena);
for (const [at, array] of [c.rayBounds, c.rayAxisMasks, c.rayIntervalMasks, c.rayBlockers,
  c.shellCellMasks, c.rayAimBoxes].entries())
  assert.equal(array, storage[at], "setup reuses each fixed allocation");
assert.equal(storage.reduce((sum, array) => sum + array.byteLength, 0), 15760);

// Lifecycle: a query never rebuilds the index; an invalidation re-masks it
// and an arena data refresh drops it, neither rebuilding; and the point
// queries the shells and hulls use never consult it (their first-hit order
// is their own). Checked by calling them, through counting wrappers.
{
  for (const name of ["barrierAt", "bulletHitsRectangles", "bulletHitsObstacle"])
    vm.runInContext(fn(name), c);
  let rebuilds = 0, masks = 0, candidates = 0;
  const fill = c.fillRayGeometry, mask = c.updateRayActiveMask, index = c.rayCandidateMask;
  c.fillRayGeometry = (...args) => { rebuilds++; return fill(...args); };
  c.updateRayActiveMask = (...args) => { masks++; return mask(...args); };
  c.rayCandidateMask = (...args) => { candidates++; return index(...args); };
  for (let q = 0; q < 64; q++)
    c.lineCrossesWalls(q * .2 - 6, -5, 6 - q * .1, 5, !!(q & 1), .08, q % 3 - 1);
  assert(rebuilds === 0 && candidates > 0, "queries read the index without rebuilding it");
  c.invalidateBotNavigation();
  assert(rebuilds === 0 && masks === 1, "invalidation re-masks the index");
  candidates = 0;
  for (let q = 0; q < 64; q++) {
    const x = q * .25 - 8, z = 7.5 - q * .2;
    c.barrierAt(x, z, .1); c.bulletHitsObstacle(x, z); c.circleHitsObstacle(x, z, .52);
  }
  assert(candidates === 0, "point queries keep their own order");
  c.fillArenaSpatialData(c.state.arena);
  assert(c.rayGeometryArena === -1 && rebuilds === 0,
    "an arena data refresh drops the index for the next placement");
  c.fillRayGeometry = fill; c.updateRayActiveMask = mask; c.rayCandidateMask = index;
}
console.log("treadline ray queries: " + JSON.stringify({checks, positive,
  generatedReplacements: 100, storageBytes: 15760, corpusWork,
  emptyRayOldVisits: oldVisits, emptyRayNewVisits: optimizedVisits,
  lostMemberNegativeControl: true}));
