/* Treadline Arena: qualification and harness tooling.

   Not part of the game. game.js fetches this script only for a
   qualification URL (?qualification=...), and a harness that evaluates the
   game scripts itself sets globalThis.__treadlineEnableDebug = true before
   game.js and evaluates this file after it (docs/DEVELOPMENT.md, "Canvas
   and WebGL game qualification"). Ordinary play never loads it, and an
   installed game carries it only when its package was built for
   qualification (the files package-files.txt marks "qualification").

   It provides:
   - __treadlineDebug, the harness API (fixtures, input-script companions,
     the bot league, the campaign and invariant sweeps);
   - the phase-clock profile behind __tilefinchStartInputProfile, its
     TREADLINE-JS-PHASES report (__treadlinePhaseReport) and the ranked
     slow frames behind __treadlineDebug.slowProfile();
   - the soak's repeating action cycle and the long soak's driver.

   game.js lends it a bridge (qualificationBridge): its bindings, accessors
   for the values it reassigns, and installers for the clocks and hooks. The
   hooks run only in qualification runs: one call per frame for the action
   cycle, two per measured profile frame, and the long-soak driver, which
   binds everything it calls once, here. */
(() => {
  "use strict";
  if (globalThis.__treadlineDebug
      || typeof globalThis.__treadlineQualify !== "function") return;
  globalThis.__treadlineQualify((G) => {
    const {
      AIM_FULL, AIM_SCORE_MULTIPLIERS, ARENAS, ASSIST_LOCK, ASSIST_OFF,
      ASSIST_SNAP, BOSS, BOT_FIRE_RANGE_SQUARED, CLASSES, CLASS_COLLISION_RADIUS,
      CLASS_COMBINED_RADIUS_SQUARED, COLLISION_GRID_CELLS, COLLISION_GRID_SIDE,
      CRATE_HALF,
      DIFFICULTIES, GADGETS, GAME_MODES, GENERATED_ARENA_INDEX,
      HUD_GLYPH_LIMIT, MODE_CONTROL, HUD_OBJECTIVE_RADIUS_X, HUD_OBJECTIVE_RADIUS_Y, HUD_PRIMITIVE_LIMIT,
      HUD_TEXT, HUD_VERTEX_WORDS, HULL_EDGE, MAX_BARRIERS, MAX_BOX_INSTANCES,
      MAX_BULLETS, MAX_DECALS, MAX_TANKS, PAINT_NAMES, REPLAY_CAPACITY,
      RETAINED_TANK_PARTS, REVERSE_DEGREES, TRACER_SLOTS, TRACER_VERTICES,
      activateCommand, activeBarrierMask, activeEnemyCount, addHudGlyph,
      advanceElapsed, aimGuide, aimGuideLevel, applyOnlineSnapshot,
      applyPalette, applyScreenAim, arenaGenerator, arenaGeometryCache,
      barrierCircleGrid, barriers, beginArena, beginBotFireWindup,
      botDebug, botDifficultyTables, botDifficultyValue, boxInstanceSlotOwners,
      boxInstanceTints, bulletHitsObstacle, bulletTracer, bullets,
      cancelSpawnChecks, circleHitsObstacle, clearSchemeKeys, convoy,
      crateCircleGrid, crates, dailySeed, damageTank, decals,
      elevatedFiringOrigin, explodeCrate,
      exportReplay, fillArenaSpatialData, fillBarrierCircleGrid, fillRayGeometry,
      fillTinyCircleOccupied, finalScore, finishReplayRecording, fireSecondary,
      generatedArena, gl, hazards, hudGlyphRuns, hudGlyphStarts,
      hudModeVisible, hudVertices, importReplay, input,
      invalidateBotNavigation, lineCrossesWalls, loadPreferences, noteArenaGeneration, mines,
      navigationCell, objectiveArrowShown, objectivePosition, online,
      orientationIndex, particles, pickups, placeTank, playerTank, pocketKept,
      pollPlayerInput, preferences, prepareNavigationGrids, program,
      qualificationAutoStart, qualificationKind, qualificationLongSoak, urlSwitch,
      refreshSetupLabels, render, resetAimAssist, resetGame,
      retainedTankPartSlots, savePreferences, sceneryTintLocation,
      segmentHitsBox, sendOnlineSnapshot, steerBotRoute, bankAim, setBulletActive, setMode,
      setParticleActive, setQualificationAIActive, setSource, smokeClouds,
      sounds, spawnDecal, spawnHazard, spawnParticles, startOrResume,
      startReplay, state, surfaceHeightAt, tanks, toolingSurfaces,
      tracerBullet, tracerHead,
      tracerVertexCount, tracerVertices, unlockMedal, update, updateAimGuide,
      updateAimGuideEnemy, updateBotCommand, updateBullets, updateCamera,
      updateControlHint, updateHud, updateOverlay, updateRayActiveMask,
      updateTankTurretCache, updateTankYawCache, useGadget,
      view, viewProjection, wrapAngle
    } = G;

    // -------------------------------------------------------- profile --
    /* Totals and maxima per frame phase, filled by game.js's clocks on
       measured frames. A long profile (qualification=input) keeps 540
       frames, covering a whole scripted button workload. */
    const timing = {
      samples: 0, update: 0, camera: 0, build: 0, upload: 0, hud: 0,
      commands: 0, frame: 0, maxUpdate: 0, maxCamera: 0, maxBuild: 0,
      maxUpload: 0, maxHud: 0, maxCommands: 0, maxFrame: 0,
      updatePrelude: 0, updateInput: 0, updateBots: 0, updateMove: 0,
      updateProjectiles: 0, updateWorld: 0, updateUi: 0,
      maxUpdatePrelude: 0, maxUpdateInput: 0, maxUpdateBots: 0, maxUpdateMove: 0,
      maxUpdateProjectiles: 0, maxUpdateWorld: 0, maxUpdateUi: 0,
      buildScenery: 0, buildTanks: 0, buildEffects: 0,
      instances: 0, maxInstances: 0,
    };
    const sampleLimit = qualificationKind === "input" ? 540 : 180;
    // AI, movement and routing phases; see aiRows in slowProfile().
    const aiTimes = new Float64Array(31), aiCounts = new Uint32Array(7);
    const moveTimes = new Float64Array(8);
    // Total/max pairs: full envelope, cancellation, reset, attack, release.
    const soundTimes = new Float64Array(10);
    // Main cannon, secondary, gadget, command: totals then maxima.
    const actionTimes = new Float64Array(8), actionCounts = new Uint16Array(4);
    const hudPhaseTotals = new Float32Array(7);
    const hudPhaseMaximums = new Float32Array(7);
    const hudPhaseCounts = new Uint16Array(7);
    // HUD indicator rebuilds: total, maximum, count.
    const hudIndicator = new Float64Array(3);
    G.installProfile({sampleLimit, timing, aiTimes, aiCounts, moveTimes,
      soundTimes, actionTimes, actionCounts, hudPhaseTotals, hudPhaseMaximums,
      hudPhaseCounts, hudIndicator});

    // The slowest measured frames, ranked, with their AI phase deltas.
    const SLOW_LIMIT = 16, SLOW_WORDS = 10;
    const slowFrames = new Float32Array(SLOW_LIMIT * SLOW_WORDS);
    const slowAI = new Float32Array(SLOW_LIMIT * aiTimes.length);
    const slowEvents = new Uint8Array(SLOW_LIMIT);
    const aiBefore = new Float64Array(aiTimes.length);
    let slowCount = 0;
    const reports = urlSwitch("profile-report") !== "off";
    const bridgeProfile = urlSwitch("bridge-profile") !== "off";
    const BRIDGE_COUNTERS = ["profileDraws", "profileBasicMs",
      "profileInstancesMs", "profileRangesMs", "profilePrepareMs",
      "profileEnqueueMs", "profileFinishMs", "profileQueueAdmissionMs",
      "profileWirePackMs", "profileWireSourcesMs", "profileWireStateMs",
      "profileWireInstancesMs", "profileWireRetainMs", "drawPlanHits",
      "drawPlanMisses", "commandTemplateHits",
      "commandTemplateMisses", "commandSlotTemplateHits",
      "commandSlotTemplateMisses", "sourcePacketHits",
      "sourcePacketMisses", "sourcePacketIndexHits",
      "sourcePacketIndexMisses"];
    let profileStarted = false;
    function startProfile() {
      profileStarted = true;
      const bridge = globalThis.__tilefinchWebGLDiagnostics;
      if (!bridge) return;
      for (const key of BRIDGE_COUNTERS) bridge[key] = 0;
      bridge.profileDrawPhases = bridgeProfile;
    }

    // Validation profiles real input, never auto-starts or synthesizes commands.
    globalThis.__tilefinchStartInputProfile = () => {
      aiTimes.fill(0); aiCounts.fill(0); moveTimes.fill(0); soundTimes.fill(0);
      actionTimes.fill(0); actionCounts.fill(0);
      slowCount = 0; slowFrames.fill(0); slowEvents.fill(0); slowAI.fill(0);
      for (const key of Object.keys(timing)) timing[key] = 0;
      profileStarted = false;
      G.validationInputProfile = true;
    };

    function recordSlowFrame(event, update, prelude, bots, move, projectiles,
                             world, build, hud, commands, total) {
      let insertion = slowCount;
      if (insertion === SLOW_LIMIT) {
        if (total <= slowFrames[(SLOW_LIMIT - 1) * SLOW_WORDS + 9]) return;
        insertion--;
      } else slowCount++;
      const aiWords = aiTimes.length;
      while (insertion > 0) {
        const before = (insertion - 1) * SLOW_WORDS;
        if (slowFrames[before + 9] >= total) break;
        slowFrames.copyWithin(insertion * SLOW_WORDS, before, before + SLOW_WORDS);
        slowEvents[insertion] = slowEvents[insertion - 1];
        slowAI.copyWithin(insertion * aiWords, (insertion - 1) * aiWords,
          insertion * aiWords);
        insertion--;
      }
      const at = insertion * SLOW_WORDS;
      slowFrames[at] = update; slowFrames[at + 1] = prelude;
      slowFrames[at + 2] = bots; slowFrames[at + 3] = move;
      slowFrames[at + 4] = projectiles; slowFrames[at + 5] = world;
      slowFrames[at + 6] = build; slowFrames[at + 7] = hud;
      slowFrames[at + 8] = commands; slowFrames[at + 9] = total;
      slowEvents[insertion] = event;
      for (let word = 0; word < aiWords; word++)
        slowAI[insertion * aiWords + word] = aiTimes[word] - aiBefore[word];
    }

    // The one report a script reads (scripts/run-ppsspp-input-script.sh).
    function reportPhases() {
      const count = timing.samples;
      const bridge = globalThis.__tilefinchWebGLDiagnostics;
      const draws = Math.max(1, Number(bridge?.profileDraws) || 0);
      const perDraw = (key) => (Number(bridge?.[key] || 0) / draws).toFixed(3);
      const ratio = (hits, misses) =>
        `${Number(bridge?.[hits] || 0)}/${Number(bridge?.[misses] || 0)}`;
      const mean = (value) => (value / count).toFixed(3);
      const hudPhases = [];
      for (let phase = 0; phase < hudPhaseCounts.length; phase++)
        hudPhases.push(`${phase}:`
          + `${(hudPhaseTotals[phase] / Math.max(1, hudPhaseCounts[phase])).toFixed(2)}`
          + `/${hudPhaseMaximums[phase].toFixed(2)}/${hudPhaseCounts[phase]}`);
      const report = ["TREADLINE-JS-PHASES", `samples=${count}`,
        `slot=${ratio("commandSlotTemplateHits", "commandSlotTemplateMisses")}`,
        `packet=${ratio("sourcePacketHits", "sourcePacketMisses")}`,
        `pre=${mean(timing.updatePrelude)}`, `input=${mean(timing.updateInput)}`,
        `bots=${mean(timing.updateBots)}`, `move=${mean(timing.updateMove)}`,
        `projectiles=${mean(timing.updateProjectiles)}`,
        `world=${mean(timing.updateWorld)}`, `ui=${mean(timing.updateUi)}`,
        `scenery=${mean(timing.buildScenery)}`, `tanks=${mean(timing.buildTanks)}`,
        `effects=${mean(timing.buildEffects)}`,
        `instances=${(timing.instances / count).toFixed(1)}`,
        `maxinst=${timing.maxInstances}`,
        `kills=${state.kills}`, `shots=${state.shots}`,
        `blocked=${playerTank().blockedTime.toFixed(3)}`,
        `camera=${mean(timing.camera)}`, `upload=${mean(timing.upload)}`,
        `hud=${mean(timing.hud)}`, `commands=${mean(timing.commands)}`,
        `frame=${mean(timing.frame)}`, `action=${actionMax.toFixed(3)}`,
        `maxu=${timing.maxUpdate.toFixed(3)}`,
        `maxpre=${timing.maxUpdatePrelude.toFixed(3)}`,
        `maxinput=${timing.maxUpdateInput.toFixed(3)}`,
        `maxbot=${timing.maxUpdateBots.toFixed(3)}`,
        `maxmove=${timing.maxUpdateMove.toFixed(3)}`,
        `maxproj=${timing.maxUpdateProjectiles.toFixed(3)}`,
        `maxworld=${timing.maxUpdateWorld.toFixed(3)}`,
        `maxui=${timing.maxUpdateUi.toFixed(3)}`,
        `maxb=${timing.maxBuild.toFixed(3)}`, `maxup=${timing.maxUpload.toFixed(3)}`,
        `maxhud=${timing.maxHud.toFixed(3)}`, `hudph=${hudPhases.join(",")}`,
        `hudi=${(hudIndicator[0] / Math.max(1, hudIndicator[2])).toFixed(2)}`
          + `/${hudIndicator[1].toFixed(2)}/${hudIndicator[2]}`,
        `maxcmd=${timing.maxCommands.toFixed(3)}`,
        `maxframe=${timing.maxFrame.toFixed(3)}`,
        `repeatcap=${G.repeatedFrameCaptures}`,
        `idx=${ratio("sourcePacketIndexHits", "sourcePacketIndexMisses")}`,
        `b-basic=${perDraw("profileBasicMs")}`, `b-inst=${perDraw("profileInstancesMs")}`,
        `b-range=${perDraw("profileRangesMs")}`, `b-prep=${perDraw("profilePrepareMs")}`,
        `b-enq=${perDraw("profileEnqueueMs")}`, `b-finish=${perDraw("profileFinishMs")}`,
        `b-admit=${perDraw("profileQueueAdmissionMs")}`,
        `b-pack=${perDraw("profileWirePackMs")}`,
      ].join(" ");
      console.log(report);
      // Kept apart too: a later game event may overwrite pocSummary.
      globalThis.pocSummary = globalThis.__treadlinePhaseReport = report;
      if (bridge) bridge.profileDrawPhases = false;
    }

    // ----------------------------------------------------- frame hooks --
    /* The soak's action cycle: the four expensive interactions, repeated
       without growing any pool. The quiet part of each eight-second cycle
       still exercises ordinary play, AI, collision and reclamation. A
       measured frame records which action it carried (slow-frame events). */
    let actionFrames = 0, actionKind = 0, actionSamples = 0, actionMax = 0;
    function runAction(frameNumber) {
      if (!qualificationAutoStart || state.mode !== "playing") return;
      if (qualificationLongSoak) frameNumber %= 240;
      const player = tanks[0];
      if (frameNumber === 80) {
        actionKind = 1;
        spawnParticles(player.x, .45, player.z, 18, 3.2);
      } else if (frameNumber === 110) {
        actionKind = 2;
        player.secondaryCooldown = 0;
        fireSecondary(player);
      } else if (frameNumber === 140) {
        actionKind = 3;
        player.gadget = "SMOKE";
        player.gadgetCooldown = 0;
        useGadget(player);
      } else if (frameNumber === 170) {
        actionKind = 4;
        const target = tanks[1];
        if (target && target.active) {
          target.spawnGrace = 0;
          damageTank(target, target.maxHealth + 1, 0, player.x, player.z, true);
        }
      } else return;
      // The triggering frame and the two after it, where the new particles,
      // projectiles and HUD state first reach the renderer.
      actionFrames = 3;
    }
    let frameStarted = 0, preludeBefore = 0, botsBefore = 0, moveBefore = 0;
    let projectilesBefore = 0, worldBefore = 0, buildBefore = 0, hudBefore = 0;
    let commandsBefore = 0;
    /* game.js reads three switches off this object: frozen bots (no bot
       planning; a replay clears it), the killcam (qualification runs keep
       it off unless a test turns it on) and the bot league's scripted
       player (leaguePlayer, null when the AI drives both sides). */
    const hooks = {
      botsFrozen: false, killcam: false, leaguePlayer: null,
      frame(frameNumber, idle) {
        if (actionFrames > 0 && --actionFrames === 0) actionKind = 0;
        if (idle) runAction(frameNumber);
      },
      measureStart() {
        if (!profileStarted) startProfile();
        frameStarted = performance.now();
        preludeBefore = timing.updatePrelude; botsBefore = timing.updateBots;
        moveBefore = timing.updateMove; projectilesBefore = timing.updateProjectiles;
        worldBefore = timing.updateWorld; buildBefore = timing.build;
        hudBefore = timing.hud; commandsBefore = timing.commands;
        aiBefore.set(aiTimes);
      },
      measureEnd(updateElapsed) {
        timing.update += updateElapsed;
        timing.maxUpdate = Math.max(timing.maxUpdate, updateElapsed);
        const frameElapsed = performance.now() - frameStarted;
        timing.frame += frameElapsed;
        timing.maxFrame = Math.max(timing.maxFrame, frameElapsed);
        recordSlowFrame(actionKind, updateElapsed,
          timing.updatePrelude - preludeBefore, timing.updateBots - botsBefore,
          timing.updateMove - moveBefore,
          timing.updateProjectiles - projectilesBefore,
          timing.updateWorld - worldBefore, timing.build - buildBefore,
          timing.hud - hudBefore, timing.commands - commandsBefore, frameElapsed);
        if (actionFrames > 0) {
          actionSamples++;
          actionMax = Math.max(actionMax, frameElapsed);
        }
        if (++timing.samples === sampleLimit && reports) reportPhases();
      },
    };
    G.setHooks(hooks);

    /* The bot league's and the invariant sweep's statistics, which game.js
       counts while they are installed: per tank shots, hits, damage
       contacts, shots spent on scenery, blocked seconds and front and rear
       damage taken; Team Control seconds per side; bot telegraphs and shots
       at the player per arena. bulletTurns holds where each shell turned
       during its last step (x then z per turn). */
    const stats = {shots: new Uint32Array(MAX_TANKS), hits: new Uint32Array(MAX_TANKS),
      damageContacts: new Uint32Array(MAX_TANKS), wallShots: new Uint32Array(MAX_TANKS),
      stuck: new Float64Array(MAX_TANKS), frontDamage: new Float64Array(MAX_TANKS),
      rearDamage: new Float64Array(MAX_TANKS), control: new Float64Array(2),
      botTelegraphs: 0, botPlayerShots: 0, botTelegraphViolations: 0, botVolleyMinimum: 99};
    const bulletTurns = new Float64Array(MAX_BULLETS * 8);
    G.installStats(stats, bulletTurns);

    // ------------------------------------------------- long-soak driver --
    /* A repeatable 360-frame path through translation, turning, obstacle
       contact and backing away, for when the player has no target. */
    function setLongSoakMovement(command, frameNumber) {
      const phase = frameNumber % 360;
      if (phase < 90 || (phase >= 150 && phase < 240)) {
        command.left = command.right = 1;
        command.reverse = false;
      } else if (phase < 150) {
        command.left = .28;
        command.right = 1;
        command.reverse = false;
      } else if (phase < 300) {
        command.left = 1;
        command.right = .28;
        command.reverse = false;
      } else {
        command.left = command.right = 1;
        command.reverse = true;
      }
    }
    function pointInSmoke(x, z) {
      for (let at = 0; at < smokeClouds.length; at++) {
        const smoke = smokeClouds[at];
        if (!smoke.active) continue;
        const dx = x - smoke.x, dz = z - smoke.z;
        if (dx * dx + dz * dz < 3.1) return true;
      }
      return false;
    }
    if (qualificationLongSoak) G.setLongSoakDriver((frameNumber, dt) => {
      if (state.mode !== "playing") return;
      /* Exercise both camera modes for thirty seconds each. This changes
         only the in-memory preference, never the saved camera choice. */
      const camera = ((frameNumber / 900) | 0) & 1;
      if (preferences.camera !== camera) {
        preferences.camera = camera;
        if (!camera) { G.cameraYaw = 0; G.cameraSine = 0; G.cameraCosine = 1; }
      }
      /* Drive the player with the bots' own bounded combat planner: a blind
         loop parked on scenery and exercised neither aiming nor kills. */
      const player = playerTank();
      updateBotCommand(player, dt);
      const target = tanks[player.target];
      if (!target || !target.active || target.team === player.team) {
        setLongSoakMovement(player.command, frameNumber);
        return;
      }
      const dx = target.x - player.x, dz = target.z - player.z;
      const aligned = Math.abs(wrapAngle(Math.atan2(dx, dz) - player.turret)) < .18;
      // It attacks through the real bounded projectile path.
      player.command.fire = aligned && dx * dx + dz * dz < BOT_FIRE_RANGE_SQUARED
        && !pointInSmoke(player.x, player.z) && !pointInSmoke(target.x, target.z);
      if (aligned && player.secondaryCooldown <= 0 && (frameNumber % 300) === 120)
        player.command.secondary = true;
    });

    /* Every active crate and barrier, every wall and, while it is shut,
       the gate: the scenery circleHitsObstacle covers, without its grids. */
    function bruteCircleHits(x, z, radius) {
      const spatial = globalThis.__treadlineArenaData.spatial, arena = state.arena;
      const hits = (left, right, top, bottom) => {
        const dx = x < left ? x - left : x > right ? x - right : 0;
        const dz = z < top ? z - top : z > bottom ? z - bottom : 0;
        return dx * dx + dz * dz < radius * radius;
      };
      for (const crate of crates)
        if (crate.active && hits(crate.x - CRATE_HALF, crate.x + CRATE_HALF,
          crate.z - CRATE_HALF, crate.z + CRATE_HALF)) return true;
      // Off the collision grid only crates count (the arena edge is not scenery).
      if (!(x >= -8 && x < 8 && z >= -8 && z < 8)) return false;
      for (const barrier of barriers)
        if (barrier.active && hits(barrier.left, barrier.right, barrier.top,
          barrier.bottom)) return true;
      const lists = [[spatial.obstacleBounds[arena], spatial.obstacleCounts[arena]]];
      if (!state.gateOpen) lists.push([spatial.gateBounds[arena], spatial.gateCounts[arena]]);
      for (const [bounds, count] of lists)
        for (let at = 0; at < count * 4; at += 4)
          if (hits(bounds[at], bounds[at + 1], bounds[at + 2], bounds[at + 3])) return true;
      return false;
    }

    // ------------------------------------------------------ bot league --
    /* Seeded one-on-one Team Control matches between bot difficulties
       (scripts/run-treadline-bot-league.py). The player's tank is a bot
       too, or, with a script, one of three scripted drivers. */
    let leagueEffects = true, leagueMusic = 0, leagueScript = 0;
    function leaguePlayer(dt) {
      const tank = tanks[0], target = tanks[1], command = tank.command;
      let gx = tank.x, gz = tank.z;
      if (leagueScript === 1) {
        /* Circle-strafe, with a slow deterministic reversal. */
        const dx = target.x - tank.x, dz = target.z - tank.z;
        const distance = Math.sqrt(dx * dx + dz * dz) || 1;
        const side = ((state.time / 4) | 0) & 1 ? -1 : 1;
        gx += dz / distance * side + dx / distance * (distance - 4) * .5;
        gz += -dx / distance * side + dz / distance * (distance - 4) * .5;
      } else if (leagueScript === 2) {
        gx = -4.5 + (((state.time / 2) | 0) & 1) * 1.5; gz = -4;
      } else { gx = target.x; gz = target.z; }
      let steeringX = gx - tank.x, steeringZ = gz - tank.z;
      if (lineCrossesWalls(tank.x, tank.z, gx, gz, false, tank.collisionRadius + .02)
          && steerBotRoute(tank, gx, gz)) {
        steeringX = bankAim[0]; steeringZ = bankAim[1];
      }
      const turn = wrapAngle(Math.atan2(steeringX, steeringZ) - tank.yaw);
      command.left = turn > .2 ? 0 : 1;
      command.right = turn < -.2 ? 0 : 1; command.reverse = false;
      if (steeringX * steeringX + steeringZ * steeringZ < .04)
        command.left = command.right = 0;
      command.aimX = target.x - tank.x; command.aimZ = target.z - tank.z;
      command.fire = Math.abs(wrapAngle(Math.atan2(command.aimX, command.aimZ)
        - tank.turret)) < .16 && !lineCrossesWalls(tank.x, tank.z, target.x, target.z);
      command.secondary = command.gadget = command.ultimate = false;
    }

    // ---------------------------------------------- arena generation --
    // The generated tails' index budget (8 boxes and 2 ramps), reported.
    const GENERATED_STATIC_TAIL_INDEX_LIMIT = 8 * 36 + 2 * 18;
    // Generate a seed's arena at once (Deploy steps it over frames).
    function generateArena(seed) {
      const generated = arenaGenerator.generate(seed >>> 0 || 1);
      noteArenaGeneration(generated);
      fillArenaSpatialData(GENERATED_ARENA_INDEX);
      G.generatedGeometryDirty = true;
      return generated.accepted;
    }

    // ---------------------------------------------------- harness API --
    const debug = Object.freeze({
      /* The modules' harness surfaces: the campaign's (its menu, save,
         missions and bridge(), the game bindings it attached with), the
         Practice Range's and the control schemes'. */
      campaign: toolingSurfaces.campaign, practice: toolingSurfaces.practice,
      controls: toolingSurfaces.controls,
      /* Shells, where each turned during its last step (x then z per turn),
         and whether a tank fires from a ramp over cover. */
      bullets, bulletTurns, elevatedFiringOrigin, stats,
      replayStop: finishReplayRecording,
      replayStart: startReplay,
      exportReplay, importReplay, dailySeed,
      replayState() { return {count: G.replayCount, active: G.replayActive,
        recording: G.replayRecording, truncated: G.replayTruncated,
        verified: G.replayVerified, digest: G.replayDigest, capacity: REPLAY_CAPACITY}; },
      replayDigestState() {
        return {rng: G.randomState, score: state.score, lives: state.lives, wave: state.wave,
          tanks: tanks.map(t => t.active ? [t.x,t.z,t.health,t.yaw,t.turret,t.cooldown,t.fireCharge] : null),
          bullets: bullets.map(b => b.active ? [b.x,b.z,b.vx,b.vz,b.life,b.bounceCount] : null)};
      },
      enableKillcam(enabled) { hooks.killcam = !!enabled; },
      setWave(wave) {
        state.wave = Math.max(1, Math.min(999, wave | 0)); beginArena(state.arena);
      },
      damageTank(index, damage, x, z, bounces = 0) {
        const tank = tanks[index | 0];
        if (!tank) return false;
        tank.spawnGrace = 0;
        return damageTank(tank, Number(damage) || 1, 0, x, z, true, bounces | 0);
      },
      resumeDuel() { if (state.mode === "duel-pass") startOrResume(); },
      addHazard(type, x, z) { spawnHazard(Math.max(0, Math.min(2, type | 0)), x, z, 1.2, 5); },
      addDecal(kind, x, z, yaw = 0, width = .42, depth = .34) {
        spawnDecal(kind === 1 ? 1 : 2, Number(x) || 0, Number(z) || 0, Number(yaw) || 0,
          Number(width) || .42, Number(depth) || .34);
      },
      decalState() { return decals.map((decal) => ({...decal})); },
      // The first count sparks hang at (x, .3, z) for a while; the rest go out.
      setSparks(count, x = 0, z = 0) {
        for (let at = 0; at < particles.length; at++) {
          setParticleActive(at, at < count);
          if (at >= count) continue;
          const particle = particles[at];
          particle.x = x + at * .05; particle.y = .3; particle.z = z;
          particle.vx = particle.vy = particle.vz = 0;
          particle.life = particle.maximum = 5;
        }
      },
      clearCrates() {
        for (let at = 0; at < crates.length; at++) crates[at].active = false;
        invalidateBotNavigation();
      },
      crateState() { return crates.map((crate) => ({...crate})); },
      hazardState() { return hazards.map((hazard) => ({...hazard})); },
      explodeCrate(index) { if (crates[index | 0]) explodeCrate(index | 0, 0, 0); },
      setPalette(index) { preferences.palette = Math.max(0, Math.min(3, index | 0)); applyPalette(); },
      setReverse(index) { preferences.reverse = Math.max(0, Math.min(2, index | 0)); },
      getSceneryTint() { return Array.from(gl.getUniform(program, sceneryTintLocation)); },
      hudGlyphGeometry(character, scale = 1, available = HUD_PRIMITIVE_LIMIT,
                       characters = 0) {
        available = Math.max(0, Math.min(HUD_PRIMITIVE_LIMIT, available | 0));
        const saved = new Float32Array(hudVertices);
        const vertices = G.hudVertexCount, indices = G.hudIndexCount;
        const count = G.hudCharacterCount;
        const first = (HUD_PRIMITIVE_LIMIT - available) * 4;
        G.hudVertexCount = first; G.hudIndexCount = first / 4 * 6;
        G.hudCharacterCount = Math.max(0, Math.min(HUD_GLYPH_LIMIT, characters | 0));
        try {
          const accepted = addHudGlyph(String(character), -1.25, 2.5, scale, HUD_TEXT);
          return {accepted, characters: G.hudCharacterCount,
            runsBytes: hudGlyphStarts.byteLength + hudGlyphRuns.byteLength,
            words: Array.from(hudVertices.subarray(first * HUD_VERTEX_WORDS,
              G.hudVertexCount * HUD_VERTEX_WORDS))};
        } finally {
          hudVertices.set(saved);
          G.hudVertexCount = vertices; G.hudIndexCount = indices; G.hudCharacterCount = count;
        }
      },
      unlockMedal(index) { if (index > 0 && index < PAINT_NAMES.length) unlockMedal(index | 0); },
      beginLeague(seed, difficultyA = 1, difficultyB = 2, script = 0, arena = 0) {
        if (!G.botLeagueActive) { leagueEffects = preferences.effects; leagueMusic = preferences.music; }
        G.botLeagueActive = false; online.active = false;
        state.gameMode = MODE_CONTROL;
        resetGame();
        state.time = state.wallTime = state.frames = 0;
        state.lives = 1;
        stats.control.fill(0);
        state.killBeat = 0; state.pendingClear = false;
        G.randomState = (Number(seed) >>> 0) || 1;
        G.bulletSpawnCursor = G.particleSpawnCursor = G.particleTemplateCursor = 0;
        beginArena(Math.max(0, Math.min(2, arena | 0)), true);
        for (let at = 0; at < MAX_TANKS; at++) {
          const tank = tanks[at];
          tank.active = at < 2; tank.player = at === 0 && !!script;
          tank.difficulty = at === 0 ? Math.max(0, Math.min(2, difficultyA | 0))
            : Math.max(0, Math.min(2, difficultyB | 0));
        }
        for (const counts of [stats.shots, stats.hits, stats.damageContacts,
          stats.wallShots, stats.stuck, stats.frontDamage, stats.rearDamage]) counts.fill(0);
        placeTank(tanks[0], -4.5, -4.5, .7, 0, GADGETS[2], "HUNTER", 1);
        placeTank(tanks[1], 4.5, 4.5, -.7, 1, GADGETS[2], "HUNTER", 1);
        tanks[0].spawnGrace = tanks[1].spawnGrace = 0;
        preferences.effects = false; preferences.music = 0;
        G.botLeagueActive = true; leagueScript = Math.max(0, Math.min(3, script | 0));
        hooks.leaguePlayer = leagueScript ? leaguePlayer : null;
        cancelSpawnChecks();
        G.replayPending = G.replayRecording = false; hooks.botsFrozen = false;
        state.mode = "playing";
      },
      stepLeague(frames = 30) {
        frames = Math.max(1, Math.min(120, frames | 0));
        for (let at = 0; at < frames && tanks[0].active && tanks[1].active
            && state.mode === "playing"; at++) {
          update(1 / 30); state.frames++;
        }
        return !tanks[0].active || !tanks[1].active || state.mode !== "playing";
      },
      leagueResult() {
        return {winner: !tanks[0].active || state.mode === "game-over" ? 1
            : !tanks[1].active || state.mode === "victory" ? 0 : -1,
          end: !tanks[0].active || !tanks[1].active ? "kill"
            : state.mode === "playing" ? "timeout" : "objective",
          seconds: state.time, frames: state.frames, randomState: G.randomState,
          tanks: tanks.slice(0, 2).map((tank, id) => ({health: tank.health,
            x: tank.x, z: tank.z, yaw: tank.yaw, turret: tank.turret,
            shots: stats.shots[id], hits: stats.hits[id],
            damageContacts: stats.damageContacts[id],
            wastedWallShots: stats.wallShots[id], stuckSeconds: stats.stuck[id],
            frontDamage: stats.frontDamage[id], rearDamage: stats.rearDamage[id]})),
          objectiveSeconds: [stats.control[0], stats.control[1]]};
      },
      finishLeague() {
        G.botLeagueActive = false; leagueScript = 0; hooks.leaguePlayer = null;
        preferences.effects = leagueEffects; preferences.music = leagueMusic;
        for (let at = 0; at < MAX_TANKS; at++) {
          tanks[at].player = at === 0; tanks[at].difficulty = -1;
        }
        setMode("title");
      },
      botIntent(index, seconds = 1 / 30) {
        const tank = tanks[Math.max(0, Math.min(5, index | 0))];
        const wasProfiling = G.qualificationAIActive;
        setQualificationAIActive(true);
        try { updateBotCommand(tank, seconds); }
        finally { setQualificationAIActive(wasProfiling); }
        return {target: tank.target, bank: tank.bankAim,
          left: tank.command.left, right: tank.command.right,
          fire: tank.command.fire, secondary: tank.command.secondary,
          aimX: tank.command.aimX, aimZ: tank.command.aimZ,
          windup: tank.fireWindup, navigationGoal: tank.navGoal};
      },
      botDifficultyValue, botDifficultyTables,
      aiProfile() {
        return {samples: timing.samples,
          target: aiTimes[0], goal: aiTimes[1],
          route: aiTimes[2], steering: aiTimes[3],
          aim: aiTimes[4], fire: aiTimes[5],
          bfs: aiTimes[6], calls: aiCounts[0],
          searches: aiCounts[1], nodes: aiCounts[2],
          bankPlans: aiCounts[3], bankHits: aiCounts[4],
          targetSelections: aiCounts[5], routeShares: aiCounts[6],
          update: timing.update, build: timing.build,
          frame: timing.frame, maxFrame: timing.maxFrame,
          maxUpdate: timing.maxUpdate, maxBuild: timing.maxBuild,
          input: timing.updateInput, bots: timing.updateBots,
          move: timing.updateMove, projectiles: timing.updateProjectiles,
          world: timing.updateWorld, ui: timing.updateUi,
          prelude: timing.updatePrelude,
          scenery: timing.buildScenery, tanks: timing.buildTanks,
          effects: timing.buildEffects, camera: timing.camera,
          upload: timing.upload, hud: timing.hud,
          commands: timing.commands,
          maxBots: timing.maxUpdateBots, maxMove: timing.maxUpdateMove,
          maxProjectiles: timing.maxUpdateProjectiles,
          maxWorld: timing.maxUpdateWorld, maxUi: timing.maxUpdateUi,
          reuses: G.deadlineSceneReuses};
      },
      slowProfile() {
        return {aiRowWords: aiTimes.length,
          rows: Array.from(slowFrames.subarray(0,
          Math.min(6, slowCount) * SLOW_WORDS)),
          aiRows: Array.from(slowAI.subarray(0,
            Math.min(3, slowCount) * aiTimes.length)),
          movement: Array.from(moveTimes),
          soundEnvelope: Array.from(soundTimes),
          actions: Array.from(actionTimes),
          actionCounts: Array.from(actionCounts),
          aimParts: Array.from(aiTimes.subarray(7, 11))};
      },
      stopInputProfile() {
        // Keep the native displayed-frame measurement running while removing
        // JS phase clocks for a matched, uninstrumented gameplay comparison.
        G.validationInputProfile = false;
        const bridge = globalThis.__tilefinchWebGLDiagnostics;
        if (bridge) bridge.profileDrawPhases = false;
      },
      setAudioCurveMode(mode) { sounds.curveMode=(mode|0)&1; },
      routeInternals: botDebug.routeInternals,
      // Pocket and breach state (tests/fixtures/treadline-breach.js).
      breachState(index) {
        const tank = tanks[index | 0];
        return {active: tank.breachActive, kind: tank.breachKind, index: tank.breachIndex,
          hold: tank.breachHold, angle: tank.breachAngle, breaches: tank.breaches,
          shots: tank.breachShots, standX: tank.breachStandX, standZ: tank.breachStandZ};
      },
      spawnStats: botDebug.spawnStats,
      spawnVerdict: botDebug.spawnVerdict,
      pocketKept(index) { return pocketKept(tanks[index | 0]); },
      wallSight(ax, az, bx, bz, overCover = false) {
        return !lineCrossesWalls(ax, az, bx, bz, overCover);
      },
      circleBlocked: circleHitsObstacle,
      navigationCell,
      // The objective cue: its target, kind and whether Auto shows the arrow.
      objective() {
        const out = {x: 0, z: 0}, kind = objectivePosition(out);
        return {x: out.x, z: out.z, kind, key: G.objectiveKey,
          mission: G.objectiveMission, shown: objectiveArrowShown(kind)};
      },
      /* circleHitsObstacle (its candidate grids) against a brute-force
         scan of every rectangle, over every cell and, for each wall and
         gate face, just inside each radius's reach. */
      collisionGridProbe() {
        let checks = 0, mismatches = 0, candidates = 0, faceChecks = 0;
        const radii = [CLASS_COLLISION_RADIUS[0], CLASS_COLLISION_RADIUS[1],
          CLASS_COLLISION_RADIUS[2], BOSS.collisionRadius, .84, .95];
        const compare = (px, pz, radius) => {
          if (circleHitsObstacle(px, pz, radius) !== bruteCircleHits(px, pz, radius))
            mismatches++;
        };
        for (let z = 0; z < COLLISION_GRID_SIDE; z++) {
          for (let x = 0; x < COLLISION_GRID_SIDE; x++) {
            const px = x * .5 - 8 + .001, pz = z * .5 - 8 + .499;
            let mask = barrierCircleGrid[z * COLLISION_GRID_SIDE + x];
            while (mask) { candidates += mask & 1; mask >>>= 1; }
            for (const radius of radii) { compare(px, pz, radius); checks++; }
          }
        }
        const spatial = globalThis.__treadlineArenaData.spatial, arena = state.arena;
        for (const [bounds, count] of [
          [spatial.obstacleBounds[arena], spatial.obstacleCounts[arena]],
          [spatial.gateBounds[arena], spatial.gateCounts[arena]]]) {
          for (let at = 0; at < count * 4; at += 4) {
            const left = bounds[at], right = bounds[at + 1];
            const top = bounds[at + 2], bottom = bounds[at + 3];
            for (const radius of radii) {
              const reach = radius - .001;
              for (let step = 0; step <= 8; step++) {
                const x = left + (right - left) * step / 8;
                const z = top + (bottom - top) * step / 8;
                compare(x, top - reach, radius); compare(x, bottom + reach, radius);
                compare(left - reach, z, radius); compare(right + reach, z, radius);
                faceChecks += 4;
              }
            }
          }
        }
        return {checks, faceChecks, mismatches, candidates,
          fullCandidates: COLLISION_GRID_CELLS * MAX_BARRIERS,
          bytes: barrierCircleGrid.byteLength + crateCircleGrid.byteLength};
      },
      segmentBlocked(ax, az, bx, bz, left, right, top, bottom, padding) {
        return segmentHitsBox(ax, az, bx, bz, left, right, top, bottom, padding);
      },
      segmentCostProbe(iterations = 64) {
        // Keep the pre-optimization arithmetic as a same-run timing/parity
        // control. This fixture is never entered by normal gameplay.
        function reference(ax, az, bx, bz, left, right, top, bottom, padding) {
          left -= padding; right += padding; top -= padding; bottom += padding;
          if ((ax < left && bx < left) || (ax > right && bx > right)
              || (az < top && bz < top) || (az > bottom && bz > bottom)) return false;
          const dx = bx - ax, dz = bz - az;
          if (Math.abs(dx) < .00001) return ax >= left && ax <= right
            && (Math.abs(dz) >= .00001 || (az >= top && az <= bottom));
          if (Math.abs(dz) < .00001) return az >= top && az <= bottom;
          const cx = left + right - ax - bx, cz = top + bottom - az - bz;
          return Math.abs(dx * cz - dz * cx)
            <= Math.abs(dz) * (right - left) + Math.abs(dx) * (bottom - top);
        }
        const count = Math.max(1, Math.min(128, iterations | 0));
        let referenceMs = 0, fastMs = 0, referenceHits = 0, fastHits = 0;
        let mismatches = 0;
        for (let at = 0; at < count; at++) {
          const x = ((at * 13) & 31) * .5 - 7.75;
          const z = ((at * 7) & 31) * .5 - 7.75;
          const bx = at % 3 === 0 ? x + .000001 : -z;
          const bz = at % 3 === 1 ? z : -x;
          if (reference(x, z, bx, bz, -1, 1, -.2, .2, .08)
              !== segmentHitsBox(x, z, bx, bz, -1, 1, -.2, .2, .08)) mismatches++;
        }
        for (let round = 0; round < 4; round++) {
          for (let phase = 0; phase < 2; phase++) {
            const original = !!((round + phase) & 1);
            const fn = original ? reference : segmentHitsBox;
            let hits = 0;
            const started = performance.now();
            for (let at = 0; at < count; at++) {
              const x = ((at * 13) & 31) * .5 - 7.75;
              const z = ((at * 7) & 31) * .5 - 7.75;
              if (fn(x, z, -x, -z, -1, 1, -.2, .2, .08)) hits++;
            }
            const elapsed = performance.now() - started;
            if (original) { referenceMs += elapsed; referenceHits += hits; }
            else { fastMs += elapsed; fastHits += hits; }
          }
        }
        return {referenceMs, fastMs, referenceHits, fastHits, mismatches};
      },
      orientationIndex,
      setTankVelocity(index, x, z) {
        const tank = tanks[index | 0];
        if (tank) { tank.velocityX = Number(x) || 0; tank.velocityZ = Number(z) || 0; }
      },
      start() { if (resetGame()) setMode("playing"); },
      setPaused(paused) { setMode(paused ? "paused" : "playing"); },
      selectMode(index) {
        state.gameMode = Math.max(0, Math.min(GAME_MODES.length - 1, index | 0));
        refreshSetupLabels();
      },
      selectGadget(index) {
        state.gadgetChoice = Math.max(0, Math.min(GADGETS.length - 1, index | 0));
        refreshSetupLabels();
      },
      selectClass(index) {
        state.classChoice = Math.max(0, Math.min(CLASSES.length - 1, index | 0));
        refreshSetupLabels();
      },
      selectDifficulty(index) {
        state.difficultyChoice = Math.max(0,
          Math.min(DIFFICULTIES.length - 1, index | 0));
        refreshSetupLabels();
      },
      selectCamera(index) {
        preferences.camera = index ? 1 : 0;
        if (!preferences.camera) {
          G.cameraYaw = 0; G.cameraSine = 0; G.cameraCosine = 1;
        }
        refreshSetupLabels();
      },
      selectControls(index) {
        preferences.controls = index === 1 || index === 2 ? index : 0;
        clearSchemeKeys();
        resetAimAssist();
        refreshSetupLabels(); updateControlHint();
      },
      selectAimAssist(index) {
        index = Number(index) | 0;
        preferences.assist = index >= ASSIST_OFF && index <= ASSIST_LOCK
          ? index : ASSIST_SNAP;
        resetAimAssist();
        refreshSetupLabels(); updateControlHint();
      },
      savePreferences,
      reloadPreferences() {
        loadPreferences(); resetAimAssist(); refreshSetupLabels();
        updateControlHint();
      },
      setCommandEnabled(enabled) {
        preferences.command = !!enabled;
        if (!preferences.command) state.commandMeter = 0;
        refreshSetupLabels(); updateHud(true);
      },
      setMusicEnabled(enabled) {
        preferences.music = enabled ? 2 : 0; sounds.setMusic(preferences.music);
        refreshSetupLabels();
      },
      setCommandMeter(value) {
        state.commandMeter = Math.max(0, Math.min(100, Number(value) || 0));
        updateOverlay();
      },
      mapScreenAim(x, y) {
        const command = {aimX: 0, aimZ: 0};
        applyScreenAim(command, Number(x) || 0, Number(y) || 0);
        return {x: command.aimX, z: command.aimZ};
      },
      pollInput() {
        pollPlayerInput();
        const command = playerTank().command;
        return {left: command.left, right: command.right,
          reverse: command.reverse, aimX: command.aimX, aimZ: command.aimZ,
          fire: command.fire, secondary: command.secondary,
          gadget: command.gadget, ultimate: command.ultimate,
          lunge: command.lunge, lockTarget: input.assistLockTarget};
      },
      resetInputProbe() {
        input.keys = Object.create(null);
        input.source = "keyboard";
        input.lastFire = input.lastSecondary = input.lastGadget = false;
        input.lastUltimate = input.lastPause = false;
        input.fireQueued = input.secondaryQueued = input.gadgetQueued = false;
        input.ultimateQueued = input.pauseQueued = false;
        resetAimAssist();
        const command = playerTank().command;
        command.left = command.right = 0; command.reverse = false;
        command.fire = command.secondary = command.gadget = command.ultimate = false;
      },
      cameraProgramsAligned() {
        if (!G.boxInstanceProgram) return true;
        const boxCamera = gl.getUniform(
          G.boxInstanceProgram, G.boxInstanceViewProjectionLocation);
        if (!boxCamera || boxCamera.length !== viewProjection.length)
          return false;
        for (let at = 0; at < viewProjection.length; at++)
          if (Math.abs(boxCamera[at] - viewProjection[at]) > .00001)
            return false;
        return true;
      },
      freezeBots(frozen) { hooks.botsFrozen = !!frozen; },
      setTankActive(index, active) {
        const tank = tanks[index | 0];
        if (tank && !tank.player) tank.active = !!active;
      },
      setTankPosition(index, x, z) {
        const tank = tanks[index | 0];
        if (!tank || !tank.active) return;
        tank.x = Number(x) || 0; tank.z = Number(z) || 0;
        tank.surfaceY = surfaceHeightAt(tank.x, tank.z);
        tank.strategyNext = 0;
      },
      setTankHeading(index, yaw) {
        const tank = tanks[index | 0];
        if (!tank || !tank.active || !Number.isFinite(Number(yaw))) return;
        tank.yaw = tank.turret = wrapAngle(Number(yaw));
        updateTankYawCache(tank);
        updateTankTurretCache(tank);
      },
      armBotShot(index, secondary = false) {
        const tank = tanks[index | 0];
        const player = tanks[0];
        if (!tank || tank.player || !tank.active || !player.active) return false;
        tank.target = player.id;
        tank.aiThink = 999;
        const angle = Math.atan2(player.x - tank.x, player.z - tank.z);
        tank.yaw = tank.turret = angle;
        updateTankYawCache(tank);
        updateTankTurretCache(tank);
        return beginBotFireWindup(tank, !!secondary);
      },
      destroyTank(index, attacker = 0) {
        const tank = tanks[index | 0];
        if (!tank || tank.player || !tank.active) return false;
        tank.spawnGrace = 0;
        tank.health = 1;
        return damageTank(tank, 1000, attacker | 0,
          tank.x, tank.z - 1, true);
      },
      completeArenaTransition() {
        const before = state.arena;
        const writesBefore = G.authoredHudWrites;
        for (let at = 1; at < MAX_TANKS; at++) tanks[at].active = false;
        state.mode = "arena-clear";
        state.transition = 0;
        updateHud(true);
        update(1 / 30);
        return {transitioned: state.mode === "playing" && state.arena !== before,
          authoredWrites: G.authoredHudWrites - writesBefore};
      },
      tankInstanceTint(index, part = 3) {
        index = Math.max(0, Math.min(MAX_TANKS - 1, index | 0));
        part = Math.max(0, Math.min(RETAINED_TANK_PARTS - 1, part | 0));
        const owner = index * RETAINED_TANK_PARTS + part;
        const slot = retainedTankPartSlots[owner];
        if (slot < 0 || slot >= G.boxInstanceCount
            || boxInstanceSlotOwners[slot] !== owner) return null;
        return Array.from(boxInstanceTints.subarray(slot * 4, slot * 4 + 4));
      },
      tankState(index) {
        const tank = tanks[index | 0];
        if (!tank) return null;
        return {active: tank.active, x: tank.x, z: tank.z, team: tank.team,
          role: tank.role, health: tank.health, maxHealth: tank.maxHealth,
          classId: tank.classId, target: tank.target,
          blockedTime: tank.blockedTime, avoidTime: tank.avoidTime,
          hitFlash: tank.hitFlash, fireWindup: tank.fireWindup,
          fireTelegraphed: tank.fireTelegraphed,
          backingOff: tank.backingOff, standoff: tank.standoff,
          orbitTurn: tank.orbitTurn,
          reversing: tank.command.reverse, yaw: tank.yaw, turret: tank.turret,
          lunge: tank.lunge, lungeCooldown: tank.lungeCooldown, drift: tank.drift,
          slideX: tank.slideX, slideZ: tank.slideZ,
          placementValid: !tank.active || (Math.abs(tank.x) <= HULL_EDGE
            && Math.abs(tank.z) <= HULL_EDGE
            && !circleHitsObstacle(tank.x, tank.z, tank.collisionRadius))};
      },
      tankPairClearance() {
        let minimum = 99;
        for (let left = 0; left < MAX_TANKS; left++) {
          const first = tanks[left];
          if (!first.active) continue;
          for (let right = left + 1; right < MAX_TANKS; right++) {
            const second = tanks[right];
            if (!second.active) continue;
            const dx = first.x - second.x, dz = first.z - second.z;
            const distance = Math.sqrt(dx * dx + dz * dz);
            const contact = Math.sqrt(CLASS_COMBINED_RADIUS_SQUARED[
              first.classId * CLASSES.length + second.classId]);
            minimum = Math.min(minimum, distance - contact);
          }
        }
        return minimum;
      },
      fireSecondary(index = 0) {
        const tank = tanks[index | 0];
        tank.secondaryCooldown = 0;
        const fired = fireSecondary(tank); render(1 / 60);
        return fired;
      },
      activateCommand() {
        activateCommand(tanks[0]); render(1 / 60);
      },
      damagePlayerFrom(zone, damage = 40, bypassShield = false) {
        const player = tanks[0];
        player.spawnGrace = 0;
        let angle = player.yaw;
        if (zone === "SIDE") angle += Math.PI * .5;
        else if (zone === "REAR") angle += Math.PI;
        damageTank(player, Number(damage) || 0, 1,
          player.x + Math.sin(angle) * 2,
          player.z + Math.cos(angle) * 2, !!bypassShield);
        render(1 / 60);
      },
      activateGadget(name) {
        if (!GADGETS.includes(name)) return;
        const player = tanks[0];
        player.gadget = name; player.gadgetCooldown = 0;
        useGadget(player); render(1 / 60);
      },
      clearGadgetEffects() {
        const player = tanks[0];
        player.shield = player.repair = player.boost = player.gadgetCooldown = 0;
        for (const mine of mines) mine.active = false;
        for (const smoke of smokeClouds) smoke.active = false;
        G.activeSmokeCount = 0;
      },
      probeRicochet() {
        for (const bullet of bullets) bullet.active = false;
        G.bulletActiveMask = 0;
        state.ricochets = 0;
        const bullet = bullets[0];
        setBulletActive(0, true); bullet.owner = 0;
        bullet.x = 7.82; bullet.z = 6.5; bullet.vx = 6; bullet.vz = 0;
        bullet.speed = 6; bullet.pierceId = -1;
        bullet.headingSine = 1; bullet.headingCosine = 0;
        bullet.life = 3; bullet.bounces = 1;
        updateBullets(1 / 60); render(1 / 60);
      },
      probeSecondImpact() {
        const bullet = bullets[0];
        setBulletActive(0, true); bullet.owner = 0;
        bullet.x = -7.82; bullet.z = 6.5; bullet.vx = -6; bullet.vz = 0;
        bullet.speed = 6; bullet.pierceId = -1;
        bullet.headingSine = -1; bullet.headingCosine = 0;
        bullet.life = 3; bullet.bounces = 0;
        updateBullets(1 / 60); render(1 / 60);
      },
      probeBarrier() {
        const barrier = barriers.find((candidate) => candidate.active);
        if (!barrier) return;
        for (let hit = 0; hit < 2; hit++) {
          const bullet = bullets[0];
          setBulletActive(0, true); bullet.owner = 0;
          bullet.x = barrier.x; bullet.z = barrier.z;
          bullet.vx = 0; bullet.vz = 0; bullet.life = 1; bullet.bounces = 1;
          bullet.speed = 0; bullet.pierceId = -1; bullet.pierce = 0;
          bullet.overCover = false;
          bullet.headingSine = 0; bullet.headingCosine = 1;
          updateBullets(1 / 60);
        }
        render(1 / 60);
      },
      step(frames = 1) {
        for (let at = 0; at < frames; at++) {
          update(1 / 60); render(1 / 60);
        }
      },
      stepBudgetedHud(frames = 1) {
        for (let at = 0; at < frames; at++)
          render(1 / 60, false, true, performance.now());
      },
      stepSimulation(frames = 1, dt = 1 / 60) {
        // Device frames are 1/30 s at most (advanceElapsed); never larger.
        dt = Math.min(1 / 30, Math.max(1 / 240, Number(dt) || 1 / 60));
        for (let at = 0; at < frames; at++) {
          update(dt);
          state.frames++;
        }
      },
      stepCamera(frames = 1) {
        for (let at = 0; at < frames; at++) {
          update(1 / 60);
          updateCamera(1 / 60);
          state.frames++;
        }
      },
      /* Invariant-sweep camera track: compose the camera for a step the
         caller already simulated, as a device frame does after its update,
         without drawing. Shake is left out of the view (it is reported
         separately) so motion checks see the camera path itself. Fills out
         with distance, safe distance, yaw, camera mode, visible shake and
         the 16 view-matrix floats. */
      presentCamera(dt = 1 / 30, out = null) {
        const shake = preferences.shake;
        preferences.shake = false;
        updateCamera(Math.min(1 / 30, Math.max(1 / 240, Number(dt) || 1 / 60)));
        preferences.shake = shake;
        if (!out) return;
        out[0] = G.cameraDistance; out[1] = G.cameraSafeDistance; out[2] = G.cameraYaw;
        out[3] = preferences.camera; out[4] = shake ? state.shake : 0;
        for (let at = 0; at < 16; at++) out[5 + at] = view[at];
      },
      delayedFrame(elapsedSeconds) {
        const before = state.time;
        advanceElapsed(elapsedSeconds);
        return state.time - before;
      },
      setKeyboard(action, pressed) {
        if (Object.prototype.hasOwnProperty.call(input.keys, action)
            || ["a", "d", "w", "s", "fire", "secondary", "gadget", "ultimate", "aimLeft",
              "aimRight", "aimUp", "aimDown", "pivotLeft", "pivotRight"].includes(action)) {
          input.keys[action] = !!pressed;
          setSource("keyboard");
        }
      },
      longSoakCommand(frameNumber) {
        const command = {left: 0, right: 0, reverse: false};
        setLongSoakMovement(command, Math.max(0, Number(frameNumber) | 0));
        return command;
      },
      saturateVisualLoad() {
        const positions = [-6, -5, -3.8, -4.8, 3.8, -4.8,
          -4.8, 4.6, 4.8, 4.6, 0, 5.8];
        for (let at = 0; at < MAX_TANKS; at++) {
          const tank = tanks[at];
          placeTank(tank, positions[at * 2], positions[at * 2 + 1],
            at * .55, at === 0 ? 0 : 1, tank.gadget,
            at === 0 ? "PLAYER" : "HUNTER", at % CLASSES.length);
          tank.spawnGrace = 0;
          tank.recoil = .1; tank.shield = 1; tank.repair = 1;
        }
        G.bulletActiveMask = 0;
        for (let at = 0; at < MAX_BULLETS; at++) {
          const bullet = bullets[at];
          setBulletActive(at, true);
          bullet.owner = 0; bullet.x = -7 + (at % 9) * 1.6;
          bullet.z = -6.8 + ((at / 9) | 0) * .35;
          bullet.vx = 0; bullet.vz = 1; bullet.life = 3;
          bullet.speed = 1; bullet.pierceId = -1;
          bullet.headingSine = 0; bullet.headingCosine = 1;
        }
        for (let at = 0; at < MAX_DECALS + 5; at++)
          spawnDecal(at & 1 ? 1 : 2, -5 + (at % 6) * 1.7,
            5.3 + ((at / 6) | 0) * .25, at * .19);
        for (let at = 0; at < 12; at++) {
          const particle = particles[at];
          setParticleActive(at, true);
          particle.x = -4 + at * .6; particle.y = .3; particle.z = 6;
          particle.vx = 1 + at * .1; particle.vy = .2; particle.vz = .5;
          particle.life = particle.maximum = 1;
        }
        render(1 / 60);
      },
      arenaStaticIndexCounts() {
        const counts = [];
        for (let at = 0; at < ARENAS.length; at++) {
          const cached = arenaGeometryCache[at];
          counts.push(G.arenaCommonIndexCount + (cached ? cached.indexCount : 0));
        }
        return counts;
      },
      arenaValidationMask(index, mode = 0) {
        index = Math.max(0, Math.min(ARENAS.length - 1, index | 0));
        const result = arenaGenerator.validate(index, mode | 0);
        return (result.connected ? 1 : 0) | (result.flanking ? 2 : 0)
          | (result.sightlines ? 4 : 0) | (result.cover ? 8 : 0)
          | (result.density ? 16 : 0) | (result.symmetry ? 32 : 0);
      },
      arenaValidationMetrics(index, mode = 0) {
        index = Math.max(0, Math.min(ARENAS.length - 1, index | 0));
        return arenaGenerator.validate(index, mode | 0);
      },
      generateArenaSeed(seed) {
        const gameplayBefore = G.randomState;
        const accepted = generateArena(Number(seed) >>> 0);
        return {accepted, checksum: G.arenaGenerationChecksum,
          attempts: G.arenaGenerationAttempts, fallback: G.arenaGenerationFallback,
          gameplayUnchanged: gameplayBefore === G.randomState,
          storageStable: arenaGenerator.storageStable()};
      },
      beginArenaGeneration(seed, attemptLimit = 20) {
        return arenaGenerator.beginGeneration(Number(seed) >>> 0,
          Number(attemptLimit) | 0);
      },
      stepArenaGeneration(maxAttempts = 2) {
        return arenaGenerator.stepGeneration(Number(maxAttempts) | 0);
      },
      generatedArenaProbe(seedCount = 100) {
        const gameplayBefore = G.randomState;
        let accepted = 0, fallbacks = 0, maximumAttempts = 0;
        seedCount = Math.max(1, Math.min(100, seedCount | 0));
        for (let seed = 1; seed <= seedCount; seed++) {
          if (generateArena(Math.imul(seed, 0x45d9f3b) >>> 0)) accepted++;
          else fallbacks++;
          maximumAttempts = Math.max(maximumAttempts, G.arenaGenerationAttempts);
        }
        return {accepted, fallbacks, maximumAttempts,
          obstacleCount: generatedArena.obstacleCount,
          barrierCount: generatedArena.barrierCount,
          rampCount: generatedArena.rampCount,
          checksum: G.arenaGenerationChecksum,
          gameplayUnchanged: gameplayBefore === G.randomState,
          storageStable: arenaGenerator.storageStable()};
      },
      onlineGeneratedSnapshot(seed) {
        const accepted = generateArena(Number(seed) >>> 0);
        beginArena(GENERATED_ARENA_INDEX); sendOnlineSnapshot();
        return {accepted, seed: state.arenaSeed,
          checksum: G.arenaGenerationChecksum, mask: activeBarrierMask()};
      },
      emitOnlineSnapshot() { sendOnlineSnapshot(); },
      applyOnlinePacket(buffer) { return applyOnlineSnapshot(buffer); },
      /* Aim guide test surface: the level in force, a fresh update of the
         guide (and its Full enemy test) exactly as a drawn frame runs it, and
         the tracer slots. Never read per frame. */
      aimGuideLevel() { return aimGuideLevel(); },
      setAimGuide(level) {
        preferences.aimGuide = Math.max(0, Math.min(2, level | 0));
        refreshSetupLabels();
      },
      aimGuide(level = aimGuideLevel()) {
        const player = playerTank(), recast = updateAimGuide(player, level);
        if (level === AIM_FULL) updateAimGuideEnemy(player);
        else { aimGuide.enemy = -1; aimGuide.enemyLeg = 0; aimGuide.hidden = false; }
        return {...aimGuide, recast, level};
      },
      aimScore() {
        return {level: G.runAimLevel, multiplier: AIM_SCORE_MULTIPLIERS[G.runAimLevel],
          final: finalScore(), raw: state.score};
      },
      tracerState() {
        const out = [];
        for (let slot = 0; slot < TRACER_SLOTS; slot++) {
          const bulletAt = tracerBullet[slot];
          if (bulletAt < 0) continue;
          const first = slot * TRACER_VERTICES * 2;
          out.push({slot, bullet: bulletAt,
            live: bulletTracer[bulletAt] === slot && bullets[bulletAt].active,
            vertices: Array.from(tracerVertices.subarray(first,
              first + tracerVertexCount[slot] * 2)),
            head: [tracerHead[slot * 2], tracerHead[slot * 2 + 1]]});
        }
        return {slots: out, boxes: G.tracerBoxes};
      },
      /* Place barrier slot index as an unbreakable wall [left, right] x
         [top, bottom] and rebuild everything placement derives from it (the
         collision grids, the ray index, the navigation grids). */
      setBarrierRect(index, left, right, top, bottom) {
        const barrier = barriers[index | 0];
        if (!barrier) return false;
        barrier.present = barrier.active = true; barrier.health = 1e9;
        barrier.left = left; barrier.right = right; barrier.top = top; barrier.bottom = bottom;
        barrier.x = (left + right) * .5; barrier.z = (top + bottom) * .5;
        barrier.width = right - left; barrier.depth = bottom - top;
        fillBarrierCircleGrid(); fillTinyCircleOccupied(); fillRayGeometry();
        prepareNavigationGrids(); invalidateBotNavigation();
        G.retainedSceneryDirty = true;
        return true;
      },
      setBarrierActive(index, active) {
        const barrier = barriers[index | 0];
        if (barrier && barrier.present) barrier.active = !!active;
        updateRayActiveMask();
      },
      /* Real shell casts across barrier 0, set across the origin, through
         the ray index and through the record walk: a shell fired from the
         ground stops on it, one fired from a ramp (overCover) flies over.
         Crates and the gate stand aside meanwhile; all of it is restored. */
      elevationBarrierProbe() {
        const barrier = barriers[0], saved = {...barrier};
        const crateActive = crates.map((crate) => crate.active);
        const gateOpen = state.gateOpen;
        for (const crate of crates) crate.active = false;
        state.gateOpen = true;
        Object.assign(barrier, {present: true, active: true, x: 0, z: 0,
          width: 2, depth: .4, left: -1, right: 1, top: -.2, bottom: .2});
        const casts = [];
        for (const indexed of [true, false])
          for (const overCover of [false, true])
            casts.push(G.castShellOnce(0, -.9, 0, 1, 1.8, overCover, indexed));
        Object.assign(barrier, saved);
        crates.forEach((crate, at) => { crate.active = crateActive[at]; });
        state.gateOpen = gateOpen;
        fillRayGeometry();
        // Ground: barrier 0 (ray id 20) .6 along; elevated: the full 1.8.
        return casts.every((cast, at) => cast.indexed === at < 2 && (at & 1
          ? cast.id !== 20 && cast.length === 1.8
          : cast.id === 20 && Math.abs(cast.length - .6) < 1e-6));
      },
      snapshot() {
        const player = tanks[0];
        let bulletCount = 0, particleCount = 0, pickupCount = 0;
        let piercingBullets = 0, bypassBullets = 0, maxBulletDamage = 0;
        let mineCount = 0, smokeCount = 0, barrierCount = 0, decalCount = 0;
        let blueTanks = 0, redTanks = 0;
        for (const bullet of bullets) if (bullet.active) {
          bulletCount++;
          if (bullet.pierce > 0) piercingBullets++;
          if (bullet.bypassShield) bypassBullets++;
          maxBulletDamage = Math.max(maxBulletDamage, bullet.damage);
        }
        for (const particle of particles) if (particle.active) particleCount++;
        for (const pickup of pickups) if (pickup.active) pickupCount++;
        for (const mine of mines) if (mine.active) mineCount++;
        for (const smoke of smokeClouds) if (smoke.active) smokeCount++;
        for (const decal of decals) if (decal.active) decalCount++;
        for (const barrier of barriers) if (barrier.active) barrierCount++;
        for (const tank of tanks) if (tank.active) {
          if (tank.team === 0) blueTanks++; else redTanks++;
        }
        return {
          mode: state.mode, gameMode: state.gameMode, arena: state.arena,
          hudVisible: hudModeVisible(state.mode), hudToast: G.hudToast,
          score: state.score, wave: state.wave, multiplier: state.multiplier,
          bestWave: state.bestWave, bestScore: state.bestScore,
          dailyDay: state.dailyDay, dailyBestDay: state.dailyBestDay,
          dailyBest: state.dailyBest, bankKills: state.bankKills, doubleBanks: state.doubleBanks,
          medals: state.medals, mainShots: state.mainShots,
          palette: preferences.palette, paint: preferences.paint, reverseThreshold: REVERSE_DEGREES[preferences.reverse],
        boss: tanks[MAX_TANKS - 1].active && tanks[MAX_TANKS - 1].boss,
          bossLeftTread: tanks[MAX_TANKS - 1].leftTreadHealth,
          bossRightTread: tanks[MAX_TANKS - 1].rightTreadHealth,
          bossTurret: tanks[MAX_TANKS - 1].turretHealth,
          duelTurn: state.duelTurn, duelTime: state.duelTime,
          duelResolving: state.duelResolving, duelWinner: state.duelWinner,
          hazards: hazards.reduce((count, hazard) => count + (hazard.active ? 1 : 0), 0),
          crates: crates.reduce((count, crate) => count + (crate.active ? 1 : 0), 0),
          killcamFrames: G.killcamCount, killcamElapsed: G.killcamElapsed,
          simTime: state.time, wallTime: state.wallTime,
          audioRole: sounds.engineRole,
          audioCurves: sounds.curves.size,
          killBeat: state.killBeat, pendingClear: state.pendingClear,
          botTelegraphs: stats.botTelegraphs,
          botPlayerShots: stats.botPlayerShots,
          botTelegraphViolations: stats.botTelegraphViolations,
          botVolleyMinimum: stats.botVolleyMinimum,
          lives: state.lives, enemies: activeEnemyCount(), frames: state.frames,
          playerX: player.x, playerZ: player.z, playerYaw: player.yaw,
          turret: player.turret, armor: player.health, maxArmor: player.maxHealth,
          classId: player.classId, className: CLASSES[player.classId].name,
          armorZone: state.armorZone, gadget: player.gadget,
          gadgetCooldown: player.gadgetCooldown, shield: player.shield,
          secondaryCooldown: player.secondaryCooldown,
          commandEnabled: preferences.command, commandMeter: state.commandMeter,
          commandBuff: player.commandBuff,
          repair: player.repair, boost: player.boost,
          bullets: bulletCount, piercingBullets, bypassBullets, maxBulletDamage,
          particles: particleCount, pickups: pickupCount,
          decals: decalCount, decalRecycles: G.decalRecycles,
          mines: mineCount, smoke: smokeCount, barriers: barrierCount,
          barriersBroken: state.barriersBroken, ricochets: state.ricochets,
          blueTanks, redTanks, blueControl: state.blueControl,
          redControl: state.redControl, gateOpen: state.gateOpen,
          convoyActive: convoy.active, convoyProgress: convoy.progress,
          convoyHealth: convoy.health,
          difficulty: state.difficultyChoice, shots: state.shots, hits: state.hits,
          damageTaken: state.damageTaken, objectiveSeconds: state.objectiveTicks,
          controls: preferences.controls, assist: preferences.assist,
          cameraMode: preferences.camera, cameraYaw: G.cameraYaw,
          cameraDistance: G.cameraDistance, cameraSafeDistance: G.cameraSafeDistance, cameraSafetyUpdates: G.cameraSafetyUpdates,
          vertices: G.vertexCount, indices: G.indexCount, staticIndices: G.staticIndexCount,
          arenaPlaneMaxSpan: G.arenaPlaneMaxSpan,
          boxInstances: G.boxInstanceCount, tankBarrels: G.tankBarrelCount,
          renderedBulletInstances: G.renderedBulletInstances, aimBoxes: aimGuide.boxes, aimDrops: aimGuide.drops,
          tracerBoxes: G.tracerBoxes,
          instanceLimit: MAX_BOX_INSTANCES, frameInstanceCeiling: G.frameInstanceCeiling, instanceCapHitFrames: G.instanceCapHitFrames,
          droppedDecalInstances: G.droppedDecalInstances, droppedParticleInstances: G.droppedParticleInstances,
          arenaMaximumStaticIndexCount: G.arenaMaximumStaticIndexCount,
          generatedStaticTailIndexLimit: GENERATED_STATIC_TAIL_INDEX_LIMIT,
          arenaSeed: state.arenaSeed, arenaGenerationAttempts: G.arenaGenerationAttempts,
          arenaGenerationFallback: G.arenaGenerationFallback, arenaGenerationChecksum: G.arenaGenerationChecksum,
          arenaGenerationPhase: G.arenaGenerationPhase,
          arenaGenerationAge: state.wallTime - G.arenaGenerationStartedAt,
          onlineArenaGeometryPending: G.onlineArenaGeometryPending,
          hudCharacters: G.hudCharacterCount,
          hudPrimitives: (G.hudPublishedIndexCount
            + G.hudPublishedIndicatorIndexCount) / 6,
          hudTextPrimitives: G.hudPublishedIndexCount / 6,
          hudIndicatorPrimitives: G.hudPublishedIndicatorIndexCount / 6,
          objectiveIndicatorVisible: G.hudObjectiveVisible,
          objectiveIndicatorX: 160 + Math.sin(G.hudObjectiveAngle) * HUD_OBJECTIVE_RADIUS_X,
          objectiveIndicatorY: 90 - Math.cos(G.hudObjectiveAngle) * HUD_OBJECTIVE_RADIUS_Y,
          hudTextUploads: G.hudTextUploads, hudIndicatorUploads: G.hudIndicatorUploads, repeatedFrameCaptures: G.repeatedFrameCaptures,
          meshDrops: G.meshDrops, deadlineSceneReuses: G.deadlineSceneReuses, deadlineHudDeferrals: G.deadlineHudDeferrals,
          inputSource: input.source, gamepadConnected: input.gamepadConnected,
          onlineActive: online.active, onlineRole: online.role,
          onlineInputSequence: online.receivedInputSequence,
          onlineInputValid: online.receivedInputValid,
          onlineSnapshotSequence: online.snapshotSequence,
          onlineSnapshotValid: online.snapshotValid,
          remotePlayerX: tanks[1].x, remotePlayerZ: tanks[1].z,
        };
      },
      stop() { state.running = false; },
    });
    Object.defineProperty(globalThis, "__treadlineDebug", {
      value: debug, configurable: false, writable: false,
    });
  });
})();
