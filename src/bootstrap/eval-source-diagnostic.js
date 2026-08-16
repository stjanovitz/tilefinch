(() => {
  const nativeEval = globalThis.eval,
    sources = [];
  let total = 0;
  globalThis.__tilefinchEvalSources = sources;
  globalThis.eval = function (source) {
    const text = String(source);
    if (text.length <= 512 * 1024) {
      sources.push(text);
      total += text.length;
      while (sources.length > 8 || total > 768 * 1024)
        total -= sources.shift().length;
    }
    return nativeEval(text);
  };
})();
