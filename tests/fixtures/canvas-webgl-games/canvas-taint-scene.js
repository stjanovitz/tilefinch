/* One scene through every Canvas 2D path that can carry an image's pixels:
   drawImage (plain, scaled, transformed, clipped, composited), patterns
   (image, canvas, self, bitmap; fill, fillRect, clipped, text), canvas to
   canvas copies (including onto itself), ImageBitmaps, and every 2D op on
   a canvas after the image reached it, then a resize and the ops again.
   #x is a 16x16 image; the harness marks it cross-origin or not and sets
   globalThis.expectTainted to match. It compares the committed surfaces of
   both runs, so the scene itself never branches on taint, and checks here
   that every readback of an unclean canvas throws SecurityError while the
   clean run can read them. globalThis.taintSceneDone(summary) (if the
   harness defined it) receives the result. */
(() => {
  const image = document.getElementById("x"),
    canvases = [],
    make = (width = 24, height = 24) => {
      const canvas = document.createElement("canvas");
      canvas.width = width;
      canvas.height = height;
      document.body.appendChild(canvas);
      canvases[canvases.length] = canvas;
      return canvas;
    },
    ops = (context) => {
      context.fillStyle = "#3a6";
      context.fillRect(1, 1, 8, 8);
      context.fillStyle = "rgba(200,40,40,0.5)";
      context.fillRect(5, 5, 10, 6);
      context.save();
      context.beginPath();
      context.rect(2, 2, 12, 12);
      context.clip();
      context.fillStyle = "#08f";
      context.fillRect(0, 10, 24, 4);
      context.clearRect(3, 3, 3, 3);
      context.restore();
      context.clearRect(20, 20, 3, 3);
      context.save();
      context.rotate(0.2);
      context.clearRect(10, 0, 4, 4);
      context.restore();
      context.beginPath();
      context.moveTo(0, 0);
      context.lineTo(23, 17);
      context.strokeStyle = "#ff0";
      context.lineWidth = 2;
      context.stroke();
      const gradient = context.createLinearGradient(0, 0, 24, 0);
      gradient.addColorStop(0, "#000");
      gradient.addColorStop(1, "#f0f");
      context.fillStyle = gradient;
      context.beginPath();
      context.arc(16, 8, 5, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = "#fff";
      context.font = "10px sans-serif";
      context.fillText("Ab", 2, 22);
      context.strokeStyle = "#0ff";
      context.lineWidth = 1;
      context.strokeText("Q", 14, 22);
      context.shadowColor = "rgba(0,0,0,0.5)";
      context.shadowOffsetX = 2;
      context.shadowOffsetY = 2;
      context.fillStyle = "#f80";
      context.fillRect(14, 12, 5, 5);
      context.shadowColor = "rgba(0,0,0,0)";
      context.shadowOffsetX = 0;
      context.shadowOffsetY = 0;
      const data = new ImageData(4, 4);
      for (let i = 0; i < 64; i++) data.data[i] = (i * 37) & 255;
      context.putImageData(data, 18, 2);
      context.putImageData(data, 0, 14, 1, 1, 2, 2);
      context.globalCompositeOperation = "xor";
      context.fillStyle = "#123";
      context.fillRect(8, 8, 8, 8);
      context.globalCompositeOperation = "source-over";
      context.setTransform(1, 0.2, 0, 1, 0, 0);
      context.fillStyle = "#fff";
      context.fillRect(2, 16, 6, 6);
      context.setTransform(1, 0, 0, 1, 0, 0);
    };

  /* Only the image, so the harness can find its pixels in the surface. */
  const plain = make(16, 16);
  plain.getContext("2d").drawImage(image, 0, 0);

  const drawn = make(), d = drawn.getContext("2d");
  d.fillStyle = "#204060";
  d.fillRect(0, 0, 24, 24);
  d.drawImage(image, 2, 2);
  d.imageSmoothingEnabled = true;
  d.drawImage(image, 0, 0, 16, 16, 12, 12, 12, 12);
  d.globalAlpha = 0.5;
  d.globalCompositeOperation = "lighter";
  d.drawImage(image, 6, 0);
  d.globalAlpha = 1;
  d.globalCompositeOperation = "source-over";
  d.save();
  d.translate(12, 12);
  d.rotate(0.4);
  d.drawImage(image, -8, -8);
  d.restore();
  d.save();
  d.beginPath();
  d.arc(12, 12, 7, 0, Math.PI * 2);
  d.clip();
  d.drawImage(image, 4, 4, 20, 20);
  d.restore();

  const patterned = make(), p = patterned.getContext("2d"),
    repeat = p.createPattern(image, "repeat"),
    single = p.createPattern(image, "no-repeat");
  p.fillStyle = repeat;
  p.fillRect(0, 0, 24, 24);
  single.setTransform(new DOMMatrix([1, 0, 0, 1, 3, 5]));
  p.fillStyle = single;
  p.beginPath();
  p.arc(12, 12, 9, 0, Math.PI * 2);
  p.fill();
  p.save();
  p.beginPath();
  p.rect(2, 2, 10, 10);
  p.clip();
  p.fillStyle = p.createPattern(image, "repeat-x");
  p.fillRect(0, 0, 24, 24);
  p.restore();
  p.fillStyle = repeat;
  p.font = "12px sans-serif";
  p.fillText("Wx", 2, 20);
  p.globalAlpha = 0.6;
  p.globalCompositeOperation = "source-atop";
  p.fillRect(4, 4, 10, 10);
  p.globalAlpha = 1;
  p.globalCompositeOperation = "source-over";

  const copied = make(), c = copied.getContext("2d");
  c.fillStyle = "#fff";
  c.fillRect(0, 0, 24, 24);
  c.drawImage(drawn, 0, 0);
  c.drawImage(patterned, 4, 4, 12, 12);
  c.drawImage(copied, 6, 6);
  c.fillStyle = c.createPattern(drawn, "repeat");
  c.fillRect(14, 0, 10, 10);
  c.fillStyle = c.createPattern(copied, "repeat");
  c.fillRect(0, 14, 10, 10);

  const operated = make(), o = operated.getContext("2d");
  o.drawImage(image, 0, 0, 4, 4);
  ops(o);
  /* A resize clears the bitmap but keeps the canvas tainted (HTML). */
  operated.width = 20;
  ops(o);

  /* A canvas that only paints with a pattern made elsewhere. */
  const borrowed = make(), b = borrowed.getContext("2d");
  b.fillStyle = repeat;
  b.fillRect(0, 0, 24, 24);

  const tainted = globalThis.expectTainted === true, fails = [],
    threw = (run) => {
      try { run(); return false; }
      catch (error) { return !!error && error.name === "SecurityError"; }
    },
    readable = (canvas, label) => {
      const context = canvas.getContext("2d");
      if (threw(() => context.getImageData(0, 0, 1, 1)) !== tainted)
        fails.push(label + "-getImageData");
      if (threw(() => canvas.toDataURL()) !== tainted)
        fails.push(label + "-toDataURL");
      if (threw(() => canvas.toBlob(() => {})) !== tainted)
        fails.push(label + "-toBlob");
      if (!globalThis.__tilefinchCanvasReadbackState?.(canvas) !== tainted)
        fails.push(label + "-readback-hook");
    },
    /* webgl.js's texImage2D takes its pixels from the readback hook (a
       canvas) or the image source (an image): unclean, both refuse. */
    hidesPixels = (element) => {
      const source = __tilefinchCanvasImageSource(element.__handle);
      return !source || (source.sameOrigin === false && !("pixels" in source));
    };

  createImageBitmap(image).then((bitmap) => {
    const fromBitmap = make(), m = fromBitmap.getContext("2d");
    m.drawImage(bitmap, 1, 1);
    m.drawImage(bitmap, 0, 0, 8, 8, 12, 12, 12, 12);
    m.fillStyle = m.createPattern(bitmap, "repeat");
    m.fillRect(0, 18, 24, 6);
    return createImageBitmap(drawn, 2, 2, 10, 10, {});
  }).then((bitmap) => {
    const cropped = make(), k = cropped.getContext("2d");
    k.drawImage(bitmap, 3, 3);
    bitmap.close();
    for (const [index, canvas] of canvases.entries())
      if (canvas.getContext("2d")) readable(canvas, "c" + index);
    __tilefinchFlushCanvasSurfaces();
    __tilefinchFlushCanvasSurfaces();
    if (hidesPixels(image) !== tainted) fails.push("image-source");
    if (hidesPixels(drawn) !== tainted) fails.push("surface-source");
    const summary = fails.length === 0
      ? "TAINT-SCENE-PASS" : "TAINT-SCENE-FAIL:" + fails.join(",");
    if (typeof globalThis.taintSceneDone === "function")
      globalThis.taintSceneDone(summary);
    else globalThis.pocSummary = summary;
  }).catch((error) => {
    globalThis.pocSummary = "TAINT-SCENE-FAIL:threw:" + error;
  });
})();
