"use strict";

// Exercise the actual emitters against the original three-box specification.
// Work counts are deterministic; this is not a host-timing PSP prediction.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
function section(begin, end) {
  const start = source.indexOf(begin), finish = source.indexOf(end, start);
  assert(start >= 0 && finish > start);
  return source.slice(start, finish);
}
const boxes = section("  const BOX_VERTICES =", "  const MAX_INSTANCES_PER_DRAW");
const emitter = section("  function addBox(", "  function addOptionalBox(");
const marker = section("  function addObjectiveMarker(", "  function buildDynamicScene(");
function make(candidate) {
  let writes = 0, calls = 0;
  const matrices = new Float32Array(64 * 16), tints = new Float32Array(64 * 4);
  for (let at = 0; at < 64; at++) matrices[at * 16 + 15] = 1;
  const count = array => new Proxy(array, {
    set(target, key, value) { writes++; target[key] = value; return true; },
  });
  const context = vm.createContext({collectInstancedBoxes: true, boxInstanceProgram: {},
    frameInstanceCeiling: 64, boxInstanceCount: 0, retainedObjectiveFirst: -1,
    boxInstanceMatrices: count(matrices),
    boxInstanceTints: count(tints), boxInstanceSlotOwners: new Int16Array(64).fill(-1),
    COLORS: {pickup: [.23, .84, .47], bullet: [1, .82, .28]},
    lastBoxYaw: NaN, lastBoxSine: 0, lastBoxCosine: 1,
    vertexCount: 0, indexCount: 0, MAX_VERTICES: 1024, MAX_INDICES: 2048,
    positions: new Float32Array(1024 * 3), colors: new Float32Array(1024 * 4),
    indices: new Uint16Array(2048), meshDrops: 0, instanceCapHitThisFrame: false,
    call: () => calls++});
  vm.runInContext(boxes + emitter + (candidate ? marker : `
    function addObjectiveMarker(x, z) {
      addBox(x,.045,z,.46,.09,.46,0,COLORS.pickup,.72);
      addBox(x,.57,z,.12,1.05,.12,0,COLORS.pickup,1);
      addBox(x,1.12,z,.28,.12,.28,0,COLORS.bullet,1);
    }`) + `
    const originalBox = addBox;
    addBox = (...args) => { call(); return originalBox(...args); };
    this.emit = addObjectiveMarker;
    this.overwrite = () => addBox(1,2,3,.4,.6,.8,.5,[.4,.5,.6],.7);
  `, context);
  return {context, matrices, tints, work: () => ({writes, calls})};
}
const reference = make(false), candidate = make(true);
let cases = 0;
function run(first, cap, x, z, instanced = true) {
  for (const fixture of [reference, candidate]) {
    const c = fixture.context;
    c.collectInstancedBoxes = instanced; c.boxInstanceCount = first;
    c.frameInstanceCeiling = cap; c.meshDrops = 0; c.instanceCapHitThisFrame = false;
    c.vertexCount = c.indexCount = 0;
    c.emit(x, z);
  }
  for (const field of ["boxInstanceCount", "meshDrops", "instanceCapHitThisFrame",
    "vertexCount", "indexCount"])
    assert.equal(candidate.context[field], reference.context[field], field);
  for (const field of ["matrices", "tints"])
    assert.deepEqual(candidate[field], reference[field], field);
  for (const field of ["positions", "colors", "indices"])
    assert.deepEqual(candidate.context[field], reference.context[field], field);
  cases++;
}
run(10, 64, 1, -2);
const cold = candidate.work(), original = reference.work();
run(10, 64, -3.1, 4.8);
assert.equal(candidate.work().writes - cold.writes, 6);
assert.equal(reference.work().writes - original.writes, 36);
assert.equal(candidate.work().calls - cold.calls, 0);
assert.equal(reference.work().calls - original.calls, 3);
// Two adjacent old marker ranges must not look like one shifted live marker.
run(13, 64, 2, 3); run(11, 64, 4, 5);

for (let frame = 0; frame < 2048; frame++) {
  const first = (frame * 7) % 64;
  const cap = frame % 9 === 0 ? first + frame % 3 : 64;
  const x = ((frame * 13) % 311 - 155) / 20;
  const z = ((frame * 17) % 307 - 153) / 20;
  run(first, cap, x, z);
  run(first, cap, x + .03, z - .02);
  if (frame % 11 === 0) for (const fixture of [reference, candidate]) {
    fixture.context.boxInstanceCount = first;
    fixture.context.frameInstanceCeiling = 64;
    fixture.context.overwrite();
  }
  if (frame % 13 === 0) for (const fixture of [reference, candidate]) {
    // Killcam exit/scene rebuild invalidates physical ownership before use.
    fixture.context.boxInstanceSlotOwners.fill(-1);
    fixture.matrices.fill(0); fixture.tints.fill(0);
  }
  run(first, 64, x, z);
}
run(0, 64, .2, -.3, false);
run(0, 64, -.7, .9, false);
console.log(JSON.stringify({cases, warmWordWrites: [36, 6], warmBoxCalls: [3, 0]}));
