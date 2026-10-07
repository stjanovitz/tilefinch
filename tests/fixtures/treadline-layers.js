/* Flat things on the floor and the bars above tanks write depth in the one
   instanced box draw, so two faces that overlap on screen must be far
   enough apart in 16-bit depth that rounding never flips which one wins.
   With equal or near-equal planes the PSP GE flips whole triangles as the
   camera moves: decals over decals, tread marks under the tank shadow, the
   tank shadow on an obstacle's contact shadow, mines dropped on mines,
   aim-guide legs that retrace each other, the two health bars.

   This captures the instance stream and camera the game uploads, builds
   every box face, and for each pair of parallel faces that overlap and face
   the camera measures their separation in 16-bit depth steps at the actual
   camera (the viewProjection uniform), at the overlap's corners and centre.
   Flat tops are also measured against the static floor layers under them
   (grid lines at .014 and contact shadows at .03, built once in
   buildArena). Two scenes: the marks around the player, and the same marks
   near the far wall. Every pair must keep MIN_STEPS (game.js FLAT_STEP).

   The second half checks decal eviction under instance pressure: sparks
   coming and going change the room left for decals every frame; a decal
   the cap drops must not come back (it used to blink back for a frame). */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  function check(value, what) { if (!value) throw new Error(what); }
  const MIN_STEPS = 2;
  const STATIC_LAYERS = [.014, .03];
  const gl = document.getElementById('game').getContext('webgl');
  /* The game's last eight float uploads, as views: the instance stores are
     persistent arrays read before the next frame. Copying every upload ran
     the fixture out of the features lane's 7.5 MiB realm. */
  const uploads = [];
  let camera = null;
  const originalUpload = gl.bufferSubData, originalUniform = gl.uniformMatrix4fv;
  gl.bufferSubData = function (target, offset, data) {
    if (data && data.byteLength && data.byteLength % 16 === 0 && data.byteOffset % 4 === 0) {
      if (uploads.length === 8) uploads.shift();
      uploads.push(new Float32Array(data.buffer, data.byteOffset, data.byteLength / 4));
    }
    return originalUpload.apply(this, arguments);
  };
  // publishCamera uploads viewProjection to both programs every frame.
  const cameraValue = new Float32Array(16);
  gl.uniformMatrix4fv = function (location, transpose, value) {
    if (value && value.length === 16) { cameraValue.set(value); camera = cameraValue; }
    return originalUniform.apply(this, arguments);
  };
  /* uploadBoxInstances sends the matrices (16 floats each) and then the
     tints (4 floats each) of the same high-water view: the last such pair. */
  function boxes() {
    const count = d.snapshot().boxInstances;
    for (let at = uploads.length - 1; at > 0; at--) {
      const matrices = uploads[at - 1], tints = uploads[at];
      if (matrices.length !== tints.length * 4 || tints.length < count * 4) continue;
      const found = [];
      for (let i = 0; i < count; i++)
        found.push({m: matrices.subarray(i * 16, i * 16 + 16), t: tints.subarray(i * 4, i * 4 + 4)});
      return found;
    }
    throw new Error('no instance upload captured');
  }
  function frame() { uploads.length = 0; camera = null; d.step(1); check(camera, 'camera'); return boxes(); }

  // ---- geometry ----
  const BOX = [
    -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,   1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,
    -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,  -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,
     1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,  -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1];
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  function face(vs, box, label) {
    let n = cross(sub(vs[1], vs[0]), sub(vs[2], vs[0]));
    const length = Math.hypot(n[0], n[1], n[2]);
    if (length < 1e-9) return null;
    n = n.map((v) => v / length);
    return {vs, n, c: dot(n, vs[0]), box, label};
  }
  // Only faces turned to the eye: steps() needs both faces of a pair so.
  function boxFaces(m, box, label, eye, out) {
    for (let f = 0; f < 6; f++) {
      const vs = [];
      for (let k = 0; k < 4; k++) {
        const s = (f * 4 + k) * 3, x = BOX[s], y = BOX[s + 1], z = BOX[s + 2];
        vs.push([m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
          m[2] * x + m[6] * y + m[10] * z + m[14]]);
      }
      const made = face(vs, box, label + ':' + f);
      if (made && dot(made.n, eye) > made.c) out.push(made);
    }
  }
  // The eye: the point viewProjection sends to x = y = w = 0 (Cramer's rule).
  function eyeOf(vp) {
    const row = (r) => [vp[r], vp[4 + r], vp[8 + r], -vp[12 + r]];
    const a = row(0), b = row(1), c = row(3);
    const det = (p, q, s) => p[0] * (q[1] * s[2] - q[2] * s[1])
      - p[1] * (q[0] * s[2] - q[2] * s[0]) + p[2] * (q[0] * s[1] - q[1] * s[0]);
    const D = det(a, b, c);
    return [det([a[3], a[1], a[2]], [b[3], b[1], b[2]], [c[3], c[1], c[2]]) / D,
      det([a[0], a[3], a[2]], [b[0], b[3], b[2]], [c[0], c[3], c[2]]) / D,
      det([a[0], a[1], a[3]], [b[0], b[1], b[3]], [c[0], c[1], c[3]]) / D];
  }
  function project(vp, p) {
    const w = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    return [(vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12]) / w,
      (vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13]) / w,
      (vp[2] * p[0] + vp[6] * p[1] + vp[10] * p[2] + vp[14]) / w, w];
  }
  function clip(subject, clipper) {
    let area = 0;
    for (let i = 0; i < clipper.length; i++) {
      const p = clipper[i], q = clipper[(i + 1) % clipper.length];
      area += p[0] * q[1] - q[0] * p[1];
    }
    const sign = area >= 0 ? 1 : -1;
    let out = subject;
    for (let i = 0; i < clipper.length && out.length; i++) {
      const a = clipper[i], b = clipper[(i + 1) % clipper.length];
      const inside = (p) => sign * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) >= -1e-9;
      const input = out; out = [];
      for (let j = 0; j < input.length; j++) {
        const p = input[j], q = input[(j + 1) % input.length], pin = inside(p), qin = inside(q);
        if (pin) out.push(p);
        if (pin === qin) continue;
        const dx = q[0] - p[0], dy = q[1] - p[1], ex = b[0] - a[0], ey = b[1] - a[1];
        const den = ex * dy - ey * dx;
        if (Math.abs(den) < 1e-12) continue;
        const s = (ex * (a[1] - p[1]) - ey * (a[0] - p[0])) / den;
        out.push([p[0] + dx * s, p[1] + dy * s]);
      }
    }
    return out;
  }
  /* The fewest 16-bit depth steps between faces a and b where they overlap
     on screen, or null when they are not parallel, close, overlapping and
     both turned to the camera. */
  function steps(a, b, vp, eye) {
    const parallel = dot(a.n, b.n);
    if (parallel < .9995 || Math.abs(a.c - b.c) > .08) return null;
    const t = Math.abs(a.n[1]) < .9 ? [0, 1, 0] : [1, 0, 0];
    let u = cross(a.n, t); const lu = Math.hypot(u[0], u[1], u[2]); u = u.map((v) => v / lu);
    const v = cross(a.n, u);
    const flat = (f) => f.vs.map((p) => [dot(p, u), dot(p, v)]);
    const overlap = clip(flat(a), flat(b));
    if (overlap.length < 3) return null;
    let area = 0, cx = 0, cy = 0;
    for (let i = 0; i < overlap.length; i++) {
      const p = overlap[i], q = overlap[(i + 1) % overlap.length];
      area += p[0] * q[1] - q[0] * p[1]; cx += p[0]; cy += p[1];
    }
    if (Math.abs(area) * .5 < 1e-4) return null;
    let fewest = Infinity;
    for (const s of [[cx / overlap.length, cy / overlap.length], ...overlap]) {
      const P = [0, 1, 2].map((k) => a.n[k] * a.c + u[k] * s[0] + v[k] * s[1]);
      const ray = sub(P, eye), den = dot(b.n, ray);
      if (Math.abs(den) < 1e-9 || dot(a.n, sub(eye, P)) <= 0) continue;
      const at = (b.c - dot(b.n, eye)) / den;
      const Q = [0, 1, 2].map((k) => eye[k] + ray[k] * at);
      const pa = project(vp, P), pb = project(vp, Q);
      if (pa[3] <= .25 || Math.abs(pa[0]) > 1 || Math.abs(pa[1]) > 1) continue;
      fewest = Math.min(fewest, Math.abs(pa[2] - pb[2]) * .5 * 65535);
    }
    return fewest === Infinity ? null : fewest;
  }
  const label = (m) => `box at (${m[12].toFixed(2)}, ${m[13].toFixed(3)}, ${m[14].toFixed(2)})`
    + ` half (${Math.hypot(m[0], m[2]).toFixed(3)}, ${m[5].toFixed(4)}, ${Math.hypot(m[8], m[10]).toFixed(3)}) face`;
  function audit(where) {
    const list = frame(), vp = camera, eye = eyeOf(vp);
    const faces = [];
    list.forEach((b, i) => boxFaces(b.m, i, '', eye, faces));
    const name = (f) => f.box < 0 ? 'static' : label(list[f.box].m) + f.label;
    let pairs = 0, fewest = Infinity;
    for (let i = 0; i < faces.length; i++) {
      for (let j = i + 1; j < faces.length; j++) {
        if (faces[i].box === faces[j].box) continue;
        const s = steps(faces[i], faces[j], vp, eye);
        if (s === null) continue;
        pairs++; fewest = Math.min(fewest, s);
        if (s < MIN_STEPS) check(false, `${where}: ${name(faces[i])} (box ${faces[i].box}) and `
          + `${name(faces[j])} (box ${faces[j].box}) are ${s.toFixed(2)} depth steps apart`);
      }
      // A flat top over the static floor layers.
      const f = faces[i];
      if (f.n[1] < .9999 || f.c > .2) continue;
      for (const y of STATIC_LAYERS) {
        const floor = face(f.vs.map((p) => [p[0], y, p[2]]).reverse(), -1, 'static');
        const under = {...floor, n: [0, 1, 0], c: y};
        const s = steps(f, under, vp, eye);
        if (s === null) continue;
        pairs++; fewest = Math.min(fewest, s);
        if (s < MIN_STEPS) check(false, `${where}: ${name(f)} (box ${f.box}) is ${s.toFixed(2)}`
          + ` depth steps above the static layer at ${y}`);
      }
    }
    return {pairs, fewest};
  }

  C.loadSave(''); C.save.d = 1; C.setRadio(2); C.writeSave();
  C.menu.show('quick'); d.selectMode(0); d.start(); d.freezeBots(true);
  for (let id = 1; id < 6; id++) d.setTankActive(id, false);
  d.clearGadgetEffects(); d.setSparks(0);
  const player = B.tanks[0];
  /* One cluster of marks: tread marks under where the tank will park,
     scorches piled on one spot, a hazard over them, mines dropped on each
     other. */
  function marks(cx, cz) {
    for (let k = 0; k < 3; k++) d.addDecal(1, cx - .1 + k * .1, cz - .38 - k * .3, 0, .38, .23);
    for (const [x, z] of [[0, 1.6], [.2, 1.7], [-.15, 1.85], [.1, 1.95]])
      d.addDecal(2, cx + x, cz + z, 0, .48, .38);
    d.addHazard(0, cx + .9, cz + 1.8);
    for (const [x, z] of [[-1.2, 1.2], [-1, 1.3], [-1.35, 1.4]]) {
      d.setTankPosition(0, cx + x, cz + z + .72); d.setTankHeading(0, 0);
      d.activateGadget('MINES');
    }
  }
  let total = 0, fewest = Infinity;
  // Near: the player parked on its own tread marks, half its health gone,
  // the aim guide on (Full) and pointed at the scorch pile.
  d.setAimGuide(2);
  marks(0, -4);
  player.health = player.maxHealth * .5;
  for (let at = 0; at < 30; at++) {
    d.setTankPosition(0, at * .01 - .15, -4); d.setTankHeading(0, 0);
    const r = audit(`near frame ${at}`); total += r.pairs; fewest = Math.min(fewest, r.fewest);
  }
  // Far: the same marks by the north wall, seen from the south side.
  d.clearGadgetEffects();
  marks(.5, 5.4);
  for (let at = 0; at < 30; at++) {
    d.setTankPosition(0, at * .01 - .15, -6.8); d.setTankHeading(0, 0);
    const r = audit(`far frame ${at}`); total += r.pairs; fewest = Math.min(fewest, r.fewest);
  }
  check(total >= 1000, `overlapping face pairs were measured (${total})`);

  /* Eviction: twelve decals, then mines (drawn before decals) until the
     decals only just fit; six sparks coming and going on alternate frames
     then take room from the oldest decals. A decal dropped for room is
     retired, never drawn again. */
  d.clearGadgetEffects(); d.setSparks(0); d.setAimGuide(0);
  player.health = player.maxHealth;
  for (let at = 0; at < 12; at++)
    d.addDecal(at & 1, -5 + (at % 6) * 1.7, 5.3 + ((at / 6) | 0) * .4, 0);
  for (let at = 0; at < 12; at++) {
    const s = d.snapshot();
    if (s.boxInstances >= s.frameInstanceCeiling - 2) break;
    d.setTankPosition(0, -5 + at * .8, -6 + .72); d.setTankHeading(0, 0);
    d.activateGadget('MINES');
  }
  d.setTankPosition(0, 0, -4);
  const decalKey = (b) => `${b.m[12].toFixed(3)},${b.m[14].toFixed(3)}`;
  // Decal boxes: FLAT_STEP thick now, .018 before the layers.
  const isDecal = (b) => Math.abs(b.m[5] - .006) < 1e-5 || Math.abs(b.m[5] - .009) < 1e-5;
  let previous = null, dropped = 0, frames = 0;
  for (let at = 0; at < 40; at++) {
    d.setSparks(at & 1 ? 6 : 0, 2, -2);
    const drawn = new Set(frame().filter(isDecal).map(decalKey));
    if (previous) {
      for (const key of drawn)
        check(previous.has(key), `frame ${at}: decal ${key} came back after being dropped`);
      dropped += previous.size - drawn.size;
    }
    previous = drawn; frames++;
  }
  check(dropped > 0, 'sparks pushed decals out of the instance budget');
  gl.bufferSubData = originalUpload; gl.uniformMatrix4fv = originalUniform;
  d.clearGadgetEffects(); d.setSparks(0);
  C.menu.toMain();
  globalThis.pocSummary = 'TREADLINE-LAYERS-PASS';
  globalThis.__treadlineLayersReport = `pairs=${total} fewest=${fewest.toFixed(2)} dropped=${dropped}`;
})();
