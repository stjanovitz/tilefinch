/* Headless Treadline invariant sweep (scripts/run-treadline-invariants.py).

   Seeded, bounded matches across every mode, class, campaign mission,
   Range Fault sample and Practice drill, played by bot proxies (the
   league AI, with injected lunges and pivots) or by a scripted keyboard
   player that steers, double-taps to lunge, pivots, brake-drifts, taps and
   charges shots and uses gadgets through the real input path. After every
   1/30 s step the sweep checks geometry with its own exact rectangle
   tests (independent of the game's broad-phase grids):

   - tanks inside walls, closed gates, barriers or crates, outside the arena,
     overlapping each other, or crossing a wall between two steps;
   - shells inside walls, crossing walls (even a corner's tip) or enemy
     hulls without a hit along the path they swept (each contact a step
     turned at), spawned through a wall, or bouncing more often than
     allowed;
   - pickups, memory cards, mines, crates and the convoy inside scenery,
     and tanks inside the convoy;
     pickups, slalom gates, the capture zone and the convoy path reachable
     from the player (flood fill over permanent walls);
   - Quick Match foes deploying within 6 units of the player, and guards
     left alone in an elimination mode that never trade damage (45 s);
   - non-finite or out-of-range numbers (positions, health, cooldowns),
     transitions that never end, and matches that cannot finish;
   - bots stuck for long periods; replay determinism for keyboard runs.

   Every step also composes the camera (camera motion block below) and
   reports oscillation, pumping, jumps and jerk per case; bot cases alternate
   the fixed and follow cameras.

   No network, no clock reads in the simulation, no captured data. */
(() => {
  "use strict";
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const P = d.practice, DATA = __treadlineArenaData;
  const M = C.runtime, st = B.state, tanks = B.tanks;
  const DT = 1 / 30, EPS = 1e-3;
  const MIRROR = 128, GLASS = 1, RICO = 2, IRON = 4, WOUNDS = 8, FOG = 16;
  const BATTERY = 32, ECHO = 64, SECOND = 512;

  // ------------------------------------------------------------ cases --
  const CASES = [];
  for (const mode of [0, 1, 2, 3, 4, 5]) {
    for (let cls = 0; cls < 3; cls++) {
      for (const proxy of ["bot", "keys"]) {
        const n = CASES.length;
        CASES.push({kind: "quick", mode, cls, proxy, gadget: (mode * 2 + cls) % 5,
          scheme: (mode + cls) % 3, diff: (mode + cls + (proxy === "keys")) % 3,
          command: n % 3 === 0, seconds: mode === 3 || mode === 4 ? 120 : 90});
      }
    }
  }
  for (let cls = 0; cls < 3; cls++) {
    for (const wave of [5, 10]) {
      CASES.push({kind: "quick", mode: 3, wave, cls, proxy: wave === 5 ? "keys" : "bot",
        gadget: cls + 1, scheme: cls, diff: 1 + (cls & 1), command: true, seconds: 90});
    }
  }
  for (let cls = 0; cls < 3; cls++)
    CASES.push({kind: "quick", mode: 6, cls, proxy: "keys", gadget: cls, scheme: cls,
      diff: 1, command: false, seconds: 100});
  for (let mission = 0; mission < 25; mission++) {
    for (const proxy of ["bot", "keys"]) {
      CASES.push({kind: "mission", mission, proxy, diff: (mission + (proxy === "keys")) % 3,
        faults: 0, cls: mission % 3, gadget: mission % 5, scheme: (mission >> 1) % 3,
        command: mission % 4 === 0, seconds: 150});
    }
  }
  for (const mission of [0, 2, 3, 4, 5, 6, 7, 9, 10, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24]) {
    CASES.push({kind: "mission", mission, proxy: "bot", rep: true, diff: 2, faults: 0,
      cls: mission % 3, gadget: (mission + 2) % 5, scheme: 0, command: false, seconds: 90});
  }
  for (let mission = 0; mission < 24; mission++) {
    CASES.push({kind: "mission", mission, proxy: mission & 1 ? "bot" : "keys",
      diff: mission % 3, faults: MIRROR, cls: (mission + 1) % 3, gadget: (mission + 3) % 5,
      scheme: mission % 3, command: false, seconds: 60});
  }
  const FAULT_SAMPLE = [GLASS, RICO, IRON, WOUNDS, FOG, BATTERY, ECHO, SECOND,
    SECOND | MIRROR, GLASS | ECHO];
  FAULT_SAMPLE.forEach((faults, at) => {
    for (const mission of [at % 25, (at * 7 + 3) % 25, 20 + (at % 5)]) {
      CASES.push({kind: "mission", mission, proxy: (at + mission) & 1 ? "bot" : "keys",
        diff: (at + mission) % 3, faults, cls: at % 3, gadget: at % 5,
        scheme: (at + 1) % 3, command: at % 2 === 0, seconds: 90});
    }
  });
  for (let drill = 0; drill < 4; drill++) {
    for (const behaviour of drill ? [0] : [0, 1, 2]) {
      for (const proxy of ["keys", "bot"]) {
        CASES.push({kind: "practice", drill, behaviour, proxy, cls: (drill + behaviour) % 3,
          gadget: drill + 1, scheme: (drill + behaviour) % 3, diff: 1, command: false,
          seconds: 75});
      }
    }
  }

  // --------------------------------------------------------- geometry --
  /* Exact solid rectangles [left, right, top, bottom] for the current
     arena: static walls, the closed gate, present/active barriers and
     active crates. Kinds: 0 wall, 1 gate, 2 barrier, 3 crate. */
  const solids = new Float64Array(64 * 4), solidKind = new Int8Array(64);
  const solidIndex = new Int8Array(64);
  let solidCount = 0;
  function arenaRecord() { return DATA.arenas[st.arena]; }
  function count(arena, kind) {
    if (!arena.generated) return arena[kind].length;
    return kind === "obstacles" ? arena.obstacleCount
      : kind === "gates" ? arena.gateCount : arena.barrierCount;
  }
  function addSolid(kind, index, left, right, top, bottom) {
    const at = solidCount++ * 4;
    solids[at] = left; solids[at + 1] = right; solids[at + 2] = top; solids[at + 3] = bottom;
    solidKind[solidCount - 1] = kind; solidIndex[solidCount - 1] = index;
  }
  function rebuildSolids() {
    solidCount = 0;
    const arena = arenaRecord();
    for (let at = 0; at < count(arena, "obstacles"); at++) {
      const o = arena.obstacles[at];
      addSolid(0, at, o[0] - o[2] * .5, o[0] + o[2] * .5, o[1] - o[3] * .5, o[1] + o[3] * .5);
    }
    if (!st.gateOpen) {
      for (let at = 0; at < count(arena, "gates"); at++) {
        const g = arena.gates[at];
        addSolid(1, at, g[0] - g[2] * .5, g[0] + g[2] * .5, g[1] - g[3] * .5, g[1] + g[3] * .5);
      }
    }
    for (let at = 0; at < B.barriers.length; at++) {
      const b = B.barriers[at];
      if (b.present && b.active) addSolid(2, at, b.left, b.right, b.top, b.bottom);
    }
    for (let at = 0; at < B.crates.length; at++) {
      const c = B.crates[at];
      if (c.active) addSolid(3, at, c.x - .22, c.x + .22, c.z - .22, c.z + .22);
    }
  }
  function rectDistance(at, x, z) {
    const left = solids[at], right = solids[at + 1], top = solids[at + 2], bottom = solids[at + 3];
    const dx = x < left ? left - x : x > right ? x - right : 0;
    const dz = z < top ? top - z : z > bottom ? z - bottom : 0;
    return Math.sqrt(dx * dx + dz * dz);
  }
  // First solid a circle penetrates by more than EPS, else -1.
  function circleSolid(x, z, radius, skipKind = -1) {
    for (let at = 0; at < solidCount; at++) {
      if (solidKind[at] === skipKind) continue;
      if (rectDistance(at * 4, x, z) < radius - EPS) return at;
    }
    return -1;
  }
  function pointInside(at, x, z, margin) {
    return x > solids[at] + margin && x < solids[at + 1] - margin
      && z > solids[at + 2] + margin && z < solids[at + 3] - margin;
  }
  // Segment against the rectangle shrunk by margin (Liang-Barsky).
  function segmentCrosses(at, ax, az, bx, bz, margin) {
    const left = solids[at] + margin, right = solids[at + 1] - margin;
    const top = solids[at + 2] + margin, bottom = solids[at + 3] - margin;
    if (left >= right || top >= bottom) return false;
    let t0 = 0, t1 = 1;
    const dx = bx - ax, dz = bz - az;
    const p = [-dx, dx, -dz, dz], q = [ax - left, right - ax, az - top, bottom - az];
    for (let k = 0; k < 4; k++) {
      if (p[k] === 0) { if (q[k] < 0) return false; continue; }
      const r = q[k] / p[k];
      if (p[k] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
      else { if (r < t0) return false; if (r < t1) t1 = r; }
    }
    return t0 <= t1;
  }
  // Deepest point of a segment inside a rectangle (16 samples).
  function crossingDepth(at, ax, az, bx, bz) {
    let deepest = 0;
    for (let k = 0; k <= 16; k++) {
      const x = ax + (bx - ax) * k / 16, z = az + (bz - az) * k / 16;
      const depth = Math.min(x - solids[at], solids[at + 1] - x, z - solids[at + 2], solids[at + 3] - z);
      if (depth > deepest) deepest = depth;
    }
    return deepest;
  }
  function solidName(at) {
    return ["wall", "gate", "barrier", "crate"][solidKind[at]] + solidIndex[at];
  }

  // Reachability for the player hull over permanent scenery only (static
  // walls and unbreakable barriers); gates may open, barriers and crates
  // can be destroyed. 0.25-unit cells, 4-connected flood fill.
  const REACH_SIDE = 64, REACH_STEP = .25;
  const reachFree = new Uint8Array(REACH_SIDE * REACH_SIDE);
  const reachSeen = new Uint8Array(REACH_SIDE * REACH_SIDE);
  const reachQueue = new Uint16Array(REACH_SIDE * REACH_SIDE);
  const permanent = new Float64Array(32 * 4);
  let permanentCount = 0, reachKey = "", reachRadius = 0;
  function reachKeyNow() {
    let key = st.arena + ":" + st.arenaSeed + ":";
    for (let at = 0; at < B.barriers.length; at++) {
      const b = B.barriers[at];
      if (b.present && b.health >= 1e5) key += at + "," + b.left + "," + b.top + ";";
    }
    return key + ":" + tanks[0].collisionRadius;
  }
  function rebuildReach() {
    const key = reachKeyNow();
    if (key === reachKey) return;
    reachKey = key;
    permanentCount = 0;
    const arena = arenaRecord();
    const add = (left, right, top, bottom) => {
      const at = permanentCount++ * 4;
      permanent[at] = left; permanent[at + 1] = right;
      permanent[at + 2] = top; permanent[at + 3] = bottom;
    };
    for (let at = 0; at < count(arena, "obstacles"); at++) {
      const o = arena.obstacles[at];
      add(o[0] - o[2] * .5, o[0] + o[2] * .5, o[1] - o[3] * .5, o[1] + o[3] * .5);
    }
    for (let at = 0; at < B.barriers.length; at++) {
      const b = B.barriers[at];
      if (b.present && b.health >= 1e5) add(b.left, b.right, b.top, b.bottom);
    }
    reachRadius = tanks[0].collisionRadius;
    for (let cz = 0; cz < REACH_SIDE; cz++) {
      for (let cx = 0; cx < REACH_SIDE; cx++) {
        const x = -7.875 + cx * REACH_STEP, z = -7.875 + cz * REACH_STEP;
        let free = Math.abs(x) <= 7.65 && Math.abs(z) <= 7.65;
        for (let at = 0; free && at < permanentCount * 4; at += 4) {
          const dx = x < permanent[at] ? permanent[at] - x : x > permanent[at + 1] ? x - permanent[at + 1] : 0;
          const dz = z < permanent[at + 2] ? permanent[at + 2] - z : z > permanent[at + 3] ? z - permanent[at + 3] : 0;
          if (dx * dx + dz * dz < reachRadius * reachRadius) free = false;
        }
        reachFree[cz * REACH_SIDE + cx] = free ? 1 : 0;
      }
    }
    floodFrom(tanks[0].x, tanks[0].z);
  }
  function floodFrom(x, z) {
    reachSeen.fill(0);
    let best = -1, bestDistance = 1e9;
    for (let cell = 0; cell < reachFree.length; cell++) {
      if (!reachFree[cell]) continue;
      const cx = -7.875 + (cell % REACH_SIDE) * REACH_STEP;
      const cz = -7.875 + ((cell / REACH_SIDE) | 0) * REACH_STEP;
      const distance = (cx - x) * (cx - x) + (cz - z) * (cz - z);
      if (distance < bestDistance) { bestDistance = distance; best = cell; }
    }
    if (best < 0) return;
    let head = 0, tail = 0;
    reachQueue[tail++] = best; reachSeen[best] = 1;
    while (head < tail) {
      const cell = reachQueue[head++], cx = cell % REACH_SIDE, cz = (cell / REACH_SIDE) | 0;
      const next = [cx > 0 ? cell - 1 : -1, cx < REACH_SIDE - 1 ? cell + 1 : -1,
        cz > 0 ? cell - REACH_SIDE : -1, cz < REACH_SIDE - 1 ? cell + REACH_SIDE : -1];
      for (const n of next) {
        if (n < 0 || reachSeen[n] || !reachFree[n]) continue;
        reachSeen[n] = 1; reachQueue[tail++] = n;
      }
    }
  }
  // A reachable hull centre within distance of (x, z)?
  function reachableWithin(x, z, distance) {
    rebuildReach();
    const limit = distance * distance;
    const first = Math.max(0, Math.floor((x - distance + 7.875) / REACH_STEP));
    const last = Math.min(REACH_SIDE - 1, Math.ceil((x + distance + 7.875) / REACH_STEP));
    const firstZ = Math.max(0, Math.floor((z - distance + 7.875) / REACH_STEP));
    const lastZ = Math.min(REACH_SIDE - 1, Math.ceil((z + distance + 7.875) / REACH_STEP));
    for (let cz = firstZ; cz <= lastZ; cz++) {
      for (let cx = first; cx <= last; cx++) {
        if (!reachSeen[cz * REACH_SIDE + cx]) continue;
        const px = -7.875 + cx * REACH_STEP, pz = -7.875 + cz * REACH_STEP;
        if ((px - x) * (px - x) + (pz - z) * (pz - z) <= limit) return true;
      }
    }
    return false;
  }

  // ---------------------------------------------------------- run state --
  let run = null;
  const prevX = new Float64Array(6), prevZ = new Float64Array(6);
  const prevActive = new Uint8Array(6), overlapFrames = new Int32Array(6);
  const activeBefore = new Uint8Array(6), stuckBlocked = new Float64Array(6);
  const startX = new Float64Array(6), startZ = new Float64Array(6);
  const overlapTankFrames = new Int32Array(6);
  const stuckX = new Float64Array(6), stuckZ = new Float64Array(6);
  const stuckSince = new Float64Array(6), stuckIntent = new Float64Array(6);
  const prevBlocked = new Float64Array(6);
  const bPrevX = new Float64Array(18), bPrevZ = new Float64Array(18);
  const bPrevActive = new Uint8Array(18), bPrevLife = new Float64Array(18);
  const bPrevPierce = new Int8Array(18), bAllowed = new Int8Array(18);
  const bPrevPierceId = new Int8Array(18);
  /* The path a shell swept this step: from (ax, az) through each contact it
     turned at (d.bulletTurns) to its position now. */
  const path = new Float64Array(2 * 6);
  let pathPoints = 0;
  function shellPath(at, b, ax, az) {
    path[0] = ax; path[1] = az; pathPoints = 1;
    for (let turn = 0; turn < b.turns; turn++) {
      path[pathPoints * 2] = d.bulletTurns[at * 8 + turn * 2];
      path[pathPoints * 2 + 1] = d.bulletTurns[at * 8 + turn * 2 + 1];
      pathPoints++;
    }
    path[pathPoints * 2] = b.x; path[pathPoints * 2 + 1] = b.z; pathPoints++;
  }
  // First leg of the path that crosses solid k (shrunk by EPS), else -1.
  function pathCrosses(k) {
    for (let leg = 0; leg + 1 < pathPoints; leg++)
      if (segmentCrosses(k * 4, path[leg * 2], path[leg * 2 + 1],
          path[leg * 2 + 2], path[leg * 2 + 3], EPS)) return leg;
    return -1;
  }
  const pickupSeen = new Array(6).fill("");
  let prevGate = false, prevSolidsKey = "", epochKey = "";
  let rng = 1;
  function random() {
    rng ^= rng << 13; rng ^= rng >>> 17; rng ^= rng << 5; rng >>>= 0;
    return rng / 4294967296;
  }
  function violation(type, detail) {
    const key = type + "|" + (detail.tank === undefined ? "" : detail.tank)
      + "|" + (detail.what || "");
    if (run.seen[key]) { run.seen[key].count++; return; }
    const entry = {type, t: Math.round(run.t * 100) / 100, mode: st.mode, arena: st.arena,
      wave: st.wave, stage: M.on ? M.stage : -1, count: 1, ...detail};
    for (const name of ["x", "z", "x2", "z2", "speed"])
      if (typeof entry[name] === "number") entry[name] = Math.round(entry[name] * 1000) / 1000;
    run.seen[key] = entry; run.violations.push(entry);
  }
  function info(type, detail) {
    if (run.notes.length < 24) run.notes.push({type, t: Math.round(run.t * 10) / 10, ...detail});
  }
  function epoch() {
    return st.arena + ":" + st.wave + ":" + (M.on ? M.stage : -1) + ":" + st.arenaSeed
      + ":" + (P.on ? P.runtime.drill + "/" + (P.runtime.done ? 1 : 0) : "");
  }
  function baseline() {
    for (let id = 0; id < 6; id++) {
      const t = tanks[id];
      prevX[id] = t.x; prevZ[id] = t.z; prevActive[id] = t.active ? 1 : 0;
      stuckX[id] = t.x; stuckZ[id] = t.z; stuckSince[id] = run.t; stuckIntent[id] = 0;
      prevBlocked[id] = stuckBlocked[id] = d.stats.stuck[id];
    }
    for (let at = 0; at < d.bullets.length; at++) {
      const b = d.bullets[at];
      bPrevActive[at] = b.active ? 1 : 0; bPrevX[at] = b.x; bPrevZ[at] = b.z;
      bPrevLife[at] = b.life; bPrevPierce[at] = b.pierce;
      bPrevPierceId[at] = b.pierceId;
      bAllowed[at] = b.bounces + b.bounceCount;
    }
    prevGate = st.gateOpen;
    epochKey = epoch();
  }

  // ------------------------------------------------------- keyboard proxy --
  const KEYS = ["w", "a", "s", "d", "aimUp", "aimDown", "aimLeft", "aimRight", "fire",
    "secondary", "gadget", "ultimate", "pivotLeft", "pivotRight"];
  const held = Object.create(null);
  function key(name, pressed) {
    if (!!held[name] === pressed) return;
    held[name] = pressed; d.setKeyboard(name, pressed);
  }
  function releaseAll() { for (const name of KEYS) key(name, false); }
  const proxy = {moveX: 0, moveY: 0, until: 0, pivot: 0, pivotUntil: 0, fireUntil: 0,
    tap: 0, tapAt: 0, gap: 0};
  function playerGoal(player) {
    // Mode objective first, then the nearest live foe.
    const pickups = B.pickups;
    if (P.on && P.runtime.drill === 1 && pickups[0].active) return [pickups[0].x, pickups[0].z];
    if (M.on && M.card >= 0 && !M.cardFound && pickups[M.card] && pickups[M.card].active
        && run.t > 20 && run.t < 50) return [pickups[M.card].x, pickups[M.card].z];
    if (M.on && M.m && M.m.id === "5-2") {
      for (let at = 0; at < pickups.length; at++)
        if (pickups[at].active) return [pickups[at].x, pickups[at].z];
    }
    if (st.gameMode === 2 && B.convoy.active && player.team === 0)
      return [B.convoy.x + 1.2, B.convoy.z - .6];
    if (st.gameMode === 1 && (run.t % 30) < 20) return [0, -.9];
    let best = null, bestDistance = 1e9;
    for (let id = 0; id < 6; id++) {
      const t = tanks[id];
      if (!t.active || t === player || t.team === player.team) continue;
      const distance = (t.x - player.x) ** 2 + (t.z - player.z) ** 2;
      if (distance < bestDistance) { bestDistance = distance; best = t; }
    }
    return best ? [best.x, best.z] : [0, 0];
  }
  function driveKeys() {
    const player = B.playerTank ? B.playerTank() : tanks[st.gameMode === 6 ? st.duelTurn : 0];
    if (st.mode !== "playing" || !player.active) { releaseAll(); return; }
    const t = run.t, scheme = run.c.scheme;
    const [gx, gz] = playerGoal(player);
    const dx = gx - player.x, dz = gz - player.z;
    const distance = Math.sqrt(dx * dx + dz * dz) || 1;
    if (t >= proxy.until) {
      const roll = random();
      proxy.until = t + .35 + random() * 1.1;
      if (roll < .58) {
        proxy.moveX = Math.abs(dx) > .38 * distance ? Math.sign(dx) : 0;
        proxy.moveY = Math.abs(dz) > .38 * distance ? -Math.sign(dz) : 0;
        if (distance < 2.2 && random() < .6) { proxy.moveX = -proxy.moveX; proxy.moveY = -proxy.moveY; }
      } else if (roll < .86) {
        proxy.moveX = ((random() * 3) | 0) - 1; proxy.moveY = ((random() * 3) | 0) - 1;
      } else proxy.moveX = proxy.moveY = 0;
      // Double-tap lunge: release now, press again within the tap window.
      if (random() < .3 && (proxy.moveX || proxy.moveY)) { proxy.tap = 2; proxy.tapAt = t + .05; }
      if (random() < .12) { proxy.pivot = random() < .5 ? -1 : 1; proxy.pivotUntil = t + .2 + random() * .6; }
      if (random() < .18) key("gadget", true);
      if (random() < .12) key("secondary", true);
      if (run.c.command && random() < .2) key("ultimate", true);
    } else {
      key("gadget", false); key("secondary", false); key("ultimate", false);
    }
    let mx = proxy.moveX, my = proxy.moveY;
    if (proxy.tap === 2) {
      // Press briefly, release, press again: a lunge in every scheme.
      if (t < proxy.tapAt) { mx = proxy.moveX; my = proxy.moveY; }
      else if (t < proxy.tapAt + .1) { mx = my = 0; }
      else proxy.tap = 0;
    }
    if (scheme === 1) {
      // Classic: W both treads, A/D one tread (a brake-drift when fast), S back.
      key("w", my < 0 && !mx); key("s", my > 0);
      key("a", mx < 0 || (my < 0 && mx < 0)); key("d", mx > 0 || (my < 0 && mx > 0));
    } else {
      key("a", mx < 0); key("d", mx > 0); key("w", my < 0); key("s", my > 0);
    }
    const pivoting = proxy.pivot && t < proxy.pivotUntil;
    key("pivotLeft", pivoting && proxy.pivot < 0); key("pivotRight", pivoting && proxy.pivot > 0);
    if (!pivoting) proxy.pivot = 0;
    // Aim at the goal (eight directions); Gunner cranks and snaps instead.
    const ax = Math.abs(dx) > .38 * distance ? Math.sign(dx) : 0;
    const az = Math.abs(dz) > .38 * distance ? Math.sign(dz) : 0;
    if (scheme === 2) {
      const want = Math.atan2(dx, dz), turret = player.turret;
      const error = Math.atan2(Math.sin(want - turret), Math.cos(want - turret));
      key("aimRight", error > .08); key("aimLeft", error < -.08); key("aimUp", random() < .02);
      key("aimDown", false);
    } else {
      key("aimRight", ax > 0); key("aimLeft", ax < 0); key("aimUp", az > 0); key("aimDown", az < 0);
    }
    // Tap or hold-and-release (charged) shots.
    if (held.fire) { if (t >= proxy.fireUntil) key("fire", false); }
    else if (random() < .09) { key("fire", true); proxy.fireUntil = t + (random() < .35 ? .45 + random() * .3 : .04); }
  }
  // Bot proxy: the league AI drives tank 0; add the new movement on top.
  function perturbBot() {
    const player = tanks[0];
    if (st.mode !== "playing" || !player.active) return;
    const roll = random();
    if (roll < .012) player.command.lunge = random() < .5 ? -1 : 1;
    else if (roll < .02) { player.command.left = -1; player.command.right = 1; player.command.reverse = false; }
    else if (roll < .028) { player.command.left = 1; player.command.right = 0; }
  }

  // ------------------------------------------------------------ checks --
  const MODES = ["playing", "arena-clear", "victory", "game-over", "generating",
    "duel-pass", "paused", "replay-done", "killcam"];
  function finite(v) { return typeof v === "number" && Number.isFinite(v); }
  // Quick Match foes deploy at least 6 units from the player; allow one
  // step of movement by both hulls before the check sees them.
  function checkSpawnDistance(id) {
    const t = tanks[id], player = tanks[0];
    if (run.c.kind !== "quick" || run.c.mode === 6 || !player.active
        || t.team === player.team) return;
    const distance = Math.hypot(t.x - player.x, t.z - player.z);
    if (distance < 6 - .9) violation("foe-spawned-near-player", {tank: id,
      what: distance.toFixed(2), x: t.x, z: t.z, role: t.role});
  }
  // Guards escalate: once only guards are left in a Quick Match elimination
  // mode, within GUARD_ENGAGE seconds (and again and again) there must be a
  // damage exchange (any hull losing armor) or a guard heading for the
  // player (its goal much nearer the player than it is), never a guard-only
  // stand-off behind cover. A hunting guard boxed in by scenery it cannot
  // route out of is a navigation stall, not a stand-off.
  const GUARD_ENGAGE = 45, healthBefore = new Float64Array(6);
  function checkGuards() {
    const c = run.c;
    if (c.kind !== "quick" || (c.mode !== 0 && c.mode !== 3 && c.mode !== 4 && c.mode !== 5)
        || st.mode !== "playing") { run.guardSince = -1; return; }
    let guards = 0, others = 0, exchanged = false;
    for (let id = 0; id < 6; id++) {
      const t = tanks[id];
      if (t.active && t.health < healthBefore[id] - 1e-9) exchanged = true;
      healthBefore[id] = t.active ? t.health : 0;
      if (!t.active || t.team === tanks[0].team) continue;
      if (t.role !== "GUARD") { others++; continue; }
      guards++;
      const player = tanks[0];
      if (Math.hypot(t.strategyGoalX - player.x, t.strategyGoalZ - player.z)
          < .6 * Math.hypot(t.x - player.x, t.z - player.z)) exchanged = true;
    }
    if (!guards || others) { run.guardSince = -1; return; }
    if (run.guardSince < 0 || exchanged) run.guardSince = run.t;
    else if (run.t - run.guardSince > GUARD_ENGAGE)
      violation("guard-stand-off", {what: guards + " guard(s)", tank: 0});
  }
  function checkStep() {
    const solidsKeyBefore = prevSolidsKey;
    rebuildSolids();
    let solidsKey = "";
    for (let at = 0; at < solidCount; at++) solidsKey += solidKind[at] + "" + solidIndex[at] + ",";
    prevSolidsKey = solidsKey;
    const appeared = solidsKey !== solidsKeyBefore;
    const gateClosed = prevGate && !st.gateOpen;
    if (!MODES.includes(st.mode)) violation("unknown-mode", {what: st.mode});
    if (!finite(st.time) || !finite(st.score) || st.lives < 0)
      violation("bad-state", {what: `time=${st.time} score=${st.score} lives=${st.lives}`});
    const fresh = epoch() !== epochKey;
    if (fresh) {
      for (let id = 1; id < 6; id++) if (tanks[id].active) checkSpawnDistance(id);
      baseline(); return;
    }
    for (let id = 0; id < 6; id++) {
      activeBefore[id] = prevActive[id]; startX[id] = prevX[id]; startZ[id] = prevZ[id];
    }
    for (let id = 0; id < 6; id++) {
      const t = tanks[id];
      const was = prevActive[id];
      if (!t.active) { prevActive[id] = 0; overlapFrames[id] = overlapTankFrames[id] = 0; continue; }
      for (const name of ["x", "z", "yaw", "turret", "slideX", "slideZ", "velocityX", "velocityZ", "health"])
        if (!finite(t[name])) violation("non-finite", {tank: id, what: name});
      for (const name of ["cooldown", "secondaryCooldown", "gadgetCooldown", "lungeCooldown",
        "shield", "boost", "repair", "spawnGrace", "drift", "lunge", "commandBuff", "fireCharge"])
        if (t[name] < 0) violation("negative-timer", {tank: id, what: name});
      if (t.health <= 0) violation("active-with-no-health", {tank: id, what: String(t.health)});
      if (t.health > t.maxHealth + 1e-6) violation("health-above-max", {tank: id, what: t.health + "/" + t.maxHealth});
      if (Math.abs(t.x) > 7.65 + EPS || Math.abs(t.z) > 7.65 + EPS)
        violation("tank-out-of-bounds", {tank: id, x: t.x, z: t.z});
      const moved = Math.sqrt((t.x - prevX[id]) ** 2 + (t.z - prevZ[id]) ** 2);
      const teleported = !was || moved > 12 * DT + .05;
      if (!was) checkSpawnDistance(id);
      const hit = circleSolid(t.x, t.z, t.collisionRadius);
      if (hit >= 0) {
        if (!overlapFrames[id]++) {
          const cause = !was ? "spawned" : teleported ? "teleported"
            : gateClosed && solidKind[hit] === 1 ? "gate-closed-on-tank"
              : appeared ? "scenery-appeared" : "moved-into";
          violation("tank-in-scenery", {tank: id, what: solidName(hit) + ":" + cause,
            x: t.x, z: t.z, boss: t.boss, role: t.role});
        }
        if (overlapFrames[id] === 30) violation("tank-trapped-in-scenery", {tank: id,
          what: solidName(hit), x: t.x, z: t.z});
      } else overlapFrames[id] = 0;
      if (was && !teleported) {
        for (let at = 0; at < solidCount; at++) {
          if (segmentCrosses(at * 4, prevX[id], prevZ[id], t.x, t.z, EPS)) {
            violation("tank-crossed-scenery", {tank: id, what: solidName(at),
              x: prevX[id], z: prevZ[id], x2: t.x, z2: t.z, speed: moved / DT});
            break;
          }
        }
      }
      // Hull overlaps (strictly deeper than the 1e-3 tolerance).
      for (let other = id + 1; other < 6; other++) {
        const o = tanks[other];
        if (!o.active) continue;
        const reach = t.collisionRadius + o.collisionRadius;
        const distance = Math.sqrt((t.x - o.x) ** 2 + (t.z - o.z) ** 2);
        if (distance < reach - .02) {
          const cause = !was || !prevActive[other] ? "spawned"
            : teleported ? "teleported" : "moved-into";
          if (!overlapTankFrames[id]++) violation("tanks-overlap", {tank: id,
            what: `tank${other}:${cause}:${(reach - distance).toFixed(3)}`, x: t.x, z: t.z});
        }
      }
      // Stuck bots: intent to move, but no progress for a long time.
      if (!t.player) {
        const intent = Math.abs(t.command.left + t.command.right) > .1;
        if (intent) stuckIntent[id] += DT;
        if ((t.x - stuckX[id]) ** 2 + (t.z - stuckZ[id]) ** 2 > .36) {
          stuckX[id] = t.x; stuckZ[id] = t.z; stuckSince[id] = run.t; stuckIntent[id] = 0;
          stuckBlocked[id] = d.stats.stuck[id];
        } else if (run.t - stuckSince[id] > 6 && stuckIntent[id] > 4.5 && !t.inert
            && t.driveSpeed > 0
            // A boss with a tread shot off can only spin or circle: by design.
            && !(t.boss && (t.leftTreadHealth <= 0 || t.rightTreadHealth <= 0))) {
          const key = id + ":" + Math.round(stuckX[id]) + "," + Math.round(stuckZ[id]);
          if (!run.stuck[key]) {
            run.stuck[key] = {tank: id, x: Math.round(stuckX[id] * 100) / 100,
              z: Math.round(stuckZ[id] * 100) / 100, t: Math.round(stuckSince[id]),
              role: t.role, boss: t.boss, seconds: 0, blocked: 0, target: t.target,
              standoff: t.standoff, backing: t.backingOff};
            run.stuckList.push(run.stuck[key]);
          }
          run.stuck[key].seconds = Math.round((run.t - stuckSince[id]) * 10) / 10;
          run.stuck[key].blocked = Math.round((d.stats.stuck[id] - stuckBlocked[id]) * 10) / 10;
        }
      }
      prevX[id] = t.x; prevZ[id] = t.z; prevActive[id] = 1;
    }
    for (let id = 0; id < 6; id++) if (!tanks[id].active) overlapTankFrames[id] = 0;
    // Shells.
    const bullets = d.bullets;
    for (let at = 0; at < bullets.length; at++) {
      const b = bullets[at];
      if (!b.active) { bPrevActive[at] = 0; continue; }
      if (!finite(b.x) || !finite(b.z) || !finite(b.vx) || !finite(b.vz))
        violation("non-finite", {what: "bullet"});
      const spawned = !bPrevActive[at] || b.life > bPrevLife[at] + 1e-9;
      if (spawned) {
        bAllowed[at] = b.bounces + b.bounceCount;
        const owner = tanks[b.owner];
        // A shooter redeployed later in the same step (killed by a return
        // shot) no longer marks where the shell started.
        const shooterMoved = !activeBefore[b.owner] || Math.hypot(owner.x - startX[b.owner],
          owner.z - startZ[b.owner]) > 12 * DT + .05;
        // The spawn point lies .78 along the turret and the shell flew on
        // along it to its first contact, so the path from the hull centre
        // through each contact covers spawn and flight; the centre cannot
        // be inside scenery, so a solid crossed means the shell started
        // through or inside a wall.
        shellPath(at, b, owner.x, owner.z);
        for (let k = 0; !shooterMoved && k < solidCount; k++) {
          if (solidKind[k] === 2 && (b.overCover || b.pierceId === 20 + solidIndex[k]))
            continue;
          if (solidKind[k] === 3) continue; // crates stop shells at .32
          if (pathCrosses(k) >= 0) {
            violation("shell-spawned-through-scenery", {tank: b.owner, what: solidName(k),
              x: owner.x, z: owner.z});
            break;
          }
        }
      } else {
        shellPath(at, b, bPrevX[at], bPrevZ[at]);
        for (let k = 0; k < solidCount; k++) {
          const kind = solidKind[k];
          // A piercing shell passes through the barrier it is piercing.
          if (kind === 2 && (b.overCover || bPrevPierce[at] > b.pierce
              || b.pierceId === 20 + solidIndex[k]
              || bPrevPierceId[at] === 20 + solidIndex[k])) continue;
          if (kind === 3) continue; // crates use their own .32 box
          // A gate that closed this step over a shell: the next step's sweep
          // starts embedded and ends it.
          if (kind === 1 && gateClosed) continue;
          if (pointInside(k * 4, b.x, b.z, EPS)) {
            violation("shell-inside-scenery", {tank: b.owner, what: solidName(k), x: b.x, z: b.z});
            break;
          }
          // Shells sweep their path, so even a corner's tip is a crossing.
          const leg = pathCrosses(k);
          if (leg >= 0) {
            violation("shell-crossed-scenery", {tank: b.owner, what: solidName(k),
              x: path[leg * 2], z: path[leg * 2 + 1], x2: path[leg * 2 + 2],
              z2: path[leg * 2 + 3], depth: Math.round(crossingDepth(k * 4, path[leg * 2],
                path[leg * 2 + 1], path[leg * 2 + 2], path[leg * 2 + 3]) * 1e6) / 1e6});
            break;
          }
        }
        const owner = tanks[b.owner];
        for (let id = 0; id < 6; id++) {
          const t = tanks[id];
          // Respawns run after the shell update (or inside it, when a hit
          // redeploys the player at once): only hulls present in both steps,
          // and where they were, can have been missed.
          if (!t.active || !activeBefore[id] || id === b.owner || t.team === owner.team
              || Math.hypot(t.x - startX[id], t.z - startZ[id]) > 12 * DT + .05) continue;
          // Closest approach of the step's path to the hull centre.
          for (let leg = 0; leg + 1 < pathPoints; leg++) {
            const ax = path[leg * 2], az = path[leg * 2 + 1];
            const ex = path[leg * 2 + 2] - ax, ez = path[leg * 2 + 3] - az;
            const length = ex * ex + ez * ez || 1;
            const s = Math.max(0, Math.min(1, ((t.x - ax) * ex + (t.z - az) * ez) / length));
            const px = ax + ex * s - t.x, pz = az + ez * s - t.z;
            if (px * px + pz * pz < .2) {
              violation("shell-passed-through-tank", {tank: b.owner, what: "tank" + id, x: t.x, z: t.z});
              break;
            }
          }
        }
      }
      if (b.bounceCount > bAllowed[at]) violation("shell-extra-bounce", {tank: b.owner,
        what: b.bounceCount + ">" + bAllowed[at]});
      if (b.life > 3 + 1e-6) violation("shell-life", {what: String(b.life)});
      bPrevActive[at] = 1; bPrevX[at] = b.x; bPrevZ[at] = b.z;
      bPrevLife[at] = b.life; bPrevPierce[at] = b.pierce; bPrevPierceId[at] = b.pierceId;
    }
    // Pickups (armor/coolant, memory cards, slalom gates).
    for (let at = 0; at < B.pickups.length; at++) {
      const p = B.pickups[at];
      if (!p.active) { pickupSeen[at] = ""; continue; }
      const key = p.x + "," + p.z + ":" + reachKeyNow();
      if (pickupSeen[at] === key) continue;
      pickupSeen[at] = key;
      if (!finite(p.x) || !finite(p.z)) { violation("non-finite", {what: "pickup"}); continue; }
      const inside = circleSolid(p.x, p.z, .05, 3);
      if (inside >= 0 && solidKind[inside] !== 1)
        violation("pickup-in-scenery", {what: solidName(inside) + ":" + p.type, x: p.x, z: p.z});
      const what = M.on && at === M.card ? "memory-card" : P.on && P.runtime.drill === 1 ? "slalom-gate" : p.type;
      if (!reachableWithin(p.x, p.z, Math.sqrt(.65) - .03))
        violation("pickup-unreachable", {what, x: p.x, z: p.z});
    }
    for (let at = 0; at < B.crates.length; at++) {
      const c = B.crates[at];
      if (!c.active) continue;
      for (let k = 0; k < solidCount; k++) {
        if (solidKind[k] === 3 || solidKind[k] === 1) continue;
        if (c.x + .22 > solids[k * 4] + EPS && c.x - .22 < solids[k * 4 + 1] - EPS
            && c.z + .22 > solids[k * 4 + 2] + EPS && c.z - .22 < solids[k * 4 + 3] - EPS) {
          violation("crate-in-scenery", {what: "crate" + at + ":" + solidName(k), x: c.x, z: c.z});
          break;
        }
      }
    }
    for (let at = 0; at < B.mines.length; at++) {
      const m = B.mines[at];
      if (!m.active || m.checked === m.x + m.z) continue;
      m.checked = m.x + m.z;
      const inside = circleSolid(m.x, m.z, .05, 3);
      if (inside >= 0) violation("mine-in-scenery", {tank: m.owner, what: solidName(inside), x: m.x, z: m.z});
    }
    if (B.convoy.active && st.gameMode === 2) {
      const c = B.convoy;
      if (!finite(c.x) || !finite(c.z) || !finite(c.health) || c.progress < 0 || c.progress > 1)
        violation("bad-convoy", {what: `${c.progress}/${c.health}`});
      for (let k = 0; k < solidCount; k++) {
        const left = solids[k * 4], right = solids[k * 4 + 1], top = solids[k * 4 + 2], bottom = solids[k * 4 + 3];
        if (c.x + .75 > left + .05 && c.x - .75 < right - .05 && c.z + .925 > top + .05 && c.z - .925 < bottom - .05) {
          violation("convoy-in-scenery", {what: solidName(k), x: c.x, z: c.z});
          break;
        }
      }
      // The crawler is solid to hulls: none inside it, however it got there.
      for (let id = 0; id < 6; id++) {
        const t = tanks[id];
        if (!t.active) continue;
        const ax = Math.abs(t.x - c.x) - .75, az = Math.abs(t.z - c.z) - .925;
        const depth = ax <= 0 && az <= 0 ? t.collisionRadius - Math.max(ax, az)
          : t.collisionRadius - Math.hypot(Math.max(0, ax), Math.max(0, az));
        if (depth > .02) violation("tank-in-convoy", {tank: id,
          what: (activeBefore[id] ? "moved" : "spawned") + ":" + depth.toFixed(3),
          x: t.x, z: t.z, role: t.role});
      }
    }
    checkGuards();
    // Transitions that never finish.
    if (st.mode === run.lastMode) run.modeTime += DT; else { run.lastMode = st.mode; run.modeTime = 0; }
    if ((st.mode === "arena-clear" && run.modeTime > 6) || (st.mode === "generating" && run.modeTime > 15))
      violation("stuck-transition", {what: st.mode});
    prevGate = st.gateOpen;
  }

  function objectiveChecks() {
    rebuildSolids(); rebuildReach();
    const player = tanks[0];
    if (!reachSeen.some(Boolean)) violation("player-enclosed", {x: player.x, z: player.z});
    if (st.gameMode === 1 && !reachableWithin(0, 0, Math.sqrt(2.9) - .05))
      violation("capture-zone-unreachable", {});
    if (st.gameMode === 2 && B.convoy.active && player.team === 0) {
      for (let p = B.convoy.progress; p <= 1; p += .02) {
        const x = -1.8 + Math.sin(p * Math.PI * 2) * 1.15, z = -4.8 + p * 10.6;
        if (!reachableWithin(x, z, 2.95)) {
          violation("convoy-escort-unreachable", {what: p.toFixed(2), x, z});
          break;
        }
      }
    }
    for (let id = 1; id < 6; id++) {
      const t = tanks[id];
      if (t.active && t.team !== player.team && !reachableWithin(t.x, t.z, 7))
        info("foe-far", {tank: id});
    }
  }

  // ----------------------------------------------------- camera motion --
  /* Camera track (tests/fixtures/treadline-camera-motion.js): after every
     step, compose the camera the way a device frame does after its update
     and feed the tracker the camera relative to the player. Hard cuts reset
     it: arena/wave/stage changes, the killcam and other non-play modes, the
     player down or switched (Duel turns), a respawn teleport, and a camera
     mode switch. Flags are reported per case, and
     run-treadline-invariants.py fails the sweep on any of them. */
  const CAMERA = globalThis.__treadlineCameraMotion;
  const CAMERA_LIVE = ["playing", "arena-clear", "victory", "game-over"];
  const cameraOut = new Float64Array(21);
  let cameraTrack = null, cameraEpoch = "", cameraModeSeen = -1, cameraPlayer = -1;
  let cameraX = 0, cameraZ = 0;
  // Bot proxies alternate the fixed and follow cameras; the keyboard proxy
  // steers in world directions, which only the fixed camera keeps.
  function cameraModeFor(c, index) { return c.proxy === "bot" && (index & 1) ? 1 : 0; }
  function cameraBegin() {
    // A probe may set __treadlineCameraTrace = {case, from, to, rows: []}.
    const trace = globalThis.__treadlineCameraTrace;
    cameraTrack = CAMERA.create(undefined, trace && trace.case === run.index ? trace : null);
    cameraEpoch = ""; cameraModeSeen = cameraPlayer = -1;
  }
  function cameraStep() {
    d.presentCamera(DT, cameraOut);
    const player = B.playerTank ? B.playerTank() : tanks[st.gameMode === 6 ? st.duelTurn : 0];
    const id = tanks.indexOf(player), key = epoch(), live = CAMERA_LIVE.includes(st.mode);
    const cut = !live ? st.mode : !player.active ? "player-down" : key !== cameraEpoch ? "arena"
      : cameraOut[3] !== cameraModeSeen ? "camera-mode" : id !== cameraPlayer ? "player-switch"
        : Math.hypot(player.x - cameraX, player.z - cameraZ) > 12 * DT + .05 ? "teleport" : "";
    cameraEpoch = key; cameraModeSeen = cameraOut[3]; cameraPlayer = id;
    cameraX = player.x; cameraZ = player.z;
    if (cut) cameraTrack.cut(cut);
    if (!live || !player.active) return;
    // Eye and view direction from the composed view matrix (column-major
    // lookAt); the look target is where that ray meets target height .28.
    const v = 5, o = cameraOut, t0 = o[v + 12], t1 = o[v + 13], t2 = o[v + 14];
    const ex = -(o[v] * t0 + o[v + 1] * t1 + o[v + 2] * t2);
    const ey = -(o[v + 4] * t0 + o[v + 5] * t1 + o[v + 6] * t2);
    const ez = -(o[v + 8] * t0 + o[v + 9] * t1 + o[v + 10] * t2);
    const along = o[v + 6] > 1e-6 ? (ey - .28) / o[v + 6] : 0;
    const tx = ex - o[v + 2] * along, tz = ez - o[v + 10] * along;
    cameraTrack.sample(run.t, o[0], Math.asin(Math.max(-1, Math.min(1, o[v + 6]))), o[2],
      ex - player.x, ez - player.z, tx - player.x, tz - player.z, o[4], o[3] === 1);
  }

  // ------------------------------------------------------------- api --
  globalThis.__treadlineInvariants = {
    count: CASES.length,
    begin(index) {
      const c = CASES[index];
      run = {c, index, t: 0, violations: [], notes: [], seen: Object.create(null),
        stuck: Object.create(null), stuckList: [], lastMode: "", modeTime: 0,
        replay: null, ended: "", maxWave: 0, steps: 0, deaths: [], lives: 99,
        guardSince: -1};
      rng = (Math.imul(index + 1, 0x9e3779b1) ^ 0x5bd1e995) >>> 0 || 1;
      releaseAll();
      Object.assign(proxy, {moveX: 0, moveY: 0, until: 0, pivot: 0, pivotUntil: 0,
        fireUntil: 0, tap: 0, tapAt: 0});
      C.menu.toMain();
      d.selectControls(c.scheme); d.selectAimAssist(1); d.selectCamera(cameraModeFor(c, index));
      d.setCommandEnabled(c.command); d.selectClass(c.cls); d.selectGadget(c.gadget);
      d.selectDifficulty(c.diff); d.setReverse(2);
      if (c.proxy === "bot") d.beginLeague(index + 1, c.diff, c.diff, 0, 0);
      if (c.kind === "quick") {
        st.gameMode = c.mode; st.difficultyChoice = c.diff;
        d.start();
        // Onslaught/Daily forge their arena cooperatively first; its last
        // phases wait for the HUD to be drawn, so render while forging.
        for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
        if (d.snapshot().arenaGenerationPhase) violation("stuck-transition", {what: "forging"});
        if (c.wave) d.setWave(c.wave);
      } else if (c.kind === "mission") {
        C.loadSave(""); C.save.d = c.diff; C.save.f = c.faults; C.save.k = 0x1ffffff;
        if (c.rep) { C.save.e = 1; C.save.rep = 1; }
        C.setRadio(2);
        C.startMission(c.mission, false);
      } else {
        P.runtime.behaviour = c.behaviour;
        P.enter(c.drill);
      }
      if (c.proxy === "bot") {
        st.difficultyChoice = c.diff;
        tanks[0].difficulty = c.diff;
      }
      run.label = c.kind === "mission" ? C.missions[c.mission].id + (c.rep ? "R" : "")
        + (c.faults ? "+f" + c.faults : "") : c.kind === "practice"
        ? "practice" + c.drill + "/" + c.behaviour : ["SURV", "TEAM", "CONVOY", "ONSL", "DAILY",
          "BILL", "DUEL"][c.mode] + (c.wave ? "w" + c.wave : "");
      run.lives = st.lives;
      prevSolidsKey = ""; reachKey = "";
      for (let at = 0; at < 6; at++) overlapFrames[at] = overlapTankFrames[at] = 0;
      pickupSeen.fill("");
      for (const m of B.mines) m.checked = NaN;
      baseline();
      // Placement: the very first state must already be clean.
      prevActive.fill(0);
      checkStep();
      objectiveChecks();
      cameraBegin();
      return run.label;
    },
    step(frames) {
      const c = run.c;
      for (let at = 0; at < frames; at++) {
        const mode = st.mode;
        if (mode === "victory" || mode === "game-over" || run.t >= c.seconds) {
          run.ended = run.t >= c.seconds && (mode === "playing" || mode === "paused") ? "timeout" : mode;
          return true;
        }
        if (mode === "duel-pass") { releaseAll(); d.resumeDuel(); }
        if (mode === "paused" && P.on) { releaseAll(); B.startOrResume(); }
        if (c.proxy === "keys") driveKeys(); else perturbBot();
        const arenaBefore = st.arena + ":" + st.wave + ":" + (M.on ? M.stage : -1);
        d.stepSimulation(1, DT);
        run.t += DT; run.steps++;
        cameraStep();
        if (st.wave > run.maxWave) run.maxWave = st.wave;
        if (st.lives < run.lives) {
          run.deaths.push(Math.round(run.t * 10) / 10);
          run.lives = st.lives;
        }
        checkStep();
        if (arenaBefore !== st.arena + ":" + st.wave + ":" + (M.on ? M.stage : -1)
            && st.mode === "playing") objectiveChecks();
      }
      return false;
    },
    // Keyboard runs record; play the log back and compare the digest.
    replay() {
      releaseAll();
      // Debug wave jumps are not replay inputs.
      if (run.c.proxy !== "keys" || P.on || run.c.wave) return "skipped";
      d.replayStop();
      const saved = d.replayState();
      if (!saved.count) return "empty";
      const expected = JSON.stringify(d.replayDigestState());
      if (!d.replayStart()) { violation("replay-refused", {}); return "refused"; }
      for (let at = 0; at < saved.count + 4 && st.mode !== "replay-done"; at++) d.stepSimulation(1, DT);
      const state = d.replayState();
      if (!state.verified) {
        violation("replay-mismatch", {what: `frames=${saved.count}`});
        info("replay-digest", {expected: expected.slice(0, 400),
          actual: JSON.stringify(d.replayDigestState()).slice(0, 400)});
        return "mismatch";
      }
      return "verified";
    },
    result(replay) {
      const foes = [];
      for (let id = 1; id < 6; id++) {
        const t = tanks[id];
        if (t.active && t.team !== tanks[0].team)
          foes.push([id, Math.round(t.x * 10) / 10, Math.round(t.z * 10) / 10, t.role]);
      }
      return {case: run.index, label: run.label, proxy: run.c.proxy, cls: run.c.cls,
        diff: run.c.diff, scheme: run.c.scheme, end: run.ended || st.mode,
        seconds: Math.round(run.t * 10) / 10, wave: run.maxWave, deaths: run.deaths,
        mission: M.on && M.result ? {won: M.result.won, medals: M.result.medals} : null,
        foes: run.ended === "timeout" ? foes : undefined,
        stuck: run.stuckList.filter(s => s.seconds >= 8), replay,
        violations: run.violations, notes: run.notes, camera: cameraTrack.result()};
    },
    finish() {
      releaseAll();
      d.finishLeague(); C.menu.toMain();
      d.setCommandEnabled(false);
    },
  };
  globalThis.pocSummary = "TREADLINE-INVARIANTS-READY:" + CASES.length;
})();
