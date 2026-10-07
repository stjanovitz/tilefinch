"use strict";

// Compare the actual lifetime-owned emitter with the original addOptionalBox.
// Counts are deterministic work, not a prediction of device milliseconds.
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path"), vm = require("node:vm");
const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
const particleLimit = Number(source.match(/\bMAX_PARTICLES = (\d+)[,;]/)[1]);
const decalLimit = Number(source.match(/const MAX_DECALS = (\d+);/)[1]);
function section(begin, end) {
  const first = source.indexOf(begin), last = source.indexOf(end, first);
  assert(first >= 0 && last > first, begin);
  return source.slice(first, last);
}
const definitions = section("  const BOX_VERTICES =", "  const MAX_INSTANCES_PER_DRAW");
const boxes = section("  function addBox(", "  function addRetainedTankBox(");
const helper = section("  function addRetainedEffectBox(", "  function addRetainedTankBox(");
const pool = section("  const particles =", "  let decalCursor =");
const lifecycle = section("  function clearEffects(", "  function activeMaskIndex(");
const colors = section("  const COLORS =", "  const CONTROL_BLUE =");
const sparks = section("  const SPARK_STEPS =", "  function buildDynamicScene(");
const particleReset = "    if (active) retainedEffectSlots[at] = -1;";
const decalReset = "    retainedEffectSlots[MAX_PARTICLES + decalCursor] = -1;";
assert.equal((source.match(/addRetainedEffectBox\(at, false,/g) || []).length, 2);
assert.equal((source.match(/addRetainedEffectBox\(MAX_PARTICLES \+ at, true,/g) || []).length, 1);
const killcam = section("  function advanceKillcam(", "  function updateBullets(");
const invalidation = killcam.indexOf("boxInstanceSlotOwners.fill(-1)");
assert(invalidation >= 0 && invalidation < killcam.indexOf("finishPlayerDeath();"));
const spawn = section("  function spawnParticles(", "  function spawnShell(");
assert(spawn.indexOf("setParticleActive(particleAt, true)")
  < spawn.indexOf("particle.renderSine ="));
// These arrays, unlike tank.renderTint, are literal read-only inputs. Pin the
// current effect call sites and all basis writers; a new mutation needs an
// explicit lifetime invalidation or a different emitter before this can pass.
for (const name of ["particle", "tread", "shadow"])
  assert(!new RegExp("COLORS\\." + name + "(?:\\s*\\[[^\\]]*\\])?\\s*=(?!=)").test(source));
for (const field of ["renderSine", "renderCosine", "renderStreak"])
  assert.equal((source.match(new RegExp("particle\\." + field + "\\s*=", "g")) || []).length, 1);
for (const field of ["kind", "x", "y", "z", "yaw", "width", "depth", "sine", "cosine"])
  assert.equal((source.match(new RegExp("decal\\." + field + "\\s*=(?!=)", "g")) || []).length, 1);
const effects = section("    /* Pressure discards oldest decals", "    if (instanceCapHitThisFrame) instanceCapHitFrames++;");
// Sparks pass a cooling tint from SPARK_TINTS (a fixed table) instead.
assert.equal((effects.match(
  /SPARK_TINTS\[\(heat \* SPARK_STEPS\) \| 0\], heat < \.625 \? heat \* 1\.6 : 1/g)
  || []).length, 2);

const originalHelper = `function addRetainedEffectBox(owner, stationary,
  x,y,z,width,height,depth,yaw,color,alpha,sine,cosine) {
  return addOptionalBox(x,y,z,width,height,depth,yaw,color,alpha,sine,cosine);
}`;
function make(reference, broken = "") {
  const work = {matrixWrites: 0, tintWrites: 0, finiteChecks: 0};
  const arrays = {};
  function floats(name, count, fill) {
    const array = arrays[name] = new Float32Array(count).fill(fill);
    return new Proxy(array, {
      get(target, key) { return Reflect.get(target, key, target); },
      set(target, key, value) {
        if (/^\d+$/.test(String(key))) work[name === "matrix" ? "matrixWrites" : "tintWrites"]++;
        return Reflect.set(target, key, value, target);
      },
    });
  }
  const context = vm.createContext({
    Number: {isFinite(value) { work.finiteChecks++; return Number.isFinite(value); }},
    boxInstanceMatrices: floats("matrix", 1024, .123),
    boxInstanceTints: floats("tint", 256, .456),
    boxInstanceSlotOwners: new Int16Array(64).fill(-1),
    collectInstancedBoxes: true, boxInstanceProgram: {}, boxInstanceCount: 0,
    frameInstanceCeiling: 64, optionalInstanceCeiling: 64,
    lastBoxYaw: NaN, lastBoxCosine: 1, lastBoxSine: 0,
    meshDrops: 0, instanceCapHitThisFrame: false, vertexCount: 0, indexCount: 0,
    MAX_VERTICES: 1024, MAX_INDICES: 2048, MAX_PARTICLES: particleLimit,
    positions: new Float32Array(3072), colors: new Float32Array(4096),
    indices: new Uint16Array(2048), particleActiveLowMask: 0, particleActiveHighMask: 0,
    decalCursor: 0, decalRecycles: 0,
    // Destroyed-tank scorches queued for a later frame (none here).
    wreckCount: 0, wreckWaits: new Float32Array(4), wreckPoses: new Float32Array(12),
    directionSines: Float32Array.from({length: 512}, (_, i) => Math.sin(i * Math.PI / 256)),
    directionCosines: Float32Array.from({length: 512}, (_, i) => Math.cos(i * Math.PI / 256)),
    orientationIndex: yaw => (Math.round(yaw * 256 / Math.PI) & 511),
    surfaceHeightAt: (x, z) => .2 + x * .03125 + z * .015625,
    // spawnDecal's ground-mark levels also consult hazards (none here).
    hazards: [],
  });
  let body = boxes;
  if (reference) body = body.replace(helper, originalHelper);
  let life = lifecycle;
  if (broken === "particle-reset") life = life.replace(particleReset, "");
  if (broken === "decal-reset") life = life.replace(decalReset, "");
  // A negative control must really remove the invalidation it names.
  assert(!broken.endsWith("-reset") || life !== lifecycle, broken + " anchor");
  if (broken === "owner") body = body.replace(
    /&&\s*boxInstanceSlotOwners\[instance\]\s*===\s*encoded/, "");
  if (broken === "disabled") body = body.replace(
    /retainedEffectSlots\[owner\]\s*===\s*instance/, "false");
  vm.runInContext(definitions + colors + sparks + pool + life + body + `
    for (const name of ['particle','tread','shadow']) Object.freeze(COLORS[name]);
    this.sparkTints=SPARK_TINTS;
    this.emit=addRetainedEffectBox; this.generic=addOptionalBox;
    this.activate=setParticleActive; this.spawn=spawnDecal; this.expire=updateDecals;
    this.clear=clearEffects;
    this.slots=retainedEffectSlots; this.particles=particles; this.decals=decals;
    this.palette=COLORS; this.flatStep=FLAT_STEP;
  `, context);
  return {context, work, arrays};
}
const reference = make(true), candidate = make(false), pair = [reference, candidate];
const scalarFields = ["boxInstanceCount", "meshDrops", "instanceCapHitThisFrame",
  "vertexCount", "indexCount", "lastBoxYaw", "lastBoxCosine", "lastBoxSine",
  "particleActiveLowMask", "particleActiveHighMask", "decalCursor", "decalRecycles"];
let comparisons = 0;
function compare(a = reference, b = candidate) {
  for (const field of scalarFields) assert.deepEqual(b.context[field], a.context[field], field);
  for (const field of ["matrix", "tint"])
    assert(Buffer.from(b.arrays[field].buffer).equals(Buffer.from(a.arrays[field].buffer)), field);
  for (const field of ["positions", "colors", "indices"])
    assert(Buffer.from(b.context[field].buffer).equals(Buffer.from(a.context[field].buffer)), field);
  comparisons++;
}
function begin(first = 0, cap = 64, optional = cap, instanced = true, program = true) {
  for (const f of pair) Object.assign(f.context, {boxInstanceCount: first,
    frameInstanceCeiling: cap, optionalInstanceCeiling: optional,
    collectInstancedBoxes: instanced, boxInstanceProgram: program ? {} : null,
    vertexCount: 0, indexCount: 0, meshDrops: 0, instanceCapHitThisFrame: false});
}
function args(owner, frame = 0) {
  const c = reference.context, stationary = owner >= particleLimit;
  const p = stationary ? c.decals[owner - particleLimit] : c.particles[owner];
  return [owner, stationary, p.x, stationary ? p.y : Math.max(.04, p.y), p.z,
    stationary ? p.width : .045, stationary ? c.flatStep : .045,
    stationary ? p.depth : p.renderStreak, stationary ? p.yaw : 0,
    // A moving spark cools: its tint changes while its slot stays warm.
    stationary ? (p.kind === 1 ? c.palette.tread : c.palette.shadow)
      : c.sparkTints[(c.sparkTints.length - 1) - frame % c.sparkTints.length],
    frame % 31 / 31, stationary ? p.sine : p.renderSine,
    stationary ? p.cosine : p.renderCosine];
}
function emit(owner, frame) {
  const a = args(owner, frame);
  assert.equal(candidate.context.emit(...a), reference.context.emit(...a)); compare();
}
function particle(owner, life) {
  for (const f of pair) {
    f.context.activate(owner, true);
    Object.assign(f.context.particles[owner], {x: owner * .01, y: .3, z: -1,
      renderSine: Math.sin(life * .7), renderCosine: Math.cos(life * .7),
      renderStreak: .12 + life % 7 * .03});
  }
}
for (let i = 0; i < particleLimit; i++) particle(i, i);
for (let i = 0; i < 12; i++) for (const f of pair) f.context.spawn(i & 1 ? 1 : 2, i * .3, -i * .2, i * .17);
for (let frame = 0; frame < 1200; frame++) {
  if (frame % 7 === 0) particle(frame % particleLimit, frame);
  if (frame % 11 === 0) for (const f of pair) f.context.spawn(frame & 1 ? 1 : 2, frame * .001, -1, frame * .03, .3 + frame % 5 * .02, .2);
  // Cold prefix, compaction, saturated admission, expiry, pool wrap and moving
  // particle translation all share the same deterministic workload.
  const first = Math.floor(frame / 13) % 58, cap = frame % 19 === 0 ? 48 : 64;
  begin(first, cap, frame % 17 === 0 ? first : cap);
  if (frame % 23 === 0) for (const f of pair) {
    f.context.generic(3,.2,1,.7,.4,.9,0,f.context.palette.shadow,.9,0,1);
  }
  for (let i = 0; i < decalLimit; i++) emit(particleLimit + (i + Math.floor(frame / 71)) % decalLimit, frame);
  for (let i = 0; i < 6; i++) {
    const owner = (i + Math.floor(frame / 97)) % particleLimit;
    for (const f of pair) {f.context.particles[owner].x += .007; f.context.particles[owner].y -= .001;}
    emit(owner, frame);
  }
  if (frame % 101 === 0) {
    // Killcam restores different render history; its normal exit invalidates
    // physical ownership before either redeploy or game-over rendering.
    for (const f of pair) {
      f.arrays.matrix.fill(.876); f.arrays.tint.fill(.234);
      f.context.boxInstanceSlotOwners.fill(-1);
    }
  }
  if (frame % 131 === 0) for (const f of pair) {
    for (let i=0;i<particleLimit;i++) f.context.activate(i,false);
    f.context.expire(100);
  }
}
// Fallbacks retain the complete non-instanced geometry and refusal behavior.
for (const instanced of [false, true]) for (const program of [false, true])
  for (const first of [0, 63, 64]) for (const cap of [0, 32, 64])
    for (const optional of [0, 31, 64]) {
      begin(first, cap, optional, instanced, program); emit(3, 0); emit(particleLimit, 1);
    }
// Unsupported owners and cold nonfinite bases use the original yaw path.
for (const owner of [-1, particleLimit + decalLimit]) for (const basis of [[NaN, 1], [0, Infinity], [.6,.8]]) {
  begin(); const a=args(2,2);a[0]=owner;a[8]=.7;a[11]=basis[0];a[12]=basis[1];
  assert.equal(candidate.context.emit(...a),reference.context.emit(...a));compare();
}
// A stable effect must now eliminate work, not merely produce equal output.
// Frames 1 and 1 + steps share a spark tint (a steady cooling step); the
// next frame changes step and must rewrite exactly the three colour channels.
const steps = reference.context.sparkTints.length;
particle(0, 17); begin(); emit(0, 1);
const before = pair.map(f => ({...f.work}));
begin(); emit(0, 1 + steps);
assert.equal(reference.work.matrixWrites-before[0].matrixWrites, 8);
assert.equal(reference.work.tintWrites-before[0].tintWrites, 4);
function particleWarmBudget(fixture, prior, tintWrites = 1) {
  assert.equal(fixture.work.matrixWrites-prior.matrixWrites, 3, "warm particle matrix writes");
  assert.equal(fixture.work.tintWrites-prior.tintWrites, tintWrites, "warm particle tint writes");
  assert.equal(fixture.work.finiteChecks-prior.finiteChecks, 0, "warm particle basis reads");
}
particleWarmBudget(candidate, before[1]);
const cooling = pair.map(f => ({...f.work}));
begin(); emit(0, 2 + steps);
particleWarmBudget(candidate, cooling[1], 4);
begin(); emit(particleLimit, 1);
const decalBefore = pair.map(f => ({...f.work}));
begin(); emit(particleLimit, 2);
assert.equal(reference.work.matrixWrites-decalBefore[0].matrixWrites, 8);
assert.equal(candidate.work.matrixWrites-decalBefore[1].matrixWrites, 0);
assert.equal(reference.work.tintWrites-decalBefore[0].tintWrites, 4);
assert.equal(candidate.work.tintWrites-decalBefore[1].tintWrites, 1);
assert.equal(candidate.work.finiteChecks-decalBefore[1].finiteChecks, 0);

// Removing either lifetime reset or physical-owner check must cause a real
// output mismatch, not only a source-string assertion.
for (const broken of ["particle-reset", "decal-reset", "owner"]) {
  const old = make(true), bad = make(false, broken), fixtures=[old,bad];
  const owner=broken==='decal-reset'?particleLimit:0;
  for (const f of fixtures) {
    f.context.activate(0,true);f.context.spawn(1,0,0,0);
    f.context.emit(owner,owner>=particleLimit,0,.2,0,.2,.04,.4,0,f.context.palette.tread,.8,0,1);
    f.context.boxInstanceCount=0;
    if(broken==='particle-reset')f.context.activate(0,true);
    else if(broken==='decal-reset'){f.context.decalCursor=0;f.context.spawn(2,1,1,.5);}
    else {f.context.generic(0,1,0,.8,.8,.8,0,f.context.palette.shadow,1,0,1);f.context.boxInstanceCount=0;}
    f.context.emit(owner,owner>=particleLimit,1,.3,1,.7,.1,.8,.5,f.context.palette.shadow,.4,.6,.8);
  }
  assert.throws(()=>compare(old,bad),/matrix|tint/,broken+' must be observable');
}
const disabled = make(false, "disabled");
const sameArgs = [0,false,1,.2,3,.045,.045,.3,0,[1,.38,.1],.5,.6,.8];
disabled.context.emit(...sameArgs);
disabled.context.boxInstanceCount = 0;
const disabledBefore = {...disabled.work};
disabled.context.emit(...sameArgs);
assert.throws(() => particleWarmBudget(disabled, disabledBefore), /warm particle matrix writes/,
  "disabling retention preserves pixels but must fail the work budget");
// A new arena (beginArena, which replays and online starts share) ends every
// effect lifetime: nothing stays active and a respawned particle is fresh.
{
  const c = candidate.context;
  for (let at = 0; at < 6; at++) c.activate(at, true);
  for (let at = 0; at < 3; at++) c.spawn(1, at, at, .2);
  c.clear();
  assert(c.particles.every((particle) => !particle.active)
    && c.decals.every((decal) => !decal.active) && c.decalCursor === 0
    && c.particleActiveLowMask === 0 && c.particleActiveHighMask === 0,
    "an arena start clears every effect");
  c.slots[0] = 3; c.activate(0, true);
  assert.equal(c.slots[0], -1, "a particle activated after the clear is a new lifetime");
}
console.log(JSON.stringify({comparisons,reference:reference.work,candidate:candidate.work,
  negativeControls:4,metadataBytes:particleLimit + decalLimit}));
