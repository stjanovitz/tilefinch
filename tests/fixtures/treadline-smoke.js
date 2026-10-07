/* Smoke clouds are translucent boxes drawn in the one instanced box draw,
   and like everything in it they write depth. Every cloud breathes in step,
   so two overlapping clouds of one size share their top face (and a side
   face when their centres line up). The later cloud's face is then tested
   against the earlier one's depth on the same plane, rounding picks the
   winner, and on the PSP GE whole triangles of the overlap flip in and out
   as the camera moves. This captures the instance stream the game uploads
   and requires that clouds which overlap never share a top or side plane:
   for every pair, every frame of a full breath, those planes stay at least
   MARGIN apart (about four 16-bit depth steps at the arena's far side when
   seen at the camera's angle; the game's per-slot inset is .014). */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  function check(value, what) { if (!value) throw new Error(what); }
  const MARGIN = .01;
  // SMOKE_LOW and the cloud alpha in game.js identify a cloud's instance.
  const SMOKE = [.34, .38, .39, .42];
  const gl = document.getElementById('game').getContext('webgl');
  const uploads = [];
  const original = gl.bufferSubData;
  gl.bufferSubData = function (target, offset, data) {
    if (data && data.byteLength && data.byteLength % 16 === 0)
      uploads.push(new Float32Array(data.buffer.slice(
        data.byteOffset, data.byteOffset + data.byteLength)));
    return original.apply(this, arguments);
  };
  /* uploadBoxInstances sends the matrices (16 floats each) and then the
     tints (4 floats each) of the same high-water view: the last such pair. */
  function clouds() {
    const count = d.snapshot().boxInstances;
    for (let at = uploads.length - 1; at > 0; at--) {
      const matrices = uploads[at - 1], tints = uploads[at];
      if (matrices.length !== tints.length * 4 || tints.length < count * 4) continue;
      const found = [];
      for (let i = 0; i < count; i++) {
        const t = tints.subarray(i * 4, i * 4 + 4);
        if (SMOKE.some((v, c) => Math.abs(t[c] - v) > 1e-5)) continue;
        const m = matrices.subarray(i * 16, i * 16 + 16);
        check(Math.abs(m[2]) < 1e-6 && Math.abs(m[8]) < 1e-6, 'clouds are axis-aligned');
        found.push({x: m[12], y: m[13], z: m[14], hw: m[0], hh: m[5], hd: m[10]});
      }
      return found;
    }
    throw new Error('no instance upload captured');
  }
  const apart = (a, b) => Math.abs(a - b) >= MARGIN;
  function audit(where, expected) {
    uploads.length = 0;
    d.step(1);
    const list = clouds();
    check(list.length === expected, `${where}: ${list.length} clouds drawn, ${expected} active`);
    let pairs = 0;
    for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
      const a = list[i], b = list[j];
      if (Math.abs(a.x - b.x) >= a.hw + b.hw || Math.abs(a.z - b.z) >= a.hd + b.hd) continue;
      pairs++;
      const what = `${where}: clouds at (${a.x.toFixed(2)}, ${a.z.toFixed(2)}) and `
        + `(${b.x.toFixed(2)}, ${b.z.toFixed(2)})`;
      check(apart(a.y + a.hh, b.y + b.hh), `${what} share a top plane`
        + ` (${(a.y + a.hh).toFixed(4)} vs ${(b.y + b.hh).toFixed(4)})`);
      for (const side of [-1, 1]) {
        check(apart(a.x + side * a.hw, b.x + side * b.hw), `${what} share an x side plane`);
        check(apart(a.z + side * a.hd, b.z + side * b.hd), `${what} share a z side plane`);
      }
    }
    return pairs;
  }

  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  C.menu.show('quick'); d.selectMode(0); d.start(); d.freezeBots(true);
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  d.clearGadgetEffects();
  /* Six clouds (every slot): two pairs that share a centre, so every plane
     would coincide, and two more overlapping them off-centre. */
  const drops = [[0, -5.8], [.7, -5.2], [0, -5.8], [.7, -5.2], [-.5, -6.3], [.35, -5.5]];
  for (const [x, z] of drops) { d.setTankPosition(0, x, z); d.activateGadget('SMOKE'); }
  check(d.snapshot().smoke === drops.length, 'every slot holds a cloud');
  d.setTankPosition(0, 0, -8.6);
  // One breath is pi seconds: 190 steps of 1/60 s cover it.
  let pairs = 0;
  for (let frame = 0; frame < 190; frame++) pairs += audit(`frame ${frame}`, drops.length);
  check(pairs >= 190 * 9, `overlapping pairs were audited (${pairs})`);
  gl.bufferSubData = original;
  d.clearGadgetEffects();
  C.menu.toMain();
  globalThis.pocSummary = 'TREADLINE-SMOKE-PASS';
})();
