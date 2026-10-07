"use strict";

// Reject unavailable optional slots before preparing their pure arguments.
// Compare actual game emitters with the original blocks, not a second guard.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = fs.readFileSync(process.argv[2]
  || path.join(__dirname, "../examples/treadline-arena/game.js"), "utf8");
function section(begin, end, from = 0) {
  const first = source.indexOf(begin, from), last = source.indexOf(end, first);
  assert(first >= 0 && last > first, begin);
  return source.slice(first, last);
}
const build = source.indexOf("  function buildDynamicScene(");
assert(build >= 0);
const guide = section("    const player = playerTank();",
  "    let activeBullets = bulletActiveMask;", build);
const effects = section("    const particleReserve = Math.min(6,",
  "    if (instanceCapHitThisFrame) instanceCapHitFrames++;", build);
/* A hand-written reference for the aim guide's block (not a historical
   copy): it updates the guide and offers its marks box by box, while the
   game rejects the whole set up front once the optional ceiling is
   reached. */
const referenceGuide = `
  const player = playerTank();
  const aimLevel = player.active ? aimGuideLevel() : AIM_OFF;
  aimGuide.boxes = 0;
  if (aimLevel !== AIM_OFF) {
    const at = profileFrame ? performance.now() : 0;
    if (updateAimGuide(player, aimLevel) && profileFrame) qualificationAITimes[20]++;
    if (aimLevel === AIM_FULL) {
      if ((aimEnemyPhase ^= 1) === 1 || aimEnemyLevel !== AIM_FULL)
        updateAimGuideEnemy(player);
    } else { aimGuide.enemy = -1; aimGuide.enemyLeg = 0; aimGuide.hidden = false; }
    aimEnemyLevel = aimLevel;
    if (profileFrame) qualificationAITimes[19] += performance.now() - at;
    addAimMark();
  }`;
const originalDecals = `
  const particleReserve = Math.min(6,
    activeMaskCount(particleActiveLowMask) + activeMaskCount(particleActiveHighMask));
  const effectsInstanceCeiling = optionalInstanceCeiling;
  optionalInstanceCeiling = Math.max(boxInstanceCount,
    optionalInstanceCeiling - particleReserve);
  for (let age = 0; age < MAX_DECALS; age++) {
    const at = (decalCursor + MAX_DECALS - 1 - age) % MAX_DECALS;
    const decal = decals[at];
    if (!decal.active) continue;
    const alpha = Math.max(0, Math.min(.58, decal.life / decal.maximum * .58));
    // A decal that cannot be published is retired, not skipped (it would
    // come back next frame and blink).
    if (!addOptionalBox(decal.x, decal.y, decal.z,
        decal.width, FLAT_STEP, decal.depth, decal.yaw,
        decal.kind === 1 ? COLORS.tread : COLORS.shadow, alpha,
        decal.sine, decal.cosine)) { droppedDecalInstances++; decal.active = false; }
  }
`;
const particles = effects.slice(effects.indexOf(
  "    optionalInstanceCeiling = effectsInstanceCeiling;"));
assert(particles.startsWith("    optionalInstanceCeiling"));
const helpers = section("  const BOX_VERTICES =", "  const MAX_INSTANCES_PER_DRAW")
  + section("  function addBox(", "  function addRetainedTankBox(")
  + section("  function activeMaskIndex(", "  const pickups =")
  + section("  const SHELL_RANGE = 0,", "  let castLength = 0,")
  + section("  const AIM_OFF = 0,", "  /* Recast only when")
  + section("  const AIM_LINE_COLOR =", "  function addAimLeg(")
  + section("  const SPARK_STEPS =", "  function buildDynamicScene(");
function make(guideBody, effectsBody) {
  const context = vm.createContext({});
  vm.runInContext(`
    const MAX_DECALS=12, MAX_PARTICLES=64, MAX_VERTICES=1024, MAX_INDICES=2048;
    const FLAT_STEP=${source.match(/const FLAT_STEP = ([\d.]+),/)[1]};
    const COLORS={bullet:[1,.82,.28],pickup:[.23,.84,.47],tread:[.1,.15,.17],
      shadow:[.03,.04,.05],particle:[.8,.6,.2]};
    const boxInstanceMatrices=new Float32Array(1024).fill(.123);
    const boxInstanceTints=new Float32Array(256).fill(.456);
    const boxInstanceSlotOwners=new Int16Array(64).fill(17);
    const positions=new Float32Array(MAX_VERTICES*3).fill(.789);
    const colors=new Float32Array(MAX_VERTICES*4).fill(.987);
    const indices=new Uint16Array(MAX_INDICES).fill(93);
    const qualificationAITimes=new Float64Array(24);
    let collectInstancedBoxes=true,boxInstanceProgram={},boxInstanceCount=0;
    let frameInstanceCeiling=64,optionalInstanceCeiling=64;
    let vertexCount=0,indexCount=0,meshDrops=0,instanceCapHitThisFrame=false;
    let droppedDecalInstances=0,droppedParticleInstances=0;
    let lastBoxYaw=NaN,lastBoxCosine=1,lastBoxSine=0;
    let decalCursor=0,particleActiveLowMask=0,particleActiveHighMask=0;
    let optionalCalls=0,boxCalls=0,aimCalls=0,rayCalls=0,clock=0,aimLevelNow=1,aimKind=1;
    let aimEnemyPhase=0,aimEnemyLevel=0;
    const state={frames:0},player={active:true,x:0,z:0,turret:.7,
      turretSine:Math.sin(.7),turretCosine:Math.cos(.7)};
    const performance={now(){clock+=.125;return clock;}};
    function playerTank(){return player;}
    function aimGuideLevel(){return aimLevelNow;}
    // A stand-in cast: what it computes is checked elsewhere; here only
    // whether it runs and what the mark then emits.
    function updateAimGuide(tank){
      aimCalls++;aimGuide.kind1=aimKind;aimGuide.hitX=tank.x+.2;aimGuide.hitZ=tank.z+.3;
      aimGuide.normalX=aimKind===1?-1:0;aimGuide.normalZ=0;
      aimGuide.dirX=tank.turretSine;aimGuide.dirZ=tank.turretCosine;
      return (state.frames&1)===0;
    }
    function updateAimGuideEnemy(){rayCalls++;aimGuide.enemy=-1;aimGuide.enemyLeg=0;}
    function record(i) {return {active:true,x:i*.07-1,z:i*.13-2,y:.06,
      life:1,maximum:2,width:.2,depth:.7,yaw:i*.03,kind:i%2,
      sine:Math.sin(i*.03),cosine:Math.cos(i*.03),
      renderStreak:.6,renderSine:.6,renderCosine:.8};}
    const decalRecords=Array.from({length:MAX_DECALS},(_,i)=>record(i));
    const particles=Array.from({length:64},(_,i)=>record(i));
    const decalVisits=[],published=[];
    const decals=new Proxy(decalRecords,{get(target,key){
      if(typeof key==='string' && /^\\d+$/.test(key))decalVisits.push(+key);
      return target[key];
    }});
    ${helpers}
    // This fixture mutates independent effect lifetimes between cases and
    // isolates admission/order. Retention and its complete output buffers are
    // checked separately by test_treadline_retained_effects.js.
    addRetainedEffectBox=function(owner,stationary,...args){
      return addOptionalBox(...args);
    };
    const box=addBox,optional=addOptionalBox;
    addBox=function(...args){boxCalls++;published.push(args);return box(...args);};
    addOptionalBox=function(...args){optionalCalls++;return optional(...args);};
    this.run=function(config) {
      collectInstancedBoxes=config.instanced;boxInstanceProgram=config.program?{}:null;
      boxInstanceCount=config.first;frameInstanceCeiling=config.cap;
      optionalInstanceCeiling=config.optional;state.frames=config.frame;
      player.active=config.active;player.x=config.x;player.z=config.z;
      aimLevelNow=config.level;aimKind=config.hit?1:2;
      vertexCount=config.vertices;indexCount=config.indices;
      instanceCapHitThisFrame=false;meshDrops=0;
      droppedDecalInstances=0;droppedParticleInstances=0;
      optionalCalls=boxCalls=aimCalls=rayCalls=clock=0;aimEnemyPhase=aimEnemyLevel=0;
      decalVisits.length=published.length=0;
      qualificationAITimes.fill(0);
      for(let i=0;i<MAX_DECALS;i++) {
        const decal=decalRecords[i];decal.active=!!(config.decalMask&(1<<i));
        decal.life=(i-config.frame%5)*.13;decal.maximum=i===11?0:2;
        decal.sine=i===2?NaN:Math.sin(decal.yaw);
      }
      decalCursor=config.cursor;particleActiveLowMask=config.low>>>0;
      particleActiveHighMask=config.high>>>0;
      const profileFrame=config.profile;
      { ${guideBody} }
      // Intervening protected/scenery instances can consume further slots.
      boxInstanceCount=Math.max(boxInstanceCount,config.effectsFirst);
      optionalInstanceCeiling=config.effectsCap;
      ${effectsBody}
    };
    this.snapshot=function(){return {
      arrays:[boxInstanceMatrices,boxInstanceTints,boxInstanceSlotOwners,
        positions,colors,indices,qualificationAITimes].map(a=>new Uint8Array(a.buffer)),
      values:[boxInstanceCount,frameInstanceCeiling,optionalInstanceCeiling,
        vertexCount,indexCount,meshDrops,+instanceCapHitThisFrame,
        droppedDecalInstances,droppedParticleInstances,lastBoxYaw,lastBoxCosine,
        lastBoxSine,aimCalls,rayCalls,aimGuide.kind1,aimGuide.hitX,aimGuide.hitZ,
        aimGuide.boxes,boxCalls],
      visits:Array.from(decalVisits),published,optionalCalls};};
  `, context);
  return context;
}
function compare(a, b) {
  const left=a.snapshot(),right=b.snapshot();
  for(let i=0;i<left.arrays.length;i++)
    assert(Buffer.from(left.arrays[i]).equals(Buffer.from(right.arrays[i])),
      `complete output buffer ${i}`);
  assert.deepEqual(Array.from(left.values),Array.from(right.values),"state/counters");
  assert.deepEqual(Array.from(left.visits),Array.from(right.visits),"decal scan order");
  assert.equal(JSON.stringify(left.published),JSON.stringify(right.published),
    "unchanged exact emission order and arguments");
  return left.optionalCalls-right.optionalCalls;
}
const reference=make(referenceGuide,originalDecals+particles);
const candidate=make(guide,effects);
const MAX_TEST_VERTICES=1024,MAX_TEST_INDICES=2048;
const masks=[[0,0],[1,0],[0,1],[0x80000000,1],[7,3],[31,3],[0xffffffff,0xffffffff]];
const caps=[[0,0,64],[0,1,64],[61,62,64],[62,64,64],[63,64,64],
  [64,64,64],[10,9,64],[10,64,10],[10,12,11],[0,64,64]];
let cases=0,avoided=0,ordinaryCalls=0,optimizedCalls=0,fallbackCases=0;
const particleCounts=new Set();
const config={first:0,cap:64,optional:64,effectsFirst:0,effectsCap:64,
  frame:0,active:true,hit:true,x:0,z:0,vertices:0,indices:0,cursor:0,level:1,
  decalMask:4095,low:0,high:0,profile:false,instanced:true,program:true};
for(const [instanced,program]of [[true,true],[false,false],[true,false]])
  for(const [first,optional,cap]of caps)for(const [low,high]of masks)
    for(let frame=0;frame<16;frame++) {
      Object.assign(config,{instanced,program,first,optional,cap,low,high,frame,
        effectsFirst:first+(frame%3),effectsCap:cap,cursor:frame%12,
        active:frame%7!==0,hit:!!(frame&1),x:(frame%5)-2,z:(frame%7)-3,level:frame%3,
        profile:!!(frame&2),decalMask:frame%3===0?0:frame%3===1?0x555:4095,
        vertices:frame%5===0?MAX_TEST_VERTICES-24:0,
        indices:frame%5===0?MAX_TEST_INDICES-36:0});
      reference.run(config);candidate.run(config);
      const saved=compare(reference,candidate);
      if(!instanced)assert.equal(saved,0,"fallback calls must remain unchanged");
      if(!instanced||!program)fallbackCases++;
      const a=reference.snapshot(),b=candidate.snapshot();
      ordinaryCalls+=a.optionalCalls;optimizedCalls+=b.optionalCalls;
      avoided+=saved;cases++;
      particleCounts.add(Math.min(6,popcount(low)+popcount(high)));
    }
function popcount(mask){let n=0;for(mask>>>=0;mask;mask=(mask&(mask-1))>>>0)n++;return n;}
assert(avoided>1000,"pre-admission must eliminate rejected calls");
assert(particleCounts.has(0)&&particleCounts.has(1)&&particleCounts.has(6));
const guard=guide.indexOf("if (collectInstancedBoxes && boxInstanceCount >= optionalInstanceCeiling)");
assert(guard>guide.indexOf("updateAimGuide(player, aimLevel)"),"never skip aim updates");
Object.assign(config,{instanced:true,program:true,first:64,optional:64,cap:64,
  effectsFirst:64,effectsCap:64,active:true,frame:0,hit:true,decalMask:4095,level:1});
for(const [name,r,e]of [
  ["lost drop accounting",guide,effects.replace("        droppedDecalInstances++;","        droppedDecalInstances += 2;")],
  ["skipped aim update",guide.replace("if (updateAimGuide(player, aimLevel) && profileFrame)","if (false)"),effects],
  ["fallback admission",guide.replace("collectInstancedBoxes && boxInstanceCount", "boxInstanceCount"),effects],
]) {
  const bad=make(r,e),good=make(referenceGuide,originalDecals+particles);
  const faultConfig={...config,instanced:name!=="fallback admission"};
  good.run(faultConfig);bad.run(faultConfig);
  assert.throws(()=>compare(good,bad),assert.AssertionError,name);
}
console.log(JSON.stringify({cases,fallbackCases,ordinaryCalls,optimizedCalls,
  avoided,negativeControls:3,fullBufferAndStateParity:true}));
