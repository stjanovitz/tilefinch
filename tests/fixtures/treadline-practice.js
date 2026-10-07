/* Practice Range: entry/exit and menus, no damage or hostile fire, target
   behaviours and resets, bank-only targets, live switches, the shared aim
   guide (Off by default; against real shells at Full, its hit ring and
   its quick toggle), drills and best times, the save section's
   versioning and corruption handling. Synthetic inputs only (debug seams,
   keyboard state); storage is the realm's own. The guide's own exhaustive
   tests are tests/fixtures/treadline-aim-guide.js. */
(() => {
  const d = __treadlineDebug, P = d.practice, C = d.campaign;
  const B = C.bridge(), R = P.runtime, menu = C.menu, K = d.controls;
  const $ = (id) => document.getElementById(id);
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(what + ': ' + JSON.stringify({label, mode: B.state.mode,
      screen: menu.screen, t: R.t, drill: R.drill, aim: R.aim}));
  }
  const near = (a, b, eps) => Math.abs(a - b) <= eps;
  const labels = () => [...$('menu').children].filter(b => !b.hidden).map(b => b.textContent);
  const click = (text) => {
    const b = [...$('menu').children].find(e => !e.hidden && e.textContent.startsWith(text));
    check(b && !b.disabled, 'button ' + text + ' in ' + labels()); b.click();
  };
  const savedCampaign = JSON.stringify(C.save);
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  d.setCommandEnabled(false); d.selectControls(0); d.selectAimAssist(0);
  const toasts = [];
  const realToast = B.showToast;

  // ---------------------------------------------------------------- menus --
  label = 'menus';
  menu.toMain();
  check(labels().includes('Practice Range') && !labels().includes('Daily'),
    'main menu swaps Daily for Practice Range ' + labels());
  check(labels().length === 8, 'main menu keeps eight items');
  menu.show('quick');
  check(labels().join() === 'Practice Range,Back', 'Quick Match links the range ' + labels());
  click('Practice Range');
  check(menu.screen === 'practice' && labels()[0] === 'Free range'
    && labels().slice(1, 4).every((t, at) => t.startsWith(P.drills[at + 1].n)), 'drill list ' + labels());
  check($('legend').textContent === 'X Select | O Back', 'practice legend');
  const listButtons = [...$('menu').children].filter(b => !b.hidden);
  listButtons[1].focus(); menu.poll(0); menu.poll(0.2);
  check($('message').textContent.startsWith(P.drills[1].note)
    && /pivot/.test($('message').textContent) && /lunge/.test($('message').textContent),
    'focused drill explains itself with its controls: ' + $('message').textContent);
  d.selectControls(2); menu.render();
  listButtons[2].focus(); menu.poll(0); menu.poll(0.2);
  check(/Square \/ Circle: turn turret/.test($('message').textContent), 'notes follow the scheme ' + $('message').textContent);
  d.selectControls(0);
  menu.back();
  check(menu.screen === 'quick', 'O backs out of the drill list');
  menu.toMain(); click('Practice Range'); click('Free range');
  check(P.on && B.state.mode === 'playing' && B.state.arena === 3 && B.state.gameMode === 0,
    'Free range deploys');
  d.step(2);

  // --------------------------------------------------------------- layout --
  label = 'layout';
  const arena = __treadlineArenaData.generatedArena;
  check(arena.obstacleCount === 8 && arena.rampCount === 2 && arena.gateCount === 0
    && arena.barrierCount === P.layout.POLES.length, 'range keeps the generated slot shape');
  const statics = d.arenaStaticIndexCounts();
  check(statics[3] === statics[3] && statics.every(n => n > 0), 'static tails');
  for (const [x, z] of P.layout.BANK_SPOTS) check(!d.circleBlocked(x, z, .6), 'bank spot clear ' + x + ',' + z);
  for (const [x, z] of P.layout.COURSE) check(!d.circleBlocked(x, z, .5), 'gate clear ' + x + ',' + z);
  for (let lane = 0; lane < 3; lane++) for (const x of [-1.25, 0, 1.25])
    check(!d.circleBlocked(x, P.layout.LANE_Z[lane], .6), 'lane swing clear ' + x);
  for (let id = 0; id < 6; id++) {
    const t = d.tankState(id);
    check(t.active && !d.circleBlocked(t.x, t.z, .55), 'tank placed clear ' + id);
  }
  check(d.snapshot().crates === 0 && d.snapshot().hazards === P.layout.ICE.length, 'no crates; ice patches');

  // -------------------------------------------------------- no hostility --
  label = 'safe';
  const replay = d.replayState();
  check(!replay.recording && replay.count === 0, 'the range never records a replay');
  d.damagePlayerFrom('REAR', 80);
  check(d.snapshot().armor === d.snapshot().maxArmor, 'the player takes no damage');
  const before = d.snapshot();
  d.stepSimulation(1800); // 30 s: no timer, no game over, nothing fires
  const after = d.snapshot();
  check(after.mode === 'playing' && after.lives === 3 && after.botPlayerShots === 0
    && after.bullets === 0 && after.damageTaken === 0, 'calm range ' + JSON.stringify([after.mode, after.lives, after.bullets]));
  check(!d.replayState().recording && d.replayState().count === 0, 'still not recording');
  for (let id = 1; id < 6; id++) {
    const t = B.tanks[id];
    check(t.inert && t.command.fire === false && t.target === -1, 'targets are inert ' + id);
  }
  check(near(d.tankState(1).x, before.remotePlayerX, 1e-9), 'still targets hold position');

  // ------------------------------------------------------------- shooting --
  label = 'shoot';
  B.showToast = (text, seconds) => { toasts.push(String(text)); return realToast(text, seconds); };
  function aim(x, z, angle) {
    for (let at = 0; at < 300 && d.snapshot().bullets; at++) d.stepSimulation(1);
    d.setTankPosition(0, x, z); d.setTankHeading(0, 0);
    const p = B.tanks[0];
    p.turret = angle; B.updateTankTurretCache(p); p.slideX = p.slideZ = 0;
    p.command.aimX = Math.sin(angle); p.command.aimZ = Math.cos(angle);
    p.cooldown = 0;
  }
  /* Fire one real shell; return its bounce vertices (position before the
     bouncing step, as the guide records them) and last position. */
  function fire() {
    const shots = d.snapshot().shots;
    d.setKeyboard('fire', true); d.stepSimulation(1); d.setKeyboard('fire', false);
    const p = B.tanks[0], track = [];
    let count = 0, end = null, prev = [p.x + p.turretSine * .78, p.z + p.turretCosine * .78];
    for (let at = 0; at < 240; at++) {
      d.stepSimulation(1);
      const b = d.replayDigestState().bullets.find(v => v);
      if (!b) break;
      if (b[5] > count) { track.push(prev[0], prev[1]); count = b[5]; }
      prev = [b[0], b[1]]; end = prev;
    }
    check(d.snapshot().shots === shots + 1, 'one shell fired');
    return {track, end};
  }
  /* Fire is held one step already: release it and return the shell's
     first event [kind, x, z, vx, vz] (position before the event step). */
  function release(fromX, fromZ) {
    let prev = [fromX, fromZ], first = null;
    for (let at = 0; at < 240 && !first; at++) {
      d.stepSimulation(1);
      const shell = d.bullets.find(b => b.active && b.owner === 0);
      if (!shell) first = ['stop', prev[0], prev[1]];
      else if (shell.bounceCount) first = ['bounce', prev[0], prev[1], shell.vx, shell.vz];
      else prev = [shell.x, shell.z];
    }
    for (const bullet of d.bullets) bullet.active = false;
    return {first};
  }
  // Direct hit on the near lane target: damage bark, no kill, pop and respawn.
  aim(.8, -5, 0);
  check(d.aimGuideLevel() === 0 && R.aim === 0, 'the range starts Off');
  P.cycleAim(-1);
  check(d.aimGuideLevel() === 2, 'one press back reaches Full');
  const toTarget = d.aimGuide();
  check(toTarget.enemy === 1 && toTarget.enemyLeg === 1, 'guide confirms the near target');
  const hp0 = B.tanks[1].health, hits0 = R.hits;
  fire();
  check(B.tanks[1].health < hp0 && R.hits === hits0 + 1 && toasts.some(t => /^HIT \d+$/.test(t)),
    'a direct hit damages and barks ' + toasts);
  const kills0 = d.snapshot().kills, score0 = B.state.score;
  const downs0 = R.downs;
  for (let shot = 0; shot < 4 && R.downs === downs0; shot++) {
    aim(.8, -5, 0); B.tanks[0].cooldown = 0;
    d.setKeyboard('fire', true); d.stepSimulation(1); d.setKeyboard('fire', false); d.stepSimulation(1);
    for (let at = 0; at < 120 && d.snapshot().bullets && R.downs === downs0; at++) d.stepSimulation(1);
  }
  check(!B.tanks[1].active && R.downs === downs0 + 1 && toasts.includes('DOWN'), 'target pops ' + toasts);
  const snap = d.snapshot();
  check(snap.pendingClear === false && snap.killBeat === 0 && snap.pickups === 0
    && B.state.kills === 0, 'a pop is not a kill: no clear, beat or pickup');
  d.stepSimulation(90);
  check(B.tanks[1].active && B.tanks[1].health === 60 && B.tanks[1].inert, 'target respawns full');
  check(B.state.score === R.hits, 'HUD score shows hits in the free range');

  // Bank-only target: a direct shell does nothing; a banked one counts.
  label = 'bank';
  const bankTarget = B.tanks[4];
  check(bankTarget.active && near(bankTarget.x, P.layout.BANK_SPOTS[0][0], 1e-9), 'bank target on its spot');
  aim(-3.2, 1.9, -Math.PI / 2); // a clear direct line across the yard
  check(d.aimGuide().enemy === 4 && d.aimGuide().enemyLeg === 1, 'direct line to the bank target');
  const bankHp = bankTarget.health;
  toasts.length = 0; fire();
  check(bankTarget.health === bankHp && toasts.includes('BANK IT'), 'direct hit on a bank target does nothing');
  // A one-bounce solution found by the guide itself.
  aim(-7.2, -3.2, 74 * Math.PI / 256);
  const bankGuide = d.aimGuide();
  check(bankGuide.enemy === 4 && bankGuide.enemyLeg === 2, 'one-bounce bank solution');
  toasts.length = 0; const bankHits0 = R.bankHits; fire(); d.stepSimulation(2);
  check(R.bankHits === bankHits0 + 1 && !bankTarget.active, 'banked shell downs the bank target');

  // ------------------------------------------------------ guide vs shells --
  /* The range's walls, poles (barriers) and edges through the shared
     guide: each real shell's first event lies within one step of the
     guide's contact, and a ricochet turns the way the guide says. */
  label = 'guide';
  for (const id of [1, 2, 3, 4, 5]) d.setTankActive(id, false);
  R.kind.fill(0);
  const cases = [
    [0, -5, 0],                 // straight up the lane to the backstop
    [-.6, -5, .28],             // off the left lane divider
    [-7.2, -4.4, -8 * Math.PI / 256], // bank yard
    [4.3, -6, .9],              // across the drive area
    [-1.2, -6.5, -2.5],         // into the near corner
    [2.9, -5, .337],            // a slalom pole stops it
  ];
  let stops = 0;
  for (const [x, z, angle] of cases) {
    aim(x, z, angle);
    d.setKeyboard('fire', true); d.stepSimulation(1);
    const guide = d.aimGuide();
    d.setKeyboard('fire', false);
    const real = release(guide.originX, guide.originZ);
    const step = 7.2 / 60;
    if (guide.kind1 === 2) stops++;
    check(real.first, `an event ${x},${z}`);
    const [kind, ex, ez, vx, vz] = real.first;
    check(kind === (guide.kind1 === 1 ? 'bounce' : 'stop'), `event kind ${x},${z}: ${kind} vs ${guide.kind1}`);
    check(Math.hypot(ex - guide.hitX, ez - guide.hitZ) <= step + 1e-3, `contact ${x},${z}: ${[ex, ez]} vs ${[guide.hitX, guide.hitZ]}`);
    if (kind === 'bounce')
      check(Math.sign(vx) === Math.sign(guide.bounceX) && Math.sign(vz) === Math.sign(guide.bounceZ),
        `bounce direction ${x},${z}`);
  }
  check(stops >= 1, 'a pole stops a shell');

  // Instances: the guide draws in the range scene within the cap.
  label = 'instances';
  P.begin(0); d.step(30);
  aim(-7.2, -3.2, 74 * Math.PI / 256); d.step(4);
  const s1 = d.snapshot();
  check(s1.aimBoxes >= 3 && s1.aimBoxes <= 5, 'Full guide drawn ' + s1.aimBoxes);
  check(s1.boxInstances <= s1.frameInstanceCeiling && s1.boxInstances <= s1.instanceLimit,
    'range frame within the instance ceiling ' + s1.boxInstances + '/' + s1.frameInstanceCeiling);
  R.aim = 0; d.step(2);
  check(d.snapshot().aimBoxes === 0, 'Off draws no guide');
  R.aim = 2;
  d.saturateVisualLoad();
  d.stepBudgetedHud(2);
  const sat = d.snapshot();
  check(sat.boxInstances <= sat.frameInstanceCeiling, 'never past the cap ' + sat.boxInstances);

  // ------------------------------------------------------- live switches --
  label = 'switches';
  P.begin(0); d.step(2);
  B.setMode('paused'); d.stepSimulation(1);
  check(menu.screen === 'range' && labels()[0] === 'Resume', 'pause shows the Range panel ' + labels());
  check(/Shots \d+ \| Hits \d+ \(\d+%\) \| Bank hits \d+/.test($('message').textContent), 'range stats');
  check($('legend').textContent.includes('O Resume'), 'range legend');
  const class0 = B.tanks[0].classId;
  click('Class');
  const class1 = B.tanks[0].classId;
  check(class1 === (class0 + 1) % 3 && labels()[2] === 'Class ' + B.CLASSES[class1].name
    && B.tanks[0].health === B.CLASSES[class1].health && B.state.classChoice === class1, 'class switches live ' + labels());
  click('Class'); click('Class');
  check(B.tanks[0].classId === class0, 'class cycles');
  const gadget0 = B.tanks[0].gadget;
  click('Gadget'); check(B.tanks[0].gadget !== gadget0 && labels()[3].includes(B.tanks[0].gadget), 'gadget switches');
  click('Targets'); check(R.behaviour === 1 && [1, 2, 3].every(id => R.kind[id] === 2), 'targets: moving');
  click('Targets'); check(R.behaviour === 2 && [1, 2, 3, 4, 5].every(id => !B.tanks[id].active), 'targets: off');
  click('Targets'); check(R.behaviour === 0 && [1, 2, 3].every(id => R.kind[id] === 1 && B.tanks[id].active), 'targets: still');
  check(R.aim === 2 && labels()[5] === 'Aim guide Full', 'guide at Full ' + labels());
  click('Aim guide'); check(R.aim === 0 && labels()[5] === 'Aim guide Off', 'guide toggles Off');
  click('Aim guide'); check(R.aim === 1 && labels()[5] === 'Aim guide Sight', 'then Sight');
  click('Aim guide'); check(R.aim === 2 && d.aimGuideLevel() === 2, 'then Full');
  click('Controls');
  check(menu.screen === 'controls', 'Controls link opens the Controls menu');
  menu.cycleScheme(1); $('assist-choice').click();
  check(d.snapshot().controls === 1 && d.snapshot().assist === 1, 'scheme and assist switch from the range');
  menu.back();
  check(menu.screen === 'range' && labels()[1] === 'Controls CLASSIC / SNAP', 'back to the panel ' + labels());
  menu.back(); d.stepSimulation(1);
  check(B.state.mode === 'playing' && menu.screen === 'game', 'O resumes the range');
  d.selectControls(0); d.selectAimAssist(0);

  // Moving targets drive slow lateral patterns inside the lane.
  label = 'moving';
  R.behaviour = 1; P.begin(0);
  const xs = [];
  for (let at = 0; at < 300; at++) { d.stepSimulation(1); xs.push(B.tanks[2].x); }
  const span = Math.max(...xs) - Math.min(...xs);
  check(span > 1.5 && Math.max(...xs.map(Math.abs)) <= 1.26 && B.tanks[2].z === P.layout.LANE_Z[1],
    'moving target sweeps its lane ' + span);
  let fastest = 0;
  for (let at = 1; at < xs.length; at++) fastest = Math.max(fastest, Math.abs(xs[at] - xs[at - 1]) * 60);
  check(fastest < 1.3, 'slow enough to lead ' + fastest);
  R.behaviour = 0;

  // ---------------------------------------------------------------- drills --
  label = 'slalom';
  const realNow = Date.now;
  function runSlalom(stepsPerGate) {
    B.setMode('paused'); d.stepSimulation(1); click('Drills'); click('Slalom');
    check(R.drill === 1 && B.state.mode === 'playing' && d.snapshot().pickups === 1, 'slalom starts with one gate');
    for (let gate = 0; gate < P.layout.COURSE.length; gate++) {
      d.stepSimulation(stepsPerGate);
      const [x, z] = P.layout.COURSE[gate];
      check(B.pickups[0].active && near(B.pickups[0].x, x, 1e-9), 'gate ' + gate + ' shown');
      const objective = d.objective();
      check(near(objective.x, x, 1e-6) && near(objective.z, z, 1e-6),
        'objective marker points at the next gate');
      d.setTankPosition(0, x, z); d.stepSimulation(2);
    }
    d.stepSimulation(1);
  }
  runSlalom(60);
  check(B.state.mode === 'paused' && menu.screen === 'drilldone', 'slalom completes into its result');
  const first = R.result;
  check(first.record && near(first.tenths / 10, P.layout.COURSE.length * 62 / 60 + .02, .25), 'time ' + first.tenths);
  check(P.record.t[0] === first.tenths && /new best/.test($('message').textContent), 'first time is the best');
  check(toasts.includes('PIVOT') && toasts.includes('LUNGE') && toasts.includes('DRIFT')
    && toasts.includes('FINISH'), 'cues ' + toasts);
  const stored = JSON.parse(localStorage.getItem('treadline-campaign-v1'));
  check(stored.pr && stored.pr.v === P.VERSION && stored.pr.t[0] === first.tenths, 'best time saved');
  click('Retry'); check(R.drill === 1 && B.state.mode === 'playing', 'retry');
  menu.toMain(); click('Practice Range'); // fresh entry keeps the record
  click('Slalom');
  for (let gate = 0; gate < P.layout.COURSE.length; gate++) {
    d.stepSimulation(80); const [x, z] = P.layout.COURSE[gate]; d.setTankPosition(0, x, z); d.stepSimulation(2);
  }
  d.stepSimulation(1);
  check(R.result && !R.result.record && P.record.t[0] === first.tenths, 'a slower run keeps the best');
  check(/\(best \d/.test($('message').textContent), 'result names the best');
  runSlalom(20);
  check(R.result.record && P.record.t[0] < first.tenths, 'a faster run sets a new best');
  menu.back(); d.stepSimulation(1);
  check(R.drill === 0 && B.state.mode === 'playing' && menu.screen === 'game', 'O on the result returns to the free range');

  label = 'bank drill';
  B.setMode('paused'); d.stepSimulation(1); click('Drills'); click('Bank shots');
  check([1, 2, 3].every(id => !B.tanks[id].active) && B.tanks[4].active && B.tanks[5].active, 'bank drill targets');
  for (let down = 0; down < 4; down++) {
    d.stepSimulation(90);
    const id = B.tanks[4].active ? 4 : 5;
    const hits = R.hits;
    P.damage(B.tanks[id], 0, 0, 0);
    check(B.tanks[id].health === 18 && R.hits === hits,
      'drill ignores direct hits, and accuracy does not count them');
    B.tanks[id].health -= P.damage(B.tanks[id], 30, 0, 1);
    d.stepSimulation(1);
  }
  d.stepSimulation(1);
  check(menu.screen === 'drilldone' && R.result.drill === 2 && P.record.t[1] > 0, 'bank drill completes');

  B.startOrResume(); d.stepSimulation(2);
  check(R.drill === 0 && !R.done && B.state.mode === 'playing' && menu.screen === 'game',
    'START on a result resumes into the free range');
  B.setMode('paused'); d.stepSimulation(1);
  label = 'moving drill';
  click('Drills'); click('Moving targets');
  check([1, 2, 3].every(id => R.kind[id] === 2) && !B.tanks[4].active, 'moving drill targets');
  for (let down = 0; down < 5; down++) {
    d.stepSimulation(90);
    const id = [1, 2, 3].find(k => B.tanks[k].active);
    B.tanks[id].health -= P.damage(B.tanks[id], 99, 0, 0);
    d.stepSimulation(1);
  }
  d.stepSimulation(1);
  check(menu.screen === 'drilldone' && R.result.drill === 3 && P.record.t[2] > 0, 'moving drill completes');
  for (const text of toasts) check(/^[ 0-9A-Z\-\/]{1,12}$/.test(text), 'toast fits the HUD glyphs ' + text);

  // ------------------------------------------------------------ leaving --
  label = 'leave';
  const lifetime = P.record.s[0];
  click('Leave');
  check(!P.on && menu.screen === 'main' && B.state.mode === 'title', 'Leave returns to the main menu');
  check(arena.enemies === 5 && B.tanks.every(t => !t.inert), 'range state restored');
  check(P.record.s[0] >= lifetime && P.record.s[1] <= P.record.s[0], 'lifetime stats kept');
  menu.show('quick'); d.selectMode(0); d.start(); d.stepSimulation(30);
  check(B.state.arena === 0 && [1, 2, 3].every(id => B.tanks[id].active && !B.tanks[id].inert)
    && d.replayState().recording, 'Quick Match afterwards is ordinary (bots think, replay records)');
  menu.toMain();

  // ----------------------------------------------------------------- save --
  label = 'save';
  const base = JSON.parse(localStorage.getItem('treadline-campaign-v1'));
  const good = {...base, pr: {v: 1, t: [123, 0, 456], s: [10, 7, 3]}};
  C.loadSave(JSON.stringify(good));
  check(P.record.t.join() === '123,0,456' && P.record.s.join() === '10,7,3' && !P.record.corrupt, 'loads its section');
  C.loadSave(JSON.stringify({...base, pr: {v: 2, t: [1, 2, 3], s: [1, 1, 1]}}));
  check(P.record.corrupt && P.record.t.join() === '0,0,0' && C.save.d === base.d, 'unknown version resets only practice');
  for (const bad of [{v: 1, t: 'x', s: [0, 0, 0]}, {v: 1, t: [1, 2], s: [0, 0, 0]},
    {v: 1, t: [-4, 0, 0], s: [0, 0, 0]}, {v: 1, t: [0, 0, 0], s: [1, 5, 0]}, 'nope', 7])
    { C.loadSave(JSON.stringify({...base, pr: bad})); check(P.record.corrupt && P.record.t[0] === 0, 'malformed ' + JSON.stringify(bad)); }
  C.loadSave(JSON.stringify({...base, pr: undefined}));
  check(!P.record.corrupt && P.record.t[0] === 0, 'a save without the section is not corrupt');
  C.loadSave('{not json');
  check(C.save.corrupt && P.record.t[0] === 0, 'a damaged campaign save starts practice fresh too');
  C.loadSave(JSON.stringify(good)); C.writeSave();
  check(JSON.parse(localStorage.getItem('treadline-campaign-v1')).pr.t[2] === 456, 'round trip');

  B.showToast = realToast;
  Date.now = realNow;
  C.loadSave(savedCampaign.length ? savedCampaign : ''); C.writeSave();
  d.selectControls(0); d.selectAimAssist(1); d.savePreferences();
  globalThis.pocSummary = 'TREADLINE-PRACTICE-PASS';
})();
