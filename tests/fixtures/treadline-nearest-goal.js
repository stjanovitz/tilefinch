/* Synthetic routing oracle: exact nearest centers and bounded local work. */
(() => {
  const nearest = __treadlineArenaData.spatial.nearestOpenCell;
  const grid = new Uint8Array(256);
  function reference(x, z) {
    let best = Infinity, cell = -1;
    for (let at = 0; at < 256; at++) {
      if (grid[at]) continue;
      const dx = (at & 15) - 7.5 - x, dz = (at >> 4) - 7.5 - z;
      const score = dx * dx + dz * dz;
      if (score < best) { best = score; cell = at; }
    }
    return cell;
  }
  function check(x, z) {
    const actual = nearest(grid, x, z), expected = reference(x, z);
    if (actual !== expected) throw new Error('nearest cell parity: '
      + [x, z, actual, expected]);
  }
  for (let pattern = 0; pattern < 6; pattern++) {
    for (let at = 0; at < 256; at++) {
      const x = at & 15, z = at >> 4;
      grid[at] = pattern === 0 ? 0 : pattern === 1 ? 1
        : pattern === 2 ? (x === 0 || z === 15 ? 0 : 1)
        : pattern === 3 ? ((x * 7 + z * 3) % 5 ? 1 : 0)
        : pattern === 4 ? (x > 2 && x < 13 && z > 2 && z < 13 ? 1 : 0)
        : (x === 3 || x === 12 || z === 3 || z === 12 ? 1 : 0);
    }
    for (let at = 0; at < 256; at++) {
      const x = (at & 15) - 7.5, z = (at >> 4) - 7.5;
      check(x, z);
      check(x + .49, z - .49);
      check(x + .5, z + .5); // exact ties retain ascending-index order
      check(x + .5 - 1e-12, z - .5 + 1e-12);
    }
    check(-12, 12); check(12, -12); check(0, Infinity); check(NaN, 0);
  }
  grid.fill(0); grid[8 * 16 + 8] = 1;
  let visits = 0;
  const counted = new Proxy(grid, {get(target, key) {
    visits++; return target[key];
  }});
  if (nearest(counted, .7, .8) !== reference(.7, .8) || visits > 40)
    throw new Error('local goal must avoid full-grid scan: ' + visits);
  globalThis.pocSummary = 'TREADLINE-NEAREST-GOAL';
})();
