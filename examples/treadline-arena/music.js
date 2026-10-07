/* Treadline Arena: adaptive music.

   Loaded before game.js. game.js creates one conductor for its SoundBank
   (not in timing qualification runs unless the URL has music=on) and calls
   three bounded hooks: tick() once per simulation step, sfxVoice() when an
   effect needs an oscillator, and heartbeat() before the low-armor heartbeat
   effect. campaign.js keeps the settings in its save. Music is output only:
   it reads game state and never writes it, never draws a random number and
   never touches replays, so league, sweep and replay results are unchanged.
   preferences.music 0 (Off; the bot league and campaign sweep set it)
   silences it.

   Voices. Music borrows two of the four game-audio voices and adds no node:
   the lead plays on the second effect oscillator (triangle) and the bass on
   the engine-hum oscillator (triangle). Effects keep priority: while music
   holds the lead, effects prefer the first oscillator; when that one is busy
   the effect takes the lead voice at once and music sits out until the next
   bar. The bass replaces the engine hum only while the arrangement has a bass
   part; in quiet exploration the hum returns.

   Scheduling. Every bar is one prebuilt gain curve (and a pitch curve when
   the bar changes pitch) per voice: 2-64 float32 samples, linear, holding the
   last value, cancelled at the current time before the next is set. A note
   is [silent glide, attack, decay..., release]; the pitch moves only while
   the gain is zero. Each bar's curve ends early enough to leave a window in
   which the next bar is scheduled at most PREPARE_LEAD seconds ahead, so the
   ten-second horizon is never approached. Between windows a frame costs a
   few comparisons; the intensity state is evaluated five times a second.

   Intensity. EXPLORE (no foe close) is a sparse motif with the hum, ALERT
   adds a bass pulse, COMBAT runs 1.5x tempo with arpeggios, DANGER (low
   armor) turns the motif minor over a heartbeat bass. Escalation takes
   effect at the next bar; calming needs CALM_HOLD seconds and MIN_BARS bars.
   Stingers (kill, clear, boss, victory, defeat) replace whole bars. */
(() => {
  "use strict";
  const EXPLORE = 0, ALERT = 1, COMBAT = 2, DANGER = 3;
  const LEVELS = ["explore", "alert", "combat", "danger"];
  const NONE = 0, MENU = 1, GAME = 2;
  const KINDS = ["none", "menu", "game"];
  const OFF = 0, MENU_ONLY = 1, FULL = 2;
  const VOLUMES = [.6, 1, 1.45];
  const LEAD = 0, BASS = 1;
  const POINTS = 64, HORIZON = 10, GAME_FLOATS = 3392, GLOBAL_FLOATS = 2688;
  /* Seconds. The next bar is decided this long before it starts; a voice
     schedules once its previous curve has finished, never closer than
     MIN_LEAD to the new curve's start (a later voice sits the bar out). */
  const PREPARE_LEAD = .5, MIN_LEAD = .02, START_LEAD = .15, RESYNC = .3;
  const EVAL_PERIOD = .2, COMBAT_HOLD = 4.5, CALM_HOLD = 3.5, MIN_BARS = 2;
  const KILL_WINDOW = 3;
  const ALERT_IN = 6.5 * 6.5, ALERT_OUT = 8.5 * 8.5, FIRE_RANGE = 7.5 * 7.5;
  const DANGER_IN = .3, DANGER_OUT = .42;

  // -------------------------------------------------------------- scales --
  const MAJOR = [0, 2, 4, 5, 7, 9, 11], MINOR = [0, 2, 3, 5, 7, 8, 10];
  const DORIAN = [0, 2, 3, 5, 7, 9, 10], MIXOLYDIAN = [0, 2, 4, 5, 7, 9, 10];
  const LYDIAN = [0, 2, 4, 6, 7, 9, 11], PHRYGIAN = [0, 1, 3, 5, 7, 8, 10];
  const HARMONIC = [0, 2, 3, 5, 7, 8, 11], LOCRIAN = [0, 1, 3, 5, 6, 8, 10];

  /* ---------------------------------------------------------- notation --
     One character per sixteenth. Melodies: 0-9 and a-f are scale degrees
     counted from the octave below the program's tonic, so 7 is the tonic,
     e the octave above. Arpeggios: 1 3 5 8 x c are chord tones (root,
     third, fifth, octave, tenth, twelfth) of the bar's chord. Bass: R root,
     r soft root, O octave, F fifth. "_" holds, "." rests. Every
     bar leaves its final step(s) silent: that is the scheduling window. */
  const MARSHAL = ["7__9b_____a_9_..", "8__9a___9___7_..",
    "7__9b_____e___..", "d_c_b_a_9_8_7_.."];
  const VESPER = ["7__c__b_9_____..", "8___9___8___4_..",
    "7__c__b_e_____..", "d_c_b_9_8_____.."];
  const SUNSET = ["e___d___c___b_..", "a___9___8_____..",
    "e___d___c___b_..", "8___7_________.."];
  const ARPS = ["1358135813588_..", "8531853185311_..", "1358531813588_.."];
  const KILL_HEAD = "58xc";
  const BASS_ALERT = "R.R.R.R.R.R.R...", BASS_COMBAT = "R.R.O.R.R.R.F...";
  const BASS_DANGER = "Rr..Rr..Rr..Rr..";
  /* key: the lead tonic (MIDI). bpm: explore/alert; combat runs 1.5x and
     danger 0.75x (its heartbeat sits near the old 0.82 s effect). */
  const PROGRAMS = {
    yard: {name: "Proving Yard (Marshal)", key: 67, scale: MAJOR, dark: MINOR,
      bpm: 96, prog: [0, 3, 0, 4], motif: MARSHAL},
    foundry: {name: "Foundry", key: 62, scale: MIXOLYDIAN, dark: DORIAN,
      bpm: 100, prog: [0, 6, 0, 3], motif: ["7_7_9_b___d_b_..",
        "a___9___8_____..", "7_7_9_b___e_d_..", "b___a___9_8_7_.."]},
    glacier: {name: "Glacier", key: 65, scale: LYDIAN, dark: MINOR,
      bpm: 76, prog: [0, 1, 0, 5], motif: ["7_______b_____..",
        "a___________....", "9_______d_____..", "c___b_________.."]},
    city: {name: "Night City", key: 69, scale: DORIAN, dark: MINOR,
      bpm: 92, prog: [0, 3, 0, 4], motif: ["7__9__a_b_____..",
        "c_b_____9_7___..", "7__9__a_b___d_..", "b___a_9_7_____.."]},
    fortress: {name: "Fortress", key: 64, scale: PHRYGIAN, dark: LOCRIAN,
      bpm: 84, prog: [0, 5, 0, 1], motif: ["7___8___7_____..",
        "4___5___6___7_..", "7___8___9___b_..", "a___8___7_____.."]},
    practice: {name: "Practice Range", key: 72, scale: MAJOR, dark: MAJOR,
      bpm: 72, prog: [0, 4, 5, 3], min: ALERT, max: ALERT, bassAlert: "R_______F_____..",
      motif: ["b_______9___7_..", "8___________....", "c_______b___9_..",
        "8___9_________.."]},
    vesper: {name: "Vesper (rival)", key: 69, scale: MINOR, dark: HARMONIC,
      bpm: 104, prog: [0, 4, 5, 4], motif: VESPER},
    ally: {name: "Vesper (ally)", key: 69, scale: MAJOR, dark: MINOR,
      bpm: 104, prog: [0, 4, 5, 4], motif: VESPER},
    sunset: {name: "Sunset", key: 62, scale: PHRYGIAN, dark: LOCRIAN,
      bpm: 88, prog: [0, 1, 0, 1], bassAlert: "R___R___R___R_..", motif: SUNSET},
    last: {name: "Last Match", key: 69, scale: MAJOR, dark: MAJOR, bpm: 80,
      prog: [0, 4, 5, 4], max: ALERT, bassAlert: "R_______F_____..", motif: VESPER},
  };
  /* The title theme keeps the old menu melody's contour (E G A G D E B G),
     now a lead over a bass. Stingers are global except the program-keyed
     clear. Each is [bpm, key, scale, lead, bass, chord]. */
  const TITLE = {key: 64, scale: MINOR, bpm: 92, prog: [0, 2, 5, 3, 2, 4, 5, 0],
    bass: "R___F___R___F_..", motif: ["7___9___a___9_..", "6___7___b___9_..",
      "7_9_a_b_c___b_..", "a___9___7_____..", "9___a___b___e_..",
      "d___c___b_____..", "c_b_a_9_a___7_..", "7_____________.."]};
  const S_CLEAR = 0, S_BOSS = 1, S_VICTORY = 2, S_VICTORY_END = 3,
    S_DEFEAT = 4, S_DEFEAT_END = 5;
  const STINGER_NAMES = ["clear", "boss", "victory", "victory-end", "defeat",
    "defeat-end"];
  const STINGERS = [null,
    [88, 62, PHRYGIAN, "e___d___c___b_..", "R___R___R___R_..", 0],
    [112, 67, MAJOR, "7_9_b_e___b_e_..", "R___F___R___O_..", 0],
    [112, 67, MAJOR, "e_____________..", "R_____________..", 0],
    [70, 64, MINOR, "b___a___9___8_..", "R_______F_____..", 0],
    [70, 64, MINOR, "7_____________..", "R_____________..", 0]];
  const CLEAR_LEAD = "7_9_b_e_______..", CLEAR_BASS = "R___F___O_____..";

  /* Peak gain and decay per part: [peak, tau seconds, sustain floor]. */
  const LEAD_SHAPE = [[.042, .55, 0], [.05, .8, .15], [.04, .09, 0], [.045, 1.5, .35]];
  const BASS_SHAPE = [null, [.05, .1, 0], [.065, .12, 0], [.08, .09, 0]];
  const TITLE_LEAD = [.045, .7, .2], TITLE_BASS = [.045, .5, .2];
  const STINGER_LEAD = [.05, .5, .2], STINGER_BASS = [.06, .4, .1];

  // ------------------------------------------------------------- builder --
  /* Build-time scratch; nothing below runs per frame. */
  const noteStep = new Int8Array(16), noteLength = new Int8Array(16);
  const noteMidi = new Float32Array(16), noteVelocity = new Float32Array(16);
  const hz = (midi) => 440 * Math.pow(2, (midi - 69) / 12);
  const degreeMidi = (key, scale, degree) => {
    const octave = Math.floor(degree / scale.length);
    return key - 12 + octave * 12 + scale[degree - octave * scale.length];
  };
  const ARP_TONES = {49: 0, 51: 2, 53: 4, 56: 7, 120: 9, 99: 11};

  function mapNote(kind, code, key, scale, root) {
    if (kind === 0) {
      const degree = code <= 57 ? code - 48 : code - 87;
      return degree >= 0 && degree < 16 ? degreeMidi(key, scale, degree) : -1;
    }
    if (kind === 1) {
      const tone = ARP_TONES[code];
      if (tone === undefined) return -1;
      return degreeMidi(key, scale, (root <= 2 ? root + 7 : root) + tone);
    }
    let bass = key - 24 + scale[root % scale.length];
    if (bass < 40) bass += 12;
    if (code === 82 || code === 114) return bass;
    if (code === 79) return bass + 12;
    if (code === 70) return bass + 7;
    return -1;
  }

  function parse(pattern, kind, key, scale, root) {
    let count = 0, held = false;
    for (let at = 0; at < pattern.length; at++) {
      const code = pattern.charCodeAt(at);
      if (code === 95) {
        if (held) noteLength[count - 1]++;
        continue;
      }
      held = false;
      if (code === 46) continue;
      const midi = mapNote(kind, code, key, scale, root);
      if (midi < 0 || count === 16) throw new Error(`Bad note in ${pattern}`);
      noteStep[count] = at; noteLength[count] = 1; noteMidi[count] = midi;
      noteVelocity[count] = code === 114 ? .6 : 1;
      count++; held = true;
    }
    return count;
  }

  class Pool {
    constructor(floats) { this.data = new Float32Array(floats); this.used = 0; }
    take(count) {
      if (this.used + count > this.data.length)
        throw new Error("Music curve pool exhausted");
      const view = this.data.subarray(this.used, this.used + count);
      this.used += count;
      return view;
    }
  }

  /* One bar of one voice -> {start, end, k, n, gain, pitch|null, hz}.
     pitch is null when the bar holds one pitch: the base frequency is then
     set once instead of evaluating a pitch curve on every output sample. */
  function segment(pool, pattern, kind, key, scale, root, bpm, shape, volume,
                   velocity = 1, head = null) {
    if (head) pattern = head + pattern.slice(head.length);
    const count = parse(pattern, kind, key, scale, root);
    if (!count) return null;
    const start = noteStep[0];
    const end = noteStep[count - 1] + noteLength[count - 1];
    const span = end - start, k = Math.min(8, (POINTS / span) | 0);
    const n = span * k, dt = 60 / bpm / 4 / k;
    let constant = true;
    for (let at = 1; at < count; at++)
      if (noteMidi[at] !== noteMidi[0]) constant = false;
    const gain = pool.take(n), pitch = constant ? null : pool.take(n);
    gain.fill(0);
    const peak = shape[0] * volume * velocity, tau = shape[1], floor = shape[2];
    for (let note = 0; note < count; note++) {
      const base = (noteStep[note] - start) * k, length = noteLength[note] * k;
      const level = peak * noteVelocity[note], frequency = hz(noteMidi[note]);
      for (let offset = 0; offset < length; offset++) {
        gain[base + offset] = offset === 0 || offset === length - 1 ? 0
          : level * (floor + (1 - floor) * Math.exp(-(offset - 1) * dt / tau));
        if (pitch) pitch[base + offset] = frequency;
      }
      // A rest before this note glides to its pitch in silence.
      if (pitch && note > 0) {
        const previous = (noteStep[note - 1] + noteLength[note - 1] - start) * k;
        for (let at = previous; at < base; at++) pitch[at] = frequency;
      }
    }
    return {start, end, k, n, gain, pitch, hz: hz(noteMidi[0])};
  }

  /* Every bar a program can play, built into a game pool one segment per
     step, so a program change in the middle of a run can be spread over
     frames (Conductor.continueBuild). The bars are its return value. */
  function* programSteps(pool, id, volume) {
    const p = PROGRAMS[id], combat = p.bpm * 1.5, danger = p.bpm * .75;
    const bars = {lead: [[], [], [], []], bass: [[], [], [], []], kill: [],
      clear: null, clearBass: null};
    for (let bar = 0; bar < 4; bar++) {
      const root = p.prog[bar], motif = p.motif[bar];
      bars.lead[EXPLORE][bar] = (bar & 1) ? null
        : segment(pool, motif, 0, p.key, p.scale, root, p.bpm, LEAD_SHAPE[EXPLORE], volume, .85);
      yield;
      bars.lead[ALERT][bar] = segment(pool, motif, 0, p.key, p.scale, root,
        p.bpm, LEAD_SHAPE[ALERT], volume);
      yield;
      bars.lead[COMBAT][bar] = bar === 3
        ? segment(pool, motif, 0, p.key, p.scale, root, combat, LEAD_SHAPE[ALERT], volume)
        : segment(pool, ARPS[bar], 1, p.key, p.scale, root, combat, LEAD_SHAPE[COMBAT], volume);
      yield;
      bars.kill[bar] = bar === 3 ? null : segment(pool, ARPS[bar], 1, p.key,
        p.scale, root, combat, LEAD_SHAPE[COMBAT], volume, 1.2, KILL_HEAD);
      yield;
      bars.lead[DANGER][bar] = segment(pool, motif, 0, p.key, p.dark, root,
        danger, LEAD_SHAPE[DANGER], volume);
      yield;
      bars.bass[EXPLORE][bar] = null;
      bars.bass[ALERT][bar] = segment(pool, p.bassAlert || BASS_ALERT, 2, p.key,
        p.scale, root, p.bpm, BASS_SHAPE[ALERT], volume);
      yield;
      bars.bass[COMBAT][bar] = segment(pool, BASS_COMBAT, 2, p.key, p.scale, root,
        combat, BASS_SHAPE[COMBAT], volume);
      yield;
      bars.bass[DANGER][bar] = segment(pool, BASS_DANGER, 2, p.key, p.dark, root,
        danger, BASS_SHAPE[DANGER], volume);
      yield;
    }
    bars.clear = segment(pool, CLEAR_LEAD, 0, p.key, p.scale, 0, p.bpm,
      STINGER_LEAD, volume);
    yield;
    bars.clearBass = segment(pool, CLEAR_BASS, 2, p.key, p.scale, 0, p.bpm,
      STINGER_BASS, volume);
    yield;
    return bars;
  }

  function buildProgram(pool, id, volume) {
    const steps = programSteps(pool, id, volume);
    let step = steps.next();
    while (!step.done) step = steps.next();
    return step.value;
  }

  /* Title theme and the global stingers share the second pool. */
  function buildGlobal(pool, volume) {
    const title = {lead: [], bass: []}, stingers = [];
    for (let bar = 0; bar < TITLE.motif.length; bar++) {
      const root = TITLE.prog[bar];
      title.lead[bar] = segment(pool, TITLE.motif[bar], 0, TITLE.key, TITLE.scale,
        root, TITLE.bpm, TITLE_LEAD, volume);
      title.bass[bar] = segment(pool, TITLE.bass, 2, TITLE.key, TITLE.scale, root,
        TITLE.bpm, TITLE_BASS, volume);
    }
    for (let at = 1; at < STINGERS.length; at++) {
      const [bpm, key, scale, lead, bass, root] = STINGERS[at];
      stingers[at] = [segment(pool, lead, 0, key, scale, root, bpm, STINGER_LEAD, volume),
        segment(pool, bass, 2, key, scale, root, bpm, STINGER_BASS, volume), bpm];
    }
    return {title, stingers};
  }

  // ------------------------------------------------------------ conductor --
  const STAT_NAMES = ["segments", "pitchCurves", "baseFrequencies", "cancels",
    "misses", "steals", "refused", "transitions", "stingers", "bars",
    "maxPoints", "builds", "evaluations", "services"];
  const S_SEGMENTS = 0, S_PITCH = 1, S_BASE = 2, S_CANCELS = 3, S_MISSES = 4,
    S_STEALS = 5, S_REFUSED = 6, S_TRANSITIONS = 7, S_STINGERS = 8,
    S_BARS = 9, S_MAX_POINTS = 10, S_BUILDS = 11, S_EVALUATIONS = 12,
    S_SERVICES = 13;

  class Conductor {
    constructor(bank, env) {
      this.bank = bank; this.env = env;
      this.state = env.state; this.preferences = env.preferences;
      this.tanks = env.tanks || [];
      this.stats = new Float64Array(STAT_NAMES.length);
      this.maxHorizon = 0;
      // Sized for the largest program (3,216 floats) and the title theme plus
      // stingers (2,576); tests build every program into these pools. Two
      // game pools: the program playing, and the one being built.
      this.gamePools = [new Pool(GAME_FLOATS), new Pool(GAME_FLOATS)];
      this.front = 0; this.globalPool = new Pool(GLOBAL_FLOATS);
      this.programId = ""; this.bars = null; this.global = null;
      this.pending = null; this.pendingId = "";
      this.builtVolume = -1;
      this.kind = NONE; this.mode = ""; this.forced = -1;
      this.level = EXPLORE; this.desired = EXPLORE; this.levelBars = 0;
      this.calmSince = -1; this.lastCombat = -1e9; this.danger = false;
      this.alert = false; this.kills = 0; this.killAt = -1e9; this.bossSeen = false;
      this.evalAt = 0; this.wake = 0; this.nextStart = 0; this.barCount = 0;
      this.menuBar = 0; this.lastArena = -1; this.stealth = false;
      this.queue = new Int8Array(4); this.queued = 0;
      this.plan = {active: false, start: 0, end: 0, stepDur: 0, kind: NONE,
        level: EXPLORE, stinger: -1, lead: null, bass: null};
      this.voices = [LEAD, BASS].map((part) => ({part, segEnd: 0, done: true,
        curves: false, pitchCurve: false, base: -1, effectStamp: -1}));
      this.leadOwned = false; this.bassOwned = false;
    }

    get gamePool() { return this.gamePools[this.front]; }

    // ---- settings
    /* preferences.music: 0/false Off, 1 Menus, 2/true Full (campaign save). */
    get musicMode() {
      const value = this.preferences.music;
      return value === true ? FULL : value === MENU_ONLY || value === FULL ? value : OFF;
    }
    volume() {
      const at = this.preferences.musicVolume;
      return at === 0 || at === 2 ? at : 1;
    }

    // ---- per frame (called from SoundBank.tick)
    tick(time, player, mode) {
      const preference = this.musicMode;
      if (mode !== this.mode) this.onMode(mode, preference);
      let kind = NONE;
      if (preference !== OFF) {
        kind = this.menuMode(mode, preference) ? MENU : GAME;
        if (kind === GAME && preference !== FULL) kind = NONE;
      }
      if (kind !== this.kind) this.switchKind(kind);
      if (kind === NONE) return false;
      if (kind === GAME && time >= this.evalAt) {
        this.evalAt = time + EVAL_PERIOD;
        this.evaluate(time, player);
      }
      if (this.pending) this.continueBuild();
      const bank = this.bank, context = bank.context;
      if (!context || !bank.engine || bank.voices.length < 2
          || context.state !== "running") return this.claimsEngine(kind, mode, -1);
      const now = context.currentTime;
      if (now >= this.wake) this.service(now, time);
      return this.claimsEngine(kind, mode, now);
    }

    /* True while the engine-hum oscillator belongs to music: always in
       menus and outside active play (where the hum is silent anyway), and
       in play while the arrangement has a bass part. The hum returns once
       the last bass bar has finished in quiet exploration. now < 0: no
       running graph, so ownership cannot change. */
    claimsEngine(kind, mode, now) {
      const quiet = kind === GAME && mode === "playing";
      if (now >= 0) {
        const bass = this.voices[BASS];
        if (!this.bassOwned) {
          if (!quiet) this.acquireBass();
        } else if (quiet && this.level === EXPLORE && !this.plan.bass && !this.queued
                   && now >= bass.segEnd && (!this.plan.active || bass.done))
          this.releaseBass(now);
      }
      if (quiet && !this.bassOwned) return false;
      this.bank.engineRole = "music";
      return true;
    }

    menuMode(mode, preference) {
      return mode === "title" || mode === "victory" || mode === "game-over"
        || mode === "replay-done" || (mode === "paused" && preference !== FULL);
    }

    onMode(mode, preference) {
      const previous = this.mode;
      this.mode = mode;
      if (preference === OFF) this.queued = 0;
      else if (mode === "victory") this.enqueue(S_VICTORY, S_VICTORY_END);
      else if (mode === "game-over") this.enqueue(S_DEFEAT, S_DEFEAT_END);
      else if (mode === "arena-clear" && preference === FULL) this.enqueue(S_CLEAR, -1);
      else if (mode === "title" || mode === "replay-done") {
        if (previous !== "victory" && previous !== "game-over") this.queued = 0;
      }
      this.forced = mode === "killcam" || mode === "arena-clear"
        || (mode === "paused" && preference === FULL) ? EXPLORE : -1;
      if (mode === "playing" && preference === FULL) {
        const inRun = previous === "paused" || previous === "hidden"
          || previous === "killcam" || previous === "generating"
          || previous === "duel-pass";
        // Survival's next arena arrives from arena-clear in the same run.
        this.selectProgram(inRun || previous === "arena-clear");
        if (!inRun) {
          this.level = this.desired = EXPLORE; this.levelBars = 0;
          this.calmSince = -1; this.lastCombat = -1e9;
          this.danger = this.alert = false; this.kills = this.state.kills | 0;
          this.killAt = -1e9;
        }
      }
    }

    enqueue(first, second) {
      this.queued = 0;
      this.queue[this.queued++] = first;
      if (second >= 0) this.queue[this.queued++] = second;
    }

    programFor() {
      const env = this.env, state = this.state;
      if (env.practice && env.practice.on) return "practice";
      const runtime = env.campaign && env.campaign.runtime;
      if (runtime && runtime.on && runtime.m) {
        // A mission's own program (campaign.js MISSIONS), else its theater's.
        const m = runtime.m;
        return m.music || ["yard", "foundry", "glacier", "city", "fortress"][
          (m.id.charCodeAt(0) - 49) % 5];
      }
      const mode = state.gameMode;
      if (mode === 3) return "fortress";
      if (mode === 4 || mode === 5) return "city";
      if (mode === 6) return "vesper";
      const arena = state.arena | 0;
      return arena < 3 ? ["yard", "foundry", "glacier"][arena] : "city";
    }

    /* sliced: a change in the middle of a run (Survival's next arena)
       keeps the current program playing and builds the new one a segment
       per tick (about 40 ticks); otherwise (a new run, menus) it is built
       at once. A whole program cost about 100 ms in one PSP frame. */
    selectProgram(sliced = false) {
      const id = this.programFor();
      this.lastArena = this.state.arena;
      /* Whiteout and Lights Out hide foes until they fire: a closing foe
         must not announce itself, so only combat escalates there. */
      const runtime = this.env.campaign && this.env.campaign.runtime;
      this.stealth = !!(runtime && runtime.on && runtime.m && runtime.m.stealth);
      if (id === this.programId && this.builtVolume === this.volume()) {
        this.pending = null; this.pendingId = "";
        return;
      }
      if (sliced && this.bars && this.builtVolume === this.volume()) {
        if (id === this.pendingId) return;
        const pool = this.gamePools[this.front ^ 1];
        pool.used = 0;
        this.pending = programSteps(pool, id, VOLUMES[this.builtVolume]);
        this.pendingId = id;
        return;
      }
      this.programId = id;
      this.build();
    }

    /* One segment of a sliced program build; the finished program takes
       over at the next bar. The bar now playing keeps its own pool. */
    continueBuild() {
      const step = this.pending.next();
      if (!step.done) return;
      this.bars = step.value; this.programId = this.pendingId;
      this.front ^= 1;
      this.pending = null; this.pendingId = "";
      this.stats[S_BUILDS]++;
    }

    build() {
      const volume = this.volume(), scale = VOLUMES[volume];
      if (this.builtVolume !== volume || !this.global) {
        this.globalPool.used = 0;
        this.global = buildGlobal(this.globalPool, scale);
      }
      // A sliced build in progress is finished here, at once.
      if (this.pending) this.programId = this.pendingId;
      this.pending = null; this.pendingId = "";
      if (this.programId) {
        const pool = this.gamePools[this.front ^ 1];
        pool.used = 0;
        this.bars = buildProgram(pool, this.programId, scale);
        this.front ^= 1;
      }
      this.builtVolume = volume;
      /* A pending bar may point into the rebuilt pool: decide it again. */
      this.plan.active = false;
      this.stats[S_BUILDS]++;
    }

    switchKind(kind) {
      const now = this.bank.context ? this.bank.context.currentTime : 0;
      if (kind === NONE) {
        this.releaseLead(now);
        this.releaseBass(now);
        this.plan.active = false;
        this.nextStart = 0; this.queued = 0;
      } else if (kind === GAME && !this.programId) this.selectProgram();
      if (kind === MENU) this.menuBar = 0;
      this.kind = kind;
      this.wake = 0;
      this.stats[S_TRANSITIONS]++;
    }

    // ---- intensity (5 Hz)
    evaluate(time, player) {
      this.stats[S_EVALUATIONS]++;
      const state = this.state;
      if (state.arena !== this.lastArena && state.mode === "playing"
          && this.musicMode === FULL) this.selectProgram(true);
      if (state.mode !== "playing" || !player || !player.active) return;
      const tanks = this.tanks;
      let nearest = Infinity, fired = false, boss = false;
      for (let at = 0; at < tanks.length; at++) {
        const tank = tanks[at];
        if (!tank.active || tank === player || tank.team === player.team
            || tank.inert) continue;
        const dx = tank.x - player.x, dz = tank.z - player.z;
        const distance = dx * dx + dz * dz;
        if (distance < nearest) nearest = distance;
        if (tank.cooldown > 0 && distance < FIRE_RANGE) fired = true;
        if (tank.boss) boss = true;
      }
      if (state.damageIndicator > 0 || state.hitConfirm > 0 || fired || boss
          || (player.cooldown > 0 && nearest < FIRE_RANGE)) this.lastCombat = time;
      if (boss && !this.bossSeen && this.kind === GAME) this.enqueue(S_BOSS, -1);
      this.bossSeen = boss;
      const kills = state.kills | 0;
      if (kills > this.kills) this.killAt = time;
      this.kills = kills;
      const health = player.maxHealth > 0 ? player.health / player.maxHealth : 1;
      this.danger = this.danger ? health < DANGER_OUT : health < DANGER_IN;
      this.alert = !this.stealth && (this.alert ? nearest < ALERT_OUT : nearest < ALERT_IN);
      let desired = this.danger ? DANGER : time - this.lastCombat < COMBAT_HOLD
        ? COMBAT : this.alert ? ALERT : EXPLORE;
      const program = this.programId ? PROGRAMS[this.programId] : null;
      if (program && program.max !== undefined && desired > program.max) desired = program.max;
      if (program && program.min !== undefined && desired < program.min) desired = program.min;
      this.desired = desired;
      if (desired >= this.level) this.calmSince = -1;
      else if (this.calmSince < 0) this.calmSince = time;
    }

    /* Commit the level for the next bar: escalate at once, calm slowly. */
    commitLevel(time) {
      const previous = this.level;
      if (this.forced >= 0) this.level = this.forced;
      else if (this.desired > this.level) this.level = this.desired;
      else if (this.desired < this.level && this.levelBars >= MIN_BARS
               && this.calmSince >= 0 && time - this.calmSince >= CALM_HOLD)
        this.level = this.desired;
      if (this.level !== previous) { this.levelBars = 0; this.stats[S_TRANSITIONS]++; }
      this.levelBars++;
    }

    // ---- bar clock
    prepare(start, time) {
      const plan = this.plan;
      if (this.builtVolume !== this.volume()) this.build();
      plan.active = true; plan.start = start; plan.kind = this.kind;
      plan.stinger = -1; plan.lead = plan.bass = null;
      let bpm;
      if (this.queued) {
        const stinger = this.queue[0];
        for (let at = 1; at < this.queued; at++) this.queue[at - 1] = this.queue[at];
        this.queued--;
        plan.stinger = stinger; this.stats[S_STINGERS]++;
        if (stinger === S_CLEAR && this.bars) {
          plan.lead = this.bars.clear; plan.bass = this.bars.clearBass;
          bpm = PROGRAMS[this.programId].bpm;
        } else {
          const entry = this.global.stingers[stinger === S_CLEAR ? S_VICTORY : stinger];
          plan.lead = entry[0]; plan.bass = entry[1]; bpm = entry[2];
        }
        if (stinger === S_CLEAR) this.level = EXPLORE;
      } else if (this.kind === MENU) {
        const bar = this.menuBar++ % TITLE.motif.length;
        plan.lead = this.global.title.lead[bar]; plan.bass = this.global.title.bass[bar];
        bpm = TITLE.bpm;
      } else {
        this.commitLevel(time);
        const level = this.level, bar = this.barCount++ & 3, p = PROGRAMS[this.programId];
        plan.level = level;
        plan.lead = this.bars.lead[level][bar];
        if (level === COMBAT && bar !== 3 && time - this.killAt < KILL_WINDOW) {
          plan.lead = this.bars.kill[bar]; this.killAt = -1e9;
        }
        plan.bass = this.bars.bass[level][bar];
        bpm = level === COMBAT ? p.bpm * 1.5 : level === DANGER ? p.bpm * .75 : p.bpm;
      }
      plan.stepDur = 60 / bpm / 4;
      plan.end = start + 16 * plan.stepDur;
      this.voices[LEAD].done = this.voices[BASS].done = false;
      this.stats[S_BARS]++;
      if (plan.bass && !this.bassOwned) this.acquireBass();
      if (!this.leadOwned) this.leadOwned = true;
    }

    service(now, time) {
      this.stats[S_SERVICES]++;
      const plan = this.plan;
      if (!plan.active) {
        if (this.nextStart === 0 || now > this.nextStart + RESYNC)
          this.nextStart = now + START_LEAD;
        if (now < this.nextStart - PREPARE_LEAD) {
          this.wake = this.nextStart - PREPARE_LEAD;
          return;
        }
        this.prepare(this.nextStart, time);
      }
      let pending = false, wake = Infinity;
      for (let part = 0; part < 2; part++) {
        const voice = this.voices[part];
        if (voice.done) continue;
        const seg = part === LEAD ? plan.lead : plan.bass;
        if (!seg) { voice.done = true; continue; }
        const when = plan.start + seg.start * plan.stepDur;
        if (now >= when - MIN_LEAD) {
          voice.done = true; this.stats[S_MISSES]++;
          continue;
        }
        if (now < voice.segEnd || !this.voiceFree(part)) {
          pending = true;
          const next = voice.segEnd > now ? voice.segEnd : now;
          if (next < wake) wake = next;
          continue;
        }
        this.schedule(voice, part, seg, when, plan.stepDur, now);
        voice.done = true;
      }
      if (!pending) {
        plan.active = false;
        this.nextStart = plan.end;
        wake = plan.end - PREPARE_LEAD;
      }
      this.wake = wake;
    }

    voiceFree(part) {
      if (part === BASS) return this.bassOwned;
      const voice = this.bank.voices[1];
      return this.leadOwned && !(voice.stopAt > this.state.wallTime);
    }

    schedule(voice, part, seg, when, stepDur, now) {
      const target = part === LEAD ? this.bank.voices[1] : this.bank.engine;
      const gain = target.gainParameter, frequency = target.frequencyParameter;
      const duration = (seg.n - 1) * stepDur / seg.k;
      /* An effect may have left its own held curves on the lead voice. */
      if (part === LEAD && target.startAt !== voice.effectStamp) {
        voice.curves = voice.pitchCurve = true; voice.base = -1;
      }
      try {
        if (voice.curves) { gain.cancelScheduledValues(now); this.stats[S_CANCELS]++; }
        if (voice.pitchCurve) { frequency.cancelScheduledValues(now); this.stats[S_CANCELS]++; }
        voice.curves = voice.pitchCurve = false;
        if (seg.pitch) {
          frequency.setValueCurveAtTime(seg.pitch, when, duration);
          voice.pitchCurve = true; this.stats[S_PITCH]++;
        } else if (voice.base !== seg.hz) {
          frequency.value = seg.hz; voice.base = seg.hz; this.stats[S_BASE]++;
        }
        gain.setValueCurveAtTime(seg.gain, when, duration);
        voice.curves = true;
        voice.segEnd = when + duration;
        if (part === LEAD) voice.effectStamp = target.startAt;
        this.stats[S_SEGMENTS]++;
        if (seg.n > this.stats[S_MAX_POINTS]) this.stats[S_MAX_POINTS] = seg.n;
        if (when + duration - now > this.maxHorizon) this.maxHorizon = when + duration - now;
      } catch (_) {
        voice.segEnd = 0; this.stats[S_REFUSED]++;
      }
    }

    clearVoice(voice, target, now) {
      try {
        if (voice.curves) target.gainParameter.cancelScheduledValues(now);
        if (voice.pitchCurve) target.frequencyParameter.cancelScheduledValues(now);
      } catch (_) { this.stats[S_REFUSED]++; }
      voice.curves = voice.pitchCurve = false;
      voice.segEnd = 0; voice.base = -1;
    }

    // ---- voice ownership
    acquireBass() {
      const engine = this.bank.engine;
      if (!engine) return;
      engine.gainParameter.value = 0;
      engine.level = 0;
      this.voices[BASS].base = -1;
      this.bassOwned = true;
      this.bank.engineRole = "music";
    }

    releaseBass(now) {
      if (!this.bassOwned) return;
      const engine = this.bank.engine;
      if (engine) {
        this.clearVoice(this.voices[BASS], engine, now);
        engine.level = -1; engine.frequency = -1;
      }
      this.bank.engineStep = -1;
      this.bassOwned = false;
    }

    releaseLead(now) {
      if (!this.leadOwned) return;
      const target = this.bank.voices[1];
      if (target) this.clearVoice(this.voices[LEAD], target, now);
      this.leadOwned = false;
    }

    /* SoundBank.play(): effects keep priority over the lead. */
    sfxVoice(voices, next) {
      if (!this.leadOwned || voices.length < 2) return voices[next % voices.length];
      const time = this.state.wallTime, first = voices[0], second = voices[1];
      if (!(first.stopAt > time)) return first;
      if (second.stopAt > time) return voices[next % voices.length];
      const voice = this.voices[LEAD];
      if (voice.curves || voice.pitchCurve) {
        this.clearVoice(voice, second, this.bank.context.currentTime);
        this.stats[S_STEALS]++;
      }
      voice.base = -1; voice.segEnd = 0;
      return second;
    }

    /* SoundBank.heartbeat(): the danger bass already beats. */
    heartbeat() {
      return this.kind === GAME && this.bassOwned && this.level === DANGER;
    }

    // ---- tests and tooling; never per frame
    snapshot() {
      const stats = {};
      for (let at = 0; at < STAT_NAMES.length; at++) stats[STAT_NAMES[at]] = this.stats[at];
      return {kind: KINDS[this.kind], level: LEVELS[this.level],
        desired: LEVELS[this.desired], program: this.programId, mode: this.mode,
        queued: Array.from(this.queue.subarray(0, this.queued), (at) => STINGER_NAMES[at]),
        leadOwned: this.leadOwned, bassOwned: this.bassOwned,
        nextStart: this.nextStart, wake: this.wake, maxHorizon: this.maxHorizon,
        poolFloats: [this.gamePool.used, this.globalPool.used], stats};
    }
  }

  const api = {create(bank, env) { return new Conductor(bank, env); }};
  /* Tables and build helpers for the tests and the offline preview
     renderer, which evaluate this file themselves and set the harness flag
     game.js's harnesses do; a page never gets them. */
  if (globalThis.__treadlineEnableDebug === true) Object.assign(api, {
    levels: LEVELS, programs: PROGRAMS, stingers: STINGER_NAMES,
    constants: {PREPARE_LEAD, MIN_LEAD, EVAL_PERIOD, COMBAT_HOLD, CALM_HOLD,
      MIN_BARS, HORIZON, POINTS, DANGER_IN, DANGER_OUT, ALERT_IN, ALERT_OUT},
    buildProgram(id, volume = 1) { return buildProgram(new Pool(GAME_FLOATS), id, volume); },
    buildGlobal(volume = 1) { return buildGlobal(new Pool(GLOBAL_FLOATS), volume); },
  });
  Object.defineProperty(globalThis, "__treadlineMusic", {
    value: Object.freeze(api), configurable: false, writable: false,
  });
})();
