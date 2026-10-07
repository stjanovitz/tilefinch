// Before Deploy: the runtime is ready with Deploy focused. After: play has
// begun (forging, if any, is done).
if (globalThis.__treadlineLongSoakDeployed !== true) {
  const focused = document.activeElement;
  return globalThis.__treadlineBootReady === true && !!focused && focused.id === 'play';
}
return __treadlineDebug.snapshot().mode === 'playing';
