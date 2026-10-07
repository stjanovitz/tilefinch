/* Treadline Arena: "The Line" campaign, menu screens and range radio.

   Loaded before game.js. game.js attaches its internals once at boot
   (attach) and calls bounded hooks: arena(index) after arena placement,
   owns(index) to ask whether a mission places that arena, tick(dt) once per
   simulation step, damage(...) before armor is removed, objective(offer)
   for the marked objective, hidesHits() and concealed(id) for the aim
   guide and hit indicators, and showMenu(name) when the online flow
   returns to the Multiplayer screen. The menu screens own the panel's
   heading, message and groups; game.js keeps the stats line, Deploy's
   label and which online group is live. In qualification and test runs
   attach also lends the harness surface (tooling(): the menu, save,
   missions and bridge()), through which the long soak opens Quick Match.
   Missions are data plus small rule handlers that reuse the existing tanks,
   barriers, hazards, crates, pickups, convoy, palettes and HUD toast.
   Nothing here adds a draw call, a texture or steady-state allocation;
   menus are ordinary DOM.

   Determinism: every simulation input a mission needs (mission, faults,
   difficulty, Vesper's adaptation, the ghost loadout, sidegrades) is packed
   into one 32-bit code carried in state.dailyDay, which replays already
   record. Missions never read the save or the clock while simulating. */
(() => {
  "use strict";
  /* Practice Range (practice.js, loaded first): its hooks are forwarded
     from arena/tick/damage while it runs, and it lends screens to the
     menu. Its section of the save rides in "pr". */
  const P = globalThis.__treadlinePractice;

  // ---------------------------------------------------------------- text --
  const WHO = {M: "MARSHAL", V: "VESPER", S: "SUNSET", Y: "YOU"};
  const THEATERS = [
    {n: "PROVING YARD", sub: "Where it started.", tint: [1, 1, 1]},
    {n: "FOUNDRY", sub: "Where they learn.", tint: [1.12, .78, .65]},
    {n: "GLACIER", sub: "Where things slow down.", tint: [1.3, 1.5, 1.6]},
    {n: "NIGHT CITY", sub: "Where the names are.", tint: [.42, .58, .86]},
    {n: "FORTRESS", sub: "Where it ends.", tint: [.92, .58, .52]},
  ];
  /* mode: 0 Survival, 1 Team Control, 2 Convoy. arena: authored index.
     brief[0] is the Brief-radio line. bark is the first HUD line (<=20
     HUD glyphs). med: [challenge, mastery]. music: the program music.js
     plays (else its theater's); stealth: foes hide until they fire, so
     the music never announces one; noClear: no arena-clear ending;
     keepFoes: the game mode's own foes stay (the campaign adds its own). */
  const MISSIONS = [
    {id: "1-1", n: "Roll Call", mode: 0, arena: 0,
      obj: "Destroy the four range targets.",
      med: ["No wall bumps", "Under 60 seconds"],
      brief: ["M|RECRUIT. You're the first login in 6,914 days. I kept the range clean.",
        "M|Players online: 1. Players worldwide: also 1, I suspect. Don't let it go to your head.",
        "M|Targets are up. Drive, aim, shoot. The usual order works best."],
      bark: "M- DRILL",
      win: ["M|Clean run. The targets have filed no complaints."]},
    {id: "1-2", n: "Bank Statement", mode: 0, arena: 0,
      obj: "These targets only take banked shells.",
      med: ["Three double-bounce hits", "No misses"],
      brief: ["M|Mirror armor. Only a shell off a wall gets through.",
        "M|Walls aren't obstacles. They're accounts. Make deposits."],
      bark: "M- BANK IT",
      win: ["M|Your balance is positive. First time for everything."]},
    {id: "1-3", n: "Old Drills", mode: 0, arena: 0,
      obj: "Clear the old regulars. Survival rules.",
      med: ["Take no damage", "Under 2:00"],
      brief: ["M|KAZ_99, PSPGOD and xXshellXx are still on the roster.",
        "M|Their players stopped logging in. Their tanks didn't get the memo.",
        "M|Go easy. They're running on habit."],
      bark: "M- OLDIES",
      win: ["M|They'll be back at the top of the hour. They always are."]},
    {id: "1-4", n: "Gadget Day", mode: 0, arena: 0, stages: 5,
      obj: "Five waves. A new gadget every wave.",
      med: ["Use every gadget", "No armor pickups"],
      brief: ["M|Gadget Day! Every wave a new toy. Every toy a new way to lose. Kidding.",
        "M|Mostly kidding."],
      bark: "M- NEW TOYS",
      win: ["M|Five gadgets, one recruit, zero incidents. A good day."]},
    {id: "1-5", n: "Vesper", mode: 0, arena: 0, music: "vesper", noClear: true,
      obj: "Duel VESPER. She withdraws at half armor.",
      med: ["Lose at most one armor plate (25%)", "Win in 90 seconds"],
      brief: ["M|One more on the roster. VESPER. Number one for 41,000 matches.",
        "M|Nobody watched any of them. She doesn't seem to mind. Hard to tell.",
        "V|New login. Let's see what you remember."],
      bark: "V- SHOW ME",
      win: ["V|Again tomorrow.",
        "V|Forty-one thousand wins. You're the first one who was watching."]},
    {id: "2-1", n: "Hot Floor", mode: 0, arena: 1,
      obj: "Clear the Foundry. Oil slicks, chain crates.",
      med: ["Three crate-chain kills", "Never slide into oil"],
      brief: ["M|The Foundry runs hot. Oil on the floor, crates that go bang.",
        "M|Use the floor. Don't become part of it."],
      bark: "M- FLOOR",
      win: ["M|You let the floor do the work. That's called learning."]},
    {id: "2-2", n: "Copycat", mode: 0, arena: 1,
      obj: "These bots copy the last gadget you used.",
      med: ["Use your gadget at most twice", "Under 2:30"],
      brief: ["M|They learn from you. That's what you're for. That's what anybody's for.",
        "M|Use your gadget and they'll carry one too. Choose carefully."],
      bark: "M- ECHO",
      win: ["M|Teach them something useful next time. Like losing."]},
    {id: "2-3", n: "Demolition", mode: 0, arena: 1, noClear: true, limit: 120,
      obj: "Break all six barriers before time runs out.",
      med: ["Main cannon only", "Under 60 seconds"],
      brief: ["M|Six barriers between you and the exit. The building manager wants them gone.",
        "M|There is no building manager. I'm the building manager."],
      bark: "M- KNOCK",
      win: ["M|Every route open. Very good. Very loud."]},
    {id: "2-4", n: "Teaching Day", mode: 1, arena: 1,
      obj: "Hold the transmitter with two Cadet allies.",
      med: ["No ally lost", "Win by 30%"],
      brief: ["M|Two Cadets want to learn from the best. You'll have to do.",
        "M|Hold the transmitter. They'll copy you, so do something good."],
      bark: "M- LEAD ON",
      win: ["M|They copied you. Some of it was even on purpose."]},
    {id: "2-5", n: "Vesper Learns", mode: 0, arena: 1, music: "vesper", noClear: true,
      obj: "VESPER fights with your favourite trick.",
      med: ["Win with a gadget she didn't copy", "Take no damage"],
      brief: ["V|I watched your runs. All of them. There aren't many.",
        "V|I borrowed your favourite trick. Let's see if you have another."],
      bark: "V- GO ON",
      win: ["V|Huh. You did something I haven't seen.",
        "M|She'll be up all night. She's always up all night."]},
    {id: "3-1", n: "Cold Start", mode: 0, arena: 2,
      obj: "Treads and cannon start cold. They warm up in 40 s.",
      med: ["A kill before warm-up", "Never reverse"],
      brief: ["M|Glacier sector. Everything starts slow out here. Your treads. Your cannon. Me.",
        "M|Give it a minute. Warm up. Then show off."],
      bark: "M- WARM UP",
      win: ["M|There. Old systems still run. They just need a minute."]},
    {id: "3-2", n: "The Archive", mode: 2, arena: 2, keepFoes: true,
      obj: "Escort the archive crawler through the gate.",
      med: ["Convoy undamaged", "Under 2:00"],
      brief: ["M|That truck carries every score ever set here. Drive like it matters.",
        "M|It's the only copy."],
      bark: "M- EASY",
      win: ["M|Archive secured. Every score. Even the bad ones. Especially those."]},
    {id: "3-3", n: "Repeat Briefing", mode: 0, arena: 2, stages: 3,
      obj: "Hold the ridge. Three times. One rule changes each time.",
      med: ["No redeploys", "All three under 2:30"],
      brief: ["M|Hold the ridge until-- did I say that already? I said that.",
        "M|My memory card is older than you think."],
      bark: "M- RIDGE",
      win: ["M|Three times, same ridge. You held it every time. I remember that part."]},
    {id: "3-4", n: "Whiteout", mode: 0, arena: 2, stealth: true,
      obj: "Foes are hidden in the snow until they fire.",
      med: ["Every kill on a revealed foe", "No Shield"],
      brief: ["M|Whiteout. My sensors are older than the snow.",
        "M|You'll see them when they shoot. Make it count."],
      bark: "M- EYES UP",
      win: ["M|You saw them first. Out here that's most of the job."]},
    {id: "3-5", n: "Thaw", mode: 0, arena: 2, stages: 5,
      obj: "Survive the waves while the barriers collapse. Three waves clears.",
      med: ["Survive all five waves", "Never still for 3 s"],
      brief: ["M|The ice is going. So are the walls. Things don't last. That's fine.",
        "M|Keep moving. Standing still is how the snow gets you."],
      bark: "M- MOVE",
      win: ["M|The thaw can have the rest."]},
    {id: "4-1", n: "Lights Out", mode: 0, arena: 1, stealth: true,
      obj: "Foes are dark until they fire. So are you.",
      med: ["No damage in the first 20 s", "No Smoke"],
      brief: ["M|Night City. The streetlights went out in an update nobody installed.",
        "M|They can't see you either. Be a rumour."],
      bark: "M- DARK",
      win: ["M|Nobody saw a thing. Nobody ever does, here."]},
    {id: "4-2", n: "Names on the Wall", mode: 2, arena: 2, keepFoes: true, limit: 100,
      obj: "Defend the old scoreboard until the cleanup script gives up.",
      med: ["Monument at 100%", "Three bank kills"],
      brief: ["M|That's the old scoreboard. Every callsign that ever topped a table.",
        "M|A cleanup script wants it gone. Hold the line."],
      bark: "M- NAMES",
      win: ["M|Still standing. KAZ_99 would be proud, if KAZ_99 still logged in."]},
    {id: "4-3", n: "Last Server", mode: 1, arena: 1,
      obj: "Team Control beside your own ghost from 1-3.",
      med: ["Out-score your ghost", "Your ghost lands no kills"],
      brief: ["M|That's you from Tuesday. You were worse. Be kind to him.",
        "M|The server kept your old drill. It won't hold a grudge."],
      bark: "M- HI",
      win: ["M|You beat Tuesday. Wednesday is looking forward to it."]},
    {id: "4-4", n: "Ambush Alley", mode: 2, arena: 2,
      obj: "Reverse drill: stop the cleanup crawler before the far gate.",
      med: ["Stop it before halfway", "Mines only on the crawler"],
      brief: ["M|Reverse drill. That crawler is deleting old replays.",
        "M|Stop it before the far gate. Mines love a slow target."],
      bark: "M- STOP IT",
      win: ["M|Crawler stopped. Somebody's terrible replay from 2009 lives on."]},
    {id: "4-5", n: "Vesper's Question", mode: 0, arena: 0, music: "vesper", noClear: true,
      obj: "One more duel. Then answer: gold for 'I missed it', cyan for 'I wanted to see'.",
      med: ["Win without the main cannon", "Take no damage"],
      brief: ["V|One more round. Then I want to ask you something."],
      bark: "V- DUEL",
      win: ["V|Why did you come back?", "Y|(answer)", "V|Me too."]},
    {id: "5-1", n: "Notice", mode: 0, arena: 1, music: "sunset", noClear: true,
      obj: "Survive as zones power off. Reinforcements arrive with each.",
      med: ["Finish within 20 s of the last zone", "Under 2:00"],
      brief: ["S|RANGE UNUSED. SCHEDULING SHUTDOWN.",
        "M|I filed an appeal. It's been pending a while.",
        "M|Zones are going dark. Finish the drill before the lights do."],
      bark: "S- NOTICE",
      win: ["M|Drill complete. Appeal still pending. Legally that's a draw."]},
    {id: "5-2", n: "Salvage", mode: 0, arena: 2, noClear: true, limit: 100,
      obj: "Collect memory cards before their zones go dark. Three clears.",
      med: ["All five cards", "No repairs"],
      brief: ["M|Memory cards all over the fortress. Grab them before the zones go dark.",
        "M|You can't carry everything. Nobody can. Carry what you can."],
      bark: "M- CARDS",
      win: ["M|Saved. Not all of it. Enough of it."]},
    {id: "5-3", n: "Allies", mode: 0, arena: 0, music: "ally", stages: 3,
      obj: "VESPER on your wing against SUNSET's drones.",
      med: ["VESPER undamaged", "More kills than VESPER"],
      brief: ["V|SUNSET is sending drones. I'd like to stay. Cover me?",
        "M|VESPER on your wing. I never thought I'd see that."],
      bark: "V- WITH YOU",
      win: ["V|That was fun. Is that what it's for?"]},
    {id: "5-4", n: "Sunset", mode: 0, arena: 1, music: "sunset", noClear: true,
      obj: "Hold off SUNSET: treads, then turret, then core.",
      med: ["Both treads before the turret", "No gadget"],
      brief: ["S|FINAL NOTICE. RANGE 7 WILL POWER DOWN.",
        "M|You can't beat a schedule. You can make it wait.",
        "M|Treads, then turret, then whatever's left. Go."],
      bark: "S- FINAL",
      win: ["S|SHUTDOWN POSTPONED.",
        "M|You can't save everything. You saved tonight."]},
    {id: "5-5", n: "Last Match", mode: 0, arena: 0, music: "last", noClear: true, quiet: true,
      obj: "No timer. No score. Pause and leave whenever you like.",
      med: null,
      brief: ["V|No timer. No score. Just a match.",
        "M|Press START when you're ready to go. No rush. There's never a rush now."],
      bark: "V- PLAY",
      win: ["V|I think I was practicing for you.",
        "M|Same time tomorrow? No pressure. I'll be here either way."]},
  ];
  const LORE = [
    "PROVING RANGE 7. Built for a war that only ever happened on 4.3-inch screens.",
    "SCOREBOARD. KAZ_99 999,990. PSPGOD 871,200. xXshellXx 640,075. Last updated a long time ago.",
    "VESPER. Ace personality. 41,000 matches. Zero spectators, until recently.",
    "MARSHAL v1.0.3. Changelog: 'Fixed typo in greeting.' No later entries.",
    "MEMORY CARD, 8 MB. Contents: settings, three saves, one half-finished callsign.",
    "THE ARCHIVE. Every score set here, including the one where somebody reversed for four minutes.",
    "NIGHT CITY. Streetlights disabled by update 2.71, which nobody installed except the streetlights.",
    "SUNSET. Facilities scheduler. Not evil. Just very organised.",
  ];
  const REPRISE_LINE = "M|Same drills, new ground. That's most of life.";
  /* In-play radio uses the retained HUD toast, which has room for about a
     dozen glyphs next to the standard HUD; the fixture checks every entry
     against the HUD primitive budget. */
  const TOAST = {
    gadgetDay: ["", "SMOKE", "SHIELD", "DRONE", "BOOST"],
    waves: ["", "WAVE 2", "WAVE 3", "WAVE 4", "WAVE 5"],
    noGadget: "NO TOYS", halfArmor: "HALF HP", ridge: "M- RIDGE",
    more: "V- MORE", last: "V- LAST", bankIt: "BANK IT",
    off: {KAZ_99: "KAZ 99", PSPGOD: "PSPGOD", xXshellXx: "SHELL"},
    rewind: "REWIND", cardLogged: "CARD", cardFound: "CARD",
    why: "V- WHY", pick: "V- PICK", meToo: "V- ME TOO", tomorrow: "V- SOON",
    huh: "V- HUH", warm: "M- WARM", zone: "S- ZONE", postponed: "S- LATER",
    again: "V- AGAIN",
  };
  const FAULTS = [
    // name, multiplier, cards needed, note
    ["Glass Treads", 1.5, 1, "One hit destroys any tank, yours included."],
    ["Ricochet Only", 1.4, 13, "Your direct hits deal no damage."],
    ["Iron Rain", 1.2, 2, "No armor pickups."],
    ["Old Wounds", 1.2, 4, "Deploy at half armor."],
    ["Fog of Memory", 1.3, 10, "No hit or damage indicators."],
    ["Low Battery", 1.3, 16, "Two-minute limit. The range dims."],
    ["Echo", 1.2, 6, "Foes carry your gadget."],
    ["Mirror", 1, 8, "Deploy on the far side."],
    /* Slot 8 was Toy Box, retired because larger hulls broke collision.
       It stays empty so Second Chance keeps bit 512 in saves and run
       codes; no save or run carries bit 256, and the menu skips it. */
    null,
    ["Second Chance", .8, 19, "Once per mission, lethal damage rewinds you 5 s."],
  ];
  const GLASS = 1, RICO = 2, IRON = 4, WOUNDS = 8, FOG = 16, BATTERY = 32,
    ECHO = 64, MIRROR = 128, SECOND = 512;
  const TAG = 0x80000000;
  const FAULT_BITS = 1023 & ~256;

  // ---------------------------------------------------------------- save --
  const SAVE_KEY = "treadline-campaign-v1";
  const save = {v: 1, d: -1, r: 0, rr: 1, cs: "YOU", m: [], k: 0, b: [], f: 0,
    g: [0, 0, 0, 0, 0], sh: 0, ri: 0, last: 0, e: 0, x: 0, gh: [1, 0],
    rep: 0, ans: -1, best: null, corrupt: false, mu: 2, mv: 1};
  function resetSave() {
    save.d = -1; save.r = 0; save.rr = 1; save.cs = "YOU"; save.k = 0;
    save.f = 0; save.sh = save.ri = save.last = save.e = save.x = 0;
    save.gh = [1, 0]; save.rep = 0; save.ans = -1; save.best = null;
    save.m = MISSIONS.map(() => 0); save.b = MISSIONS.map(() => 0);
    save.g = [0, 0, 0, 0, 0];
  }
  const int = (v, lo, hi, fallback) =>
    Number.isInteger(v) && v >= lo && v <= hi ? v : fallback;
  function loadSave(text) {
    resetSave(); save.corrupt = false;
    if (P) P.load(null);
    if (text === undefined) {
      try { text = localStorage.getItem(SAVE_KEY); } catch (_) { text = null; }
    }
    if (!text) return save;
    let raw = null;
    try { raw = JSON.parse(text); } catch (_) { raw = null; }
    if (!raw || typeof raw !== "object" || raw.v !== 1) {
      save.corrupt = true; return save;
    }
    save.d = int(raw.d, -1, 2, -1); save.r = int(raw.r, 0, 2, 0);
    save.rr = int(raw.rr, 0, 1, 1);
    if (typeof raw.cs === "string" && /^[A-Z]{3}$/.test(raw.cs)) save.cs = raw.cs;
    for (let at = 0; at < MISSIONS.length; at++) {
      save.m[at] = Array.isArray(raw.m) ? int(raw.m[at], 0, 15, 0) : 0;
      save.b[at] = Array.isArray(raw.b) ? int(raw.b[at], 0, 99999999, 0) : 0;
    }
    save.k = int(raw.k, 0, 0x1ffffff, 0); save.f = int(raw.f, 0, 1023, 0) & FAULT_BITS;
    for (let at = 0; at < 5; at++)
      save.g[at] = Array.isArray(raw.g) ? int(raw.g[at], 0, 1e6, 0) : 0;
    save.sh = int(raw.sh, 0, 1e9, 0); save.ri = int(raw.ri, 0, 1e9, 0);
    save.last = int(raw.last, 0, 1e15, 0); save.e = int(raw.e, 0, 1, 0);
    save.x = int(raw.x, 0, 0x1ffffff, 0); save.rep = int(raw.rep, 0, 1, 0);
    save.ans = int(raw.ans, -1, 1, -1);
    // Older saves may carry lk (the retired Classic/Enhanced look): ignored.
    // Music (music.js): 0 Off, 1 Menus, 2 Full; volume 0 Low, 1 Mid, 2 High.
    save.mu = int(raw.mu, 0, 2, 2); save.mv = int(raw.mv, 0, 2, 1);
    if (Array.isArray(raw.gh))
      save.gh = [int(raw.gh[0], 0, 2, 1), int(raw.gh[1], 0, 4, 0)];
    if (raw.best && int(raw.best.i, 0, 24, -1) >= 0
        && typeof raw.best.c === "string" && raw.best.c.length < 24000)
      save.best = {i: raw.best.i, c: raw.best.c};
    if (P) P.load(raw.pr);
    return save;
  }
  function writeSave() {
    const out = {v: 1, d: save.d, r: save.r, rr: save.rr, cs: save.cs,
      m: save.m, k: save.k, b: save.b, f: save.f, g: save.g, sh: save.sh,
      ri: save.ri, last: save.last, e: save.e, x: save.x, gh: save.gh,
      rep: save.rep, ans: save.ans, best: save.best,
      mu: save.mu, mv: save.mv, pr: P ? P.saved() : undefined};
    const text = JSON.stringify(out);
    try { localStorage.setItem(SAVE_KEY, text); return true; } catch (_) {
      // A full store keeps progress in RAM; drop the optional replay first.
      if (save.best) {
        out.best = null;
        try { localStorage.setItem(SAVE_KEY, JSON.stringify(out)); } catch (_) {}
      }
      return false;
    }
  }
  loadSave();
  const medalCount = () => save.m.reduce((n, bits) =>
    n + (bits & 1) + (bits >> 1 & 1) + (bits >> 2 & 1), 0);
  const cardCount = () => {
    let n = 0; for (let k = save.k; k; k &= k - 1) n++; return n;
  };
  const cleared = (at) => (save.m[at] & 1) !== 0;
  const unlocked = (at) => at === 0 || cleared(at - 1) || save.e === 1;
  const nextMission = () => {
    for (let at = 0; at < MISSIONS.length; at++) if (!cleared(at)) return at;
    return MISSIONS.length - 1;
  };
  const sidegrades = () => (medalCount() >= 10 ? 1 : 0) | (medalCount() >= 20 ? 2 : 0);
  const faultUnlocked = (at) => FAULTS[at] !== null && FAULTS[at][2] <= cardCount();
  const unlockedFaults = () => {
    let mask = 0;
    for (let at = 0; at < FAULTS.length; at++) if (faultUnlocked(at)) mask |= 1 << at;
    return mask;
  };

  // ---------------------------------------------------------- run code --
  function encode(o) {
    return (TAG | (o.i & 31) | (o.rep ? 32 : 0) | (o.d & 3) << 6
      | (o.f & 1023) << 8 | (o.vg & 7) << 18 | (o.vr & 3) << 21
      | (o.vb ? 1 << 23 : 0) | (o.sg & 3) << 24 | (o.gc & 3) << 26
      | (o.gg & 7) << 28) >>> 0;
  }
  function decode(v) {
    if (!Number.isInteger(v) || v < TAG || v > 0xffffffff) return null;
    const i = v & 31;
    if (i >= MISSIONS.length) return null;
    return {i, rep: (v & 32) !== 0, d: Math.min(2, v >>> 6 & 3),
      f: v >>> 8 & FAULT_BITS, vg: v >>> 18 & 7, vr: Math.min(2, v >>> 21 & 3),
      vb: (v >>> 23 & 1) === 1, sg: v >>> 24 & 3, gc: Math.min(2, v >>> 26 & 3),
      gg: Math.min(4, v >>> 28 & 7)};
  }
  /* Vesper studies the save once, before deploy; the result travels in the
     code so a replay never depends on whose memory card is inserted. */
  function runCode(at, faults) {
    let fav = 0;
    for (let g = 1; g < 5; g++) if (save.g[g] > save.g[fav]) fav = g;
    const cls = B ? B.state.classChoice : 1;
    return encode({i: at, rep: save.rep && save.e, d: save.rep && save.e ? 2 : Math.max(0, save.d),
      f: MISSIONS[at].quiet ? 0 : faults & unlockedFaults(), vg: save.g[fav] ? fav : 7, vr: cls === 0 ? 0 : cls === 2 ? 2 : 1,
      vb: save.sh > 20 && save.ri * 8 > save.sh, sg: sidegrades(),
      gc: save.gh[0], gg: save.gh[1]});
  }

  // ------------------------------------------------------------ runtime --
  let B = null;
  const M = {on: false, armed: false, code: 0, c: null, m: null, idx: -1,
    t: 0, stage: 0, stages: 1, pendingStage: false, f: 0, mult: 1,
    replay: false, ended: false, won: false,
    bumps: 0, lastBlocked: 0, gUses: 0, gMask: 0, lastGc: 0, sUses: 0,
    lastSc: 0, reversed: false, still: 0, maxStill: 0, px: 0, pz: 0,
    lastHp: 0, armorPickups: 0, lastLives: 3, deaths: 0, shellHits: 0,
    shots0: 0, doubleHits: 0, crateKills: 0, revealed: 0, kills: 0,
    allyLost: 0, watchHurt: false, flag: false, flag2: false, mark: 0,
    killsBy: new Uint8Array(6), dummy: 0, bankOnly: 0, immortal: 0, camo: 0,
    withdraw: -1, withdrawAt: 0, yaw: new Float32Array(6),
    respawnAt: new Float32Array(6), cfg: [], revealAt: new Float32Array(6),
    barkText: ["", "", "", ""],
    barkHead: 0, barkCount: 0, barkReady: 0, warnAt: 0,
    rewind: new Float32Array(24), rewindAt: 0, rewound: false,
    rewindPending: false, card: -1, cardX: 0, cardZ: 0, cardFound: false,
    salvage: 0, crates: 0, convoyHp: 0, mineHits: 0, bulletHits: false,
    zone: 0, zoneAt: 0, winAt: 0, answer: -1, names: ["", "", "", "", "", ""],
    radio: 0, noClear: false, victory: false, bankHits: 0, lastCommand: 0,
    lastKills: 0, lastCooldown: 0, dim: 0, light: null, salvageSlots: null,
    result: null, quickMode: -1};

  function restoreLooks() {
    if (!B) return;
    B.restoreTankTints();
    for (let id = 0; id < 6; id++) {
      const tank = B.tanks[id];
      tank.aggression = .8 + (id % 3) * .2;
      tank.preferredRange = 3.1 + (id % 3) * .45;
    }
  }
  /* The scenery tint is one uniform. Borrow the user's palette slot for a
     moment so the theater light needs no new state in game.js. */
  function sceneryTint(rgb, scale = 1) {
    const slot = B.PALETTE_TINTS[B.preferences.palette];
    const r = slot[0], g = slot[1], b = slot[2];
    slot[0] = rgb[0] * scale; slot[1] = rgb[1] * scale; slot[2] = rgb[2] * scale;
    B.applyPalette();
    slot[0] = r; slot[1] = g; slot[2] = b;
  }
  /* A campaign placement into out: game.js's placement search over five
     rings .55 apart, keeping radius + .08 from scenery, within 7.1 of the
     centre and 1.5 (squared) from every active tank but self. */
  function freeSpot(out, x, z, radius, self = null) {
    const tanks = B.tanks;
    const found = B.searchClearSpot(x, z, 5, .55, 8, (cx, cz) => {
      if (Math.abs(cx) > 7.1 || Math.abs(cz) > 7.1
          || B.circleHitsObstacle(cx, cz, radius + .08)) return true;
      for (let id = 0; id < 6; id++) {
        const t = tanks[id];
        if (!t.active || t === self) continue;
        const dx = t.x - cx, dz = t.z - cz;
        if (dx * dx + dz * dz < 1.5) return true;
      }
      return false;
    });
    out.x = B.clearSpot.x; out.z = B.clearSpot.z;
    return found;
  }
  const spot = {x: 0, z: 0};
  // Foes deploy on the far arc, never beside the player's spawn lane.
  const arcAngle = (k, n, shift = 0) =>
    (n > 1 ? -1.2 + 2.4 * k / (n - 1) : 0) + (shift & 1 ? .3 : shift ? -.3 : 0);
  const arcX = (k, n, shift) => Math.sin(arcAngle(k, n, shift)) * 5.5;
  const arcZ = (k, n, shift) => Math.cos(arcAngle(k, n, shift)) * 5.5;
  function skill(base) { return Math.max(0, Math.min(2, base + M.c.d - 1)); }
  /* Place a tank and remember its recipe for campaign respawns. */
  function foe(id, x, z, cls, o = {}) {
    const tank = B.tanks[id];
    const gadget = o.gadget === undefined ? B.GADGETS[id % 5] : o.gadget;
    freeSpot(spot, x, z, .62, tank);
    const yaw = o.yaw === undefined ? Math.atan2(-spot.x, -spot.z) : o.yaw;
    B.placeTank(tank, spot.x, spot.z, yaw, o.team === undefined ? 1 : o.team,
      gadget, o.role || "HUNTER", cls);
    tank.difficulty = o.skill === undefined ? skill(1) : o.skill;
    tank.aggression = o.aggr === undefined ? .8 + (id % 3) * .2 : o.aggr;
    tank.preferredRange = o.range === undefined ? 3.1 + (id % 3) * .45 : o.range;
    if (o.hp) tank.maxHealth = tank.health = o.hp;
    if (o.dummy) {
      M.dummy |= 1 << id; tank.aggression = 0; tank.driveSpeed = 0;
      tank.cooldown = tank.secondaryCooldown = tank.gadgetCooldown = 1e9;
      M.yaw[id] = yaw;
    }
    if ((M.f & ECHO) && tank.team !== B.tanks[0].team && !o.dummy && B.tanks[0].gadget)
      tank.gadget = B.tanks[0].gadget;
    if (o.tint) B.setTankTint(id, o.tint, o.flash || o.tint);
    // Camouflaged foes stay hidden from the aim guide until they fire.
    if (o.tint === SNOW_CAMO || o.tint === NIGHT_CAMO) M.camo |= 1 << id;
    else M.camo &= ~(1 << id);
    M.names[id] = o.name || "";
    M.cfg[id] = o; o.cls = cls; o.x = x; o.z = z;
    return tank;
  }
  function clearFoes() {
    for (let id = 1; id < 6; id++) {
      B.tanks[id].active = false; M.respawnAt[id] = 0; M.cfg[id] = null;
      M.names[id] = "";
    }
    M.dummy = 0;
  }
  const VIOLET = [.78, .42, 1];
  const DRONE = [.86, .84, .78], NIGHT_CAMO = [.035, .075, .095];
  const SNOW_CAMO = [.17, .42, .46], RED_FLASH = [1, .3, .16];
  const duelArmor = (hp) => Math.round(hp * [.8, 1, 1.2][M.c.d]);
  function vesper(id, team, o = {}) {
    const ranges = [2.8, 3.6, 4.6];
    o.cls = o.cls === undefined ? 1 : o.cls;
    o.skill = o.skill === undefined ? (M.c.d ? 2 : 1) : o.skill;
    o.aggr = o.aggr === undefined ? (M.c.vb ? 1.45 : 1.25) : o.aggr;
    o.range = o.range === undefined ? ranges[M.c.vr] : o.range;
    o.team = team; o.tint = VIOLET; o.name = "VESPER";
    if (o.gadget === undefined) o.gadget = "SHIELD";
    return foe(id, o.x === undefined ? 0 : o.x, o.z === undefined ? 5.2 : o.z,
      o.cls, o);
  }
  function placeCard(x, z) {
    for (let at = 0; at < 6; at++) {
      const pickup = B.pickups[at];
      if (pickup.active) continue;
      freeSpot(spot, x, z, .3);
      pickup.active = true; pickup.x = spot.x; pickup.z = spot.z;
      pickup.phase = 0; pickup.type = "COOLANT";
      return at;
    }
    return -1;
  }
  // Hidden cards sit in hard corners away from the usual spawn lanes.
  const CORNERS = [[-7, 7], [7, 7], [7, -7], [-7, -7], [-6.9, .2], [6.9, -.2]];
  function setBarrier(at, x, z, width, depth, health) {
    const barrier = B.barriers[at];
    barrier.present = barrier.active = width > 0;
    barrier.x = x; barrier.z = z; barrier.width = width; barrier.depth = depth;
    barrier.health = health;
    barrier.left = x - width * .5; barrier.right = x + width * .5;
    barrier.top = z - depth * .5; barrier.bottom = z + depth * .5;
  }
  function sceneryChanged() {
    B.refreshScenery(); B.dirty(false);
  }

  /* --- hook: arena(index) runs at the end of beginArena's placement. --- */
  function arena(index) {
    if (P) P.arena(index);
    const code = B.state.dailyDay;
    const c = decode(code);
    if (!c || !(M.armed || B.replaying())) {
      if (M.on) deactivate();
      return;
    }
    const m = MISSIONS[c.i];
    if (index !== missionArena(m, c)) return;
    const stageChange = M.pendingStage && M.on && M.code === code;
    M.pendingStage = false;
    if (!stageChange) begin(c, code);
    setup(m);
  }
  function missionArena(m, c) { return c.rep && m.mode === 0 ? 3 : m.arena; }
  /* Will arena(index) set up a mission? beginArena then keeps the ring
     spawn the missions (and their crate layouts) were tuned on. */
  function owns(index) {
    const c = decode(B.state.dailyDay);
    return !!c && (M.armed || B.replaying()) && index === missionArena(MISSIONS[c.i], c);
  }
  // Accepted generator seeds (validation mask 63), one per mission.
  const REPRISE_SEEDS = [0xcf9099b1, 0xa01c3367, 0x51a81dde, 0x51a83ccd, 0xcf9f220d,
    0x8b0e17b8, 0x51a8999a, 0x51a8b889, 0x8b0eba6b, 0x297510a3, 0x51a91556,
    0xcf9e4df4, 0x8b0f3e27, 0xcf9e0b92, 0x51a99112, 0x6dc74363, 0x6dc73d92,
    0x8b0f80cc, 0x51aa0cce, 0x51aa2bbd, 0x2977ac68, 0x6dc49af9, 0x6dc47be8,
    0x8b0cca6a, 0xcf9dbfd9];
  function begin(c, code) {
    const m = MISSIONS[c.i];
    restoreLooks();
    M.on = true; M.code = code; M.c = c; M.m = m; M.idx = c.i;
    M.replay = B.replaying(); M.ended = false; M.won = false;
    M.t = 0; M.stage = 0; M.stages = m.stages || 1; M.f = c.f;
    M.mult = 1;
    for (let at = 0; at < FAULTS.length; at++)
      if (M.f & 1 << at && FAULTS[at]) M.mult *= FAULTS[at][1];
    M.bumps = M.gUses = M.gMask = M.sUses = M.armorPickups = M.deaths = 0;
    M.reversed = M.watchHurt = M.flag = M.flag2 = M.rewound = false;
    M.rewindPending = M.cardFound = M.bulletHits = false;
    M.still = M.maxStill = M.shellHits = M.doubleHits = M.crateKills = 0;
    M.revealed = M.kills = M.allyLost = M.mark = M.salvage = M.zone = 0;
    M.mineHits = M.winAt = M.zoneAt = M.bankHits = M.lastCooldown = M.dim = 0;
    M.result = null;
    M.answer = -1; M.killsBy.fill(0); M.victory = false;
    M.noClear = !!m.noClear;
    M.lastCommand = B.state.commandMeter; M.lastKills = B.state.kills;
    M.barkHead = M.barkCount = 0; M.barkReady = 1.6; M.warnAt = 0;
    M.lastLives = B.state.lives; M.shots0 = B.state.shots; M.rewindAt = 0;
    M.rewind.fill(0);
    say(m.bark);
  }
  function deactivate() {
    M.on = false; M.m = null; M.c = null;
    restoreLooks();
    if (B) B.applyPalette();
  }

  function setup(m) {
    const st = B.state, tanks = B.tanks, player = tanks[0];
    const theater = THEATERS[+m.id[0] - 1];
    M.dummy = M.bankOnly = M.immortal = M.camo = 0; M.withdraw = -1;
    M.revealAt.fill(-9); M.card = -1;
    if (!m.keepFoes) clearFoes();
    else for (let id = 1; id < 6; id++) { M.cfg[id] = null; M.names[id] = ""; M.respawnAt[id] = 0; }
    B.restoreTankTints();
    let light = theater.tint;
    if (m.id === "3-4") light = [4.2, 4.6, 4.8];
    if (m.id === "4-1") light = [.34, .46, .7];
    if (m.id === "4-5") light = THEATERS[3].tint;
    M.light = light;
    sceneryTint(light);
    const s = M.stage, ace = M.c.d === 2, cadet = M.c.d === 0;
    // Cadet drops one foe; Ace keeps the count and sharpens the bots.
    const count = (n) => Math.max(1, Math.min(5, n - (cadet ? 1 : 0)));
    switch (m.id) {
      case "1-1": {
        const at = [[-1.4, -2.4], [1.8, -3.2], [2.1, 1.3], [-1.3, 5.3]];
        for (let k = 0; k < 4; k++) foe(k + 1, at[k][0], at[k][1], 0,
          {dummy: true, tint: B.GOLD_TINT, hp: 40, gadget: ""});
        break;
      }
      case "1-2": {
        const at = [[-1.6, 1.6], [3.6, 1.7], [-.3, 5.6]];
        for (let k = 0; k < 3; k++) foe(k + 1, at[k][0], at[k][1], 0,
          {dummy: true, tint: B.CYAN_TINT, hp: 18, gadget: ""});
        M.bankOnly = 14;
        break;
      }
      case "1-3": {
        const names = ["KAZ_99", "PSPGOD", "xXshellXx"];
        const colors = [[1, .23, .16], [1, .51, .08], [.16, .57, 1]];
        for (let k = 0; k < 3; k++) {
          foe(k + 1, arcX(k, 3), arcZ(k, 3), (k + 1) % 3,
            {skill: skill(0), name: names[k], tint: colors[k], aggr: cadet ? .55 : undefined,
              role: k === 1 ? "FLANK" : "HUNTER"});
        }
        break;
      }
      case "1-4": {
        player.gadget = B.GADGETS[s];
        const n = s >= 3 ? 2 : 1;
        for (let k = 0; k < n; k++)
          foe(k + 1, k ? -4.5 : 4.5, 5, k % 3, {skill: skill(0)});
        if (s) say(TOAST.gadgetDay[s], true);
        break;
      }
      case "1-5": case "2-5": {
        const learned = m.id === "2-5";
        vesper(1, 1, {gadget: learned && M.c.vg < 5 ? B.GADGETS[M.c.vg] : "SHIELD",
          skill: learned ? 2 : undefined, hp: duelArmor(learned ? 220 : 200)});
        M.withdraw = 1; M.withdrawAt = learned ? .25 : .5;
        break;
      }
      case "2-1": {
        for (let k = 0; k < 3; k++) {
          foe(k + 1, arcX(k, 3), arcZ(k, 3), k % 3, {skill: skill(0)});
        }
        for (const crate of B.crates) crate.type = 1;
        const oil = [[-2.6, 0], [2.6, 0], [0, 2.4]];
        for (const [x, z] of oil) B.spawnHazard(1, x, z, 1, -1);
        M.crates = activeCrates();
        break;
      }
      case "2-2": {
        for (let k = 0; k < count(3); k++) {
          foe(k + 1, arcX(k, count(3)), arcZ(k, count(3)), k % 3,
            {gadget: player.gadget});
        }
        break;
      }
      case "2-3": {
        setBarrier(4, 0, 4.6, 1.8, .38, 2);
        setBarrier(5, 0, -4.4, 1.8, .38, 2);
        sceneryChanged();
        for (let k = 0; k < (ace ? 3 : 2); k++)
          foe(k + 1, k ? -5 : 5, 5.5 - k, k % 3, {skill: skill(0), role: "GUARD"});
        break;
      }
      case "2-4": {
        foe(1, -2, -5.2, 1, {team: 0, role: "CAPTURE", skill: 0, gadget: "REPAIR DRONE"});
        foe(5, 2, -5.2, 0, {team: 0, role: "GUARD", skill: 0, gadget: "SHIELD"});
        for (let k = 0; k < 3; k++) {
          foe(k + 2, arcX(k, 3), arcZ(k, 3), k % 3,
            {role: k === 0 ? "CAPTURE" : k === 1 ? "FLANK" : "GUARD"});
        }
        break;
      }
      case "3-1": {
        for (let k = 0; k < count(3); k++) {
          foe(k + 1, arcX(k, count(3)), arcZ(k, count(3)), k % 3);
        }
        icePatches();
        break;
      }
      case "3-2": case "4-2": {
        // Convoy mode placed four attackers; keep fewer at lower difficulty.
        for (let id = 1; id < 5; id++) {
          const t = tanks[id];
          if (id > M.c.d + 2) t.active = false;
          else if (t.active) t.difficulty = skill(m.id === "3-2" ? 1 : 0);
        }
        B.convoy.health = [360, 270, 200][M.c.d] * (m.id === "4-2" ? 1.5 : 1);
        M.convoyHp = B.convoy.health;
        if (m.id === "3-2") icePatches();
        else B.convoy.progress = .45;
        break;
      }
      case "3-3": {
        for (let k = 0; k < count(2); k++)
          foe(k + 1, k ? -3.5 : 3.5, 5.6, (k + s) % 3, {skill: skill(0)});
        if (s === 1) { player.gadget = ""; say(TOAST.noGadget, true); }
        if (s === 2) {
          player.health = Math.ceil(player.maxHealth / 2);
          say(TOAST.halfArmor, true);
        }
        if (s) say(TOAST.ridge);
        icePatches();
        break;
      }
      case "3-4": case "4-1": {
        const camo = m.id === "3-4" ? SNOW_CAMO : NIGHT_CAMO;
        const n = count(3);
        for (let k = 0; k < n; k++) {
          foe(k + 1, arcX(k, n), arcZ(k, n), k % 3,
            {tint: camo, flash: RED_FLASH,
              role: k % 3 === 1 ? "FLANK" : k % 3 === 2 ? "GUARD" : "HUNTER"});
        }
        if (m.id === "3-4") icePatches();
        break;
      }
      case "3-5": {
        const n = Math.min(5, 1 + (s >> 1));
        for (let k = 0; k < n; k++) {
          foe(k + 1, arcX(k, n, s), arcZ(k, n, s), (k + s) % 3,
            {skill: skill(s >= 3 ? 1 : 0)});
        }
        for (let at = 0; at < 6 && at < s; at++)
          if (B.barriers[at].present) B.barriers[at].active = false;
        sceneryChanged(); icePatches();
        if (s) say(TOAST.waves[s], true);
        break;
      }
      case "4-3": {
        foe(1, -2, -5.2, M.c.gc, {team: 0, role: "CAPTURE", skill: 1,
          gadget: B.GADGETS[M.c.gg], tint: ghostTint(), name: "TUESDAY"});
        for (let k = 0; k < 3; k++) {
          foe(k + 2, arcX(k, 3), arcZ(k, 3), k % 3,
            {role: k === 0 ? "CAPTURE" : k === 1 ? "FLANK" : "GUARD"});
        }
        break;
      }
      case "4-4": {
        clearFoes();
        player.team = 1; player.gadget = "MINES";
        freeSpot(spot, 1.6, 6, .62);
        player.x = spot.x; player.z = spot.z; player.yaw = player.turret = Math.PI;
        B.updateTankYawCache(player); B.updateTankTurretCache(player);
        player.surfaceY = B.surfaceHeightAt(player.x, player.z);
        const roles = ["GUARD", "FLANK", "HUNTER"];
        for (let k = 0; k < count(2); k++)
          foe(k + 1, -1.8 + (k - 1) * 1.6, -3.6 + (k & 1) * .8, k % 3,
            {team: 0, role: roles[k % 3], skill: skill(0)});
        B.convoy.health = cadet ? 100 : ace ? 130 : 120;
        M.convoyHp = B.convoy.health;
        break;
      }
      case "4-5": {
        vesper(1, 1, {gadget: "SMOKE", hp: duelArmor(220)});
        M.withdraw = 1; M.withdrawAt = .3;
        break;
      }
      case "5-1": {
        for (let k = 0; k < count(2); k++) {
          foe(k + 1, arcX(k, count(2)), arcZ(k, count(2)), k % 3,
            {tint: DRONE, flash: RED_FLASH});
        }
        break;
      }
      case "5-2": {
        for (let k = 0; k < count(3); k++)
          foe(k + 1, (k - 1) * 3, 5.4, k % 3, {tint: DRONE, flash: RED_FLASH});
        const cards = [[-5.5, -5], [-5, 5.5], [5.5, 5.5], [5.6, -4.5], [.6, -2.4]];
        M.salvageSlots = cards.map(([x, z]) => placeCard(x, z));
        break;
      }
      case "5-3": {
        vesper(1, 0, {x: 1.6, z: -5.2, yaw: 0, skill: 2, gadget: "REPAIR DRONE"});
        const n = Math.min(4, 1 + s - (cadet && s ? 1 : 0));
        for (let k = 0; k < n; k++) {
          foe(k + 2, arcX(k, n, s), arcZ(k, n, s), k % 3,
            {tint: DRONE, flash: RED_FLASH});
        }
        if (s) say(s === 1 ? TOAST.more : TOAST.last, false);
        break;
      }
      case "5-4": {
        const boss = foe(5, 0, 5, 2, {role: "GUARD", gadget: "SHIELD", skill: skill(1),
          name: "SUNSET"});
        B.makeBoss(boss, M.c.d);
        freeSpot(spot, 0, 5, .76, boss);
        boss.x = spot.x; boss.z = spot.z;
        boss.surfaceY = B.surfaceHeightAt(boss.x, boss.z);
        foe(1, 5, 3, 0, {tint: DRONE, flash: RED_FLASH, skill: skill(0)});
        break;
      }
      case "5-5": {
        vesper(1, 1, {skill: 0, aggr: .55, range: 4.2, gadget: "SMOKE"});
        M.immortal = 3;
        break;
      }
    }
    if (s === 0 && !m.quiet) {
      const corner = CORNERS[M.idx % CORNERS.length];
      M.card = placeCard(corner[0], corner[1]);
      if (M.card >= 0) {
        M.cardX = B.pickups[M.card].x; M.cardZ = B.pickups[M.card].z;
      }
    }
    unstick(player);
    if ((M.f & WOUNDS) && s === 0)
      player.health = Math.ceil(player.maxHealth / 2);
    if (M.f & MIRROR) mirror();
    if (M.f & BATTERY) sceneryTint(light, 1 - Math.min(.55, M.t / 120 * .55));
    if (M.f & ECHO)
      for (let id = 1; id < 6; id++) {
        const t = tanks[id];
        if (t.active && t.team !== player.team && !(M.dummy & 1 << id) && player.gadget)
          t.gadget = player.gadget;
      }
    M.lastHp = player.health; M.lastGc = player.gadgetCooldown;
    M.lastSc = player.secondaryCooldown; M.px = player.x; M.pz = player.z;
    M.crates = activeCrates();
  }
  // Generated Reprise ground can put scenery on the authored spawn point.
  function unstick(tank) {
    if (!B.circleHitsObstacle(tank.x, tank.z, tank.collisionRadius)) return;
    freeSpot(spot, tank.x, tank.z, tank.collisionRadius, tank);
    tank.x = spot.x; tank.z = spot.z;
    tank.surfaceY = B.surfaceHeightAt(tank.x, tank.z);
  }
  function ghostTint() {
    const paint = B.PAINT_COLORS[B.preferences.paint] || B.PAINT_COLORS[0];
    return [paint[0] * .55 + .2, paint[1] * .55 + .2, paint[2] * .55 + .2];
  }
  function icePatches() {
    B.spawnHazard(0, -1.6, -2.6, 1.15, -1);
    B.spawnHazard(0, 2.4, 2.8, 1.15, -1);
    B.spawnHazard(0, -3.2, 4.6, 1, -1);
  }
  function mirror() {
    for (let id = 0; id < 6; id++) {
      const t = B.tanks[id];
      if (!t.active) continue;
      t.x = -t.x; t.z = -t.z; t.yaw = t.turret = t.yaw + Math.PI;
      B.updateTankYawCache(t); B.updateTankTurretCache(t);
      t.command.aimX = t.yawSine; t.command.aimZ = t.yawCosine;
      if (B.circleHitsObstacle(t.x, t.z, t.collisionRadius)) {
        freeSpot(spot, t.x, t.z, t.collisionRadius, t);
        t.x = spot.x; t.z = spot.z;
      }
      // The convoy is solid: never mirror a hull into the crawler.
      if (B.convoy.active && B.convoyOverlap(t.x, t.z, t.collisionRadius) > 0
          && B.findClearSpot(t, t.x, t.z, t.collisionRadius + .04)) {
        t.x = B.clearSpot.x; t.z = B.clearSpot.z;
      }
      t.surfaceY = B.surfaceHeightAt(t.x, t.z);
      M.yaw[id] = t.yaw;
    }
  }
  function activeCrates() {
    let n = 0;
    for (let at = 0; at < B.crates.length; at++) if (B.crates[at].active) n++;
    return n;
  }

  /* Radio barks use the existing 20-glyph HUD toast. Story lines respect
     the Radio setting; prompts that carry an objective always show. */
  function say(text, prompt = false) {
    if (!text || M.barkCount >= 4) return;
    if (!prompt && (M.radio !== 0 || M.replay)) return;
    const at = (M.barkHead + M.barkCount) & 3;
    M.barkText[at] = text; M.barkCount++;
  }
  function win() { if (B.state.mode === "playing") B.setMode("victory"); }
  function fail() { if (B.state.mode === "playing") B.setMode("game-over"); }

  /* --- hook: damage(...) before armor is removed. Returns the damage. --- */
  function damage(tank, amount, attacker, bounces) {
    if (P && P.on) return P.damage(tank, amount, attacker, bounces);
    if (!M.on) return amount;
    const tanks = B.tanks, player = tanks[0], id = tank.id;
    const playerShot = attacker === 0 && tank.team !== player.team;
    if ((M.f & GLASS) && !tank.boss) amount = Math.max(amount, tank.health);
    if ((M.f & RICO) && playerShot && !bounces) amount = 0;
    if (playerShot && !bounces && (M.bankOnly & 1 << id)) {
      amount = 0;
      if (M.t >= M.warnAt) { M.warnAt = M.t + 3; say(TOAST.bankIt, true); }
    }
    if (M.immortal & 1 << id) amount = Math.min(amount, tank.health - 1);
    if (id === M.withdraw) amount = Math.min(amount, tank.health - 1);
    if (id === 0 && amount >= tank.health && (M.f & SECOND) && !M.rewound) {
      M.rewound = M.rewindPending = true; amount = 0;
    }
    if (playerShot && amount > 0) {
      M.shellHits++;
      if (bounces) M.bankHits++;
      if (bounces >= 2) M.doubleHits++;
    }
    if (id === 1 && M.m.id === "5-3" && amount > 0) M.watchHurt = true;
    if (M.m.id === "4-4" && playerShot && amount >= tank.health)
      B.state.score += 300;
    if (amount >= tank.health && !tank.boss && tank.active) {
      M.killsBy[attacker]++;
      if (tank.team !== player.team) {
        M.kills++;
        if (tank.fireWindup > 0 || tank.recoil > 0 || M.revealAt[id] > M.t - 1.5)
          M.revealed++;
        if (M.names[id] && M.names[id] !== "VESPER")
          say(TOAST.off[M.names[id]]);
      } else if (id) M.allyLost++;
    }
    return amount;
  }

  /* --- hook: tick(dt) once per simulation step, before input. --- */
  let lastMode = "";
  function tick(dt) {
    if (!B) return;
    const mode = B.state.mode;
    if (P && P.on && mode === "playing") P.tick(dt);
    if (M.on) {
      if (mode === "playing") step(dt);
      else if (mode === "arena-clear") clearStage();
    }
    if (mode !== lastMode) { lastMode = mode; menu.onMode(mode); }
    if (mode !== "playing") menu.poll(dt);
  }
  function clearStage() {
    if (M.stage + 1 < M.stages) {
      M.stage++; M.pendingStage = true;
      B.beginArena(B.state.arena);
      B.setMode("playing");
    } else B.setMode("victory");
  }
  function step(dt) {
    const st = B.state, tanks = B.tanks, player = tanks[0], m = M.m;
    M.t += dt;
    if (st.pendingClear && M.noClear) { st.pendingClear = false; st.killBeat = 0; }
    // Generic run statistics: bounded scalar comparisons only.
    if (player.blockedTime > 0 && M.lastBlocked === 0) M.bumps++;
    M.lastBlocked = player.blockedTime;
    if (player.gadgetCooldown > M.lastGc + .5) {
      M.gUses++;
      const g = B.GADGETS.indexOf(player.gadget);
      if (g >= 0) M.gMask |= 1 << g;
      if (player.gadget === "MINES" && (M.c.sg & 1))
        player.gadgetCooldown = player.gadgetCooldownMax = 3.5;
      if (m.id === "2-2")
        for (let id = 1; id < 6; id++) tanks[id].gadget = player.gadget;
    }
    M.lastGc = player.gadgetCooldown;
    if (player.secondaryCooldown > M.lastSc + .5) M.sUses++;
    M.lastSc = player.secondaryCooldown;
    const command = player.command;
    if (command.reverse && (command.left || command.right)) M.reversed = true;
    const dx = player.x - M.px, dz = player.z - M.pz;
    if (dx * dx + dz * dz < 1e-6) {
      M.still += dt; if (M.still > M.maxStill) M.maxStill = M.still;
    } else M.still = 0;
    M.px = player.x; M.pz = player.z;
    if (st.lives < M.lastLives) {
      M.deaths++; redeployed(player);
    } else if (player.health > M.lastHp + 25 && player.repair <= 0) M.armorPickups++;
    M.lastLives = st.lives; M.lastHp = player.health;
    if ((M.c.sg & 2) && st.commandMeter > M.lastCommand)
      st.commandMeter = Math.min(100, st.commandMeter + (st.commandMeter - M.lastCommand) * .25);
    M.lastCommand = st.commandMeter;
    // Faults.
    if (M.f & IRON)
      for (let at = 0; at < B.pickups.length; at++)
        if (B.pickups[at].active && B.pickups[at].type === "ARMOR") B.pickups[at].active = false;
    if (M.f & FOG) st.hitConfirm = st.damageIndicator = 0;
    if (M.f & BATTERY) {
      const level = Math.min(7, (M.t / 15) | 0);
      if (level !== M.dim) { M.dim = level; sceneryTint(M.light, 1 - level * .07); }
      if (M.t >= 120) { fail(); return; }
    }
    if (M.f & SECOND) {
      if (M.t >= M.rewindAt) {
        const at = ((M.rewindAt | 0) % 6) * 4;
        M.rewind[at] = player.x; M.rewind[at + 1] = player.z;
        M.rewind[at + 2] = player.yaw; M.rewind[at + 3] = player.health;
        M.rewindAt += 1;
      }
      if (M.rewindPending) {
        M.rewindPending = false;
        // The oldest of six one-second samples is five to six seconds old.
        const at = ((M.rewindAt | 0) % 6) * 4;
        if (M.rewind[at + 3] > 0) {
          player.x = M.rewind[at]; player.z = M.rewind[at + 1];
          player.yaw = M.rewind[at + 2]; player.health = M.rewind[at + 3];
          // A tank or a zone wall may stand there by now.
          B.findClearSpot(player, player.x, player.z, player.collisionRadius);
          player.x = B.clearSpot.x; player.z = B.clearSpot.z;
          B.updateTankYawCache(player);
          player.surfaceY = B.surfaceHeightAt(player.x, player.z);
        }
        player.slideX = player.slideZ = 0; player.spawnGrace = 1.5;
        M.lastHp = player.health; M.px = player.x; M.pz = player.z;
        say(TOAST.rewind, true);
      }
    }
    // Range targets hold their facing; turrets may still track you.
    for (let mask = M.dummy, id = 0; mask; mask >>= 1, id++) {
      if (!(mask & 1)) continue;
      const t = tanks[id];
      if (t.active && t.yaw !== M.yaw[id]) { t.yaw = M.yaw[id]; B.updateTankYawCache(t); }
    }
    for (let id = 1; id < 6; id++) {
      const t = tanks[id];
      if (t.active && (t.fireWindup > 0 || t.recoil > 0)) M.revealAt[id] = M.t;
      if (!t.active && M.respawnAt[id] > 0 && M.t >= M.respawnAt[id]) {
        M.respawnAt[id] = 0;
        const o = M.cfg[id];
        if (o) { foe(id, o.x, o.z, o.cls, o); B.validateSpawn(tanks[id]); tanks[id].spawnGrace = 1.5; }
      }
    }
    if (M.card >= 0 && !M.cardFound) {
      const pickup = B.pickups[M.card];
      if (!pickup.active || pickup.x !== M.cardX || pickup.z !== M.cardZ) {
        M.cardFound = true;
        say((save.k >> M.idx & 1) ? TOAST.cardLogged : TOAST.cardFound, true);
      }
    }
    const crates = activeCrates();
    if (crates < M.crates && st.kills > M.lastKills) M.crateKills += st.kills - M.lastKills;
    M.crates = crates; M.lastKills = st.kills;
    rules(dt, st, tanks, player, m);
    if (M.barkCount && M.t >= M.barkReady) {
      // The Command meter and label take most of the HUD mesh's spare room,
      // so radio stays quiet while Command is on; objectives say the rest.
      if (!B.preferences.command) B.showToast(M.barkText[M.barkHead], 2.2);
      M.barkHead = (M.barkHead + 1) & 3; M.barkCount--;
      M.barkReady = M.t + 2.4;
    }
  }
  function redeployed(player) {
    unstick(player);
    if (M.f & WOUNDS) player.health = Math.ceil(player.maxHealth / 2);
    if (M.m.id === "4-4") {
      player.team = 1;
      freeSpot(spot, 1.6, 6, .62, player);
      player.x = spot.x; player.z = spot.z; player.yaw = Math.PI;
      B.updateTankYawCache(player);
    }
    if (M.f & MIRROR) {
      // Mirror deploys on the far side, redeploys included (the normal
      // spawn is on the foes' arc).
      const ambush = M.m.id === "4-4";
      const x = ambush || B.state.gameMode === B.MODE_CONVOY ? 1.6 : 0, z = ambush ? 6 : -5.8;
      B.findClearSpot(player, -x, -z, player.collisionRadius + .04);
      player.x = B.clearSpot.x; player.z = B.clearSpot.z;
      player.yaw = player.turret = ambush ? 0 : Math.PI;
      B.updateTankYawCache(player); B.updateTankTurretCache(player);
      player.command.aimX = player.yawSine; player.command.aimZ = player.yawCosine;
      player.surfaceY = B.surfaceHeightAt(player.x, player.z);
    }
    if (M.m.id === "3-3") M.flag = true;
  }
  function liveFoes(team) {
    let n = 0;
    for (let id = 1; id < 6; id++) {
      const t = B.tanks[id];
      if (t.active && t.team === team) n++;
    }
    return n;
  }

  /* Mission rules. Each case reads and writes existing simulation state. */
  function rules(dt, st, tanks, player, m) {
    switch (m.id) {
      case "1-5": case "2-5": case "4-5": {
        const v = tanks[1];
        if (M.mark === 0 && v.active && v.health <= v.maxHealth * M.withdrawAt) {
          v.active = false; M.mark = 2;
          B.spawnParticles(v.x, .4, v.z, 6, 2);
          if (m.id === "4-5") {
            say(TOAST.why, true);
            foe(2, -3, 2.2, 0, {dummy: true, tint: B.GOLD_TINT, hp: 1, gadget: ""});
            foe(3, 3, 2.2, 0, {dummy: true, tint: B.CYAN_TINT, hp: 1, gadget: ""});
            say(TOAST.pick, true);
          } else {
            say(m.id === "1-5" ? TOAST.tomorrow : TOAST.huh, true);
            M.winAt = M.t + 1.2;
          }
        }
        if (m.id === "4-5" && M.mark === 2) {
          const gold = tanks[2].active, cyan = tanks[3].active;
          if (gold !== cyan) {
            M.answer = gold ? 1 : 0; M.mark = 3;
            tanks[2].active = tanks[3].active = false;
            say(TOAST.meToo, true); M.winAt = M.t + 2.4;
          } else if (!gold && !cyan) { M.answer = 0; M.mark = 3; M.winAt = M.t + 1; }
        }
        if (M.winAt && M.t >= M.winAt) win();
        break;
      }
      case "2-3": {
        let standing = 0;
        for (let at = 0; at < B.barriers.length; at++)
          if (B.barriers[at].present && B.barriers[at].active) standing++;
        respawnFallen(5);
        if (!standing) win();
        else if (M.t >= limit(m)) fail();
        break;
      }
      case "3-1": {
        const warm = Math.min(1, M.t / 40);
        player.driveSpeed = B.CLASSES[player.classId].speed * (.55 + .45 * warm);
        if (warm < 1 && player.cooldown > M.lastCooldown + .1) player.cooldown += .45;
        M.lastCooldown = player.cooldown;
        if (warm < 1 && st.kills > 0) M.flag2 = true;
        if (warm === 1 && !M.flag) { M.flag = true; say(TOAST.warm); }
        break;
      }
      case "3-2": armorConvoy(); break;
      case "4-1": if (M.t < 20 && st.damageTaken > 0) M.flag = true; break;
      case "4-2": {
        B.convoy.progress = .45;
        armorConvoy();
        if (M.t >= limit(m)) win();
        break;
      }
      case "4-4": {
        const convoy = B.convoy;
        if (!convoy.active) break;
        if (convoy.health < M.convoyHp) M.bulletHits = true;
        for (let at = 0; at < B.mines.length; at++) {
          const mine = B.mines[at];
          if (!mine.active || mine.arm > 0 || mine.team !== player.team) continue;
          const mx = mine.x - convoy.x, mz = mine.z - convoy.z;
          if (mx * mx + mz * mz > 1.4) continue;
          mine.active = false; convoy.health -= 55; M.mineHits++;
          B.spawnParticles(convoy.x, .3, convoy.z, 6, 2.6);
          B.sounds.explosion(B.sounds.blast.mine);
        }
        M.convoyHp = convoy.health;
        convoy.progress = Math.min(1, convoy.progress + dt * (M.c.d === 0 ? .016 : M.c.d === 2 ? .025 : .02));
        if (convoy.health <= 0) {
          convoy.active = false; st.score += 2000;
          win();
        } else if (convoy.progress >= .98) fail();
        break;
      }
      case "5-1": {
        if (M.zone < 3 && (M.t >= NOTICE_DUE[M.zone] || !liveFoes(1))) {
          M.zone++; M.zoneAt = M.t;
          zoneOff(M.zone - 1);
          say(TOAST.zone);
          // One reinforcement, in the first free slot.
          for (let id = 1; id < 6; id++)
            if (!tanks[id].active) {
              foe(id, -2, -1, 0, {tint: DRONE, flash: RED_FLASH});
              B.validateSpawn(tanks[id]);
              break;
            }
        }
        if (M.zone >= 3 && !liveFoes(1)) win();
        break;
      }
      case "5-2": {
        if (M.zone < 3 && M.t >= SALVAGE_DUE[M.zone]) {
          M.zone++; zoneOff(M.zone + 2); say(TOAST.zone);
        }
        let left = 0;
        for (let k = 0; k < 5; k++) {
          const at = M.salvageSlots[k];
          if (at < 0 || (M.salvage & 1 << k)) continue;
          const pickup = B.pickups[at];
          if (!pickup.active) M.salvage |= 1 << k;
          else if (outsideZone(pickup.x, pickup.z)) { pickup.active = false; M.salvageSlots[k] = -1; }
          else left++;
        }
        respawnFallen(6);
        if (!left || M.t >= limit(m)) { if (salvaged() >= 3) win(); else fail(); }
        break;
      }
      case "5-4": {
        const boss = tanks[5];
        M.mark |= 1;
        if (boss.turretHealth <= 0 && (boss.leftTreadHealth > 0 || boss.rightTreadHealth > 0))
          M.flag = true;
        if (!boss.active) { say(TOAST.postponed, true); win(); }
        respawnFallen(12);
        break;
      }
      case "5-5": {
        st.score = 0;
        const v = tanks[1];
        if (v.active && v.health <= 1) {
          v.health = v.maxHealth; say(TOAST.again, true);
          B.spawnParticles(v.x, .4, v.z, 4, 1.5);
        }
        if (player.health <= 1) player.health = player.maxHealth;
        break;
      }
    }
  }
  const NOTICE_DUE = [35, 70, 105], SALVAGE_DUE = [30, 60, 90];
  /* The archive and the monument are armored: each step refunds part of
     the damage the convoy took, which stretches its existing health pool. */
  function armorConvoy() {
    const convoy = B.convoy;
    if (convoy.health < M.convoyHp) {
      M.flag = true;
      convoy.health += (M.convoyHp - convoy.health) * [.7, .6, .45][M.c.d];
    }
    M.convoyHp = convoy.health;
  }
  function limit(m) { return m.limit * (M.c.d === 0 ? 1.25 : M.c.d === 2 ? .85 : 1); }
  function salvaged() { let n = 0; for (let k = M.salvage; k; k &= k - 1) n++; return n; }
  function respawnFallen(delay) {
    for (let id = 1; id < 5; id++) {
      const t = B.tanks[id];
      if (!t.active && M.cfg[id] && !M.respawnAt[id]) M.respawnAt[id] = M.t + delay;
    }
  }
  /* Powered-off zones are unbreakable barriers that slide in as walls. */
  const ZONES = [
    [3, 0, 3.4, 15.4, .4], [4, 3.4, -2.1, .4, 11], [5, -3.6, -2.1, .4, 11],
    [3, -2.6, 0, .4, 15.4], [4, 2.5, 2.6, 10.2, .4], [5, 3.2, -2.6, .4, 10.2],
  ];
  function zoneOff(at) {
    const z = ZONES[at];
    setBarrier(z[0], z[1], z[2], z[3], z[4], 1e9);
    // A crate the wall slides over is crushed, quietly.
    const wall = B.barriers[z[0]];
    for (const crate of B.crates)
      if (crate.active && crate.x + B.CRATE_HALF > wall.left
          && crate.x - B.CRATE_HALF < wall.right && crate.z + B.CRATE_HALF > wall.top
          && crate.z - B.CRATE_HALF < wall.bottom) crate.active = false;
    sceneryChanged();
    // Anything caught on the dark side or under the new wall is moved into
    // the live zone.
    for (let id = 0; id < 6; id++) {
      const t = B.tanks[id];
      if (!t.active || !(outsideZone(t.x, t.z)
          || B.circleHitsObstacle(t.x, t.z, t.collisionRadius, true))) continue;
      freeSpot(spot, t.x * .3, t.z * .3 - 1, t.collisionRadius, t);
      t.x = spot.x; t.z = spot.z; t.slideX = t.slideZ = 0;
      t.surfaceY = B.surfaceHeightAt(t.x, t.z);
    }
    // Pickups behind the wall (the hidden card, dropped armor, salvage)
    // can no longer be reached; they go dark with the zone, uncollected.
    for (let at = 0; at < B.pickups.length; at++) {
      const pickup = B.pickups[at];
      if (!pickup.active || !outsideZone(pickup.x, pickup.z)) continue;
      pickup.active = false;
      if (at === M.card) M.card = -1;
      const slot = M.salvageSlots ? M.salvageSlots.indexOf(at) : -1;
      if (slot >= 0) M.salvageSlots[slot] = -1;
    }
  }
  function outsideZone(x, z) {
    const id = M.m && M.m.id;
    if (id === "5-1") return (M.zone >= 1 && z > 3.2) || (M.zone >= 2 && x > 3.2)
      || (M.zone >= 3 && x < -3.4);
    if (id === "5-2") return (M.zone >= 1 && x < -2.4) || (M.zone >= 2 && z > 2.4 && x < 7.7)
      || (M.zone >= 3 && x > 3.0);
    return false;
  }

  // ------------------------------------------------------------ results --
  const MEDAL_TESTS = {
    "1-1": () => [M.bumps === 0, M.t < 60],
    "1-2": () => [M.doubleHits >= 3, M.shellHits >= B.state.shots - M.shots0],
    "1-3": () => [B.state.damageTaken === 0, M.t < 120],
    "1-4": () => [(M.gMask & 31) === 31, M.armorPickups === 0],
    "1-5": () => [B.state.damageTaken <= B.tanks[0].maxHealth * .25, M.t < 90],
    "2-1": () => [M.crateKills >= 3, !M.flag],
    "2-2": () => [M.gUses <= 2, M.t < 150],
    "2-3": () => [M.sUses === 0, M.t < 60],
    "2-4": () => [M.allyLost === 0, B.state.blueControl - B.state.redControl >= 30],
    "2-5": () => [M.c.vg > 4 || B.tanks[0].gadget !== B.GADGETS[M.c.vg], B.state.damageTaken === 0],
    "3-1": () => [M.flag2, !M.reversed],
    "3-2": () => [!M.flag, M.t < 120],
    "3-3": () => [M.deaths === 0, M.t < 150],
    "3-4": () => [M.kills >= 2 && M.revealed >= M.kills, !(M.gMask & 4)],
    "3-5": () => [M.victory, M.maxStill < 3],
    "4-1": () => [!M.flag, !(M.gMask & 2)],
    "4-2": () => [!M.flag, B.state.bankKills >= 3],
    "4-3": () => [M.killsBy[0] > M.killsBy[1], M.killsBy[1] === 0],
    "4-4": () => [B.convoy.progress < .5, !M.bulletHits && M.mineHits > 0],
    "4-5": () => [B.state.mainShots === 0, B.state.damageTaken === 0],
    "5-1": () => [M.t - (M.zoneAt || 0) <= 20, M.t < 120],
    "5-2": () => [salvaged() === 5, M.armorPickups === 0 && !(M.gMask & 8)],
    "5-3": () => [!M.watchHurt, M.killsBy[0] > M.killsBy[1]],
    "5-4": () => [!M.flag, M.gUses === 0],
  };
  function finish(mode) {
    M.ended = true;
    const m = M.m, st = B.state;
    M.victory = mode === "victory";
    M.won = mode === "victory" || (m.id === "3-5" && M.stage >= 3)
      || (m.id === "4-4" && !B.convoy.active && B.convoy.health <= 0);
    const result = {mission: M.idx, won: M.won, medals: 0, newMedals: 0,
      score: 0, best: false, card: false, quiet: !!m.quiet, aim: 0};
    if (M.won && !m.quiet) {
      result.medals = 1;
      const tests = MEDAL_TESTS[m.id]();
      if (tests[0]) result.medals |= 2;
      if (tests[1]) result.medals |= 4;
      if (M.c.d === 2 || M.c.rep) result.medals |= 8;
      const bonus = (result.medals & 2 ? 1000 : 0) + (result.medals & 4 ? 1000 : 0)
        + Math.max(0, (180 - M.t) * 10 | 0);
      // The aim guide's cost rides the faults' multiplier (game.js keeps
      // the highest level the run used; a replay keeps its recorded one).
      result.aim = B.runAimLevel();
      result.score = Math.round((st.score + 1000 + bonus) * M.mult * B.aimScoreMultiplier());
    }
    M.result = result;
    if (M.replay) return result;
    const before = save.m[M.idx];
    if (M.won) {
      save.m[M.idx] |= result.medals;
      result.newMedals = save.m[M.idx] & ~before & 7;
      if (result.score > save.b[M.idx]) {
        save.b[M.idx] = result.score; result.best = true;
        const code = B.exportReplay();
        if (code && code.length < 24000) save.best = {i: M.idx, c: code};
      }
      if (M.cardFound && !(save.k >> M.idx & 1)) { save.k |= 1 << M.idx; result.card = true; }
      if (m.id === "1-3")
        save.gh = [B.tanks[0].classId, Math.max(0, B.GADGETS.indexOf(B.tanks[0].gadget))];
      if (m.id === "4-5") save.ans = M.answer;
      if (m.quiet) save.e = 1;
      awardPaints();
    }
    for (let g = 0; g < 5; g++) if (M.gMask & 1 << g) save.g[g]++;
    save.sh += st.shots - M.shots0; save.ri += M.bankHits;
    save.last = Date.now();
    writeSave();
    return result;
  }
  /* Theater ribbons unlock the existing Quick Match paints. */
  function awardPaints() {
    for (let theater = 0; theater < 5; theater++) {
      let all = true;
      for (let k = 0; k < 5; k++) {
        const at = theater * 5 + k;
        if (!MISSIONS[at].quiet && (save.m[at] & 7) !== 7) all = false;
      }
      if (all && !(B.state.medals & 1 << theater + 1)) {
        B.state.medals |= 1 << theater + 1; B.savePreferences();
      }
    }
  }
  function ribbons(theater) {
    let all = true, aceAll = true;
    for (let k = 0; k < 5; k++) {
      const at = theater * 5 + k;
      if (MISSIONS[at].quiet) continue;
      if ((save.m[at] & 7) !== 7) all = false;
      if (!(save.m[at] & 8)) aceAll = false;
    }
    return (all ? "[ALL MEDALS] " : "") + (aceAll ? "[ACE] " : "");
  }

  // --------------------------------------------------------------- start --
  function startMission(at, deploy = true) {
    if (!B) { queued = at; return false; }
    const m = MISSIONS[at], st = B.state;
    if (st.mode !== "title" && st.mode !== "victory" && st.mode !== "game-over"
        && st.mode !== "replay-done") B.setMode("title");
    if (!M.armed) M.quickMode = st.gameMode;
    M.armed = true;
    M.radio = runRadio;
    st.gameMode = m.mode;
    const code = runCode(at, save.f);
    st.dailyDay = code;
    if (deploy) B.requestPresentation();
    const c = decode(code), arenaIndex = missionArena(m, c);
    if (!B.resetGame()) return false;
    if (arenaIndex === 3) {
      st.arenaSeed = REPRISE_SEEDS[at];
      B.arenaGenerator.materialize(st.arenaSeed);
      B.fillArenaSpatialData(3); B.dirty(true);
    }
    if (st.arena !== arenaIndex || arenaIndex === 3) B.beginArena(arenaIndex);
    save.x |= 1 << at;
    menu.show("game", false);
    B.setMode("playing");
    return true;
  }
  let queued = -1, runRadio = 0;

  // --------------------------------------------------------------- menus --
  /* The page's elements are all authored (or pooled at boot) and never
     replaced, so a lookup by id is answered once. */
  const byId = new Map();
  const $ = (id) => {
    let el = byId.get(id);
    if (!el) { el = document.getElementById(id); if (el) byId.set(id, el); }
    return el;
  };
  const menu = (() => {
    const panel = $("panel"), tabs = $("tabs"), list = $("menu");
    const heading = panel ? panel.querySelector("h1") : null;
    const message = $("message"), radio = $("radio"), medals = $("medals"), legend = $("legend");
    const bindings = $("bindings");
    let screen = "main", tab = 0, stack = [], theater = 0, selected = 0;
    let logPage = 0, radioLines = [], radioShown = 0, radioNext = 0, radioPage = 0;
    /* Menus are sized for 480x272 at ~25% larger text: the briefing radio
       shows two lines per page (L/R pages back), a focused fault explains
       itself in the message line, and the legend names each screen's L/R. */
    const RADIO_PAGE = 2, faultNotes = [];
    const LEGEND = "X Select | O Back | L/R Tabs | START Deploy";
    let pad = 0, startHeld = 0, pending = null;
    /* A focused fault or drill explains itself in the message line once
       focus has rested NOTE_REST seconds, so a held D-pad does not rewrite
       the line on every step. Rest counts wall time as well as simulation
       time: Tilefinch advances page time by at most 16 ms per frame, and a
       PSP menu frame takes about 50 ms. */
    const NOTE_REST = 0.15;
    let noteFor = null, noteWait = 0, noteSince = 0;
    const PAD_BUTTONS = [12, 13, 14, 15, 0, 1, 4, 5, 9];
    // An unchanged text write still costs the browser a relayout.
    const set = (el, text) => { if (el && el.textContent !== text) el.textContent = text; };
    /* Fixed node pools: every screen reuses these buttons and text rows,
       so menu navigation creates no DOM nodes or listeners after boot. */
    const buttons = [], actions = [], tabPool = [], radioRows = [], medalRows3 = [];
    const bindingRows = [];
    let used = 0, tabPick = null, tabCount = 0;
    /* What the pools last wrote, so a screen change writes only what it
       changes: every DOM write is a mutation the browser relays out, and
       reading back hidden/disabled costs a native call per button. */
    const poolDisabled = [], shownState = new Map();
    function hide(el, on) {
      if (el && shownState.get(el) !== on) { el.hidden = on; shownState.set(el, on); }
    }
    function grow(parent, store, count, tag) {
      if (!parent) return;
      for (let at = 0; at < parent.children.length && store.length < count; at++)
        store.push(parent.children[at]);
      while (store.length < count) {
        const el = document.createElement(tag);
        if (tag === "button") el.type = "button";
        else { el.appendChild(document.createElement("b")); el.appendChild(document.createElement("span")); }
        el.hidden = true; parent.appendChild(el); store.push(el);
      }
    }
    grow(list, buttons, 14, "button"); grow(tabs, tabPool, 8, "button");
    grow(radio, radioRows, 6, "span"); grow(medals, medalRows3, 3, "span");
    grow(bindings, bindingRows, 6, "span");
    function button(label, action, cls = "", disabled = false) {
      const b = buttons[used];
      if (!b) return null;
      if (b.textContent !== label) b.textContent = label;
      if (b.className !== cls) b.className = cls;
      if (poolDisabled[used] !== disabled) b.disabled = poolDisabled[used] = disabled;
      hide(b, false);
      actions[used++] = action;
      return b;
    }
    function tabButtons(names, active, pick) {
      tabPick = pick; tabCount = Math.min(names.length, tabPool.length);
      for (let at = 0; at < tabCount; at++) {
        const b = tabPool[at], cls = at === active ? "on" : "";
        if (b.textContent !== names[at]) b.textContent = names[at];
        if (b.className !== cls) b.className = cls;
      }
    }
    function row(el, head, text, off) {
      hide(el, false);
      const b = el.firstChild, cls = off ? "off" : "";
      set(b, head); if (b.className !== cls) b.className = cls;
      set(el.lastChild, text);
    }
    function line(text) {
      const bar = text.indexOf("|");
      const who = WHO[text.slice(0, bar)] || "";
      return who + ": " + text.slice(bar + 1).replace("(answer)",
        save.ans === 1 ? "I wanted to see." : "I missed it.");
    }
    function showRadio(lines, all) {
      radioLines = lines; radioShown = all ? lines.length : Math.min(1, lines.length);
      radioPage = 0; radioNext = performance.now() + 2600; drawRadio();
    }
    function drawRadio() {
      if (!radio) return;
      hide(radio, radioShown === 0);
      const first = screen === "briefing" ? radioPage * RADIO_PAGE : 0;
      const end = screen === "briefing" ? Math.min(radioShown, first + RADIO_PAGE) : radioShown;
      for (let at = 0; at < radioRows.length; at++) {
        if (first + at >= end) { hide(radioRows[at], true); continue; }
        const text = line(radioLines[first + at]), colon = text.indexOf(":");
        row(radioRows[at], text.slice(0, colon + 1), text.slice(colon + 1), false);
      }
    }
    function remark() {
      if (!save.rr || save.r === 2) return "";
      const now = new Date(), hour = now.getHours();
      if (hour < 5) return `M|It's ${hour}:${String(now.getMinutes()).padStart(2, "0")}. The war will still be fictional in the morning.`;
      const days = save.last ? Math.floor((Date.now() - save.last) / 864e5) : 0;
      if (days >= 2) return `M|It's been ${days} days. That's all right. I kept your seat warm. Metaphorically.`;
      return "";
    }
    let remarked = false;
    function medalText(at) {
      const bits = save.m[at];
      return MISSIONS[at].quiet ? (bits & 1 ? "done" : "")
        : (bits & 1 ? "*" : ".") + (bits & 2 ? "*" : ".") + (bits & 4 ? "*" : ".");
    }
    function multiplier() {
      let mult = 1;
      for (let at = 0; at < FAULTS.length; at++)
        if (save.f & 1 << at && FAULTS[at]) mult *= FAULTS[at][1];
      return mult;
    }
    const MEDAL_NAMES = ["Clear", "Challenge", "Mastery"];
    const IN_RUN = ["game", "pause", "results", "replaydone", "duel", "range", "drilldone"];
    let ranOnce = false;
    function medalRows(at, earned) {
      const m = MISSIONS[at];
      hide(medals, !m.med);
      if (!m.med) return;
      const bits = earned === undefined ? save.m[at] : earned;
      for (let k = 0; k < 3; k++) {
        const lit = (bits & 1 << k) !== 0;
        row(medalRows3[k], (lit ? "[*] " : "[ ] ") + MEDAL_NAMES[k] + ": ",
          k ? m.med[k - 1] : m.obj, !lit);
      }
    }
    const screens = {
      main() {
        set(heading, "TREADLINE ARENA");
        list.className = "";
        const next = nextMission(), fresh = save.d < 0;
        button(fresh ? "Start campaign" : `Continue ${MISSIONS[next].id}`, () => {
          if (fresh) return show("difficulty");
          if (!(save.x >> next & 1)) { selected = next; return show("briefing"); }
          runRadio = cleared(next) ? 2 : save.r; startMission(next);
        }, "primary");
        button("Campaign", () => show(fresh ? "difficulty" : "campaign"));
        button("Quick Match", () => show("quick"));
        // Daily stays one MODE step away on Quick Match.
        if (P) button("Practice Range", () => show("practice"));
        else button("Daily", () => { B.state.gameMode = B.MODE_DAILY; B.refreshSetupLabels(); show("quick"); });
        button("Multiplayer", () => show("multiplayer"));
        button("Garage", () => show("garage"));
        button("Replays", () => show("replays"));
        button("Settings", () => show("settings"));
      },
      difficulty() {
        set(heading, "THE LINE");
        set(message, "Choose once for the campaign. Bots, lives and limits follow it.");
        list.className = "list";
        ["Cadet: gentle drills", "Veteran: the intended range", "Ace: hard but fair"].forEach((label, d) =>
          button(label, () => { save.d = d; writeSave(); stack.pop(); show("campaign"); }));
        button("Back", back);
      },
      campaign() {
        tabButtons(THEATERS.map((t, at) => `${at + 1} ${t.n.split(" ").pop()}`), theater,
          (at) => { theater = at; render(); });
        set(heading, THEATERS[theater].n);
        const said = remarked ? "" : remark();
        remarked = true;
        set(message, said ? line(said) : save.rep && save.e ? line(REPRISE_LINE)
          : `${THEATERS[theater].sub} ${ribbons(theater)}Medals ${medalCount()}/72 | Cards ${cardCount()}/24`);
        list.className = "mixed";
        for (let k = 0; k < 5; k++) {
          const at = theater * 5 + k, m = MISSIONS[at];
          button(`${m.id} ${m.n}   ${medalText(at)}${save.b[at] ? "  " + save.b[at] : ""}`,
            () => { selected = at; show("briefing"); }, "wide", !unlocked(at));
        }
        button(`Faults x${multiplier().toFixed(2)}`, () => show("faults"));
        button(`Radio: ${["Full", "Brief", "Off"][save.r]}`, () => {
          save.r = (save.r + 1) % 3; writeSave(); refreshLabels(); render();
        });
        button("Range Logs", () => show("logs"));
        if (save.e) button(`Reprise: ${save.rep ? "On" : "Off"}`, () => {
          save.rep ^= 1; writeSave(); render();
        });
        button("Back", back);
      },
      briefing() {
        const m = MISSIONS[selected];
        set(heading, `${m.id} ${m.n.toUpperCase()}`);
        const guide = B ? B.aimGuideLevel() : 0;
        set(message, m.obj + (save.f && !m.quiet ? `  Faults x${multiplier().toFixed(2)}` : "")
          + (guide && !m.quiet ? `  Aim guide x${B.AIM_SCORE_MULTIPLIERS[guide]}` : ""));
        if (!pending || pending.at !== selected) {
          pending = {at: selected, radio: cleared(selected) ? 2 : save.r};
        }
        const lines = pending.radio === 2 ? [] : pending.radio === 1 ? m.brief.slice(0, 1)
          : (save.rep && save.e ? [REPRISE_LINE] : []).concat(m.brief);
        showRadio(lines, false);
        medalRows(selected);
        list.className = "row";
        button("Deploy", deploy, "primary");
        button(`Radio: ${["Full", "Brief", "Off"][pending.radio]}`, () => {
          pending.radio = (pending.radio + 1) % 3; render();
        });
        if (save.best && save.best.i === selected) button("Watch best", watchBest);
        button("Back", back);
      },
      faults() {
        tabButtons(["Faults 1-5", "Faults 6-10"], tab, (at) => { tab = at; render(); });
        set(heading, "RANGE FAULTS");
        set(message, `Score x${multiplier().toFixed(2)} | Memory cards ${cardCount()}/24 unlock faults.`);
        list.className = "list";
        for (let at = tab * 5; at < tab * 5 + 5; at++) {
          if (!FAULTS[at]) continue;
          const [name, mult, cards, note] = FAULTS[at], open = faultUnlocked(at);
          faultNotes[used] = note;
          button(open ? `${name} x${mult}: ${save.f & 1 << at ? "ON" : "off"}`
            : `Locked: ${cards} memory cards`, () => {
            save.f ^= 1 << at; writeSave(); render();
          }, save.f & 1 << at ? "on" : "", !open);
        }
        button("Back", back);
      },
      logs() {
        const entries = logEntries(), pages = Math.max(1, Math.ceil(entries.length / 4));
        logPage = Math.min(logPage, pages - 1);
        tabButtons(Array.from({length: Math.min(pages, 8)}, (_, at) => `${at + 1}`), logPage,
          (at) => { logPage = at; render(); });
        set(heading, "RANGE LOGS");
        set(message, `${entries.length} entries unlocked.`);
        radioLines = entries.slice(logPage * 4, logPage * 4 + 4);
        radioShown = radioLines.length; drawRadio();
        list.className = "row";
        button("Prev", () => { logPage = Math.max(0, logPage - 1); render(); });
        button("Next", () => { logPage = Math.min(pages - 1, logPage + 1); render(); });
        button("Back", back);
      },
      quick() {
        set(heading, "QUICK MATCH");
        if (B) { B.refreshSetupLabels(); set(message, modeLine()); }
        list.className = "row";
        if (P) button("Practice Range", () => show("practice"));
        button("Back", back);
      },
      multiplayer() {
        // game.js's online flow picks the live group (B.onlineView).
        const view = B ? B.onlineView() : "actions";
        set(heading, view === "web" ? "WEB TEAM CONTROL"
          : view === "status" || view === "response" ? "ONLINE TEAM CONTROL" : "MULTIPLAYER");
        list.className = "row"; button("Back", back);
      },
      replays() {
        set(heading, "REPLAYS");
        set(message, save.best ? `Best run on file: ${MISSIONS[save.best.i].id} ${MISSIONS[save.best.i].n}.`
          : "Replays stay in RAM unless you copy a code.");
        list.className = "row";
        if (save.best) button("Watch best run", watchBest);
        button("Back", back);
      },
      garage() {
        set(heading, "GARAGE");
        const st = B && B.state;
        set(message, st ? `${B.CLASSES[st.classChoice].name} | ${B.GADGETS[st.gadgetChoice]} | `
          + `${B.PAINT_NAMES[B.preferences.paint]} | Medals ${medalCount()}/72`
          + (sidegrades() & 1 ? " | Quick Mines" : "") + (sidegrades() & 2 ? " | Fast Command" : "") : "");
        list.className = "row";
        for (let at = 0; at < 3; at++) button(`Callsign ${save.cs[at]}`, () => {
          const code = save.cs.charCodeAt(at) - 65;
          save.cs = save.cs.slice(0, at) + String.fromCharCode(65 + (code + 1) % 26) + save.cs.slice(at + 1);
          writeSave(); render();
        });
        button("Back", back);
      },
      settings() {
        tabButtons(["Controls", "Camera & Display", "Audio"], tab, (at) => { tab = at; render(); });
        if (panel.getAttribute("data-tab") !== String(tab)) panel.setAttribute("data-tab", String(tab));
        refreshLabels();
        set(heading, "SETTINGS");
        list.className = "row";
        if (tab === 0) button("Controls", () => show("controls"), "primary");
        button("Back", back);
      },
      /* Scheme, assist and reverse, then the scheme as a readable list:
         tab 0 driving, tab 1 shooting (L/R), one action per row. Tab 2 is
         the aim guide: its level choice, then one row per level. */
      controls() {
        tabButtons(["Driving", "Shooting", "Aim guide"], tab, (at) => { tab = at; render(); });
        if (panel.getAttribute("data-tab") !== String(tab)) panel.setAttribute("data-tab", String(tab));
        set(heading, "CONTROLS");
        if (tab === 2) {
          aimRows();
          list.className = "row";
          button("Back", back);
          return;
        }
        const K = globalThis.__treadlineControls, rows = K ? K.bindings(tab) : [];
        for (let at = 0; at < bindingRows.length; at++) {
          const text = rows[at];
          if (!text) { hide(bindingRows[at], true); continue; }
          const bar = text.indexOf("|");
          row(bindingRows[at], text.slice(0, bar), text.slice(bar + 1), false);
        }
        list.className = "row";
        button("Back", back);
      },
      game() { list.className = ""; },
      pause() {
        set(heading, "PAUSED");
        const m = M.on && M.m;
        set(message, m ? `${m.id} ${m.n}: ${m.obj}` : B ? `${B.GAME_MODES[B.state.gameMode]}: simulation frozen.` : "");
        list.className = "row";
        if (m && m.quiet) button("Leave the range", () => B.setMode("victory"), "primary");
        button("Restart", () => {
          if (M.on && M.m && !M.replay) { runRadio = M.radio; startMission(M.idx); }
          else { B.setMode("title"); B.startOrResume(); }
        });
        button("Controls", () => show("controls"));
        button("Quit", toMain);
      },
      results() {
        const r = M.on && M.ended ? M.result : null;
        list.className = "row";
        if (r) {
          const m = MISSIONS[r.mission];
          set(heading, r.quiet ? "THE LINE" : r.won ? "MISSION CLEAR" : "MISSION FAILED");
          set(message, r.quiet ? `${save.cs}, under KAZ_99 on the scoreboard. It's yours.`
            : `${m.id} ${m.n}${r.won ? ` | Score ${r.score}${r.best ? " (best)" : ""}` : ""}`
              + (r.won && r.aim ? ` | Guide x${B.AIM_SCORE_MULTIPLIERS[r.aim]}` : "")
              + (r.card ? " | Memory card logged" : ""));
          medalRows(r.mission, r.medals);
          showRadio(M.radio === 0 && r.won ? m.win : [], true);
          if (r.quiet) {
            radioLines = ["Y|"].concat(m.win); radioShown = radioLines.length; drawRadio();
            row(radioRows[0], "SCOREBOARD:", ` KAZ_99 999,990 | ${save.cs} | PSPGOD 871,200 | xXshellXx 640,075`, false);
            button("Daily run", () => { B.state.gameMode = B.MODE_DAILY; B.refreshSetupLabels(); stack = ["main"]; show("quick", false); }, "primary");
            button("Menu", toMain);
            return;
          }
          button("Retry", () => { runRadio = 2; startMission(r.mission); }, "primary");
          if (r.won && r.mission + 1 < MISSIONS.length) button("Next", () => {
            selected = r.mission + 1; stack = ["main", "campaign"]; show("briefing");
          });
        } else {
          const st = B.state, won = st.mode === "victory";
          const onslaught = st.gameMode === B.MODE_ONSLAUGHT || st.gameMode === B.MODE_DAILY;
          set(heading, won ? "MISSION COMPLETE" : "TANK LOST");
          set(message, won && st.gameMode === B.MODE_DUEL
            ? `PLAYER ${st.duelWinner + 1} WINS. Handheld duel complete.`
            : `Final score ${B.finalScore()}. ${won ? "Objective secured" : "The arena held"}.`
              + B.aimScoreNote() + (onslaught ? ` Seed ${st.arenaSeed}.` : "")
              + (!won && st.gameMode === B.MODE_DAILY ? ` TD1-${st.dailyDay}.` : ""));
          radioShown = 0; drawRadio(); hide(medals, true);
          button("Retry", () => B.startOrResume(), "primary");
        }
        button("Watch replay", () => { if (B.startReplay()) B.requestPresentation(); });
        button("Menu", toMain);
      },
      replaydone() {
        set(heading, "REPLAY");
        set(message, B && B.replayVerified() ? "Replay verified. Same simulation result."
          : "Replay ended with a simulation mismatch.");
        list.className = "row";
        button("Watch again", () => { if (B.startReplay()) B.requestPresentation(); }, "primary");
        button("Menu", toMain);
      },
      duel() {
        set(heading, "PASS THE PSP");
        if (B) set(message, `PLAYER ${B.state.duelTurn + 1}: take the controls, then press Ready.`);
        list.className = "row";
      },
    };
    const AIM_ROWS = [["Off", "Turret only. Full score."],
      ["Sight", "Line to the wall, bounce way · score x"],
      ["Full", "+ bounce leg, red on a foe · score x"],
      ["Tracers", "Your shell's trail. Always on, no cost."]];
    function aimRows() {
      const level = B ? B.aimGuideLevel() : 0;
      // The level in force here (the range keeps its own).
      if (B) set($("aim-value"), B.AIM_LEVEL_NAMES[level]);
      const note = P && P.on ? ["Range", "No score here; its panel toggles it."]
        : ["Cost", "A run scores at the highest level used."];
      for (let at = 0; at < bindingRows.length; at++) {
        if (at < AIM_ROWS.length)
          row(bindingRows[at], AIM_ROWS[at][0], AIM_ROWS[at][1]
            + (at === 1 || at === 2 ? B.AIM_SCORE_MULTIPLIERS[at] : ""), at < 3 && at !== level);
        else if (at === AIM_ROWS.length) row(bindingRows[at], note[0], note[1], false);
        else hide(bindingRows[at], true);
      }
    }
    function logEntries() {
      const out = [];
      for (let at = 0; at < MISSIONS.length; at++) {
        if (!cleared(at)) continue;
        for (const text of MISSIONS[at].brief) out.push(text);
        for (const text of MISSIONS[at].win) out.push(text);
      }
      const lore = Math.min(LORE.length, 1 + (medalCount() / 8 | 0));
      for (let at = 0; at < lore; at++) out.push("M|" + LORE[at]);
      if (save.e) out.push(REPRISE_LINE);
      return out;
    }
    function render() {
      if (!panel || !list) return;
      used = 0; tabPick = null; faultNotes.length = 0;
      set(legend, P && P.legends[screen] ? P.legends[screen]
        : screen === "briefing" ? "X Select | O Back | L/R Radio | START Deploy"
        : screen === "logs" ? "X Select | O Back | L/R Page"
          : screen === "controls" ? (tab === 2 ? "X Select | \u25c4 \u25ba Level | L/R Page | O Back"
            : "X Select | \u25c4 \u25ba Scheme | L/R Page | O Back") : LEGEND);
      tabCount = 0;
      if (radio && screen !== "briefing" && screen !== "results" && screen !== "logs") {
        radioShown = 0; radioLines = []; drawRadio();
      }
      if (medals && screen !== "briefing" && screen !== "results") hide(medals, true);
      if (panel.getAttribute("data-screen") !== screen) panel.setAttribute("data-screen", screen);
      const shell = $("game-shell"), inRun = ranOnce || IN_RUN.includes(screen);
      // After the first run the canvas keeps its retained HUD behind every
      // menu, so the authored copy stays hidden rather than overlapping it.
      if (shell && shell.classList.contains("in-run") !== inRun) shell.classList.toggle("in-run", inRun);
      if (screen !== "settings" && panel.hasAttribute("data-tab")) panel.removeAttribute("data-tab");
      (screens[screen] || (P && P.screens[screen]) || screens.main)();
      for (let at = 0; at < tabPool.length; at++) hide(tabPool[at], at >= tabCount);
      for (let at = used; at < buttons.length; at++) {
        hide(buttons[at], true); actions[at] = null;
      }
      focusFirst();
      noteFor = document.activeElement; noteWait = 0;
      faultNote(noteFor);
    }
    /* Quick Match's message: the chosen mode and its best (a setup
       change on that screen refreshes it). */
    function modeLine() {
      const st = B.state, mode = st.gameMode;
      return B.MODE_DESCRIPTIONS[mode] + (mode === B.MODE_ONSLAUGHT
        ? ` Best W${st.bestWave} · ${st.bestScore}.`
        : mode === B.MODE_DAILY ? ` TD1-${st.dailyDay} · best ${st.dailyBestDay === st.dailyDay ? st.dailyBest : 0}.` : "");
    }
    function refreshMessage() {
      if (screen === "quick" && B) set(message, modeLine());
    }
    let autofocused;
    function focusFirst() {
      const items = focusables();
      const preferred = screen === "quick" || screen === "pause" || screen === "duel"
        ? $("play") : screen === "controls" ? $(tab === 2 ? "aim-choice" : "controls-choice") : null;
      const target = preferred && items.includes(preferred) ? preferred : items[0];
      if (autofocused === undefined) autofocused = panel.querySelector("[autofocus]");
      if (autofocused && autofocused !== target) autofocused.removeAttribute("autofocus");
      if (target) {
        if (autofocused !== target) target.setAttribute("autofocus", "");
        if (document.activeElement !== target) try { target.focus(); } catch (_) {}
      }
      autofocused = target || null;
    }
    /* Focus order mirrors the DOM order of the groups each screen shows.
       Visibility comes from the screen table rather than computed style,
       which would materialise a style object per query. The menu's DOM
       never changes shape after boot, so each group's candidates (and
       their parents) are queried once, and the pools report the state
       they wrote instead of reading it back. */
    const groups = new Map();
    function collect(out, root, selector) {
      if (!root || root.hidden) return;
      const key = selector + "\n" + root.id;
      let group = groups.get(key);
      if (!group) {
        const items = Array.from(root.querySelectorAll(selector));
        group = {items, parents: items.map((el) => el.parentElement)};
        groups.set(key, group);
      }
      for (let at = 0; at < group.items.length; at++) {
        const el = group.items[at], parent = group.parents[at];
        if (!el.disabled && !el.hidden && !(parent && parent !== root && parent.hidden))
          out.push(el);
      }
    }
    function collectPool(out, root, pool, count, disabled) {
      if (!root || root.hidden) return;
      for (let at = 0; at < count; at++) if (!disabled[at]) out.push(pool[at]);
    }
    function focusables() {
      const out = [];
      if (!panel) return out;
      if (screen === "campaign" || screen === "settings" || screen === "faults" || screen === "logs"
          || screen === "controls")
        collectPool(out, tabs, tabPool, tabCount, []);
      collectPool(out, list, buttons, used, poolDisabled);
      if (screen === "quick") collect(out, $("loadout"), "button");
      if (screen === "briefing") { collect(out, $("loadout"), "#class-choice, #gadget-choice"); }
      const prefs = $("preferences");
      if (screen === "settings") collect(out, prefs, `[data-tab="${tab}"]:not([data-ctl])`);
      if (screen === "pause") collect(out, prefs, '[data-tab="0"]:not([data-ctl])');
      if (screen === "controls")
        collect(out, prefs, tab === 2 ? "#aim-choice" : "[data-ctl]:not(#aim-choice)");
      if (screen === "garage") collect(out, prefs, '[data-tab="3"]');
      if (screen === "quick" || screen === "pause" || screen === "duel" || screen === "game") {
        const play = $("play"); if (play && !play.disabled) out.push(play);
      }
      if (screen === "replays") { collect(out, $("replay-actions"), "button"); collect(out, panel, "#replay-code"); }
      if (screen === "multiplayer")
        for (const id of ["online-actions", "online-status", "code-entry", "web-pairing"])
          collect(out, $(id), "button, textarea");
      return out;
    }
    function show(name, push = true) {
      if (push && screen !== name && screen !== "game") {
        stack.push(screen);
        if (stack.length > 12) stack.shift();
      }
      if (name === "quick" || name === "main") disarm();
      if (name !== screen) tab = 0;
      screen = name; render();
    }
    function back() {
      if (screen === "pause" || (P && P.back(screen))) {
        if (screen === "pause") B.startOrResume();
        return;
      }
      if (screen === "results" || screen === "replaydone") { toMain(); return; }
      if (screen === "game" || screen === "duel") return;
      if (screen === "multiplayer") {
        const status = $("online-status"), cancel = $("online-cancel");
        if (status && !status.hidden && cancel) cancel.click();
      }
      const previous = stack.pop();
      if (previous === undefined) { if (screen !== "main") { screen = "main"; render(); } return; }
      screen = previous; tab = 0; render();
    }
    function toMain() {
      if (P && P.on) P.leave();
      disarm();
      if (B && B.state.mode !== "title") B.setMode("title");
      stack = []; screen = "main"; render();
    }
    function deploy() {
      const at = selected, radioChoice = pending && pending.at === at ? pending.radio : save.r;
      runRadio = radioChoice; pending = null;
      startMission(at);
    }
    function watchBest() {
      if (!B || !save.best) return;
      if (B.importReplay(save.best.c) && B.startReplay()) { menu.show("game", false); B.requestPresentation(); }
    }
    function refreshLabels() {
      set($("radio-value"), ["Full", "Brief", "Off"][save.r]);
      set($("remarks-value"), save.rr ? "On" : "Off");
      set($("music-value"), ["Off", "Menus", "Full"][save.mu]);
      set($("music-volume-value"), ["Low", "Mid", "High"][save.mv]);
    }
    function onMode(mode) {
      if (!panel) return;
      if (mode === "victory" || mode === "game-over") {
        if (M.on && !M.ended && !M.replay) finish(mode);
        screen = "results"; render();
      } else if (mode === "paused") { screen = P && P.on ? P.pauseScreen() : "pause"; render(); }
      else if (mode === "duel-pass") { screen = "duel"; render(); }
      else if (mode === "replay-done") { screen = "replaydone"; render(); }
      else if (mode === "playing" || mode === "generating") {
        ranOnce = true;
        if (screen !== "game") {
          screen = "game"; panel.setAttribute("data-screen", "game");
          const shell = $("game-shell");
          if (shell) shell.classList.add("in-run");
        }
      } else if (mode === "title" && (screen === "game" || screen === "results"
          || screen === "pause" || screen === "replaydone" || screen === "duel"
          || screen === "range" || screen === "drilldone")) {
        if (P && P.on) P.leave();
        stack = []; screen = "main"; render();
      }
    }
    /* Gamepad menus: only after Page controls hand the pad to the page.
       Before that, Tilefinch's own focus navigation drives the buttons. */
    function poll(dt) {
      if (!panel || panel.hidden) return;
      if (screen === "faults" || screen === "practice") watchNote(dt);
      if (radioShown < radioLines.length && (screen === "briefing")
          && performance.now() >= radioNext) {
        radioShown++; radioPage = (radioShown - 1) / RADIO_PAGE | 0;
        radioNext = performance.now() + 2600; drawRadio();
      }
      // game.js reads the pad once per step for the menus and the controls.
      const gamepad = B.connectedGamepad();
      if (!gamepad) { pad = 0; return; }
      const buttons = gamepad.buttons;
      let now = 0;
      for (let at = 0; at < PAD_BUTTONS.length; at++) {
        const b = buttons[PAD_BUTTONS[at]];
        if (b && b.pressed) now |= 1 << at;
      }
      const pressed = now & ~pad;
      if (now & 256) { if (!(pad & 256)) startHeld = performance.now(); }
      else if (pad & 256) {
        const held = performance.now() - startHeld;
        if (held >= 500 && radioShown < radioLines.length) skipRadio();
        else if (screen === "briefing") deploy();
        else if (screen === "quick") { const play = $("play"); if (play) play.click(); }
      }
      pad = now;
      if (!pressed) return;
      if (pressed & 3) move(pressed & 1 ? -1 : 1);
      if (pressed & 12) {
        const focused = document.activeElement;
        if (focused && focused.id === "mode-choice") cycleMode(pressed & 4 ? -1 : 1);
        else if (focused && focused.id === "controls-choice") cycleScheme(pressed & 4 ? -1 : 1);
        else if (focused && focused.id === "aim-choice") cycleAim(pressed & 4 ? -1 : 1);
        else move(pressed & 4 ? -1 : 1);
      }
      if (pressed & 16) { const focused = document.activeElement; if (focused && focused.click) focused.click(); }
      if (pressed & 32) back();
      if (pressed & 192) switchTab(pressed & 64 ? -1 : 1);
    }
    function move(delta) {
      const items = focusables();
      if (!items.length) return;
      const at = items.indexOf(document.activeElement);
      const next = items[(at < 0 ? 0 : at + delta + items.length) % items.length];
      try { next.focus(); } catch (_) {}
    }
    function switchTab(delta) {
      if (screen === "campaign") { theater = (theater + delta + 5) % 5; render(); }
      else if (screen === "settings") { tab = (tab + delta + 3) % 3; render(); }
      else if (screen === "faults") { tab = (tab + delta + 2) % 2; render(); }
      else if (screen === "controls") { tab = (tab + delta + 3) % 3; render(); }
      else if (screen === "logs") { logPage = Math.max(0, logPage + delta); render(); }
      else if (screen === "briefing" && radioShown > RADIO_PAGE) {
        radioPage = Math.max(0, Math.min((radioShown - 1) / RADIO_PAGE | 0, radioPage + delta));
        drawRadio();
      }
    }
    function cycleMode(delta) {
      const st = B.state;
      st.gameMode = (st.gameMode + delta + B.GAME_MODES.length) % B.GAME_MODES.length;
      B.refreshSetupLabels(); B.savePreferences(); refreshMessage();
    }
    function cycleScheme(delta) {
      const prefs = B.preferences;
      prefs.controls = (prefs.controls + delta + 3) % 3;
      B.clearSchemeKeys(); B.resetAimAssist(); B.refreshSetupLabels(); B.savePreferences();
      B.updateControlHint();
      if (screen === "controls") render();
    }
    /* The aim guide level shown on Controls (the range's own while it
       runs). */
    function cycleAim(delta) {
      if (!B) return;
      B.cycleAimGuide(delta);
      if (screen === "controls") render();
    }
    function disarm() {
      if (M.armed && B && M.quickMode >= 0) { B.state.gameMode = M.quickMode; B.refreshSetupLabels(); }
      M.armed = false; M.quickMode = -1;
      if (B && decode(B.state.dailyDay)) B.state.dailyDay = 0;
      if (M.on && B && !B.replaying()) deactivate();
    }
    if (list) list.addEventListener("click", (event) => {
      const at = buttons.indexOf(event.target);
      if (!B) {
        /* Before game.js attaches, Continue is queued like an early Deploy
           (not when a game script failed to load: see index.html). */
        if (at === 0 && save.d >= 0 && !globalThis.__treadlineBootFailed) {
          queued = nextMission(); claim();
        }
        return;
      }
      if (at >= 0 && actions[at]) actions[at]();
    });
    function faultNote(el) {
      const at = buttons.indexOf(el);
      if (P && message) P.focusNote(screen, at, message);
      if (screen === "faults" && faultNotes[at])
        set(message, `Score x${multiplier().toFixed(2)} | ${faultNotes[at]}`);
    }
    /* Notes poll focus rather than listen for focusin: with no focus-event
       listener on the page, Tilefinch moves focus without dispatching
       blur/focusout/focus/focusin through script on every D-pad step. */
    function watchNote(dt) {
      const focused = document.activeElement;
      if (focused !== noteFor) {
        noteFor = focused; noteWait = NOTE_REST; noteSince = performance.now();
      } else if (noteWait > 0 && ((noteWait -= dt) <= 0
          || performance.now() - noteSince >= NOTE_REST * 1000)) {
        noteWait = 0; faultNote(focused);
      }
    }
    if (tabs) tabs.addEventListener("click", (event) => {
      const at = tabPool.indexOf(event.target);
      if (at >= 0 && tabPick) tabPick(at);
    });
    addEventListener("keydown", (event) => {
      if (!panel || panel.hidden || (B && B.state.mode === "playing")) return;
      const tag = event.target && event.target.tagName;
      if (tag === "TEXTAREA" || tag === "INPUT") return;
      const key = event.key;
      if (key === "ArrowUp" || key === "ArrowLeft" || key === "ArrowDown" || key === "ArrowRight") {
        const backward = key === "ArrowUp" || key === "ArrowLeft";
        const focusedId = document.activeElement && document.activeElement.id;
        if ((key === "ArrowLeft" || key === "ArrowRight") && focusedId === "mode-choice")
          cycleMode(backward ? -1 : 1);
        else if ((key === "ArrowLeft" || key === "ArrowRight") && focusedId === "controls-choice")
          cycleScheme(backward ? -1 : 1);
        else if ((key === "ArrowLeft" || key === "ArrowRight") && focusedId === "aim-choice")
          cycleAim(backward ? -1 : 1);
        else move(backward ? -1 : 1);
      /* Escape pauses and resumes like P, through game.js's handler alone:
         also treating it as Back here resumed the run, and the queued
         pause key then paused it again on the next frame. */
      } else if (key === "Backspace" || (key === "Escape" && !(B && B.state.mode === "paused"))) back();
      else if (key === "PageUp" || key === "PageDown") switchTab(key === "PageUp" ? -1 : 1);
      else if (key === " " && document.activeElement && document.activeElement.click) {
        document.activeElement.click();
      } else return;
      event.preventDefault();
    });
    function claim() { globalThis.__treadlineControls?.claim($("game-shell")); }
    function skipRadio() {
      radioShown = radioLines.length;
      radioPage = Math.max(0, radioShown - 1) / RADIO_PAGE | 0; drawRadio();
    }
    // The static first paint already shows Main; label it from the save now.
    if (panel && panel.getAttribute("data-screen") === "main") render();
    const kit = {show, back, render, toMain, disarm, writeSave, button, set, heading,
      message, list};
    return {show, back, render, poll, onMode, toMain, refreshLabels, refreshMessage,
      skipRadio, switchTab, cycleScheme, cycleAim, kit,
      get screen() { return screen; }, get stack() { return stack.slice(); },
      focusables, select(at) { selected = at; }, set theater(at) { theater = at; }};
  })();

  // -------------------------------------------- setup buttons (from game.js)
  function wire() {
    const st = B.state, prefs = B.preferences, ui = B.ui;
    const after = () => {
      B.refreshSetupLabels(); B.savePreferences(); menu.refreshMessage(); menu.refreshLabels();
    };
    const on = (button, action) => { if (button) button.addEventListener("click", action); };
    on(ui.modeChoice, () => { st.gameMode = (st.gameMode + 1) % B.GAME_MODES.length; after(); });
    on(ui.classChoice, () => {
      if (st.gameMode === B.MODE_DAILY) return;
      st.classChoice = (st.classChoice + 1) % B.CLASSES.length; after();
    });
    on(ui.gadgetChoice, () => {
      if (st.gameMode === B.MODE_DAILY) return;
      st.gadgetChoice = (st.gadgetChoice + 1) % B.GADGETS.length; after();
    });
    on(ui.difficultyChoice, () => {
      if (st.gameMode === B.MODE_DAILY) return;
      st.difficultyChoice = (st.difficultyChoice + 1) % 3; after();
    });
    on(ui.reverseChoice, () => { prefs.reverse = (prefs.reverse + 1) % 3; after(); });
    on(ui.paletteChoice, () => {
      prefs.palette = (prefs.palette + 1) % B.PALETTES.length; B.applyPalette(); after();
    });
    on(ui.paintChoice, () => {
      for (let at = 0; at < B.PAINT_NAMES.length; at++) {
        prefs.paint = (prefs.paint + 1) % B.PAINT_NAMES.length;
        if (st.medals & (1 << prefs.paint)) break;
      }
      after();
    });
    on(ui.cameraChoice, () => {
      prefs.camera = prefs.camera ? 0 : 1;
      if (!prefs.camera) B.cameraReset();
      after();
    });
    on(ui.controlsChoice, () => menu.cycleScheme(1));
    on(ui.aimChoice, () => menu.cycleAim(1));
    on(ui.assistChoice, () => {
      prefs.assist = (prefs.assist + 1) % 3; B.resetAimAssist(); after(); B.updateControlHint();
    });
    on(ui.commandChoice, () => { prefs.command = !prefs.command; after(); B.updateHud(true); });
    // Music lives in this save: Full -> Off -> Menus -> Full, Low/Mid/High.
    on(ui.musicChoice, () => {
      prefs.music = save.mu = (save.mu + 1) % 3; writeSave(); after();
      B.sounds.start(); B.sounds.setMusic(prefs.music);
    });
    on($("music-volume-choice"), () => {
      prefs.musicVolume = save.mv = (save.mv + 1) % 3; writeSave(); after();
    });
    on(ui.effectsChoice, () => { prefs.effects = !prefs.effects; after(); });
    on(ui.shakeChoice, () => {
      prefs.shake = !prefs.shake; if (!prefs.shake) st.shake = 0; after();
    });
    on(ui.objectiveChoice, () => { prefs.objectiveArrow = (prefs.objectiveArrow + 1) % 3; after(); });
    on($("radio-choice"), () => { save.r = (save.r + 1) % 3; writeSave(); menu.refreshLabels(); });
    on($("remarks-choice"), () => { save.rr ^= 1; writeSave(); menu.refreshLabels(); });
  }

  function attach(bridge) {
    B = bridge;
    B.preferences.music = save.mu; B.preferences.musicVolume = save.mv;
    wire();
    if (P) P.attach(B, menu.kit);
    // Qualification and test runs only: the harness surface below.
    if (B.lendTooling) B.lendTooling("campaign", tooling());
    menu.refreshLabels();
    if (B.state.mode === "title") menu.render();
    if (queued >= 0) { const at = queued; queued = -1; runRadio = save.r; startMission(at, false); }
  }

  /* Aim guide rules (game.js asks per drawn frame; constant time). A
     camouflaged foe is hidden until it fires, for the same 1.5 s the
     Whiteout medal counts it as revealed; Fog of Memory hides every hit
     indicator. */
  function hidesHits() { return M.on && (M.f & FOG) !== 0; }
  /* The objective game.js marks (world mast, HUD arrow; constant time):
     -1 outside a mission; 0 where the foes come to you (Auto hides the
     arrow); 1 where game.js's own target needs it: Team Control's
     transmitter, a convoy, the monument or the crawler, and the stationary
     targets of 1-1 and 1-2; 2 after offering this mission's own targets as
     offer(key, x, z), keys from 8: the standing barriers of 2-3 and the
     salvage cards of 5-2 still to collect. */
  function objective(offer) {
    if (!M.on) return -1;
    const id = M.m.id;
    if (M.m.mode || id === "1-1" || id === "1-2") return 1;
    let offered = 0;
    if (id === "2-3") {
      for (let at = 0; at < B.barriers.length; at++) {
        const barrier = B.barriers[at];
        if (barrier.present && barrier.active) { offer(8 + at, barrier.x, barrier.z); offered++; }
      }
    } else if (id === "5-2" && M.salvageSlots) {
      for (let k = 0; k < 5; k++) {
        const at = M.salvageSlots[k];
        if (at < 0 || (M.salvage & 1 << k) || !B.pickups[at].active) continue;
        offer(16 + k, B.pickups[at].x, B.pickups[at].z); offered++;
      }
    }
    return offered ? 2 : 0;
  }

  function concealed(id) {
    if (!M.on || !(M.camo >> id & 1)) return false;
    const t = B.tanks[id];
    return !(t.fireWindup > 0 || t.recoil > 0 || M.revealAt[id] > M.t - 1.5);
  }

  // runtime: the mission state bots.js and music.js read (runtime.on, .m).
  const api = {attach, arena, owns, tick, damage, hidesHits, concealed, objective,
    showMenu(name) { menu.show(name); }, runtime: M};
  /* The harness surface (qualification.js's __treadlineDebug.campaign):
     game.js asks for it only in qualification and test runs, so ordinary
     play never builds it. None of it runs per frame. */
  function tooling() {
    // The public hooks too, by prototype (so live getters stay live).
    return Object.freeze(Object.assign(Object.create(api), {
      missions: MISSIONS, theaters: THEATERS, faults: FAULTS, lore: LORE, toasts: TOAST,
      save, loadSave, writeSave, encode, decode, runCode,
      startMission, menu, medalCount, cardCount,
      setRadio(value) { save.r = value; runRadio = value; },
      bridge() { return B; }, medalTests: MEDAL_TESTS,
    }));
  }
  Object.defineProperty(globalThis, "__treadlineCampaign", {
    value: Object.freeze(api), configurable: false, writable: false,
  });
})();
