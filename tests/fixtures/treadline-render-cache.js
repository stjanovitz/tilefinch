/* Authored font/terrain oracle: no captured gameplay or private fuzz inputs. */
(() => {
  const d = __treadlineDebug, data = __treadlineArenaData;
  const chars = ' 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/.,:+!';
  // Authored 5x7 bitmaps are the independent representation here; the game
  // stores a rectangle cover of each, drawn one pixel wider (HUD_INK_BOLD).
  const bitmaps = {
    '0': '.XXX.|X...X|X...X|X...X|X...X|X...X|.XXX.',
    '1': '..X..|.XX..|..X..|..X..|..X..|..X..|.XXX.',
    '2': 'XXXX.|....X|....X|.XXX.|X....|X....|XXXXX',
    '3': 'XXXX.|....X|....X|.XXX.|....X|....X|XXXX.',
    '4': 'X...X|X...X|X...X|XXXXX|....X|....X|....X',
    '5': 'XXXXX|X....|XXXX.|....X|....X|....X|XXXX.',
    '6': '.XXX.|X....|X....|XXXX.|X...X|X...X|.XXX.',
    '7': 'XXXXX|....X|...X.|..X..|..X..|..X..|..X..',
    '8': '.XXX.|X...X|X...X|XXXXX|X...X|X...X|.XXX.',
    '9': '.XXX.|X...X|X...X|.XXXX|....X|....X|.XXX.',
    'A': '.XXX.|X...X|X...X|XXXXX|X...X|X...X|X...X',
    'B': 'XXXX.|X...X|X...X|XXXX.|X...X|X...X|XXXX.',
    'C': '.XXXX|X....|X....|X....|X....|X....|.XXXX',
    'D': 'XXXX.|X...X|X...X|X...X|X...X|X...X|XXXX.',
    'E': 'XXXXX|X....|X....|XXXX.|X....|X....|XXXXX',
    'F': 'XXXXX|X....|X....|XXXX.|X....|X....|X....',
    'G': '.XXXX|X....|X....|X..XX|X...X|X...X|.XXX.',
    'H': 'X...X|X...X|X...X|XXXXX|X...X|X...X|X...X',
    'I': '.XXX.|..X..|..X..|..X..|..X..|..X..|.XXX.',
    'J': '....X|....X|....X|....X|X...X|X...X|.XXX.',
    'K': 'X...X|X...X|X..X.|XXX..|X..X.|X...X|X...X',
    'L': 'X....|X....|X....|X....|X....|X....|XXXXX',
    'M': 'X...X|XX.XX|X.X.X|X.X.X|X...X|X...X|X...X',
    'N': 'X...X|X...X|XX..X|X.X.X|X..XX|X...X|X...X',
    'O': '.XXX.|X...X|X...X|X...X|X...X|X...X|.XXX.',
    'P': 'XXXX.|X...X|X...X|XXXX.|X....|X....|X....',
    'Q': '.XXX.|X...X|X...X|X...X|X...X|X..X.|.XX.X',
    'R': 'XXXX.|X...X|X...X|XXXX.|X..X.|X...X|X...X',
    'S': '.XXXX|X....|X....|.XXX.|....X|....X|XXXX.',
    'T': 'XXXXX|..X..|..X..|..X..|..X..|..X..|..X..',
    'U': 'X...X|X...X|X...X|X...X|X...X|X...X|.XXX.',
    'V': 'X...X|X...X|X...X|X...X|X...X|.X.X.|..X..',
    'W': 'X...X|X...X|X...X|X.X.X|X.X.X|X.X.X|.X.X.',
    'X': 'X...X|X...X|.X.X.|..X..|.X.X.|X...X|X...X',
    'Y': 'X...X|X...X|.X.X.|..X..|..X..|..X..|..X..',
    'Z': 'XXXXX|....X|...X.|..X..|.X...|X....|XXXXX',
    '-': '.....|.....|.....|.XXX.|.....|.....|.....',
    '/': '...X.|...X.|..X..|..X..|..X..|.X...|.X...',
    '.': '.....|.....|.....|.....|.....|.....|..X..',
    ',': '.....|.....|.....|.....|.....|..X..|.X...',
    ':': '.....|.....|..X..|.....|.....|..X..|.....',
    '+': '.....|.....|..X..|.XXX.|..X..|.....|.....',
    '!': '..X..|..X..|..X..|..X..|..X..|.....|..X..',
  };
  function check(value, label) { if (!value) throw new Error(label); }
  // Recover glyph-local pixel coordinates (origin -1.25, 2.5) from clip space.
  const local = (words, at) => [
    Math.round(((words[at] + 1) * 160 + 1.25) * 4) / 4,
    Math.round(((1 - words[at + 1]) * 90 - 2.5) * 4) / 4];
  const ink = [.91, .98, 1, 1].map(Math.fround);
  function quads(words) {
    check(words.length % 24 === 0, 'HUD quads are four six-word vertices');
    const out = [];
    for (let at = 0; at < words.length; at += 24) {
      const corner = [0, 6, 12, 18].map((offset) => local(words, at + offset));
      for (const offset of [0, 6, 12, 18])
        check(ink.every((value, channel) =>
          Object.is(words[at + offset + 2 + channel], value)), 'HUD ink tint');
      const [left, top] = corner[0], [right, bottom] = corner[2];
      check(corner[1][0] === right && corner[1][1] === top
        && corner[3][0] === left && corner[3][1] === bottom
        && right > left && bottom > top, 'HUD quad corner order');
      out.push([left, top, right, bottom]);
    }
    return out;
  }
  function coversBitmap(rects, scale, character) {
    const cells = new Set(), expected = new Set();
    for (const [left, top, right, bottom] of rects)
      for (let y = top; y < bottom; y++) for (let x = left; x < right; x++)
        cells.add(x + ',' + y);
    bitmaps[character].split('|').forEach((row, y) => [...row].forEach((pen, x) => {
      if (pen !== 'X') return;
      for (let dy = 0; dy < scale; dy++) for (let dx = 0; dx <= scale; dx++)
        expected.add((x * scale + dx) + ',' + (y * scale + dy));
    }));
    return cells.size === expected.size && [...cells].every((cell) => expected.has(cell));
  }
  let glyphCases = 0;
  const fullQuads = {};
  for (const character of chars + '?\u0080\ud800') {
    const inked = Object.prototype.hasOwnProperty.call(bitmaps, character);
    const full = d.hudGlyphGeometry(character, 1, 150, 0);
    const runs = full.words.length / 24;
    fullQuads[character] = runs;
    check(inked ? runs > 0 && runs <= 9 : runs === 0, 'HUD glyph run count');
    if (inked) for (const scale of [1, 2]) {
      const whole = d.hudGlyphGeometry(character, scale, 150, 0);
      check(whole.accepted && coversBitmap(quads(whole.words), scale, character),
        'HUD runs cover exactly the authored bitmap, one pixel bolder');
    }
    for (const scale of [0, .5, 1, 2]) {
      const whole = d.hudGlyphGeometry(character, scale, 150, 0).words;
      for (const available of [0, 1, 3, 6, 150])
        for (const count of [0, 63, 64]) {
          const got = d.hudGlyphGeometry(character, scale, available, count);
          const written = count >= 64 || scale <= 0 ? 0 : Math.min(runs, available);
          const accepted = count < 64 && (!runs || (scale > 0 && runs <= available));
          check(got.accepted === accepted && got.characters === Math.min(64, count + 1),
            'HUD glyph/primitive admission');
          check(got.runsBytes === 393 && got.words.length === written * 24
            && got.words.every((word, at) => Object.is(word, whole[at])),
            'HUD packed run order and vertices');
          glyphCases++;
        }
    }
  }
  check(glyphCases === 2820, 'HUD glyph oracle coverage');
  // The retained HUD shares a translated-vertex budget with box instances:
  // a typical HUD must not need more quads than the former 3x5 font (102).
  const lineQuads = (line) => [...line].reduce((sum, c) => sum + (fullQuads[c] || 0), 0);
  check(['00000', 'A1 F1', 'HP100 L3', 'SAB R/MIN R'].reduce(
    (sum, line) => sum + lineQuads(line), 0) <= 102, 'HUD quad budget');

  function height(arena, x, z) {
    const cx = Math.floor((x + 8) * 2), cz = Math.floor((z + 8) * 2);
    if (cx < 0 || cx >= 32 || cz < 0 || cz >= 32) return 0;
    const at = data.spatial.rampGrids[arena][cz * 32 + cx] - 1;
    if (at < 0) return 0;
    const r = data.arenas[arena].ramps[at];
    if (Math.abs(x - r[0]) > r[2] * .5 || Math.abs(z - r[1]) > r[3] * .5) return 0;
    const local = (z - (r[1] - r[3] * .5)) / r[3];
    return Math.max(0, Math.min(r[4], (r[5] > 0 ? local : 1 - local) * r[4]));
  }
  let sceneryChecks = 0;
  function scenery() {
    const arena = d.snapshot().arena;
    for (const c of d.crateState()) if (c.active) {
      check(Object.is(c.renderY, height(arena, c.x, c.z) + .25), 'cached crate height');
      sceneryChecks++;
    }
    for (const h of d.hazardState()) if (h.active) {
      // MARK_TOP .042 and FLAT_STEP .012 in game.js; .01 is half the slab.
      check(h.level >= 0 && h.level < 3
        && Object.is(h.renderY, height(arena, h.x, h.z) + .042 + h.level * .012 - .01)
        && Object.is(h.renderDiameter, h.radius * 2), 'cached hazard constants');
      sceneryChecks++;
    }
  }
  for (let arena = 0; arena < 3; arena++) {
    d.beginLeague(12345, 1, 2, 1, arena); d.freezeBots(true); scenery();
    for (let at = 0; at < 16; at++) {
      const r = data.arenas[arena].ramps[at % 2];
      d.addHazard(at % 3, r[0], r[1] + (at % 3 - 1) * r[3] * .5);
      scenery();
    }
    const before = d.hazardState();
    d.stepSimulation(1); scenery();
    const after = d.hazardState();
    check(after.some((h, at) => h.active && h.life < before[at].life),
      'hazard fade life stays live');
    const crate = d.crateState().findIndex(c => c.active);
    if (crate >= 0) { d.explodeCrate(crate); scenery(); }
    d.finishLeague();
  }
  d.selectMode(0); d.start(); d.freezeBots(true);
  d.onlineGeneratedSnapshot(7919); scenery();
  let changedHeights = 0;
  for (let seed = 2; seed <= 9; seed++) {
    const r = data.arenas[3].ramps[0];
    d.addHazard(0, r[0], r[1]);
    const before = d.hazardState();
    d.generateArenaSeed(seed * 7919); scenery();
    d.hazardState().forEach((h, at) => {
      if (h.active && h.renderY !== before[at].renderY) changedHeights++;
    });
  }
  check(changedHeights > 0 && sceneryChecks > 300, 'same-index terrain replacement coverage');
  d.selectMode(0); d.start();
  globalThis.pocSummary = 'TREADLINE-RENDER-CACHE';
})();
