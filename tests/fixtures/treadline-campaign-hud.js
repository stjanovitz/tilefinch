/* Campaign radio lines must fit the retained HUD toast. The HUD mesh caps
   at 150 primitives and its standard text varies with class, gadget and
   armor digits, so every line keeps a margin beside a measured baseline.
   A truncated toast would reach the cap. Separate from the main campaign
   fixture because each line needs a complete sliced HUD rebuild. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  function check(value, what) { if (!value) throw new Error(what); }
  const lines = new Set(C.missions.map(m => m.bark));
  for (const value of Object.values(C.toasts))
    for (const text of typeof value === 'string' ? [value] : Object.values(value))
      if (text) lines.add(text);
  C.loadSave(''); C.save.d = 1; C.setRadio(2);
  d.setCommandEnabled(false);
  C.startMission(2, false); d.freezeBots(true);
  // The HUD rebuilds in slices; wait until a rebuild has published and the
  // next read agrees.
  function settle() {
    let previous = -1;
    for (let tries = 0; tries < 10; tries++) {
      d.step(8);
      const used = d.snapshot().hudTextPrimitives;
      if (used === previous) return used;
      previous = used;
    }
    return previous;
  }
  B.showToast('', 1); d.step(40);
  const base = settle();
  check(base > 60 && base <= 110, 'standard HUD baseline ' + base);
  for (const text of lines) {
    check(/^[ 0-9A-Z\-\/]{1,20}$/.test(text), 'toast glyphs ' + text);
    // Six or fewer inked glyphs cannot reach the margin; skip their rebuild.
    if (text.replace(/ /g, '').length <= 6) continue;
    B.showToast(text, 30);
    const used = settle();
    check(used <= Math.min(140, base + 37), `toast fits the HUD mesh: ${text} base=${base} got=${used}`);
  }
  d.freezeBots(false);
  C.menu.toMain();
  globalThis.pocSummary = 'TREADLINE-CAMPAIGN-HUD-PASS';
})();
