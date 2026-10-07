/* Inserted into the game's lexical scope only by --treadline-host-profile.
 * Fixed 30Hz simulation + instance preparation, NOT PSP renderer/vblank time.
 * Work counters are integers, so clustering comparisons do not depend on clocks.
 */
  globalThis.__treadlineHostStart = (seed, arena) => {
    // qualification.js (evaluated after game.js) holds the harness API.
    __treadlineDebug.beginLeague(seed, 1, 2, 1, arena);
    botLeagueActive = false; qualificationHooks.leaguePlayer = null;
    preferences.effects = preferences.music = false;
    state.gameMode = 3; state.wave = 5;
    beginArena(arena, true);
    state.mode = "playing"; state.time = state.wallTime = state.frames = 0;
    replayPending = replayRecording = false;
    input.keys.w = input.keys.fire = true;
  };
  globalThis.__treadlineHostFrame = () => {
    let reset = 0;
    if (state.mode !== "playing" || !playerTank().active) {
      beginArena(state.arena, true); state.mode = "playing";
      state.pendingClear = false; state.killBeat = 0; reset = 1 << 30;
    }
    playerTank().spawnGrace = 1;
    const phase = state.frames % 240;
    input.keys.a = phase < 60;
    input.keys.d = phase >= 120 && phase < 180;
    input.keys.aimLeft = phase < 120;
    input.keys.aimRight = phase >= 120;
    input.keys.secondary = phase >= 110 && phase < 113;
    input.keys.gadget = phase >= 140 && phase < 143;
    input.keys.ultimate = phase >= 170 && phase < 173;
    hostProxyWork.fill(0);
    update(1 / 30);
    prepareFrame(false);
    state.frames++;
    if (hostProxyWork[0] > 6 || hostProxyWork[1] > 6
        || hostProxyWork[2] >= 4095 || hostProxyWork[3] > 6) return -1;
    return reset | hostProxyWork[0] | (hostProxyWork[1] << 3)
      | (hostProxyWork[2] << 6) | (hostProxyWork[3] << 18)
      | (hostProxyWork[4] << 21);
  };
  globalThis.__treadlineHostFairness = () => {
    const last = new Int16Array(6), seen = new Uint8Array(6);
    for (let first = 1; first >= 0; first--) {
      beginArena(state.arena, true); state.mode = "playing";
      botLeagueActive = first === 0; qualificationHooks.leaguePlayer = null;
      last.fill(-1); seen.fill(0);
      for (let frame = 0; frame < 48; frame++) {
        for (let at = first; at < MAX_TANKS; at++) {
          tanks[at].active = true; tanks[at].spawnGrace = 1;
          tanks[at].strategyNext = -1;
        }
        const mask = (__treadlineHostFrame() >>> 21) & 63;
        for (let at = first; at < MAX_TANKS; at++) {
          if (mask & (1 << at)) { seen[at] = 1; last[at] = frame; }
          if (frame - last[at] > 6) return false;
        }
      }
      for (let at = first; at < MAX_TANKS; at++) if (!seen[at]) return false;
    }
    botLeagueActive = false;
    return true;
  };
