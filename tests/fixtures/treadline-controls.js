/* Gunner scheme, movement extras (pivot, lunge, drift), their replay and
   online encodings. Synthetic: a stubbed pad, keyboard events and direct
   calls into controls.js; no captured data or network. */
(() => {
  const d = __treadlineDebug, K = d.controls, T = K.tuning;
  const C = d.campaign, B = C.bridge();
  function check(value, label) {
    if (!value) throw new Error(label + ': ' + JSON.stringify(d.tankState(0)));
  }
  const near = (a, b, eps) => Math.abs(a - b) <= eps;
  const wrap = (a) => Math.atan2(Math.sin(a), Math.cos(a));
  document.getElementById('online-cancel').click();

  // ------------------------------------------------------------ pad stub --
  const realPads = navigator.getGamepads;
  const pad = {connected: true, axes: [0, 0, 0, 0],
    buttons: Array.from({length: 17}, () => ({pressed: false, value: 0}))};
  let padOn = true;
  navigator.getGamepads = () => [padOn ? pad : null];
  function hold(...ids) {
    for (const b of pad.buttons) b.pressed = false;
    for (const id of ids) pad.buttons[id].pressed = true;
  }
  function nub(x, y) { pad.axes[0] = x; pad.axes[1] = y; }
  function idle() { hold(); nub(0, 0); }
  function setup(scheme, assist = 0) {
    idle();
    d.selectMode(0); d.selectClass(1); d.start(); d.freezeBots(true);
    for (let id = 2; id < 6; id++) d.setTankActive(id, false);
    d.setTankPosition(1, 6, 6);
    d.setTankPosition(0, 0, -5.8); d.setTankHeading(0, 0);
    d.selectControls(scheme); d.selectAimAssist(assist); d.selectCamera(0);
    d.resetInputProbe(); d.stepSimulation(1);
  }

  // ---------------------------------------------------- Gunner traverse --
  setup(2);
  check(document.getElementById('controls-value').textContent === 'GUNNER',
    'Gunner label');
  check(/Square\/Circle turn turret/.test(document.getElementById('controls').textContent),
    'Gunner hint');
  let angle = K.gunnerAngle();
  check(near(angle, 0, 1e-9), 'Gunner starts on the turret');
  hold(1); d.stepSimulation(1); idle(); d.stepSimulation(1);
  const tapStep = K.gunnerAngle() - angle;
  check(tapStep > .009 && tapStep < .0115, 'a Circle tap is a fine adjustment ' + tapStep);
  angle = K.gunnerAngle();
  hold(1);
  const steps = [];
  for (let at = 0; at < 45; at++) {
    const before = K.gunnerAngle(); d.stepSimulation(1);
    steps.push(wrap(K.gunnerAngle() - before));
  }
  for (let at = 1; at < 30; at++)
    check(steps[at] >= steps[at - 1] - 1e-9, 'traverse accelerates at ' + at);
  check(near(steps[44], T.TRAVERSE_MAX / 60, 1e-6), 'traverse caps below turret slew');
  check(T.TRAVERSE_MAX < 3.8, 'max traverse below turret rate');
  const turretLag = Math.abs(wrap(d.tankState(0).turret - K.gunnerAngle()));
  check(turretLag < .2, 'turret keeps up with held traverse ' + turretLag);
  angle = K.gunnerAngle();
  hold(2); d.stepSimulation(3);
  check(K.gunnerAngle() < angle, 'Square traverses the other way');
  angle = K.gunnerAngle();
  hold(1, 2); d.stepSimulation(5);
  check(K.gunnerAngle() === angle, 'Square+Circle together hold');
  idle(); d.stepSimulation(1);
  angle = K.gunnerAngle(); d.setPaused(true);
  hold(1); d.stepSimulation(6);
  check(d.snapshot().mode === 'playing', 'O resumes from pause');
  check(K.gunnerAngle() === angle, 'the O that resumes does not crank the gun');
  idle(); d.stepSimulation(1); hold(1); d.stepSimulation(2); idle(); d.stepSimulation(1);
  check(K.gunnerAngle() > angle, 'a fresh press traverses again');

  // Triangle snaps the aim to the hull, which pivots under it.
  hold(14); d.stepSimulation(30); idle(); d.stepSimulation(1);
  const hullYaw = d.tankState(0).yaw;
  check(Math.abs(wrap(hullYaw - K.gunnerAngle())) > .5, 'hull and aim differ');
  hold(3); const snapped = d.pollInput(); idle(); d.pollInput();
  check(near(K.gunnerAngle(), hullYaw, 1e-9) && near(snapped.aimX, Math.sin(hullYaw), 1e-9)
    && near(snapped.aimZ, Math.cos(hullYaw), 1e-9), 'Triangle snaps to hull facing');

  // Gunner buttons: R fire, L secondary, X/Up gadget, Down Command.
  d.resetInputProbe(); hold(4); let p = d.pollInput();
  check(p.secondary && !p.fire && !p.gadget, 'L is secondary');
  d.resetInputProbe(); hold(0); p = d.pollInput();
  check(p.gadget && !p.fire, 'X is the gadget');
  d.resetInputProbe(); hold(12); p = d.pollInput();
  check(p.gadget, 'Up is the gadget');
  d.resetInputProbe(); hold(13); p = d.pollInput();
  check(p.ultimate, 'Down is Command');
  d.resetInputProbe(); hold(5); p = d.pollInput();
  check(p.fire && !p.secondary, 'R holds fire');
  const shots = d.snapshot().shots;
  hold(5); d.stepSimulation(2); idle(); d.stepSimulation(1);
  check(d.snapshot().shots === shots + 1, 'R release fires');
  // Arcade keeps X as an aim button, not the gadget.
  d.selectControls(0); d.resetInputProbe(); hold(0); p = d.pollInput();
  check(!p.gadget && p.aimZ < -.99, 'Arcade X still aims toward the camera');
  d.selectControls(2);

  // Assist: Snap nudges once after a traverse; Lock tracks; never mid-press.
  setup(2, 1);
  d.setTankPosition(1, Math.sin(.3) * 4, -5.8 + Math.cos(.3) * 4);
  hold(1); d.stepSimulation(1);
  const held = K.gunnerAngle();
  check(held < .05, 'Snap does not pull a held traverse ' + held);
  idle(); d.stepSimulation(1);
  const nudged = K.gunnerAngle();
  check(nudged > .15 && nudged < .3, 'Snap nudges toward the target on release ' + nudged);
  d.stepSimulation(3);
  check(K.gunnerAngle() === nudged, 'Snap nudges only once');
  setup(2, 2);
  d.setTankPosition(1, Math.sin(.3) * 4, -5.8 + Math.cos(.3) * 4);
  hold(1); d.stepSimulation(1); idle(); d.stepSimulation(1);
  check(near(K.gunnerAngle(), .3, .01), 'Lock acquires on release ' + K.gunnerAngle());
  d.setTankPosition(1, Math.sin(.6) * 4, -5.8 + Math.cos(.6) * 4);
  d.stepSimulation(1);
  check(near(K.gunnerAngle(), .6, .01), 'Lock tracks ' + K.gunnerAngle());
  hold(2); d.stepSimulation(4);
  check(K.gunnerAngle() < .58, 'manual traverse overrides lock');
  idle(); d.stepSimulation(1);

  // ------------------------------------------------------------- pivot --
  for (const scheme of [0, 1, 2]) {
    setup(scheme);
    hold(15); p = d.pollInput();
    check(p.left === -1 && p.right === 1 && !p.reverse, 'D-pad Right pivots ' + scheme);
    hold(14); p = d.pollInput();
    check(p.left === 1 && p.right === -1, 'D-pad Left pivots ' + scheme);
  }
  setup(0);
  const start = d.tankState(0);
  hold(15);
  let frames = 0;
  while (frames < 120 && wrap(d.tankState(0).yaw - start.yaw) >= 0
      && Math.abs(wrap(d.tankState(0).yaw - start.yaw)) < Math.PI - .06) {
    d.stepSimulation(1); frames++;
  }
  const spun = d.tankState(0);
  check(frames >= 40 && frames <= 50, 'pivot 180 in ~0.77 s: ' + frames + ' frames');
  check(near(spun.x, start.x, 1e-6) && near(spun.z, start.z, 1e-6), 'pivot stays in place');
  idle(); d.stepSimulation(1);
  // Keyboard pivot.
  padOn = false; d.stepSimulation(1);
  d.setKeyboard('pivotLeft', true); p = d.pollInput(); d.setKeyboard('pivotLeft', false);
  check(p.left === 1 && p.right === -1, 'keyboard Z pivots left');
  padOn = true;

  // ------------------------------------------------------------- lunge --
  function doubleTap(press, release, first = 3, gap = 3) {
    release(); d.stepSimulation(10);
    press(); d.stepSimulation(first); release(); d.stepSimulation(gap);
    press(); d.stepSimulation(1); const after = d.tankState(0);
    release(); d.stepSimulation(1); return after;
  }
  setup(0);
  let t = doubleTap(() => nub(0, -1), idle);
  check(t.lunge > 0 && near(t.lungeCooldown, T.LUNGE_COOLDOWN - 1 / 60, 1e-6), 'nub double tap lunges');
  t = doubleTap(() => nub(0, -1), idle);
  check(t.lungeCooldown < T.LUNGE_COOLDOWN - .2, 'cooldown blocks a second lunge');
  setup(0);
  t = doubleTap(() => nub(0, -1), idle, 20);
  check(t.lunge === 0 && t.lungeCooldown === 0, 'a long first press is not a tap');
  setup(0);
  t = doubleTap(() => nub(0, -1), idle, 3, 20);
  check(t.lunge === 0, 'a slow second press is not a double tap');
  setup(0);
  idle(); d.stepSimulation(10); nub(0, -1); d.stepSimulation(3); idle(); d.stepSimulation(2);
  nub(0, 1); d.stepSimulation(1);
  check(d.tankState(0).lungeCooldown === 0, 'opposite flicks do not lunge');
  setup(1);
  t = doubleTap(() => hold(4, 5), idle);
  check(t.lunge > 0, 'Classic L+R double press lunges');
  setup(2);
  t = doubleTap(() => nub(.9, 0), idle);
  check(t.lunge > 0, 'Gunner nub double tap lunges');
  padOn = false;
  for (const scheme of [0, 1]) {
    setup(scheme); padOn = false; d.stepSimulation(1);
    t = doubleTap(() => d.setKeyboard('w', true), () => d.setKeyboard('w', false));
    check(t.lunge > 0, 'keyboard W double tap lunges ' + scheme);
  }
  padOn = true;

  // Lunge physics on a synthetic hull: distance, framerate independence.
  function hull() {
    return {command: {lunge: 0}, yawSine: 0, yawCosine: 1, slideX: 0, slideZ: 0,
      driveSpeed: 2.55, lunge: 0, lungeCooldown: 0, lungeSign: 1, drift: 0,
      boss: false};
  }
  function run(dt, seconds, lunge, terrain = -1, turnAt = 99) {
    const h = hull(); let z = 0, x = 0;
    h.slideZ = 2.55;
    h.command.lunge = lunge;
    for (let time = 0; time < seconds - 1e-9; time += dt) {
      if (time >= turnAt) { h.yawSine = 1; h.yawCosine = 0; }
      K.slide(h, dt, terrain, time >= turnAt ? .5 : 1, 2.55, time >= turnAt ? 1 : 0);
      x += h.slideX * dt; z += h.slideZ * dt;
    }
    return {x, z, h};
  }
  const plain = run(1 / 60, 1, 0).z, lunged = run(1 / 60, 1, 1).z;
  const extra = lunged - plain;
  check(extra > .8 && extra < 1.3, 'lunge adds about one unit: ' + extra);
  const coarse = run(1 / 30, 1, 1).z - run(1 / 30, 1, 0).z;
  check(Math.abs(coarse - extra) < .08, 'lunge is framerate independent ' + coarse + ' vs ' + extra);
  check(run(1 / 60, 1, -1).z < plain - .8, 'reverse lunge pushes backwards');
  const boss = hull(); boss.boss = true; boss.command.lunge = 1;
  K.slide(boss, 1 / 60, -1, 1, 2.55, 0);
  check(boss.lunge === 0 && boss.command.lunge === 0, 'bosses never lunge');

  // Drift: a fast hull that turns hard keeps sliding along its old line.
  const grip = run(1 / 60, .5, 0, -1, .1);      // normal speed: no drift
  const drift = run(1 / 60, .5, 1, -1, .2);     // turns just after a lunge
  check(grip.h.drift === 0 && grip.z < .3, 'normal-speed turns keep grip');
  check(drift.z > grip.z + .5, 'post-lunge turn drifts on its old line ' + drift.z);
  const ice = run(1 / 60, .5, 1, 0, .2);
  check(ice.z > drift.z, 'ice drifts further ' + ice.z + ' vs ' + drift.z);
  const ice30 = run(1 / 30, .5, 1, 0, .2);
  check(Math.abs(ice30.z - ice.z) < .12, 'drift is framerate independent ' + ice30.z + ' vs ' + ice.z);
  const regain = run(1 / 60, 1.5, 1, -1, .2);
  check(regain.h.drift === 0 && near(regain.h.slideX, 1.275, .01) && near(regain.h.slideZ, 0, .01),
    'traction returns after the drift');

  // --------------------------------------------- bots: dodge and pivot --
  d.selectDifficulty(2); setup(0);
  const bot = B.tanks[1];
  bot.difficulty = 2; d.setTankPosition(1, 0, 0); d.setTankHeading(1, Math.PI / 2);
  let dodged = 0;
  for (let at = 0; at < 40; at++) {
    bot.lungeCooldown = 0; bot.command.lunge = 0;
    K.dodge(B.tanks[0]);
    if (bot.command.lunge) dodged++;
  }
  check(dodged > 8 && dodged < 32, 'Ace dodges a flank shot about 45%: ' + dodged);
  d.setTankHeading(1, 0); bot.command.lunge = 0;
  for (let at = 0; at < 40; at++) K.dodge(B.tanks[0]);
  check(!bot.command.lunge, 'a head-on bot cannot dodge along its hull');
  bot.difficulty = 0; d.setTankHeading(1, Math.PI / 2);
  for (let at = 0; at < 40; at++) K.dodge(B.tanks[0]);
  check(!bot.command.lunge, 'Cadets never dodge');
  bot.difficulty = -1; d.selectDifficulty(1);

  // ---------------------------------------- replay with all three extras --
  setup(0); d.freezeBots(false); d.selectGadget(4);
  d.start(); idle();
  padOn = false; d.stepSimulation(1);
  d.setKeyboard('w', true); d.step(3); d.setKeyboard('w', false); d.step(2);
  d.setKeyboard('w', true); d.step(1);
  const lungedRun = d.tankState(0).lungeCooldown > 0;
  d.step(5); d.setKeyboard('gadget', true); d.step(1); d.setKeyboard('gadget', false);
  d.step(12); d.setKeyboard('w', false); d.setKeyboard('d', true); d.step(4);
  const drifted = d.tankState(0).drift > 0;
  d.step(8); d.setKeyboard('d', false);
  d.setKeyboard('pivotRight', true); d.step(20); d.setKeyboard('pivotRight', false);
  d.step(6);
  d.replayStop();
  check(lungedRun, 'recorded run lunged');
  check(drifted, 'recorded run drifted');
  const code = d.exportReplay();
  check(code.startsWith('TR1.') && d.importReplay(code) && d.replayStart(), 'extras replay imports');
  d.step(d.replayState().count);
  check(d.replayState().verified && d.snapshot().mode === 'replay-done', 'extras replay is identical');
  // Version 5 codes predate signed treads and lunges; refuse them.
  const parts = code.split('.'), bin = atob(parts[2]);
  const bytes = new Uint8Array(bin.length);
  for (let at = 0; at < bin.length; at++) bytes[at] = bin.charCodeAt(at);
  new DataView(bytes.buffer).setFloat64(0, 9, true);
  let hash = 2166136261;
  for (let at = 0; at < bytes.length; at++) hash = Math.imul(hash ^ bytes[at], 16777619) >>> 0;
  let text = '';
  for (let at = 0; at < bytes.length; at++) text += String.fromCharCode(bytes[at]);
  check(!d.importReplay(`TR1.${hash.toString(16)}.${btoa(text)}`), 'version 9 replay refused');
  padOn = true;

  // ---------------------------------------------------- online packets --
  const OT = __treadlineOnlineTest;
  setup(0); padOn = false;
  document.getElementById('online-host').click();
  __tilefinchDeliverMultiplayer(71, 'invitecode', undefined, 'internet', 0, false, 0, '123456789012', '');
  __tilefinchDeliverMultiplayer(71, 'peerrequest', undefined, 'incoming', 0, false, 99, '', 'Guest');
  document.getElementById('online-accept').click();
  __tilefinchDeliverMultiplayer(71, 'open', undefined, '', 0, false, 99, '', 'Guest');
  check(d.snapshot().onlineRole === 'host', 'host seam');
  const yaw1 = d.tankState(1).yaw, x1 = d.tankState(1).x;
  function inputPacket(version, sequence, flags) {
    const buffer = new ArrayBuffer(12), view = new DataView(buffer);
    view.setUint8(0, 1); view.setUint8(1, version); view.setUint16(2, sequence, true);
    view.setUint16(4, flags, true); view.setInt16(8, 32767, true);
    return buffer;
  }
  // Right pivot (left tread negative) plus a lunge; a later packet in the
  // same step must not drop the lunge edge.
  __tilefinchDeliverMultiplayer(71, 'binary', inputPacket(5, 1, 1 | 2 | 256 | 1024), '', 0, false, 99, '', '');
  __tilefinchDeliverMultiplayer(71, 'binary', inputPacket(5, 2, 1 | 2 | 256), '', 0, false, 99, '', '');
  d.stepSimulation(6);
  const remote = d.tankState(1);
  check(wrap(remote.yaw - yaw1) > .3 && remote.lungeCooldown > 0, 'host applies pivot and lunge');
  __tilefinchDeliverMultiplayer(71, 'binary', inputPacket(4, 3, 3), '', 0, false, 99, '', '');
  check(!d.snapshot().onlineActive
    && /different Treadline version/.test(document.getElementById('online-message').textContent),
    'old-version peer is refused with a message');
  document.getElementById('online-cancel').click();
  // Guest: pivot and a lunge between packets reach the host bytes.
  document.getElementById('online-code').click();
  const keypad = document.getElementById('code-keypad');
  for (let at = 0; at < 12; at++) keypad.querySelector('button').click();
  keypad.querySelector('[data-key="join"]').click();
  __tilefinchDeliverMultiplayer(71, 'open', undefined, '', 0, false, 99, '', 'Host');
  check(d.snapshot().onlineRole === 'guest', 'guest seam');
  d.resetInputProbe(); d.stepSimulation(1);
  d.setKeyboard('pivotRight', true); d.stepSimulation(4);
  const sent = new DataView(OT.sentLast);
  check(sent.getUint8(0) === 1 && sent.getUint8(1) === 5
    && (sent.getUint16(4, true) & 0x303) === 0x103,
    'guest sends signed treads ' + sent.getUint16(4, true));
  d.setKeyboard('pivotRight', false);
  __tilefinchDeliverMultiplayer(71, 'drain', undefined, '', 0, false, 99, '', '');
  d.stepSimulation(4);
  d.setKeyboard('w', true); d.stepSimulation(2); d.setKeyboard('w', false); d.stepSimulation(1);
  d.setKeyboard('w', true); d.stepSimulation(1); d.setKeyboard('w', false);
  let lungeSent = false;
  for (let at = 0; at < 6; at++) {
    d.stepSimulation(1);
    if (new DataView(OT.sentLast).getUint16(4, true) & 1024) lungeSent = true;
    __tilefinchDeliverMultiplayer(71, 'drain', undefined, '', 0, false, 99, '', '');
  }
  check(lungeSent, 'guest lunge reaches a packet');
  document.getElementById('online-cancel').click();

  navigator.getGamepads = realPads;

  // -------------------------------------------------------------- menus --
  const menu = C.menu, $ = (id) => document.getElementById(id);
  d.selectControls(0); menu.toMain(); menu.show('settings');
  const names = [];
  for (let at = 0; at < 3; at++) { $('controls-choice').click(); names.push($('controls-value').textContent); }
  check(names.join() === 'CLASSIC,GUNNER,ARCADE', 'Controls cycles three schemes ' + names);
  check(/^Arcade:/.test($('controls').textContent), 'hint follows the scheme');
  check($('legend').textContent === 'X Select | O Back | L/R Tabs | START Deploy', 'default legend');
  // Controls submenu: reachable from Settings and Pause, scheme selector
  // plus assist/reverse, the scheme as one action per row on two L/R pages.
  const visible = (root) => [...$(root).children].filter(e => !e.hidden);
  const listLabels = () => visible('menu').map(b => b.textContent);
  check(listLabels().join() === 'Controls,Back', 'Settings links to Controls ' + listLabels());
  visible('menu')[0].click();
  check(menu.screen === 'controls' && document.activeElement === $('controls-choice'),
    'Controls opens on the scheme selector');
  check(/Scheme/.test($('legend').textContent) && /L\/R Page/.test($('legend').textContent),
    'Controls legend');
  for (let scheme = 0; scheme < 3; scheme++) for (let page = 0; page < 2; page++)
    for (const pad of [0, 1]) {
      const rows = K.BINDINGS[scheme][pad][page];
      check(rows.length >= 4 && rows.length <= 6, 'binding page size ' + scheme + page + pad);
      for (const text of rows)
        check(/^[^|]{1,24}\|[^|]{1,32}$/.test(text) && !/[\u25b3\u2715]/.test(text),
          'binding row ' + text);
    }
  const shownRows = () => visible('bindings').map(r => r.firstChild.textContent + '=' + r.lastChild.textContent);
  const expected = (scheme, page) => K.bindings(page, scheme).map(t => t.replace('|', '='));
  check(shownRows().join() === expected(0, 0).join(), 'Arcade driving rows ' + shownRows());
  menu.switchTab(1);
  check(shownRows().join() === expected(0, 1).join(), 'L/R pages to shooting');
  menu.cycleScheme(1); menu.cycleScheme(1);
  check($('controls-value').textContent === 'GUNNER' && d.snapshot().controls === 2
    && shownRows().some(r => r.startsWith('Square / Circle=turn turret')),
    'scheme selector switches the list ' + shownRows());
  menu.cycleScheme(-1);
  check($('controls-value').textContent === 'CLASSIC' && shownRows()[0] === 'Nub=aim', 'scheme cycles back');
  check(!/\|/.test($('controls').textContent) || $('controls').textContent.length < 90,
    'quick-match hint is one short pointer');
  check(/Controls/.test($('controls').textContent), 'hint points at Controls');
  menu.back();
  check(menu.screen === 'settings', 'O returns to Settings');
  menu.cycleScheme(1); // back to Arcade
  d.selectMode(0); d.start(); B.setMode('paused'); d.stepSimulation(1);
  check(menu.screen === 'pause' && listLabels().includes('Controls'), 'Pause links to Controls ' + listLabels());
  menu.show('controls');
  check(menu.screen === 'controls' && B.state.mode === 'paused', 'Controls from pause keeps the run paused');
  menu.back();
  check(menu.screen === 'pause' && B.state.mode === 'paused', 'O returns to Pause');
  // The end panel shows once its screen has filled it, never the last one's text.
  B.setMode('playing'); B.setMode('game-over');
  check($('panel').hidden, 'the end panel waits for the results screen');
  d.stepSimulation(1);
  check(!$('panel').hidden && menu.screen === 'results'
    && document.querySelector('h1').textContent === 'TANK LOST'
    && /^Final score \d+\. The arena held\./.test($('message').textContent), 'results fill the panel');
  // Backing out of the code keypad returns to Multiplayer and leaves the
  // other screens' setup groups alone (Garage keeps its Paint row).
  menu.toMain(); menu.show('multiplayer');
  $('online-code').click(); $('code-cancel').click();
  check(menu.screen === 'multiplayer' && document.querySelector('h1').textContent === 'MULTIPLAYER'
    && !$('online-actions').hidden && $('code-entry').hidden, 'keypad Back returns to Multiplayer');
  $('online-code').click(); menu.back(); menu.show('garage');
  check(!$('preferences').hidden, 'Garage keeps its Paint row after the keypad');
  menu.toMain();
  const saved = JSON.stringify(C.save);
  C.loadSave(''); C.save.d = 1; C.save.k = 0x3ff; C.setRadio(0);
  for (let at = 0; at < 5; at++) C.save.m[at] = 15;
  menu.toMain(); menu.show('campaign');
  const tabs = [...$('tabs').children].filter(b => !b.hidden).map(b => b.textContent);
  check(tabs.join() === '1 YARD,2 FOUNDRY,3 GLACIER,4 CITY,5 FORTRESS', 'short theater tabs ' + tabs);
  check(document.querySelector('h1').textContent === 'PROVING YARD'
    && /\[ALL MEDALS\] \[ACE\] Medals/.test($('message').textContent), 'ribbons move to the message');
  menu.show('faults');
  const faultButtons = [...$('menu').children].filter(b => !b.hidden);
  check(faultButtons.every(b => !b.textContent.includes('|')), 'fault rows carry no note');
  // Notes follow focus once it rests (0.15 s of menu polls), not per step.
  const before = $('message').textContent;
  faultButtons[0].focus(); menu.poll(0); menu.poll(0.1);
  check($('message').textContent === before, 'a moving focus leaves the message alone');
  menu.poll(0.1);
  check($('message').textContent === 'Score x1.00 | ' + C.faults[0][3], 'focused fault explains itself');
  faultButtons[1].focus(); menu.poll(0); menu.poll(0.2);
  check(faultButtons[1].disabled || $('message').textContent.endsWith(C.faults[1][3]), 'second fault note');
  for (let at = 0; at < 5; at++) C.save.m[at] = 0;
  menu.toMain(); menu.select(0); menu.show('briefing'); menu.skipRadio();
  const radioRows = () => [...$('radio').children].filter(r => !r.hidden).map(r => r.textContent);
  const lastPage = radioRows();
  check(C.missions[0].brief.length === 3 && lastPage.length === 1
    && lastPage[0].includes('Targets are up'), 'radio shows its newest page ' + lastPage);
  check($('legend').textContent.includes('L/R Radio'), 'briefing legend names radio paging');
  menu.switchTab(-1);
  const firstPage = radioRows();
  check(firstPage.length === 2 && firstPage[0].includes('RECRUIT'), 'L pages back ' + firstPage);
  menu.switchTab(-1);
  check(radioRows()[0] === firstPage[0], 'paging clamps at the first page');
  menu.back();
  check(menu.screen === 'main' && $('legend').textContent.includes('L/R Tabs'), 'O backs out; legend resets');
  C.loadSave(saved.length ? saved : ''); C.writeSave();

  // Escape toggles pause exactly like P: one press pauses, one resumes.
  for (const key of ['Escape', 'p']) {
    d.selectMode(0); d.start(); d.freezeBots(true); d.step(2);
    const press = () => {
      dispatchEvent(new KeyboardEvent('keydown', {key, cancelable: true}));
      d.step(1);
      dispatchEvent(new KeyboardEvent('keyup', {key, cancelable: true}));
      d.step(3);
    };
    press();
    check(B.state.mode === 'paused' && C.menu.screen === 'pause', key + ' pauses');
    press();
    check(B.state.mode === 'playing', key + ' on the pause menu resumes, once');
    press();
    C.menu.toMain();
  }

  d.selectControls(0); d.selectAimAssist(1); d.savePreferences();
  globalThis.pocSummary = 'TREADLINE-CONTROLS-PASS';
})();
