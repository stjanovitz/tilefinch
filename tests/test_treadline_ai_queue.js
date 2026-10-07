"use strict";

// Integer work-count tests for the actual authored queue, not host timings.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
// The queue lives in bots.js; MAX_TANKS in game.js.
const arena = path.join(__dirname, "../examples/treadline-arena");
const source = fs.readFileSync(process.argv[2] || path.join(arena, "game.js"), "utf8")
  + fs.readFileSync(process.argv[3] || path.join(arena, "bots.js"), "utf8");
const maximum = Number(source.match(/const MAX_TANKS = (\d+)/)[1]);
assert.equal(maximum, 6);
assert.equal(maximum % 2, 0, "parity queues require an even actor limit");
const begin = source.indexOf("  const botPlanPending =");
const end = source.indexOf("  function updateBotCommand(tank, dt) {", begin);
assert(begin >= 0 && end > begin, "bounded planning queue is present");
const helper = source.slice(begin, end);
const cancellation = source.match(/if \(targetRank === Infinity\) \{\n([^\n]*botPlanPending[^\n]*)/);
assert(cancellation, "no-live-target return cancels pending work");

function context() {
  const tanks = Array.from({length: maximum}, (_, id) => ({id, active: true}));
  const scope = vm.createContext({state: {frames: 0}, tanks, MAX_TANKS: maximum,
    playerIsBot: () => true, qualificationLongSoak: false,
    online: {active: false}});
  vm.runInContext(helper + "\nthis.admit=botPlanAdmit;"
    + "this.pending=id=>{botPlanPending[0]|=1<<id;botPlanPending[1]|=1<<id};"
    + "this.cancel=id=>{const tank={id};" + cancellation[1] + "};", scope);
  return scope;
}

function workload(saturated, burst) {
  const scope = context();
  const deadlines = Array.from({length: maximum}, () => [0, 0]);
  const requested = Array.from({length: maximum}, () => [-1, -1]);
  const last = Array.from({length: maximum}, () => [-1, -1]);
  const totals = Array.from({length: maximum}, () => [0, 0]);
  let maximumWait = 0, maximumInterval = 0, total = 0;
  for (let frame = 0; frame < 600; frame++) {
    scope.state.frames = frame;
    if (burst && frame === 300) deadlines.forEach(row => row.fill(0));
    let admitted = 0;
    for (let id = 0; id < maximum; id++) {
      if ((frame + id) & 1) continue;
      let actorJobs = 0;
      for (let kind = 0; kind < 2; kind++) {
        if (!saturated && frame < deadlines[id][kind]) continue;
        if (requested[id][kind] < 0) requested[id][kind] = frame;
        if (!scope.admit(scope.tanks[id], kind)) continue;
        assert.equal(actorJobs++, 0, "ordinary jobs do not coincide on one bot");
        admitted++; totals[id][kind]++;
        maximumWait = Math.max(maximumWait, frame - requested[id][kind]);
        if (last[id][kind] >= 0)
          maximumInterval = Math.max(maximumInterval, frame - last[id][kind]);
        requested[id][kind] = -1; last[id][kind] = frame;
        // Existing now+.25 deadline at the 15Hz command cadence: eight frames.
        deadlines[id][kind] = frame + 8;
      }
    }
    assert(admitted <= 2, "at most one ordinary job per kind per frame");
    total += admitted;
  }
  assert(maximumWait <= (burst ? 6 : 4),
    "saturated queues serve within four frames; epoch bursts within six");
  for (const row of last) for (const frame of row)
    assert(599 - frame <= 8, "no continuously requesting actor starves");
  if (!saturated && !burst) for (const row of totals)
    assert.deepEqual(row, [75, 75], "periodic work frequency is retained");
  return {maximumWait, maximumInterval, total, totals};
}

const ordinary = workload(false, false);
const geometryBurst = workload(false, true);
const saturated = workload(true, false);

// The symmetric full-load case alone misses cross-kind starvation: a lone
// always-due bot must not repeatedly win strategy while its bank plan waits.
const asymmetric = context(), kinds = [0, 0];
for (let frame = 0; frame < 120; frame += 2) {
  asymmetric.state.frames = frame;
  for (let kind = 0; kind < 2; kind++)
    kinds[kind] += asymmetric.admit(asymmetric.tanks[2], kind);
}
assert.deepEqual(kinds, [30, 30]);

for (const change of ["inactive", "player", "no-target"]) {
  const scope = context();
  scope.pending(0);
  if (change === "inactive") scope.tanks[0].active = false;
  if (change === "player") scope.playerIsBot = () => false;
  if (change === "no-target") scope.cancel(0);
  let jobs = 0;
  for (let frame = 0; frame < 12; frame++) {
    scope.state.frames = frame;
    for (let id = 1; id < maximum; id++)
      if (!((frame + id) & 1)) jobs += scope.admit(scope.tanks[id], 0);
  }
  assert(jobs >= 10, change + " must not leave a pending ghost");
}

// Game behaviour (safety bypasses, bank invalidation, refresh cadence) is
// exercised by the conformance fixtures and the bot league.
console.log(JSON.stringify({ordinary, geometryBurst, saturated, asymmetricKinds: kinds,
  cancellation: "passed", scope: "integer work/fairness only, no device timing"}));
