"use strict";

// Actual authored decision/planning functions with deterministic world seams.
// Retaining a bank plan during windup intentionally changes intermediate aim
// for moving targets. This tests cadence and fresh-shot safety, not identical
// full simulations, rendered pixels, or host-time predictions for the PSP.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const args = process.argv.slice(2);
const mutant = args.includes("--mutant-no-final");
assert(args.every(arg => !arg.startsWith("--") || arg === "--mutant-no-final"));
const sourcePath = args.find(arg => !arg.startsWith("--"))
  || path.join(__dirname, "../examples/treadline-arena/game.js");
// The decision functions live in bots.js beside game.js (wrapAngle in game.js).
const source = fs.readFileSync(sourcePath, "utf8")
  + fs.readFileSync(path.join(path.dirname(sourcePath), "bots.js"), "utf8");

function authoredFunction(name) {
  const start = source.indexOf("  function " + name + "(");
  const end = source.indexOf("\n  }", start);
  assert(start >= 0 && end > start, "authored function: " + name);
  return source.slice(start, end + 4);
}
const tuningStart = source.indexOf("  const BOT_REACTION =");
const tuningEnd = source.indexOf("\n",
  source.indexOf("  const BOT_STANDOFF_RELEASE_SQUARED =", tuningStart)) + 1;
assert(tuningStart >= 0 && tuningEnd > tuningStart);
const tuning = source.slice(tuningStart, tuningEnd);
// game.js's game-mode names, as declared.
const modesStart = source.indexOf("  const MODE_SURVIVAL =");
const modes = source.slice(modesStart, source.indexOf(";\n", modesStart) + 2);
const update = authoredFunction("updateBotCommand");
const legacy = update.replace(
  "(tank.aiThink <= dt && tank.fireWindup <= 0)", "tank.aiThink <= dt");
const finalUrgency = "|| (tank.fireWindup > 0 && tank.fireWindup <= dt)";
assert.equal(update.split(finalUrgency).length, 2, "one fresh final-shot check");
const missingFinal = update.replace(finalUrgency, "");
const tested = mutant ? missingFinal : update;
const bank = authoredFunction("planBankShot")
  .replace("function planBankShot(", "function actualPlanBankShot(")
  .replace("for (let side = 0; side < 4; side++) {",
    "for (let side = 0; side < 4; side++) { counts.faces++;");

function run(decision, scenario) {
  const scope = vm.createContext({scenario});
  vm.runInContext(`
    ${tuning}
    ${modes}
    const MAX_TANKS=2, MAX_PICKUPS=0, MAX_BARRIERS=0, DAILY_DIFFICULTY=1;
    // game.js's shared arena, crate and bot-fire constants and volley wait.
    const ARENA_EDGE=7.85, HULL_EDGE=7.65, CRATE_HALF=.22;
    const BOT_FIRE_RANGE_SQUARED=67.24, BOT_VOLLEY_GAP=.25;
    ${authoredFunction("botVolleyDelay")}
    const state={frames:1,time:0,gameMode:0,difficultyChoice:1,arena:0};
    const online={active:false}, convoy={active:false}, pickups=[], barriers=[];
    let qualificationAIActive=false, navEpoch=1, smokeCount=0;
    let lastPlayerShot=-99;
    // game.js lends bots.js these reassigned values as getters.
    const activeSmokeCount=()=>smokeCount, playerIsBot=()=>false,
      qualificationStats=()=>null, lastBotPlayerShotAt=()=>lastPlayerShot;
    const qualificationLongSoak=false;
    let routeDisconnected=false;
    const botPlanPending=[0,0], botPlanCursor=[0,1,2,3];
    let botPlanFrame=-1,botPlanUsed=0,botPlanTaken=0,botPlanBots=0,botPlanKinds=0;
    const bankAim=new Float32Array(4);
    let blocked=false, smoke=false;
    const counts={plans:0,faces:0,wallCalls:0,random:0,telegraphs:0};
    const target={id:0,team:0,active:true,x:2,z:2,health:100,maxHealth:100,
      velocityX:0,velocityZ:0,turretSine:0,turretCosine:1};
    const tank={id:1,team:1,active:true,x:0,z:0,health:100,maxHealth:100,
      command:{},targetChoice:0,targetNext:999,targetEpoch:1,target:0,
      strategyTarget:0,strategyMood:0,strategyNext:999,strategyEpoch:1,
      strategyGoalX:2,strategyGoalZ:2,strategySteerX:2,strategySteerZ:2,
      strategyConvoy:false,role:'HUNTER',backingOff:false,standoff:false,
      yaw:0,avoidTime:0,difficulty:1,aimRefresh:999,aimErrorX:0,aimErrorZ:0,
      bankTarget:0,bankRevision:1,bankNext:.25,bankAim:true,
      bankMirrorX:Math.fround(13.7),bankMirrorZ:2,
      bankHitX:7.85,bankHitZ:7.85*2/13.7,bankTargetX:2,bankTargetZ:2,
      fireWindup:.5,aiThink:0,turret:Math.atan2(Math.fround(13.7),2),
      fireSecondaryArmed:false,fireTelegraphed:false,aggression:1,
      secondaryCooldown:999,gadgetCooldown:999};
    const tanks=[target,tank];
    function playerTank(){return target;}
    function steerBotRoute(){return false;}
    function elevatedFiringOrigin(){return false;}
    function isOnslaught(){return false;}
    function random(){counts.random++;return .125;}
    ${authoredFunction("wrapAngle")}
    // Block the direct ray, but admit the actual planner's reflected legs.
    // Explicit obstacle/smoke transitions below can revoke those legs.
    function lineCrossesWalls(x,z,xx,zz){
      counts.wallCalls++;
      return blocked || (x===tank.x && z===tank.z
        && xx===target.x && zz===target.z);
    }
    function lineCrossesSmoke(){return smoke;}
    const sounds={telegraph(){counts.telegraphs++;}};
    ${authoredFunction("botDifficulty")}
    ${authoredFunction("botDifficultyValue")}
    ${authoredFunction("beginBotFireWindup")}
    ${authoredFunction("botPlanAdmit")}
    ${bank}
    function planBankShot(...args){counts.plans++;return actualPlanBankShot(...args);}
    ${decision}
    const dt=2/30, rows=[];
    if(scenario==='reaction-start')tank.fireWindup=0;
    for(let tick=0;tick<(scenario==='volley-delay'?12:8);tick++){
      state.frames=1+tick*2;state.time=tick*dt;
      if(scenario==='moving-target')target.x=2+tick*.15;
      if(scenario==='final-block' && tick===7)blocked=true;
      if(scenario==='smoke-cancel' && tick===2){smoke=true;smokeCount=1;}
      if(scenario==='revision-block' && tick===2){blocked=true;navEpoch++;}
      if(scenario==='volley-delay' && tick===7)lastPlayerShot=state.time;
      const beforePlans=counts.plans, beforeWindup=tank.fireWindup;
      updateBotCommand(tank,dt);
      rows.push({tick,time:state.time,beforeWindup,plans:counts.plans-beforePlans,
        windup:tank.fireWindup,think:tank.aiThink,fire:tank.command.fire,
        secondary:tank.command.secondary,left:tank.command.left,right:tank.command.right,
        bank:tank.bankAim,aimX:tank.command.aimX,aimZ:tank.command.aimZ,
        random:counts.random,telegraphs:counts.telegraphs});
    }
    globalThis.result={counts,rows};
  `, scope);
  return JSON.parse(JSON.stringify(scope.result));
}

function validateFinalDenial(result) {
  assert.equal(result.rows[7].plans, 1, "final windup must freshly plan");
  assert.equal(result.rows[7].fire, false, "newly blocked bank legs cannot fire");
  assert.equal(result.rows[7].secondary, false);
  assert.equal(result.rows[7].windup, 0, "invalid final shot cancels windup");
}
// Validate safety before count assertions so the explicit mutant fails here.
validateFinalDenial(run(tested, "final-block"));

const results = {};
for (const scenario of ["stationary", "moving-target", "final-block",
  "smoke-cancel", "revision-block", "reaction-start", "volley-delay"]) {
  const reference = run(legacy, scenario), current = run(tested, scenario);
  const timing = result => result.rows.map(row => ({windup:row.windup,
    think:row.think,fire:row.fire,secondary:row.secondary,
    random:row.random,telegraphs:row.telegraphs}));
  assert.deepEqual(timing(current), timing(reference),
    scenario + ": reaction/telegraph timing, shots, and RNG must remain unchanged");
  assert(current.counts.plans < reference.counts.plans,
    scenario + ": expired reaction must not force every windup update to plan");
  for (const row of current.rows) if (row.fire || row.secondary)
    assert.equal(row.plans, 1, "every final shot must use a fresh bank plan");
  results[scenario] = {referencePlans:reference.counts.plans,
    plans:current.counts.plans,faces:current.counts.faces,
    planTicks:current.rows.filter(row => row.plans).map(row => row.tick),
    shotTicks:current.rows.filter(row => row.fire || row.secondary).map(row => row.tick)};
  if (scenario === "stationary") {
    assert.equal(reference.counts.plans, 8);
    assert.equal(current.counts.plans, 2);
    assert.deepEqual(results[scenario].planTicks, [4,7]);
    assert.deepEqual(current.rows.map(({plans,...row})=>row),
      reference.rows.map(({plans,...row})=>row));
  } else if (scenario === "moving-target") {
    assert.equal(current.counts.plans, 2);
    assert.notEqual(current.rows[1].aimX, reference.rows[1].aimX,
      "intermediate aiming intentionally follows retained-plan cadence");
    assert.equal(current.rows[1].aimX, current.rows[0].aimX);
    for (const tick of [4,7]) {
      assert.equal(current.rows[tick].aimX, reference.rows[tick].aimX);
      assert.equal(current.rows[tick].aimZ, reference.rows[tick].aimZ);
    }
  } else if (scenario === "smoke-cancel" || scenario === "revision-block") {
    assert.equal(current.rows[2].windup, 0);
    assert.equal(current.rows[2].bank, false);
    assert.equal(current.rows[2].fire, false);
  } else if (scenario === "reaction-start") {
    assert.equal(current.counts.plans, 3);
    assert.equal(current.counts.telegraphs, 1);
    assert.equal(current.counts.random, 2);
    assert.equal(current.rows[0].plans, 1);
    assert.equal(current.rows[0].windup, .42 + .025);
    assert.equal(current.rows[0].think, .2 + .125 * .16);
    assert.deepEqual(results[scenario].shotTicks, [7]);
  } else if (scenario === "volley-delay") {
    assert.equal(current.rows[7].plans, 1);
    assert(current.rows[7].windup > 0);
    assert.equal(current.rows[7].fire, false);
    assert.deepEqual(results[scenario].shotTicks, [11]);
    assert.equal(current.rows[11].plans, 1);
  }
}
assert.throws(() => validateFinalDenial(run(missingFinal, "final-block")),
  assert.AssertionError, "negative control must detect missing final urgency");
console.log(JSON.stringify({results,negativeControl:"passed",
  scope:"Actual decision functions with deterministic world seams; retained intermediate aim is intentional, not exact simulation or device timing."}));
