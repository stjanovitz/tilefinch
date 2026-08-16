(() => {
  const schedule = globalThis.requestAnimationFrame;
  let frameKey = NaN,
    frameTimestamp = 0;
  globalThis.requestAnimationFrame = (callback) => {
    if (typeof callback !== "function")
      throw new TypeError("callback must be a function");
    return schedule(() => {
      const key = Number(globalThis.__tilefinchNow) || 0;
      if (key !== frameKey) {
        frameKey = key;
        frameTimestamp = __tilefinchPerformanceSample(8);
      }
      callback(frameTimestamp);
    });
  };
  const timeline = {};
  Object.defineProperty(timeline, "currentTime", {
    get() {
      return __tilefinchPerformanceNow(6);
    },
    enumerable: true,
    configurable: true,
  });
  Object.defineProperty(document, "timeline", {
    get() {
      return timeline;
    },
    enumerable: true,
    configurable: true,
  });
})();
