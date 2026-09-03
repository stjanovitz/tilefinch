(() => {
  const nativeEval = globalThis.eval,
    traceSource = typeof globalThis.__tilefinchTraceEvalSource === "function"
      ? globalThis.__tilefinchTraceEvalSource
      : null,
    sources = [];
  delete globalThis.__tilefinchTraceEvalSource;
  let total = 0;
  globalThis.__tilefinchEvalSources = sources;
  globalThis.eval = function (source) {
    const text = String(source);
    if (traceSource)
      traceSource(text);
    if (text.length <= 512 * 1024) {
      sources.push(text);
      total += text.length;
      while (sources.length > 8 || total > 768 * 1024)
        total -= sources.shift().length;
    }
    return nativeEval(text);
  };
})();
