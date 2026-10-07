/* Exact navigation occupancy oracle; no captured gameplay or network data. */
(() => {
  const data = __treadlineArenaData, spatial = data.spatial;
  const radius = new Float32Array([.52 * 1.14])[0] + .06;
  const cache = spatial.createNavigationCache(radius);
  const grid = new Uint8Array(256);
  const barriers = Array.from({length: 6}, () => ({present: false, active: false,
    left: 0, right: 0, top: 0, bottom: 0}));
  const crates = Array.from({length: 4}, () => ({active: true, x: 0, z: 0}));
  const generated = data.generatedArena;
  const savedCounts = [generated.obstacleCount, generated.barrierCount,
    generated.rampCount, generated.gateCount];
  const saved = ['obstacles', 'barriers', 'ramps', 'gates'].map(kind =>
    generated[kind].map(rect => new Float32Array(rect)));
  const generator = __treadlineCreateArenaGenerator(data.arenas, generated);
  let comparisons = 0;
  function box(x, z, left, right, top, bottom) {
    const dx = x < left ? x - left : x > right ? x - right : 0;
    const dz = z < top ? z - top : z > bottom ? z - bottom : 0;
    return dx * dx + dz * dz < radius * radius;
  }
  function reference(arena, cell, gateOpen) {
    const x = (cell & 15) - 7.5, z = (cell >> 4) - 7.5;
    if (Math.abs(x) > 7 || Math.abs(z) > 7) return 1;
    for (let at = 0; at < crates.length; at++) {
      const c = crates[at];
      if (!c.active) continue;
      const dx = Math.max(0, Math.abs(x - c.x) - .22);
      const dz = Math.max(0, Math.abs(z - c.z) - .22);
      if (dx * dx + dz * dz < radius * radius) return 1;
    }
    const bounds = spatial.obstacleBounds[arena];
    for (let at = 0; at < spatial.obstacleCounts[arena] * 4; at += 4)
      if (box(x, z, bounds[at], bounds[at + 1], bounds[at + 2], bounds[at + 3]))
        return 1;
    for (let at = 0; at < barriers.length; at++) {
      const b = barriers[at];
      if (b.active && box(x, z, b.left, b.right, b.top, b.bottom)) return 1;
    }
    if (!gateOpen && spatial.gateCounts[arena]) {
      const g = spatial.gateBounds[arena];
      if (box(x, z, g[0], g[1], g[2], g[3])) return 1;
    }
    return 0;
  }
  function check(arena, pattern) {
    const source = data.arenas[arena];
    const count = spatial.itemCount(source, 'barriers');
    for (let at = 0; at < barriers.length; at++) {
      const b = barriers[at];
      b.present = b.active = at < count;
      if (!b.present) continue;
      const r = source.barriers[at];
      b.left = r[0] - r[2] * .5; b.right = r[0] + r[2] * .5;
      b.top = r[1] - r[3] * .5; b.bottom = r[1] + r[3] * .5;
    }
    for (let at = 0; at < crates.length; at++) {
      const c = crates[at];
      c.active = true;
      c.x = ((pattern * 7 + at * 5) % 15) - 7;
      c.z = ((pattern * 11 + at * 3) % 15) - 7;
      // Include almost-tangent and exact cell-center cases.
      if (at === 0) { c.x = .5 + radius + .22 - 1e-12; c.z = .5; }
      if (at === 1) { c.x = -.5 + radius + .22; c.z = -.5; }
    }
    cache.prepare(arena, barriers, crates);
    let geometryReads = 0;
    const countedBarriers = barriers.map(b => new Proxy(b, {get(target, key) {
      if (key !== 'active') geometryReads++;
      return target[key];
    }}));
    const countedCrates = crates.map(c => new Proxy(c, {get(target, key) {
      if (key !== 'active') geometryReads++;
      return target[key];
    }}));
    const NativeU16 = globalThis.Uint16Array;
    globalThis.Uint16Array = function () { throw new Error('hot mask allocation'); };
    try {
      for (let state = 0; state < 16; state++) {
        for (let at = 0; at < barriers.length; at++)
          barriers[at].active = barriers[at].present && !(state & (1 << (at % 4)));
        for (let at = 0; at < crates.length; at++) crates[at].active = !(state & (1 << at));
        const gateOpen = !!(state & 1);
        grid.fill(7);
        for (let first = 0; first < 256; first += 32) {
          cache.update(grid, first, first + 32, countedBarriers, countedCrates, gateOpen);
          for (let cell = first; cell < first + 32; cell++) {
            comparisons++;
            if (grid[cell] !== reference(arena, cell, gateOpen))
              throw new Error('occupancy mismatch ' + [arena, pattern, state, cell]);
          }
          if (first + 32 < 256 && grid[first + 32] !== 7)
            throw new Error('navigation publication cadence changed');
        }
      }
      if (geometryReads !== 0) throw new Error('hot occupancy re-read geometry');
      // Re-preparing unchanged scenery must also reuse its fixed storage.
      cache.prepare(arena, barriers, crates);
    } finally { globalThis.Uint16Array = NativeU16; }
  }
  try {
    for (let arena = 0; arena < 3; arena++) check(arena, arena);
    for (let seed = 1; seed <= 32; seed++) {
      generator.materialize(seed * 7919);
      spatial.fillSpatialData(3);
      check(3, seed);
    }
    let refused = false;
    try { cache.prepare(0, new Array(7), crates); } catch (e) { refused = e instanceof RangeError; }
    if (!refused || comparisons !== 35 * 16 * 256)
      throw new Error('navigation occupancy capacity or coverage');
  } finally {
    for (let kind = 0; kind < saved.length; kind++) {
      const list = generated[['obstacles', 'barriers', 'ramps', 'gates'][kind]];
      for (let at = 0; at < list.length; at++) list[at].set(saved[kind][at]);
    }
    [generated.obstacleCount, generated.barrierCount, generated.rampCount,
      generated.gateCount] = savedCounts;
    spatial.fillSpatialData(3);
  }
  globalThis.pocSummary = 'TREADLINE-NAVIGATION-OCCUPANCY';
})();
