"use strict";

// The invariant sweep's camera motion detector on synthetic series: it must
// fire on the wall pumping seen on device (distance flipping 1.2 <-> 2.1
// every other frame, and the slower 3.2 -> 3.6 sawtooth) and on a yaw that
// dithers back and forth, and stay quiet on smooth or deliberate motion: a
// single retraction that eases back out, a steady follow turn, hit shake,
// and hard cuts.
const fs = require("node:fs"), vm = require("node:vm");
const assert = require("node:assert/strict"), path = require("node:path");
const context = vm.createContext({});
vm.runInContext(fs.readFileSync(
  path.join(__dirname, "fixtures/treadline-camera-motion.js"), "utf8"), context);
const CM = context.__treadlineCameraMotion;
const DT = 1 / 30, EYE = 5.7, TARGET = .28;

// Feed a distance (and optional yaw/shake) series as the fixture would.
function run(distances, {yaws = null, shakes = null, follow = false, cuts = []} = {}) {
  const track = CM.create();
  distances.forEach((distance, at) => {
    if (cuts.includes(at)) track.cut("arena");
    const yaw = yaws ? yaws[at] : 0;
    const lead = Math.min(1.25, Math.max(.35, distance * .32));
    const pitch = Math.atan2(EYE - TARGET, distance + lead);
    track.sample(at * DT, distance, pitch, yaw, -Math.sin(yaw) * distance,
      -Math.cos(yaw) * distance, Math.sin(yaw) * lead, Math.cos(yaw) * lead,
      shakes ? shakes[at] : 0, follow);
  });
  return track.result();
}
function types(result) { return [...new Set(result.flags.map(f => f.type))].sort(); }
function ease(from, to, steps) {
  const out = [];
  let d = from;
  for (let at = 0; at < steps; at++) { out.push(d); d += (to - d) * Math.min(1, DT * 7); }
  return out;
}
const steady = n => new Array(n).fill(6.4);

// Device pumping: 1.2 <-> 2.13 every other frame for three seconds.
const flip = run([...steady(30), ...Array.from({length: 90}, (_, at) => at & 1 ? 2.13 : 1.2)]);
assert.ok(types(flip).includes("oscillation"), JSON.stringify(flip));
assert.ok(types(flip).includes("re-retraction"), JSON.stringify(flip));
assert.ok(flip.maxDistanceReversals >= 30);

// The probe's five-frame cycle 1.81, 2.82, 1.20, 2.34, 3.23.
const cycle = [1.81, 2.82, 1.2, 2.34, 3.23];
const five = run([...steady(20), ...Array.from({length: 60}, (_, at) => cycle[at % 5])]);
assert.deepEqual(types(five), ["oscillation", "re-retraction"]);

// The slower sawtooth: snap 3.6 -> 3.2, ease back to 3.59, repeat (16 frames).
const saw = [];
for (let k = 0; k < 6; k++) saw.push(...ease(3.2, 3.6, 16));
assert.ok(types(run([...steady(10), ...saw])).includes("re-retraction"));

// Yaw dithering back and forth in follow mode (the wrap at +-pi).
const yawRun = run(steady(90), {yaws: Array.from({length: 90}, (_, at) => 2.7 + .1 * Math.sin(at * 1.6)),
  follow: true});
assert.ok(types(yawRun).includes("jerk") || types(yawRun).includes("oscillation"), JSON.stringify(yawRun));

// Quiet: a single retraction, held, then easing back out.
const single = run([...steady(30), ...new Array(15).fill(1.2), ...ease(1.2, 6.4, 60)]);
assert.equal(single.flags.length, 0, JSON.stringify(single));
assert.equal(single.retractions, 1);
// Quiet: two retractions 2.5 s apart (walls passed one at a time).
const two = run([...steady(20), ...new Array(10).fill(2), ...ease(2, 6.4, 65),
  ...new Array(10).fill(2.4), ...ease(2.4, 6.4, 40)]);
assert.equal(two.flags.length, 0, JSON.stringify(two));
// Quiet: a steady follow turn at the authored .72 rad/s.
const turn = run(steady(120), {yaws: Array.from({length: 120}, (_, at) => at * .024), follow: true});
assert.equal(turn.flags.length, 0, JSON.stringify(turn));
// Quiet: hit shake decaying (shake is reported, never part of the path).
const shake = run(steady(60), {shakes: Array.from({length: 60}, (_, at) => Math.max(0, .22 - at * DT * .55))});
assert.equal(shake.flags.length, 0);
// Quiet: a hard cut (arena change) explains a distance jump.
const cut = run([...steady(30), ...ease(1.2, 6.4, 40)], {cuts: [30]});
assert.equal(cut.flags.length, 0, JSON.stringify(cut));
assert.equal(cut.cuts, 1);
// Flagged: a jump the authored motion never makes (fixed-mode yaw step).
const jump = run(steady(20), {yaws: Array.from({length: 20}, (_, at) => at === 12 ? .4 : 0)});
assert.deepEqual(types(jump), ["jump"]);
// Flagged: shake that never settles.
const unsettled = run(steady(200), {shakes: new Array(200).fill(.1)});
assert.deepEqual(types(unsettled), ["shake"]);
console.log("treadline camera motion detector: ok");
