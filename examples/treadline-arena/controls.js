/* Treadline Arena: control schemes and tank movement extras.

   Loaded before game.js, which attaches its input state once at boot and
   calls four bounded hooks: poll() once per simulation step (player input),
   slide(...) once per tank per step (hull velocity), dodge(...) after a human
   cannon shot, and hint() when the scheme or pad changes. Nothing here
   allocates or draws per frame.

   Schemes (preferences.controls): 0 Arcade, 1 Classic, 2 Gunner.
   Extras, all schemes: D-pad Left/Right (keyboard Z/C) pivot in place;
   a double tap of the drive input lunges (nub or move keys in Arcade and
   Gunner, L+R together or W in Classic); a fast hull that releases or
   reverses one side drifts with reduced grip, longer on ice.

   Determinism: only the resulting command (signed treads, aim, the lunge bit)
   reaches the simulation, and replays/online packets carry exactly that. */
(() => {
  "use strict";
  const ARCADE = 0, CLASSIC = 1, GUNNER = 2;
  // game.js's aim-assist settings, bound at attach.
  let ASSIST_OFF = 0, ASSIST_LOCK = 0;
  const DEAD_ZONE = .22, ROLL_FRAMES = 3, CONE = .75, SNAP_BLEND = .6;
  const DIAGONAL = .7071067811865476;
  const REVERSE_COSINES = [0, -.2588190451, -.5];
  /* Gunner traverse: a tap moves ~1 degree, a held button eases in to just
     under the 3.8 rad/s turret slew so the barrel never lags its target. */
  const TRAVERSE_MIN = .6, TRAVERSE_MAX = 3.6, TRAVERSE_RAMP = .5;
  /* Lunge: double tap = second press within TAP_GAP of releasing a first
     press no longer than TAP_HOLD. Adds 1.6x class speed along the hull,
     decaying with LUNGE_TAU: LUNGE_TAU*1.6*speed of extra travel, 1.0 unit
     for a Striker (Scout 1.23, Bulwark .75), with a 2.5 s cooldown. */
  const TAP_HOLD = .3, TAP_GAP = .25;
  const LUNGE_SCALE = 2.6, LUNGE_TAU = .245, LUNGE_COOLDOWN = 2.5;
  /* Drift: hull speed above 1.2x its class speed (boost, lunge, Scout
     command) plus a tread split of at least .9 drops grip for .4 s (ice
     .9 s). Grip K/(remaining + .05) rises as the timer runs out, so traction
     returns smoothly instead of snapping. */
  const DRIFT_SPEED = 1.2, DRIFT_TURN = .9;
  const DRIFT_TIME = .4, ICE_DRIFT_TIME = .9, DRIFT_K = 1.1;
  /* Veteran/Ace bots lunge clear of a human shell aimed at their flank. */
  const DODGE_CHANCE = [0, .2, .45];
  /* One-line hints for the Quick Match panel point at the Controls menu;
     BINDINGS is that menu's readable list, one action per row, as
     [driving page, shooting page] per scheme for [pad, keyboard]. Face
     buttons are words: the menu fonts lack the triangle and cross marks. */
  const HINTS = [
    ["Arcade: nub moves | face buttons aim | R fires. All buttons: Controls.",
      "Arcade: WASD moves | IJKL aims | Space fires. All keys: Controls."],
    ["Classic: L/R treads | nub aims | Cross fires. All buttons: Controls.",
      "Classic: A/D treads | arrows aim | Space fires. All keys: Controls."],
    ["Gunner: nub drives | Square/Circle turn turret | R fires. All buttons: Controls.",
      "Gunner: WASD moves | J/L turn turret | Space fires. All keys: Controls."],
  ];
  const PIVOT = "D-pad \u25c4 \u25ba|pivot in place", PAUSE = "START|pause";
  const UP = "D-pad \u2191", DOWN = "D-pad \u2193";
  const KEY_PIVOT = "Z / C|pivot in place", KEY_PAUSE = "P or Esc|pause";
  const BINDINGS = [
    [[["Nub|drive that way", "Nub pulled back|reverse", "Nub double-tap|lunge",
      PIVOT, PAUSE],
    ["Face buttons|aim (two at once: diagonal)", "R|fire (hold to charge)",
      "L|secondary", UP + "|gadget", DOWN + "|Command"]],
    [["WASD or arrows|drive that way", "Move key double-tap|lunge", KEY_PIVOT,
      KEY_PAUSE],
    ["IJKL or numpad|aim", "Space|fire (hold to charge)", "Shift or E|secondary",
      "F|gadget", "Q|Command"]]],
    [[["L / R|left / right tread", "L + R|both treads forward", "Circle|reverse",
      "L + R twice|lunge", PIVOT, PAUSE],
    ["Nub|aim", "Cross|fire (hold to charge)", "Square|secondary",
      UP + "|gadget", "Triangle|Command"]],
    [["A / D|left / right tread", "W / S|both forward / reverse", "W twice|lunge",
      KEY_PIVOT, KEY_PAUSE],
    ["Arrows, IJKL, pointer|aim", "Space|fire (hold to charge)", "E|secondary",
      "F|gadget", "Q|Command"]]],
    [[["Nub|drive that way", "Nub pulled back|reverse", "Nub double-tap|lunge",
      PIVOT, PAUSE],
    ["Square / Circle|turn turret (hold: faster)", "Triangle|center turret on hull",
      "R|fire (hold to charge)", "L|secondary", "Cross or " + UP + "|gadget",
      DOWN + "|Command"]],
    [["WASD or arrows|drive that way", "Move key double-tap|lunge", KEY_PIVOT,
      KEY_PAUSE],
    ["J / L|turn turret (hold: faster)", "I|center turret on hull",
      "Space|fire (hold to charge)", "Shift|secondary", "F|gadget", "Q|Command"]]],
  ];
  const keyboardLayout = () =>
    !B.connectedGamepad() && navigator.platform !== "PSP";
  let B = null, input = null, prefs = null;
  const scratch = {aimX: 0, aimZ: 0};
  // Gesture and traverse state; reset() clears it with the aim assist.
  let tapState = 0, tapTime = 0, tapX = 0, tapY = 0, lastPoll = -1;
  let gunAngle = NaN, gunHeld = 0, gunTracking = false, lastSnap = false;
  let gunLocked = false;

  function reset() {
    tapState = 0; gunAngle = NaN; gunHeld = 0; gunTracking = false;
    lastSnap = false; lastPoll = -1;
  }

  function hint() {
    const ui = B.ui, pad = B.connectedGamepad();
    input.gamepadConnected = !!pad;
    if (ui.controls) ui.controls.textContent =
      HINTS[prefs.controls][pad || navigator.platform === "PSP" ? 0 : 1];
  }

  /* Double-tap detector over one drive signal. x/y is the tap direction
     (unit for the nub, fixed for Classic) so a wiggle through neutral to the
     opposite side is not mistaken for a second tap. */
  function tap(active, x, y, now) {
    if (active) {
      if (tapState === 2 && now - tapTime <= TAP_GAP
          && x * tapX + y * tapY > .7) { tapState = 3; return true; }
      if (tapState === 0 || tapState === 2) {
        tapState = 1; tapTime = now; tapX = x; tapY = y;
      } else if (tapState === 1 && now - tapTime > TAP_HOLD) tapState = 3;
    } else if (tapState === 1) { tapState = 2; tapTime = now; }
    else if (tapState === 3 || (tapState === 2 && now - tapTime > TAP_GAP))
      tapState = 0;
    return false;
  }

  function arcadeMove(command, moveX, moveY) {
    const magnitudeSquared = moveX * moveX + moveY * moveY;
    if (!(magnitudeSquared > DEAD_ZONE * DEAD_ZONE)) return false;
    const magnitude = Math.sqrt(magnitudeSquared);
    // Screen-to-world is the same rotation as aiming.
    B.applyScreenAim(scratch, moveX / magnitude, moveY / magnitude);
    const desiredX = scratch.aimX, desiredZ = scratch.aimZ;
    const tank = B.playerTank();
    /* Dot/cross instead of atan2 (software on Allegrex); same reverse cone
       and a smooth normalized steering approximation. */
    let turn = desiredX * tank.yawCosine - desiredZ * tank.yawSine;
    let alignment = desiredX * tank.yawSine + desiredZ * tank.yawCosine;
    command.reverse = alignment < REVERSE_COSINES[prefs.reverse];
    if (command.reverse) { turn = -turn; alignment = -alignment; }
    let steering = Math.max(-1, Math.min(1,
      turn / Math.max(.5, 1 + alignment * .5)));
    // moveTank applies the reverse sign before its tread differential.
    if (command.reverse) steering = -steering;
    if (steering >= 0) { command.left = 1 - steering; command.right = 1; }
    else { command.left = 1; command.right = 1 + steering; }
    return true;
  }

  function enemyInCone(player, aimX, aimZ) {
    const tanks = B.tanks;
    let chosen = -1, nearest = Infinity;
    for (let at = 0; at < tanks.length; at++) {
      const enemy = tanks[at];
      if (!enemy.active || enemy.id === player.id || enemy.team === player.team)
        continue;
      const dx = enemy.x - player.x, dz = enemy.z - player.z;
      const distanceSquared = dx * dx + dz * dz;
      if (!(distanceSquared > .0001)) continue;
      const alignment = aimX * dx + aimZ * dz;
      if (!(alignment > 0) || alignment * alignment < distanceSquared * CONE)
        continue;
      if (distanceSquared < nearest) { nearest = distanceSquared; chosen = at; }
    }
    return chosen;
  }

  function assist(command, active) {
    if (!active) {
      input.assistLockTarget = -1;
      input.assistLockBroken = false;
      return;
    }
    let length = Math.sqrt(command.aimX * command.aimX
      + command.aimZ * command.aimZ);
    if (!(length > .0001)) return;
    command.aimX /= length; command.aimZ /= length;
    if (prefs.assist === ASSIST_OFF) return;
    const player = B.playerTank(), tanks = B.tanks, smoke = B.lineCrossesSmoke;
    let target = -1;
    if (prefs.assist === ASSIST_LOCK) {
      if (input.assistLockTarget >= 0) {
        const locked = tanks[input.assistLockTarget];
        if (!locked || !locked.active || locked.team === player.team
            || smoke(player.x, player.z, locked.x, locked.z)) {
          input.assistLockTarget = -1;
          input.assistLockBroken = true;
        }
      }
      if (input.assistLockTarget < 0 && !input.assistLockBroken)
        input.assistLockTarget = enemyInCone(player, command.aimX, command.aimZ);
      if (input.assistLockTarget >= 0) {
        const selected = tanks[input.assistLockTarget];
        if (smoke(player.x, player.z, selected.x, selected.z)) {
          input.assistLockTarget = -1;
          input.assistLockBroken = true;
        }
      }
      target = input.assistLockTarget;
    } else target = enemyInCone(player, command.aimX, command.aimZ);
    if (target < 0) return;
    const enemy = tanks[target];
    const dx = enemy.x - player.x, dz = enemy.z - player.z;
    const inverse = 1 / Math.sqrt(dx * dx + dz * dz);
    if (prefs.assist === ASSIST_LOCK) {
      command.aimX = dx * inverse; command.aimZ = dz * inverse;
      return;
    }
    command.aimX = command.aimX * (1 - SNAP_BLEND) + dx * inverse * SNAP_BLEND;
    command.aimZ = command.aimZ * (1 - SNAP_BLEND) + dz * inverse * SNAP_BLEND;
    length = Math.sqrt(command.aimX * command.aimX
      + command.aimZ * command.aimZ) || 1;
    command.aimX /= length; command.aimZ /= length;
  }

  function faceAim(command, buttons) {
    const away = !!buttons[3]?.pressed, toward = !!buttons[0]?.pressed;
    const left = !!buttons[2]?.pressed, right = !!buttons[1]?.pressed;
    if (!(away || toward || left || right)) {
      input.aimAwayAge = input.aimTowardAge = 0;
      input.aimLeftAge = input.aimRightAge = 0;
      return false;
    }
    // A released diagonal partner lingers a few frames so 8-way rolls work.
    input.aimAwayAge = away ? ROLL_FRAMES : Math.max(0, input.aimAwayAge - 1);
    input.aimTowardAge = toward ? ROLL_FRAMES : Math.max(0, input.aimTowardAge - 1);
    input.aimLeftAge = left ? ROLL_FRAMES : Math.max(0, input.aimLeftAge - 1);
    input.aimRightAge = right ? ROLL_FRAMES : Math.max(0, input.aimRightAge - 1);
    return screenAim(command,
      (input.aimRightAge ? 1 : 0) - (input.aimLeftAge ? 1 : 0),
      (input.aimTowardAge ? 1 : 0) - (input.aimAwayAge ? 1 : 0));
  }

  function screenAim(command, aimX, aimY) {
    if (!aimX && !aimY) return false;
    if (aimX && aimY) { aimX *= DIAGONAL; aimY *= DIAGONAL; }
    B.applyScreenAim(command, aimX, aimY);
    return true;
  }

  /* Gunner turret: traverse relative to the current aim (world yaw), snap
     to the hull, and let assist act only between presses so it never
     fights a held traverse. Snap nudges once on release; Lock tracks. */
  function gunner(command, traverse, snap, dt) {
    const player = B.playerTank();
    if (gunAngle !== gunAngle) gunAngle = player.turret;
    if (traverse) {
      gunHeld += dt;
      const ramp = Math.min(1, gunHeld / TRAVERSE_RAMP);
      gunAngle += traverse * dt
        * (TRAVERSE_MIN + (TRAVERSE_MAX - TRAVERSE_MIN) * ramp * ramp);
      if (gunAngle > Math.PI) gunAngle -= Math.PI * 2;
      else if (gunAngle < -Math.PI) gunAngle += Math.PI * 2;
      gunTracking = false;
      assist(command, false);
    } else if (gunHeld) { gunHeld = 0; gunTracking = true; }
    if (snap && !lastSnap) {
      gunAngle = player.yaw; gunTracking = true; assist(command, false);
    }
    lastSnap = snap;
    command.aimX = Math.sin(gunAngle); command.aimZ = Math.cos(gunAngle);
    if (gunTracking && prefs.assist !== ASSIST_OFF) {
      assist(command, true);
      gunAngle = Math.atan2(command.aimX, command.aimZ);
      // Snap is one nudge; Lock keeps tracking while a target is held.
      if (prefs.assist !== ASSIST_LOCK || input.assistLockTarget < 0)
        gunTracking = false;
    } else gunTracking = false;
  }

  function poll() {
    const state = B.state, online = B.online;
    const command = B.playerTank().command;
    const scheme = prefs.controls, classic = scheme === CLASSIC;
    const now = state.wallTime;
    const dt = lastPoll < 0 ? 0 : Math.max(0, Math.min(1 / 30, now - lastPoll));
    lastPoll = now;
    command.left = command.right = 0;
    command.reverse = false; command.fire = command.secondary = false;
    command.gadget = command.ultimate = false; command.lunge = 0;
    const pad = B.connectedGamepad();
    if (!!pad !== input.gamepadConnected) hint();
    let fire = false, secondary = false, gadget = false, ultimate = false;
    let pause = false, aimActive = false, pivot = 0, traverse = 0, snap = false;
    let tapActive = false, tapDirX = 0, tapDirY = -1;
    if (pad) {
      const buttons = pad.buttons;
      const nubX = Number(pad.axes[0]) || 0, nubY = Number(pad.axes[1]) || 0;
      const leftShoulder = !!buttons[4]?.pressed;
      const rightShoulder = !!buttons[5]?.pressed;
      const cross = !!buttons[0]?.pressed, circle = !!buttons[1]?.pressed;
      const square = !!buttons[2]?.pressed, triangle = !!buttons[3]?.pressed;
      const dpadLeft = !!buttons[14]?.pressed, dpadRight = !!buttons[15]?.pressed;
      gadget = !!buttons[12]?.pressed;
      pause = !!buttons[9]?.pressed;
      if (classic) {
        fire = cross; secondary = square; ultimate = triangle;
      } else {
        fire = rightShoulder; secondary = leftShoulder;
        ultimate = !!buttons[13]?.pressed;
        if (scheme === GUNNER) gadget = gadget || cross;
      }
      if (leftShoulder || rightShoulder || cross || circle || square
          || triangle || gadget || ultimate || pause || dpadLeft || dpadRight
          || Math.abs(nubX) > .16 || Math.abs(nubY) > .16) {
        B.setSource("gamepad");
        pivot = (dpadRight ? 1 : 0) - (dpadLeft ? 1 : 0);
        if (classic) {
          command.left = leftShoulder ? 1 : 0;
          command.right = rightShoulder ? 1 : 0;
          command.reverse = circle;
          tapActive = leftShoulder && rightShoulder;
          if (Math.sqrt(nubX * nubX + nubY * nubY) > DEAD_ZONE) {
            B.applyScreenAim(command, nubX, nubY);
            aimActive = true;
          }
        } else {
          arcadeMove(command, nubX, nubY);
          // Hysteresis keeps a held nub near the threshold from chattering.
          const magnitude = Math.sqrt(nubX * nubX + nubY * nubY);
          tapActive = magnitude > (tapState & 1 ? .35 : .6);
          if (tapActive) { tapDirX = nubX / magnitude; tapDirY = nubY / magnitude; }
          if (scheme === ARCADE) aimActive = faceAim(command, buttons);
          else {
            traverse = (circle ? 1 : 0) - (square ? 1 : 0);
            snap = triangle;
          }
        }
      }
    }
    if (!pad && input.source === "gamepad") B.setSource("keyboard");
    if (input.source !== "gamepad") {
      const key = B.keyboardDown;
      const moveX = (key("d") || key("moveRight") ? 1 : 0)
        - (key("a") || key("moveLeft") ? 1 : 0);
      const moveY = (key("s") || key("moveDown") ? 1 : 0)
        - (key("w") || key("moveUp") ? 1 : 0);
      if (!classic) {
        arcadeMove(command, moveX, moveY);
        tapActive = !!(moveX || moveY);
        if (tapActive) { tapDirX = moveX; tapDirY = moveY; }
      } else {
        const forward = key("w"), back = key("s");
        command.left = key("a") || forward || back ? 1 : 0;
        command.right = key("d") || forward || back ? 1 : 0;
        command.reverse = back || key("shift");
        tapActive = forward !== back;
        tapDirY = back ? 1 : -1;
      }
      pivot = (key("pivotRight") ? 1 : 0) - (key("pivotLeft") ? 1 : 0);
      fire = key("fire");
      secondary = key("secondary") || (!classic && key("shift"));
      gadget = key("gadget");
      ultimate = key("ultimate");
      pause = key("pause");
      if (scheme === GUNNER) {
        traverse = (key("aimRight") ? 1 : 0) - (key("aimLeft") ? 1 : 0);
        snap = key("aimUp");
      } else {
        let aimX = (key("aimRight") ? 1 : 0) - (key("aimLeft") ? 1 : 0);
        let aimY = (key("aimDown") ? 1 : 0) - (key("aimUp") ? 1 : 0);
        if (!aimX && !aimY && input.pointerActive) {
          aimX = input.pointerAimX; aimY = input.pointerAimY;
        }
        aimActive = screenAim(command, aimX, aimY);
      }
    }
    if (pivot) {
      // Opposite treads: spin in place; the turret keeps its world aim.
      command.left = -pivot; command.right = pivot; command.reverse = false;
    }
    if (tap(tapActive && !pivot, tapDirX, tapDirY, now))
      command.lunge = command.reverse ? -1 : 1;
    /* Face buttons also drive the pause menu (O resumes): a crank or snap
       pressed outside play must be released before it acts. */
    if (state.mode !== "playing" || input.fireReleaseRequired) gunLocked = true;
    if (!traverse && !snap) gunLocked = false;
    if (scheme === GUNNER) gunner(command, gunLocked ? 0 : traverse,
      snap && !gunLocked, dt);
    else assist(command, aimActive);
    if (input.fireReleaseRequired) {
      command.fire = false;
      if (!fire && !input.fireQueued) input.fireReleaseRequired = false;
    } else command.fire = input.fireQueued || fire;
    command.secondary = input.secondaryQueued || (secondary && !input.lastSecondary);
    command.gadget = input.gadgetQueued || (gadget && !input.lastGadget);
    command.ultimate = input.ultimateQueued || (ultimate && !input.lastUltimate);
    if (input.pauseQueued || (pause && !input.lastPause)) {
      if (state.mode === "playing") B.setMode("paused");
      else if (state.mode === "paused") B.setMode("playing");
    }
    if (online.active && online.role === "guest" && state.mode === "playing") {
      /* Preserve edges between 20Hz packets, including a tap whose release
         is polled before the next send. Clear only after successful handoff. */
      if (command.fire && !input.lastFire) online.pendingActions |= 8;
      if (command.secondary) online.pendingActions |= 16;
      if (command.gadget) online.pendingActions |= 32;
      if (command.ultimate) online.pendingActions |= 64;
      if (command.lunge) online.pendingActions |= 1024;
    }
    input.lastFire = fire; input.lastSecondary = secondary;
    input.lastGadget = gadget; input.lastUltimate = ultimate;
    input.lastPause = pause;
    input.fireQueued = input.secondaryQueued = input.gadgetQueued = false;
    input.ultimateQueued = input.pauseQueued = false;
  }

  /* Hull velocity for one tank and step: lunge burst/cooldown, drift grip,
     terrain traction. Returns the intended travel used for tread marks. */
  function slide(tank, dt, terrain, movement, speed, turn) {
    const command = tank.command;
    if (command.lunge) {
      if (!(tank.lungeCooldown > 0) && !tank.boss) {
        tank.lunge = 1; tank.lungeCooldown = LUNGE_COOLDOWN;
        tank.lungeSign = command.lunge < 0 ? -1 : 1;
      }
      command.lunge = 0;
    }
    if (tank.lungeCooldown > 0) {
      tank.lungeCooldown -= dt; if (tank.lungeCooldown < 0) tank.lungeCooldown = 0;
    }
    let traction = terrain === 0 ? Math.min(1, dt * 3.5)
      : terrain === 1 ? Math.min(1, dt * 6) : 1;
    let desired = movement * speed;
    if (tank.lunge > 0) {
      /* Extra hull speed decaying with LUNGE_TAU, added to whatever the
         treads ask for. The midpoint factor makes the summed distance match
         the continuous integral at any step size. */
      const half = Math.exp(-dt * .5 / LUNGE_TAU);
      desired += tank.lungeSign * (LUNGE_SCALE - 1) * tank.driveSpeed
        * tank.lunge * half;
      tank.lunge *= half * half;
      if (tank.lunge < .02) tank.lunge = 0;
      traction = 1;
    }
    const fast = tank.slideX * tank.slideX + tank.slideZ * tank.slideZ
      > tank.driveSpeed * tank.driveSpeed * DRIFT_SPEED * DRIFT_SPEED;
    if (fast && (turn > DRIFT_TURN || turn < -DRIFT_TURN))
      tank.drift = terrain === 0 ? ICE_DRIFT_TIME : DRIFT_TIME;
    if (tank.drift > 0) {
      /* Grip K/(remaining + .05) integrated exactly over the step: low while
         sliding, rising as the timer ends so traction returns smoothly. */
      const left = tank.drift > dt ? tank.drift - dt : 0;
      traction = Math.min(traction,
        1 - Math.pow((left + .05) / (tank.drift + .05), DRIFT_K));
      tank.drift = left;
    }
    tank.slideX += (tank.yawSine * desired - tank.slideX) * traction;
    tank.slideZ += (tank.yawCosine * desired - tank.slideZ) * traction;
    return desired * dt;
  }

  /* A Veteran or Ace bot whose flank faces a human shell's line lunges
     along its hull away from that line. Event-driven: runs per human shot. */
  function dodge(shooter) {
    const tanks = B.tanks;
    for (let at = 0; at < tanks.length; at++) {
      const bot = tanks[at];
      if (!bot.active || bot.player || bot.boss || bot.team === shooter.team
          || bot.lungeCooldown > 0 || bot.command.lunge) continue;
      const chance = DODGE_CHANCE[B.botDifficulty(bot)];
      if (!chance) continue;
      const dx = bot.x - shooter.x, dz = bot.z - shooter.z;
      const along = dx * shooter.turretSine + dz * shooter.turretCosine;
      const lateral = dx * shooter.turretCosine - dz * shooter.turretSine;
      const side = bot.yawSine * shooter.turretCosine
        - bot.yawCosine * shooter.turretSine;
      if (along < 1.5 || along > 9 || lateral > .8 || lateral < -.8
          || (side < .5 && side > -.5) || B.random() >= chance)
        continue;
      bot.command.lunge = lateral * side >= 0 ? 1 : -1;
    }
  }

  function attach(bridge) {
    B = bridge; input = B.input; prefs = B.preferences;
    ASSIST_OFF = B.ASSIST_OFF; ASSIST_LOCK = B.ASSIST_LOCK;
    hint();
    // Qualification and test runs only (__treadlineDebug.controls).
    if (B.lendTooling) B.lendTooling("controls", Object.freeze(Object.assign(Object.create(api), {BINDINGS,
      tuning: {TRAVERSE_MIN, TRAVERSE_MAX, TRAVERSE_RAMP, TAP_HOLD, TAP_GAP,
        LUNGE_SCALE, LUNGE_TAU, LUNGE_COOLDOWN, DRIFT_SPEED,
        DRIFT_TURN, DRIFT_TIME, ICE_DRIFT_TIME, DRIFT_K, DODGE_CHANCE},
      gunnerAngle() { return gunAngle; }})));
  }

  /* The Controls menu's rows for the current scheme and input: page 0
     driving, page 1 shooting. Menu-time only. */
  function bindings(page, scheme = prefs.controls) {
    return BINDINGS[scheme][keyboardLayout() ? 1 : 0][page];
  }

  /* Page controls for Deploy and resume. Tilefinch announces the exit chord
     on a page load's first claim; later claims ask it not to repeat that
     notice. False outside Tilefinch, where the caller uses fullscreen. */
  function claim(shell) {
    if (!shell || !navigator.tilefinch?.requestPageControls) return false;
    navigator.tilefinch.requestPageControls(shell, {notice: "once"})
      .catch(() => {});
    return true;
  }

  const api = {attach, poll, slide, dodge, hint, reset, bindings,
    claim, names: ["ARCADE", "CLASSIC", "GUNNER"]};
  Object.defineProperty(globalThis, "__treadlineControls", {
    value: Object.freeze(api), configurable: false, writable: false,
  });
})();
