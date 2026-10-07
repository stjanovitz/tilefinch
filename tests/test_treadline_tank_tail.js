"use strict";

// Compare the actual emitter with the same emitter before its optional-tail
// rejection. This measures eliminated work, not a host-timing PSP prediction.
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
function section(begin, end) {
  const start = source.indexOf(begin), finish = source.indexOf(end, start);
  assert(start >= 0 && finish > start, begin);
  return source.slice(start, finish);
}
const definitions = section("  const BOX_VERTICES =", "  const MAX_INSTANCES_PER_DRAW");
const boxes = section("  function addBox(", "  function initializeBoxInstancing(");
const tankSource = section("  function addTank(", "  function addRetainedArenaScenery(");
const guard = /    \/\/ OPTIONAL_TANK_TAIL_REJECTION_BEGIN[\s\S]*?    \/\/ OPTIONAL_TANK_TAIL_REJECTION_END\n/;
assert(guard.test(tankSource), "candidate has one separately testable admission guard");
const referenceSource = tankSource.replace(guard, "");
const colors = {
  shadow: [.01, .015, .015], tread: [.035, .045, .048],
  muzzle: [1, .96, .56], wall: [.145, .315, .31], pickup: [.08, 1, .82],
  barrel: [.12, .14, .13], health: [.2, 1, .4], bullet: [1, .82, .12],
  healthLost: [.28, .1, .08],
};
const palette = Array.from({length: 6}, (_, at) => [at / 8, 1 - at / 9, .3 + at / 10]);
function make(body) {
  const work = {retainedCalls: 0, sine: 0, cosine: 0};
  const math = Object.create(Math);
  math.sin = value => { work.sine++; return Math.sin(value); };
  math.cos = value => { work.cosine++; return Math.cos(value); };
  const matrices = new Float32Array(64 * 16);
  for (let at = 0; at < 64; at++) matrices[at * 16 + 15] = 1;
  const tank = {renderTint: new Float32Array([.7, .8, .9])};
  const context = vm.createContext({Math: math, tank,
    collectInstancedBoxes: true, boxInstanceProgram: {},
    frameInstanceCeiling: 64, optionalInstanceCeiling: 64, boxInstanceCount: 0,
    boxInstanceMatrices: matrices, boxInstanceTints: new Float32Array(64 * 4),
    boxInstanceSlotOwners: new Int16Array(64).fill(-1),
    retainedTankPartSlots: new Int8Array(72).fill(-1),
    retainedTankPartState: new Array(72 * 5).fill(NaN), RETAINED_TANK_PARTS: 12,
    COLORS: colors, PAINT_COLORS: palette, TANK_COLORS: palette,
    PAINT_BASE_TINTS: palette.map(row => new Float32Array(row)),
    TANK_BASE_TINTS: palette.map(row => new Float32Array(row)),
    HEALTH_BASE_TINT: new Float32Array(colors.health), preferences: {paint: 0},
    state: {gameMode: 0, time: 1, wallTime: 2},
    MODE_DUEL: Number(source.match(/MODE_DUEL = (\d+)/)[1]),
    cameraYaw: .23, cameraSine: Math.sin(.23), cameraCosine: Math.cos(.23),
    lastBoxYaw: NaN, lastBoxSine: 0, lastBoxCosine: 1,
    vertexCount: 0, indexCount: 0, MAX_VERTICES: 1024, MAX_INDICES: 2048,
    positions: new Float32Array(1024 * 3), colors: new Float32Array(1024 * 4),
    indices: new Uint16Array(2048), meshDrops: 0, instanceCapHitThisFrame: false,
    tankBarrelCount: 0, countRetained: () => work.retainedCalls++,
    TANK_SHADOW_TOP: Number(source.match(/TANK_SHADOW_TOP = ([\d.]+),/)[1])});
  vm.runInContext(definitions + boxes + body + `
    const originalRetained = addRetainedTankBox;
    addRetainedTankBox = function() {
      countRetained(); return originalRetained.apply(null, arguments);
    };
    this.emit = addTank;
  `, context);
  return {context, work};
}
const reference = make(referenceSource), candidate = make(tankSource);
const fields = ["boxInstanceCount", "meshDrops", "instanceCapHitThisFrame",
  "tankBarrelCount", "vertexCount", "indexCount", "lastBoxYaw", "lastBoxSine",
  "lastBoxCosine", "boxInstanceMatrices", "boxInstanceTints", "boxInstanceSlotOwners",
  "retainedTankPartSlots", "retainedTankPartState", "positions", "colors", "indices", "tank"];
let cases = 0, savedCalls = 0, savedSines = 0, savedCosines = 0;
function configure(fixture, options) {
  const c = fixture.context, t = c.tank;
  const yaw = options.yaw ?? .57, turret = options.turret ?? -.81;
  Object.assign(t, {id: 0, boss: false, player: true, classId: 0,
    x: 1.2, z: -2.3, surfaceY: .15, scale: 1,
    yaw, yawSine: Math.sin(yaw), yawCosine: Math.cos(yaw),
    turret, turretSine: Math.sin(turret), turretCosine: Math.cos(turret),
    hitFlash: 0, fireWindup: 0, recoil: .1, shield: 1, repair: 1,
    health: 20, maxHealth: 100, leftTreadHealth: 1, rightTreadHealth: 1,
    turretHealth: 1}, options.tank);
  c.collectInstancedBoxes = options.instanced ?? true;
  c.boxInstanceProgram = options.program === false ? null : {};
  c.boxInstanceCount = options.first ?? 0;
  c.frameInstanceCeiling = options.cap ?? 64;
  c.optionalInstanceCeiling = options.optional ?? c.frameInstanceCeiling;
  c.state.gameMode = options.mode ?? 0;
  c.state.time = options.time ?? 1;
  c.state.wallTime = options.wallTime ?? 2;
  c.preferences.paint = options.paint ?? 0;
  c.meshDrops = options.drops ?? 0;
  c.instanceCapHitThisFrame = options.capHit ?? false;
  c.vertexCount = c.indexCount = c.tankBarrelCount = 0;
  if (options.resetOwners) {
    c.boxInstanceSlotOwners.fill(-1);
    c.retainedTankPartSlots.fill(-1);
  }
}
function run(options) {
  const before = [reference, candidate].map(f => ({...f.work}));
  for (const fixture of [reference, candidate]) {
    configure(fixture, options); fixture.context.emit(fixture.context.tank);
  }
  for (const field of fields)
    assert.deepEqual(candidate.context[field], reference.context[field],
      `${field} after ${JSON.stringify(options)}`);
  const calls = reference.work.retainedCalls - before[0].retainedCalls
    - (candidate.work.retainedCalls - before[1].retainedCalls);
  assert(calls >= 0);
  savedCalls += calls;
  savedSines += reference.work.sine - before[0].sine - (candidate.work.sine - before[1].sine);
  savedCosines += reference.work.cosine - before[0].cosine - (candidate.work.cosine - before[1].cosine);
  cases++;
  return calls;
}

assert.equal(run({first: 64, cap: 64}), 5);
assert.equal(candidate.context.meshDrops, 12); // all twelve attempted parts
assert.equal(candidate.context.tankBarrelCount, 1); // including refused barrel
assert.equal(run({first: 20, cap: 64, optional: 0}), 5);
assert.equal(candidate.context.meshDrops, 0); // optional-only refusal is not a mesh drop
assert.equal(run({first: 0, cap: 64}), 0);
assert.equal(run({first: 64, cap: 64, tank: {id: 1, player: false,
  recoil: 0, shield: 0, repair: 0}}), 0);
assert.equal(candidate.context.meshDrops, 3); // no optional attempt, only compact core
// Every part boundary, both ceilings, all classes, effect combinations, and
// detailed/non-detailed variants; reuse contexts and retain cache contents.
for (let variation = 0; variation < 48; variation++) {
  const tank = {id: variation % 6, classId: variation % 3,
    boss: variation % 5 === 0, player: variation % 6 === 0,
    shield: variation & 1, repair: (variation >> 1) & 1,
    recoil: variation & 4 ? .1 : .055,
    health: variation & 8 ? 20 : variation & 16 ? 0 : 100,
    hitFlash: variation % 4 === 0 ? .07 : 0,
    fireWindup: variation % 7 === 0 ? .2 : 0,
    scale: variation % 5 === 0 ? 1.4 : 1};
  for (let cap = 0; cap <= 14; cap++) for (let optional = 0; optional <= 14; optional++)
    run({first: 0, cap, optional, tank, mode: variation % 11 === 0 ? 6 : 0,
      paint: variation % 6, drops: 17, capHit: variation % 2 === 0,
      wallTime: variation / 11, time: variation / 7,
      resetOwners: variation % 13 === 0});
}
for (const instanced of [false, true]) for (const program of [false, true])
  for (let at = 0; at < 32; at++) run({first: at * 2, cap: at,
    optional: at >> 1, instanced, program,
    tank: {id: at % 6, boss: at % 3 === 0, player: at % 2 === 0,
      health: at & 1 ? 0 : 20, repair: at & 2 ? 1 : 0},
    wallTime: at / 13, time: at / 17});

// Deliberate corruptions prove the gate watches counters and the scratch tint,
// not merely visible geometry. Both must disagree with the reference.
for (const [from, to, field] of [
  ["meshDrops += rejected;", "meshDrops += 0;", "meshDrops"],
  ["if (pulse > 0) {", "if (false && pulse > 0) {", "tank"],
]) {
  assert(tankSource.includes(from));
  const mutant = make(tankSource.replace(from, to));
  const clean = make(referenceSource), options = {first: 64, cap: 64};
  for (const fixture of [mutant, clean]) {
    configure(fixture, options); fixture.context.emit(fixture.context.tank);
  }
  assert.notDeepEqual(mutant.context[field], clean.context[field], `${field} negative control`);
}
assert(savedCalls > 1000 && savedSines > 0 && savedCosines > 0);
console.log(JSON.stringify({cases, savedRetainedCalls: savedCalls,
  savedArgumentExpressions: savedCalls * 14, savedSines, savedCosines,
  counterAndTintNegativeControls: true}));
