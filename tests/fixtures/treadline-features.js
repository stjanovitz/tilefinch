/* Synthetic game tests: no captured page data or network access. */
(() => {
  const d = __treadlineDebug;
  document.getElementById('online-cancel').click();
  function check(value, label) { if (!value) throw new Error(label + ': ' + JSON.stringify(d.snapshot())); }
  function start(mode) { d.selectMode(mode); d.selectClass(1); d.start(); d.freezeBots(true); }

  // The boss is larger than the ordinary class collision envelope. A
  // broad-phase mask must include every rectangle its circle can overlap.
  const spatial = __treadlineArenaData.spatial;
  for (let arena = 0; arena < 3; arena++) {
    const bounds = spatial.obstacleBounds[arena];
    for (let rectangle = 0; rectangle < spatial.obstacleCounts[arena]; rectangle++) {
      const at = rectangle * 4;
      for (let edge = 0; edge < 4; edge++) for (let probe = 0; probe < 16; probe++) {
        const distance = .6 + probe * .01;
        const x = edge < 2 ? bounds[at + edge] + (edge ? distance : -distance)
          : (bounds[at] + bounds[at + 1]) * .5;
        const z = edge >= 2 ? bounds[at + edge] + (edge === 3 ? distance : -distance)
          : (bounds[at + 2] + bounds[at + 3]) * .5;
        const cx = Math.floor((x + 8) * 2), cz = Math.floor((z + 8) * 2);
        if (cx < 0 || cx >= 32 || cz < 0 || cz >= 32) continue;
        check(spatial.circleObstacleGrids[arena][cz * 32 + cx] & (1 << rectangle),
          'boss collision broad-phase includes expanded radius');
      }
    }
  }

  // Differential oracle for the LOS broad phase: include parallel, tangent,
  // inside, reversed and padded segments, not only the obvious wall shot.
  function oldSlab(ax, az, bx, bz, left, right, top, bottom, padding) {
    left -= padding; right += padding; top -= padding; bottom += padding;
    const dx = bx - ax, dz = bz - az;
    let enter = 0, leave = 1;
    if (Math.abs(dx) < .00001) {
      if (ax < left || ax > right) return false;
    } else {
      let a = (left - ax) / dx, b = (right - ax) / dx;
      if (a > b) { const swap = a; a = b; b = swap; }
      enter = Math.max(enter, a); leave = Math.min(leave, b);
    }
    if (Math.abs(dz) < .00001) {
      if (az < top || az > bottom) return false;
    } else {
      let a = (top - az) / dz, b = (bottom - az) / dz;
      if (a > b) { const swap = a; a = b; b = swap; }
      enter = Math.max(enter, a); leave = Math.min(leave, b);
    }
    return enter <= leave && leave >= 0 && enter <= 1;
  }
  for (let at = 0; at < 2048; at++) {
    const ax = (at % 17 - 8) * .5, az = ((at / 17 | 0) % 17 - 8) * .5;
    const bx = at & 1 ? ax : ((at * 7) % 17 - 8) * .5;
    const bz = at & 2 ? az : ((at * 11) % 17 - 8) * .5;
    const padding = at & 4 ? .08 : 0;
    check(d.segmentBlocked(ax, az, bx, bz, -1, 1, -.5, .5, padding)
      === oldSlab(ax, az, bx, bz, -1, 1, -.5, .5, padding), 'LOS slab parity');
  }
  let lineSeed = 0x12345678;
  function lineRandom() {
    lineSeed ^= lineSeed << 13; lineSeed ^= lineSeed >>> 17; lineSeed ^= lineSeed << 5;
    return (lineSeed >>> 0) / 4294967296;
  }
  for (let at = 0; at < 8192; at++) {
    const ax = lineRandom() * 16 - 8, az = lineRandom() * 16 - 8;
    const bx = at & 1 ? ax + .000001 : lineRandom() * 16 - 8;
    const bz = at & 2 ? az + .000001 : lineRandom() * 16 - 8;
    const left = lineRandom() * 10 - 6, top = lineRandom() * 10 - 6;
    const right = left + lineRandom() * 3, bottom = top + lineRandom() * 3;
    const padding = lineRandom() * .8;
    check(d.segmentBlocked(ax, az, bx, bz, left, right, top, bottom, padding)
      === oldSlab(ax, az, bx, bz, left, right, top, bottom, padding), 'division-free LOS parity');
  }
  check(!d.segmentBlocked(0, -.5000005, .000001, -.4999995,
    -1, 1, -.5, .5, 0), 'near-axis LOS convention');
  for (let at = -4096; at <= 4096; at++) {
    const angle = at * Math.PI / 128;
    const expected = (Math.round(angle * 512 / (Math.PI * 2)) % 512 + 512) % 512;
    check(d.orientationIndex(angle) === expected, 'heading table wrap parity');
  }

  check(d.dailySeed(20261001) === d.dailySeed(20261001), 'daily deterministic');
  check(d.dailySeed(20261001) !== d.dailySeed(20261002), 'daily variation');
  start(4);
  // The calendar seed can require several sliced validation attempts.
  // Bound the hands-free wait, rather than requiring today's seed to fit
  // the old single-template generation window.
  for (let wait = 0; wait < 1200 && d.snapshot().mode === 'generating'; wait++) d.step(1);
  const daily = d.snapshot(), seed = d.dailySeed(daily.dailyDay);
  check(daily.mode === 'playing' && daily.classId === seed % 3,
    'daily fixed class and hands-free generation');
  check(daily.gadget === ['MINES','SMOKE','SHIELD','REPAIR DRONE','BOOST TREADS'][(seed >>> 8) % 5], 'daily fixed gadget');

  start(5);
  d.damageTank(1, 10000, 0, 0, 0);
  check(d.tankState(1).active && d.tankState(1).health === 1, 'billiards rejects direct kill');
  const score = d.snapshot().score;
  d.damageTank(1, 10000, 0, 0, 2);
  check(!d.tankState(1).active && d.snapshot().doubleBanks === 1
    && d.snapshot().score >= score + 1000, 'double bank bonus');

  start(3); d.step(120); d.setWave(5);
  const boss = d.snapshot();
  check(boss.boss && boss.bossLeftTread === 110 && boss.bossTurret === 180, 'boss wave');
  const b = d.tankState(5);
  d.damageTank(5, 110, b.x + 3, b.z);
  check(d.snapshot().bossRightTread === 0 || d.snapshot().bossLeftTread === 0, 'boss tread damage');
  d.damageTank(5, 300, b.x, b.z - 3);
  check(d.snapshot().bossTurret === 0, 'boss turret damage');
  check(d.snapshot().boxInstances <= 64 && d.snapshot().vertices <= 4096, 'boss render ceiling');

  // A permanently busy simulation must still eventually publish its HUD.
  // Exercise the real present-frame deferral path, not the unlimited debug
  // render loop used by the other simulation tests.
  const hudUploadsBefore = d.snapshot().hudTextUploads;
  d.setWave(6);
  d.stepBudgetedHud(512);
  check(d.snapshot().hudTextUploads > hudUploadsBefore
    && d.snapshot().hudTextPrimitives > 0,
    'HUD progresses through sustained frame-budget deferral');

  start(0);
  const crates = d.crateState(), crateAt = crates.findIndex(c => c.active);
  check(crateAt >= 0, 'crates placed');
  const hazardCount = d.snapshot().hazards;
  d.explodeCrate(crateAt);
  check(!d.crateState()[crateAt].active && d.snapshot().hazards > hazardCount, 'crate spawns bounded hazard');
  d.clearCrates();
  d.setPalette(2);
  check(d.getSceneryTint()[0] < .6 && d.snapshot().palette === 2, 'night palette');
  d.setPalette(0); d.setReverse(0);
  check(d.snapshot().reverseThreshold === 90, 'reverse setting');
  d.unlockMedal(1);
  check((d.snapshot().medals & 2) !== 0, 'medal unlock');

  start(0); d.clearCrates(); d.selectDifficulty(0);
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  const wall = __treadlineArenaData.arenas[0].obstacles[0];
  const left = wall[0] - wall[2] * .5 - 1, right = wall[0] + wall[2] * .5 + 1;
  d.setTankPosition(1, left, wall[1]); d.setTankPosition(0, right, wall[1]);
  d.stepSimulation(16);
  const occluded = d.botIntent(1);
  check(!d.wallSight(left, wall[1], right, wall[1]) && !occluded.fire
    && !occluded.secondary && !occluded.bank, 'cadet refuses wall shot');
  check(occluded.navigationGoal >= 0, 'blocked bot gets bounded route');
  d.selectDifficulty(2);
  d.setTankPosition(1, -6, -3); d.setTankPosition(0, -6, -5);
  d.setTankVelocity(0, 3, 0);
  const leading = d.botIntent(1);
  check(leading.target === 0 && leading.aimX > .2, 'ace leads moving target');

  start(0); d.clearCrates(); d.selectDifficulty(2);
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  d.setTankPosition(1, left, wall[1]); d.setTankPosition(0, right, wall[1]);
  const planningBefore = d.aiProfile();
  d.botIntent(1); d.botIntent(1); d.botIntent(1);
  const planningAfter = d.aiProfile();
  check(planningAfter.bankPlans === planningBefore.bankPlans + 1
    && planningAfter.bankHits >= planningBefore.bankHits + 2,
    'bank strategy retained between reaction ticks');
  d.clearCrates(); // Occupancy epoch changes even before its sliced rebuild.
  d.botIntent(1);
  check(d.aiProfile().bankPlans === planningAfter.bankPlans + 1,
    'bank strategy invalidated immediately by scenery change');

  start(1);
  const targetBefore = d.aiProfile().targetSelections;
  const firstTarget = d.botIntent(0).target;
  d.botIntent(0); d.botIntent(0);
  check(d.aiProfile().targetSelections === targetBefore + 1,
    'multi-enemy target selection retained between reaction ticks');
  d.setTankActive(firstTarget, false);
  check(d.botIntent(0).target !== firstTarget
    && d.aiProfile().targetSelections === targetBefore + 2,
    'dead target replaced without waiting for reaction tick');

  start(0); d.clearCrates();
  for (let id = 2; id < 6; id++) d.setTankActive(id, false);
  d.setTankPosition(1, -5.45, -1);
  d.setTankPosition(0, -5.45, 1);
  d.botIntent(1); // Enter the backoff/standoff hysteresis.
  d.setTankPosition(0, -5.45, 2);
  d.setTankPosition(1, -5.45, -1);
  d.botIntent(1);
  check(d.tankState(1).standoff && d.tankState(1).orbitTurn === -1,
    'orbit chooses clear side instead of grinding into wall');

  start(6); d.clearCrates();
  d.setKeyboard('fire', true); d.step(26);
  check(d.snapshot().shots === 0, 'charge waits for release');
  d.setKeyboard('fire', false); d.step(1);
  check(d.snapshot().shots === 1 && d.snapshot().maxBulletDamage === 38,
    'charged shell');
  d.step(120);
  check(d.snapshot().mode === 'duel-pass' && d.snapshot().duelTurn === 1, 'turn passes after shot');
  d.resumeDuel();
  check(d.snapshot().mode === 'playing', 'next player ready');

  // No debug mutations during the recorded run: replay restores its initial
  // simulation state, then consumes actual post-input command vectors.
  start(0); d.freezeBots(false);
  d.setKeyboard('w', true); d.step(24); d.setKeyboard('w', false);
  d.setKeyboard('fire', true); d.step(3); d.setKeyboard('fire', false); d.step(12);
  d.replayStop();
  const expectedReplayState = d.replayDigestState();
  const saved = d.replayState(), code = d.exportReplay();
  check(saved.count > 0 && code.startsWith('TR1.'), 'replay exported');
  check(!d.importReplay(code.slice(0, -4) + 'AAAA') && d.replayState().count === saved.count,
    'corrupt replay transactional refusal');
  check(d.importReplay(code) && d.replayStart(), 'replay imported');
  d.step(saved.count);
  if (!d.replayState().verified) throw new Error('replay expected=' + JSON.stringify(expectedReplayState)
    + ' actual=' + JSON.stringify(d.replayDigestState()));
  check(d.snapshot().mode === 'replay-done', 'replay simulation identical');

  // A replay borrows its run's loadout and Command/Camera preferences. The
  // viewer's own come back when it ends or is quit, and a save while it
  // plays writes the viewer's, never the borrowed ones.
  {
    const B = d.campaign.bridge(), st = B.state, pr = B.preferences;
    const viewer = () => [st.gameMode, st.classChoice, st.gadgetChoice,
      st.difficultyChoice, pr.command, pr.camera].join();
    const before = [st.gameMode, st.classChoice, st.gadgetChoice,
      st.difficultyChoice, pr.command, pr.camera];
    d.selectMode(5); d.selectClass(2); d.selectGadget(3); d.selectDifficulty(0);
    d.setCommandEnabled(true); d.selectCamera(1);
    const mine = viewer();
    check(d.importReplay(code) && d.replayStart() && viewer() !== mine,
      'replay borrows its loadout');
    d.savePreferences();
    const stored = JSON.parse(localStorage.getItem('treadline-settings-v1'));
    check(stored.mode === 5 && stored.classId === 2 && stored.gadget === 3
      && stored.difficulty === 0 && stored.command === true && stored.camera === 1,
      'a save during a replay keeps the viewer settings');
    d.step(saved.count);
    check(d.snapshot().mode === 'replay-done' && d.replayState().verified
      && viewer() === mine, 'replay end restores the viewer settings');
    check(d.replayStart() && viewer() !== mine, 'replay borrows again');
    d.step(5); B.setMode('title');
    check(!d.replayState().active && viewer() === mine,
      'quitting a replay restores the viewer settings');
    d.selectMode(before[0]); d.selectClass(before[1]); d.selectGadget(before[2]);
    d.selectDifficulty(before[3]); d.setCommandEnabled(before[4]);
    d.selectCamera(before[5]); d.savePreferences();
  }

  // The arena-clear transition keeps the HUD and its ARENA CLEAR toast up
  // from the last kill until the next arena's own toast replaces it.
  {
    start(0);
    const arena = d.snapshot().arena;
    // One at a time, as in play: the gate opens (with its own toast) first.
    for (let at = 1; at < 6; at++) { d.damageTank(at, 1000, 0, 0); d.step(30); }
    let steps = 0;
    while (d.snapshot().mode === 'playing' && steps++ < 60) d.step(1);
    check(d.snapshot().mode === 'arena-clear', 'arena clears');
    let clearFrames = 0;
    while (d.snapshot().mode === 'arena-clear' && steps++ < 200) {
      const s = d.snapshot();
      check(s.hudVisible && s.hudToast === 'ARENA CLEAR',
        'arena-clear frame ' + clearFrames + ' shows the HUD and its toast');
      clearFrames++;
      d.step(1);
    }
    const next = d.snapshot();
    check(clearFrames > 5 && next.mode === 'playing' && next.arena === arena + 1
      && next.hudToast === 'ARENA ' + (arena + 2), 'next arena takes over the toast ' + clearFrames);
    // The last kills landing in one frame (an explosion) also open the gate:
    // ARENA CLEAR still wins the toast.
    start(0);
    for (let at = 1; at < 6; at++) d.damageTank(at, 1000, 0, 0);
    for (let step = 0; step < 60 && d.snapshot().mode === 'playing'; step++) d.step(1);
    check(d.snapshot().mode === 'arena-clear' && d.snapshot().hudToast === 'ARENA CLEAR',
      'a same-frame gate opening does not replace ARENA CLEAR');
  }

  // A breach shot aims at scenery: neither weapon counts it as a shot at
  // the player. A full shell pool refuses the secondary without blaming
  // the cannon.
  {
    start(0);
    const B = d.campaign.bridge(), bot = B.tanks[1];
    bot.target = B.tanks[0].id; bot.fireTelegraphed = true;
    const shots = () => d.snapshot().botPlayerShots;
    const before = shots();
    bot.breachShot = true;
    check(d.fireSecondary(1) && shots() === before, 'breach secondary is not a shot at the player');
    bot.breachShot = false;
    check(d.fireSecondary(1) && shots() === before + 1, 'an aimed secondary is');
    for (const bullet of d.bullets) bullet.active = true;
    check(!d.fireSecondary(0) && d.snapshot().hudToast !== 'CANNON BUSY',
      'a full shell pool is not a busy cannon');
    for (const bullet of d.bullets) bullet.active = false;
    start(0);
  }

  // A turret that keeps turning one way stays in [-pi, pi] (it used to
  // accumulate turns, and every wrapAngle of it looped once per turn).
  {
    start(0);
    const bot = d.campaign.bridge().tanks[1];
    let worst = 0;
    for (let step = 0; step < 240; step++) {
      const angle = step * .2;
      bot.command.aimX = Math.sin(angle); bot.command.aimZ = Math.cos(angle);
      d.stepSimulation(1);
      worst = Math.max(worst, Math.abs(bot.turret));
    }
    check(worst <= Math.PI + 1e-9, 'a spinning turret stays wrapped ' + worst);
  }

  // Turn handoffs are input-log pauses, not invented player actions.
  start(6); d.stepSimulation(740); d.resumeDuel(); d.stepSimulation(8); d.replayStop();
  const duelFrames = d.replayState().count;
  check(d.replayStart(), 'duel replay starts'); d.stepSimulation(duelFrames);
  check(d.replayState().verified && d.snapshot().duelTurn === 1, 'duel replay handoff');

  start(0); d.freezeBots(true); d.enableKillcam(true); d.step(110);
  check(d.snapshot().killcamFrames > 20 && d.snapshot().killcamFrames <= 48, 'killcam bounded history');
  d.damagePlayerFrom('REAR', 1000, true);
  check(d.snapshot().mode === 'killcam', 'death enters killcam');
  d.step(181);
  check(d.snapshot().mode === 'playing' && d.snapshot().lives === 2, 'killcam returns to play');
  d.enableKillcam(false);

  d.beginLeague(12345, 1, 2, 1, 0); d.stepLeague(120);
  const leagueA = JSON.stringify(d.leagueResult()); d.finishLeague();
  d.beginLeague(12345, 1, 2, 1, 0); d.stepLeague(120);
  check(JSON.stringify(d.leagueResult()) === leagueA, 'league deterministic');
  const league = d.leagueResult();
  check(league.frames === 120 && league.tanks[1].shots >= 0
    && league.tanks.every(t => Number.isFinite(t.stuckSeconds)), 'league metrics');
  d.finishLeague();

  // A complete tap can occur between the guest's 20Hz input packets. Retain
  // its press through publication, then send the release on the next packet.
  document.getElementById('online-code').click();
  const keypad = document.getElementById('code-keypad');
  const one = keypad.querySelector('button');
  for (let at = 0; at < 12; at++) one.click();
  keypad.querySelector('[data-key="join"]').click();
  __tilefinchDeliverMultiplayer(71, 'open', undefined, '', 0, false, 99, '', 'Host');
  check(d.snapshot().onlineRole === 'guest', 'guest input seam');
  d.resetInputProbe(); d.stepSimulation(1);
  d.setKeyboard('fire', true); d.setKeyboard('secondary', true); d.stepSimulation(1);
  d.setKeyboard('fire', false); d.setKeyboard('secondary', false); d.stepSimulation(1);
  check((new Uint8Array(__treadlineOnlineTest.sentLast)[4] & 24) === 24,
    'short online tap reaches host');
  __tilefinchDeliverMultiplayer(71, 'drain', undefined, '', 0, false, 99, '', '');
  d.stepSimulation(3);
  check((new Uint8Array(__treadlineOnlineTest.sentLast)[4] & 24) === 0,
    'online tap releases after publication');
  document.getElementById('online-cancel').click();
  // Once protected tanks/shells have been emitted, their old reservation
  // must not leave holes while bounded optional effects still need slots.
  start(2); d.saturateVisualLoad();
  const saturated = d.snapshot();
  check(saturated.bullets === 18 && saturated.renderedBulletInstances === 18,
    'optional effects never displace protected shells');
  check(saturated.frameInstanceCeiling > 0
    && saturated.frameInstanceCeiling <= saturated.instanceLimit
    && saturated.boxInstances === saturated.frameInstanceCeiling,
    'late effects use the released protected-instance reservation');
  check(saturated.staticIndices + saturated.boxInstances * 36
    + saturated.hudPrimitives * 6 <= 4096, 'filled frame retains aggregate geometry cap');

  // Adaptive music (music.js): the Audio settings cycle and persist through
  // the campaign save; the bot league is silent; music never changes a
  // replay (recorded with music Full, verified with music Off).
  const C = d.campaign, B = C.bridge(), music = B.sounds.music;
  check(music && typeof music.tick === 'function', 'music conductor attached');
  // Each mission's program and stealth come from the MISSIONS table.
  {
    const runtime = C.runtime, was = [runtime.on, runtime.m];
    const programs = {'1-1': 'yard', '1-5': 'vesper', '2-3': 'foundry', '2-5': 'vesper',
      '3-4': 'glacier', '4-1': 'city', '4-2': 'city', '4-5': 'vesper', '5-1': 'sunset',
      '5-2': 'fortress', '5-3': 'ally', '5-4': 'sunset', '5-5': 'last'};
    for (const m of C.missions) {
      runtime.on = true; runtime.m = m;
      if (programs[m.id]) check(music.programFor() === programs[m.id],
        `${m.id} plays ${programs[m.id]}: ${music.programFor()}`);
      check(!!m.stealth === (m.id === '3-4' || m.id === '4-1'), `${m.id} stealth`);
    }
    [runtime.on, runtime.m] = was;
  }
  const value = (id) => document.getElementById(id).textContent;
  check(B.preferences.music === C.save.mu && B.preferences.musicVolume === C.save.mv,
    `attach hands the saved music settings to the game: ${B.preferences.music}/${C.save.mu}`);
  C.save.mu = 2; C.save.mv = 1; B.preferences.music = 2; B.preferences.musicVolume = 1;
  const musicChoice = document.getElementById('music-choice');
  const volumeChoice = document.getElementById('music-volume-choice');
  const seen = [];
  for (let at = 0; at < 3; at++) {
    musicChoice.click();
    seen.push(B.preferences.music + ':' + C.save.mu + ':' + value('music-value'));
  }
  for (let at = 0; at < 3; at++) {
    volumeChoice.click();
    seen.push(B.preferences.musicVolume + ':' + C.save.mv + ':' + value('music-volume-value'));
  }
  check(seen.join() === '0:0:Off,1:1:Menus,2:2:Full,2:2:High,0:0:Low,1:1:Mid',
    'music settings cycle: ' + seen.join());
  check(JSON.parse(localStorage.getItem('treadline-campaign-v1')).mu === 2,
    'music setting saved');
  // Saved Music and Volume values load clamped, and a save writes them back.
  {
    const kept = localStorage.getItem('treadline-campaign-v1');
    for (const [stored, music, volume] of [[undefined, 2, 1], [{v: 1}, 2, 1],
      [{v: 1, mu: 0}, 0, 1], [{v: 1, mu: 1, mv: 2}, 1, 2], [{v: 1, mu: 2, mv: 0}, 2, 0],
      [{v: 1, mu: 7, mv: 9}, 2, 1], [{v: 1, mu: '1', mv: '0'}, 2, 1]]) {
      C.loadSave(stored === undefined ? '' : JSON.stringify(stored));
      check(C.save.mu === music && C.save.mv === volume,
        `load ${JSON.stringify(stored)} -> ${C.save.mu}/${C.save.mv}`);
      C.writeSave();
      const saved = JSON.parse(localStorage.getItem('treadline-campaign-v1'));
      check(saved.mu === music && saved.mv === volume, 'music settings save round-trips');
    }
    C.loadSave(kept); localStorage.setItem('treadline-campaign-v1', kept);
  }
  // One writer and one vocabulary: game.js no longer labels or saves it.
  B.refreshSetupLabels();
  check(value('music-value') === 'Full', 'game labels leave Music alone: ' + value('music-value'));
  const gameSave = JSON.parse(localStorage.getItem('treadline-settings-v1'));
  check(!('music' in gameSave), 'the game save carries no second music field');
  for (const old of [true, false]) {
    localStorage.setItem('treadline-settings-v1', JSON.stringify({...gameSave, music: old}));
    d.reloadPreferences();
    check(B.preferences.music === 2, 'an old boolean music field loads and is ignored');
  }
  localStorage.setItem('treadline-settings-v1', JSON.stringify(gameSave));
  d.beginLeague(7, 1, 1, 0, 0); d.stepLeague(30);
  check(music.snapshot().kind === 'none' && !music.leadOwned && !music.bassOwned,
    'bot league plays no music');
  d.finishLeague();
  check(B.preferences.music === 2, 'league restores the music setting');
  start(0); d.freezeBots(false);
  for (let at = 0; at < 90; at++) d.step(1);
  d.replayStop();
  const musicReplay = d.replayState();
  check(musicReplay.count > 0 && music.snapshot().stats.evaluations > 0, 'recorded with music Full');
  B.preferences.music = 0;
  check(d.replayStart(), 'music replay starts'); d.step(musicReplay.count);
  check(d.replayState().verified, 'music Full -> Off replay identical');
  B.preferences.music = 2;

  // Objective cue: Auto hides the HUD arrow where the foes come to you and
  // shows it for the transmitter, the convoy and the other duel player. The
  // target holds through near-ties: two foes at about the same distance
  // used to swap every frame and flip the arrow across the compass.
  start(0);
  for (let id = 3; id < 6; id++) d.setTankActive(id, false);
  d.setTankPosition(0, 0, -5); d.setTankPosition(1, 3, -5); d.setTankPosition(2, -3.05, -5);
  d.step(1);
  let cue = d.objective();
  check(cue.key === 1 && cue.x === 3 && !cue.shown, 'survival cue on the nearest foe, arrow hidden');
  d.stepSimulation(4);
  check(!d.snapshot().objectiveIndicatorVisible, 'survival HUD arrow not drawn');
  d.setTankPosition(2, -2.9, -5); d.step(40);
  check(d.objective().x === 3, 'a foe a little nearer does not take the cue');
  d.setTankPosition(2, -2, -5); d.step(10);
  check(d.objective().x === 3, 'a clearly nearer foe waits out the hold');
  d.step(30);
  check(d.objective().x === -2, 'a clearly nearer foe takes the cue after the hold');
  d.destroyTank(2); d.step(1);
  check(d.objective().x === 3, 'the cue moves at once when its foe is gone');
  const objectiveChoice = document.getElementById('objective-choice');
  const arrowSeen = [];
  for (let at = 0; at < 3; at++) {
    objectiveChoice.click();
    arrowSeen.push(B.preferences.objectiveArrow + ':' + value('objective-value')
      + ':' + d.objective().shown);
  }
  check(arrowSeen.join() === '1:On:true,2:Off:false,0:Auto:false',
    'objective arrow setting cycles: ' + arrowSeen.join());
  for (const [mode, label] of [[1, 'team control'], [2, 'convoy'], [6, 'pass the PSP']]) {
    start(mode); d.stepSimulation(4);
    check(d.objective().shown && d.snapshot().objectiveIndicatorVisible, label + ' shows the arrow');
  }
  start(3);
  for (let at = 0; at < 600 && (d.snapshot().mode !== 'playing'
    || d.snapshot().arenaGenerationPhase !== 0); at++) d.step(1);
  d.stepSimulation(4);
  check(!d.objective().shown && !d.snapshot().objectiveIndicatorVisible, 'onslaught hides the arrow');

  // A run's first HUD and the forge panel are published as the run starts,
  // not in deferred slices behind the arena and the last run's HUD.
  let uploads = d.snapshot().hudTextUploads;
  start(0);
  check(d.snapshot().hudTextUploads === uploads + 1 && d.snapshot().hudTextPrimitives > 0,
    'first HUD published as the run starts');
  uploads = d.snapshot().hudTextUploads;
  B.setMode('title'); start(3);
  check(d.snapshot().mode === 'generating' && d.snapshot().hudTextUploads === uploads + 1
    && d.snapshot().hudTextPrimitives > 0, 'forge panel published as generation starts');
  // Menus and controls share one gamepad read per simulation step.
  {
    let reads = 0;
    const pads = [{connected: true, axes: [0, 0], buttons: Array.from({length: 17},
      () => ({pressed: false, value: 0}))}];
    navigator.getGamepads = () => { reads++; return pads; };
    try {
      // The title menu with its panel up, as a person sees it.
      start(0); B.setMode('title'); d.stepSimulation(2); C.menu.toMain();
      document.getElementById('panel').hidden = false; reads = 0;
      d.stepSimulation(30);
      check(reads === 30, `one gamepad read per menu step: ${reads} in 30`);
      reads = 0; start(0); d.stepSimulation(30);
      check(reads === 30, `one gamepad read per play step: ${reads} in 30`);
    } finally { delete navigator.getGamepads; B.setMode('title'); C.menu.toMain(); }
  }
  globalThis.pocSummary = 'TREADLINE-FEATURES-PASS';
})();
