(() => {
  "use strict";

  const canvas = document.getElementById("game");
  const gl = canvas && canvas.getContext("webgl", {
    alpha: false, depth: true, antialias: false,
  });
  const ui = {
    panel: document.getElementById("panel"),
    heading: document.querySelector("#panel h1"),
    message: document.getElementById("message"),
    play: document.getElementById("play"),
    score: document.getElementById("score"),
    level: document.getElementById("level"),
    lives: document.getElementById("lives"),
    power: document.getElementById("power"),
    toast: document.getElementById("toast"),
  };
  if (!gl || !ui.panel || !ui.play) {
    if (ui.message) ui.message.textContent = "WebGL is unavailable.";
    globalThis.pocSummary = "PRISM-BREAK-NO-WEBGL";
    return;
  }

  const MAX_VERTICES = 3072;
  const MAX_INDICES = 4608;
  const positions = new Float32Array(MAX_VERTICES * 3);
  const colors = new Float32Array(MAX_VERTICES * 4);
  const indices = new Uint16Array(MAX_INDICES);
  let vertexCount = 0, indexCount = 0;

  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS))
      throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, `
    attribute vec3 aPosition;
    attribute vec4 aColor;
    uniform mat4 uProjection;
    uniform mat4 uModel;
    varying lowp vec4 vColor;
    void main(void) {
      gl_Position = uProjection * uModel * vec4(aPosition, 1.0);
      vColor = aColor;
    }
  `));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, `
    varying lowp vec4 vColor;
    void main(void) { gl_FragColor = vColor; }
  `));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);

  const positionBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, positions.byteLength, gl.DYNAMIC_DRAW);
  const positionLocation = gl.getAttribLocation(program, "aPosition");
  gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(positionLocation);
  const colorBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, colorBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, colors.byteLength, gl.DYNAMIC_DRAW);
  const colorLocation = gl.getAttribLocation(program, "aColor");
  gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
  gl.enableVertexAttribArray(colorLocation);
  const indexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices.byteLength, gl.DYNAMIC_DRAW);
  const staticPositionBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, staticPositionBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, positions.byteLength, gl.DYNAMIC_DRAW);
  const staticColorBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, staticColorBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, colors.byteLength, gl.DYNAMIC_DRAW);
  const staticIndexBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, staticIndexBuffer);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices.byteLength, gl.DYNAMIC_DRAW);

  const projection = new Float32Array(16);
  const f = 1 / Math.tan(52 * Math.PI / 360), aspect = 320 / 180,
    near = .5, far = 30;
  projection[0] = f / aspect;
  projection[5] = f;
  projection[10] = (far + near) / (near - far);
  projection[11] = -1;
  projection[14] = 2 * far * near / (near - far);
  gl.uniformMatrix4fv(
    gl.getUniformLocation(program, "uProjection"), false, projection);
  const modelLocation = gl.getUniformLocation(program, "uModel");
  const model = new Float32Array(16);
  gl.enable(gl.DEPTH_TEST);
  gl.depthFunc(gl.LEQUAL);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  gl.clearColor(.008, .016, .045, 1);

  const BOX_VERTICES = new Int8Array([
    -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1,
     1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1,
    -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1,
    -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1,
     1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1,
    -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1,
  ]);
  const BOX_INDICES = new Uint8Array([
     0, 1, 2,  0, 2, 3,  4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11, 12,13,14, 12,14,15,
    16,17,18, 16,18,19, 20,21,22, 20,22,23,
  ]);
  const OCTA_VERTICES = new Int8Array([
     1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1,
  ]);
  const OCTA_INDICES = new Uint8Array([
    2,0,4, 2,4,1, 2,1,5, 2,5,0,
    3,4,0, 3,1,4, 3,5,1, 3,0,5,
  ]);
  const FACE_SHADE = new Float32Array([1, .56, 1.18, .46, .82, .68]);
  const rotationX = -.17, rotationY = .09;
  const cosX = Math.cos(rotationX), sinX = Math.sin(rotationX);
  const cosY = Math.cos(rotationY), sinY = Math.sin(rotationY);

  function writeVertex(x, y, z, color, shade, alpha = 1,
                       brightness = 1) {
    if (vertexCount >= MAX_VERTICES) return false;
    const rotatedX = x * cosY + z * sinY;
    const rotatedZ = -x * sinY + z * cosY;
    const rotatedY = y * cosX - rotatedZ * sinX;
    const viewZ = y * sinX + rotatedZ * cosX - 7.7;
    const p = vertexCount * 3, c = vertexCount * 4;
    positions[p] = rotatedX;
    positions[p + 1] = rotatedY;
    positions[p + 2] = viewZ;
    colors[c] = Math.min(1, color[0] * shade * brightness);
    colors[c + 1] = Math.min(1, color[1] * shade * brightness);
    colors[c + 2] = Math.min(1, color[2] * shade * brightness);
    colors[c + 3] = alpha;
    vertexCount++;
    return true;
  }

  function addBox(x, y, z, width, height, depth, color, alpha = 1,
                  brightness = 1) {
    if (vertexCount + 24 > MAX_VERTICES || indexCount + 36 > MAX_INDICES)
      return false;
    const base = vertexCount;
    for (let at = 0; at < 24; at++) {
      const source = at * 3;
      writeVertex(
        x + BOX_VERTICES[source] * width * .5,
        y + BOX_VERTICES[source + 1] * height * .5,
        z + BOX_VERTICES[source + 2] * depth * .5,
        color, FACE_SHADE[(at / 4) | 0], alpha, brightness);
    }
    for (let at = 0; at < 36; at++) indices[indexCount++] = base + BOX_INDICES[at];
    return true;
  }

  function addOctahedron(x, y, z, radius, color, alpha = 1) {
    if (vertexCount + 6 > MAX_VERTICES || indexCount + 24 > MAX_INDICES)
      return false;
    const base = vertexCount;
    for (let at = 0; at < 6; at++) {
      const source = at * 3;
      writeVertex(
        x + OCTA_VERTICES[source] * radius,
        y + OCTA_VERTICES[source + 1] * radius,
        z + OCTA_VERTICES[source + 2] * radius,
        color, at < 2 ? .82 : at < 4 ? 1.08 : 1, alpha);
    }
    for (let at = 0; at < 24; at++)
      indices[indexCount++] = base + OCTA_INDICES[at];
    return true;
  }

  const LEVELS = [
    ["11111111", "11111111", "11111111", "11111111", "11111111"],
    [".122221.", "121..121", "21211212", "12122121", ".211112."],
    ["33333333", "3.2222.3", "32.11.23", "321..123", "22222222"],
    ["4.4.4.4.", ".333333.", "22222222", "2.1111.2", "11111111", ".1.1.1.1"],
  ];
  const PALETTE = [
    [0,0,0], [.18,.88,1], [.78,.3,1], [1,.36,.25], [1,.84,.24],
  ];
  const POWER_COLORS = {
    wide: [.2,1,.64], multi: [1,.45,.98], slow: [.35,.72,1],
    shield: [1,.9,.25], laser: [1,.25,.2],
  };
  const POWER_NAMES = {
    wide: "WIDE PADDLE", multi: "MULTIBALL", slow: "SLOW FIELD",
    shield: "PRISM SHIELD", laser: "LASER CORE",
  };
  const powerCycle = ["wide", "multi", "slow", "shield", "laser"];
  const SCENE_COLORS = {
    star: [.28,.55,1], floor: [.05,.16,.32], rail: [.08,.32,.58],
    paddle: [.16,.72,1], paddleWide: [.2,1,.65], paddleLaser: [1,.28,.22],
    shield: [1,.88,.22], trail: [.24,.85,1], ball: [1,.97,.7],
    shot: [1,.25,.18], flash: [1,1,1],
  };
  const stars = [];
  let randomState = 0x51f15e1d;
  function random() {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return (randomState >>> 0) / 4294967296;
  }
  for (let at = 0; at < 24; at++) {
    stars.push({
      x: random() * 10 - 5, y: random() * 6 - 3,
      z: -.7 - random() * .8, size: .012 + random() * .022,
    });
  }

  const state = {
    mode: "title", score: 0, lives: 3, level: 0, combo: 0,
    bricks: [], balls: [], particles: [], powerups: [], shots: [],
    paddle: { x: 0, width: 1.55 },
    shield: 0, laser: 0, wideUntil: 0, slowUntil: 0,
    time: 0, transition: 0, shake: 0, flash: 0,
    destroyed: 0, frames: 0, meshDrops: 0, running: true,
  };
  const input = {
    left: false, right: false, launch: false, pause: false,
    axis: 0, lastPrimary: false, lastPause: false,
  };
  let lastTimestamp = 0, accumulator = 0, toastUntil = 0;
  let hudCache = "", hudDirty = true, hudSecond = -1;
  let staticDirty = true, staticVertexCount = 0, staticIndexCount = 0;

  function makeBall(x = state.paddle.x, y = -2.45, vx = 1.8, vy = 3.2) {
    return {
      x, y, vx, vy, radius: .12, stuck: true, alive: true,
      trailX: new Float32Array(7), trailY: new Float32Array(7),
      trailHead: 0, trailCount: 0,
    };
  }

  function buildLevel(index) {
    state.bricks.length = 0;
    state.powerups.length = 0;
    state.shots.length = 0;
    const rows = LEVELS[index % LEVELS.length];
    for (let row = 0; row < rows.length; row++) {
      for (let column = 0; column < rows[row].length; column++) {
        const hp = Number(rows[row][column]);
        if (!hp) continue;
        state.bricks.push({
          x: (column - 3.5) * 1.08,
          y: 2.42 - row * .52,
          width: .98, height: .36,
          hp, maximum: hp, alive: true,
          color: PALETTE[Math.min(4, hp)],
        });
      }
    }
    state.balls.length = 0;
    state.balls.push(makeBall());
    state.paddle.x = 0;
    state.paddle.width = state.time < state.wideUntil ? 2.35 : 1.55;
    state.combo = 0;
    state.transition = 0;
    staticDirty = true;
    hudDirty = true;
    showToast(`LEVEL ${index + 1}`, 1.2);
  }

  function resetGame() {
    state.score = 0;
    state.lives = 3;
    state.level = 0;
    state.destroyed = 0;
    state.shield = 0;
    state.laser = 0;
    state.wideUntil = 0;
    state.slowUntil = 0;
    state.particles.length = 0;
    randomState = 0x51f15e1d;
    buildLevel(0);
    hudDirty = true;
  }

  function showPanel(title, message, label) {
    ui.heading.textContent = title;
    ui.message.textContent = message;
    ui.play.textContent = label;
    ui.panel.hidden = false;
  }

  function hidePanel() { ui.panel.hidden = true; }

  function showToast(text, seconds = .8) {
    ui.toast.textContent = text;
    ui.toast.classList.add("visible");
    toastUntil = state.time + seconds;
  }

  function setMode(mode) {
    state.mode = mode;
    if (mode === "playing") hidePanel();
    else if (mode === "paused")
      showPanel("PAUSED", "The prism field is frozen.", "Resume");
    else if (mode === "game-over")
      showPanel("GAME OVER", `Final score ${state.score}.`, "Play again");
    else if (mode === "victory")
      showPanel("PRISM MASTER", `All levels clear · ${state.score} points.`, "Play again");
  }

  class SoundBank {
    constructor() {
      this.context = null;
      this.voices = [];
      this.next = 0;
    }
    start() {
      if (this.context || typeof AudioContext !== "function") return;
      try {
        this.context = new AudioContext();
        this.context.resume().then(() => {
          for (let at = 0; at < 2; at++) {
            const gain = this.context.createGain();
            const oscillator = this.context.createOscillator();
            gain.gain.value = 0;
            oscillator.type = at ? "triangle" : "square";
            oscillator.connect(gain).connect(this.context.destination);
            oscillator.start();
            this.voices.push({ oscillator, gain, timer: 0 });
          }
        }).catch(() => {});
      } catch (_) { this.context = null; }
    }
    play(frequency, duration = .045, volume = .12) {
      if (!this.voices.length) return;
      const voice = this.voices[this.next++ % this.voices.length];
      voice.oscillator.frequency.value = frequency;
      voice.gain.gain.value = volume;
      clearTimeout(voice.timer);
      voice.timer = setTimeout(() => { voice.gain.gain.value = 0; }, duration * 1000);
    }
  }
  const sounds = new SoundBank();

  function requestPresentation() {
    sounds.start();
    const shell = document.getElementById("game-shell");
    if (shell && shell.requestFullscreen)
      shell.requestFullscreen().catch(() => {});
    canvas.focus();
  }

  function startOrResume() {
    requestPresentation();
    if (state.mode === "paused") {
      setMode("playing");
      return;
    }
    resetGame();
    setMode("playing");
  }
  ui.play.addEventListener("click", startOrResume);

  function togglePause() {
    if (state.mode === "playing") setMode("paused");
    else if (state.mode === "paused") setMode("playing");
  }

  function spawnParticles(x, y, color, count = 6) {
    for (let at = 0; at < count && state.particles.length < 48; at++) {
      const angle = random() * Math.PI * 2, speed = .7 + random() * 1.8;
      state.particles.push({
        x, y, vx: Math.cos(angle) * speed, vy: Math.sin(angle) * speed,
        life: .35 + random() * .5, maximum: .85, color,
      });
    }
  }

  function maybeDropPower(brick) {
    if (state.powerups.length >= 4 || state.destroyed % 7 !== 0) return;
    const type = powerCycle[((state.destroyed / 7) | 0) % powerCycle.length];
    state.powerups.push({ x: brick.x, y: brick.y, type, phase: 0 });
  }

  function destroyBrick(brick) {
    brick.alive = false;
    staticDirty = true;
    state.destroyed++;
    state.combo = Math.min(12, state.combo + 1);
    state.score += 100 * state.combo * brick.maximum;
    hudDirty = true;
    const color = PALETTE[Math.min(4, brick.maximum)];
    spawnParticles(brick.x, brick.y, color, 7);
    maybeDropPower(brick);
    state.shake = Math.min(.13, state.shake + .035);
    state.flash = .09;
    sounds.play(240 + state.combo * 18, .035, .1);
    if (!state.bricks.some((candidate) => candidate.alive)) {
      state.mode = "level-clear";
      state.transition = 1.25;
      showToast("FIELD CLEAR", 1.1);
      sounds.play(660, .18, .16);
    }
  }

  function hitBrick(brick) {
    if (!brick.alive) return;
    brick.hp--;
    staticDirty = true;
    if (brick.hp <= 0) destroyBrick(brick);
    else {
      state.score += 35;
      hudDirty = true;
      spawnParticles(brick.x, brick.y, PALETTE[Math.min(4, brick.maximum)], 3);
      state.shake = Math.min(.1, state.shake + .018);
      sounds.play(170 + brick.hp * 35, .025, .07);
    }
  }

  function applyPower(type) {
    const duration = state.time + 9;
    if (type === "wide") {
      state.wideUntil = duration;
      state.paddle.width = 2.35;
    } else if (type === "slow") {
      state.slowUntil = duration;
      for (const ball of state.balls) { ball.vx *= .78; ball.vy *= .78; }
    } else if (type === "shield") state.shield = 1;
    else if (type === "laser") state.laser = 12;
    else if (type === "multi" && state.balls.length < 3) {
      const source = state.balls.find((ball) => ball.alive) || makeBall();
      while (state.balls.length < 3) {
        const direction = state.balls.length & 1 ? -1 : 1;
        const ball = makeBall(
          source.x, source.y,
          direction * Math.max(1.8, Math.abs(source.vx)),
          Math.max(2.5, Math.abs(source.vy)));
        ball.stuck = false;
        state.balls.push(ball);
      }
    }
    showToast(POWER_NAMES[type], 1);
    hudDirty = true;
    sounds.play(520, .12, .13);
  }

  function launchOrFire() {
    const stuck = state.balls.find((ball) => ball.alive && ball.stuck);
    if (stuck) {
      stuck.stuck = false;
      const slowScale = state.time < state.slowUntil ? .78 : 1;
      const speed = (3.55 + state.level * .18) * slowScale;
      stuck.vx = input.axis * 1.4 || 1.25;
      stuck.vy = Math.sqrt(Math.max(2, speed * speed - stuck.vx * stuck.vx));
      sounds.play(330, .06, .1);
      return;
    }
    if (state.laser > 0 && state.shots.length <= 6) {
      state.shots.push({ x: state.paddle.x - state.paddle.width * .34, y: -2.22 });
      state.shots.push({ x: state.paddle.x + state.paddle.width * .34, y: -2.22 });
      state.laser--;
      hudDirty = true;
      sounds.play(760, .04, .08);
    }
  }

  function pollInput() {
    let axis = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    let primary = input.launch, pause = input.pause;
    input.launch = input.pause = false;
    const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pad = gamepads && gamepads[0];
    if (pad && pad.connected) {
      const analog = Number(pad.axes[0]) || 0;
      const digital = (pad.buttons[15]?.pressed ? 1 : 0)
        - (pad.buttons[14]?.pressed ? 1 : 0);
      axis = Math.abs(analog) > .12 ? analog : digital;
      primary ||= !!pad.buttons[0]?.pressed;
      pause ||= !!pad.buttons[3]?.pressed;
    }
    input.axis = Math.max(-1, Math.min(1, axis));
    if (primary && !input.lastPrimary) {
      if (state.mode === "playing") launchOrFire();
      else if (state.mode === "paused") setMode("playing");
      else if (["title", "game-over", "victory"].includes(state.mode)) {
        resetGame();
        setMode("playing");
      }
    }
    if (pause && !input.lastPause) togglePause();
    input.lastPrimary = primary;
    input.lastPause = pause;
  }

  addEventListener("keydown", (event) => {
    if (event.key === "ArrowLeft" || event.key === "a") input.left = true;
    if (event.key === "ArrowRight" || event.key === "d") input.right = true;
    if (event.key === " " || event.key === "Enter") input.launch = true;
    if (event.key === "p" || event.key === "Escape") input.pause = true;
  });
  addEventListener("keyup", (event) => {
    if (event.key === "ArrowLeft" || event.key === "a") input.left = false;
    if (event.key === "ArrowRight" || event.key === "d") input.right = false;
  });
  canvas.addEventListener("pointermove", (event) => {
    if (!canvas.clientWidth) return;
    const relative = (event.clientX - canvas.getBoundingClientRect().left)
      / canvas.clientWidth;
    const bound = 4.42 - state.paddle.width * .5;
    state.paddle.x = Math.max(-bound, Math.min(bound, relative * 9 - 4.5));
  });
  canvas.addEventListener("pointerdown", () => {
    if (state.mode === "playing") launchOrFire();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden && state.mode === "playing") {
      state.mode = "hidden";
    } else if (!document.hidden && state.mode === "hidden") {
      state.mode = "playing";
      lastTimestamp = 0;
    }
  });

  function ballBrickCollision(ball, brick) {
    const dx = ball.x - brick.x, dy = ball.y - brick.y;
    const reachX = brick.width * .5 + ball.radius;
    const reachY = brick.height * .5 + ball.radius;
    if (Math.abs(dx) > reachX || Math.abs(dy) > reachY) return false;
    const penetrationX = reachX - Math.abs(dx);
    const penetrationY = reachY - Math.abs(dy);
    if (penetrationX < penetrationY) {
      ball.vx = dx < 0 ? -Math.abs(ball.vx) : Math.abs(ball.vx);
      ball.x += dx < 0 ? -penetrationX : penetrationX;
    } else {
      ball.vy = dy < 0 ? -Math.abs(ball.vy) : Math.abs(ball.vy);
      ball.y += dy < 0 ? -penetrationY : penetrationY;
    }
    hitBrick(brick);
    return true;
  }

  function loseBall(ball) {
    if (state.shield > 0) {
      state.shield = 0;
      ball.y = -3;
      ball.vy = Math.abs(ball.vy);
      state.shake = .12;
      sounds.play(140, .1, .14);
      return;
    }
    ball.alive = false;
    ball.trailCount = 0;
    if (state.balls.some((candidate) => candidate.alive)) return;
    state.lives--;
    hudDirty = true;
    state.combo = 0;
    if (state.lives <= 0) {
      setMode("game-over");
      sounds.play(90, .3, .14);
    } else {
      state.balls.length = 0;
      state.balls.push(makeBall());
      showToast("BALL LOST", .9);
    }
  }

  function updateBall(ball, dt) {
    if (!ball.alive) return;
    if (ball.stuck) {
      ball.x = state.paddle.x;
      ball.y = -2.18;
      return;
    }
    ball.trailHead = (ball.trailHead + 6) % 7;
    ball.trailX[ball.trailHead] = ball.x;
    ball.trailY[ball.trailHead] = ball.y;
    if (ball.trailCount < 7) ball.trailCount++;
    ball.x += ball.vx * dt;
    ball.y += ball.vy * dt;
    if (ball.x < -4.42 + ball.radius) {
      ball.x = -4.42 + ball.radius;
      ball.vx = Math.abs(ball.vx);
      sounds.play(150, .02, .045);
    } else if (ball.x > 4.42 - ball.radius) {
      ball.x = 4.42 - ball.radius;
      ball.vx = -Math.abs(ball.vx);
      sounds.play(150, .02, .045);
    }
    if (ball.y > 3.05 - ball.radius) {
      ball.y = 3.05 - ball.radius;
      ball.vy = -Math.abs(ball.vy);
      sounds.play(180, .02, .045);
    }
    const paddleTop = -2.35;
    if (ball.vy < 0 && ball.y - ball.radius <= paddleTop
        && ball.y > paddleTop - .28
        && Math.abs(ball.x - state.paddle.x)
           <= state.paddle.width * .5 + ball.radius) {
      ball.y = paddleTop + ball.radius;
      const hit = (ball.x - state.paddle.x) / (state.paddle.width * .5);
      const speed = Math.min(5.25, Math.hypot(ball.vx, ball.vy) * 1.015);
      ball.vx = Math.max(-3.7, Math.min(3.7, hit * 3.3));
      ball.vy = Math.sqrt(Math.max(2.2, speed * speed - ball.vx * ball.vx));
      state.combo = 0;
      sounds.play(290, .035, .07);
    }
    for (const brick of state.bricks) {
      if (brick.alive && ballBrickCollision(ball, brick)) break;
    }
    if (ball.y < -3.38) loseBall(ball);
  }

  function updatePowerups(dt) {
    for (let at = state.powerups.length - 1; at >= 0; at--) {
      const power = state.powerups[at];
      power.y -= 1.15 * dt;
      power.phase += dt * 5;
      if (power.y < -2.15 && power.y > -2.75
          && Math.abs(power.x - state.paddle.x)
             < state.paddle.width * .5 + .18) {
        applyPower(power.type);
        state.powerups.splice(at, 1);
      } else if (power.y < -3.35) state.powerups.splice(at, 1);
    }
  }

  function updateShots(dt) {
    for (let at = state.shots.length - 1; at >= 0; at--) {
      const shot = state.shots[at];
      shot.y += 5.6 * dt;
      let removed = shot.y > 3.3;
      if (!removed) {
        for (const brick of state.bricks) {
          if (brick.alive && Math.abs(shot.x - brick.x) < brick.width * .5
              && Math.abs(shot.y - brick.y) < brick.height * .5 + .12) {
            hitBrick(brick);
            removed = true;
            break;
          }
        }
      }
      if (removed) state.shots.splice(at, 1);
    }
  }

  function updateEffects(dt) {
    state.shake = Math.max(0, state.shake - dt * .45);
    state.flash = Math.max(0, state.flash - dt);
    for (let at = state.particles.length - 1; at >= 0; at--) {
      const particle = state.particles[at];
      particle.life -= dt;
      if (particle.life <= 0) {
        state.particles.splice(at, 1);
        continue;
      }
      particle.x += particle.vx * dt;
      particle.y += particle.vy * dt;
      particle.vy -= 1.8 * dt;
    }
    if (toastUntil && state.time >= toastUntil) {
      ui.toast.classList.remove("visible");
      toastUntil = 0;
    }
  }

  function update(dt) {
    state.time += dt;
    pollInput();
    updateEffects(dt);
    if (state.mode === "level-clear") {
      state.transition -= dt;
      if (state.transition <= 0) {
        state.level++;
        if (state.level >= LEVELS.length) setMode("victory");
        else {
          buildLevel(state.level);
          setMode("playing");
        }
      }
      return;
    }
    if (state.mode !== "playing") return;
    if (state.time >= state.wideUntil) state.paddle.width = 1.55;
    const movement = input.axis * 5.4 * dt;
    state.paddle.x = Math.max(
      -4.42 + state.paddle.width * .5,
      Math.min(4.42 - state.paddle.width * .5, state.paddle.x + movement));
    for (const ball of state.balls) updateBall(ball, dt);
    updatePowerups(dt);
    updateShots(dt);
  }

  function buildStaticScene() {
    vertexCount = indexCount = 0;
    for (const star of stars)
      addOctahedron(star.x, star.y, star.z, star.size,
        SCENE_COLORS.star, .48);
    addBox(0, -3.18, .16, 9.35, .08, .42, SCENE_COLORS.floor);
    addBox(-4.58, 0, .06, .12, 6.35, .35, SCENE_COLORS.rail);
    addBox(4.58, 0, .06, .12, 6.35, .35, SCENE_COLORS.rail);
    addBox(0, 3.18, .06, 9.25, .12, .35, SCENE_COLORS.rail);

    for (const brick of state.bricks) {
      if (!brick.alive) continue;
      const damage = brick.hp / brick.maximum;
      if (!addBox(brick.x, brick.y, 0, brick.width, brick.height, .34,
                  brick.color, 1, .58 + .42 * damage))
        state.meshDrops++;
    }
    staticVertexCount = vertexCount;
    staticIndexCount = indexCount;
  }

  function buildDynamicScene() {
    vertexCount = indexCount = 0;
    const paddleColor = state.laser ? SCENE_COLORS.paddleLaser
      : state.time < state.wideUntil
        ? SCENE_COLORS.paddleWide : SCENE_COLORS.paddle;
    addBox(state.paddle.x, -2.52, .04,
      state.paddle.width, .25, .44, paddleColor);
    if (state.shield)
      addBox(0, -3.02, .02, 8.6, .06, .16, SCENE_COLORS.shield, .8);

    for (const ball of state.balls) {
      if (!ball.alive) continue;
      for (let at = ball.trailCount - 1; at >= 0; at--) {
        const slot = (ball.trailHead + at) % 7;
        const strength = 1 - at / ball.trailCount;
        addOctahedron(ball.trailX[slot], ball.trailY[slot], .08,
          ball.radius * (.3 + strength * .45),
          SCENE_COLORS.trail, strength * .24);
      }
      addOctahedron(ball.x, ball.y, .1, ball.radius, SCENE_COLORS.ball);
    }
    for (const power of state.powerups) {
      const bob = Math.sin(power.phase) * .045;
      addOctahedron(power.x, power.y + bob, .14, .19,
        POWER_COLORS[power.type]);
    }
    for (const shot of state.shots)
      addBox(shot.x, shot.y, .1, .055, .28, .09, SCENE_COLORS.shot);
    for (const particle of state.particles) {
      const alpha = Math.min(1, particle.life / particle.maximum);
      addOctahedron(particle.x, particle.y, .22, .035 + .035 * alpha,
        particle.color, alpha);
    }
    if (state.flash > 0)
      addBox(0, 0, .52, 9.05, 5.9, .015,
             SCENE_COLORS.flash, state.flash * 2.2);
  }

  function uploadMesh(position, color, index) {
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      positions.subarray(0, vertexCount * 3));
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0,
      colors.subarray(0, vertexCount * 4));
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.bufferSubData(gl.ELEMENT_ARRAY_BUFFER, 0,
      indices.subarray(0, indexCount));
  }

  function drawMesh(position, color, index, count) {
    gl.bindBuffer(gl.ARRAY_BUFFER, position);
    gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, color);
    gl.vertexAttribPointer(colorLocation, 4, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index);
    gl.drawElements(gl.TRIANGLES, count, gl.UNSIGNED_SHORT, 0);
  }

  function updateModel() {
    const angle = state.time * 71;
    const shakeX = Math.sin(angle) * state.shake;
    const shakeY = Math.cos(angle * 1.31) * state.shake * .72;
    /* Keep the title field visibly alive without starting game physics. The
       same model-uniform path used by play animates the whole star/rail field,
       so the title adds no geometry uploads, particles, or DOM mutations. */
    const title = state.mode === "title" ? 1 : 0;
    const titleX = title * Math.sin(state.time * .72) * .075;
    const titleY = title * Math.cos(state.time * .54) * .045;
    const roll = Math.sin(angle * .67) * state.shake * .022
      + title * Math.sin(state.time * .38) * .012;
    const cosine = Math.cos(roll), sine = Math.sin(roll);
    model.fill(0);
    model[0] = cosine;
    model[1] = sine;
    model[4] = -sine;
    model[5] = cosine;
    model[10] = 1;
    model[12] = shakeX + titleX;
    model[13] = shakeY + titleY;
    model[15] = 1;
    gl.uniformMatrix4fv(modelLocation, false, model);
  }

  function updateHud() {
    const second = state.time | 0;
    if (!hudDirty && second === hudSecond) return;
    hudDirty = false;
    hudSecond = second;
    let powerText = "";
    if (state.time < state.wideUntil) powerText = "WIDE";
    if (state.time < state.slowUntil)
      powerText += `${powerText ? " · " : ""}SLOW`;
    if (state.shield) powerText += `${powerText ? " · " : ""}SHIELD`;
    if (state.laser)
      powerText += `${powerText ? " · " : ""}LASER ${state.laser}`;
    const next = `${state.score}|${state.level}|${state.lives}|${powerText}`;
    if (next === hudCache) return;
    hudCache = next;
    ui.score.textContent = String(state.score).padStart(6, "0");
    ui.level.textContent = `LEVEL ${Math.min(LEVELS.length, state.level + 1)}`;
    ui.lives.textContent = `LIVES ${state.lives}`;
    ui.power.textContent = powerText;
  }

  function render() {
    if (staticDirty) {
      buildStaticScene();
      uploadMesh(staticPositionBuffer, staticColorBuffer, staticIndexBuffer);
      staticDirty = false;
    }
    buildDynamicScene();
    uploadMesh(positionBuffer, colorBuffer, indexBuffer);
    updateModel();
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (staticIndexCount)
      drawMesh(staticPositionBuffer, staticColorBuffer,
        staticIndexBuffer, staticIndexCount);
    if (indexCount)
      drawMesh(positionBuffer, colorBuffer, indexBuffer, indexCount);
    state.frames++;
    updateHud();
  }

  function frame(timestamp) {
    if (!state.running) return;
    const elapsed = lastTimestamp ? Math.min(.05, (timestamp - lastTimestamp) / 1000) : 0;
    lastTimestamp = timestamp;
    accumulator = Math.min(.08, accumulator + elapsed);
    while (accumulator >= 1 / 60) {
      update(1 / 60);
      accumulator -= 1 / 60;
    }
    render();
    requestAnimationFrame(frame);
  }

  const debug = Object.freeze({
    start() { resetGame(); setMode("playing"); },
    step(frames = 1) {
      frames = Math.max(0, Math.min(600, Number(frames) | 0));
      for (let at = 0; at < frames; at++) update(1 / 60);
      render();
    },
    clearLevel() {
      for (const brick of state.bricks) brick.alive = false;
      state.mode = "level-clear";
      state.transition = 0;
      staticDirty = true;
    },
    injectPower(type) {
      if (Object.prototype.hasOwnProperty.call(POWER_NAMES, type)) applyPower(type);
    },
    burst() {
      spawnParticles(0, 0, SCENE_COLORS.ball, 48);
      state.shake = .13;
      state.flash = .09;
      render();
    },
    launch() { launchOrFire(); },
    snapshot() {
      return {
        mode: state.mode, score: state.score, lives: state.lives,
        level: state.level, bricks: state.bricks.filter((brick) => brick.alive).length,
        balls: state.balls.filter((ball) => ball.alive).length,
        movingBalls: state.balls.filter((ball) => ball.alive && !ball.stuck).length,
        particles: state.particles.length, powerups: state.powerups.length,
        shots: state.shots.length, vertices: vertexCount, indices: indexCount,
        staticVertices: staticVertexCount, staticIndices: staticIndexCount,
        paddleX: state.paddle.x, paddleWidth: state.paddle.width,
        shield: state.shield, laser: state.laser,
        meshDrops: state.meshDrops, frames: state.frames,
        titleMotionX: model[12], titleMotionY: model[13],
      };
    },
    stop() { state.running = false; },
  });
  Object.defineProperty(globalThis, "__prismBreakDebug", {
    value: debug, configurable: false, writable: false,
  });
  globalThis.pocSummary = "PRISM-BREAK-READY";
  if (location.search.includes("demo=1")) {
    resetGame();
    setMode("playing");
    launchOrFire();
  }
  render();
  requestAnimationFrame(frame);
})();
