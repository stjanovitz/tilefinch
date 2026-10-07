/* Synthetic campaign tests: mission table, save handling, radio, faults,
   replay determinism, menu focus/back and the ending. No network or
   captured data; storage is the realm's own bounded localStorage. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const M = C.runtime, save = C.save;
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(what + ': ' + JSON.stringify({
      mode: B.state.mode, screen: C.menu.screen, t: M.t, mission: M.idx,
      stage: M.stage, lives: B.state.lives, label}));
  }
  const HUD_SAFE = /^[ 0-9A-Z\-\/]{1,20}$/;
  const fresh = () => { C.loadSave(''); save.d = 1; C.setRadio(0); };
  function run(index, faults = 0, setup) {
    fresh(); save.f = faults; save.k = 0x1ffffff;
    if (setup) setup();
    check(C.startMission(index, false), 'start ' + index);
    return B.state;
  }

  // Mission table: every mission names an authored arena and an existing
  // mode, its HUD barks fit the retained glyph set, medal tests evaluate.
  label = 'table';
  check(C.missions.length === 25 && C.theaters.length === 5, 'mission count');
  C.missions.forEach((m, at) => {
    check(m.id === `${(at / 5 | 0) + 1}-${at % 5 + 1}`, 'mission id order ' + m.id);
    check([0, 1, 2].includes(m.mode) && [0, 1, 2].includes(m.arena), 'mode/arena ' + m.id);
    check(HUD_SAFE.test(m.bark), 'bark glyphs ' + m.bark);
    check(m.brief.length >= 1 && m.win.length >= 1 && m.obj, 'story lines ' + m.id);
    for (const line of m.brief.concat(m.win))
      check(/^[MVSY]\|[ -~]+$/.test(line), 'printable radio line ' + line);
    check(m.quiet ? m.med === null : m.med.length === 2 && C.medalTests[m.id], 'medals ' + m.id);
  });
  for (const lore of C.lore) check(/^[ -~]+$/.test(lore), 'printable lore');
  check(C.faults.length === 10 && C.faults.every(f => f === null || f[1] > 0), 'fault table');

  // Command on: the meter leaves the HUD toast too little room, so campaign
  // radio drains without showing (treadline-campaign-hud.js sizes the rest).
  label = 'command radio';
  d.setCommandEnabled(true); C.setRadio(0);
  run(0); d.stepSimulation(150);
  check(M.barkCount === 0, 'Command-on runs drain radio without showing it');
  d.setCommandEnabled(false);

  // Every mission deploys, places tanks clear of scenery and simulates.
  for (let at = 0; at < 25; at++) {
    label = 'deploy ' + C.missions[at].id;
    const st = run(at);
    check(st.mode === 'playing' && M.on && M.idx === at, 'deployed');
    check(st.gameMode === C.missions[at].mode && st.arena === C.missions[at].arena, 'mode and arena');
    for (let id = 0; id < 6; id++) {
      const t = B.tanks[id];
      if (!t.active) continue;
      check(Math.abs(t.x) < 7.65 && Math.abs(t.z) < 7.65, 'tank in bounds');
      check(t.boss || !d.circleBlocked(t.x, t.z, t.collisionRadius * .9), 'tank clear of scenery ' + id);
    }
    d.stepSimulation(90);
    check(['playing', 'arena-clear', 'victory'].includes(B.state.mode), 'mission simulates');
    const tests = C.medalTests[C.missions[at].id];
    if (tests) check(tests().length === 2, 'medal tests evaluate');
  }

  // Reprise runs Survival missions on fixed generated arenas with Ace bots.
  label = 'reprise';
  C.missions.forEach((m, at) => {
    if (m.mode !== 0) return;
    run(at, 0, () => { save.e = 1; save.rep = 1; });
    check(B.state.arena === 3 && M.c.rep && M.c.d === 2, 'reprise uses generated arena at Ace ' + m.id);
    check(d.arenaValidationMask(3, 0) === 63, 'reprise arena passes the generator validator ' + m.id);
    for (let id = 0; id < 6; id++) {
      const t = B.tanks[id];
      if (t.active) check(t.boss || !d.circleBlocked(t.x, t.z, t.collisionRadius * .9), 'reprise placement ' + m.id + ' tank ' + id + ' at ' + t.x.toFixed(2) + ',' + t.z.toFixed(2));
    }
    d.stepSimulation(20);
    check(['playing', 'arena-clear', 'victory'].includes(B.state.mode), 'reprise simulates ' + m.id);
  });

  // Codes: round trip, tag check and a malformed value.
  label = 'codes';
  const code = C.encode({i: 17, rep: false, d: 2, f: 0x2a1, vg: 3, vr: 2, vb: true, sg: 3, gc: 2, gg: 4});
  const decoded = C.decode(code);
  check(code >= 0x80000000 && decoded.i === 17 && decoded.d === 2 && decoded.f === 0x2a1
    && decoded.vg === 3 && decoded.vr === 2 && decoded.vb && decoded.sg === 3
    && decoded.gc === 2 && decoded.gg === 4, 'code round trip');
  check(C.decode(20261004) === null && C.decode(0x80000000 | 31) === null
    && C.decode(-1) === null && C.decode(1.5) === null, 'non-campaign codes refused');

  // Save: version, corruption and range tolerance; round trip.
  label = 'save';
  C.loadSave('{not json');
  check(save.corrupt && save.d === -1 && save.m.every(v => v === 0), 'corrupt save starts fresh');
  C.loadSave(JSON.stringify({v: 2, d: 2}));
  check(save.corrupt && save.d === -1, 'unknown save version refused');
  C.loadSave(JSON.stringify({v: 1, d: 9, r: -3, cs: 'toolong', m: [7, 'x', 99, 3],
    b: [-5, 1e12, 300], k: 'many', f: 0x1ff, ans: 5, best: {i: 3, c: 7}}));
  check(!save.corrupt && save.d === -1 && save.r === 0 && save.cs === 'YOU'
    && save.m[0] === 7 && save.m[1] === 0 && save.m[2] === 0 && save.m[3] === 3
    && save.b[0] === 0 && save.b[1] === 0 && save.b[2] === 300 && save.k === 0
    && save.f === 0xff && save.ans === -1 && save.best === null, 'out-of-range fields clamp');
  fresh(); save.d = 2; save.cs = 'ABC'; save.m[4] = 15; save.b[4] = 1234; save.k = 5;
  check(C.writeSave(), 'save writes');
  C.loadSave();
  check(save.d === 2 && save.cs === 'ABC' && save.m[4] === 15 && save.b[4] === 1234
    && save.k === 5 && C.medalCount() === 3 && C.cardCount() === 2, 'save round trip');

  // Radio: Full queues the opening bark, Brief and Off keep play silent.
  label = 'radio';
  for (const [radio, queued] of [[0, 1], [1, 0], [2, 0]]) {
    fresh(); C.setRadio(radio); C.startMission(0, false);
    check(M.barkCount === queued, 'radio ' + radio + ' opening bark');
    d.stepSimulation(150);
    check(M.barkCount === 0, 'barks drain through the HUD toast');
  }
  C.menu.toMain(); fresh(); C.menu.select(0); C.menu.show('briefing');
  check(document.getElementById('radio').textContent.includes('MARSHAL'), 'briefing radio line');
  C.menu.skipRadio();
  check(document.getElementById('radio').textContent.includes('Targets are up'), 'Start-hold skips to the end');
  C.menu.toMain();

  // Faults and their multipliers.
  label = 'faults';
  run(2, 1 | 8 | 64 | 128);
  check(Math.abs(M.mult - 1.5 * 1.2 * 1.2) < 1e-9, 'fault multiplier product');
  check(B.tanks[0].health === Math.ceil(B.tanks[0].maxHealth / 2), 'Old Wounds half armor');
  check(B.tanks[0].z > 0, 'Mirror deploys on the far side');
  check([1, 2, 3].every(id => B.tanks[id].gadget === B.tanks[0].gadget), 'Echo copies your gadget');
  const foe = B.tanks[1];
  d.damageTank(1, 1, foe.x, foe.z - 2);
  check(!foe.active, 'Glass Treads one-hit kill');
  run(2, 2);
  d.damageTank(1, 60, B.tanks[1].x, B.tanks[1].z - 2, 0);
  check(B.tanks[1].health === B.tanks[1].maxHealth, 'Ricochet Only refuses direct hits');
  d.damageTank(1, 20, B.tanks[1].x, B.tanks[1].z - 2, 1);
  check(B.tanks[1].health < B.tanks[1].maxHealth, 'Ricochet Only admits banked hits');
  run(2, 4 | 16);
  B.pickups[5].active = true; B.pickups[5].type = 'ARMOR'; B.pickups[5].x = 6; B.pickups[5].z = 6;
  d.damageTank(1, 5, B.tanks[1].x, B.tanks[1].z - 2);
  d.stepSimulation(1);
  check(!B.pickups[5].active, 'Iron Rain removes armor pickups');
  check(B.state.hitConfirm === 0 && B.state.damageIndicator === 0, 'Fog of Memory hides indicators');
  run(2, 512);
  d.freezeBots(true);
  d.stepSimulation(400);
  const before = {x: B.tanks[0].x, z: B.tanks[0].z};
  d.damageTank(0, 9999, B.tanks[0].x, B.tanks[0].z + 2);
  check(B.tanks[0].active && M.rewound, 'Second Chance absorbs lethal damage');
  d.stepSimulation(1);
  check(B.tanks[0].health > 0 && B.state.lives === 3, 'Second Chance rewinds instead of redeploying');
  d.freezeBots(false);
  check(Number.isFinite(before.x), 'rewind sample');
  run(0, 32);
  d.freezeBots(true);
  d.stepSimulation(30); M.t = 119.9; d.stepSimulation(30);
  check(B.state.mode === 'game-over' || B.state.mode === 'victory' || M.ended, 'Low Battery limit ends the run');
  d.freezeBots(false);
  fresh(); save.f = 1; save.k = 0;
  check(C.decode(C.runCode(0, save.f)).f === 0, 'locked faults never reach a run');

  // Mission rules: targets, bank-only armor, withdrawal, staged waves.
  label = 'rules';
  run(0);
  for (let id = 1; id < 5; id++) d.damageTank(id, 999, B.tanks[id].x, B.tanks[id].z - 2);
  d.stepSimulation(40);
  check(B.state.mode === 'victory' && M.ended && M.result.won, 'Roll Call clears on the last target');
  check((save.m[0] & 1) === 1, 'clear saved after the mission');
  check(C.menu.screen === 'results', 'results screen');
  run(1);
  d.damageTank(1, 999, B.tanks[1].x, B.tanks[1].z - 2, 0);
  check(B.tanks[1].active, 'bank-only target ignores direct hit');
  d.damageTank(1, 999, B.tanks[1].x, B.tanks[1].z - 2, 2);
  check(!B.tanks[1].active && M.doubleHits === 1, 'bank-only target takes a double bank');
  run(4);
  d.damageTank(1, 999, B.tanks[1].x, B.tanks[1].z - 2);
  check(B.tanks[1].active && B.tanks[1].health === 1, 'Vesper cannot be destroyed in her first duel');
  d.stepSimulation(80);
  check(B.state.mode === 'victory' && M.won, 'Vesper withdraws and the duel is won');
  run(3);
  for (let wave = 0; wave < 5; wave++) {
    check(B.tanks[0].gadget === B.GADGETS[wave], 'Gadget Day wave gadget ' + wave);
    for (let id = 1; id < 6; id++)
      if (B.tanks[id].active) d.damageTank(id, 999, B.tanks[id].x, B.tanks[id].z - 2);
    d.stepSimulation(80);
  }
  check(B.state.mode === 'victory' && M.stage === 4, 'Gadget Day runs five waves');
  run(18);
  check(B.tanks[0].team === 1 && B.tanks[0].gadget === 'MINES', 'Ambush Alley swaps sides');
  B.convoy.health = 1;
  d.stepSimulation(1);
  B.convoy.health = -1; d.stepSimulation(2);
  check(M.ended && M.won, 'Ambush Alley is won by stopping the crawler');
  run(16);
  d.stepSimulation(30);
  check(Math.abs(B.convoy.progress - .45) < .01, 'monument holds its place');

  // Replay determinism of a campaign mission with faults and live bots.
  label = 'replay';
  run(2, 64 | 128);
  d.setKeyboard('w', true); d.stepSimulation(30); d.setKeyboard('w', false);
  d.setKeyboard('fire', true); d.stepSimulation(4); d.setKeyboard('fire', false);
  d.stepSimulation(200);
  d.replayStop();
  const expected = d.replayDigestState(), saved = d.replayState(), replayCode = d.exportReplay();
  check(saved.count > 200 && replayCode.startsWith('TR1.'), 'campaign replay exported');
  C.menu.toMain();
  check(!M.armed && B.state.dailyDay === 0, 'leaving the campaign disarms its rules');
  check(d.importReplay(replayCode) && d.replayStart(), 'campaign replay imported');
  check(M.on && M.idx === 2 && M.replay, 'replay restores the campaign context from its code');
  d.stepSimulation(saved.count);
  if (!d.replayState().verified) throw new Error('campaign replay expected=' + JSON.stringify(expected)
    + ' actual=' + JSON.stringify(d.replayDigestState()));
  label = 'replay reprise';
  run(5, 0, () => { save.e = 1; save.rep = 1; });
  d.stepSimulation(240); d.replayStop();
  const repriseCount = d.replayState().count;
  check(d.replayStart(), 'reprise replay starts'); d.stepSimulation(repriseCount);
  check(d.replayState().verified, 'reprise replay identical on its generated arena');
  // A stage change rebuilds the arena inside the campaign tick, before the
  // frame's input is read; playback must apply the recorded input at that
  // same point or every later stage diverges. Held keys span two stage
  // changes; the debug kills are repeated at the same playback frames.
  label = 'replay stages';
  run(3);
  const kills = [];
  d.setKeyboard('w', true); d.setKeyboard('aimRight', true);
  for (let stage = 0; stage < 2; stage++) {
    d.stepSimulation(20);
    kills.push(d.replayState().count);
    for (let id = 1; id < 6; id++) if (B.tanks[id].active) d.destroyTank(id);
    d.stepSimulation(150);
  }
  d.setKeyboard('w', false); d.setKeyboard('aimRight', false);
  check(M.stage === 2, 'Gadget Day advanced two stages');
  d.replayStop();
  const stageExpected = d.replayDigestState(), stageCount = d.replayState().count;
  check(d.replayStart(), 'stage replay starts');
  let played = 0;
  for (const at of kills) {
    d.stepSimulation(at - played); played = at;
    for (let id = 1; id < 6; id++) if (B.tanks[id].active) d.destroyTank(id);
  }
  d.stepSimulation(stageCount - played);
  if (!d.replayState().verified) throw new Error('stage replay expected='
    + JSON.stringify(stageExpected) + ' actual=' + JSON.stringify(d.replayDigestState()));

  // Menu focus and O-back on every screen (DOM level).
  label = 'menus';
  C.menu.toMain(); fresh();
  const screens = ['difficulty', 'campaign', 'briefing', 'faults', 'logs', 'quick',
    'multiplayer', 'replays', 'garage', 'settings'];
  for (const name of screens) {
    C.menu.show('main', false);
    C.menu.select(0);
    C.menu.show(name);
    const items = C.menu.focusables();
    check(C.menu.screen === name && items.length > 0, 'screen has focus targets ' + name);
    check(document.activeElement === items[0] || document.activeElement === document.getElementById('play'),
      'screen focuses its first target ' + name);
    check(items.length <= 16, 'screen stays compact ' + name);
    C.menu.back();
    check(C.menu.screen === 'main', 'O backs out of ' + name);
  }
  C.menu.show('settings');
  const tabsBefore = document.getElementById('panel').getAttribute('data-tab');
  C.menu.switchTab(1);
  check(tabsBefore === '0' && document.getElementById('panel').getAttribute('data-tab') === '1',
    'L/R switch settings tabs');
  C.menu.toMain();
  run(2);
  d.setPaused(true); d.stepSimulation(1);
  check(C.menu.screen === 'pause', 'pause screen');
  const pauseLabels = C.menu.focusables().map(el => el.textContent);
  check(pauseLabels.includes('Restart') && pauseLabels.includes('Quit')
    && pauseLabels.includes('Resume'), 'pause items');
  C.menu.back(); d.stepSimulation(1);
  check(B.state.mode === 'playing', 'O resumes from pause');
  d.setPaused(true); d.stepSimulation(1);
  C.menu.back(); d.stepSimulation(1);
  for (let death = 0; death < 3 && B.state.mode === 'playing'; death++) {
    d.damageTank(0, 9999, B.tanks[0].x, B.tanks[0].z + 2); d.stepSimulation(4);
  }
  d.stepSimulation(4);
  check(B.state.mode === 'game-over' && C.menu.screen === 'results', 'failed mission results');
  const resultLabels = C.menu.focusables().map(el => el.textContent);
  check(resultLabels[0] === 'Retry' && resultLabels.includes('Watch replay')
    && resultLabels.includes('Menu'), 'results items');
  C.menu.back();
  check(C.menu.screen === 'main' && B.state.mode === 'title', 'O leaves results to the main menu');

  // Ending: the quiet match ends on request and records the callsign.
  label = 'ending';
  run(24, 0, () => { for (let at = 0; at < 24; at++) save.m[at] = 7; save.cs = 'TLF'; });
  check(B.state.mode === 'playing' && M.m.quiet, 'last match deploys');
  d.damageTank(0, 9999, B.tanks[0].x, B.tanks[0].z + 2);
  d.damageTank(1, 9999, B.tanks[1].x, B.tanks[1].z + 2);
  d.stepSimulation(30);
  check(B.tanks[0].active && B.tanks[1].active && B.state.score === 0, 'no deaths, no score');
  d.setPaused(true); d.stepSimulation(1);
  const leave = C.menu.focusables().find(el => el.textContent === 'Leave the range');
  check(leave, 'leave option in pause');
  leave.click(); d.stepSimulation(1);
  check(B.state.mode === 'victory' && save.e === 1, 'ending recorded');
  check(document.querySelector('#panel h1').textContent === 'THE LINE'
    && document.getElementById('message').textContent.includes('TLF'), 'callsign on the scoreboard');
  check(C.menu.focusables().some(el => el.textContent === 'Daily run'), 'daily run offered');
  C.menu.toMain();
  C.loadSave(''); C.writeSave();
  {
    // Spawns never start inside scenery or another tank: placeTank moves a
    // covered spawn point to the nearest clear spot.
    d.selectMode(0); d.selectClass(1); d.start(); d.freezeBots(true);
    const wall = B.barriers.find(b => b.active);
    check(wall, 'arena has a barrier to spawn into');
    const mover = B.tanks[1];
    B.placeTank(mover, wall.x, wall.z, 0, 1, mover.gadget, 'HUNTER', 1);
    check(!B.circleHitsObstacle(mover.x, mover.z, mover.collisionRadius, true),
      'spawn on a barrier moved clear of it');
    check(Math.hypot(mover.x - wall.x, mover.z - wall.z) < 6.5,
      'moved spawn stays near the requested point');
    const other = B.tanks[2];
    B.placeTank(other, mover.x, mover.z, 0, 1, other.gadget, 'HUNTER', 1);
    check(Math.hypot(other.x - mover.x, other.z - mover.z)
      >= other.collisionRadius + mover.collisionRadius,
      'spawn on another tank moved apart');
    const open = B.tanks[3], before = [open.x, open.z];
    B.placeTank(open, before[0], before[1], 0, 1, open.gadget, 'HUNTER', 1);
    check(open.x === before[0] && open.z === before[1], 'clear spawn is unchanged');
  }
  d.freezeBots(false);
  globalThis.pocSummary = 'TREADLINE-CAMPAIGN-PASS';
})();
