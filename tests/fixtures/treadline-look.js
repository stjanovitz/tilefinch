/* Treadline has one look. Its lighting is baked into vertices the game
   already uploads, so it must keep a stable frame shape in every theater;
   the WebGL clear colour, which shows only where the PSP GE drops a backdrop
   triangle, must match the backdrop's upper half under every scenery tint
   and change only when the look does (each change re-captures the retained
   frame command list); a kill's scorch lands after the kill frame; and saves
   written while the retired Look setting existed (they carry an lk field)
   keep loading with their progress and round-trip through a new save. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const P = d.practice;
  function check(value, what) { if (!value) throw new Error(what); }
  /* The backdrop's base colour and its upper half's mid-gradient shade
     (addHorizonBackdrop: shade 1 at the foot to 1.3 at the top), restated
     here as the contract. The old constant clear colour was near-black. */
  const BASE = [-.002, .11, .13], HOLE_SHADE = 1.225, OLD = [.012, .031, .037];
  const gl = document.getElementById('game').getContext('webgl');
  const clamp = (v) => Math.max(0, Math.min(1, v));
  const audited = [];
  function clearMatches(where) {
    const tint = d.getSceneryTint(), clear = gl.getParameter(gl.COLOR_CLEAR_VALUE);
    for (let c = 0; c < 3; c++) {
      const want = clamp(BASE[c] * HOLE_SHADE * tint[c]);
      const low = clamp(BASE[c] * 1.15 * tint[c]), high = clamp(BASE[c] * 1.3 * tint[c]);
      check(Math.abs(clear[c] - want) <= .004 && clear[c] >= low - .004
        && clear[c] <= high + .004,
        `${where}: clear ${Array.from(clear).map(v => v.toFixed(3))} is the backdrop `
        + `(${[0, 1, 2].map(k => clamp(BASE[k] * HOLE_SHADE * tint[k]).toFixed(3))}) under tint `
        + `${tint.map(v => v.toFixed(2))}`);
    }
    audited.push(where);
  }

  C.loadSave(''); C.save.d = 1; C.setRadio(2);
  for (const mission of [0, 13]) {
    C.startMission(mission, false); d.freezeBots(true); d.step(30);
    const s = d.snapshot();
    check(s.meshDrops === 0, `no mesh drops in mission ${mission}`);
    check(s.staticIndices > 0
      && s.staticIndices <= s.arenaMaximumStaticIndexCount,
      `static indices fit the retained draw in mission ${mission}`);
    check(s.arenaPlaneMaxSpan > 0 && s.arenaPlaneMaxSpan <= 4.5,
      `arena planes stay clip-safe in mission ${mission}: ${s.arenaPlaneMaxSpan}`);
    // A Low Battery dim step mid-play moves the clear colour with the
    // tint (one re-capture per step) and nothing else does.
    const tint = B.PALETTE_TINTS[B.preferences.palette], saved = tint.slice();
    let captures = d.snapshot().repeatedFrameCaptures;
    d.step(30);
    check(d.snapshot().repeatedFrameCaptures === captures,
      `steady play keeps the retained frame in mission ${mission}`);
    for (let level = 1; level <= 7; level++) {
      for (let channel = 0; channel < 3; channel++)
        tint[channel] = saved[channel] * (1 - level * .07);
      B.applyPalette(); d.step(2); clearMatches(`mission ${mission} dim ${level}`);
      B.applyPalette(); d.step(2);
    }
    check(d.snapshot().repeatedFrameCaptures - captures <= 7,
      `a dim step re-captures at most once in mission ${mission}`);
    tint.splice(0, 3, ...saved); B.applyPalette(); d.step(2);
    captures = d.snapshot().repeatedFrameCaptures;
    B.applyPalette(); d.step(30);
    check(d.snapshot().repeatedFrameCaptures === captures,
      `re-applying an unchanged look keeps the retained frame in mission ${mission}`);
    // A kill's scorch skips the busy kill frame and lands a moment later.
    const foe = B.tanks.findIndex((tank, id) => id > 0 && tank.active);
    check(foe > 0, `mission ${mission} has a foe`);
    const decals = d.snapshot().decals;
    d.damageTank(foe, 999, B.tanks[foe].x, B.tanks[foe].z - 2);
    check(!B.tanks[foe].active, `the foe is destroyed in mission ${mission}`);
    d.step(1);
    check(d.snapshot().decals === decals, 'no scorch on the kill frame');
    d.step(60);
    const landed = d.snapshot();
    check(landed.decals === decals + 1,
      `the scorch lands after the sparks in mission ${mission}: `
      + `${decals} -> ${landed.decals} (${landed.mode})`);
    d.freezeBots(false);
  }
  // Every look the backdrop can take: the four palettes in every mode
  // (Onslaught and Daily arenas included), the Practice Range, each
  // campaign mission (five theater lights, Whiteout 3-4, Night City 4-1,
  // the 4-5 finale) and the Low Battery fault's start-of-stage dim.
  const defaultClear = (() => { d.setPalette(0); return gl.getParameter(gl.COLOR_CLEAR_VALUE); })();
  check([0, 1, 2].some(c => Math.abs(defaultClear[c] - OLD[c]) > .05),
    'the default clear colour is not the old near-black constant');
  for (let palette = 0; palette < 4; palette++) {
    d.setPalette(palette); clearMatches(`palette ${palette} menu`);
    for (let mode = 0; mode < 7; mode++) {
      d.selectMode(mode); d.selectClass(1); d.start(); d.freezeBots(true);
      d.step(10); clearMatches(`palette ${palette} mode ${mode}`);
    }
    P.begin(0); d.step(10); clearMatches(`palette ${palette} practice range`);
  }
  d.setPalette(2);
  for (let mission = 0; mission < C.missions.length; mission++) {
    C.loadSave(''); C.save.d = 1; C.setRadio(2); C.save.f = 0;
    C.startMission(mission, false); d.freezeBots(true); d.step(4);
    clearMatches(`mission ${C.missions[mission].id}`);
  }
  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.save.f = 32; C.save.k = 0x1ffffff;
  C.startMission(5, false); d.freezeBots(true); d.step(4);
  clearMatches('Low Battery start');
  C.loadSave(''); C.save.f = 0; C.writeSave();
  C.menu.toMain(); d.setPalette(0); clearMatches('palette 0 after the campaign');
  check(audited.length > 60, `audited ${audited.length} looks`);
  globalThis.__treadlineClearAudit = audited.length;

  // Saves written while the Look setting existed still load in full.
  for (const lk of [0, 1, 7]) {
    C.loadSave(JSON.stringify({v: 1, d: 2, k: 5, m: [15, 7], lk}));
    check(!C.save.corrupt && C.save.d === 2 && C.save.k === 5
      && C.save.m[0] === 15 && C.save.m[1] === 7,
      `a save carrying lk=${lk} keeps its progress`);
  }
  // The last one (lk=7) is written back and read again: the progress
  // survives the round trip. localStorage must work here, so a throwing or
  // empty store fails rather than passing vacuously.
  check(C.writeSave() === true, 'the save was stored');
  const written = localStorage.getItem('treadline-campaign-v1');
  check(typeof written === 'string' && written.length > 0, 'the save was written');
  const stored = JSON.parse(written);
  check(stored.d === 2 && stored.k === 5 && stored.m[0] === 15 && stored.m[1] === 7
    && !('lk' in stored), `the rewritten save keeps progress and drops lk: ${written}`);
  C.loadSave(written);
  check(!C.save.corrupt && C.save.d === 2 && C.save.k === 5
    && C.save.m[0] === 15 && C.save.m[1] === 7, 'the rewritten save reloads');
  C.loadSave(''); C.writeSave();
  C.menu.toMain();
  globalThis.pocSummary = 'TREADLINE-LOOK-PASS';
})();
