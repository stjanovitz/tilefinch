/* Planning-cost rewrites against the code they replaced, on the authored
   arenas and seeded Onslaught ground: route fields (distances and queue
   order on every hull grid) and route waypoints must give exactly the old
   answers. The old code lives here as the oracle; the game exposes its
   route internals (routeInternals). Synthetic states only. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  let label = 'start';
  function check(value, what) {
    if (!value) throw new Error(label + ': ' + what);
  }
  function xorshift(seed) {
    let value = seed >>> 0 || 1;
    return () => {
      value ^= value << 13; value ^= value >>> 17; value ^= value << 5;
      return (value >>> 0) / 4294967296;
    };
  }
  // The bounds-checked route field search the template search replaced.
  function referenceField(R, gridIndex, blocked, distances, queue, cell, head, total, stats) {
    const {NAV_CELLS, NAV_SIDE, navDiagonal} = R;
    let read = head, count = total;
    if (read >= count) return [read, count];
    const required = [];
    if (cell >= 0) {
      const cx = cell & 15, cz = cell >> 4;
      for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
        const x = cx + dx, z = cz + dz;
        if (x < 0 || x >= 16 || z < 0 || z >= 16) continue;
        if (!blocked[z * NAV_SIDE + x]) required.push(z * NAV_SIDE + x);
      }
    }
    for (let layers = 0; layers <= NAV_CELLS; layers++) {
      let missing = cell < 0;
      for (const at of required) if (distances[at] === 65535) missing = true;
      if (!missing || read >= count || read >= NAV_CELLS) break;
      const layerEnd = count;
      while (read < layerEnd && read < NAV_CELLS) {
        const from = queue[read++], x = from & 15, z = from >> 4;
        const distance = distances[from] + 1;
        const steps = [[x > 0, from - 1], [x < 15, from + 1],
          [from >= NAV_SIDE, from - NAV_SIDE], [from < NAV_CELLS - NAV_SIDE, from + NAV_SIDE]];
        for (const [inside, step] of steps)
          if (inside && !blocked[step] && distances[step] === 65535) {
            distances[step] = distance; queue[count++] = step;
          }
        const west = x > 0 && blocked[from - 1], east = x < 15 && blocked[from + 1];
        const north = z > 0 && blocked[from - NAV_SIDE], south = z < 15 && blocked[from + NAV_SIDE];
        if ((west || east) && (north || south)) {
          for (let side = 0; side < 4; side++) {
            if (side === 0 ? !(east && south) : side === 1 ? !(west && south)
                : side === 2 ? !(west && north) : !(east && north)) continue;
            const step = side === 0 ? from + NAV_SIDE + 1 : side === 1 ? from + NAV_SIDE - 1
              : side === 2 ? from - NAV_SIDE - 1 : from - NAV_SIDE + 1;
            if (blocked[step] || distances[step] !== 65535
                || !(side === 0 ? navDiagonal(gridIndex, from, true)
                  : side === 1 ? navDiagonal(gridIndex, from, false)
                  : side === 2 ? navDiagonal(gridIndex, step, true)
                  : navDiagonal(gridIndex, step, false))) continue;
            distances[step] = distance; queue[count++] = step;
            stats.diagonals++;
          }
        }
      }
    }
    return [read, count];
  }
  // The in-order waypoint scan the ranked choice replaced.
  function referenceWaypoint(R, tank, cell, grid, base, padding) {
    const {navDistances, lineCrossesWalls, NAV_SIDE} = R;
    let best = Infinity, chosen = -1, nearest = -1, nearestDistance = 65535;
    const cx = cell & 15, cz = cell >> 4;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const x = cx + dx, z = cz + dz;
      if (x < 0 || x >= 16 || z < 0 || z >= 16) continue;
      const at = z * NAV_SIDE + x;
      if (grid[at] || navDistances[base + at] === 65535) continue;
      const wx = x - 7.5, wz = z - 7.5;
      const px = wx - tank.x, pz = wz - tank.z;
      const score = navDistances[base + at] + (px * px + pz * pz) * .2;
      if (navDistances[base + at] < nearestDistance) {
        nearestDistance = navDistances[base + at]; nearest = at;
      }
      if (score < best && !lineCrossesWalls(tank.x, tank.z, wx, wz, false,
          padding, tank.id * 12 + 11)) { best = score; chosen = at; }
    }
    if (nearest >= 0 && nearest !== chosen && nearest !== cell
        && (chosen < 0 || nearestDistance < navDistances[base + chosen])
        && !lineCrossesWalls(tank.x, tank.z, (nearest & 15) - 7.5, (nearest >> 4) - 7.5,
          false, tank.collisionRadius * .5, tank.id * 12 + 11)) chosen = nearest;
    return chosen;
  }
  // Start a field for goal in scratch slot 0 (cleared afterwards).
  function startField(R, gridIndex, goal) {
    R.navDistances.set(R.navFieldTemplateViews[gridIndex], 0);
    R.navDistances[goal] = 0; R.navQueue[0] = goal;
    R.navQueueHeads[0] = 0; R.navQueueCounts[0] = 1;
    R.navFieldGoals[0] = goal + gridIndex * R.NAV_CELLS;
  }
  function releaseSlot(R) {
    R.navFieldGoals[0] = R.navFieldRevisions[0] = -1;
    for (const tank of R.tanks) if (tank.navFieldSlot === 0) tank.navFieldSlot = -1;
  }
  function openGoal(grid, random) {
    let goal = (random() * 256) | 0;
    for (let probe = 0; probe < 256 && grid[goal]; probe++) goal = (goal + 37) % 256;
    return grid[goal] ? -1 : goal;
  }
  // Fields: same distances (blocked cells keep the sentinel), queue, head, count.
  function compareFields(seed, stats) {
    const R = d.routeInternals(), random = xorshift(seed);
    const distances = new Uint16Array(256), queue = new Uint16Array(256);
    for (let gridIndex = 0; gridIndex < 4; gridIndex++) {
      const blocked = R.navGrids[gridIndex];
      for (let pair = 0; pair < 96; pair++) {
        const goal = openGoal(blocked, random);
        if (goal < 0) break;
        const bots = pair % 3 ? [(random() * 256) | 0, (random() * 256) | 0] : [-1];
        startField(R, gridIndex, goal);
        distances.fill(65535); distances[goal] = 0; queue[0] = goal;
        let head = 0, total = 1;
        for (const bot of bots) {
          R.prepareBotRouteField(0, bot);
          [head, total] = referenceField(R, gridIndex, blocked, distances, queue, bot,
            head, total, stats);
          stats.compared++;
          let same = R.navQueueHeads[0] === head && R.navQueueCounts[0] === total;
          for (let cell = 0; same && cell < 256; cell++)
            same = blocked[cell] ? R.navDistances[cell] === R.NAV_FIELD_BLOCKED
              : R.navDistances[cell] === distances[cell];
          for (let at = 0; same && at < total; at++) same = R.navQueue[at] === queue[at];
          check(same, `route field parity (grid ${gridIndex}, goal ${goal}, bot ${bot})`);
        }
      }
    }
    releaseSlot(R);
  }
  // Waypoints for a live tank moved to seeded clear spots (restored after).
  function compareWaypoints(seed, index, count) {
    const R = d.routeInternals(), random = xorshift(seed), tank = R.tanks[index];
    const savedX = tank.x, savedZ = tank.z, gridIndex = R.navGridIndex(tank);
    const grid = R.navGrids[gridIndex];
    let found = 0;
    for (let at = 0; at < count; at++) {
      const x = random() * 15 - 7.5, z = random() * 15 - 7.5;
      if (R.circleHitsObstacle(x, z, tank.collisionRadius)) continue;
      tank.x = x; tank.z = z;
      const goal = openGoal(grid, random);
      if (goal < 0) break;
      startField(R, gridIndex, goal);
      const cell = R.navigationCell(x, z);
      R.prepareBotRouteField(0, cell);
      const padding = R.routePadding(tank);
      const chosen = R.chooseRouteWaypoint(tank, cell, grid, 0, padding);
      check(chosen === referenceWaypoint(R, tank, cell, grid, 0, padding),
        `waypoint parity (tank ${index} at ${x.toFixed(2)}, ${z.toFixed(2)})`);
      if (chosen >= 0) found++;
    }
    tank.x = savedX; tank.z = savedZ;
    releaseSlot(R);
    return found;
  }
  /* Shared and evicted fields (was the game's routeCacheProbe): two
     Bulwark-sized hulls with one goal share one field search; more goals
     than slots evict it and a rebuild recovers exactly the original
     distances (an independent four-way plus diagonal search); a navigation
     rebuild forces exactly one new search. */
  function sharedRoutes() {
    const R = d.routeInternals(), tanks = R.tanks, blocked = R.navGrids[2];
    R.navFieldGoals.fill(-1); R.navFieldRevisions.fill(-1);
    for (const tank of tanks) tank.navGoal = tank.navFieldSlot = -1;
    let goal = 0;
    while (goal < 256 && blocked[goal]) goal++;
    if (goal === 256) return {shared: false};
    const gx = (goal & 15) - 7.5, gz = (goal >> 4) - 7.5;
    const saved = [1, 2, 3].map(id => [tanks[id].classId, tanks[id].collisionRadius]);
    for (let id = 1; id <= 3; id++) {
      tanks[id].classId = 2; tanks[id].collisionRadius = R.CLASS_COLLISION_RADIUS[2];
    }
    const previous = R.profiling(true), counts = R.qualificationAICounts;
    try {
      const before = counts[1];
      R.steerBotRoute(tanks[1], gx, gz);
      const firstSlot = tanks[1].navFieldSlot;
      R.prepareBotRouteField(firstSlot, -1);
      const reference = new Uint16Array(256).fill(65535), queue = new Uint16Array(256);
      reference[goal] = 0; queue[0] = goal;
      for (let read = 0, count = 1; read < count;) {
        const cell = queue[read++], x = cell & 15, z = cell >> 4;
        for (let side = 0; side < 4; side++) {
          if ((side === 0 && !x) || (side === 1 && x === 15)
              || (side === 2 && !z) || (side === 3 && z === 15)) continue;
          const next = cell + (side === 0 ? -1 : side === 1 ? 1 : side === 2 ? -16 : 16);
          if (blocked[next] || reference[next] !== 65535) continue;
          reference[next] = reference[cell] + 1; queue[count++] = next;
        }
        for (let side = 0; side < 4; side++) {
          const next = side === 0 ? (x < 15 && z < 15 && R.navDiagonal(2, cell, true) ? cell + 17 : -1)
            : side === 1 ? (x && z < 15 && R.navDiagonal(2, cell, false) ? cell + 15 : -1)
            : side === 2 ? (z && x && R.navDiagonal(2, cell - 17, true) ? cell - 17 : -1)
            : (z && x < 15 && R.navDiagonal(2, cell - 15, false) ? cell - 15 : -1);
          if (next < 0 || blocked[next] || reference[next] !== 65535) continue;
          reference[next] = reference[cell] + 1; queue[count++] = next;
        }
      }
      const matches = (base) => {
        for (let cell = 0; cell < 256; cell++)
          if (blocked[cell] ? R.navDistances[base + cell] !== R.NAV_FIELD_BLOCKED
              : reference[cell] !== R.navDistances[base + cell]) return false;
        return true;
      };
      const originalExact = matches(firstSlot * 256);
      R.steerBotRoute(tanks[2], gx, gz);
      const searches = counts[1] - before;
      const shared = firstSlot >= 0 && firstSlot === tanks[2].navFieldSlot;
      let visits = 0;
      for (let cell = goal + 1; cell < 256 && visits <= R.MAX_TANKS; cell++) {
        if (blocked[cell]) continue;
        R.steerBotRoute(tanks[3], (cell & 15) - 7.5, (cell >> 4) - 7.5);
        visits++;
      }
      R.steerBotRoute(tanks[1], gx, gz);
      R.prepareBotRouteField(tanks[1].navFieldSlot, -1);
      const exact = visits > R.MAX_TANKS && originalExact && matches(tanks[1].navFieldSlot * 256);
      const revisionBefore = counts[1];
      R.invalidateBotNavigation(); R.drain();
      R.steerBotRoute(tanks[1], gx, gz);
      return {searches, shared, exact, refreshed: counts[1] - revisionBefore === 1};
    } finally {
      R.profiling(previous);
      for (let id = 1; id <= 3; id++) [tanks[id].classId, tanks[id].collisionRadius] = saved[id - 1];
    }
  }
  label = 'shared routes';
  d.beginLeague(12345, 1, 2, 0, 0);
  const routes = sharedRoutes();
  check(routes.searches === 1 && routes.shared && routes.exact && routes.refreshed,
    'shared and evicted route fields ' + JSON.stringify(routes));
  d.finishLeague();

  const stats = {compared: 0, diagonals: 0};
  for (let arena = 0; arena < 3; arena++) {
    label = 'arena ' + arena;
    d.beginLeague(11 + arena, 1, 1, 0, arena);
    d.stepLeague(30);
    compareFields(arena + 1, stats);
    check(compareWaypoints(arena + 5, 1, 768) > 400, 'waypoints found');
    d.finishLeague();
  }
  // Diagonal links (corner channels) must be part of what was compared.
  check(stats.compared > 1200 && stats.diagonals > 0, 'route coverage ' + JSON.stringify(stats));
  // Onslaught ground, with every hull class and the boss on its grids.
  label = 'onslaught';
  C.menu.toMain(); d.selectMode(3); d.start();
  for (let at = 0; at < 900 && d.snapshot().arenaGenerationPhase; at++) d.step(1);
  check(B.state.arena === 3, 'Onslaught forged');
  for (const seed of [3, 7]) {
    d.generateArenaSeed(Math.imul(seed, 0x2545f491) >>> 0);
    d.setWave(5);
    d.freezeBots(true); d.stepSimulation(2, 1 / 30); d.freezeBots(false);
    compareFields(seed + 20, stats);
    const classes = new Set();
    for (let id = 1; id < 6; id++) {
      const tank = B.tanks[id];
      if (!tank.active) continue;
      classes.add(tank.boss ? 3 : tank.classId);
      compareWaypoints(seed * 7 + id, id, 256);
    }
    check(classes.size === 4, 'every hull grid exercised: ' + [...classes]);
  }
  C.menu.toMain();
  globalThis.pocSummary = 'TREADLINE-PLANNING-PASS';
})();
