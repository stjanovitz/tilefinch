/* Placement and contact regressions found by the invariant sweep
   (tests/fixtures/treadline-invariants.js): things placed or moving where
   they should not be. Synthetic setups on the authored arenas; no captured
   data. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const T = B.tanks;
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(label + ': ' + what);
  }
  const DT = 1 / 30;
  function quick(mode, cls = 1) {
    C.menu.toMain(); d.selectMode(mode); d.selectClass(cls); d.start();
    d.freezeBots(true);
  }

  // A hull resting against a thin barrier keeps its .78 muzzle inside the
  // barrier; the shell must hit the barrier, not appear beyond it.
  label = 'muzzle';
  quick(0, 0);
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  d.clearCrates();
  const barrier = B.barriers[0];
  const player = T[0];
  d.setTankPosition(0, barrier.x, barrier.top - player.collisionRadius - .002);
  d.setTankHeading(0, 0);
  check(!B.circleHitsObstacle(player.x, player.z, player.collisionRadius, true), 'hull clear');
  const healthBefore = barrier.health;
  d.setKeyboard('fire', true); d.stepSimulation(15, DT);
  d.setKeyboard('fire', false);
  let beyond = false, fired = false;
  for (let frame = 0; frame < 20; frame++) {
    d.stepSimulation(1, DT);
    for (const shell of d.bullets) {
      if (!shell.active) continue;
      fired = true;
      if (shell.z > barrier.bottom) beyond = true;
    }
  }
  check(fired || barrier.health < healthBefore, 'charged shot fired');
  check(!beyond, 'no shell beyond the barrier');
  check(barrier.health < healthBefore, 'the barrier took the hit');
  d.freezeBots(false); C.menu.toMain();

  // Onslaught bosses deploy near (0, 5) on the far side. A covered spot
  // used to fall back to a grid scan that starts in the player's corner.
  label = 'boss';
  C.menu.toMain(); d.selectMode(3); d.start();
  for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
  check(B.state.arena === 3 && B.state.mode === 'playing', 'Onslaught forged');
  let covered = 0;
  for (let seed = 1; seed <= 24; seed++) {
    d.generateArenaSeed(Math.imul(seed, 0x2545f491) >>> 0);
    const moves = d.spawnStats();
    d.setWave(5);
    const boss = T[5], after = d.spawnStats();
    check(boss.active && boss.boss, 'boss wave ' + seed);
    if (Math.hypot(boss.x, boss.z - 5) > .01) covered++;
    // A spot sealed in (or a coin-lost pocket) moves it to the nearest open
    // cell that routes to the player, still clear of the player.
    const moved = after.sealedMoved + after.moved > moves.sealedMoved + moves.moved;
    check(moved ? Math.hypot(boss.x - T[0].x, boss.z - T[0].z) >= 5.2
      : boss.z > 1.5 && Math.hypot(boss.x, boss.z - 5) < 3.5,
      `boss stays near its far-side spot (seed ${seed}: ${boss.x.toFixed(2)}, ${boss.z.toFixed(2)})`);
    check(!B.circleHitsObstacle(boss.x, boss.z, boss.collisionRadius, true), 'boss clear of scenery');
    for (let id = 0; id < 5; id++) {
      const t = T[id];
      if (t.active) check(Math.hypot(t.x - boss.x, t.z - boss.z)
        >= t.collisionRadius + boss.collisionRadius, 'boss clear of tank ' + id);
    }
  }
  check(covered > 0, 'some arenas cover the boss spot');
  C.menu.toMain();

  // A bot that pushes itself out of another hull must not land in a third.
  label = 'separation';
  quick(0, 1);
  d.clearCrates();
  for (let id = 1; id < 4; id++) d.setTankActive(id, true);
  for (let id = 4; id < 6; id++) d.setTankActive(id, false);
  const [mover, pushed, bystander] = [T[1], T[2], T[3]];
  d.setTankPosition(1, .5, -3); d.setTankPosition(2, 1.5, -3); d.setTankPosition(3, -.6, -3);
  d.setTankHeading(1, Math.PI / 2);
  for (const t of [mover, pushed, bystander]) {
    t.command.left = t.command.right = 0; t.command.reverse = false;
    t.slideX = t.slideZ = 0;
  }
  mover.command.left = mover.command.right = 1;
  d.stepSimulation(1, DT);
  const gap = Math.hypot(mover.x - bystander.x, mover.z - bystander.z)
    - mover.collisionRadius - bystander.collisionRadius;
  check(gap >= -1e-9, 'separated hull clear of the third tank (' + gap.toFixed(3) + ')');
  d.freezeBots(false); C.menu.toMain();

  // Campaign reinforcements and respawns can re-seal a Survival gate; it
  // must not close on a tank standing in the opening.
  label = 'gate';
  quick(0, 1);
  d.clearCrates();
  d.setTankActive(2, false); d.setTankActive(3, false);
  d.stepSimulation(1, DT);
  check(B.state.gateOpen, 'gate opens with one foe left');
  const gate = __treadlineArenaData.arenas[0].gates[0];
  const inGate = T[1];
  for (const t of T) { t.command.left = t.command.right = 0; t.slideX = t.slideZ = 0; }
  d.setTankPosition(1, gate[0], gate[1]);
  d.setTankActive(2, true);
  d.stepSimulation(2, DT);
  check(!B.circleHitsObstacle(inGate.x, inGate.z, inGate.collisionRadius, true),
    'the gate did not close on the tank in it');
  d.setTankPosition(1, gate[0] + 2.5, gate[1] - 2.5);
  d.stepSimulation(2, DT);
  check(!B.state.gateOpen, 'the gate seals once the opening is clear');
  d.freezeBots(false); C.menu.toMain();

  // Notice (5-1): a zone wall slides in at z 3.2-3.6. A tank on the live
  // side whose hull crosses that line must not be left inside the wall.
  label = 'zone wall';
  const M = C.runtime;
  C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.setRadio(2);
  C.startMission(20, false); d.freezeBots(true);
  for (const t of T) { t.command.left = t.command.right = 0; t.slideX = t.slideZ = 0; }
  const straddler = T[1];
  check(straddler.active, 'Notice has a foe');
  d.setTankPosition(1, 1, 3);
  check(!B.circleHitsObstacle(straddler.x, straddler.z, straddler.collisionRadius, true),
    'foe starts clear');
  M.t = 34.99; d.stepSimulation(2, DT);
  check(M.zone === 1, 'first zone powered off');
  check(!B.circleHitsObstacle(straddler.x, straddler.z, straddler.collisionRadius, true),
    'zone wall did not close on the foe');
  d.freezeBots(false); C.menu.toMain();

  // A powered-off zone takes its pickups with it (the hidden memory card
  // and dropped armor would otherwise sit behind the wall, unreachable);
  // a lost card is not a found card.
  label = 'zone pickups';
  C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.setRadio(2);
  C.startMission(20, false); d.freezeBots(true);
  const card = M.card;
  check(card >= 0 && B.pickups[card].x > 3.2, 'Notice hides its card in the east corner');
  const drop = B.pickups.findIndex(p => !p.active);
  Object.assign(B.pickups[drop], {active: true, x: 5, z: 0, phase: 0, type: 'ARMOR'});
  M.t = 70; d.stepSimulation(3, DT);
  check(M.zone === 2, 'east zone powered off');
  check(!B.pickups[card].active && !B.pickups[drop].active, 'dark-zone pickups removed');
  check(!M.cardFound, 'a lost card is not found');
  d.stepSimulation(3, DT);
  check(!M.cardFound, 'a lost card stays unfound');
  d.freezeBots(false); C.menu.toMain();

  // The convoy route crosses arena 3's west barrier; the crawler must not
  // drive through intact scenery.
  label = 'convoy';
  quick(2, 1);
  const convoy = B.convoy;
  const overlapping = () => B.barriers.filter(b => b.present && b.active
    && b.right > convoy.x - .75 && b.left < convoy.x + .75
    && b.bottom > convoy.z - .925 && b.top < convoy.z + .925).length;
  let crossed = 0;
  for (let progress = .2; progress <= 1; progress += .01) {
    convoy.progress = progress; d.stepSimulation(1, DT);
    if (!convoy.active) break;
    crossed += overlapping();
  }
  check(crossed === 0, 'the convoy never overlaps an intact barrier');
  C.menu.toMain();

  // A mine dropped while backed against scenery lands under the hull, not
  // inside the wall behind it.
  label = 'mine';
  C.menu.toMain(); d.selectGadget(0); d.selectMode(0); d.selectClass(1); d.start();
  d.freezeBots(true); d.clearCrates();
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  const wall = B.barriers[0], miner = T[0];
  d.setTankPosition(0, wall.x, wall.top - miner.collisionRadius - .002);
  d.setTankHeading(0, Math.PI);
  d.setKeyboard('gadget', true); d.stepSimulation(1, DT); d.setKeyboard('gadget', false);
  d.stepSimulation(1, DT);
  const mine = B.mines.find(m => m.active);
  check(mine, 'mine dropped');
  check(!B.circleHitsObstacle(mine.x, mine.z, .05, true), 'mine clear of scenery');
  d.freezeBots(false); C.menu.toMain();

  // Practice Range moving targets slide on rails; they must wait for a
  // tank in their lane rather than slide into its hull.
  label = 'moving targets';
  const P = d.practice;
  C.menu.toMain(); P.enter(3);
  const lane = T[1];
  check(lane.active && lane.inert, 'moving target up');
  d.setTankPosition(0, lane.x > 0 ? -.9 : .9, lane.z);
  check(Math.abs(lane.x - T[0].x) > 1.2, 'player clear of the target');
  let deepest = 0;
  for (let frame = 0; frame < 150; frame++) {
    d.stepSimulation(1, DT);
    for (let id = 1; id < 4; id++) {
      const t = T[id];
      if (!t.active) continue;
      const gap = Math.hypot(t.x - T[0].x, t.z - T[0].z) - t.collisionRadius - T[0].collisionRadius;
      deepest = Math.min(deepest, gap);
    }
  }
  check(deepest > -1e-6, 'targets never slide into the player (' + deepest.toFixed(3) + ')');
  P.leave(); C.menu.toMain();

  // Second Chance rewinds the player five seconds; something may stand
  // there by now. The rewound hull must not land inside it.
  label = 'rewind';
  C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.save.f = 512; C.save.k = 0x1ffffff;
  C.setRadio(2); C.startMission(2, false); d.freezeBots(true); d.clearCrates();
  check(M.f & 512, 'Second Chance armed');
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  for (const t of T) { t.command.left = t.command.right = 0; t.slideX = t.slideZ = 0; }
  d.setTankPosition(0, -1, -5); d.stepSimulation(210, DT);
  d.setTankPosition(0, 2.5, -5.5); d.stepSimulation(6, DT);
  d.setTankPosition(1, -1, -5);
  d.damageTank(0, 9999, 2.5, -3.5); d.stepSimulation(2, DT);
  check(M.rewound && T[0].active, 'lethal hit rewound');
  const rewoundGap = Math.hypot(T[0].x - T[1].x, T[0].z - T[1].z)
    - T[0].collisionRadius - T[1].collisionRadius;
  check(rewoundGap >= -1e-6, 'rewound hull clear of the tank now there (' + rewoundGap.toFixed(3) + ')');
  check(!B.circleHitsObstacle(T[0].x, T[0].z, T[0].collisionRadius, true), 'rewound hull clear of scenery');
  d.freezeBots(false); C.menu.toMain(); C.loadSave('');

  // A shell entering a concave corner (generated arena 3284068757: a .25
  // slit between two walls) reflected one axis into the other wall and sat
  // inside it for a frame. Shells now sweep their path: every contact lies
  // on a wall's face (.1 out, the shell's radius), the shell never comes
  // nearer a wall than that, and here it ricochets between the slit's two
  // faces (the slit is wider than the shell) until its bounces run out.
  label = 'corner';
  C.menu.toMain(); d.selectMode(3); d.selectClass(1); d.start();
  for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
  check(d.generateArenaSeed(3284068757).accepted, 'corner arena');
  d.setWave(1); d.freezeBots(true); d.clearCrates();
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  const generated = __treadlineArenaData.arenas[3];
  const wallGap = (x, z) => {
    let nearest = Infinity;
    for (let k = 0; k < generated.obstacleCount; k++) {
      const o = generated.obstacles[k];
      const dx = Math.max(0, Math.abs(x - o[0]) - o[2] / 2);
      const dz = Math.max(0, Math.abs(z - o[1]) - o[3] / 2);
      nearest = Math.min(nearest, Math.hypot(dx, dz));
    }
    return nearest;
  };
  d.setTankPosition(0, .042, -2.895); d.setTankHeading(0, -1.01);
  T[0].command.aimX = Math.sin(-1.01); T[0].command.aimZ = Math.cos(-1.01);
  d.setKeyboard('fire', true); d.stepSimulation(1, DT); d.setKeyboard('fire', false);
  let turns = 0, closest = Infinity, worstTurn = 0, flippedZ = 0;
  for (let frame = 0; frame < 20; frame++) {
    const before = d.bullets.map(b => b.vz);
    d.stepSimulation(1, DT);
    d.bullets.forEach((shell, at) => {
      if (!shell.active && !shell.turns) return;
      for (let turn = 0; turn < shell.turns; turn++) {
        turns++;
        worstTurn = Math.max(worstTurn, Math.abs(wallGap(d.bulletTurns[at * 8 + turn * 2],
          d.bulletTurns[at * 8 + turn * 2 + 1]) - .1));
      }
      if (shell.vz !== before[at]) flippedZ++;
      if (shell.active) closest = Math.min(closest, wallGap(shell.x, shell.z));
    });
  }
  check(turns >= 2 && flippedZ >= 1, 'shell ricocheted in the slit ' + turns);
  check(worstTurn < 1e-6, 'every contact on a face ' + worstTurn);
  check(closest > .1 - 1e-6, 'shell never nearer a wall than its radius ' + closest);
  d.freezeBots(false); C.menu.toMain();

  // Reprise Notice: the generated arena's crates sit where zone walls
  // later slide in. No crate may end up inside a wall.
  label = 'zone crates';
  C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.save.e = 1; C.save.rep = 1;
  C.setRadio(2); C.startMission(20, false); d.freezeBots(true);
  check(B.state.arena === 3, 'Reprise Notice on its generated arena');
  M.t = 105; d.stepSimulation(4, DT);
  check(M.zone === 3, 'every zone powered off');
  const crushed = B.crates.filter(c => c.active && B.barriers.some(b => b.present && b.active
    && c.x + .22 > b.left && c.x - .22 < b.right && c.z + .22 > b.top && c.z - .22 < b.bottom));
  check(crushed.length === 0, 'no crate inside a zone wall');
  d.freezeBots(false); C.menu.toMain(); C.loadSave('');

  // The Mirror fault deploys on the far side; a redeploy must too, not on
  // the foes' spawn arc.
  label = 'mirror redeploy';
  for (const mission of [2, 18]) {
    C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.save.f = 128; C.save.k = 0x1ffffff;
    C.setRadio(2); C.startMission(mission, false); d.freezeBots(true);
    const startZ = T[0].z;
    d.damageTank(0, 9999, T[0].x, T[0].z + 1); d.stepSimulation(3, DT);
    check(B.state.lives === 2 && T[0].active, 'redeployed ' + mission);
    check(Math.sign(T[0].z) === Math.sign(startZ) && Math.abs(T[0].z) > 4.5,
      `redeploy on the mirrored side (${mission}: ${T[0].x.toFixed(2)}, ${T[0].z.toFixed(2)})`);
    check(!B.circleHitsObstacle(T[0].x, T[0].z, T[0].collisionRadius, true), 'redeploy clear');
  }
  d.freezeBots(false); C.menu.toMain(); C.loadSave('');

  // A Scout wedged between arena 2's east barrier corner and the wall
  // beside it (the gap is narrower than its hull) could neither reverse
  // nor arc out and sat there for the rest of a mission. A bot blocked
  // through a whole avoidance manoeuvre is shoved clear.
  label = 'wedge';
  quick(1, 1); d.clearCrates();
  for (const id of [1, 2, 4, 5]) d.setTankActive(id, false);
  const wedged = T[3];
  check(wedged.classId === 0, 'a Scout to wedge');
  d.setTankPosition(3, 2.873, 1.542); d.setTankHeading(3, -2.42);
  check(!B.circleHitsObstacle(wedged.x, wedged.z, wedged.collisionRadius, true), 'wedge spot clear');
  wedged.command.left = wedged.command.right = 1; wedged.command.reverse = false;
  d.stepSimulation(150, DT);
  check(Math.hypot(wedged.x - 2.873, wedged.z - 1.542) > .1, 'wedged bot moved out');
  check(!B.circleHitsObstacle(wedged.x, wedged.z, wedged.collisionRadius, true), 'shoved clear of scenery');
  d.freezeBots(false); C.menu.toMain();

  // Quick Match foes deploy on the far arc, at least 6 units from the
  // player. The old ring put the nearest foe 1.59 units away in arena 3
  // and inside the convoy in Convoy Escort.
  label = 'spawn distance';
  function nearestFoe() {
    let best = Infinity;
    for (let id = 1; id < 6; id++) {
      const t = T[id];
      if (t.active && t.team !== T[0].team)
        best = Math.min(best, Math.hypot(t.x - T[0].x, t.z - T[0].z));
    }
    return best;
  }
  for (const mode of [0, 1, 2, 5]) {
    for (let arena = 0; arena < 3; arena++) {
      C.menu.toMain(); d.selectMode(mode); d.start(); B.beginArena(arena);
      check(nearestFoe() >= 6, `mode ${mode} arena ${arena + 1}: nearest foe `
        + nearestFoe().toFixed(2));
    }
  }
  for (const mode of [3, 4]) {
    C.menu.toMain(); d.selectMode(mode); d.start();
    for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
    for (let seed = 1; seed <= (mode === 3 ? 6 : 1); seed++) {
      if (mode === 3) d.generateArenaSeed(Math.imul(seed, 0x2545f491) >>> 0);
      for (const wave of [1, 3, 5, 8, 10]) {
        d.setWave(wave);
        check(nearestFoe() >= 6, `mode ${mode} seed ${seed} wave ${wave}: nearest foe `
          + nearestFoe().toFixed(2));
      }
    }
  }
  // A Team Control respawn with the player parked on the red base moves
  // to the far side instead of appearing beside the player.
  quick(1); d.clearCrates();
  d.setTankPosition(0, 2.2, 5.2);
  check(d.destroyTank(3), 'red tank destroyed');
  d.stepSimulation(120, DT);
  check(T[3].active, 'red tank respawned');
  check(Math.hypot(T[3].x - T[0].x, T[3].z - T[0].z) >= 6,
    `respawn ${Math.hypot(T[3].x - T[0].x, T[3].z - T[0].z).toFixed(2)} from the player`);
  d.freezeBots(false); C.menu.toMain();

  // The convoy crawler is solid: hulls cannot drive through it, it nudges
  // a hull standing in its path, and a hull pinned in front of it (here by
  // a parked ally) stalls it rather than being crushed. Tanks used to
  // drive straight through it.
  label = 'convoy';
  const V = B.convoy;
  function convoyDepth(t) {
    const ax = Math.abs(t.x - V.x) - .75, az = Math.abs(t.z - V.z) - .925;
    if (ax <= 0 && az <= 0) return t.collisionRadius - Math.max(ax, az);
    const dx = Math.max(0, ax), dz = Math.max(0, az);
    return Math.max(0, t.collisionRadius - Math.hypot(dx, dz));
  }
  quick(2); d.clearCrates(); d.selectControls(1);
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  check(V.active && Math.abs(V.x + 1.8) < 1e-9 && Math.abs(V.z + 4.8) < 1e-9, 'convoy at its start');
  d.setTankPosition(0, V.x + 2.4, V.z); d.setTankHeading(0, -Math.PI / 2);
  deepest = 0;
  d.setKeyboard('w', true);
  for (let frame = 0; frame < 90; frame++) {
    d.stepSimulation(1, DT); deepest = Math.max(deepest, convoyDepth(T[0]));
  }
  d.setKeyboard('w', false);
  check(deepest < .02, 'no drive into the convoy (' + deepest.toFixed(3) + ')');
  check(T[0].x > V.x, 'still on the near side (' + T[0].x.toFixed(2) + ')');
  // Stand in its path: the crawler advances and nudges the hull ahead.
  d.setTankPosition(0, V.x, V.z + .925 + T[0].collisionRadius + .005);
  const progressBefore = V.progress, aheadBefore = T[0].z;
  deepest = 0;
  for (let frame = 0; frame < 60; frame++) {
    d.stepSimulation(1, DT); deepest = Math.max(deepest, convoyDepth(T[0]));
  }
  check(V.progress > progressBefore + .05, 'convoy advanced (' + V.progress.toFixed(3) + ')');
  check(T[0].z > aheadBefore + .3, 'hull nudged ahead (' + T[0].z.toFixed(2) + ')');
  check(deepest < .02, 'nudged hull never inside (' + deepest.toFixed(3) + ')');
  // Pin it: an ally parked right in front of the player stops the nudge.
  const ally = T[1];
  d.setTankActive(1, true); ally.team = 0; ally.command.left = ally.command.right = 0;
  d.setTankPosition(1, T[0].x, T[0].z + T[0].collisionRadius + ally.collisionRadius + .01);
  d.stepSimulation(30, DT);
  const pinned = V.progress;
  deepest = 0;
  for (let frame = 0; frame < 60; frame++) {
    d.stepSimulation(1, DT); deepest = Math.max(deepest, convoyDepth(T[0]));
  }
  check(Math.abs(V.progress - pinned) < 1e-9, 'pinned hull stalls the convoy');
  check(deepest < .02, 'pinned hull not crushed (' + deepest.toFixed(3) + ')');
  check(Math.abs(V.z - (-4.8 + V.progress * 10.6)) < 1e-9, 'progress matches position');
  d.setTankActive(1, false);
  d.stepSimulation(30, DT);
  check(V.progress > pinned, 'convoy resumes once the way is clear');
  d.selectControls(0); d.freezeBots(false); C.menu.toMain();

  // Guards hold cover, then escalate: a lone guard (only guards left)
  // leaves cover and engages a player who never moves, and a guard beside
  // a teammate that sees no damage exchange for ten seconds swaps its cover
  // spot for a flanking goal. Guards used to sit behind cover until the
  // match timed out.
  label = 'guard';
  quick(0); d.freezeBots(false); d.clearCrates();
  const guard = T[3];
  check(guard.active && guard.role === 'GUARD', 'Survival deploys a guard');
  for (const id of [1, 2, 4, 5]) d.setTankActive(id, false);
  let firstHit = -1;
  for (let frame = 0; frame < 45 * 30 && firstHit < 0; frame++) {
    d.stepSimulation(1, DT);
    if (T[0].health < T[0].maxHealth) firstHit = frame / 30;
  }
  check(firstHit >= 0, 'a lone guard engages the player');
  C.menu.toMain();
  // An untouchable player: no exchange can happen, so patience runs out.
  quick(0); d.freezeBots(false); d.clearCrates();
  for (const id of [2, 4, 5]) d.setTankActive(id, false);
  T[1].inert = true; T[1].command.left = T[1].command.right = 0;
  function flankMiss(g = guard) {
    const side = g.id & 1 ? 1 : -1;
    return Math.hypot(g.strategyGoalX - (T[0].x + (T[0].z - g.z) * .38 * side),
      g.strategyGoalZ - (T[0].z - (T[0].x - g.x) * .38 * side));
  }
  for (let frame = 0; frame < 16 * 30; frame++) {
    T[0].spawnGrace = 5;
    d.stepSimulation(1, DT);
    if (frame === 6 * 30) check(flankMiss() > .5, 'a guard with a teammate holds cover first ('
      + flankMiss().toFixed(2) + ')');
  }
  check(flankMiss() < .5, 'a guard flanks after ten quiet seconds (' + flankMiss().toFixed(2) + ')');
  T[1].inert = false; C.menu.toMain();
  // Campaign 2-3 is a fortress of guards: they keep their posts however
  // quiet it gets, and only the last one standing comes out.
  C.loadSave(''); C.save.d = 1; C.save.f = 0; C.save.k = 0x1ffffff; C.setRadio(2);
  C.startMission(C.missions.findIndex((m) => m.id === '2-3'), false);
  const posts = [1, 2, 3, 4, 5].filter((id) => T[id].active && T[id].role === 'GUARD');
  check(posts.length >= 2, 'mission 2-3 posts guards');
  for (let frame = 0; frame < 16 * 30; frame++) { T[0].spawnGrace = 5; d.stepSimulation(1, DT); }
  for (const id of posts) check(flankMiss(T[id]) > .5, `2-3 guard ${id} holds its post`);
  for (const id of posts.slice(1)) d.destroyTank(id);
  for (let frame = 0; frame < 30; frame++) { T[0].spawnGrace = 5; d.stepSimulation(1, DT); }
  check(flankMiss(T[posts[0]]) < .5, 'the last 2-3 guard comes out ('
    + flankMiss(T[posts[0]]).toFixed(2) + ')');
  C.menu.toMain(); C.loadSave('');

  // Mirror turns every hull around at deploy; the player's aim must turn
  // with it, or the turret swings back to face the wall behind it.
  label = 'mirror aim';
  C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.save.f = 128; C.save.k = 0x1ffffff;
  C.setRadio(2); C.startMission(2, false); d.freezeBots(true);
  const facing = T[0].turret;
  d.stepSimulation(45, DT);
  check(Math.abs(Math.atan2(Math.sin(T[0].turret - facing), Math.cos(T[0].turret - facing))) < .01,
    'turret keeps its mirrored facing (' + T[0].turret.toFixed(2) + ')');
  d.freezeBots(false); C.menu.toMain(); C.loadSave('');

  // A bot routes through a gap barely wider than its hull (2r + .2, on a
  // navigation cell centre) to reach a target beyond a wall, for every hull
  // size, within a bounded time. With one Bulwark-clearance grid a Scout or
  // Striker saw no way through and drove at the wall; any hull clipping the
  // gap's corner stopped dead, backed off and arced, and dithered there.
  label = 'narrow gap';
  for (let cls = 0; cls < 3; cls++) {
    quick(0, 1); d.clearCrates();
    for (let id = 2; id < 6; id++) d.setTankActive(id, false);
    const bot = T[1], radius = [.86, 1, 1.14][cls] * .52;
    d.setTankActive(1, true);
    bot.classId = cls; bot.collisionRadius = radius; bot.scale = B.CLASSES[cls].scale;
    bot.driveSpeed = B.CLASSES[cls].speed; bot.role = 'HUNTER'; bot.boss = false; bot.team = 1;
    const half = radius + .1, gapX = .5, lineTop = -5.7, lineBottom = -5.3;
    check(d.setBarrierRect(3, -7.9, gapX - half, lineTop, lineBottom)
      && d.setBarrierRect(4, gapX + half, 7.9, lineTop, lineBottom), 'gap walls');
    d.setTankPosition(1, -4, -6.8); d.setTankHeading(1, Math.PI / 2);
    d.setTankPosition(0, -1.2, -1);
    check(!B.circleHitsObstacle(bot.x, bot.z, radius, true), 'bot starts clear');
    d.freezeBots(false);
    let crossed = -1;
    for (let frame = 0; frame < 300 && crossed < 0; frame++) {
      T[0].spawnGrace = 5; T[0].health = T[0].maxHealth;
      d.stepSimulation(1, DT);
      if (bot.z > lineBottom + radius) crossed = frame * DT;
    }
    check(crossed >= 0, `class ${cls} passes the gap within 10 s (at ${bot.x.toFixed(2)}, ${bot.z.toFixed(2)})`);
    d.freezeBots(true); C.menu.toMain();
  }

  // An Onslaught forge never outlives its run. One left unfinished (early,
  // or in its last phases, which only rendered frames complete) kept the
  // next mission or Quick Match inside the forge's phase machine, where
  // nothing moves and nothing ever clears.
  label = 'forge leftover';
  for (const forgeSteps of [3, -1]) {
    C.menu.toMain(); d.selectMode(3); d.start(); d.freezeBots(true);
    if (forgeSteps > 0) d.step(forgeSteps);
    else for (let at = 0; at < 900 && B.state.mode !== 'playing'; at++) d.step(1);
    check(d.snapshot().arenaGenerationPhase, `forge still running (${forgeSteps})`);
    B.setMode('game-over');
    C.menu.toMain(); C.loadSave(''); C.save.d = 1; C.setRadio(2);
    check(C.startMission(0, false), 'mission starts');
    d.freezeBots(true);
    check(!d.snapshot().arenaGenerationPhase, `no forge in the mission (${forgeSteps})`);
    for (let id = 1; id < 6; id++)
      if (T[id].active) d.damageTank(id, 999, T[id].x, T[id].z - 2);
    for (let at = 0; at < 120 && !C.runtime.ended; at++) d.stepSimulation(1, DT);
    check(C.runtime.ended && B.state.mode === 'victory', `the mission clears (${forgeSteps}) `
      + B.state.mode);
    C.menu.toMain(); C.loadSave('');
    quick(0);
    for (let id = 1; id < 6; id++)
      if (T[id].active && T[id].team !== T[0].team) d.damageTank(id, 999, T[id].x, T[id].z - 2);
    for (let at = 0; at < 120 && B.state.mode === 'playing'; at++) d.stepSimulation(1, DT);
    check(B.state.mode === 'arena-clear', `Quick Match clears (${forgeSteps}) ` + B.state.mode);
  }
  d.freezeBots(false); C.menu.toMain();

  globalThis.pocSummary = 'TREADLINE-PLACEMENT-PASS';
})();
