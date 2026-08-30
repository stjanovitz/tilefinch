(() => {
  "use strict";

  const SIDE = 32, CELLS = SIDE * SIDE, GENERATED = 3;
  const arenas = [
    {obstacles: [
      [-4.55, 0, .55, 5.1], [-2.75, 0, .55, 5.1],
      [3.05, -1.65, 3.25, .55], [4.4, -.25, .55, 2.8],
      [2.75, 3.25, .55, 2.35], [-.3, 3.8, 2.2, .55]],
     barriers: [[-3.65, 2.8, 1.45, .38], [2.25, -.7, 1.7, .38],
       [5, -4, 1.5, .4]],
     ramps: [[-5.75, 1.45, 1.45, 3.1, .66, 1],
       [5.65, 1.5, 1.45, 3.05, .66, -1]],
     gates: [[0, -.15, .42, 2.55]], enemies: 3},
    {obstacles: [
      [-3.6, -1.75, .55, 4.35], [3.6, 1.75, .55, 4.35],
      [-1.75, 3.4, 3.2, .55], [1.75, -3.4, 3.2, .55],
      [-5.35, 3.7, 2.05, .55], [5.35, -3.7, 2.05, .55]],
     barriers: [[-1.5, -1.2, 1.9, .38], [1.5, 1.2, 1.9, .38],
       [-4.8, -.15, .4, 1.8], [4.8, .15, .4, 1.8]],
     ramps: [[-5.55, -2.05, 1.5, 3, .64, 1],
       [5.55, 2.05, 1.5, 3, .64, -1]],
     gates: [[0, 0, 2.5, .42]], enemies: 4},
    {obstacles: [
      [-5.15, -3.55, 3, .55], [3.65, -2.15, .55, 3],
      [-4.35, -.15, .55, 3.05], [4.35, 1.55, 3, .55],
      [-5.15, 4.15, 2.35, .55], [2.95, 4.45, .55, 2.25]],
     barriers: [[1.35, -3.45, 2, .38], [-4, -1.35, 1.8, .38],
       [1.45, .55, 1.9, .38], [-3.85, 2.65, 1.9, .38]],
     ramps: [[-6.15, -1.65, 1.35, 2.8, .68, 1],
       [5.85, 2.85, 1.4, 2.8, .68, -1]],
     gates: [[0, 0, .42, 2.45]], enemies: 5},
  ];
  const generatedArena = {
    generated: true,
    obstacles: Array.from({length: 16}, () => new Float32Array(4)),
    barriers: Array.from({length: 6}, () => new Float32Array(4)),
    ramps: Array.from({length: 2}, () => new Float32Array(6)),
    gates: [new Float32Array(4)],
    obstacleCount: 0, barrierCount: 0, rampCount: 0, gateCount: 1,
    enemies: 5,
  };
  arenas.push(generatedArena);
  const obstacleBounds = Array.from({length: arenas.length},
    () => new Float32Array(64));
  const gateBounds = Array.from({length: arenas.length},
    () => new Float32Array(4));
  const obstacleCounts = new Uint8Array(arenas.length);
  const gateCounts = new Uint8Array(arenas.length);
  const rampGrids = Array.from({length: arenas.length},
    () => new Int8Array(CELLS));
  const bulletObstacleGrids = Array.from({length: arenas.length},
    () => new Uint16Array(CELLS));
  const bulletGateGrids = Array.from({length: arenas.length},
    () => new Uint16Array(CELLS));
  const circleObstacleGrids = Array.from({length: arenas.length},
    () => new Uint16Array(CELLS));
  const circleGateGrids = Array.from({length: arenas.length},
    () => new Uint16Array(CELLS));
  function itemCount(arena, kind) {
    if (!arena.generated) return arena[kind].length;
    return kind === "obstacles" ? arena.obstacleCount
      : kind === "barriers" ? arena.barrierCount
        : kind === "ramps" ? arena.rampCount : arena.gateCount;
  }
  function fillBounds(list, count, bounds) {
    bounds.fill(0);
    for (let at = 0; at < count; at++) {
      const item = list[at], out = at * 4;
      bounds[out] = item[0] - item[2] * .5;
      bounds[out + 1] = item[0] + item[2] * .5;
      bounds[out + 2] = item[1] - item[3] * .5;
      bounds[out + 3] = item[1] + item[3] * .5;
    }
  }
  function fillRectGrid(bounds, count, radius, grid) {
    grid.fill(0);
    for (let rectangle = 0; rectangle < count; rectangle++) {
      const at = rectangle * 4;
      const firstX = Math.max(0, Math.floor((bounds[at] - radius + 8) * 2));
      const lastX = Math.min(31, Math.floor((bounds[at + 1] + radius + 8) * 2));
      const firstZ = Math.max(0, Math.floor((bounds[at + 2] - radius + 8) * 2));
      const lastZ = Math.min(31, Math.floor((bounds[at + 3] + radius + 8) * 2));
      const bit = 1 << rectangle;
      for (let z = firstZ; z <= lastZ; z++)
        for (let x = firstX; x <= lastX; x++) grid[z * SIDE + x] |= bit;
    }
  }
  function fillSpatialData(index) {
    const arena = arenas[index];
    const obstacles = itemCount(arena, "obstacles");
    const gates = itemCount(arena, "gates");
    obstacleCounts[index] = obstacles; gateCounts[index] = gates;
    fillBounds(arena.obstacles, obstacles, obstacleBounds[index]);
    fillBounds(arena.gates, gates, gateBounds[index]);
    const rampGrid = rampGrids[index];
    rampGrid.fill(0);
    const ramps = itemCount(arena, "ramps");
    for (let at = 0; at < ramps; at++) {
      const ramp = arena.ramps[at];
      const firstX = Math.max(0, Math.floor((ramp[0] - ramp[2] * .5 + 8) * 2));
      const lastX = Math.min(31, Math.floor((ramp[0] + ramp[2] * .5 + 8) * 2));
      const firstZ = Math.max(0, Math.floor((ramp[1] - ramp[3] * .5 + 8) * 2));
      const lastZ = Math.min(31, Math.floor((ramp[1] + ramp[3] * .5 + 8) * 2));
      for (let z = firstZ; z <= lastZ; z++)
        for (let x = firstX; x <= lastX; x++) rampGrid[z * SIDE + x] = at + 1;
    }
    fillRectGrid(obstacleBounds[index], obstacles, .1,
      bulletObstacleGrids[index]);
    fillRectGrid(gateBounds[index], gates, .1, bulletGateGrids[index]);
    fillRectGrid(obstacleBounds[index], obstacles, .6,
      circleObstacleGrids[index]);
    fillRectGrid(gateBounds[index], gates, .6, circleGateGrids[index]);
  }
  for (let at = 0; at < arenas.length; at++) fillSpatialData(at);
  const spatial = Object.freeze({obstacleBounds, gateBounds, obstacleCounts,
    gateCounts, rampGrids, bulletObstacleGrids, bulletGateGrids,
    circleObstacleGrids, circleGateGrids, fillSpatialData, itemCount});
  Object.defineProperty(globalThis, "__treadlineArenaData", {
    value: Object.freeze({arenas, generatedArena, spatial}),
    configurable: false, writable: false,
  });
  /* Generated geometry is expressed in half-world-unit cells. Integer-only
     generation makes a transmitted winning seed authoritative across the PSP
     and host libm implementations; the conversion to Float32 happens once. */
  const MOTIF_STRIDE = 4;
  const MOTIF_LANE = 0, MOTIF_DOGLEG = 1, MOTIF_BLOCK = 2;
  const MOTIF_NOOK = 3, MOTIF_EROSION = 4, MOTIF_RAMP_ARC = 5;
  const motifCells = new Int8Array([
    8, 0, 1, 9, -4, 6, 6, 1, -1, 4, 1, 4,
    -10, -7, 4, 1, 3, -2, 3, 1, -11, -3, 3, 6,
  ]);

  function create(arenas, generated) {
    /* All arena analysis storage is allocated once here. Attempts only clear
       and refill it, so run-start generation cannot seed a later GC pause. */
    const blocked = new Uint8Array(CELLS);
    const visited = new Uint8Array(CELLS);
    const parents = new Int16Array(CELLS);
    const queue = new Int16Array(CELLS);
    const histogram = new Uint16Array(SIDE + 1);
    const spawnProbeOffsets = new Int8Array([
      4, 11, 10, 6, 11, -4, 6, -10,
      -4, -11, -10, -6, -11, 4, -6, 10,
    ]);
    const identity = [generated.obstacles, generated.barriers, generated.ramps,
      blocked, visited, parents, queue, histogram, spawnProbeOffsets];
    const result = {
      accepted: false, connected: false, flanking: false,
      sightlines: false, cover: false, density: false, symmetry: false,
      densityPercent: 0, coverPercent: 0, longLanes: 0,
      medianEngagement: 0, spawnLineClear: false, spawnClearCount: 0,
      reachable: 0, drivable: 0,
    };
    const generatedResult = {accepted: false, attempts: 0,
      fallback: false, checksum: 0, winningSeed: 0};
    /* The deploy-time stepper owns no arena storage of its own: candidate()
       and validate() refill the same fixed buffers as generate(). The result
       object is likewise retained so a two-attempt frame slice allocates
       nothing. Callers must consume it synchronously. */
    const steppedResult = {done: false, accepted: false, attempts: 0,
      fallback: false, checksum: 0, winningSeed: 0};
    let steppedSeed = 1, steppedAttemptLimit = 20, steppedAttempt = 0;
    let steppedValidationActive = false, steppedWinningSeed = 0;
    let seedState = 1;

    function count(arena, kind) {
      if (!arena.generated) return arena[kind].length;
      return kind === "obstacles" ? arena.obstacleCount
        : kind === "barriers" ? arena.barrierCount
          : kind === "ramps" ? arena.rampCount : arena.gateCount;
    }
    function next() {
      seedState ^= seedState << 13;
      seedState ^= seedState >>> 17;
      seedState ^= seedState << 5;
      return seedState >>> 0;
    }
    function rect(list, at, x, z, width, depth) {
      const out = list[at];
      out[0] = x; out[1] = z; out[2] = width; out[3] = depth;
    }
    function pair(list, at, x, z, width, depth) {
      rect(list, at, x, z, width, depth);
      rect(list, at + 1, -x, -z, width, depth);
    }
    function motifPair(list, at, motif, offsetX, offsetZ, rotate, mirror) {
      const source = motif * MOTIF_STRIDE;
      let x = motifCells[source] + offsetX;
      let z = motifCells[source + 1] + offsetZ;
      let width = motifCells[source + 2], depth = motifCells[source + 3];
      if (rotate) {
        const previousX = x, previousWidth = width;
        x = z; z = -previousX; width = depth; depth = previousWidth;
      }
      pair(list, at, mirror ? -x * .5 : x * .5, z * .5,
        width * .5, depth * .5);
    }
    function ramp(at, x, z, width, depth, height, direction) {
      const out = generated.ramps[at];
      out[0] = x; out[1] = z; out[2] = width; out[3] = depth;
      out[4] = height; out[5] = direction;
    }
    function attemptSeed(seed, attempt) {
      if (!attempt) return seed >>> 0 || 0x6d2b79f5;
      const derived = (seed ^ Math.imul(attempt, 0x9e3779b1)) >>> 0;
      return derived || 0x6d2b79f5;
    }
    function candidate(seed) {
      seedState = seed >>> 0 || 0x6d2b79f5;
      const mirror = next() & 1;
      const rotate = next() & 1;
      const firstExtra = next() & 1 ? MOTIF_BLOCK : MOTIF_NOOK;
      const secondExtra = firstExtra === MOTIF_BLOCK ? MOTIF_NOOK : MOTIF_BLOCK;
      motifPair(generated.obstacles, 0, MOTIF_LANE,
        (next() % 3) - 1, (next() % 3) - 1, rotate, mirror);
      motifPair(generated.obstacles, 2, MOTIF_DOGLEG,
        (next() % 3) - 1, (next() % 3) - 1, rotate, mirror);
      motifPair(generated.obstacles, 4, firstExtra,
        (next() % 3) - 1, (next() % 3) - 1, rotate, mirror);
      motifPair(generated.obstacles, 6, secondExtra,
        (next() % 3) - 1, (next() % 3) - 1, rotate, mirror);
      generated.obstacleCount = 8;
      motifPair(generated.barriers, 0, MOTIF_EROSION,
        (next() % 3) - 1, (next() % 3) - 1, rotate, mirror);
      motifPair(generated.barriers, 2, MOTIF_EROSION,
        6 + (next() % 3), 2 + (next() % 3), rotate, !mirror);
      generated.barrierCount = 4;
      const rampSource = MOTIF_RAMP_ARC * MOTIF_STRIDE;
      const rampX = (motifCells[rampSource] + (next() % 3) - 1) * .5;
      const rampZ = (motifCells[rampSource + 1] + (next() % 3) - 1) * .5;
      const rampWidth = motifCells[rampSource + 2] * .5;
      const rampDepth = motifCells[rampSource + 3] * .5;
      ramp(0, mirror ? -rampX : rampX, rampZ,
        rampWidth, rampDepth, .5, 1);
      ramp(1, mirror ? rampX : -rampX, -rampZ,
        rampWidth, rampDepth, .5, -1);
      generated.rampCount = 2;
      if (next() & 1) rect(generated.gates, 0, 0, 0, .5, 2.5);
      else rect(generated.gates, 0, 0, 0, 2.5, .5);
      generated.gateCount = 1;
      generated.enemies = 5;
    }

    function mark(item, clearance) {
      const firstX = Math.max(1, Math.floor(
        (item[0] - item[2] * .5 - clearance + 8) * 2));
      const lastX = Math.min(30, Math.floor(
        (item[0] + item[2] * .5 + clearance + 8) * 2));
      const firstZ = Math.max(1, Math.floor(
        (item[1] - item[3] * .5 - clearance + 8) * 2));
      const lastZ = Math.min(30, Math.floor(
        (item[1] + item[3] * .5 + clearance + 8) * 2));
      for (let z = firstZ; z <= lastZ; z++)
        for (let x = firstX; x <= lastX; x++) blocked[z * SIDE + x] = 1;
    }
    function buildGrid(arena) {
      blocked.fill(0);
      for (let edge = 0; edge < SIDE; edge++) {
        blocked[edge] = blocked[(SIDE - 1) * SIDE + edge] = 1;
        blocked[edge * SIDE] = blocked[edge * SIDE + SIDE - 1] = 1;
      }
      let total = count(arena, "obstacles");
      for (let at = 0; at < total; at++) mark(arena.obstacles[at], .55);
      total = count(arena, "barriers");
      for (let at = 0; at < total; at++) mark(arena.barriers[at], .35);
      total = count(arena, "gates");
      for (let at = 0; at < total; at++) mark(arena.gates[at], .35);
    }
    function nearest(x, z) {
      const centerX = Math.max(1, Math.min(30, Math.floor((x + 8) * 2)));
      const centerZ = Math.max(1, Math.min(30, Math.floor((z + 8) * 2)));
      for (let radius = 0; radius <= 4; radius++) {
        for (let dz = -radius; dz <= radius; dz++) {
          for (let dx = -radius; dx <= radius; dx++) {
            if (Math.abs(dx) !== radius && Math.abs(dz) !== radius) continue;
            const px = centerX + dx, pz = centerZ + dz;
            if (px <= 0 || px >= 31 || pz <= 0 || pz >= 31) continue;
            const cell = pz * SIDE + px;
            if (!blocked[cell]) return cell;
          }
        }
      }
      return -1;
    }
    let floodRead = 0, floodWrite = 0, floodRefused = -1;
    let floodRefusedSecond = -1, floodRetainParents = false;
    function beginFlood(start, refused, retainParents, refusedSecond = -1) {
      visited.fill(0);
      if (retainParents) parents.fill(-1);
      floodRead = floodWrite = 0;
      floodRefused = refused;
      floodRefusedSecond = refusedSecond;
      floodRetainParents = retainParents;
      if (start < 0 || start === refused || start === refusedSecond
          || blocked[start]) return;
      queue[floodWrite++] = start; visited[start] = 1;
    }
    function stepFlood(nodeLimit = 192) {
      let processed = 0;
      while (floodRead < floodWrite && processed < nodeLimit) {
        const cell = queue[floodRead++], x = cell & 31;
        processed++;
        for (let side = 0; side < 4; side++) {
          const nextCell = side === 0 ? cell - 1 : side === 1 ? cell + 1
            : side === 2 ? cell - SIDE : cell + SIDE;
          if (nextCell === floodRefused || nextCell === floodRefusedSecond
              || visited[nextCell] || blocked[nextCell])
            continue;
          if (side < 2 && Math.abs((nextCell & 31) - x) !== 1) continue;
          visited[nextCell] = 1;
          if (floodRetainParents) parents[nextCell] = cell;
          queue[floodWrite++] = nextCell;
        }
      }
      return floodRead >= floodWrite;
    }
    function flood(start, refused, retainParents, refusedSecond = -1) {
      beginFlood(start, refused, retainParents, refusedSecond);
      while (!stepFlood()) {}
      return floodWrite;
    }
    function lineClear(first, last) {
      let x = first & 31, z = first >> 5;
      const endX = last & 31, endZ = last >> 5;
      const dx = Math.abs(endX - x), sx = x < endX ? 1 : -1;
      const dz = -Math.abs(endZ - z), sz = z < endZ ? 1 : -1;
      let error = dx + dz;
      for (let step = 0; step < 64; step++) {
        if (blocked[z * SIDE + x]) return false;
        if (x === endX && z === endZ) return true;
        const twice = error * 2;
        if (twice >= dz) { error += dz; x += sx; }
        if (twice <= dx) { error += dx; z += sz; }
      }
      return false;
    }
    let validationPhase = -1, validationIndex = 0, validationMode = 0;
    let validationSpawn = -1, validationEnemy = -1;
    let validationFlankCursor = -1;
    let validationDrivable = 0, validationOccupied = 0;
    let validationHorizontal = 0, validationVertical = 0;
    let validationSamples = 0, validationCoverSamples = 0;
    let validationCovered = 0;

    function beginValidation(index, mode) {
      validationIndex = index;
      validationMode = mode;
      validationPhase = 0;
      validationSpawn = validationEnemy = -1;
      validationFlankCursor = -1;
      validationDrivable = validationOccupied = 0;
      validationHorizontal = validationVertical = validationSamples = 0;
      validationCoverSamples = validationCovered = 0;
      result.accepted = false;
    }

    function coverRange(firstZ, lastZ) {
      const enemyX = validationEnemy & 31, enemyZ = validationEnemy >> 5;
      for (let z = firstZ; z < lastZ; z += 2)
        for (let x = 3; x < 29; x += 2) {
          if (blocked[z * SIDE + x]) continue;
          validationCoverSamples++;
          const stepX = enemyX === x ? 0 : enemyX > x ? 1 : -1;
          const stepZ = enemyZ === z ? 0 : enemyZ > z ? 1 : -1;
          let found = false;
          for (let distance = 1; distance <= 4 && !found; distance++) {
            const aheadX = x + stepX * distance;
            const aheadZ = z + stepZ * distance;
            for (let side = -1; side <= 1; side++) {
              const px = Math.max(0, Math.min(31,
                aheadX + (stepZ ? side : 0)));
              const pz = Math.max(0, Math.min(31,
                aheadZ + (stepX ? side : 0)));
              if (blocked[pz * SIDE + px]) { found = true; break; }
            }
          }
          if (found) validationCovered++;
        }
    }

    function stepValidation() {
      if (validationPhase === 0) {
        buildGrid(arenas[validationIndex]);
        validationPhase = 1;
        return false;
      }
      if (validationPhase === 1) {
        validationSpawn = nearest(0, -5.8);
        validationEnemy = nearest(1.9, 5.25);
        let allDrivable = 0;
        validationDrivable = validationOccupied = 0;
        for (let z = 1; z < 31; z++) for (let x = 1; x < 31; x++) {
          if (!blocked[z * SIDE + x]) allDrivable++;
          if (x >= 2 && x < 30 && z >= 2 && z < 30) {
            if (blocked[z * SIDE + x]) validationOccupied++;
            else validationDrivable++;
          }
        }
        result.drivable = validationDrivable;
        result.reachable = allDrivable;
        beginFlood(validationSpawn, -1, true);
        validationPhase = 2;
        return false;
      }
      if (validationPhase === 2) {
        if (!stepFlood()) return false;
        const reachable = floodWrite;
        result.connected = validationSpawn >= 0 && validationEnemy >= 0
          && !!visited[validationEnemy] && reachable === result.reachable;
        result.reachable = reachable; result.drivable = validationDrivable;
        validationPhase = 3;
        return false;
      }
      if (validationPhase === 3) {
        let pathLength = 0, cursor = validationEnemy;
        while (cursor >= 0 && cursor !== validationSpawn
            && pathLength < CELLS) {
          cursor = parents[cursor]; pathLength++;
        }
        cursor = validationEnemy;
        for (let step = 0; step < (pathLength >> 1) && cursor >= 0; step++)
          cursor = parents[cursor];
        const predecessor = cursor >= 0 ? parents[cursor] : -1;
        validationFlankCursor = cursor;
        beginFlood(validationSpawn, cursor, false, predecessor);
        validationPhase = 4;
        return false;
      }
      if (validationPhase === 4) {
        if (!stepFlood()) return false;
        result.flanking = validationFlankCursor >= 0
          && !!visited[validationEnemy];
        validationPhase = 5;
        return false;
      }
      if (validationPhase === 5) {
        histogram.fill(0);
        let previousLong = false;
        for (let z = 2; z < 30; z++) {
          let longest = 0, run = 0;
          for (let x = 2; x <= 30; x++) {
            if (x < 30 && !blocked[z * SIDE + x]) run++;
            else { longest = Math.max(longest, run); run = 0; }
          }
          const long = longest >= 20;
          if (long && !previousLong) validationHorizontal++;
          previousLong = long;
          if (longest) {
            histogram[Math.min(32, longest)]++;
            validationSamples++;
          }
        }
        validationPhase = 6;
        return false;
      }
      if (validationPhase === 6) {
        let previousLong = false;
        for (let x = 2; x < 30; x++) {
          let longest = 0, run = 0;
          for (let z = 2; z <= 30; z++) {
            if (z < 30 && !blocked[z * SIDE + x]) run++;
            else { longest = Math.max(longest, run); run = 0; }
          }
          const long = longest >= 20;
          if (long && !previousLong) validationVertical++;
          previousLong = long;
          if (longest) {
            histogram[Math.min(32, longest)]++;
            validationSamples++;
          }
        }
        validationPhase = 7;
        return false;
      }
      if (validationPhase === 7) {
        let median = 0, accumulated = 0;
        for (let length = 1; length <= 32; length++) {
          accumulated += histogram[length];
          if (!median && accumulated * 2 >= validationSamples) median = length;
        }
        result.longLanes = Math.min(2,
          validationHorizontal + validationVertical);
        result.medianEngagement = median * .5;
        let clearSpawns = 0;
        if (validationSpawn >= 0)
          for (let at = 0; at < spawnProbeOffsets.length; at += 2) {
            const probe = nearest(spawnProbeOffsets[at] * .5,
              spawnProbeOffsets[at + 1] * .5);
            if (probe >= 0 && lineClear(validationSpawn, probe)) clearSpawns++;
          }
        result.spawnClearCount = clearSpawns;
        result.spawnLineClear = clearSpawns >= 4;
        result.sightlines = result.longLanes >= 1
          && result.medianEngagement >= 4 && result.medianEngagement <= 8
          && !result.spawnLineClear;
        validationPhase = 8;
        return false;
      }
      if (validationPhase === 8) {
        coverRange(3, 17);
        validationPhase = 9;
        return false;
      }
      if (validationPhase === 9) {
        coverRange(17, 29);
        result.coverPercent = validationCoverSamples
          ? Math.floor(validationCovered * 100 / validationCoverSamples) : 0;
        result.cover = result.coverPercent >= 55;
        result.densityPercent = Math.floor(validationOccupied * 100
          / (validationOccupied + validationDrivable));
        result.density = result.densityPercent >= 25
          && result.densityPercent <= 40;
        validationPhase = 10;
        return false;
      }
      result.symmetry = true;
      if (validationMode === 1 || validationIndex === GENERATED) {
        for (let z = 1; z < 31 && result.symmetry; z++)
          for (let x = 1; x < 31; x++)
            if (blocked[z * SIDE + x] !== blocked[(31 - z) * SIDE + 31 - x]) {
              result.symmetry = false; break;
            }
      }
      result.accepted = result.connected && result.flanking
        && result.sightlines && result.cover && result.density && result.symmetry;
      validationPhase = -1;
      return true;
    }

    function validate(index, mode) {
      beginValidation(index, mode);
      while (!stepValidation()) {}
      return result;
    }

    function fallback() {
      const source = arenas[1];
      for (let at = 0; at < source.obstacles.length; at++) {
        const item = source.obstacles[at];
        rect(generated.obstacles, at, item[0], item[1], item[2], item[3]);
      }
      for (let at = 6; at < 8; at++) {
        const item = source.obstacles[at - 6];
        rect(generated.obstacles, at, item[0], item[1], item[2], item[3]);
      }
      generated.obstacleCount = 8;
      for (let at = 0; at < source.barriers.length; at++) {
        const item = source.barriers[at];
        rect(generated.barriers, at, item[0], item[1], item[2], item[3]);
      }
      generated.barrierCount = source.barriers.length;
      for (let at = 0; at < 2; at++) {
        const item = source.ramps[at];
        ramp(at, item[0], item[1], item[2], item[3], item[4], item[5]);
      }
      generated.rampCount = 2;
      const gate = source.gates[0];
      rect(generated.gates, 0, gate[0], gate[1], gate[2], gate[3]);
      generated.gateCount = 1; generated.enemies = source.enemies;
    }
    function checksum() {
      let hash = 2166136261 >>> 0;
      for (let kind = 0; kind < 4; kind++) {
        const list = kind === 0 ? generated.obstacles
          : kind === 1 ? generated.barriers
            : kind === 2 ? generated.ramps : generated.gates;
        const total = count(generated, kind === 0 ? "obstacles"
          : kind === 1 ? "barriers" : kind === 2 ? "ramps" : "gates");
        const width = kind === 2 ? 6 : 4;
        hash ^= (kind << 8) | total;
        hash = Math.imul(hash, 16777619) >>> 0;
        for (let at = 0; at < total; at++) for (let field = 0; field < width; field++) {
          hash ^= Math.round(list[at][field] * 2) & 0xffff;
          hash = Math.imul(hash, 16777619) >>> 0;
        }
      }
      return (hash ^ (hash >>> 16)) & 0xffff;
    }
    function generate(seed, attemptLimit = 20) {
      seed = seed >>> 0 || 1;
      attemptLimit = Math.max(0, Math.min(20, attemptLimit | 0));
      generatedResult.accepted = false;
      generatedResult.fallback = true;
      generatedResult.attempts = attemptLimit;
      generatedResult.winningSeed = 0;
      for (let attempt = 0; attempt < attemptLimit; attempt++) {
        const winningSeed = attemptSeed(seed, attempt);
        candidate(winningSeed);
        if (validate(GENERATED, 3).accepted) {
          generatedResult.accepted = true;
          generatedResult.fallback = false;
          generatedResult.attempts = attempt + 1;
          generatedResult.winningSeed = winningSeed;
          break;
        }
      }
      if (generatedResult.fallback) fallback();
      generatedResult.checksum = checksum();
      return generatedResult;
    }
    function beginGeneration(seed, attemptLimit = 20) {
      steppedSeed = seed >>> 0 || 1;
      steppedAttemptLimit = Math.max(0, Math.min(20, attemptLimit | 0));
      steppedAttempt = 0;
      steppedValidationActive = false;
      steppedResult.done = false;
      steppedResult.accepted = false;
      steppedResult.fallback = false;
      steppedResult.attempts = 0;
      steppedResult.checksum = 0;
      steppedResult.winningSeed = 0;
      if (!steppedAttemptLimit) {
        fallback();
        steppedResult.done = true;
        steppedResult.fallback = true;
        steppedResult.checksum = checksum();
      }
      return steppedResult;
    }
    function stepGeneration(maxSlices = 2) {
      if (steppedResult.done) return steppedResult;
      maxSlices = Math.max(0, Math.min(20, maxSlices | 0));
      for (let slice = 0; slice < maxSlices && !steppedResult.done; slice++) {
        if (!steppedValidationActive) {
          steppedWinningSeed = attemptSeed(steppedSeed, steppedAttempt);
          candidate(steppedWinningSeed);
          beginValidation(GENERATED, 3);
          steppedValidationActive = true;
          continue;
        }
        if (!stepValidation()) continue;
        steppedValidationActive = false;
        steppedAttempt++;
        steppedResult.attempts = steppedAttempt;
        if (result.accepted) {
          steppedResult.done = true;
          steppedResult.accepted = true;
          steppedResult.winningSeed = steppedWinningSeed;
          break;
        }
        if (steppedAttempt >= steppedAttemptLimit) {
          fallback();
          steppedResult.done = true;
          steppedResult.fallback = true;
        }
      }
      if (steppedResult.done) steppedResult.checksum = checksum();
      return steppedResult;
    }
    function materialize(seed) {
      seed = seed >>> 0;
      if (!seed) return 0;
      candidate(seed);
      return checksum();
    }
    function storageStable() {
      return identity[0] === generated.obstacles
        && identity[1] === generated.barriers
        && identity[2] === generated.ramps
        && identity[3] === blocked && identity[4] === visited
        && identity[5] === parents && identity[6] === queue
        && identity[7] === histogram && identity[8] === spawnProbeOffsets;
    }
    return Object.freeze({generate, beginGeneration, stepGeneration,
      materialize, validate, storageStable});
  }

  Object.defineProperty(globalThis, "__treadlineCreateArenaGenerator", {
    value: create, configurable: false, writable: false,
  });
})();
