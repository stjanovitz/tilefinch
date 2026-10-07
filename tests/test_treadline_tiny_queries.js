"use strict";

// Real circle predicates, real arena generation, and deterministic workload
// counts. Host time is not a prediction of PSP frame time.
const fs = require("node:fs"), vm = require("node:vm");
const assert = require("node:assert/strict"), path = require("node:path");
const args = process.argv.slice(2);
assert(args.every(arg => !arg.startsWith("--") || arg === "--mutant-no-refresh"));
const sourcePath = args.find(arg => !arg.startsWith("--"))
  || path.join(__dirname, "../examples/treadline-arena/game.js");
const source = fs.readFileSync(sourcePath, "utf8");
function fn(text, name) {
  const at = text.indexOf('  function ' + name + '(');
  assert(at >= 0, name);
  return text.slice(at, text.indexOf('\n  }', at + 5) + 4);
}
// Placement/recovery must pass a persistent predicate, not allocate one per
// action. Exercise the real adapter and compare every deterministic probe.
{
  const tank = {id: 7}, seen = [];
  const placement = {
    Math, clearSpot: {x: 0, z: 0},
    spawnBlocked(owner, x, z, radius) {
      assert.equal(owner, tank); assert.equal(radius, .4);
      seen.push([x, z]);
      return seen.length < 20;
    }
  };
  vm.createContext(placement);
  for (const name of ['searchClearSpot', 'tankSpotBlocked', 'findClearSpot'])
    vm.runInContext(fn(source, name), placement);
  const search = placement.searchClearSpot;
  let predicate;
  placement.searchClearSpot = function (...args) {
    predicate = args[5];
    return search(...args);
  };
  assert(placement.findClearSpot(tank, 1, -2, .4));
  const first = predicate;
  assert.equal(first, placement.tankSpotBlocked, 'placement uses retained predicate');
  const expected = [[1, -2]];
  for (let ring = 1; expected.length < 20; ring++)
    for (let step = 0; step < 16 && expected.length < 20; step++) {
      const angle = step * Math.PI / 8;
      expected.push([1 + Math.sin(angle) * ring * .25,
        -2 + Math.cos(angle) * ring * .25]);
    }
  assert.deepEqual(seen, expected);
  seen.length = 0;
  assert(placement.findClearSpot(tank, 1, -2, .4));
  assert.equal(predicate, first, 'repeated recovery does not create a closure');
}
// The tiny-circle table serves the camera probe (cameraPointBlocked and the
// clear-box test in cameraClearAlong). Its point query is compared with the
// exact circleHitsObstacle below, and its refresh runs through the real
// initializeArenaHazards hook; that the camera reads it in play is the
// camera fixtures' and the invariant sweep's business.
const c = {Math, Uint8Array, Uint16Array, Uint32Array, Int8Array, Int16Array, Float32Array,
  MAX_BARRIERS: 6, COLLISION_GRID_SCALE: 2,
  COLLISION_GRID_SIDE: 32, COLLISION_GRID_CELLS: 1024,
  COLLISION_GRID_ORIGIN: 8, COLLISION_GRID_LAST: 31, CRATE_PLACE_EDGE: 6.5,
  MAX_HAZARDS: 8, MAX_CRATES: 4,
  state: {arena: 0, gateOpen: false}, crateGridReady: true, tinyCircleArena: -1,
  rayGeometryArena: -1, fillRayGeometry() {}, aimGeometryEpoch: 0,
  tinyCircleOccupied: new Uint8Array(1024), refreshSceneryHeights() {},
  barriers: Array.from({length: 6}, () => ({active: false, present: false, left: 0, right: 0, top: 0, bottom: 0})),
  crates: Array.from({length: 4}, () => ({active: true, x: 0, z: 0})),
  barrierCircleGrid: new Uint8Array(1024), crateCircleGrid: new Uint8Array(1024)};
vm.createContext(c);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../examples/treadline-arena/arena-generator.js'), 'utf8'), c);
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
for (const name of ['gridFirst', 'gridLast', 'fillArenaSpatialData', 'fillCircleCandidateRect', 'fillBarrierCircleGrid',
  'fillTinyCircleOccupiedRect', 'fillTinyCircleOccupied', 'circleHitsObstacle',
  'cameraPointBlocked']) {
  let body = fn(source, name);
  if (name === 'fillArenaSpatialData' && args.includes('--mutant-no-refresh'))
    body = body.replace('      fillTinyCircleOccupied();', '      /* deliberately omit refresh */');
  vm.runInContext(body, c);
}
const generator = c.__treadlineCreateArenaGenerator(data.arenas, data.generatedArena);
let randomState = 51972, checks = 0, positives = 0, staleNegatives = 0;
function random() { randomState ^= randomState << 13; randomState ^= randomState >>> 17; randomState ^= randomState << 5; return (randomState >>> 0) / 4294967296; }
// The camera's point query, with the table admitted as updateCamera does.
const tiny = (x, z) => c.cameraPointBlocked(x, z,
  c.crateGridReady && c.tinyCircleArena === c.state.arena);
function compare(x, z) {
  const expected = c.circleHitsObstacle(x, z, .07);
  assert.strictEqual(tiny(x, z), expected,
    JSON.stringify({arena: c.state.arena, x, z, ready: c.tinyCircleArena}));
  checks++; positives += Number(expected);
}
function refreshCrates() {
  c.crateCircleGrid.fill(0);
  for (let i = 0; i < 4; i++) {
    const crate = c.crates[i];
    c.fillCircleCandidateRect(c.crateCircleGrid, i, crate.x-.22, crate.x+.22, crate.z-.22, crate.z+.22);
  }
}
for (let scene = 0; scene < 103; scene++) {
  c.state.arena = scene < 3 ? scene : 3;
  if (scene >= 3) {
    // Same-index generated replacement. First deliberately bypass the game
    // refresh hook: demonstrate the stale union can miss a new obstacle.
    generator.materialize(Math.imul(scene, 0x45d9f3b) >>> 0);
    spatial.fillSpatialData(3);
    if (scene > 3 && c.tinyCircleArena === 3) {
      let found = false;
      for (let cell = 0; cell < 1024 && !found; cell++) {
        const x = (cell % 32) * .5 - 7.75, z = (cell >> 5) * .5 - 7.75;
        if (!c.tinyCircleOccupied[cell] && c.circleHitsObstacle(x, z, .07)) {
          assert.strictEqual(tiny(x, z), false);
          staleNegatives++; found = true;
        }
      }
    }
    c.fillArenaSpatialData(3); // actual production invalidation/rebuild hook
    for (let cell = 0; cell < 1024; cell++)
      compare((cell % 32)*.5-7.75, (cell >> 5)*.5-7.75);
  }
  const arena = data.arenas[c.state.arena];
  for (let i = 0; i < 6; i++) {
    const b = c.barriers[i], rect = arena.barriers[i];
    b.active = b.present = i < spatial.itemCount(arena, 'barriers');
    if (b.present) {
      b.left = rect[0] - rect[2] / 2; b.right = rect[0] + rect[2] / 2;
      b.top = rect[1] - rect[3] / 2; b.bottom = rect[1] + rect[3] / 2;
    }
  }
  c.fillBarrierCircleGrid();
  assert.strictEqual(c.tinyCircleArena, -1);
  for (let i = 0; i < 4; i++) Object.assign(c.crates[i], {x: random()*16-8, z: random()*16-8, active: true});
  refreshCrates();
  c.crateGridReady = false; c.fillTinyCircleOccupied();
  assert.strictEqual(c.tinyCircleArena, -1);
  for (let i = 0; i < 16; i++) compare(random()*20-10, random()*20-10);
  c.crateGridReady = true; c.fillTinyCircleOccupied();
  assert.strictEqual(c.tinyCircleArena, c.state.arena);
  const bounds = spatial.obstacleBounds[c.state.arena];
  for (let q = 0; q < 4096; q++) {
    let x = random()*20-10, z = random()*20-10;
    const radius = .07, at = (q % spatial.obstacleCounts[c.state.arena]) * 4;
    const barrier = c.barriers[q % 6];
    if (q % 4 === 0) { x = bounds[at] - radius; z = bounds[at+2]; }
    if (q % 4 === 1) { x = barrier.right + radius; z = barrier.top; }
    if (q % 4 === 2) { x = (q % 32)*.5-8 + (q%3-1)*1e-12; z = (q % 31)*.5-8; }
    if (q % 17 === 0) barrier.active = !barrier.active;
    if (q % 19 === 0) c.crates[q % 4].active = !c.crates[q % 4].active;
    c.state.gateOpen = !!(q % 2);
    compare(x, z);
  }
}
assert(staleNegatives > 0, 'stale same-slot generation must be detected');
// A deliberately lost dynamic member fails at an otherwise-empty edge cell.
c.state.gateOpen = true;
for (const b of c.barriers) b.active = false;
for (const crate of c.crates) crate.active = false;
c.crates[0].active = true; c.crates[0].x = -7.5; c.crates[0].z = -7.5;
refreshCrates(); c.fillTinyCircleOccupied();
compare(-7.5, -7.5);
c.tinyCircleOccupied[1 * 32 + 1] = 0;
assert(c.circleHitsObstacle(-7.5, -7.5, .07));
assert(!tiny(-7.5, -7.5));
// Exercise the real final-placement hook, including the online early return.
Object.assign(c, {MAX_TANKS: 6, tanks: Array.from({length: 6}, () => ({active: false, x: 0, z: 0})),
  online: {active: false}, surfaceHeightAt() { return 0; }, spawnHazard() {},
  invalidateBotNavigation() {}, prepareNavigationGrids() {}});
vm.runInContext(fn(source, "initializeArenaHazards"), c);
const storage = c.tinyCircleOccupied;
c.initializeArenaHazards();
assert.equal(c.tinyCircleArena, c.state.arena);
for (const crate of c.crates) if (crate.active) compare(crate.x, crate.z);
c.online.active = true;
for (const crate of c.crates) crate.active = false;
c.tinyCircleOccupied.fill(0);
c.initializeArenaHazards();
assert.equal(c.tinyCircleArena, c.state.arena);
assert.equal(c.tinyCircleOccupied, storage, "setup reuses its fixed table");
for (let cell = 0; cell < 1024; cell++)
  compare((cell % 32)*.5-7.75, (cell >> 5)*.5-7.75);
// Pin the operation elimination too: an empty valid cell does not enter the
// exact predicate, but all refusal/fallback conditions still do.
const emptyCell = c.tinyCircleOccupied.findIndex(value => value === 0);
assert(emptyCell >= 0);
const emptyX = (emptyCell % 32)*.5-7.75, emptyZ = (emptyCell >> 5)*.5-7.75;
const exact = c.circleHitsObstacle;
let exactCalls = 0;
c.circleHitsObstacle = (...args) => { exactCalls++; return exact(...args); };
assert.equal(tiny(emptyX, emptyZ), false);
assert.equal(exactCalls, 0, "empty cell skips the whole exact query");
c.crateGridReady = false;
tiny(emptyX, emptyZ);
c.crateGridReady = true; c.tinyCircleArena = -1;
tiny(emptyX, emptyZ);
assert.equal(exactCalls, 2, "not-ready tables preserve the exact fallback");
console.log("treadline tiny queries: " + JSON.stringify({checks, positives,
  generatedReplacements: 100, staleReplacementNegativeControls: staleNegatives,
  omittedMemberNegativeControl: true, fixedStorageBytes: storage.byteLength}));
