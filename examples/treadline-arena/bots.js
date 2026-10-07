/* Treadline Arena: bot AI.

   Navigation grids and route fields (updateBotNavigation refreshes the
   occupancy in slices), waypoint choice and route steering, pocket checks
   at spawn and breach planning, the planning queue (botPlanAdmit), target,
   aim, bank-shot and fire decisions, and the per-bot command
   (updateBotCommand). This code moved out of game.js unchanged.

   Loaded before game.js, which calls the factory below once, after the
   state it reads exists, and keeps the returned functions in constants:
   calls stay direct and every name here is a closure variable, as it was in
   game.js. The few game.js values that are reassigned (whether the bot
   league drives the player, qualification.js's statistics, smoke count,
   random state, the last player shot) come through getters on rare paths; setBotProfiling mirrors the per-step profiling flag. The ray index
   (routePadding) and the shell cast (breachShotHits) stay in game.js beside
   the state they read. Nothing here allocates per frame. */
globalThis.__treadlineCreateBots = (B) => {
  "use strict";
  /* var, not const: QuickJS reads a closure var without the TDZ check a
     lexical binding costs, as it read game.js's function declarations. */
  var {campaign, qualificationLongSoak, MODE_SURVIVAL, MODE_CONTROL, MODE_CONVOY,
    MODE_ONSLAUGHT, MODE_DAILY, MODE_BILLIARDS, MODE_DUEL,
    ARENAS, arenaSpatial, CLASS_COLLISION_RADIUS, BOSS, DAILY_DIFFICULTY, MAX_TANKS, MAX_PICKUPS,
    MAX_BARRIERS, ARENA_EDGE, HULL_EDGE, CRATE_HALF, BOT_FIRE_RANGE_SQUARED,
    BOT_VOLLEY_GAP, botVolleyDelay, QUICK_SPAWN_MIN_DISTANCE, tanks, pickups, barriers,
    crates, state, convoy, online, sounds, playerTank, random, wrapAngle,
    isOnslaught, orientationIndex, spawnBlocked, circleHitsObstacle,
    surfaceHeightAt, elevatedFiringOrigin, lineCrossesSmoke, lineCrossesWalls,
    routePadding, breachShotHits, invalidateBotNavigation,
    setQualificationAIActive, playerIsBot, qualificationStats, activeSmokeCount,
    randomState, lastBotPlayerShotAt} = B;
  // Phase clocks: qualification.js installs them (setProfileArrays).
  var qualificationAITimes = null, qualificationAICounts = null;
  // game.js's per-step profiling flag, kept in step by setBotProfiling.
  let qualificationAIActive = false;

  const BOT_REACTION = [.32, .2, .12];
  const BOT_ACCURACY = [.28, .2, .13];
  const BOT_FIRE_CHANCE = [.24, .36, .46];
  const BOT_FIRE_WINDUP = [.5, .42, .35];
  const botDifficultyTables = [BOT_REACTION,BOT_ACCURACY,BOT_FIRE_CHANCE,BOT_FIRE_WINDUP];
  const botWaveValues = new Array(12).fill(0);
  let botWaveCached = -1;
  const BOT_BACKOFF_ENTER_SQUARED = 4.41;
  const BOT_BACKOFF_EXIT_SQUARED = 7.29;
  const BOT_STANDOFF_RELEASE_SQUARED = 13.69;

  /* Fixed storage: sliced occupancy refresh and <=256 visits per search.
     Distance fields depend on goal/grid revision, not the starting tank.
     Share the same six-field storage across matching goals. */
  const NAV_SIDE = 16, NAV_CELLS = NAV_SIDE * NAV_SIDE;
  const navBlocked = new Uint8Array(NAV_CELLS);
  const navDistances = new Uint16Array(MAX_TANKS * NAV_CELLS);
  // Each shared field keeps its original BFS frontier for exact on-demand extension.
  const navQueue = new Uint16Array(MAX_TANKS * NAV_CELLS);
  const navQueueHeads = new Uint16Array(MAX_TANKS);
  const navQueueCounts = new Uint16Array(MAX_TANKS);
  const navRequiredCells = new Uint16Array(9);
  // Per hull grid, a route field's start: 65535 open, NAV_FIELD_BLOCKED not.
  const NAV_FIELD_BLOCKED = 65534;
  const navFieldTemplates = new Uint16Array(4 * NAV_CELLS);
  const navFieldTemplateViews = [0, 1, 2, 3].map(grid =>
    navFieldTemplates.subarray(grid * NAV_CELLS, (grid + 1) * NAV_CELLS));
  /* One occupancy grid per hull size, each with its own clearance (the
     hull radius plus .06): Scout, Striker, Bulwark (navBlocked, the
     diagnostics' grid) and boss (BOSS). A single Bulwark grid closed gaps a
     Scout or Striker fits through, and opened ones a boss does not, so
     those bots drove at a gap they could not use, or past one they could,
     and dithered at its mouth. Field slots key the goal cell plus
     NAV_CELLS times the grid. */
  const BOSS_COLLISION_RADIUS = BOSS.collisionRadius;
  const navGrids = [new Uint8Array(NAV_CELLS), new Uint8Array(NAV_CELLS), navBlocked,
    new Uint8Array(NAV_CELLS)];
  const navOccupancies = [
    arenaSpatial.createNavigationCache(CLASS_COLLISION_RADIUS[0] + .06),
    arenaSpatial.createNavigationCache(CLASS_COLLISION_RADIUS[1] + .06),
    arenaSpatial.createNavigationCache(CLASS_COLLISION_RADIUS[2] + .06),
    arenaSpatial.createNavigationCache(BOSS_COLLISION_RADIUS + .06)];
  function navGridIndex(tank) {
    return tank.collisionRadius > CLASS_COLLISION_RADIUS[2] + 1e-6 ? 3 : tank.classId;
  }
  /* Diagonal links where the two cells between are blocked but the hull
     still fits along the diagonal (a padded sight line between the
     centres). The four-way grid could not see such a channel (two
     barriers' corners either side of an open gate, say), so routing called
     the far side unreachable and bots drove at the wall. Decided when a
     route search first reaches the pair and kept per grid revision: per
     grid and lower cell, bit 1 links x+1, z+1 and bit 2 x-1, z+1; bits 4
     and 8 record that each was decided. */
  const navDiagonals = new Uint8Array(NAV_CELLS * 4);
  const NAV_DIAGONAL_PADDING = [CLASS_COLLISION_RADIUS[0] + .02,
    CLASS_COLLISION_RADIUS[1] + .02, CLASS_COLLISION_RADIUS[2] + .02,
    BOSS_COLLISION_RADIUS + .02];
  function navDiagonal(index, cell, right) {
    const at = index * NAV_CELLS + cell, decided = right ? 4 : 8, link = right ? 1 : 2;
    let flags = navDiagonals[at];
    if (!(flags & decided)) {
      flags |= decided;
      const grid = navGrids[index], x = cell & 15, z = cell >> 4;
      const step = right ? 1 : -1;
      if (z < 15 && (right ? x < 15 : x > 0) && !grid[cell]
          && !grid[cell + NAV_SIDE + step] && grid[cell + NAV_SIDE] && grid[cell + step]
          && !lineCrossesWalls(x - 7.5, z - 7.5, x - 7.5 + step, z - 6.5, false,
            NAV_DIAGONAL_PADDING[index])) flags |= link;
      navDiagonals[at] = flags;
    }
    return (flags & link) !== 0;
  }
  function prepareNavigationGrids() {
    for (let grid = 0; grid < 4; grid++)
      navOccupancies[grid].prepare(state.arena, barriers, crates);
  }
  /* ---- Pockets and breaches ----
     Shells destroy crates and barriers (health 2), not walls, the edge, the
     gate or unbreakable barriers (zone walls). Placement never seals a tank
     in (validateSpawns) and keeps a small destructible-only pocket about
     half the time; a bot with no way out shoots one (planBotBreach). */
  const BREACH_UNBREAKABLE = 1e6;
  // Navigation mask bits blocking and indestructible...
  function navSolidBits() {
    let bits = state.gateOpen ? 1 : 3;
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (barrier.active && !(barrier.health < BREACH_UNBREAKABLE)) bits |= 4 << at;
    }
    return bits;
  }
  // ...and blocking and destructible.
  function navBreakableBits() {
    let bits = 0;
    for (let at = 0; at < MAX_BARRIERS; at++) {
      const barrier = barriers[at];
      if (barrier.active && barrier.health < BREACH_UNBREAKABLE) bits |= 4 << at;
    }
    for (let at = 0; at < crates.length; at++) if (crates[at].active) bits |= 256 << at;
    return bits;
  }

  // Four-way reachability on row bit masks: sweeps, rows spread sideways.
  function floodRows(open, seed, reach) {
    reach.fill(0);
    if (!(seed >= 0)) return;
    const row = seed >> 4, bit = 1 << (seed & 15);
    if (!(open[row] & bit)) return;
    reach[row] = bit;
    floodGrow(open, reach);
  }
  function floodGrow(open, reach) {
    for (let pass = 0; pass < 64; pass++) {
      let changed = false;
      for (let step = 0; step < 32; step++) {
        const r = step < 16 ? step : 31 - step;
        let mask = (reach[r] | (r > 0 ? reach[r - 1] : 0) | (r < 15 ? reach[r + 1] : 0)) & open[r];
        if (!mask) continue;
        for (let spread = 0; spread < 16; spread++) {
          const next = (mask | (mask << 1) | (mask >>> 1)) & open[r];
          if (next === mask) break;
          mask = next;
        }
        if (mask !== reach[r]) { reach[r] = mask; changed = true; }
      }
      if (!changed) break;
    }
  }
  // Plus diagonal steps where the hull fits (navDiagonal's sight test).
  function floodDiagonals(open, reach, gridIndex) {
    const padding = NAV_DIAGONAL_PADDING[gridIndex];
    for (let round = 0; round < 8; round++) {
      let added = false;
      for (let r = 0; r < 16; r++) {
        const blocked = ~open[r] & 0xffff;
        let candidates = 0;
        for (let side = 0; side < 2; side++) {
          const from = side ? r + 1 : r - 1;
          if (from < 0 || from > 15 || !reach[from]) continue;
          const beside = ~open[from] & 0xffff;
          candidates |= ((reach[from] & (beside >>> 1) & blocked) << 1)
            | ((reach[from] & (beside << 1) & blocked) >>> 1);
        }
        candidates &= open[r] & ~reach[r];
        while (candidates) {
          const x = 31 - Math.clz32(candidates & -candidates);
          candidates &= candidates - 1;
          for (let side = 0; side < 4; side++) {
            const fromRow = side < 2 ? r - 1 : r + 1, fromX = side & 1 ? x + 1 : x - 1;
            if (fromRow < 0 || fromRow > 15 || fromX < 0 || fromX > 15
                || !(reach[fromRow] & (1 << fromX)) || (open[fromRow] & (1 << x))
                || (open[r] & (1 << fromX))) continue;
            if (lineCrossesWalls(fromX - 7.5, fromRow - 7.5, x - 7.5, r - 7.5, false, padding))
              continue;
            reach[r] |= 1 << x; added = true;
            break;
          }
        }
      }
      if (!added) break;
      floodGrow(open, reach);
    }
  }

  /* Per hull grid, cells blocked by indestructible scenery ("solid") and by
     anything ("full"), and each one's largest region: where the fight is. */
  const sealSolid = new Uint8Array(NAV_CELLS), sealFull = new Uint8Array(NAV_CELLS);
  const sealSolidRows = new Int32Array(16), sealFullRows = new Int32Array(16);
  const sealSolidReach = new Int32Array(16), sealFullReach = new Int32Array(16);
  const sealSeen = new Int32Array(16), sealRegion = new Int32Array(16);
  const SEAL_CELL_REACH = 1.25 * 1.25;
  // The nearest open cell to (x, z), if near enough.
  function sealCell(grid, x, z) {
    const cell = arenaSpatial.nearestOpenCell(grid, x, z);
    if (cell < 0) return -1;
    const dx = (cell & 15) - 7.5 - x, dz = (cell >> 4) - 7.5 - z;
    return dx * dx + dz * dz <= SEAL_CELL_REACH ? cell : -1;
  }
  function rowCells(rows) {
    let cells = 0;
    for (let r = 0; r < 16; r++) for (let row = rows[r]; row; row &= row - 1) cells++;
    return cells;
  }
  function largestRegion(open, reach) {
    let best = 0;
    reach.fill(0); sealSeen.fill(0);
    for (let r = 0; r < 16; r++) {
      for (let left = open[r] & ~sealSeen[r]; left; left = open[r] & ~sealSeen[r]) {
        floodRows(open, r * 16 + (31 - Math.clz32(left & -left)), sealRegion);
        for (let at = 0; at < 16; at++) sealSeen[at] |= sealRegion[at];
        const cells = rowCells(sealRegion);
        if (cells > best) { best = cells; reach.set(sealRegion); }
      }
    }
  }
  const SPAWN_ROUTABLE = 0, SPAWN_BREAKABLE = 1, SPAWN_SEALED = 2;
  let sealPrepared = -1;
  function prepareSealGrids(gridIndex) {
    const masks = navOccupancies[gridIndex].blockers();
    const active = navSolidBits() | navBreakableBits();
    let open = 0;
    for (let r = 0, cell = 0; r < 16; r++) {
      let row = 0;
      for (let x = 0; x < 16; x++, cell++) if (!(masks[cell] & active)) row |= 1 << x;
      sealFullRows[r] = row;
      for (let bits = row; bits; bits &= bits - 1) open++;
    }
    // The player's region is the main one when it is large; else the largest.
    const player = playerTank();
    floodRows(sealFullRows, player.active ? navigationCell(player.x, player.z) : -1,
      sealFullReach);
    if (rowCells(sealFullReach) * 3 < open) largestRegion(sealFullRows, sealFullReach);
    sealExtended = false;
    sealPrepared = gridIndex;
  }
  /* The rest, only for a tank not plainly in the main region: diagonal
     channels, cell grids for nearest-cell lookups, the solid region. */
  let sealExtended = false;
  function extendSealGrids() {
    if (sealExtended) return;
    const masks = navOccupancies[sealPrepared].blockers();
    // A closed gate opens by the mode's rules: it seals nothing for good.
    const solid = navSolidBits() & ~2;
    sealSolidRows.fill(0);
    for (let cell = 0; cell < NAV_CELLS; cell++) {
      sealFull[cell] = sealFullRows[cell >> 4] & (1 << (cell & 15)) ? 0 : 1;
      if (masks[cell] & solid) sealSolid[cell] = 1;
      else { sealSolid[cell] = 0; sealSolidRows[cell >> 4] |= 1 << (cell & 15); }
    }
    floodDiagonals(sealFullRows, sealFullReach, sealPrepared);
    largestRegion(sealSolidRows, sealSolidReach);
    sealExtended = true;
  }
  // A pocket is small: a front line of barriers across the arena is not one.
  const SPAWN_POCKET_CELLS = 20;
  const sealOwnReach = new Int32Array(16);
  // Is a cell of the main region within reach of (x, z)?
  function inMainRegion(x, z) {
    const cell = navigationCell(x, z), cx = cell & 15, cz = cell >> 4;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const nx = cx + dx, nz = cz + dz;
      if (nx < 0 || nx > 15 || nz < 0 || nz > 15 || !(sealFullReach[nz] & (1 << nx))) continue;
      const ox = nx - 7.5 - x, oz = nz - 7.5 - z;
      if (ox * ox + oz * oz <= SEAL_CELL_REACH) return true;
    }
    return false;
  }
  function spawnVerdict(tank) {
    if (inMainRegion(tank.x, tank.z)) return SPAWN_ROUTABLE;
    extendSealGrids();
    const full = sealCell(sealFull, tank.x, tank.z);
    if (full >= 0 && (sealFullReach[full >> 4] & (1 << (full & 15)))) return SPAWN_ROUTABLE;
    const solid = sealCell(sealSolid, tank.x, tank.z);
    if (!(solid >= 0 && (sealSolidReach[solid >> 4] & (1 << (solid & 15))))) return SPAWN_SEALED;
    if (full >= 0) {
      floodRows(sealFullRows, full, sealOwnReach);
      if (rowCells(sealOwnReach) >= SPAWN_POCKET_CELLS) return SPAWN_ROUTABLE;
    }
    return SPAWN_BREAKABLE;
  }
  /* The coin for keeping a pocket: a hash of the random state (read, never
     advanced; replays repeat it), the tank, its cell and the wave. */
  function pocketKept(tank) {
    let hash = (randomState() ^ Math.imul(tank.id + 1, 0x9e3779b1)) >>> 0;
    hash = Math.imul(hash ^ (hash >>> 16), 0x85ebca6b)
      ^ Math.imul(navigationCell(tank.x, tank.z) + 1 + state.wave * 257, 0xc2b2ae35);
    hash = Math.imul(hash ^ (hash >>> 13), 0x27d4eb2f);
    return ((hash ^ (hash >>> 16)) >>> 0) < 0x80000000;
  }
  // To the nearest clear cell of the main region (foes away from the player).
  const sealCandidates = new Int16Array(NAV_CELLS), sealCandidateScores = new Float64Array(NAV_CELLS);
  function placeRoutable(tank) {
    const player = playerTank(), foe = tank.team !== player.team;
    extendSealGrids();
    for (let pass = 0; pass < 2; pass++) {
      const keepOut = foe ? (pass ? 5.2 : QUICK_SPAWN_MIN_DISTANCE) : 0;
      let count = 0;
      for (let cell = 0; cell < NAV_CELLS; cell++) {
        if (!(sealFullReach[cell >> 4] & (1 << (cell & 15)))) continue;
        const x = (cell & 15) - 7.5, z = (cell >> 4) - 7.5;
        const px = x - player.x, pz = z - player.z;
        if (px * px + pz * pz < keepOut * keepOut) continue;
        const dx = x - tank.x, dz = z - tank.z;
        sealCandidates[count] = cell; sealCandidateScores[count++] = dx * dx + dz * dz;
      }
      for (let tried = 0; tried < 32 && tried < count; tried++) {
        let best = tried;
        for (let at = tried + 1; at < count; at++)
          if (sealCandidateScores[at] < sealCandidateScores[best]) best = at;
        const cell = sealCandidates[best], score = sealCandidateScores[best];
        sealCandidates[best] = sealCandidates[tried]; sealCandidateScores[best] = sealCandidateScores[tried];
        sealCandidates[tried] = cell; sealCandidateScores[tried] = score;
        const x = (cell & 15) - 7.5, z = (cell >> 4) - 7.5;
        if (spawnBlocked(tank, x, z, tank.collisionRadius + .04)) continue;
        tank.x = x; tank.z = z; tank.slideX = tank.slideZ = 0;
        tank.surfaceY = surfaceHeightAt(x, z);
        return true;
      }
    }
    return false;
  }
  let spawnPocketsKept = 0, spawnPocketsMoved = 0, spawnSealedMoved = 0, spawnSealedStuck = 0;
  function validateSpawn(tank) {
    if (!tank.active || tank.player || tank.inert || !(tank.driveSpeed > 0)
        || state.gameMode === MODE_DUEL || online.active) return;
    if (sealPrepared !== navGridIndex(tank)) prepareSealGrids(navGridIndex(tank));
    const verdict = spawnVerdict(tank);
    if (verdict === SPAWN_ROUTABLE) return;
    if (verdict === SPAWN_BREAKABLE && pocketKept(tank)) { spawnPocketsKept++; return; }
    if (placeRoutable(tank)) {
      if (verdict === SPAWN_SEALED) spawnSealedMoved++; else spawnPocketsMoved++;
    } else if (verdict === SPAWN_SEALED) spawnSealedStuck++;
  }
  function validateRespawn(tank) {
    sealPrepared = -1; validateSpawn(tank); sealPrepared = -1;
  }
  /* An arena start only queues its bots; advanceSimulation checks one hull
     grid's worth a frame (about 3 ms each on the PSP), keeping the work out
     of the already long arena-change frame. */
  let spawnChecks = 0;
  function validateSpawns() {
    spawnChecks = 0;
    for (let at = 0; at < MAX_TANKS; at++) if (tanks[at].active) spawnChecks |= 1 << at;
  }
  function settleSpawnChecks(all) {
    while (spawnChecks) {
      const first = 31 - Math.clz32(spawnChecks & -spawnChecks), grid = navGridIndex(tanks[first]);
      sealPrepared = -1;
      for (let at = first; at < MAX_TANKS; at++) {
        if (!(spawnChecks & (1 << at)) || navGridIndex(tanks[at]) !== grid) continue;
        spawnChecks &= ~(1 << at);
        validateSpawn(tanks[at]);
      }
      sealPrepared = -1;
      if (!all) break;
    }
  }

  /* The breach: the cheapest way from the bot to the goal field's region,
     a step costing 1, or 1 + BREACH_COST into a destructible-blocked cell
     (Dial's buckets). Its first such cell's nearest blocker is shot from
     the cell before it. */
  const BREACH_COST = 6, BREACH_BUCKETS = BREACH_COST + 2, BREACH_POOL = 1024;
  const breachCosts = new Uint16Array(NAV_CELLS), breachParents = new Int16Array(NAV_CELLS);
  const breachHeads = new Int16Array(BREACH_BUCKETS);
  const breachPoolCells = new Int16Array(BREACH_POOL), breachPoolNext = new Int16Array(BREACH_POOL);
  let breachPool = 0, breachPending = 0;
  function breachPush(cell, cost) {
    if (breachPool >= BREACH_POOL) return;
    const bucket = cost % BREACH_BUCKETS;
    breachPoolCells[breachPool] = cell; breachPoolNext[breachPool] = breachHeads[bucket];
    breachHeads[bucket] = breachPool++; breachPending++;
  }
  function planBotBreach(tank, fieldBase, gridIndex) {
    const start = navigationCell(tank.x, tank.z);
    if (start !== start) return false;
    const masks = navOccupancies[gridIndex].blockers(), grid = navGrids[gridIndex];
    const solid = navSolidBits(), breakable = navBreakableBits();
    breachCosts.fill(65535); breachHeads.fill(-1); breachPool = breachPending = 0;
    breachCosts[start] = 0; breachParents[start] = -1; breachPush(start, 0);
    let found = -1;
    for (let cost = 0; breachPending > 0 && found < 0 && cost < 4096; cost++) {
      const bucket = cost % BREACH_BUCKETS;
      let node = breachHeads[bucket];
      breachHeads[bucket] = -1;
      while (node >= 0) {
        const cell = breachPoolCells[node];
        node = breachPoolNext[node]; breachPending--;
        if (found >= 0 || breachCosts[cell] !== cost) continue;
        if (navDistances[fieldBase + cell] < NAV_FIELD_BLOCKED) { found = cell; continue; }
        const x = cell & 15;
        for (let side = 0; side < 4; side++) {
          if (side === 0 ? x === 0 : side === 1 ? x === 15
              : side === 2 ? cell < NAV_SIDE : cell >= NAV_CELLS - NAV_SIDE) continue;
          const next = cell + (side === 0 ? -1 : side === 1 ? 1 : side === 2 ? -NAV_SIDE : NAV_SIDE);
          const mask = masks[next];
          if (mask & solid) continue;
          const reached = cost + 1 + ((mask & breakable) ? BREACH_COST : 0);
          if (reached >= breachCosts[next]) continue;
          breachCosts[next] = reached; breachParents[next] = cell;
          breachPush(next, reached);
        }
        // Diagonals between blocked cells, where the hull fits.
        const z = cell >> 4, blockedBits = solid | breakable;
        for (let side = 0; side < 4; side++) {
          const dx = side & 1 ? 1 : -1, dz = side < 2 ? -1 : 1;
          if (x + dx < 0 || x + dx > 15 || z + dz < 0 || z + dz > 15) continue;
          const next = cell + dz * NAV_SIDE + dx, mask = masks[next];
          if ((mask & solid) || !(masks[cell + dx] & blockedBits)
              || !(masks[cell + dz * NAV_SIDE] & blockedBits)) continue;
          const reached = cost + 1 + ((mask & breakable) ? BREACH_COST : 0);
          if (reached >= breachCosts[next]) continue;
          // Between open cells, the route search's cached decision.
          if (!grid[cell] && !grid[next] ? !(dz > 0 ? navDiagonal(gridIndex, cell, dx > 0)
              : navDiagonal(gridIndex, next, dx < 0))
            : lineCrossesWalls(x - 7.5, z - 7.5, x + dx - 7.5, z + dz - 7.5, false,
              NAV_DIAGONAL_PADDING[gridIndex])) continue;
          breachCosts[next] = reached; breachParents[next] = cell;
          breachPush(next, reached);
        }
      }
    }
    if (found < 0) return false;
    // The destructible cell nearest the start.
    let breach = -1, stand = -1;
    for (let cell = found, guard = 0; cell >= 0 && guard < NAV_CELLS; guard++) {
      const parent = breachParents[cell];
      if (masks[cell] & breakable) { breach = cell; stand = parent; }
      cell = parent;
    }
    if (breach < 0) return false;
    const bits = masks[breach] & breakable;
    let kind = 0, index = -1, best = Infinity;
    for (let at = 0; at < MAX_BARRIERS; at++) {
      if (!(bits & (4 << at))) continue;
      const barrier = barriers[at];
      const dx = Math.max(barrier.left - tank.x, 0, tank.x - barrier.right);
      const dz = Math.max(barrier.top - tank.z, 0, tank.z - barrier.bottom);
      if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; kind = 2; index = at; }
    }
    for (let at = 0; at < crates.length; at++) {
      if (!(bits & (256 << at))) continue;
      const crate = crates[at], dx = crate.x - tank.x, dz = crate.z - tank.z;
      // A crate needs one shell.
      if (dx * dx + dz * dz - 1 < best) { best = dx * dx + dz * dz - 1; kind = 1; index = at; }
    }
    if (index < 0) return false;
    tank.breachKind = kind; tank.breachIndex = index;
    tank.breachStandX = stand >= 0 && stand !== start ? (stand & 15) - 7.5 : tank.x;
    tank.breachStandZ = stand >= 0 && stand !== start ? (stand >> 4) - 7.5 : tank.z;
    tank.breachRevision = navRevision; tank.breachActive = true;
    // The shot is planned on a later update, not on top of this search.
    tank.breachShotNext = state.time + .05; tank.breachAngle = NaN;
    return true;
  }
  function breachBlockerActive(tank) {
    return tank.breachKind === 1 ? !!crates[tank.breachIndex]?.active
      : tank.breachKind === 2 ? !!barriers[tank.breachIndex]?.active : false;
  }
  // The blocker's centre and its point nearest the tank, a little inside.
  const breachPoint = new Float64Array(4);
  function breachBlockerPoints(tank) {
    let left, right, top, bottom;
    if (tank.breachKind === 1) {
      const crate = crates[tank.breachIndex];
      left = crate.x - CRATE_HALF; right = crate.x + CRATE_HALF;
      top = crate.z - CRATE_HALF; bottom = crate.z + CRATE_HALF;
    } else {
      const barrier = barriers[tank.breachIndex];
      left = barrier.left; right = barrier.right; top = barrier.top; bottom = barrier.bottom;
    }
    breachPoint[0] = (left + right) * .5; breachPoint[1] = (top + bottom) * .5;
    const insetX = Math.min(.08, (right - left) * .25), insetZ = Math.min(.08, (bottom - top) * .25);
    breachPoint[2] = Math.max(left + insetX, Math.min(right - insetX, tank.x));
    breachPoint[3] = Math.max(top + insetZ, Math.min(bottom - insetZ, tank.z));
  }

  // Straight at the blocker, else banked off an outer face; NaN if neither.
  function planBreachShot(tank) {
    breachBlockerPoints(tank);
    for (let at = 0; at < 2; at++) {
      const px = breachPoint[2 - at * 2], pz = breachPoint[3 - at * 2];
      const angle = Math.atan2(px - tank.x, pz - tank.z);
      if (breachShotHits(tank, angle)) return angle;
    }
    let shortest = Infinity, chosen = NaN;
    const cx = breachPoint[0], cz = breachPoint[1];
    for (let side = 0; side < 4; side++) {
      const vertical = side < 2, wall = side & 1 ? ARENA_EDGE : -ARENA_EDGE;
      const mx = vertical ? 2 * wall - cx : cx, mz = vertical ? cz : 2 * wall - cz;
      const dx = mx - tank.x, dz = mz - tank.z, length = dx * dx + dz * dz;
      if (length >= shortest) continue;
      const angle = Math.atan2(dx, dz);
      if (breachShotHits(tank, angle)) { shortest = length; chosen = angle; }
    }
    return chosen;
  }

  const navFieldGoals = new Array(MAX_TANKS).fill(-1);
  const navFieldRevisions = new Array(MAX_TANKS).fill(-1);
  let navFieldCursor = 0;
  const bankAim = new Float32Array(4);
  let navRevision = 0, navEpoch = 0, navBuildCursor = 0, navGateOpen = false;

  /* The navigation half of game.js's invalidateBotNavigation (which first
     refreshes the ray index and the aim geometry epoch): rebuild the
     occupancy grids from the first cell. */
  function restartBotNavigation() {
    navEpoch++;
    navBuildCursor = 0;
    navGateOpen = state.gateOpen;
  }

  function updateBotNavigation() {
    if (navGateOpen !== state.gateOpen) invalidateBotNavigation();
    if (navBuildCursor >= NAV_CELLS) return;
    const end = Math.min(NAV_CELLS, navBuildCursor + 32);
    for (let grid = 0; grid < 4; grid++)
      navOccupancies[grid].update(navGrids[grid], navBuildCursor, end, barriers, crates,
        state.gateOpen, navFieldTemplates, grid * NAV_CELLS);
    navBuildCursor = end;
    if (end === NAV_CELLS) { navDiagonals.fill(0); navRevision++; }
  }

  function navigationCell(x, z) {
    const cx = x + 8, cz = z + 8;
    if (cx !== cx) return NaN;
    return ((cz <= 0 ? 0 : cz >= 16 ? 15 : cz | 0) << 4)
      + (cx <= 0 ? 0 : cx >= 16 ? 15 : cx | 0);
  }

  // cell < 0 is the diagnostic full-field request. Gameplay only requests its
  // nine waypoint candidates; unknown disconnected cells force full exhaustion.
  /* Fields start from the grid's template and every grid blocks its
     boundary ring, so one unchecked read per neighbour tells open and
     unreached (tests/fixtures/treadline-planning.js keeps the old search). */
  function prepareBotRouteField(slot, cell) {
    const base = slot * NAV_CELLS;
    const gridIndex = Math.max(0, navFieldGoals[slot]) >> 8;
    let read = navQueueHeads[slot], count = navQueueCounts[slot];
    if (read >= count) return 0;
    const before = read;
    let required = 0;
    if (cell >= 0) {
      const cx = cell & 15, cz = cell >> 4;
      for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
        const x = cx + dx, z = cz + dz;
        if (x < 0 || x >= 16 || z < 0 || z >= 16) continue;
        const at = z * NAV_SIDE + x;
        if (navDistances[base + at] !== NAV_FIELD_BLOCKED) navRequiredCells[required++] = at;
      }
    }
    // A complete layer preserves the retained queue order and exact distances.
    for (let layers = 0; layers <= NAV_CELLS; layers++) {
      let missing = cell < 0;
      for (let at = 0; at < required; at++)
        if (navDistances[base + navRequiredCells[at]] === 65535) missing = true;
      if (!missing || read >= count || read >= NAV_CELLS) break;
      const layerEnd = count;
      while (read < layerEnd && read < NAV_CELLS) {
        const cell = navQueue[base + read++], at = base + cell;
        const distance = navDistances[at] + 1;
        // Preserve left/right/up/down order.
        const west = navDistances[at - 1];
        if (west === 65535) { navDistances[at - 1] = distance; navQueue[base + count++] = cell - 1; }
        const east = navDistances[at + 1];
        if (east === 65535) { navDistances[at + 1] = distance; navQueue[base + count++] = cell + 1; }
        const north = navDistances[at - NAV_SIDE];
        if (north === 65535) {
          navDistances[at - NAV_SIDE] = distance; navQueue[base + count++] = cell - NAV_SIDE;
        }
        const south = navDistances[at + NAV_SIDE];
        if (south === 65535) {
          navDistances[at + NAV_SIDE] = distance; navQueue[base + count++] = cell + NAV_SIDE;
        }
        // Then the diagonal links (decided on first use), only where both
        // cells between are blocked: elsewhere four-way steps reach it.
        const westBlocked = west === NAV_FIELD_BLOCKED, eastBlocked = east === NAV_FIELD_BLOCKED;
        if ((westBlocked || eastBlocked)
            && (north === NAV_FIELD_BLOCKED || south === NAV_FIELD_BLOCKED)) {
          const northBlocked = north === NAV_FIELD_BLOCKED;
          const southBlocked = south === NAV_FIELD_BLOCKED;
          for (let side = 0; side < 4; side++) {
            if (side === 0 ? !(eastBlocked && southBlocked) : side === 1 ? !(westBlocked && southBlocked)
                : side === 2 ? !(westBlocked && northBlocked) : !(eastBlocked && northBlocked)) continue;
            const next = side === 0 ? cell + NAV_SIDE + 1 : side === 1 ? cell + NAV_SIDE - 1
              : side === 2 ? cell - NAV_SIDE - 1 : cell - NAV_SIDE + 1;
            if (navDistances[base + next] !== 65535
                || !(side === 0 ? navDiagonal(gridIndex, cell, true)
                  : side === 1 ? navDiagonal(gridIndex, cell, false)
                  : side === 2 ? navDiagonal(gridIndex, next, true)
                  : navDiagonal(gridIndex, next, false))) continue;
            navDistances[base + next] = distance;
            navQueue[base + count++] = next;
          }
        }
      }
    }
    navQueueHeads[slot] = read; navQueueCounts[slot] = count;
    return read - before;
  }

  /* The waypoint: the best-scoring neighbour in sight. Candidates are
     ranked first and sight-tested best first, the scan's answer with fewer
     tests (the planning fixture keeps the scan). */
  const navCandidateCells = new Int16Array(9), navCandidateScores = new Float64Array(9);
  function chooseRouteWaypoint(tank, cell, grid, base, padding) {
    let candidates = 0, chosen = -1, nearest = -1, nearestDistance = 65535;
    const cx = cell & 15, cz = cell >> 4;
    for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
      const x = cx + dx, z = cz + dz;
      if (x < 0 || x >= 16 || z < 0 || z >= 16) continue;
      const at = z * NAV_SIDE + x;
      if (grid[at] || navDistances[base + at] === 65535) continue;
      const px = x - 7.5 - tank.x, pz = z - 7.5 - tank.z;
      const score = navDistances[base + at] + (px * px + pz * pz) * .2;
      if (navDistances[base + at] < nearestDistance) {
        nearestDistance = navDistances[base + at]; nearest = at;
      }
      if (!(score < Infinity)) continue;
      let slot = candidates++;
      while (slot > 0 && navCandidateScores[slot - 1] > score) {
        navCandidateScores[slot] = navCandidateScores[slot - 1];
        navCandidateCells[slot] = navCandidateCells[slot - 1];
        slot--;
      }
      navCandidateScores[slot] = score; navCandidateCells[slot] = at;
    }
    for (let at = 0; at < candidates; at++) {
      const candidate = navCandidateCells[at];
      if (!lineCrossesWalls(tank.x, tank.z, (candidate & 15) - 7.5, (candidate >> 4) - 7.5,
          false, padding, tank.id * 12 + 11)) { chosen = candidate; break; }
    }
    /* The cell nearest the goal can be a gap the hull fits with a little to
       spare, seen through the full padding only from its centre line. A
       hull a little off that line steered to some other cell (often its
       own centre, which it then circled) and never lined up. Take the gap
       when it is in sight with half the hull radius of padding; the hull
       slides along a face it grazes on the way through. */
    if (nearest >= 0 && nearest !== chosen && nearest !== cell
        && (chosen < 0 || nearestDistance < navDistances[base + chosen])
        && !lineCrossesWalls(tank.x, tank.z, (nearest & 15) - 7.5, (nearest >> 4) - 7.5,
          false, tank.collisionRadius * .5, tank.id * 12 + 11)) chosen = nearest;
    return chosen;
  }
  /* Whether steerBotRoute's last failure found the goal disconnected: its
     route field (routeFieldSlot) is complete and never reaches the tank. */
  let routeDisconnected = false, routeFieldSlot = -1;
  function steerBotRoute(tank, goalX, goalZ) {
    routeDisconnected = false;
    if (navBuildCursor < NAV_CELLS) return false;
    const routeAt = qualificationAIActive ? performance.now() : 0;
    const gridIndex = navGridIndex(tank);
    const grid = navGrids[gridIndex], keyOffset = gridIndex * NAV_CELLS;
    let goal = navigationCell(goalX, goalZ);
    if (grid[goal]) {
      // Retain remapped cells while their goal/grid is stable.
      const raw = goal + keyOffset;
      if (tank.navRawGoal === raw && tank.navMapRevision === navRevision) {
        goal = tank.navMappedGoal;
      } else {
        const nearest = arenaSpatial.nearestOpenCell(grid, goalX, goalZ);
        tank.navRawGoal = raw; tank.navMapRevision = navRevision;
        tank.navMappedGoal = goal = nearest;
      }
      if (goal < 0) {
        if (qualificationAIActive) qualificationAITimes[15] += performance.now() - routeAt;
        return false;
      }
    }
    const goalCell = goal;
    goal += keyOffset;
    const fieldAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[15] += fieldAt - routeAt;
    let slot = tank.navFieldSlot;
    if (slot < 0 || navFieldGoals[slot] !== goal
        || navFieldRevisions[slot] !== navRevision) {
      slot = -1;
      for (let at = 0; at < MAX_TANKS; at++) {
        if (navFieldGoals[at] === goal && navFieldRevisions[at] === navRevision) {
          slot = at;
          if (qualificationAIActive) qualificationAICounts[6]++;
          break;
        }
      }
      tank.navFieldSlot = slot;
    }
    const buildField = slot < 0;
    if (buildField) {
      slot = navFieldCursor;
      navFieldCursor = (navFieldCursor + 1) % MAX_TANKS;
      tank.navFieldSlot = slot;
    }
    const base = slot * NAV_CELLS;
    if (buildField) {
      const searchAt = qualificationAIActive ? performance.now() : 0;
      navDistances.set(navFieldTemplateViews[gridIndex], base);
      navDistances[base + goalCell] = 0;
      navQueue[base] = goalCell;
      navQueueHeads[slot] = 0; navQueueCounts[slot] = 1;
      navFieldGoals[slot] = goal; navFieldRevisions[slot] = navRevision;
      if (qualificationAIActive) {
        qualificationAITimes[6] += performance.now() - searchAt;
        qualificationAICounts[1]++;
      }
    }
    let waypointAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[16] += waypointAt - fieldAt;
    // Field eviction alone must not reset a waypoint.
    if (tank.navGoal !== goal || tank.navRevision !== navRevision) {
      tank.navGoal = goal; tank.navRevision = navRevision;
      tank.navWaypoint = -1;
    }
    const cell = navigationCell(tank.x, tank.z);
    const padding = routePadding(tank);
    /* Commit to the waypoint: keep it while the hull is in the cell it was
       chosen from, or has drifted into a neighbour of it (a hull turning
       towards a gap overshoots its centre line), as long as it can still
       see it, with half its radius of padding after drifting. Re-choosing
       from the neighbour lost sight of the gap through the full padding
       and turned back, then forward again, at every gap mouth. */
    const waypoint = tank.navWaypoint;
    const drifted = waypoint >= 0 && tank.navWaypointFrom !== cell && waypoint !== cell
      && ((waypoint & 15) - (cell & 15) + 1) >>> 0 <= 2
      && ((waypoint >> 4) - (cell >> 4) + 1) >>> 0 <= 2;
    if (waypoint >= 0 && (tank.navWaypointFrom === cell || drifted)) {
      const wx = (waypoint & 15) - 7.5;
      const wz = (waypoint >> 4) - 7.5;
      // Recheck the floating-origin segment against current scenery.
      if (!lineCrossesWalls(tank.x, tank.z, wx, wz, false,
          drifted && padding > tank.collisionRadius * .5 ? tank.collisionRadius * .5 : padding,
          tank.id * 12 + 10)) {
        bankAim[0] = wx - tank.x; bankAim[1] = wz - tank.z;
        if (qualificationAIActive) qualificationAITimes[17] += performance.now() - waypointAt;
        return true;
      }
    }
    const searchAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[17] += searchAt - waypointAt;
    const expanded = prepareBotRouteField(slot, cell);
    if (qualificationAIActive) {
      const searchedAt = performance.now();
      qualificationAITimes[6] += searchedAt - searchAt;
      qualificationAITimes[16] += searchedAt - searchAt;
      qualificationAICounts[2] += expanded;
      waypointAt = searchedAt;
    }
    const chosen = chooseRouteWaypoint(tank, cell, grid, base, padding);
    if (qualificationAIActive) qualificationAITimes[17] += performance.now() - waypointAt;
    if (chosen < 0) {
      if (navQueueHeads[slot] >= navQueueCounts[slot]) {
        let reached = false;
        const cx = cell & 15, cz = cell >> 4;
        for (let dz = -1; dz <= 1 && !reached; dz++) for (let dx = -1; dx <= 1; dx++) {
          const x = cx + dx, z = cz + dz;
          if (x >= 0 && x < 16 && z >= 0 && z < 16
              && navDistances[base + z * NAV_SIDE + x] < NAV_FIELD_BLOCKED) { reached = true; break; }
        }
        if (!reached) { routeDisconnected = true; routeFieldSlot = slot; }
      }
      return false;
    }
    tank.navWaypoint = chosen; tank.navWaypointFrom = cell;
    bankAim[0] = (chosen & 15) - 7.5 - tank.x;
    bankAim[1] = (chosen >> 4) - 7.5 - tank.z;
    return true;
  }

  function planBankShot(tank, targetX, targetZ) {
    let shortest = Infinity, found = false;
    const overCover = elevatedFiringOrigin(tank);
    /* Four outer faces: bounded planning, no speculative wall traversal.
       A candidate is admitted only when both reflected legs are unobstructed. */
    for (let side = 0; side < 4; side++) {
      const vertical = side < 2, wall = side & 1 ? ARENA_EDGE : -ARENA_EDGE;
      const mirrorX = vertical ? 2 * wall - targetX : targetX;
      const mirrorZ = vertical ? targetZ : 2 * wall - targetZ;
      const dx = mirrorX - tank.x, dz = mirrorZ - tank.z;
      const divisor = vertical ? dx : dz;
      if (Math.abs(divisor) < .001) continue;
      const along = (wall - (vertical ? tank.x : tank.z)) / divisor;
      if (along <= .02 || along >= .98) continue;
      const hitX = tank.x + dx * along, hitZ = tank.z + dz * along;
      if (Math.abs(vertical ? hitZ : hitX) > HULL_EDGE) continue;
      const lengthSquared = dx * dx + dz * dz;
      // A longer reflected ray cannot replace an admitted shorter one. Rank
      // before wall/smoke tests, preserving the same winner and tie order.
      if (lengthSquared >= shortest || lengthSquared > 20 * 20) continue;
      if (lineCrossesWalls(tank.x, tank.z, hitX, hitZ, overCover, .08,
          tank.id * 12 + 2 + side * 2)
          || lineCrossesWalls(hitX, hitZ, targetX, targetZ, overCover, .08,
          tank.id * 12 + 3 + side * 2)
          || lineCrossesSmoke(tank.x, tank.z, hitX, hitZ)
          || lineCrossesSmoke(hitX, hitZ, targetX, targetZ)) continue;
      shortest = lengthSquared; bankAim[0] = dx; bankAim[1] = dz;
      bankAim[2] = hitX; bankAim[3] = hitZ;
      found = true;
    }
    return found;
  }

  /* The difficulty a bot plays at: its own (campaign and league bots carry
     one), else Daily's fixed one, else the player's choice. */
  function botDifficulty(tank) {
    return tank && tank.difficulty >= 0 ? tank.difficulty
      : state.gameMode === MODE_DAILY ? DAILY_DIFFICULTY : state.difficultyChoice;
  }
  function botDifficultyValue(values, tank = null) {
    const difficulty = botDifficulty(tank);
    if (!isOnslaught()) return values[difficulty];
    const table = values === BOT_REACTION ? 0 : values === BOT_ACCURACY ? 1
      : values === BOT_FIRE_CHANCE ? 2 : values === BOT_FIRE_WINDUP ? 3 : -1;
    if (table < 0) throw new Error("Unknown bot parameter table");
    if (botWaveCached !== state.wave) {
      const increment = Math.max(0, state.wave - 1) * .16;
      for (let at = 0; at < 3; at++) {
        const level = Math.min(2, at + increment), lower = Math.floor(level);
        const upper = Math.min(2, lower + 1), blend = level - lower;
        for (let type = 0; type < 4; type++) {
          const v = botDifficultyTables[type];
          botWaveValues[at * 4 + type] = v[lower] + (v[upper]-v[lower])*blend;
        }
      }
      botWaveCached = state.wave;
    }
    return botWaveValues[difficulty * 4 + table];
  }

  function beginBotFireWindup(tank, secondary) {
    if (tank.fireWindup > 0) return false;
    tank.fireWindup = botDifficultyValue(BOT_FIRE_WINDUP, tank)
      + (tank.id % 3) * .025;
    tank.fireSecondaryArmed = !!secondary;
    tank.fireTelegraphed = false;
    const stats = qualificationStats();
    if (stats !== null) stats.botTelegraphs++;
    sounds.telegraph();
    return true;
  }

  // Separate strategy/bank queues and parity-cohort cursors spread ordinary
  // planning without skipping command, movement, or fresh-shot checks.
  // Small fights need no queue. Mandatory target/shot work may exceed the
  // two-job ordinary budget; pending opposite kinds cannot starve each other.
  const botPlanPending = [0, 0], botPlanCursor = [0, 1, 2, 3];
  let botPlanFrame = -1, botPlanUsed = 0, botPlanTaken = 0, botPlanBots = 0, botPlanKinds = 0;
  function botPlanAdmit(tank, kind) {
    if (botPlanFrame !== state.frames) {
      botPlanFrame = state.frames; botPlanUsed = botPlanTaken = 0;
      // Count the long soak's player too: the planner drives it.
      const first = playerIsBot() || qualificationLongSoak ? 0
        : online.active && online.role === "host" ? 2 : 1;
      botPlanBots = 0;
      for (let at = 0; at < MAX_TANKS; at++) {
        if (at >= first && tanks[at].active) botPlanBots++;
        else { botPlanPending[0] &= ~(1 << at); botPlanPending[1] &= ~(1 << at); }
      }
    }
    if (botPlanBots < 3) return true;
    const bit = 1 << tank.id, cursor = kind * 2 + (state.frames & 1);
    botPlanPending[kind] |= bit;
    if ((botPlanPending[1 - kind] & bit) && !!(botPlanKinds & bit) === !!kind) return false;
    if ((botPlanUsed & (1 << kind)) || (botPlanTaken & bit)) return false;
    for (let at = 0; at < MAX_TANKS; at += 2) {
      const id = (botPlanCursor[cursor] + at) % MAX_TANKS, next = 1 << id;
      if (!tanks[id].active) { botPlanPending[kind] &= ~next; continue; }
      if (!(botPlanPending[kind] & next) || ((state.frames + id) & 1)
          || (botPlanTaken & next)) continue;
      if ((botPlanPending[1 - kind] & next) && !!(botPlanKinds & next) === !!kind) continue;
      if (id !== tank.id) return false;
      botPlanPending[kind] &= ~bit; botPlanCursor[cursor] = (id + 2) % MAX_TANKS;
      botPlanUsed |= 1 << kind; botPlanTaken |= bit;
      if (kind) botPlanKinds |= bit; else botPlanKinds &= ~bit;
      return true;
    }
    return false;
  }

  /* No way to the goal: go for the target if it is reachable (a held goal
     is given up: hold, 3), else breach (2) or hold when none (3). 1 means
     bankAim holds the steering. */
  function considerBreach(tank, goalX, goalZ, targetX, targetZ, holding) {
    let slot = routeFieldSlot;
    const offX = goalX - targetX, offZ = goalZ - targetZ;
    if (holding || offX * offX + offZ * offZ > 1) {
      if (!lineCrossesWalls(tank.x, tank.z, targetX, targetZ, false, tank.collisionRadius + .02)) {
        bankAim[0] = targetX - tank.x; bankAim[1] = targetZ - tank.z;
        return holding ? 3 : 1;
      }
      if (steerBotRoute(tank, targetX, targetZ)) return holding ? 3 : 1;
      if (!routeDisconnected) return holding ? 3 : 0;
      slot = routeFieldSlot;
    }
    if (state.time < tank.breachRetry) return 3;
    tank.breachRetry = state.time + .5;
    // Plan on the bot's next update, apart from this frame's route searches.
    tank.breachQueued = true; tank.breachQueuedSlot = slot;
    tank.breachQueuedKey = navFieldGoals[slot]; tank.breachQueuedRevision = navRevision;
    return 3;
  }
  function runQueuedBreach(tank) {
    tank.breachQueued = false;
    const slot = tank.breachQueuedSlot;
    if (tank.breachQueuedRevision !== navRevision || navFieldRevisions[slot] !== navRevision
        || navFieldGoals[slot] !== tank.breachQueuedKey
        || !planBotBreach(tank, slot * NAV_CELLS, navGridIndex(tank))) return false;
    tank.breaches++;
    return true;
  }
  // Clear of a chain crate's blast, hold and shoot, or go to the breach.
  function updateBreachSteering(tank) {
    if (state.time >= tank.breachShotNext) {
      tank.breachAngle = planBreachShot(tank);
      tank.breachShotNext = state.time + .5;
    }
    breachBlockerPoints(tank);
    const cx = tank.breachX = breachPoint[0], cz = tank.breachZ = breachPoint[1];
    tank.breachHold = false;
    tank.strategySteerX = tank.x; tank.strategySteerZ = tank.z;
    if (tank.breachKind === 1 && crates[tank.breachIndex].type === 1) {
      const dx = tank.x - cx, dz = tank.z - cz, distance = Math.sqrt(dx * dx + dz * dz);
      if (distance < 1.9 && distance > .01) {
        const x = cx + dx / distance * 2.05, z = cz + dz / distance * 2.05;
        if (Math.abs(x) < 7.4 && Math.abs(z) < 7.4
            && !circleHitsObstacle(x, z, tank.collisionRadius)
            && !lineCrossesWalls(tank.x, tank.z, x, z, false, tank.collisionRadius * .5)) {
          tank.strategySteerX = x; tank.strategySteerZ = z;
          return;
        }
      }
    }
    if (tank.breachAngle === tank.breachAngle) { tank.breachHold = true; return; }
    const sx = tank.breachStandX, sz = tank.breachStandZ;
    const fromX = sx - tank.x, fromZ = sz - tank.z;
    if (fromX * fromX + fromZ * fromZ < .09) {
      tank.breachHold = true; tank.breachShotNext = Math.min(tank.breachShotNext, state.time + .2);
      return;
    }
    if (lineCrossesWalls(tank.x, tank.z, sx, sz, false, tank.collisionRadius + .02)
        && steerBotRoute(tank, sx, sz)) {
      tank.strategySteerX = tank.x + bankAim[0]; tank.strategySteerZ = tank.z + bankAim[1];
    } else { tank.strategySteerX = sx; tank.strategySteerZ = sz; }
  }

  function updateBotCommand(tank, dt) {
    let phaseAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAICounts[0]++;
    tank.guardIdle += dt;
    const command = tank.command;
    let targetX = tanks[0].x, targetZ = tanks[0].z, targetId = 0;
    let targetDistance = Infinity, targetRank = Infinity;
    const retainedTarget = tanks[tank.targetChoice];
    const selectTarget = !retainedTarget || !retainedTarget.active
      || retainedTarget.team === tank.team || state.time >= tank.targetNext
      || tank.targetEpoch !== navEpoch;
    if (!selectTarget) {
      targetId = retainedTarget.id; targetX = retainedTarget.x; targetZ = retainedTarget.z;
      const dx = targetX - tank.x, dz = targetZ - tank.z;
      targetDistance = dx * dx + dz * dz; targetRank = 0;
    } else {
      if (qualificationAIActive) qualificationAICounts[5]++;
      /* Survival combat normally has exactly one opponent. Visibility can
         change its firing solution, but cannot change the target winner.
         Keep full threat ranking for team/online matches with several foes. */
      let soleOpponent = null, opponents = 0;
      for (let at = 0; at < MAX_TANKS; at++) {
        const candidate = tanks[at];
        if (!candidate.active || candidate.team === tank.team) continue;
        soleOpponent = candidate;
        if (++opponents > 1) break;
      }
      if (opponents === 1) {
        targetId = soleOpponent.id; targetX = soleOpponent.x; targetZ = soleOpponent.z;
        const dx = targetX - tank.x, dz = targetZ - tank.z;
        targetDistance = dx * dx + dz * dz; targetRank = 0;
      }
      for (let at = 0; opponents > 1 && at < MAX_TANKS; at++) {
        const candidate = tanks[at];
        if (!candidate.active || candidate.team === tank.team) continue;
        const dx = candidate.x - tank.x, dz = candidate.z - tank.z;
        const distance = dx * dx + dz * dz;
        const visible = !lineCrossesWalls(tank.x, tank.z,
          candidate.x, candidate.z, elevatedFiringOrigin(tank));
        const towardBotX = tank.x - candidate.x, towardBotZ = tank.z - candidate.z;
        const threatening = towardBotX * candidate.turretSine
          + towardBotZ * candidate.turretCosine > 0;
        const rank = distance * (.8 + candidate.health / candidate.maxHealth * .35)
          * (visible ? .85 : 1.2) * (threatening ? .9 : 1)
          * (candidate.id === tank.target ? .72 : 1);
        if (rank < targetRank) {
          targetRank = rank;
          targetDistance = distance; targetX = candidate.x; targetZ = candidate.z;
          targetId = candidate.id;
        }
      }
      if (targetRank !== Infinity) {
        tank.targetChoice = targetId; tank.targetEpoch = navEpoch;
        tank.targetNext = state.time + botDifficultyValue(BOT_REACTION, tank);
      }
    }
    if (targetRank === Infinity) {
      botPlanPending[0] &= ~(1 << tank.id); botPlanPending[1] &= ~(1 << tank.id);
      command.left = command.right = 0;
      command.fire = command.secondary = command.gadget = command.ultimate = false;
      tank.fireWindup = 0; tank.target = -1;
      return;
    }
    tank.target = targetId;
    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[0] += now - phaseAt;
      phaseAt = now;
    }

    let goalX = targetX, goalZ = targetZ, convoyTarget = false;
    const strategyMood = tank.health < 32 ? 2
      : tank.health / tank.maxHealth < .45 ? 1 : 0;
    const strategyUrgent = tank.strategyTarget !== targetId
      || tank.strategyMood !== strategyMood;
    const strategyRefresh = strategyUrgent ||
      ((state.time >= tank.strategyNext || tank.strategyEpoch !== navEpoch)
        && botPlanAdmit(tank, 0));
    if (strategyRefresh) {
      if (qualificationAIActive) qualificationAITimes[18]++;
      if (tank.breachActive && (tank.breachRevision !== navRevision
          || !breachBlockerActive(tank))) tank.breachActive = tank.breachHold = false;
      const guardHunts = tank.role === "GUARD" && guardEscalated(tank);
      // Held goals (cover, pickups, control points) are not breached for.
      let holding = false;
      if (tank.health < 32 && !guardHunts) {
        holding = true;
        let bestPickup = null, bestDistance = Infinity;
        for (let at = 0; at < MAX_PICKUPS; at++) {
          const pickup = pickups[at];
          if (!pickup.active || pickup.type !== "ARMOR") continue;
          const px = pickup.x - tank.x, pz = pickup.z - tank.z;
          const distance = px * px + pz * pz;
          if (distance < bestDistance) {
            bestDistance = distance; bestPickup = pickup;
          }
        }
        if (bestPickup) { goalX = bestPickup.x; goalZ = bestPickup.z; }
        else {
          goalX = tank.team === 0 ? -5.8 : 5.8;
          goalZ = tank.team === 0 ? -5.8 : 5.8;
        }
      } else if (state.gameMode === MODE_CONTROL) {
        if (tank.role === "CAPTURE"
            || (tank.role === "GUARD" && (tank.team === 0
              ? state.blueControl >= state.redControl : state.redControl >= state.blueControl))) {
          holding = true;
          const angle = tank.id * 2.1;
          goalX = Math.sin(angle) * .7; goalZ = Math.cos(angle) * .7;
        } else if (tank.role === "FLANK") {
          const side = tank.id & 1 ? 1 : -1;
          goalX = targetX + (targetZ - tank.z) * .34 * side;
          goalZ = targetZ - (targetX - tank.x) * .34 * side;
        }
      } else if (state.gameMode === MODE_CONVOY && tank.team === 1 && convoy.active) {
        const dx = convoy.x - tank.x, dz = convoy.z - tank.z;
        const distance = dx * dx + dz * dz;
        if (tank.role === "AMBUSH") {
          goalX = convoy.x + (tank.id & 1 ? 2.1 : -2.1);
          goalZ = Math.min(6.5, convoy.z + 2.2);
        } else if (tank.role === "GUARD") {
          goalX = convoy.x; goalZ = Math.min(6.3, convoy.z + 1.4);
        } else {
          const side = tank.id & 1 ? 1 : -1;
          goalX = tanks[0].x + (tanks[0].z - tank.z) * .3 * side;
          goalZ = tanks[0].z - (tanks[0].x - tank.x) * .3 * side;
        }
        if (distance < targetDistance * 1.5 || tank.role !== "FLANK") {
          targetX = convoy.x; targetZ = convoy.z; targetDistance = distance;
          convoyTarget = true; tank.target = -1;
        }
      } else if (tank.role === "FLANK" || guardHunts) {
        // A flanker, or an escalated guard leaving cover, works around the target.
        const side = tank.id & 1 ? 1 : -1;
        goalX = targetX + (targetZ - tank.z) * .38 * side;
        goalZ = targetZ - (targetX - tank.x) * .38 * side;
      } else if (tank.role === "GUARD" && state.gateOpen) {
        const gate = ARENAS[state.arena].gates[0];
        goalX = gate[0] + (tank.id & 1 ? 1.5 : -1.5);
        goalZ = gate[1] + (tank.team ? 1.3 : -1.3);
      }
      if (!guardHunts
          && (tank.health / tank.maxHealth < .45 || tank.role === "GUARD")) {
        let bestCover = Infinity;
        for (let at = 0; at < MAX_BARRIERS; at++) {
          const barrier = barriers[at];
          if (!barrier.active) continue;
          const dx = targetX - barrier.x, dz = targetZ - barrier.z;
          const inverse = 1 / (Math.sqrt(dx * dx + dz * dz) || 1);
          const awayX = dx * inverse, awayZ = dz * inverse;
          const offset = Math.max(barrier.width, barrier.depth) * .5 + .8;
          const peek = (((state.time / 2) | 0) + tank.id) & 1 ? .8 * tank.orbitTurn : 0;
          const x = barrier.x - awayX * offset + awayZ * peek;
          const z = barrier.z - awayZ * offset - awayX * peek;
          const px = x - tank.x, pz = z - tank.z, distance = px * px + pz * pz;
          if (distance >= bestCover || distance >= 25
              || Math.abs(x) > 7 || Math.abs(z) > 7
              || circleHitsObstacle(x, z, tank.collisionRadius + .03)) continue;
          bestCover = distance; goalX = x; goalZ = z; holding = true;
        }
      }
      tank.strategyGoalX = goalX; tank.strategyGoalZ = goalZ;
      tank.strategySteerX = goalX; tank.strategySteerZ = goalZ;
      tank.strategyHolding = holding;
      tank.strategyConvoy = convoyTarget;
      tank.strategyTarget = targetId; tank.strategyEpoch = navEpoch;
      tank.strategyMood = strategyMood;
      tank.strategyNext = state.time + .25;
      botPlanPending[0] &= ~(1 << tank.id);
    } else {
      goalX = tank.strategyGoalX; goalZ = tank.strategyGoalZ;
      convoyTarget = tank.strategyConvoy && convoy.active;
      if (convoyTarget) {
        targetX = convoy.x; targetZ = convoy.z;
        const dx = targetX - tank.x, dz = targetZ - tank.z;
        targetDistance = dx * dx + dz * dz; tank.target = -1;
      }
    }

    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[1] += now - phaseAt;
      phaseAt = now;
    }
    const goalDx = goalX - tank.x, goalDz = goalZ - tank.z;
    const targetDx = targetX - tank.x, targetDz = targetZ - tank.z;
    const goalDistanceSquared = goalDx * goalDx + goalDz * goalDz;
    const targetDistanceSquared = targetDx * targetDx + targetDz * targetDz;
    // Hysteresis plus tangential steering prevents reverse/pursue oscillation.
    if (tank.backingOff) {
      if (targetDistanceSquared >= BOT_BACKOFF_EXIT_SQUARED) {
        tank.backingOff = false;
        tank.standoff = true;
      }
    } else if (targetDistanceSquared < BOT_BACKOFF_ENTER_SQUARED) {
      tank.backingOff = true;
      tank.standoff = true;
    }
    if (tank.standoff
        && targetDistanceSquared > BOT_STANDOFF_RELEASE_SQUARED)
      tank.standoff = false;
    let steeringX = goalDx, steeringZ = goalDz;
    if (!tank.backingOff && !tank.standoff) {
      if (strategyRefresh) {
        tank.orbitRoute = false;
        tank.routeSteering = false;
        if (!tank.breachActive && lineCrossesWalls(tank.x, tank.z, goalX, goalZ, false,
              tank.collisionRadius + .02)) {
          let routed = steerBotRoute(tank, goalX, goalZ);
          if (!routed && routeDisconnected) {
            const choice = considerBreach(tank, goalX, goalZ, targetX, targetZ,
              tank.strategyHolding);
            if (choice === 3) steeringX = steeringZ = 0;
            routed = choice === 1;
          }
          if (routed) {
            steeringX = bankAim[0]; steeringZ = bankAim[1];
            tank.routeSteering = true;
          }
        }
        tank.strategySteerX = tank.x + steeringX;
        tank.strategySteerZ = tank.z + steeringZ;
      } else {
        steeringX = tank.strategySteerX - tank.x;
        steeringZ = tank.strategySteerZ - tank.z;
      }
    }
    if (tank.backingOff) {
      steeringX = targetDx;
      steeringZ = targetDz;
    } else if (tank.standoff && targetDistanceSquared > .01) {
      const targetDistance = Math.sqrt(targetDistanceSquared);
      const towardX = targetDx / targetDistance;
      const towardZ = targetDz / targetDistance;
      const radial = Math.max(-.62, Math.min(.62,
        (targetDistance - tank.preferredRange) * .72));
      steeringX = targetDz / targetDistance * tank.orbitTurn
        + towardX * radial;
      steeringZ = -targetDx / targetDistance * tank.orbitTurn
        + towardZ * radial;
      if (strategyRefresh) {
        // Decide at the strategy cadence, never flip-flop every tread step.
        // Check the whole padded orbit prefix, not only its end point.
        tank.orbitRoute = false;
        if (lineCrossesWalls(tank.x, tank.z,
              tank.x + steeringX * .9, tank.z + steeringZ * .9,
              false, tank.collisionRadius + .02)) {
          const oppositeX = -targetDz / targetDistance * tank.orbitTurn
            + towardX * radial;
          const oppositeZ = targetDx / targetDistance * tank.orbitTurn
            + towardZ * radial;
          if (!lineCrossesWalls(tank.x, tank.z,
                tank.x + oppositeX * .9, tank.z + oppositeZ * .9,
                false, tank.collisionRadius + .02)) {
            tank.orbitTurn = -tank.orbitTurn;
            steeringX = oppositeX; steeringZ = oppositeZ;
          } else if (!tank.breachActive && (steerBotRoute(tank, goalX, goalZ)
              || (routeDisconnected
                && considerBreach(tank, goalX, goalZ, targetX, targetZ,
                  tank.strategyHolding) === 1))) {
            steeringX = bankAim[0]; steeringZ = bankAim[1];
            tank.orbitRoute = true;
            tank.orbitRouteX = tank.x + steeringX;
            tank.orbitRouteZ = tank.z + steeringZ;
          }
        }
      } else if (tank.orbitRoute) {
        steeringX = tank.orbitRouteX - tank.x;
        steeringZ = tank.orbitRouteZ - tank.z;
      }
    }
    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[2] += now - phaseAt;
      phaseAt = now;
    }
    const breachPlanned = tank.breachQueued && !strategyRefresh && runQueuedBreach(tank);
    if (tank.breachActive) {
      if (strategyRefresh || breachPlanned) updateBreachSteering(tank);
      tank.backingOff = tank.standoff = false;
      steeringX = tank.strategySteerX - tank.x; steeringZ = tank.strategySteerZ - tank.z;
    }
    const strategyBlocked = tank.strategyEpoch !== navEpoch
      && lineCrossesWalls(tank.x, tank.z, tank.x + steeringX * .9,
        tank.z + steeringZ * .9, false, tank.collisionRadius + .02);
    const steerHold = !tank.backingOff && steeringX * steeringX + steeringZ * steeringZ < 1e-6;
    const desired = Math.atan2(steeringX, steeringZ);
    const turn = wrapAngle(desired - tank.yaw);
    command.left = turn > .22 ? 0 : 1;
    command.right = turn < -.22 ? 0 : 1;
    // Veteran/Ace pivot in place for a goal behind them.
    if ((turn > 1.9 || turn < -1.9) && botDifficulty(tank))
      command.right = -(command.left = turn > 0 ? -1 : 1);
    /* Any bot lines up on a near route waypoint in place rather than
       arcing at it: a one-tread arc swings a fast hull past a gap's centre
       line and into its corner. Routing only (not open-floor chasing). */
    else if ((turn > .5 || turn < -.5) && tank.routeSteering && !tank.backingOff
        && !tank.standoff && steeringX * steeringX + steeringZ * steeringZ < 2.25)
      command.right = -(command.left = turn > 0 ? -1 : 1);
    command.reverse = tank.backingOff;
    if (steerHold || (!tank.backingOff && !tank.standoff && goalDistanceSquared < .5)) {
      /* Hold cover/objectives without the old reverse/pursue oscillation.
         Differential counter-rotation presents front armor to the threat. */
      const armorTurn = wrapAngle(Math.atan2(targetDx, targetDz) - tank.yaw);
      command.reverse = false;
      command.left = Math.abs(armorTurn) < .12 ? 0 : armorTurn > 0 ? -.5 : .5;
      command.right = -command.left;
    }
    if (tank.avoidTime > 0) {
      if (tank.avoidTime > 1) {
        command.reverse = true;
        command.left = command.right = 1;
      } else {
        command.reverse = false;
        command.left = tank.avoidTurn > 0 ? 0 : 1;
        command.right = command.left ? 0 : 1;
      }
    }
    if (command.reverse && command.left !== command.right) {
      /* Reversing both tread signs also reverses steering. Swap the authored
         tread command so the hull still converges on its desired heading
         instead of orbiting the object it is trying to escape. */
      const left = command.left;
      command.left = command.right;
      command.right = left;
    }
    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[3] += now - phaseAt;
      phaseAt = now;
    }
    if (tank.breachActive && tank.breachHold) {
      const face = wrapAngle(Math.atan2(tank.breachX - tank.x, tank.breachZ - tank.z) - tank.yaw);
      command.reverse = false;
      command.left = Math.abs(face) < .12 ? 0 : face > 0 ? -.5 : .5;
      command.right = -command.left;
    }
    if (strategyBlocked) command.left = command.right = 0;
    const accuracy = botDifficultyValue(BOT_ACCURACY, tank);
    const difficulty = botDifficulty(tank);
    const target = tanks[targetId];
    const lead = Math.min(.85, Math.sqrt(targetDistance) / 7.2)
      * (difficulty === 0 ? .2 : difficulty === 1 ? .65 : .95);
    const aimX = targetX + (convoyTarget ? 0 : target.velocityX * lead);
    const aimZ = targetZ + (convoyTarget ? 0 : target.velocityZ * lead);
    tank.aimRefresh -= dt;
    if (tank.aimRefresh <= 0) {
      tank.aimRefresh = .35 + random() * .25;
      tank.aimErrorX = (random() * 2 - 1) * accuracy;
      tank.aimErrorZ = (random() * 2 - 1) * accuracy;
    }
    let aimPartAt = qualificationAIActive ? performance.now() : 0;
    if (qualificationAIActive) qualificationAITimes[7] += aimPartAt - phaseAt;
    const directVisible = !lineCrossesWalls(tank.x, tank.z, aimX, aimZ,
      elevatedFiringOrigin(tank), .08, tank.id * 12 + 1)
      && !lineCrossesSmoke(tank.x, tank.z, aimX, aimZ);
    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[8] += now - aimPartAt;
      aimPartAt = now;
    }
    if (tank.bankTarget !== targetId || tank.bankRevision !== navEpoch)
      tank.bankAim = false;
    if (directVisible || difficulty === 0) {
      tank.bankAim = false; botPlanPending[1] &= ~(1 << tank.id);
    } else if (tank.bankTarget < 0 || (tank.aiThink <= dt && tank.fireWindup <= 0)
        || (tank.fireWindup > 0 && tank.fireWindup <= dt)
        || ((tank.bankTarget !== targetId || tank.bankRevision !== navEpoch
          || state.time >= tank.bankNext) && botPlanAdmit(tank, 1))) {
      // Ordinary reflected-ray plans share the bounded queue. An expired
      // reaction timer cannot fire during a windup, so it must not repeatedly
      // bypass that queue. First plans and every possible shot still validate
      // now; stale geometry never permits a shot while its plan is pending.
      tank.bankAim = planBankShot(tank, aimX, aimZ);
      tank.bankTarget = targetId; tank.bankRevision = navEpoch;
      tank.bankNext = state.time + .25;
      botPlanPending[1] &= ~(1 << tank.id);
      if (tank.bankAim) {
        tank.bankMirrorX = tank.x + bankAim[0];
        tank.bankMirrorZ = tank.z + bankAim[1];
        tank.bankHitX = bankAim[2]; tank.bankHitZ = bankAim[3];
        tank.bankTargetX = aimX; tank.bankTargetZ = aimZ;
      }
      if (qualificationAIActive) qualificationAICounts[3]++;
    } else if (qualificationAIActive) qualificationAICounts[4]++;
    // Smoke cancels an in-progress warning immediately, even between bank
    // planning ticks. Firing always takes the fresh-plan path above.
    if (tank.bankAim && activeSmokeCount()
        && (lineCrossesSmoke(tank.x, tank.z, tank.bankHitX, tank.bankHitZ)
          || lineCrossesSmoke(tank.bankHitX, tank.bankHitZ,
            tank.bankTargetX, tank.bankTargetZ))) tank.bankAim = false;
    command.aimX = (tank.bankAim ? tank.bankMirrorX - tank.x : aimX - tank.x) + tank.aimErrorX;
    command.aimZ = (tank.bankAim ? tank.bankMirrorZ - tank.z : aimZ - tank.z) + tank.aimErrorZ;
    if (qualificationAIActive) {
      const now = performance.now(); qualificationAITimes[9] += now - aimPartAt;
      qualificationAITimes[4] += now - phaseAt;
      phaseAt = now;
    }
    command.fire = command.secondary = command.gadget = command.ultimate = false;
    const fireAngle = Math.atan2(command.aimX, command.aimZ);
    // Reuse this aim angle in movement instead of a second atan2.
    tank.lastAimX = command.aimX; tank.lastAimZ = command.aimZ;
    tank.aimAngle = fireAngle;
    if (qualificationAIActive) qualificationAITimes[10] += performance.now() - phaseAt;
    const aimedAtTarget = Math.abs(wrapAngle(fireAngle - tank.turret))
      < .16 + accuracy * .2;
    const playerTarget = !convoyTarget && targetId === playerTank().id;
    const targetVisible = directVisible || tank.bankAim;
    let completedWindup = false;
    if (tank.fireWindup > 0) {
      if (!playerTarget || !targetVisible) {
        tank.fireWindup = 0;
        tank.fireSecondaryArmed = false;
      } else {
        tank.fireWindup -= dt;
        if (tank.fireWindup <= 0) {
          const gap = state.time - lastBotPlayerShotAt();
          if (aimedAtTarget && targetDistanceSquared < BOT_FIRE_RANGE_SQUARED
              && gap >= BOT_VOLLEY_GAP) {
            command.secondary = tank.fireSecondaryArmed;
            command.fire = !tank.fireSecondaryArmed;
            tank.fireTelegraphed = true;
            completedWindup = true;
          } else if (aimedAtTarget && targetDistanceSquared < BOT_FIRE_RANGE_SQUARED
              && targetVisible && gap < BOT_VOLLEY_GAP) {
            /* Preserve the telegraph while the global volley lane drains;
               the id offset prevents two ready bots from waking together. */
            tank.fireWindup = botVolleyDelay(tank, gap);
          } else {
            tank.fireWindup = 0;
            tank.fireSecondaryArmed = false;
          }
        }
      }
    }
    tank.aiThink -= dt;
    if (tank.aiThink <= 0 && tank.fireWindup <= 0 && !completedWindup) {
      tank.aiThink = botDifficultyValue(BOT_REACTION, tank) + random() * .16;
      const wantsFire = random() < botDifficultyValue(BOT_FIRE_CHANCE, tank) * tank.aggression
        && aimedAtTarget
        && targetDistanceSquared < BOT_FIRE_RANGE_SQUARED
        && targetVisible;
      const wantsSecondary = tank.secondaryCooldown <= 0
        && directVisible && !tank.bankAim
        && targetDistanceSquared < (tank.classId === 2 ? 64 : 900)
        && Math.abs(wrapAngle(fireAngle - tank.turret)) < .21
        && random() < .24 + difficulty * .08;
      if (playerTarget && (wantsFire || wantsSecondary))
        beginBotFireWindup(tank, wantsSecondary);
      else {
        command.fire = wantsFire;
        command.secondary = wantsSecondary;
      }
      const nearTarget = targetDistanceSquared < 5.76;
      command.gadget = tank.gadgetCooldown <= 0 && (
        (tank.gadget === "REPAIR DRONE" && tank.health < 70)
        || (tank.gadget === "SHIELD" && targetDistance < 12)
        || (tank.gadget === "BOOST TREADS" && goalDistanceSquared > 16)
        || (tank.gadget === "SMOKE" && tank.health < 58)
        || (tank.gadget === "MINES" && nearTarget));
    }
    tank.breachShot = false;
    if (tank.breachActive && !targetVisible && !completedWindup) {
      // No shot at the target: shoot the blocker (exact, no telegraph).
      const planned = tank.breachAngle === tank.breachAngle;
      const angle = planned ? tank.breachAngle
        : Math.atan2(tank.breachX - tank.x, tank.breachZ - tank.z);
      const dx = tank.breachX - tank.x, dz = tank.breachZ - tank.z;
      const reach = Math.max(1, Math.sqrt(dx * dx + dz * dz));
      command.aimX = Math.sin(angle) * reach; command.aimZ = Math.cos(angle) * reach;
      tank.lastAimX = command.aimX; tank.lastAimZ = command.aimZ; tank.aimAngle = angle;
      if (planned && tank.cooldown <= 0 && tank.fireWindup <= 0
          && orientationIndex(tank.turret) === orientationIndex(angle)
          && breachShotHits(tank, angle)) {
        command.fire = true; command.secondary = false;
        tank.breachShot = true; tank.breachShots++;
      }
    }
    if (qualificationAIActive) qualificationAITimes[5] += performance.now() - phaseAt;
  }

  /* Guards hold cover, but not forever. In elimination modes a guard hunts
     (flanking, no cover) once only guards are left on its side, or after
     GUARD_PATIENCE seconds without trading damage, until the next exchange.
     Campaign missions post guards on purpose (2-3 is a fortress of them):
     there only the last foe standing comes out. */
  const GUARD_PATIENCE = 10;
  function guardEscalated(tank) {
    if (state.gameMode !== MODE_SURVIVAL && state.gameMode !== MODE_BILLIARDS && !isOnslaught()) return false;
    const mission = !!campaign?.runtime.on;
    if (!mission && tank.guardIdle >= GUARD_PATIENCE) return true;
    for (let at = 0; at < MAX_TANKS; at++) {
      const other = tanks[at];
      if (other.active && other !== tank && other.team === tank.team
          && (mission || other.role !== "GUARD")) return false;
    }
    return true;
  }

  // beginArena: an arena starts with an empty planning queue.
  function resetBotPlanning() {
    botPlanPending[0] = botPlanPending[1] = 0;
    botPlanCursor[0] = 0; botPlanCursor[1] = 1;
    botPlanCursor[2] = 2; botPlanCursor[3] = 3;
    botPlanFrame = -1; botPlanUsed = botPlanTaken = botPlanKinds = 0;
  }

  /* qualification.js's __treadlineDebug hands these out; never called per frame. */
  const debug = {
    /* The route-planning internals, for the planning fixture's oracles
       (tests/fixtures/treadline-planning.js). */
    routeInternals() {
      while (navBuildCursor < NAV_CELLS) updateBotNavigation();
      return {NAV_CELLS, NAV_SIDE, NAV_FIELD_BLOCKED, MAX_TANKS, navGrids,
        navFieldTemplateViews, navDistances, navQueue, navQueueHeads, navQueueCounts,
        navFieldGoals, navFieldRevisions, tanks, prepareBotRouteField, chooseRouteWaypoint,
        navDiagonal, routePadding, navigationCell, navGridIndex, lineCrossesWalls,
        circleHitsObstacle, steerBotRoute, invalidateBotNavigation, CLASS_COLLISION_RADIUS,
        qualificationAICounts, drain() { while (navBuildCursor < NAV_CELLS) updateBotNavigation(); },
        profiling(on) { const was = qualificationAIActive; setQualificationAIActive(on); return was; }};
    },
    spawnStats() {
      settleSpawnChecks(true);
      return {kept: spawnPocketsKept, moved: spawnPocketsMoved,
        sealedMoved: spawnSealedMoved, sealedStuck: spawnSealedStuck};
    },
    spawnVerdict(index) {
      settleSpawnChecks(true);
      const tank = tanks[index | 0];
      prepareSealGrids(navGridIndex(tank));
      const verdict = spawnVerdict(tank);
      sealPrepared = -1;
      return verdict;
    },
  };

  function setBotProfiling(on) { qualificationAIActive = on; }
  function setProfileArrays(times, counts) {
    qualificationAITimes = times; qualificationAICounts = counts;
  }
  function cancelSpawnChecks() { spawnChecks = 0; }

  return Object.freeze({updateBotCommand, updateBotNavigation,
    restartBotNavigation, resetBotPlanning, prepareNavigationGrids,
    validateSpawns, validateRespawn, settleSpawnChecks, cancelSpawnChecks,
    steerBotRoute, bankAim, setBotProfiling, botDifficulty, botDifficultyValue,
    botDifficultyTables, beginBotFireWindup, pocketKept, navigationCell,
    setProfileArrays, debug});
};
Object.defineProperty(globalThis, "__treadlineCreateBots",
  {configurable: false, writable: false});
