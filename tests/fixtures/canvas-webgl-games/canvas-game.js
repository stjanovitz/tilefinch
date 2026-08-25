(() => {
  "use strict";

  const canvas = document.getElementById("game") || document.body.appendChild(
    Object.assign(document.createElement("canvas"), { id: "game" }),
  );
  canvas.width = 320;
  canvas.height = 180;
  const context = canvas.getContext("2d");
  if (!context) {
    globalThis.pocSummary = "CANVAS-GAME-NO-CONTEXT";
    return;
  }

  const sprite = document.createElement("canvas");
  sprite.width = 8;
  sprite.height = 8;
  const spriteContext = sprite.getContext("2d");
  spriteContext.fillStyle = "#fff4b0";
  spriteContext.beginPath();
  spriteContext.arc(4, 4, 3.5, 0, Math.PI * 2);
  spriteContext.fill();

  const state = {
    ballX: 54,
    ballY: 78,
    velocityX: 3,
    velocityY: -2,
    paddleX: 128,
    frames: 0,
    previousTimestamp: -1,
    monotonic: true,
    inputObserved: false,
  };
  globalThis.__canvasGameState = state;

  const bricks = [];
  for (let row = 0; row < 2; row++) {
    for (let column = 0; column < 8; column++) {
      bricks.push({ x: 12 + column * 37, y: 18 + row * 15 });
    }
  }

  function frame(timestamp) {
    state.frames++;
    state.monotonic &&= timestamp >= state.previousTimestamp;
    state.previousTimestamp = timestamp;

    const pad = navigator.getGamepads?.()[0];
    if (pad) {
      state.paddleX += pad.axes[0] * 5;
      state.inputObserved ||= Math.abs(pad.axes[0]) > .01;
    }
    state.paddleX = Math.max(4, Math.min(252, state.paddleX));
    state.ballX += state.velocityX;
    state.ballY += state.velocityY;
    if (state.ballX <= 5 || state.ballX >= 307) state.velocityX *= -1;
    if (state.ballY <= 5 || state.ballY >= 154) state.velocityY *= -1;

    context.clearRect(0, 0, canvas.width, canvas.height);
    context.fillStyle = "#182247";
    context.fillRect(0, 0, canvas.width, canvas.height);

    for (let index = 0; index < bricks.length; index++) {
      context.fillStyle = index % 2 ? "#ff8a5b" : "#62d5ff";
      context.fillRect(bricks[index].x, bricks[index].y, 31, 9);
    }
    context.fillStyle = "#f4f6ff";
    context.fillRect(state.paddleX, 162, 64, 8);
    context.drawImage(sprite, 0, 0, 8, 8, state.ballX, state.ballY, 10, 10);

    context.fillStyle = "#ffffff";
    context.font = "bold 12px sans-serif";
    context.textAlign = "right";
    context.fillText(`FRAME ${state.frames}`, 309, 151);

    if (state.frames < 8) {
      requestAnimationFrame(frame);
      return;
    }
    const sample = context.getImageData(8, 8, 1, 1).data;
    const passed = state.monotonic && state.ballX !== 54
      && state.paddleX >= 4 && state.paddleX <= 252 && sample[3] === 255
      && context.measureText("FRAME 8").width > 0;
    globalThis.pocSummary = passed ? "CANVAS-GAME-PASS" :
      `CANVAS-GAME-FAIL:${state.frames}:${state.ballX}:${state.paddleX}:${sample[3]}`;
    document.documentElement.dataset.gameResult = passed ? "pass" : "fail";
  }

  /* Paint one usable frame during startup; subsequent animation remains tied
     to presentation and therefore pauses with page visibility. */
  frame(0);
})();
