(() => {
  "use strict";
  const canvas = document.createElement("canvas");
  canvas.width = 32;
  canvas.height = 32;
  document.body.appendChild(canvas);
  const context = canvas.getContext("2d");
  context.fillStyle = "#ff0000";
  context.fillRect(0, 0, 32, 32);
  context.save();
  context.beginPath();
  context.rect(8, 8, 16, 16);
  context.clip();
  context.translate(4, 4);
  context.fillStyle = "#0000ff";
  context.fillRect(0, 0, 24, 24);
  context.restore();

  const sprite = document.createElement("canvas");
  sprite.width = 2;
  sprite.height = 2;
  const spriteContext = sprite.getContext("2d");
  spriteContext.fillStyle = "#00ff00";
  spriteContext.fillRect(0, 0, 2, 2);
  context.imageSmoothingEnabled = false;
  context.drawImage(sprite, 0, 0, 2, 2, 24, 0, 8, 8);

  context.save();
  context.globalCompositeOperation = "destination-out";
  context.fillRect(28, 28, 4, 4);
  context.restore();
  context.font = "12px sans-serif";
  context.textBaseline = "top";
  context.fillStyle = "white";
  context.fillText("A", 1, 18);

  const outside = context.getImageData(2, 2, 1, 1).data;
  const inside = context.getImageData(12, 12, 1, 1).data;
  const scaled = context.getImageData(26, 2, 1, 1).data;
  const erased = context.getImageData(30, 30, 1, 1).data;
  const large = document.createElement("canvas");
  large.width = 640;
  large.height = 480;
  const passed = outside[0] > 200 && outside[2] < 20
    && inside[2] > 200 && inside[0] < 20
    && scaled[1] > 200 && erased[3] === 0
    && context.measureText("Canvas").width > 0
    && large.width === 362 && large.height === 272
    && canvas.toDataURL().startsWith("data:image/png;base64,");
  globalThis.pocSummary = passed ? "CANVAS-CONFORMANCE-PASS" :
    `CANVAS-CONFORMANCE-FAIL:${outside}:${inside}:${scaled}:${erased}:` +
      `${large.width}x${large.height}`;
})();
