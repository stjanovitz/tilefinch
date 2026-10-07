/* Aim guide: the shared shell sweep against real shells in seeded arenas, the
   cache and its invalidation, Full's concealment rules, the Off default,
   persistence and score cost (the highest level used prices a run, also in
   replays), the reserved fault slot 8, instance admission, tracers,
   and replay determinism across levels. Synthetic inputs only (debug seams, keyboard state). */
(() => {
  const d = __treadlineDebug, C = d.campaign, P = d.practice;
  const B = C.bridge(), menu = C.menu;
  const $ = (id) => document.getElementById(id);
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(label + ': ' + what);
  }
  const savedCampaign = JSON.stringify(C.save);
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  d.setCommandEnabled(false); d.selectControls(0); d.selectAimAssist(0);
  d.setAimGuide(0);
  const stats = {};

  // ------------------------------------------------- cast vs real shells --
  /* Seeded arenas (the three authored ones and generated layouts), random
     clear spots and headings. The guide is read at the frame the shell is
     released; the real shell then flies through the full simulation. Both
     run the same shell sweep, so the shell meets its first contact exactly
     where leg 1 ends (to float rounding), turns exactly as predicted (also
     at rounded corners, face ends and concave corners, whatever the frame
     rate) and meets its next contact where leg 2 ends. */
  label = 'cast';
  menu.show('quick'); d.selectMode(0); d.start();
  d.freezeBots(true);
  let seed = 0x2f6e2b1;
  const random = () => {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return (seed >>> 0) / 4294967296;
  };
  function scene(arena, generatedSeed, gameMode) {
    B.state.gameMode = gameMode;
    if (arena === 3) {
      B.arenaGenerator.materialize(generatedSeed);
      B.fillArenaSpatialData(3); B.dirty(true);
    }
    B.beginArena(arena);
    B.setMode('playing');
    for (let id = 1; id < 6; id++) d.setTankActive(id, false);
    for (const bullet of d.bullets) bullet.active = false;
  }
  function place(x, z, angle) {
    const p = B.tanks[0];
    d.setTankPosition(0, x, z); d.setTankHeading(0, angle);
    p.turret = angle; B.updateTankTurretCache(p);
    p.slideX = p.slideZ = 0; p.cooldown = 0; p.fireCharge = 0; p.fireHeld = false;
    p.command.aimX = Math.sin(angle); p.command.aimZ = Math.cos(angle);
  }
  const playerShell = () => d.bullets.find(b => b.active && b.owner === 0);
  /* Hold fire for one step, read the guide, release: returns the guide and
     the shell's path as points [kind, x, z] (the spawn, each contact it
     turned at, where it stopped or ran out of range), read from the
     contacts each step reports. */
  function shoot(dt) {
    d.setKeyboard('fire', true); d.stepSimulation(1, dt);
    for (const bullet of d.bullets) if (!bullet.active) { bullet.life = -1; bullet.turns = 0; }
    const guide = d.aimGuide(2), sight = d.aimGuide(1);
    d.setKeyboard('fire', false);
    d.stepSimulation(1, dt);
    const at = d.bullets.findIndex(b => b.owner === 0 && b.life > 0);
    const shell = d.bullets[at];
    const points = [['spawn', guide.originX, guide.originZ]];
    for (let step = 0; step < 3 / dt + 8 && points.length < 4; step++) {
      for (let turn = 0; turn < shell.turns; turn++)
        points.push(['bounce', d.bulletTurns[at * 8 + turn * 2], d.bulletTurns[at * 8 + turn * 2 + 1]]);
      if (!shell.active) { points.push(['end', shell.x, shell.z]); break; }
      d.stepSimulation(1, dt);
    }
    for (const bullet of d.bullets) bullet.active = false;
    return {guide, sight, points};
  }
  const sign = (v) => v > 1e-9 ? 1 : v < -1e-9 ? -1 : 0;
  function onRay(x, z, ox, oz, dx, dz) {
    const fx = x - ox, fz = z - oz, along = fx * dx + fz * dz;
    return {along, off: Math.abs(fx * dz - fz * dx)};
  }
  const EXACT = 1e-6;
  function runCorpus(dt, shotsPerScene, scenes, aimCorners = false) {
    const out = {shots: 0, faces: 0, faceAgree: 0, corners: 0, cornerAgree: 0, arcs: 0,
      stops: 0, ranges: 0, leg2: 0, leg2Agree: 0, stubShort: 0, worst: 0, worst2: 0,
      muzzleBlocked: 0,
      overCover: 0, failures: []};
    for (let at = 0; at < scenes; at++) {
      const arena = at < 3 ? at : 3;
      scene(arena, Math.imul(at + 11, 0x45d9f3b) >>> 0, at & 1);
      for (let shot = 0; shot < shotsPerScene; shot++) {
        let x = 0, z = 0;
        for (let tries = 0; tries < 60; tries++) {
          x = random() * 14.4 - 7.2; z = random() * 14.4 - 7.2;
          // Team Control fills its meter in the centre and opens the gate.
          if (!d.circleBlocked(x, z, .75) && (B.state.gameMode !== 1 || x * x + z * z > 6))
            break;
        }
        if (d.circleBlocked(x, z, .75)) continue;
        let angle = (random() * 2 - 1) * Math.PI;
        if (aimCorners) {
          // Aim at a wall corner's rounded arc (or just past it).
          const spatial = __treadlineArenaData.spatial;
          const bounds = spatial.obstacleBounds[arena];
          const at = (random() * spatial.obstacleCounts[arena] | 0) * 4;
          const cx = random() < .5 ? bounds[at] : bounds[at + 1];
          const cz = random() < .5 ? bounds[at + 2] : bounds[at + 3];
          const ox = cx < (bounds[at] + bounds[at + 1]) / 2 ? -1 : 1;
          const oz = cz < (bounds[at + 2] + bounds[at + 3]) / 2 ? -1 : 1;
          const spread = random() * .14;
          angle = Math.atan2(cx + ox * spread * .7 - x, cz + oz * spread * .7 - z)
            + (random() - .5) * .02;
        }
        place(x, z, angle);
        const {guide, sight, points} = shoot(dt);
        out.shots++;
        if (guide.reach === 0) out.muzzleBlocked++;
        if (d.elevatedFiringOrigin(B.tanks[0])) out.overCover++;
        const fail = (what) => out.failures.push({what, arena, x, z,
          turret: B.tanks[0].turret, guide: [guide.kind1, guide.length1, guide.hitX,
            guide.hitZ, guide.normalX, guide.normalZ, guide.bounceX, guide.bounceZ,
            guide.kind2, guide.length2], points: JSON.stringify(points)});
        const first = points[1];
        if (!first) { fail('no path'); continue; }
        const ray = onRay(first[1], first[2], guide.originX, guide.originZ, guide.dirX, guide.dirZ);
        const miss = Math.abs(guide.length1 - ray.along);
        if (guide.kind1 !== 1) {
          if (guide.kind1 === 2) out.stops++; else out.ranges++;
          out.worst = Math.max(out.worst, miss, ray.off);
          if (first[0] !== 'end') fail('predicted a stop, shell bounced');
          else if (miss > EXACT || ray.off > EXACT) fail('stop point ' + miss.toFixed(7));
          continue;
        }
        // Rounded corners and face ends (contact within a step and the
        // radius of the face's end) were frame-phase dependent before the
        // sweep; now they must agree as exactly as flat faces.
        const corner = guide.arc || guide.edgeGap < 7.2 / 30 + .1;
        if (corner) out.corners++; else out.faces++;
        if (guide.arc) out.arcs++;
        const next = points[2];
        let agree = first[0] === 'bounce' && miss <= EXACT && ray.off <= EXACT && next;
        if (agree) {
          // The direction it left in: towards its next point.
          agree = sign(next[1] - first[1]) === sign(guide.bounceX)
            && sign(next[2] - first[2]) === sign(guide.bounceZ);
        }
        out.worst = Math.max(out.worst, miss, ray.off);
        if (!agree) { fail((corner ? 'corner ' : 'face ') + 'ricochet ' + miss.toFixed(7)); continue; }
        if (corner) out.cornerAgree++; else out.faceAgree++;
        // Leg 2 ends where the shell's next contact (or its end) is.
        if (guide.length2 <= 0) continue;
        out.leg2++;
        const leg = onRay(next[1], next[2], guide.hitX, guide.hitZ, guide.bounceX, guide.bounceZ);
        const miss2 = Math.abs(guide.length2 - leg.along);
        out.worst2 = Math.max(out.worst2, miss2, leg.off);
        const sameKind = (next[0] === 'bounce') === (guide.kind2 === 1);
        if (sameKind && miss2 <= EXACT && leg.off <= EXACT) out.leg2Agree++;
        else fail('leg 2 ' + miss2.toFixed(7) + ' ' + next[0] + '/' + guide.kind2);
        // Sight's stub is the same sweep, cut at its own length: it stops
        // at any face, barrier, crate or arena edge Full's leg 2 meets.
        if (guide.length2 < .6) out.stubShort++;
        if (sight.length2 !== Math.min(.6, guide.length2))
          fail('sight stub ' + sight.length2 + ' vs ' + guide.length2);
      }
    }
    return out;
  }
  const corners = runCorpus(1 / 240, 20, 23, true);
  stats.corners = {...corners, failures: corners.failures.slice(0, 3)};
  globalThis.__aimGuideStats = stats;
  check(corners.corners >= 60 && corners.arcs >= 30, 'the corpus aims at corners '
    + JSON.stringify(stats.corners));
  check(!corners.failures.length && corners.cornerAgree === corners.corners
    && corners.faceAgree === corners.faces && corners.leg2Agree === corners.leg2,
    'corner and face-end ricochets agree exactly ' + JSON.stringify(stats.corners));
  const fine = runCorpus(1 / 240, 14, 23);
  stats.fine = {...fine, failures: fine.failures.slice(0, 3)};
  check(fine.shots >= 250 && fine.faces >= 120 && fine.stops + fine.ranges >= 10
    && fine.leg2 >= 60, 'corpus covers faces, stops and bounce legs ' + JSON.stringify(stats.fine));
  check(!fine.failures.length, 'every ricochet, stop and bounce leg agrees at 1/240 s: '
    + JSON.stringify(stats.fine));
  // Device frames (1/30 s): the same contacts, exactly; the path does not
  // depend on the frame rate.
  const coarse = runCorpus(1 / 30, 6, 12);
  const coarseCorners = runCorpus(1 / 30, 8, 12, true);
  stats.coarse = {...coarse, failures: coarse.failures.slice(0, 3)};
  stats.coarseCorners = {...coarseCorners, failures: coarseCorners.failures.slice(0, 3)};
  check(!coarse.failures.length && !coarseCorners.failures.length
    && coarseCorners.corners >= 20, 'device-step contacts agree exactly '
    + JSON.stringify([stats.coarse, stats.coarseCorners]));
  check(Math.max(corners.worst, fine.worst, coarse.worst, coarseCorners.worst) <= EXACT,
    'contacts on the predicted rays');

  globalThis.__aimGuideStats = stats;

  // ------------------------------------------------------------- cache --
  /* A still tank recasts nothing; breaking a barrier (a real shell),
     re-raising it, opening or sealing a gate, exploding a crate and moving
     the tank or turret each recast at once. */
  label = 'cache';
  const near = (a, b, eps) => Math.abs(a - b) <= eps;
  /* A clear spot from which the guide meets the target box first. */
  function aimAt(cx, cz, kind, ring = 2.4) {
    for (let k = 0; k < 48; k++) {
      const a = k * Math.PI / 24, x = cx + Math.sin(a) * ring, z = cz + Math.cos(a) * ring;
      if (Math.abs(x) > 7.1 || Math.abs(z) > 7.1 || d.circleBlocked(x, z, .75)) continue;
      place(x, z, Math.atan2(cx - x, cz - z));
      const g = d.aimGuide(1);
      if (g.kind1 === kind && Math.hypot(g.hitX - cx, g.hitZ - cz) < 1.4) return g;
    }
    return null;
  }
  scene(0, 0, 1);
  const barrierAt = B.barriers.findIndex(b => b.active);
  const barrier = B.barriers[barrierAt];
  check(barrierAt >= 0, 'arena 0 has a barrier');
  const g0 = aimAt((barrier.left + barrier.right) / 2, (barrier.top + barrier.bottom) / 2, 2);
  check(g0, 'a stop at the barrier');
  const still = d.aimGuide(1);
  check(!still.recast && still.casts === g0.casts, 'a still tank reuses the cast');
  d.stepSimulation(5, 1 / 60);
  check(!d.aimGuide(1).recast, 'stepping a still tank reuses the cast');
  barrier.health = 1;
  d.setKeyboard('fire', true); d.stepSimulation(1); d.setKeyboard('fire', false);
  d.stepSimulation(1);
  for (let at = 0; at < 120 && playerShell(); at++) d.stepSimulation(1);
  check(!barrier.active, 'the shell broke the barrier');
  const broken = d.aimGuide(1);
  check(broken.recast && (broken.kind1 !== 2 || broken.length1 > g0.length1 + .3),
    'a broken barrier no longer stops the guide');
  d.setBarrierActive(barrierAt, true);
  const raised = d.aimGuide(1);
  check(raised.recast && raised.kind1 === 2 && near(raised.length1, g0.length1, 1e-9),
    'a raised barrier stops it again');
  B.tanks[0].turret += .2; B.updateTankTurretCache(B.tanks[0]);
  check(d.aimGuide(1).recast, 'turning the turret recasts');
  // Gates: closed in Team Control until a side holds the transmitter.
  const spatial = __treadlineArenaData.spatial;
  let gateArena = -1;
  for (let arena = 0; arena < 3 && gateArena < 0; arena++)
    if (spatial.gateCounts[arena] > 0) gateArena = arena;
  check(gateArena >= 0, 'an authored arena has a gate');
  scene(gateArena, 0, 1);
  check(!B.state.gateOpen, 'Team Control starts sealed');
  const gate = spatial.gateBounds[gateArena];
  const gateX = (gate[0] + gate[1]) / 2, gateZ = (gate[2] + gate[3]) / 2;
  const sealed = aimAt(gateX, gateZ, 1, 2.2);
  check(sealed && sealed.hitX >= gate[0] - .11 && sealed.hitX <= gate[1] + .11
    && sealed.hitZ >= gate[2] - .11 && sealed.hitZ <= gate[3] + .11, 'a sealed gate bounces the guide');
  B.state.gateOpen = true;
  const opened = d.aimGuide(1);
  check(opened.recast && opened.length1 > sealed.length1 + .3, 'an open gate lets it through');
  B.state.gateOpen = false;
  check(d.aimGuide(1).recast && near(d.aimGuide(1).length1, sealed.length1, 1e-9), 'sealing restores the bounce');
  // Crates stop shells; an exploded crate no longer does.
  let crateAt = -1, crateGuide = null;
  for (let arena = 0; arena < 3 && !crateGuide; arena++) {
    scene(arena, 0, 1);
    for (let at = 0; at < B.crates.length && !crateGuide; at++) {
      if (!B.crates[at].active) continue;
      crateGuide = aimAt(B.crates[at].x, B.crates[at].z, 2, 2.2);
      if (crateGuide) crateAt = at;
    }
  }
  check(crateGuide, 'a crate stops the guide');
  d.explodeCrate(crateAt);
  const cleared = d.aimGuide(1);
  check(cleared.recast && (cleared.kind1 !== 2 || cleared.length1 > crateGuide.length1 + .3),
    'an exploded crate no longer stops it');

  // ------------------------------------------------------- concealment --
  /* Full confirms a hit only on an enemy the player can see: smoke on the
     shell's path, a campaign foe still in its camouflage and Fog of Memory
     each confirm nothing, and a hidden hull never shortens the drawn leg. */
  label = 'concealment';
  function openLane(minimum) {
    for (let tries = 0; tries < 400; tries++) {
      const x = random() * 12 - 6, z = random() * 12 - 6;
      if (d.circleBlocked(x, z, .75)) continue;
      place(x, z, (random() * 2 - 1) * Math.PI);
      const g = d.aimGuide(2);
      if (g.kind1 === 1 && g.length1 >= minimum && g.length2 >= 3.2) return g;
    }
    return null;
  }
  function putFoe(id, x, z, team = 1) {
    const tank = B.tanks[id];
    d.setTankActive(id, true); tank.team = team; tank.boss = false;
    d.setTankPosition(id, x, z);
    tank.fireWindup = tank.recoil = 0;
  }
  scene(0, 0, 0);
  const lane = openLane(4.2);
  check(lane, 'an open lane with a bounce leg');
  const ahead = (g, at) => [g.originX + g.dirX * at, g.originZ + g.dirZ * at];
  putFoe(1, ...ahead(lane, 3));
  let full = d.aimGuide(2);
  check(full.enemy === 1 && full.enemyLeg === 1 && !full.hidden
    && near(full.enemyAt, 3 - Math.sqrt(.34), 1e-6), 'Full confirms a visible enemy on leg 1');
  const sightOnly = d.aimGuide(1);
  check(sightOnly.enemy === -1 && sightOnly.length2 <= .651
    && near(sightOnly.bounceX, lane.bounceX, 1e-12) && near(sightOnly.bounceZ, lane.bounceZ, 1e-12),
    'Sight confirms nothing and casts only its stub of the bounce leg');
  d.activateGadget('SMOKE');
  full = d.aimGuide(2);
  check(full.enemy === -1 && full.hidden && near(full.length1, lane.length1, 1e-9),
    'smoke on the path hides the enemy and keeps the full leg');
  d.clearGadgetEffects();
  B.tanks[1].team = 0;
  full = d.aimGuide(2);
  check(full.enemy === -1 && !full.hidden, 'teammates are not confirmed (shells pass them)');
  // On the bounce leg.
  putFoe(1, lane.hitX + lane.bounceX * 2.4, lane.hitZ + lane.bounceZ * 2.4);
  full = d.aimGuide(2);
  check(full.enemy === 1 && full.enemyLeg === 2 && near(full.enemyAt, 2.4 - Math.sqrt(.34), 1e-3),
    'Full confirms an enemy on the bounce leg ' + JSON.stringify([full.enemy, full.enemyLeg, full.enemyAt]));
  d.activateGadget('SMOKE');
  check(d.aimGuide(2).enemy === -1 && d.aimGuide(2).hidden, 'smoke on leg 1 hides a leg-2 enemy');
  d.clearGadgetEffects();
  // Whiteout (3-4): camouflaged foes stay hidden until they fire.
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  C.startMission(13, false); d.freezeBots(true);
  check(C.runtime.m.id === '3-4', 'Whiteout');
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  const camoLane = openLane(4.2);
  check(camoLane, 'a lane in Whiteout');
  putFoe(1, ...ahead(camoLane, 3));
  full = d.aimGuide(2);
  check(full.enemy === -1 && full.hidden && C.concealed(1), 'a camouflaged foe stays hidden');
  B.tanks[1].fireWindup = .2;
  check(d.aimGuide(2).enemy === 1 && !C.concealed(1), 'a firing foe is revealed');
  B.tanks[1].fireWindup = 0; C.runtime.revealAt[1] = C.runtime.t - .5;
  check(d.aimGuide(2).enemy === 1, 'it stays revealed for a moment');
  C.runtime.revealAt[1] = C.runtime.t - 2;
  check(d.aimGuide(2).enemy === -1, 'then hides again');
  // Fog of Memory hides every hit indicator.
  C.loadSave(''); C.save.d = 1; C.save.f = 16; C.save.k = 0x1ffffff; C.setRadio(2); C.writeSave();
  C.startMission(0, false); d.freezeBots(true);
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  const fogLane = openLane(4.2);
  check(fogLane, 'a lane under Fog of Memory');
  putFoe(1, ...ahead(fogLane, 3));
  check(d.aimGuide(2).enemy === -1 && C.hidesHits(), 'Fog of Memory confirms nothing');
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  menu.toMain();

  // ---------------------------------------------- levels and persistence --
  /* Off by default everywhere; Sight and Full are assists that cost score
     (x0.9, x0.75) through the same multiplier path as Range Faults, at the
     highest level a run used while playing. Practice has no score. */
  label = 'levels';
  const KEY = 'treadline-settings-v1';
  d.setAimGuide(0);
  check(d.aimGuideLevel() === 0, 'Off by default');
  d.setAimGuide(2); d.savePreferences();
  check(JSON.parse(localStorage.getItem(KEY)).aimGuide === 2, 'saved with the preferences');
  d.setAimGuide(0); d.reloadPreferences();
  check(d.aimGuideLevel() === 2, 'loaded with the preferences');
  const stored = JSON.parse(localStorage.getItem(KEY));
  for (const bad of [7, -3, 1.5, '2', null]) {
    localStorage.setItem(KEY, JSON.stringify({...stored, aimGuide: bad})); d.reloadPreferences();
    check(d.aimGuideLevel() === 0, 'invalid level falls back to Off ' + bad);
  }
  delete stored.aimGuide; localStorage.setItem(KEY, JSON.stringify(stored)); d.reloadPreferences();
  check(B.preferences.aimGuide === 0, 'an older save starts Off');
  check(JSON.stringify(B.AIM_SCORE_MULTIPLIERS) === '[1,0.9,0.75]', 'multipliers');
  // Every campaign difficulty starts Off too (no Cadet default).
  for (const difficulty of [0, 1, 2]) {
    C.loadSave(''); C.save.d = difficulty; C.setRadio(2); C.writeSave();
    C.startMission(0, false);
    check(d.aimGuideLevel() === 0, `difficulty ${difficulty} starts Off`);
    menu.toMain();
  }
  // The No Sight fault is gone and Toy Box is retired: slot 8 stays empty,
  // and the faults' second page lists only the four that can unlock.
  check(C.faults[8] === null && C.faults[9][0] === 'Second Chance', 'slot 8 reserved');
  menu.show('faults'); menu.switchTab(1);
  const page2 = [...$('menu').children].filter(b => !b.hidden).map(b => b.textContent);
  check(page2.length === 5 && page2[4] === 'Back' && !page2.some(t => t.includes('99')),
    'no unattainable fault is listed ' + page2);
  menu.toMain();
  C.loadSave(''); C.save.k = 0x1ffffff;
  check(C.decode(C.runCode(0, 256)).f === 0 && C.decode((0x80000000 | 256 << 8) >>> 0).f === 0,
    'bit 256 never reaches a run');
  C.loadSave(JSON.stringify({v: 1, f: 0x1ff}));
  check(C.save.f === 0xff, 'a saved bit 256 is dropped as before');
  // Quick Match: the highest level used while playing prices the run.
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  menu.show('quick'); d.selectMode(0); d.setAimGuide(0); d.start(); d.freezeBots(true);
  d.stepSimulation(3);
  check(d.aimScore().level === 0 && d.aimScore().multiplier === 1, 'Off costs nothing');
  B.setMode('paused'); d.setAimGuide(2); d.setAimGuide(0); B.setMode('playing'); d.stepSimulation(3);
  check(d.aimScore().level === 0, 'a level only browsed in the pause menu is not used');
  B.setMode('paused'); d.setAimGuide(1); B.setMode('playing'); d.stepSimulation(2);
  B.setMode('paused'); d.setAimGuide(0); B.setMode('playing'); d.stepSimulation(2);
  B.state.score = 1000;
  check(d.aimScore().level === 1 && d.aimScore().final === 900,
    'switching Off before the end keeps the lowest multiplier used ' + JSON.stringify(d.aimScore()));
  d.setAimGuide(2); d.stepSimulation(1);
  check(d.aimScore().final === 750, 'Full x0.75');
  d.setAimGuide(0);
  B.setMode('game-over'); d.stepSimulation(1);
  check(/Final score 750\./.test($('message').textContent) && /x0\.75/.test($('message').textContent),
    'the end panel shows the priced score ' + $('message').textContent);
  // A replay records the level its run used and scores with it, not the
  // viewer's (a clean run: live bots, nothing edited mid-run).
  menu.show('quick'); d.selectMode(0); d.setAimGuide(1); d.start(); d.freezeBots(false);
  for (let frame = 0; frame < 90; frame++) {
    d.setKeyboard('aimLeft', frame % 40 < 20); d.setKeyboard('fire', frame % 30 < 2);
    d.stepSimulation(1);
  }
  d.setAimGuide(0);
  for (let frame = 0; frame < 30; frame++) d.stepSimulation(1);
  d.setKeyboard('aimLeft', false); d.setKeyboard('fire', false);
  d.replayStop();
  const code = d.exportReplay(), body = code.slice(code.indexOf('.', 4) + 1);
  const header = new DataView(Uint8Array.from(atob(body), ch => ch.charCodeAt(0)).buffer);
  check(header.getFloat64(0, true) === 14 && (header.getFloat64(40, true) >> 1) === 1,
    'replay v14 carries the level the run used');
  d.setAimGuide(2);
  check(d.replayStart(), 'replay starts');
  check(d.aimScore().level === 1, 'a replay scores at its recorded level');
  for (let at = 0; at < 2000 && B.state.mode !== 'replay-done'; at++) d.stepSimulation(1);
  check(d.replayState().verified && d.aimScore().level === 1, 'and still verifies');
  d.setAimGuide(0);
  // Campaign: the cost rides the faults' multiplier in the mission result.
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  d.setAimGuide(1); const started = C.startMission(0, false); d.freezeBots(true);
  check(started && B.state.score === 0, 'mission started ' + [started, B.state.score, B.state.mode]);
  d.stepSimulation(2); d.setAimGuide(0);
  for (let id = 1; id < 5; id++) d.damageTank(id, 999, B.tanks[id].x, B.tanks[id].z - 2);
  for (let at = 0; at < 120 && !C.runtime.ended; at++) d.stepSimulation(1);
  const result = C.runtime.result, M = C.runtime;
  check(result && result.won && result.aim === 1, 'mission result records the level');
  const bonus = (result.medals & 2 ? 1000 : 0) + (result.medals & 4 ? 1000 : 0)
    + Math.max(0, (180 - M.t) * 10 | 0);
  check(result.score === Math.round((B.state.score + 1000 + bonus) * M.mult * .9),
    'mission score x0.9 ' + result.score);
  check(/Guide x0\.9/.test($('message').textContent), 'results show the guide multiplier '
    + $('message').textContent);
  menu.toMain();
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  // The Controls page: third tab, focus, rows with their cost, cycling.
  d.setAimGuide(2);
  menu.show('settings'); menu.show('controls');
  menu.switchTab(-1);
  check(document.activeElement === $('aim-choice') && /Level/.test($('legend').textContent),
    'L/R reaches the Aim guide page with its choice focused');
  const rows = [...$('bindings').children].filter(e => !e.hidden).map(e => e.textContent);
  check(rows.length === 5 && rows[0].startsWith('Off') && /score x0\.9$/.test(rows[1])
    && /score x0\.75$/.test(rows[2]) && rows[3].startsWith('Tracers'), 'rows show the cost ' + rows);
  const lit = [...$('bindings').children].slice(0, 3).map(e => e.firstChild.className);
  check(lit.join() === 'off,off,', 'the current level is lit ' + lit);
  $('aim-choice').click();
  check(d.aimGuideLevel() === 0 && $('aim-value').textContent === 'Off'
    && JSON.parse(localStorage.getItem(KEY)).aimGuide === 0, 'X cycles and saves');
  dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowLeft', cancelable: true}));
  dispatchEvent(new KeyboardEvent('keyup', {key: 'ArrowLeft', cancelable: true}));
  check(d.aimGuideLevel() === 2, 'left cycles back');
  menu.switchTab(1);
  // (Layout of each page is the menu layout gate's job.)
  check(document.activeElement === $('controls-choice')
    && !menu.focusables().includes($('aim-choice')), 'the Driving page leaves the guide choice out');
  menu.toMain();
  // Practice Range: Off at first, its own quick toggle, no score cost.
  d.setAimGuide(0);
  P.enter(0); d.step(2);
  check(d.aimGuideLevel() === 0, 'the range starts Off');
  B.setMode('paused'); d.stepSimulation(1);
  const rangeButton = () => [...$('menu').children].find(b => !b.hidden && b.textContent.startsWith('Aim guide'));
  check(rangeButton() && rangeButton().textContent === 'Aim guide Off', 'Range panel toggle');
  rangeButton().click();
  check(d.aimGuideLevel() === 1 && rangeButton().textContent === 'Aim guide Sight', 'one press: Sight');
  rangeButton().click();
  check(d.aimGuideLevel() === 2, 'then Full');
  B.startOrResume(); d.stepSimulation(3);
  check(d.aimScore().level === 0, 'the range never prices a level');
  B.setMode('paused'); d.stepSimulation(1);
  menu.show('controls'); menu.switchTab(-1); $('aim-choice').click();
  check(d.aimGuideLevel() === 0 && B.preferences.aimGuide === 0, 'Controls changes the range level in the range');
  menu.back();
  P.leave(); menu.toMain();
  check(d.aimGuideLevel() === 0, 'leaving the range restores the saved level');
  P.runtime.aim = 0;
  d.setAimGuide(0);

  // --------------------------------------------------------- instances --
  /* Off draws nothing; Sight at most 3 boxes, Full at most 5, within the
     frame ceiling; a saturated frame admits the lines whole or drops them. */
  label = 'instances';
  menu.show('quick'); d.selectMode(0); d.start(); d.freezeBots(true);
  const lineup = openLane(4.2);
  check(lineup, 'a lane for drawing');
  putFoe(1, lineup.hitX + lineup.bounceX * 2.4, lineup.hitZ + lineup.bounceZ * 2.4);
  const drawn = [];
  for (const level of [0, 1, 2]) {
    d.setAimGuide(level); d.step(1);
    const snap = d.snapshot();
    drawn.push(snap.aimBoxes);
    check(snap.boxInstances <= snap.frameInstanceCeiling, 'within the frame ceiling ' + level);
  }
  check(drawn[0] === 0 && drawn[1] === 3 && drawn[2] === 5,
    'guide boxes per level ' + drawn);
  const drops0 = d.snapshot().aimDrops;
  d.saturateVisualLoad(); d.stepBudgetedHud(2);
  const sat = d.snapshot();
  check(sat.boxInstances <= sat.frameInstanceCeiling, 'a saturated frame stays capped');
  check(sat.aimDrops > drops0 || sat.aimBoxes >= drawn[2] - 1, 'lines admitted whole or dropped ' + JSON.stringify([sat.aimBoxes, sat.aimDrops]));
  d.setAimGuide(0);

  // ----------------------------------------------------------- tracers --
  /* The player's shell leaves a trail of its real path: a vertex at the
     spawn and at each ricochet, drawn in one box per straight piece, fading after
     the shell ends. Other tanks' shells leave none. */
  label = 'tracers';
  menu.show('quick'); d.selectMode(0); d.start(); d.freezeBots(true);
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  check(openLane(2.5), 'a lane for a tracer');
  // Fire without moving the camera (camera-relative aim would drift).
  d.setKeyboard('fire', true); d.stepSimulation(1);
  const shotLane = d.aimGuide(1);
  d.setKeyboard('fire', false); d.stepSimulation(1);
  let traced = d.tracerState();
  check(traced.slots.length === 1 && traced.slots[0].live
    && near(traced.slots[0].vertices[0], shotLane.originX, 1e-5)
    && near(traced.slots[0].vertices[1], shotLane.originZ, 1e-5), 'a trail starts at the spawn ' + JSON.stringify(traced));
  let maxBoxes = 0;
  for (let at = 0; at < 200 && d.tracerState().slots[0].vertices.length < 4; at++) {
    d.step(1); maxBoxes = Math.max(maxBoxes, d.snapshot().tracerBoxes);
  }
  traced = d.tracerState();
  check(traced.slots[0].vertices.length >= 4, 'a ricochet adds a vertex');
  check(Math.hypot(traced.slots[0].vertices[2] - shotLane.hitX,
    traced.slots[0].vertices[3] - shotLane.hitZ) <= 7.2 / 60 + 1e-3, 'at the real ricochet');
  check(maxBoxes >= 1 && maxBoxes <= 2, 'one or two boxes per trail ' + maxBoxes);
  for (let at = 0; at < 300 && playerShell(); at++) d.step(1);
  check(!d.tracerState().slots[0].live, 'the trail outlives the shell briefly');
  d.step(30);
  check(d.tracerState().slots.length === 0 && d.snapshot().tracerBoxes === 0, 'then fades out');
  // The bot sits up the player's clear lane, so it has a line to fire on.
  let botX = shotLane.originX + shotLane.dirX * 2.2, botZ = shotLane.originZ + shotLane.dirZ * 2.2;
  if (d.circleBlocked(botX, botZ, .75)) {
    botX = shotLane.originX + shotLane.dirX * 1.7; botZ = shotLane.originZ + shotLane.dirZ * 1.7;
  }
  d.setTankActive(1, true); d.setTankPosition(1, botX, botZ);
  B.tanks[1].cooldown = 0; d.stats.shots[1] = 0; d.armBotShot(1); d.freezeBots(false);
  // Its shell may end within a step (whatever it meets): watch the shot.
  let botTrail = false;
  for (let at = 0; at < 90 && !d.stats.shots[1]; at++) {
    d.step(1); botTrail = botTrail || d.tracerState().slots.length > 0;
  }
  d.freezeBots(true);
  check(d.stats.shots[1] > 0 && !botTrail && d.tracerState().slots.length === 0,
    'a bot shell leaves no trail');

  // ------------------------------------------------------- determinism --
  /* A run recorded with Full replays, digest-exact, with Off and Sight:
     the guide and tracers never feed the simulation, bots or replays. */
  label = 'determinism';
  d.freezeBots(false);
  for (const replayLevel of [0, 1]) {
    d.setAimGuide(2);
    menu.show('quick'); d.selectMode(0); d.start();
    for (let frame = 0; frame < 360; frame++) {
      d.setKeyboard('w', frame % 90 < 50); d.setKeyboard('a', frame % 120 < 30);
      d.setKeyboard('aimLeft', frame % 70 < 25); d.setKeyboard('aimUp', frame % 50 < 20);
      d.setKeyboard('fire', frame % 24 < 2);
      d.step(1);
      if (B.state.mode !== 'playing') break;
    }
    for (const key of ['w', 'a', 'aimLeft', 'aimUp', 'fire']) d.setKeyboard(key, false);
    d.replayStop();
    const saved = d.replayState();
    check(saved.count > 100, 'a recording');
    d.setAimGuide(replayLevel);
    check(d.replayStart(), 'replay starts');
    for (let at = 0; at < saved.count + 4 && B.state.mode !== 'replay-done'; at++) d.step(1);
    check(d.replayState().verified, 'replay verifies at level ' + replayLevel);
  }
  d.setAimGuide(0);

  label = 'onslaught';
  // Onslaught bests store the priced score.
  menu.toMain(); d.selectMode(3); d.setAimGuide(1); d.start(); d.freezeBots(true);
  // Forging: its last phases wait for a rendered HUD, so step (not
  // stepSimulation) until the forge is idle, not merely until 'playing'.
  for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
  d.stepSimulation(2); d.setAimGuide(0);
  check(B.state.mode === 'playing' && d.aimScore().level === 1, 'Onslaught used Sight');
  B.state.bestScore = 0; B.state.bestWave = 0; B.state.score = 2000;
  B.setMode('game-over');
  check(B.state.bestScore === 1800, 'Onslaught best is priced ' + B.state.bestScore);
  menu.toMain(); d.selectMode(0);
  menu.toMain();
  C.loadSave(savedCampaign.length ? savedCampaign : ''); C.writeSave();
  d.selectControls(0); d.selectAimAssist(1); d.savePreferences();
  globalThis.pocSummary = 'TREADLINE-AIM-GUIDE-PASS';
})();
