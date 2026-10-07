/* Camera distance regressions (presentation only; see updateCamera).
   A wall in front of the player once pumped the camera: its probe started
   at a look target that the live distance placed inside the wall, so it
   snapped to 1.2, which shortened the lead, which cleared the probe, which
   eased it out again every few frames. Synthetic setups on the authored
   arenas and seeded bot fights; no captured data. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const T = B.tanks;
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(label + ': ' + what);
  }
  function settle(frames) {
    let low = Infinity, high = -Infinity;
    for (let at = 0; at < frames; at++) {
      d.stepCamera(1);
      const distance = d.snapshot().cameraDistance;
      if (at >= frames / 2) { low = Math.min(low, distance); high = Math.max(high, distance); }
    }
    return {low, high};
  }
  function setup(x, z) {
    C.menu.toMain(); d.selectMode(0); d.selectClass(1); d.selectCamera(0); d.start();
    d.freezeBots(true);
    for (let id = 1; id < 6; id++) d.setTankActive(id, false);
    d.clearCrates();
    for (let at = 0; at < 6; at++) d.setBarrierActive(at, false);
    d.setTankPosition(0, x, z); d.setTankHeading(0, 0);
  }

  // A wall just ahead of the player leaves the distance where it is with
  // the wall away: the look target is walked back out of the wall.
  label = 'wall ahead';
  setup(1.5, .5);
  const open = settle(180);
  check(open.high - open.low < .01 && open.low > 6, 'open floor settles far: ' + JSON.stringify(open));
  d.setBarrierRect(0, .5, 2.5, 1.35, 1.75);
  const walled = settle(120);
  check(walled.high - walled.low < .01 && Math.abs(walled.low - open.low) < .01,
    'a wall ahead does not move the camera: ' + JSON.stringify({open, walled}));

  // Facing one wall with another at its back, the low sight line meets
  // scenery: the camera retracts to an exact, steady distance.
  label = 'walls ahead and behind';
  const player = T[0], radius = player.collisionRadius;
  d.setBarrierRect(0, .5, 2.5, .5 + radius + .05, .9 + radius);
  d.setBarrierRect(1, .5, 2.5, .1 - radius, .5 - radius - .05);
  const boxed = settle(90);
  check(boxed.high - boxed.low < .01 && boxed.low > 1.2 && boxed.low < open.low - .05,
    'boxed in, the camera retracts and holds: ' + JSON.stringify(boxed));
  // The farther distance has to stay clear for a while before it expands.
  d.setBarrierActive(1, false);
  d.stepCamera(9);
  check(Math.abs(d.snapshot().cameraDistance - boxed.low) < .01, 'expansion waits for a held clear');
  const freed = settle(180);
  check(Math.abs(freed.low - open.low) < .01, 'and then expands: ' + JSON.stringify(freed));
  d.freezeBots(false);

  // Seeded bot fights in both camera modes: no pumping.
  for (const camera of [0, 1]) {
    label = 'bot fight camera ' + camera;
    C.menu.toMain(); d.selectCamera(camera);
    d.beginLeague(5, 1, 1, 0, 1);
    d.step(2); // the first frame takes the new arena's distance
    let previous = d.snapshot().cameraDistance, jumps = 0, flips = 0, last = 0;
    for (let frame = 0; frame < 150 && T[0].active && T[1].active; frame++) {
      d.step(2);
      const distance = d.snapshot().cameraDistance, delta = distance - previous;
      if (delta < -.3) jumps++;
      const direction = delta > .002 ? 1 : delta < -.002 ? -1 : 0;
      if (direction && last && direction !== last) flips++;
      if (direction) last = direction;
      previous = distance;
    }
    /* The follow camera still swings in and out as the hull turns toward
       and away from the edge (a few slow sweeps); the pump flipped ~40
       times in these 150 frames. */
    check(jumps === 0 && flips <= 8, `no pumping: jumps=${jumps} flips=${flips}`);
    d.finishLeague();
  }
  C.menu.toMain(); d.selectCamera(0);
  globalThis.pocSummary = 'TREADLINE-CAMERA-PASS';
})();
