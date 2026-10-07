/* Camera motion detector for the Treadline invariant sweep.

   The sweep (tests/fixtures/treadline-invariants.js) composes every 1/30 s
   step's camera the way a device frame does and feeds one sample per step
   to a tracker made here. The tracker knows nothing about the game: it sees
   the camera distance, pitch and yaw and the eye and look target relative
   to the player, plus the visible shake, and the caller marks hard cuts
   (arena changes, killcam, respawns, camera-mode switches) where a jump is
   the intended picture.

   It flags, per 1.5 s window:
   - oscillation: repeated direction reversals of distance, pitch or yaw
     (a zig-zag with hysteresis, so easing and noise never count);
   - re-retraction: the camera snapping in, easing back out and snapping in
     again (a wall between eye and player retracts it once; doing it over
     and over is the visible pumping);
   - jump: a per-step change no authored motion makes (yaw beyond the follow
     rate, a look target that leaps without a retraction, an expansion
     faster than its ease);
   - jerk: sustained high third differences of the eye and target offsets
     outside retraction frames;
   - shake: visible shake that never settles.

   A deliberate single retraction, the follow yaw, easing out again and hit
   shake are all legitimate and stay below these limits. The detector is
   pure; tests/test_treadline_camera_motion.js drives it with synthetic
   series. */
(() => {
  "use strict";
  const DEGREE = Math.PI / 180;
  const LIMITS = Object.freeze({
    window: 45,                   // steps (1.5 s at 30 Hz) for every burst
    distanceHysteresis: .15,      // world units a distance swing must reach
    pitchHysteresis: 1.5 * DEGREE,
    yawHysteresis: .08,           // rad
    reversals: 6,                 // three in/out cycles (>= 2 Hz) inside a window
    retraction: .3,               // one-step distance drop that is a retraction
    reExpansion: .25,             // easing out between two retractions
    reRetractions: 2,             // pumps (retract-expand-retract) per window
    followYawStep: .06,           // rad/step; the follow camera turns <= .024
    targetStep: .6,               // look-target leap without a retraction
    distanceRise: 1.4,            // one-step expansion beyond the authored ease
    jerk: .3,                     // |third difference| of eye+target offsets
    jerkSteps: 8,                 // high-jerk steps per window
    shakeSeconds: 5,              // continuous visible shake
    dt: 1 / 30,
  });
  const EPISODE_LIMIT = 6;        // kept per flag type and case

  function round2(value) { return Math.round(value * 100) / 100; }
  function wrap(angle) {
    return Math.atan2(Math.sin(angle), Math.cos(angle));
  }

  // Zig-zag reversal counter with hysteresis over one scalar.
  function zigzag(hysteresis, unwrap) {
    return {hysteresis, unwrap, dir: 0, extreme: 0, low: 0, high: 0, value: 0,
      turn: 0, times: [], swings: [], total: 0};
  }
  function zigzagReset(z, value) {
    z.dir = 0; z.extreme = z.low = z.high = z.value = z.turn = value;
    z.times.length = z.swings.length = 0;
  }
  // Returns the number of reversals inside the trailing window.
  function zigzagPush(z, raw, step, window) {
    const value = z.unwrap ? z.value + wrap(raw - z.value) : raw;
    z.value = value;
    let turned = NaN;  // the turning point a reversal just confirmed
    if (z.dir === 0) {
      if (value < z.low) z.low = value;
      if (value > z.high) z.high = value;
      if (value - z.low >= z.hysteresis) { z.dir = 1; z.turn = z.low; z.extreme = value; }
      else if (z.high - value >= z.hysteresis) { z.dir = -1; z.turn = z.high; z.extreme = value; }
    } else if (z.dir > 0) {
      if (value > z.extreme) z.extreme = value;
      else if (z.extreme - value >= z.hysteresis) { turned = z.extreme; z.dir = -1; z.extreme = value; }
    } else {
      if (value < z.extreme) z.extreme = value;
      else if (value - z.extreme >= z.hysteresis) { turned = z.extreme; z.dir = 1; z.extreme = value; }
    }
    if (turned === turned) {
      // The completed swing runs from the previous turning point to this one.
      z.swings.push(Math.abs(turned - z.turn));
      z.turn = turned; z.times.push(step); z.total++;
    }
    while (z.times.length && z.times[0] <= step - window) { z.times.shift(); z.swings.shift(); }
    return z.times.length;
  }
  // Median completed swing inside the window (the oscillation's amplitude).
  function zigzagSwing(z) {
    if (!z.swings.length) return 0;
    const sorted = z.swings.slice().sort((a, b) => a - b);
    return sorted[sorted.length >> 1];
  }

  /* trace (optional, for probes): {from, to, rows} collects
     [t, distance, pitch degrees, yaw, ex, ez, tx, tz, shake] per sample
     between from and to seconds. */
  function create(limits = LIMITS, trace = null) {
    const W = limits.window, DT = limits.dt;
    const history = new Float64Array(4 * 4);   // last four [ex, ez, tx, tz]
    const recent = new Float64Array(W);        // last W distances for excerpts
    const zd = zigzag(limits.distanceHysteresis, false);
    const zp = zigzag(limits.pitchHysteresis, false);
    const zy = zigzag(limits.yawHysteresis, true);
    const pumps = [], jerkSteps = [];
    const episodes = [], open = Object.create(null);
    const cuts = Object.create(null);
    let step = -1, segment = 0, time = 0, follow = false;
    let previousDistance = 0, previousPitch = 0, previousYaw = 0;
    let previousTX = 0, previousTZ = 0, lastRetraction = -1e9, expanded = 0;
    let shakeRun = 0;
    const m = {samples: 0, cuts: 0, retractions: 0, reRetractions: 0,
      distanceReversals: 0, pitchReversals: 0, yawReversals: 0,
      maxDistanceReversals: 0, maxPitchReversals: 0, maxYawReversals: 0,
      maxReRetractions: 0, maxJerkSteps: 0, maxJerk: 0, maxRise: 0,
      maxYawStep: 0, maxTargetStep: 0, maxShakeSeconds: 0, minDistance: 1e9,
      maxDistance: 0};

    function excerpt() {
      const count = Math.min(segment, W, 24), out = [];
      for (let k = count; k > 0; k--) out.push(Math.round(recent[(step - k + 1 + W * 4) % W] * 100) / 100);
      return out.join(",");
    }
    // One episode per type while it keeps firing within a window.
    function flag(type, what, peak, detail) {
      const key = type + "|" + what, at = open[key];
      if (at && step - at.lastStep <= W) {
        at.lastStep = step; at.end = Math.round(time * 100) / 100;
        if (peak > at.peak) { at.peak = peak; at.excerpt = excerpt(); Object.assign(at, detail); }
        return;
      }
      const same = episodes.filter(e => e.type === type).length;
      const entry = {type, what, t: Math.round(time * 100) / 100,
        end: Math.round(time * 100) / 100, peak, lastStep: step, excerpt: excerpt(), ...detail};
      open[key] = entry;
      if (same < EPISODE_LIMIT) episodes.push(entry);
      else { entry.dropped = true; m.droppedEpisodes = (m.droppedEpisodes || 0) + 1; }
    }

    return {
      // A hard cut: the next sample starts a new, unconstrained segment.
      cut(reason) {
        if (segment) { m.cuts++; cuts[reason] = (cuts[reason] || 0) + 1; }
        segment = 0; jerkSteps.length = 0; pumps.length = 0;
        lastRetraction = -1e9; expanded = 0;
      },
      /* t seconds; distance (eye behind the player); pitch below horizon
         (rad); yaw (rad); eye and look-target offsets from the player;
         visible shake amplitude; followMode when the yaw follows the hull. */
      sample(t, distance, pitch, yaw, ex, ez, tx, tz, shake, followMode) {
        step++; time = t; follow = !!followMode; m.samples++;
        if (trace && t >= trace.from && t <= trace.to)
          trace.rows.push([t, distance, pitch / DEGREE, yaw, ex, ez, tx, tz, shake]
            .map(v => Math.round(v * 1000) / 1000));
        recent[step % W] = distance;
        if (distance < m.minDistance) m.minDistance = distance;
        if (distance > m.maxDistance) m.maxDistance = distance;
        // Shake is presentation only; it must settle between hits.
        shakeRun = shake > 1e-6 ? shakeRun + DT : 0;
        if (shakeRun > m.maxShakeSeconds) m.maxShakeSeconds = shakeRun;
        if (shakeRun > limits.shakeSeconds) flag("shake", "unsettled", shakeRun, {});
        const slot = (step & 3) * 4;
        history[slot] = ex; history[slot + 1] = ez; history[slot + 2] = tx; history[slot + 3] = tz;
        if (!segment) {
          segment = 1;
          zigzagReset(zd, distance); zigzagReset(zp, pitch); zigzagReset(zy, yaw);
          previousDistance = distance; previousPitch = pitch; previousYaw = yaw;
          previousTX = tx; previousTZ = tz;
          return;
        }
        segment++;
        const change = distance - previousDistance;
        const retracted = change < -limits.retraction;
        // Retractions, and the pumping of retract-expand-retract.
        if (retracted) {
          m.retractions++;
          if (step - lastRetraction <= W && expanded >= limits.reExpansion) {
            m.reRetractions++;
            pumps.push(step);
          }
          lastRetraction = step; expanded = 0;
        } else if (change > 0) expanded += change;
        while (pumps.length && pumps[0] <= step - W) pumps.shift();
        if (pumps.length > m.maxReRetractions) m.maxReRetractions = pumps.length;
        if (pumps.length >= limits.reRetractions)
          flag("re-retraction", "distance", pumps.length, {distance: Math.round(distance * 100) / 100});
        // Oscillation: reversals of distance, pitch and yaw.
        const dr = zigzagPush(zd, distance, step, W), pr = zigzagPush(zp, pitch, step, W);
        const yr = zigzagPush(zy, yaw, step, W);
        m.distanceReversals = zd.total; m.pitchReversals = zp.total; m.yawReversals = zy.total;
        if (dr > m.maxDistanceReversals) m.maxDistanceReversals = dr;
        if (pr > m.maxPitchReversals) m.maxPitchReversals = pr;
        if (yr > m.maxYawReversals) m.maxYawReversals = yr;
        if (dr >= limits.reversals)
          flag("oscillation", "distance", dr, {swing: round2(zigzagSwing(zd))});
        if (pr >= limits.reversals)
          flag("oscillation", "pitch", pr, {swingDegrees: round2(zigzagSwing(zp) / DEGREE)});
        if (yr >= limits.reversals)
          flag("oscillation", "yaw", yr, {swingDegrees: round2(zigzagSwing(zy) / DEGREE), follow});
        /* Jumps no authored motion makes. The first steps after a cut
           compose from the state before it, and the look target's lead
           follows the distance one step late, so a retraction explains a
           target leap on its own step and the next. */
        const settled = segment > 3;
        const yawStep = Math.abs(wrap(yaw - previousYaw));
        if (settled && yawStep > m.maxYawStep) m.maxYawStep = yawStep;
        if (settled && (follow ? yawStep > limits.followYawStep : yawStep > 1e-6))
          flag("jump", "yaw", yawStep, {follow});
        if (settled && change > m.maxRise) m.maxRise = change;
        if (settled && change > limits.distanceRise) flag("jump", "expansion", change, {});
        const targetStep = Math.hypot(tx - previousTX, tz - previousTZ);
        const explained = !settled || step - lastRetraction <= 1;
        if (!explained && targetStep > m.maxTargetStep) m.maxTargetStep = targetStep;
        if (!explained && targetStep > limits.targetStep)
          flag("jump", "target", targetStep, {});
        // Jerk of the camera relative to the player, away from retractions
        // (a retraction is a deliberate discontinuity) and cuts.
        if (segment >= 4 && step - lastRetraction > 3) {
          let sum = 0;
          for (let c = 0; c < 4; c++) {
            const j = history[slot + c] - 3 * history[((step - 1) & 3) * 4 + c]
              + 3 * history[((step - 2) & 3) * 4 + c] - history[((step - 3) & 3) * 4 + c];
            sum += j * j;
          }
          const jerk = Math.sqrt(sum);
          if (jerk > m.maxJerk) m.maxJerk = jerk;
          if (jerk > limits.jerk) jerkSteps.push(step);
        }
        while (jerkSteps.length && jerkSteps[0] <= step - W) jerkSteps.shift();
        if (jerkSteps.length > m.maxJerkSteps) m.maxJerkSteps = jerkSteps.length;
        if (jerkSteps.length >= limits.jerkSteps) flag("jerk", "offset", jerkSteps.length, {});
        previousDistance = distance; previousPitch = pitch; previousYaw = yaw;
        previousTX = tx; previousTZ = tz;
      },
      result() {
        const round = v => Math.round(v * 1000) / 1000;
        const out = {};
        for (const [name, value] of Object.entries(m)) out[name] = round(value);
        if (!m.samples) out.minDistance = 0;
        out.cutReasons = {...cuts};
        out.flags = episodes.map(e => {
          const copy = {...e};
          delete copy.lastStep; delete copy.dropped;
          copy.peak = round(copy.peak);
          return copy;
        });
        return out;
      },
    };
  }

  globalThis.__treadlineCameraMotion = Object.freeze({LIMITS, create});
})();
