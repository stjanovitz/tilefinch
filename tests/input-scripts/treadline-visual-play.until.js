// Three waits: the runtime is ready (before the companion opens the scene);
// the briefing or drill list is open with its primary button focused
// (before the press); play has begun (after it).
const C = globalThis.__treadlineDebug && globalThis.__treadlineDebug.campaign;
if (globalThis.__treadlineVisualOpened !== true)
  return globalThis.__treadlineBootReady === true && !!C && !!C.menu;
if (globalThis.__treadlineVisualDeployed !== true) {
  const focused = document.activeElement;
  return !!focused && focused.tagName === 'BUTTON' && /Deploy|Free range/.test(focused.textContent);
}
return C.bridge().state.mode === 'playing';
