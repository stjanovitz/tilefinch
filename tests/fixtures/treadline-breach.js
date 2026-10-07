/* Pockets and breaches (see "Pockets and breaches" in bots.js): a bot
   sealed in by destructible scenery shoots its way out; placement never
   leaves a tank sealed in by indestructible scenery, and keeps a
   destructible-only pocket about half the time, deterministically.
   Synthetic setups on the authored arenas and seeded Onslaught ground. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const T = B.tanks, DT = 1 / 30;
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(label + ': ' + what);
  }
  /* Box the corner x, z > 4 of arena 0 in: the arena edge, an unbreakable
     wall at x 4..4.4 and a wall at z 3.9..4.3, breakable unless solid. */
  function boxCorner(solid) {
    d.setBarrierRect(0, 4, 4.4, 3.9, 7.85);
    d.setBarrierRect(1, 4, 7.85, 3.9, 4.3);
    if (!solid) {
      B.barriers[1].health = 2;
      B.refreshScenery();
    }
  }
  function quickMatch() {
    C.menu.toMain(); d.selectMode(0); d.selectClass(1); d.selectDifficulty(1); d.start();
    d.freezeBots(true);
    for (let id = 1; id < 6; id++) d.setTankActive(id, false);
    d.clearCrates();
    for (let at = 0; at < 6; at++) d.setBarrierActive(at, false);
    d.spawnStats(); // settle the arena start's queued placement checks
  }
  function placeBulwark(x, z) {
    const tank = T[1];
    B.placeTank(tank, x, z, Math.PI, 1, B.GADGETS[0], 'HUNTER', 2);
    tank.difficulty = 1;
    return tank;
  }

  // A Bulwark boxed into a corner by a breakable barrier shoots it down and
  // comes out to the fight, purposefully: it plans one breach and fires.
  label = 'breach out';
  quickMatch();
  boxCorner(false);
  const bulwark = placeBulwark(6, 6);
  check(d.spawnVerdict(1) === 1, 'the corner is a breakable pocket');
  d.freezeBots(false);
  let out = -1;
  for (let frame = 0; frame < 30 * 25 && out < 0; frame++) {
    d.stepSimulation(1, DT);
    check(bulwark.active, 'the Bulwark survives its own breach');
    if (bulwark.z < 3.4) out = frame;
  }
  const breach = d.breachState(1);
  check(out >= 0, 'out of the pocket within 25 s: ' + JSON.stringify(breach)
    + ' at ' + bulwark.x.toFixed(2) + ', ' + bulwark.z.toFixed(2));
  check(!B.barriers[1].active && breach.shots >= 2 && breach.shots <= 6,
    'the barrier was shot down with a few shells: ' + JSON.stringify(breach));
  // ...and goes on toward the player.
  const before = Math.hypot(bulwark.x - T[0].x, bulwark.z - T[0].z);
  d.stepSimulation(90, DT);
  check(Math.hypot(bulwark.x - T[0].x, bulwark.z - T[0].z) < before - 1,
    'it heads for the fight');

  // Sealed in by unbreakable walls, a placed tank is moved where it routes.
  label = 'sealed';
  quickMatch();
  boxCorner(true);
  const sealed = placeBulwark(6, 6);
  check(d.spawnVerdict(1) === 2, 'the corner is sealed');
  const stats = d.spawnStats();
  B.validateSpawn(sealed);
  check(d.spawnVerdict(1) === 0 && d.spawnStats().sealedMoved === stats.sealedMoved + 1,
    'moved out of the sealed corner: ' + sealed.x + ', ' + sealed.z);
  check(!B.circleHitsObstacle(sealed.x, sealed.z, sealed.collisionRadius),
    'into a clear spot');

  // A breakable pocket is kept about half the time: a coin from the
  // simulation's random state, the tank and its cell, never advancing it.
  label = 'coin';
  const coins = [];
  for (let seed = 1; seed <= 400; seed++) {
    d.beginLeague(seed, 1, 1, 0, 0);
    d.setTankPosition(1, 6, 6);
    coins.push(d.pocketKept(1) ? 1 : 0);
  }
  const kept = coins.reduce((sum, coin) => sum + coin, 0);
  check(kept > 160 && kept < 240, 'about half kept: ' + kept + '/400');
  for (let seed = 1; seed <= 400; seed += 37) {
    d.beginLeague(seed, 1, 1, 0, 0);
    d.setTankPosition(1, 6, 6);
    check((d.pocketKept(1) ? 1 : 0) === coins[seed - 1], 'the coin repeats');
  }
  d.finishLeague();
  // validateSpawn follows the coin.
  label = 'coin placement';
  let keeps = 0, moves = 0;
  for (let trial = 0; trial < 12; trial++) {
    quickMatch();
    boxCorner(false);
    const tank = placeBulwark(5.5 + (trial % 3) * .5, 5.5 + ((trial / 3) | 0) * .4);
    if (d.spawnVerdict(1) !== 1) continue;
    const coin = d.pocketKept(1);
    B.validateSpawn(tank);
    const stays = d.spawnVerdict(1) === 1;
    check(stays === coin, 'placement follows the coin (trial ' + trial + ')');
    if (stays) keeps++; else moves++;
  }
  check(keeps > 0 && moves > 0, 'both outcomes seen: kept ' + keeps + ', moved ' + moves);

  // Seeded Onslaught waves: no bot starts sealed in.
  label = 'onslaught';
  C.menu.toMain(); d.selectMode(3); d.start();
  for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
  let checked = 0;
  for (let seed = 1; seed <= 24; seed++) {
    d.generateArenaSeed(Math.imul(seed, 0x2545f491) >>> 0);
    for (const wave of [1, 5, 10]) {
      d.setWave(wave);
      for (let id = 1; id < 6; id++) {
        if (!T[id].active) continue;
        checked++;
        check(d.spawnVerdict(id) !== 2, `seed ${seed} wave ${wave} tank ${id} sealed in`);
      }
    }
  }
  check(checked > 200, 'coverage ' + checked);
  C.menu.toMain(); d.freezeBots(false);
  globalThis.pocSummary = 'TREADLINE-BREACH-PASS';
})();
