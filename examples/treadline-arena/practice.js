/* Treadline Arena: Practice Range.

   A calm range for learning controls and shots: nothing shoots back, the
   player takes no damage, and there is no timer or game over. Loaded
   before campaign.js, which forwards its arena/tick/damage hooks here while
   the range is open and lends its menu (screens "practice", "range" and
   "drilldone"). game.js draws the shared aim guide at this range's own
   level (aimLevel: Off until toggled on the Range panel).

   The range is Survival on the generated-arena slot, filled from the
   authored layout below with the slot's fixed shape (8 walls, 2 ramps), so
   no new geometry, texture or draw call exists. Targets are the five bot
   tanks with their AI switched off (tank.inert) and no cannon. Practice
   runs never record replays (see the README). Nothing here allocates per
   frame. */
(() => {
  "use strict";
  // ------------------------------------------------------------ layout --
  // [x, z, width, depth]; z grows away from the spawn, walls stand at 7.85.
  const WALLS = [
    [-2.25, 2.6, .5, 8.8], [2.25, 2.6, .5, 8.8], // target lane dividers
    [-5.3, .4, 3.2, .5], [-3.9, 4.6, .5, 2.4], [-6.3, 5, 1.6, .5], // bank yard
    [-4.6, -3, 2.4, .5], [0, 7.45, 1.4, .5], // bank-yard lip, lane backstop
    [6.15, 1.6, .5, 7.4]]; // slalom return lane
  const RAMPS = [[-6.6, -5.4, 1.5, 2.4, .62, 1], [4.6, 6.8, 1.6, 1.6, .5, -1]];
  const POLES = [[4, -1], [4, 1.8], [4, 4.4]];
  const ICE = [[6.6, -3.5, 1.15], [5, -4.7, 1]];
  const LANE_Z = [-1, 2.6, 6.2], LANE_X = [.8, -.7, .4];
  const BANK_SPOTS = [[-5.3, 1.9], [-6.8, 6.7], [-3.15, 7.1], [-5, 3.2]];
  // Slalom gates: x, z and the cue shown when the gate is taken.
  const COURSE = [[3.1, -3.6, ""], [5, -1, ""], [3.1, 1.8, ""],
    [5, 4.4, "PIVOT"], [7.1, 6.7, "LUNGE"], [7.1, 1, "DRIFT"],
    [5.6, -4.2, ""], [1.6, -5.4, ""]];
  const SPAWN_Z = -5.6, LANE_HP = 60, BANK_HP = 18, RESPAWN = 1.4;
  const SWING = 1.25, SPEEDS = [.55, .75, .95];
  const DRILLS = [
    {n: "Free range", note: "Targets at three distances, a bank yard and a slalom track. Nothing shoots back.", keys: []},
    {n: "Slalom", note: "Weave the poles, pivot at the corner, lunge down the lane, drift on the ice.", keys: ["pivot", "lunge"]},
    {n: "Bank shots", note: "Down 4 cyan targets. Only a shell off a wall gets through.", keys: ["aim", "turn turret", "fire"], goal: 4},
    {n: "Moving targets", note: "Down 5 drifting targets: aim where they will be.", keys: ["aim", "turn turret", "fire"], goal: 5},
  ];
  const BEHAVIOURS = ["Still", "Moving", "Off"];
  const ORANGE = [1, .5, .18];

  // -------------------------------------------------------------- save --
  /* Lives in the campaign save as {v, t: best drill tenths, s: lifetime
     shots, hits, bank hits}. An unknown version or a malformed field resets
     only this section; the campaign keeps its progress. */
  const PRACTICE_VERSION = 1;
  const rec = {t: [0, 0, 0], s: [0, 0, 0], corrupt: false};
  const count = (v, hi) => Number.isInteger(v) && v >= 0 && v <= hi ? v : -1;
  function load(raw) {
    rec.t = [0, 0, 0]; rec.s = [0, 0, 0]; rec.corrupt = false;
    if (raw === undefined || raw === null) return rec;
    if (typeof raw !== "object" || raw.v !== PRACTICE_VERSION
        || !Array.isArray(raw.t) || !Array.isArray(raw.s)) {
      rec.corrupt = true; return rec;
    }
    const t = raw.t.map(v => count(v, 36000)), s = raw.s.map(v => count(v, 1e9));
    if (t.length !== 3 || s.length !== 3 || t.includes(-1) || s.includes(-1)
        || s[1] > s[0] || s[2] > s[1]) {
      rec.corrupt = true; return rec;
    }
    rec.t = t; rec.s = s; return rec;
  }
  function saved() { return {v: PRACTICE_VERSION, t: rec.t.slice(), s: rec.s.slice()}; }

  // ----------------------------------------------------------- runtime --
  let B = null, kit = null;
  /* aim: the range's aim guide level (0 Off, 1 Sight, 2 Full) for this
     session; it starts Off and the Range panel toggles it. The range has
     no score, so it costs nothing here. */
  const R = {on: false, drill: 0, behaviour: 0, aim: 0, t: 0,
    greeted: false, kind: new Int8Array(6), spot: new Int8Array(6),
    respawnAt: new Float32Array(6), phase: new Float32Array(6), side: new Int8Array(6),
    pop: 0, shots0: 0, shots: 0, hits: 0, bankHits: 0, downs: 0, gate: 0,
    nextSpot: 0, done: false, result: null, barkAt: 0, quickMode: 0,
    markerX: 0, markerZ: 0};
  const KIND_NONE = 0, KIND_LANE = 1, KIND_MOVING = 2, KIND_BANK = 3;

  function bark(text, hold = .9) {
    if (B.preferences.command && R.t < R.barkAt) return;
    B.showToast(text, hold); R.barkAt = R.t + .5;
  }

  /* Fill the generated slot with the range. Same shape the boot pass
     materialised (8 walls, 2 ramps), so the static tail is refreshed in
     place; enemies stays 5 for beginArena's generic placement. */
  function writeLayout() {
    const arena = globalThis.__treadlineArenaData.generatedArena;
    for (let at = 0; at < 8; at++) arena.obstacles[at].set(WALLS[at]);
    for (let at = 0; at < 2; at++) arena.ramps[at].set(RAMPS[at]);
    for (let at = 0; at < POLES.length; at++)
      arena.barriers[at].set([POLES[at][0], POLES[at][1], .5, .5]);
    arena.obstacleCount = 8; arena.rampCount = 2;
    arena.barrierCount = POLES.length; arena.gateCount = 0; arena.enemies = 5;
    B.state.arenaSeed = 0;
    B.fillArenaSpatialData(3); B.dirty(true);
  }

  function enter(drill = 0) {
    const st = B.state;
    if (st.mode !== "title" && st.mode !== "victory" && st.mode !== "game-over"
        && st.mode !== "replay-done" && !R.on) B.setMode("title");
    kit.disarm();
    if (!R.on) R.quickMode = st.gameMode;
    R.on = true; R.drill = drill; R.done = false;
    st.gameMode = B.MODE_SURVIVAL; st.dailyDay = 0;
    B.requestPresentation();
    writeLayout();
    if (!B.resetGame()) return false;
    B.noReplay();
    B.beginArena(3);
    R.shots0 = st.shots; R.shots = R.hits = R.bankHits = 0;
    kit.show("game", false);
    B.setMode("playing");
    return true;
  }
  function leave() {
    if (!R.on) return;
    R.on = false; R.done = false;
    flush();
    B.restoreTankTints();
    const arena = globalThis.__treadlineArenaData.generatedArena;
    arena.enemies = 5;
    for (let id = 0; id < 6; id++) B.tanks[id].inert = false;
    B.state.gameMode = R.quickMode; B.state.score = 0;
    B.refreshSetupLabels();
  }
  /* Session counts join the lifetime totals at menu time only. */
  function flush() {
    sessionShots();
    rec.s[0] += R.shots; rec.s[1] += Math.min(R.hits, R.shots); rec.s[2] += Math.min(R.bankHits, R.hits);
    R.shots0 = B.state.shots; R.shots = R.hits = R.bankHits = 0;
    kit.writeSave();
  }
  function sessionShots() { R.shots = B.state.shots - R.shots0; }

  // ----------------------------------------------------------- targets --
  function place(id, kind, x, z) {
    const tank = B.tanks[id];
    B.placeTank(tank, x, z, Math.PI, 1, "", "HUNTER", 0);
    tank.inert = true; tank.driveSpeed = 0; tank.spawnGrace = 0;
    tank.cooldown = tank.secondaryCooldown = tank.gadgetCooldown = 1e9;
    tank.maxHealth = tank.health = kind === KIND_BANK ? BANK_HP : LANE_HP;
    tank.command.aimX = 0; tank.command.aimZ = -1;
    R.kind[id] = kind; R.respawnAt[id] = 0;
    B.setTankTint(id, kind === KIND_BANK ? B.CYAN_TINT
      : kind === KIND_MOVING ? ORANGE : B.GOLD_TINT);
  }
  function bankSpot() {
    const at = R.nextSpot; R.nextSpot = (R.nextSpot + 1) % BANK_SPOTS.length;
    return at;
  }
  function spawnTarget(id) {
    const kind = R.kind[id];
    if (kind === KIND_BANK) {
      const at = R.spot[id] = bankSpot();
      place(id, kind, BANK_SPOTS[at][0], BANK_SPOTS[at][1]);
    } else {
      const lane = R.spot[id];
      place(id, kind, kind === KIND_MOVING ? 0 : LANE_X[lane], LANE_Z[lane]);
      if (kind === KIND_MOVING) { R.side[id] = 0; drive(id, 0); }
    }
  }
  /* Targets for the drill and behaviour: ids 1-3 lane, 4-5 bank yard. */
  function arrange() {
    const drill = R.drill, behaviour = R.behaviour;
    R.nextSpot = 0; R.downs = 0; R.gate = 0; R.pop = 0;
    for (let id = 1; id < 6; id++) {
      let kind = KIND_NONE;
      if (id <= 3) {
        if (drill === 3 || (drill === 0 && behaviour === 1)) kind = KIND_MOVING;
        else if (drill === 0 && behaviour === 0) kind = KIND_LANE;
      } else if (drill === 2 || (drill === 0 && behaviour !== 2)) kind = KIND_BANK;
      R.kind[id] = kind; R.spot[id] = (id - 1) % 3;
      R.phase[id] = id * 1.7;
      if (kind) spawnTarget(id);
      else { B.tanks[id].active = false; B.tanks[id].inert = true; }
    }
    const pickups = B.pickups;
    for (let at = 0; at < pickups.length; at++) pickups[at].active = false;
    if (drill === 1) showGate();
    else marker(0, LANE_Z[1]);
  }
  function drive(id, dt) {
    const tank = B.tanks[id], lane = R.spot[id];
    const phase = R.phase[id] + SPEEDS[lane] * dt;
    const x = SWING * Math.sin(phase), z = LANE_Z[lane];
    // Rails, not physics: wait rather than slide into another hull.
    for (let at = 0; at < B.tanks.length; at++) {
      const other = B.tanks[at];
      if (at === id || !other.active) continue;
      const dx = other.x - x, dz = other.z - z;
      const reach = tank.collisionRadius + other.collisionRadius;
      if (dx * dx + dz * dz < reach * reach) return;
    }
    R.phase[id] = phase;
    tank.x = x; tank.z = z;
    const side = Math.cos(R.phase[id]) >= 0 ? 1 : -1;
    if (side !== R.side[id]) {
      R.side[id] = side; tank.yaw = tank.turret = side * Math.PI / 2;
      B.updateTankYawCache(tank); B.updateTankTurretCache(tank);
      tank.command.aimX = side; tank.command.aimZ = 0;
    }
  }
  function showGate() {
    const pickup = B.pickups[0], gate = COURSE[R.gate];
    pickup.active = true; pickup.x = gate[0]; pickup.z = gate[1];
    pickup.phase = 0; pickup.type = "COOLANT";
    marker(gate[0], gate[1]);
  }
  /* With no target up, the objective mast and HUD compass point here
     (game.js objectivePosition): the next slalom gate, else the middle of
     the lane. */
  function marker(x, z) { R.markerX = x; R.markerZ = z; }
  function placePlayer(x, z) {
    const player = B.tanks[0];
    B.placeTank(player, x, z, 0, 0, player.gadget, "PLAYER", player.classId);
    player.spawnGrace = 0;
  }

  // ------------------------------------------------------------- hooks --
  /* beginArena(3) placed Survival's ring; replace it with the range. */
  function arena(index) {
    if (!R.on || index !== 3) return;
    const st = B.state, arenaData = globalThis.__treadlineArenaData.generatedArena;
    for (let at = 0; at < B.crates.length; at++) B.crates[at].active = false;
    for (let at = 0; at < B.hazards.length; at++) B.hazards[at].active = false;
    for (let at = 0; at < ICE.length; at++) B.spawnHazard(0, ICE[at][0], ICE[at][1], ICE[at][2], -1);
    for (let at = 0; at < POLES.length; at++) B.barriers[at].health = 1e6;
    // Survival opens its (absent) gate at <= enemies/2 foes; keep that
    // constant so respawning targets never toast GATE OPEN/SEALED.
    arenaData.enemies = 99; st.gateOpen = true;
    begin(R.drill);
    B.fillBarrierCircleGrid(); B.invalidateBotNavigation(); B.dirty(false);
    st.score = 0;
  }
  /* Start a drill (0 = free range) in place: player to its start line,
     targets and gates rearranged, clock reset. */
  function begin(drill) {
    R.drill = drill; R.done = false; R.result = null; R.t = 0; R.greeted = false;
    placePlayer(drill === 2 ? -7.2 : 0, drill === 2 ? -4.4 : SPAWN_Z);
    arrange();
  }

  function tick(dt) {
    if (!R.on) return;
    // START on a drill result resumes too: like O, into the free range.
    if (R.done) begin(0);
    const st = B.state, tanks = B.tanks;
    R.t += dt;
    // beginArena's "ARENA 4" banner would name the borrowed slot.
    if (!R.greeted) { R.greeted = true; B.showToast(R.drill ? "GO" : "RANGE", 1.2); }
    for (let id = 1; id < 6; id++) {
      const kind = R.kind[id];
      if (!kind) continue;
      const tank = tanks[id];
      if (R.pop & 1 << id) {
        R.pop &= ~(1 << id);
        tank.active = false; R.downs++;
        B.spawnParticles(tank.x, .45, tank.z, 6, 2.4);
        if (B.sounds && B.sounds.kill) B.sounds.kill();
        R.respawnAt[id] = R.t + RESPAWN;
        const goal = DRILLS[R.drill].goal;
        if (goal) bark(R.downs >= goal ? "CLEAR" : `DOWN ${R.downs}/${goal}`, 1);
        else bark("DOWN");
        if (goal && R.downs >= goal) { finish(); return; }
      } else if (!tank.active && R.t >= R.respawnAt[id]) {
        spawnTarget(id); tank.spawnGrace = 0;
      } else if (kind === KIND_MOVING && tank.active) drive(id, dt);
    }
    if (R.drill === 1) {
      const pickup = B.pickups[0];
      if (!pickup.active) {
        const cue = COURSE[R.gate][2];
        R.gate++;
        if (R.gate >= COURSE.length) { finish(); return; }
        bark(cue || `GATE ${R.gate + 1}/${COURSE.length}`, 1);
        showGate();
      }
    }
    for (let at = 0; at < POLES.length; at++) {
      const barrier = B.barriers[at];
      if (!barrier.active) { barrier.active = true; B.fillBarrierCircleGrid(); B.dirty(false); }
      barrier.health = 1e6;
    }
    sessionShots();
    st.score = R.drill ? Math.min(99999, R.t * 10 | 0) : Math.min(99999, R.hits);
  }
  function finish() {
    B.showToast("FINISH", 1.2);
    const slot = R.drill - 1, tenths = Math.max(1, Math.round(R.t * 10));
    const best = rec.t[slot];
    R.result = {drill: R.drill, tenths, best, record: !best || tenths < best};
    if (R.result.record) rec.t[slot] = tenths;
    R.done = true;
    const pickups = B.pickups;
    for (let at = 0; at < pickups.length; at++) pickups[at].active = false;
    flush();
    B.setMode("paused");
  }

  /* Before armor is removed. The player is never hurt; only player hits
     count; bank targets ignore direct hits; a target that would die pops
     on the next step instead (no score, kill beat or arena clear). */
  function damage(tank, amount, attacker, bounces) {
    const id = tank.id, kind = R.kind[id];
    if (id === 0 || attacker !== 0 || !kind) return 0;
    // A direct hit on a bank target does nothing, so it is not a hit.
    if (kind === KIND_BANK && !bounces) { bark("BANK IT"); return 0; }
    R.hits++;
    if (bounces) R.bankHits++;
    if (amount >= tank.health) { amount = tank.health - 1; R.pop |= 1 << id; }
    else bark(bounces > 1 ? `2X BANK ${amount}` : bounces ? `BANK ${amount}` : `HIT ${amount}`, .8);
    return amount;
  }

  // ------------------------------------------------------------- menus --
  const tenths = (v) => v ? `${(v / 10).toFixed(1)} s` : "-";
  function stats() {
    sessionShots();
    const shots = R.on ? R.shots : rec.s[0], hits = R.on ? R.hits : rec.s[1];
    const banks = R.on ? R.bankHits : rec.s[2];
    const pct = shots ? Math.min(100, Math.round(hits * 100 / shots)) : 0;
    return `${R.on ? "" : "All time: "}Shots ${shots} | Hits ${hits} (${pct}%) | Bank hits ${banks}`;
  }
  /* The drill's one line plus its controls in the current scheme. */
  function note(drill) {
    const d = DRILLS[drill], K = globalThis.__treadlineControls;
    let keys = "";
    if (K && d.keys.length) for (let page = 0; page < 2; page++)
      for (const row of K.bindings(page)) {
        const bar = row.indexOf("|"), action = row.slice(bar + 1);
        if (d.keys.some(k => action.startsWith(k)))
          keys += `${keys ? " | " : ""}${row.slice(0, bar)}: ${action}`;
      }
    return d.note + (keys ? `  ${keys}` : "");
  }
  const notes = [];
  const screens = {
    practice() {
      const {button, set, list} = kit;
      set(kit.heading, "PRACTICE RANGE");
      set(kit.message, stats());
      list.className = "list";
      notes.length = 0;
      for (let at = 0; at < DRILLS.length; at++) {
        notes.push(note(at));
        button(at && rec.t[at - 1] ? `${DRILLS[at].n}   best ${tenths(rec.t[at - 1])}` : DRILLS[at].n,
          () => R.on ? (kit.show("game", false), begin(at), B.setMode("playing")) : enter(at),
          at ? "" : "primary");
      }
      notes.push("");
      button("Back", kit.back);
    },
    range() {
      const {button, set, list} = kit, st = B.state, player = B.tanks[0];
      const K = globalThis.__treadlineControls;
      set(kit.heading, R.drill ? `RANGE: ${DRILLS[R.drill].n.toUpperCase()}` : "PRACTICE RANGE");
      set(kit.message, stats());
      list.className = "";
      button("Resume", () => B.startOrResume(), "primary");
      button(`Controls ${K ? K.names[B.preferences.controls] : ""} / ${B.AIM_ASSISTS[B.preferences.assist]}`,
        () => kit.show("controls"));
      button(`Class ${B.CLASSES[player.classId].name}`, () => {
        st.classChoice = (player.classId + 1) % B.CLASSES.length;
        B.placeTank(player, player.x, player.z, player.yaw, 0, player.gadget, "PLAYER", st.classChoice);
        player.spawnGrace = 0; B.savePreferences(); B.refreshSetupLabels(); kit.render();
      });
      button(`Gadget ${player.gadget}`, () => {
        st.gadgetChoice = (B.GADGETS.indexOf(player.gadget) + 1) % B.GADGETS.length;
        player.gadget = B.GADGETS[st.gadgetChoice]; player.gadgetCooldown = 0;
        B.savePreferences(); B.refreshSetupLabels(); kit.render();
      });
      button(`Targets ${R.drill ? "drill" : BEHAVIOURS[R.behaviour]}`, () => {
        R.behaviour = (R.behaviour + 1) % 3; if (!R.drill) arrange(); kit.render();
      }, "", R.drill !== 0);
      button(`Aim guide ${B.AIM_LEVEL_NAMES[R.aim]}`, () => { cycleAim(1); kit.render(); });
      button("Drills", () => kit.show("practice"));
      button("Leave range", () => { leave(); kit.toMain(); });
    },
    drilldone() {
      const {button, set, list} = kit, r = R.result;
      const d = DRILLS[r ? r.drill : 1];
      set(kit.heading, `${d.n.toUpperCase()} CLEAR`);
      set(kit.message, r ? `Time ${tenths(r.tenths)} ${r.record ? "(new best)" : `(best ${tenths(r.best)})`} | ${stats()}` : "");
      list.className = "row";
      button("Retry", () => { kit.show("game", false); begin(r.drill); B.setMode("playing"); }, "primary");
      button("Free range", () => { kit.show("game", false); begin(0); B.setMode("playing"); });
      button("Drills", () => kit.show("practice"));
      button("Leave", () => { leave(); kit.toMain(); });
    },
  };
  const LEGENDS = {practice: "X Select | O Back", range: "X Select | O Resume | START Resume",
    drilldone: "X Select | O Free range"};
  /* O on the in-run screens resumes play (the result screen into the free
     range); everything else uses the campaign's back stack. */
  function back(screen) {
    if (screen === "range") { B.startOrResume(); return true; }
    if (screen === "drilldone") { kit.show("game", false); begin(0); B.setMode("playing"); return true; }
    return false;
  }
  function focusNote(screen, at, message) {
    if (screen === "practice" && notes[at] && message.textContent !== notes[at])
      message.textContent = notes[at];
  }

  function cycleAim(delta) {
    R.aim = (R.aim + 3 + (delta < 0 ? -1 : 1)) % 3;
  }

  function attach(bridge, menuKit) {
    B = bridge; kit = menuKit;
    // Qualification and test runs only (__treadlineDebug.practice).
    if (B.lendTooling) B.lendTooling("practice", Object.freeze(Object.assign(Object.create(api), {
      runtime: R, record: rec,
      layout: {WALLS, RAMPS, POLES, ICE, LANE_Z, BANK_SPOTS, COURSE}, drills: DRILLS,
      VERSION: PRACTICE_VERSION})));
  }

  const api = {attach, load, saved, enter, leave, begin, arena, tick, damage,
    screens, legends: LEGENDS, back, focusNote, cycleAim,
    pauseScreen() { return R.done ? "drilldone" : "range"; },
    get on() { return R.on; },
    get aimLevel() { return R.aim; },
    get markerX() { return R.markerX; },
    get markerZ() { return R.markerZ; },
  };
  Object.defineProperty(globalThis, "__treadlinePractice", {
    value: Object.freeze(api), configurable: false, writable: false,
  });
})();
