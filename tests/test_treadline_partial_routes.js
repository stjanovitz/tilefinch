'use strict';
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const REPO=process.argv[3]||path.resolve(__dirname,'..');
const sourcePath=process.argv[2]||path.join(REPO,'examples/treadline-arena/game.js');
// Routing lives in bots.js beside game.js (MAX_TANKS in game.js).
const candidate=fs.readFileSync(sourcePath,'utf8')+fs.readFileSync(path.join(path.dirname(sourcePath),'bots.js'),'utf8');
const slots=Number(/const MAX_TANKS = (\d+)/.exec(candidate)?.[1]);assert(slots>=3&&slots<=16);
function extract(s,n){const a=s.indexOf('  function '+n+'('),b=s.indexOf('\n  }',a)+4;assert(a>=0&&b>a,'authored function '+n);return s.slice(a,b);}
// The full-field queue search below is a hand-kept, deliberately simple
// reference: the algorithm before partial routes, edited only where the
// tank fields it writes changed (navNext is gone). The candidate is
// extracted from the authored game, not copied into this test.
const original=`  function navigationCell(x, z) {
    const cx = x + 8, cz = z + 8;
    if (cx !== cx) return NaN;
    return ((cz <= 0 ? 0 : cz >= 16 ? 15 : cz | 0) << 4)
      + (cx <= 0 ? 0 : cx >= 16 ? 15 : cx | 0);
  }
  function steerBotRoute(tank, goalX, goalZ) {
    if (navBuildCursor < NAV_CELLS) return false;
    const routeAt = qualificationAIActive ? performance.now() : 0;
    let goal = navigationCell(goalX, goalZ);
    if (navBlocked[goal]) {
      // Retain remapped cells while their goal/grid is stable.
      const raw = goal;
      if (tank.navRawGoal === raw && tank.navMapRevision === navRevision) {
        goal = tank.navMappedGoal;
      } else {
        const nearest = arenaSpatial.nearestOpenCell(navBlocked, goalX, goalZ);
        tank.navRawGoal = raw; tank.navMapRevision = navRevision;
        tank.navMappedGoal = goal = nearest;
      }
      if (goal < 0) {
        if (qualificationAIActive) qualificationAITimes[15] += performance.now() - routeAt;
        return false;
      }
    }
    const fieldAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[15] += fieldAt - routeAt;
    let slot = tank.navFieldSlot;
    if (slot < 0 || navFieldGoals[slot] !== goal
        || navFieldRevisions[slot] !== navRevision) {
      slot = -1;
      for (let at = 0; at < MAX_TANKS; at++) {
        if (navFieldGoals[at] === goal && navFieldRevisions[at] === navRevision) {
          slot = at;
          if (qualificationAIActive) qualificationAICounts[6]++;
          break;
        }
      }
      tank.navFieldSlot = slot;
    }
    const buildField = slot < 0;
    if (buildField) {
      slot = navFieldCursor;
      navFieldCursor = (navFieldCursor + 1) % MAX_TANKS;
      tank.navFieldSlot = slot;
    }
    const base = slot * NAV_CELLS;
    if (buildField) {
      const searchAt = qualificationAIActive ? performance.now() : 0;
      navDistances.fill(65535, base, base + NAV_CELLS);
      navDistances[base + goal] = 0;
      let read = 0, count = 1;
      navQueue[0] = goal;
      while (read < count && read < NAV_CELLS) {
        const cell = navQueue[read++], x = cell & 15;
        const distance = navDistances[base + cell] + 1;
        // Preserve left/right/up/down order.
        let next = cell - 1;
        if (x > 0 && !navBlocked[next] && navDistances[base + next] === 65535) {
          navDistances[base + next] = distance; navQueue[count++] = next;
        }
        next = cell + 1;
        if (x < 15 && !navBlocked[next] && navDistances[base + next] === 65535) {
          navDistances[base + next] = distance; navQueue[count++] = next;
        }
        next = cell - NAV_SIDE;
        if (cell >= NAV_SIDE && !navBlocked[next] && navDistances[base + next] === 65535) {
          navDistances[base + next] = distance; navQueue[count++] = next;
        }
        next = cell + NAV_SIDE;
        if (cell < NAV_CELLS - NAV_SIDE && !navBlocked[next]
            && navDistances[base + next] === 65535) {
          navDistances[base + next] = distance; navQueue[count++] = next;
        }
      }
      navFieldGoals[slot] = goal; navFieldRevisions[slot] = navRevision;
      if (qualificationAIActive) {
        qualificationAITimes[6] += performance.now() - searchAt;
        qualificationAICounts[1]++; qualificationAICounts[2] += read;
      }
    }
    const waypointAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[16] += waypointAt - fieldAt;
    // Field eviction alone must not reset a waypoint.
    if (tank.navGoal !== goal || tank.navRevision !== navRevision) {
      tank.navGoal = goal; tank.navRevision = navRevision;
      tank.navWaypoint = -1;
    }
    const cell = navigationCell(tank.x, tank.z);
    // Waypoint policy (kept in step with the game): a waypoint survives
    // drifting into a neighbour cell, rechecked with half-radius padding.
    const waypoint = tank.navWaypoint;
    const drifted = waypoint >= 0 && tank.navWaypointFrom !== cell && waypoint !== cell
      && Math.abs((waypoint & 15) - (cell & 15)) <= 1 && Math.abs((waypoint >> 4) - (cell >> 4)) <= 1;
    if (waypoint >= 0 && (tank.navWaypointFrom === cell || drifted)) {
      const wx = (waypoint & 15) - 7.5;
      const wz = (waypoint >> 4) - 7.5;
      // Recheck the floating-origin segment against current scenery.
      if (!lineCrossesWalls(tank.x, tank.z, wx, wz, false,
          drifted ? Math.min(tank.collisionRadius + .02, tank.collisionRadius * .5)
            : tank.collisionRadius + .02, tank.id * 12 + 10)) {
        bankAim[0] = wx - tank.x; bankAim[1] = wz - tank.z;
        if (qualificationAIActive) qualificationAITimes[17] += performance.now() - waypointAt;
        return true;
      }
    }
    let best = Infinity, chosen = -1, nearest = -1, nearestDistance = 65535;
    const cx = cell & 15, cz = cell >> 4;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const x = cx + dx, z = cz + dz;
      if (x < 0 || x >= 16 || z < 0 || z >= 16) continue;
      const at = z * NAV_SIDE + x;
      if (navBlocked[at] || navDistances[base + at] === 65535) continue;
      const wx = x - 7.5, wz = z - 7.5;
      const px = wx - tank.x, pz = wz - tank.z;
      const score = navDistances[base + at] + (px * px + pz * pz) * .2;
      if (navDistances[base + at] < nearestDistance) {
        nearestDistance = navDistances[base + at]; nearest = at;
      }
      // Only improving candidates need LOS; preserve order/ties.
      if (score < best && !lineCrossesWalls(tank.x, tank.z, wx, wz, false,
          tank.collisionRadius + .02, tank.id * 12 + 11)) { best = score; chosen = at; }
    }
    // Waypoint policy: the neighbour nearest the goal, in sight with
    // half-radius padding, beats a farther chosen one.
    if (nearest >= 0 && nearest !== chosen && nearest !== cell
        && (chosen < 0 || nearestDistance < navDistances[base + chosen])
        && !lineCrossesWalls(tank.x, tank.z, (nearest & 15) - 7.5, (nearest >> 4) - 7.5,
          false, tank.collisionRadius * .5, tank.id * 12 + 11)) chosen = nearest;
    if (qualificationAIActive) qualificationAITimes[17] += performance.now() - waypointAt;
    if (chosen < 0) return false;
    tank.navWaypoint = chosen; tank.navWaypointFrom = cell;
    bankAim[0] = (chosen & 15) - 7.5 - tank.x;
    bankAim[1] = (chosen >> 4) - 7.5 - tank.z;
    return true;
  }`;
vm.runInThisContext(fs.readFileSync(path.join(REPO,'examples/treadline-arena/arena-generator.js'),'utf8'));
const data=globalThis.__treadlineArenaData,spatial=data.spatial;
// Every hull grid blocks its boundary ring (createNavigationCache), which the
// candidate's unchecked neighbour reads rely on; synthetic grids keep it too.
function ring(blocked){for(let i=0;i<256;i++){const x=i&15,z=i>>4;if(!x||x===15||!z||z===15)blocked[i]=1;}return blocked;}
function make(source,partial){
 const e={NAV_CELLS:256,NAV_SIDE:16,MAX_TANKS:slots,navBlocked:ring(new Uint8Array(256)),navDistances:new Uint16Array(slots*256),navQueue:new Uint16Array(partial?slots*256:256),navQueueHeads:new Uint16Array(slots),navQueueCounts:new Uint16Array(slots),navRequiredCells:new Uint16Array(9),navFieldGoals:new Array(slots).fill(-1),navFieldRevisions:new Array(slots).fill(-1),navFieldCursor:0,navRevision:0,navBuildCursor:256,qualificationAIActive:false,qualificationAITimes:new Float64Array(32),qualificationAICounts:new Uint32Array(32),performance:{now(){return 0;}},arenaSpatial:spatial,bankAim:new Float32Array(4),state:{time:0},calls:[],salt:0,NAV_FIELD_BLOCKED:65534,routeDisconnected:false,routeFieldSlot:-1,navFieldTemplateViews:[new Uint16Array(256)],navCandidateCells:new Int16Array(9),navCandidateScores:new Float64Array(9)};
 // updateBotNavigation keeps the template with the grid.
 e.syncTemplate=()=>{for(let c=0;c<256;c++)e.navFieldTemplateViews[0][c]=e.navBlocked[c]?65534:65535;};e.syncTemplate();
 // One grid (the game keeps one per hull size; index 0 keys like the
 // reference) and the full padding (start-inside fallback is exercised by
 // the game's own placement fixture).
 e.navGrids=[e.navBlocked];e.navDiagonal=()=>false;e.navGridIndex=()=>0;e.routePadding=(t)=>t.collisionRadius+.02;
 e.lineCrossesWalls=(ax,az,bx,bz,over,padding,cache)=>{const blocked=((Math.round(bx*8)+Math.round(bz*8)*19+e.salt)&7)===0;e.calls.push([ax,az,bx,bz,over,padding,cache,blocked]);return blocked;};
 const text=extract(source,'navigationCell')+'\n'+(partial?extract(source,'prepareBotRouteField'):'')+'\n'+(source.includes('  function chooseRouteWaypoint(')?extract(source,'chooseRouteWaypoint')+'\n':'')+extract(source,'steerBotRoute');
 const api=new Function('e','with(e){'+text+';return {route:steerBotRoute,drain:'+(partial?'prepareBotRouteField':'null')+'};}')(e);return{e,...api};
}
function tank(id){return{id,x:0,z:0,collisionRadius:.52,navFieldSlot:-1,navGoal:-1,navRevision:-1,navRawGoal:-1,navMapRevision:-1,navMappedGoal:-1,navWaypoint:-1,navWaypointFrom:-1,};}
const a=make(original,false),b=make(candidate,true),ta=[tank(1),tank(2),tank(3)],tb=[tank(1),tank(2),tank(3)];
let checks=0,grids=0,partialReturns=0,resumes=0,shared=0,remaps=0,refusals=0,forced=0,seed=0x1893ab21;
const rnd=()=>{seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;return seed>>>0;};
// Sight tests are pure here (and side-effect free in the game but for a
// test-order cache), so the candidate may test fewer: never one the
// reference did not, never with a different answer.
function losSubset(candidate,reference){const left=reference.map(c=>JSON.stringify(c));for(const call of candidate){const at=left.indexOf(JSON.stringify(call));assert(at>=0,'LOS call the reference never made: '+JSON.stringify(call));left.splice(at,1);}}
function query(at,goal){const x=((at*5)%16)-7.5+(at%3)*.13,z=((at*11)%16)-7.5-(at%4)*.07,id=at%3,t1=ta[id],t2=tb[id];t1.x=t2.x=x;t1.z=t2.z=z;
 a.e.state.time=b.e.state.time=checks/30;a.e.salt=b.e.salt=(checks/13)|0;a.e.calls.length=b.e.calls.length=0;
 const gx=(goal&15)-7.5,gz=(goal>>4)-7.5,slotBefore=t2.navFieldSlot,headBefore=slotBefore<0?0:b.e.navQueueHeads[slotBefore];
 const ra=a.route(t1,gx,gz),rb=b.route(t2,gx,gz);checks++;assert.equal(rb,ra,'return');assert.deepEqual(t2,t1,'tank state');assert.deepEqual(Array.from(b.e.bankAim),Array.from(a.e.bankAim),'aim');losSubset(b.e.calls,a.e.calls);assert.deepEqual(b.e.navFieldGoals,a.e.navFieldGoals);assert.deepEqual(b.e.navFieldRevisions,a.e.navFieldRevisions);assert.equal(b.e.navFieldCursor,a.e.navFieldCursor);
 if(!ra)refusals++;if(a.e.navBlocked[goal])remaps++;
 const s=t2.navFieldSlot;if(b.e.navBuildCursor===256&&s>=0&&b.e.navFieldRevisions[s]===b.e.navRevision){const base=s*256;
  if(b.e.navQueueHeads[s]<b.e.navQueueCounts[s])partialReturns++;
  if(slotBefore===s&&b.e.navQueueHeads[s]>headBefore)resumes++;
  if(ta.some((t,i)=>i!==id&&t.navFieldSlot===s))shared++;
  for(let c=0;c<256;c++){const v=b.e.navDistances[base+c];if(v===65534)assert(b.e.navBlocked[c]&&a.e.navDistances[base+c]===65535,'blocked sentinel');else if(v!==65535)assert.equal(v,a.e.navDistances[base+c],'known distance');}
  if(!(checks%31)){b.drain(s,-1);forced++;assert.deepEqual(unblock(b.e.navDistances.subarray(base,base+256)),Array.from(a.e.navDistances.subarray(base,base+256)),'explicit full diagnostic');}
 }
}
function unblock(view){return Array.from(view,v=>v===65534?65535:v);}
function grid(blocked){grids++;ring(blocked);a.e.navBlocked.set(blocked);b.e.navBlocked.set(blocked);b.e.syncTemplate();a.e.navBuildCursor=b.e.navBuildCursor=32;query(1,17);a.e.navBuildCursor=b.e.navBuildCursor=256;a.e.navRevision++;b.e.navRevision++;
 for(let goal=0;goal<256;goal++){query(goal,goal);if(!(goal%4))query(goal+1,goal);}
}
let blocked=new Uint8Array(256);grid(blocked);blocked.fill(1);grid(blocked);
for(let i=0;i<256;i++)blocked[i]=((i&15)+(i>>4))&1;grid(blocked);
blocked.fill(1);for(let y=0;y<16;y++){if(!(y&1))for(let x=0;x<16;x++)blocked[y*16+x]=0;else blocked[y*16+((y&2)?0:15)]=0;}grid(blocked);
for(let n=0;n<24;n++){for(let i=0;i<256;i++)blocked[i]=(rnd()&255)<n*10?1:0;grid(blocked);}
const barriers=Array.from({length:6},()=>({present:false,active:false,left:0,right:0,top:0,bottom:0})),crates=Array.from({length:4},()=>({active:false,x:0,z:0})),occ=spatial.createNavigationCache(Math.fround(.52*1.14)+.06),gen=globalThis.__treadlineCreateArenaGenerator(data.arenas,data.generatedArena);
function arena(id){const ar=data.arenas[id],count=spatial.itemCount(ar,'barriers');for(let i=0;i<6;i++){const b=barriers[i];b.present=b.active=i<count;if(b.present){const r=ar.barriers[i];b.left=r[0]-r[2]*.5;b.right=r[0]+r[2]*.5;b.top=r[1]-r[3]*.5;b.bottom=r[1]+r[3]*.5;}}occ.prepare(id,barriers,crates);
 for(let s=0;s<4;s++){for(let i=0;i<6;i++)barriers[i].active=barriers[i].present&&(!(s&2)||i%2===0);for(let first=0;first<256;first+=32)occ.update(blocked,first,first+32,barriers,crates,!!(s&1));grid(blocked);}}
for(let id=0;id<3;id++)arena(id);for(let n=0;n<8;n++){gen.materialize((n+1)*7919);spatial.fillSpatialData(3);arena(3);}
assert(partialReturns>0&&resumes>0&&shared>0&&remaps>0&&refusals>0&&forced>0);
// Force a wrong completed-distance claim: the ordinary decision must detect it.
let negative=false;const bad=candidate.replace('if (east === 65535) { navDistances[at + 1] = distance;','if (east === 65535) { navDistances[at + 1] = distance + 1;');assert.notEqual(bad,candidate);const broken=make(bad,true),baseline=make(original,false);const bt=tank(1),ot=tank(1);bt.x=ot.x=3;bt.z=ot.z=3;
try{baseline.route(ot,-6.5,-6.5);broken.route(bt,-6.5,-6.5);broken.drain(bt.navFieldSlot,-1);assert.deepEqual(unblock(broken.e.navDistances),Array.from(baseline.e.navDistances));losSubset(broken.e.calls,baseline.e.calls);assert.deepEqual(bt,ot);assert.deepEqual(Array.from(broken.e.bankAim),Array.from(baseline.e.bankAim));}catch(e){assert(e instanceof assert.AssertionError);negative=true;}assert(negative,'wrong-distance negative must fail');
// This deterministic work gate distinguishes the algorithm, not helper presence.
const near=make(candidate,true),full=make(original,false);
near.e.qualificationAIActive=full.e.qualificationAIActive=true;
const nt=tank(1),ft=tank(1);nt.x=ft.x=-.5;nt.z=ft.z=-.5;
assert.equal(near.route(nt,-.5,-.5),full.route(ft,-.5,-.5));
const nearbyPops=near.e.qualificationAICounts[2],originalPops=full.e.qualificationAICounts[2];
assert(nearbyPops>0&&nearbyPops<32,'nearby query must avoid full BFS');assert.equal(originalPops,14*14);
losSubset(near.e.calls,full.e.calls);assert.deepEqual(nt,ft);
near.drain(nt.navFieldSlot,-1);
assert.deepEqual(unblock(near.e.navDistances),Array.from(full.e.navDistances));
// Supply the helper to the legacy body too: its failure is excessive work,
// not a missing-symbol failure against pre-fix source.
const legacy=make(original+'\n'+extract(candidate,'prepareBotRouteField'),true);
legacy.e.qualificationAIActive=true;const lt=tank(1);lt.x=lt.z=-.5;legacy.route(lt,-.5,-.5);
assert(legacy.e.qualificationAICounts[2]>=32,'legacy work negative must fail bound');
console.log(JSON.stringify({nearbyPops,originalPops,legacyWorkNegative:true,status:'passed',grids,checks,partialReturns,resumes,shared,remaps,refusals,forcedCompleteChecks:forced,negativeControl:true}));
