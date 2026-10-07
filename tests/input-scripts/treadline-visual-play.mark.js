// treadline-visual-play companion: open the scene named by the package URL.
// The qualification=visual URL gives it __treadlineDebug (qualification.js).
const d = globalThis.__treadlineDebug, C = d && d.campaign, M = C && C.menu;
const href = String(location.href || '');
if (__tfMark === 'open') {
  if (!globalThis.__treadlineBootReady || !M) return 'TREADLINE-VISUAL-NOT-READY';
  // Qualification runs keep the killcam off; visual review shows real play.
  d.enableKillcam(true);
  if (href.includes('visual=range')) {
    M.toMain(); M.show('practice');
  } else {
    const found = /[?&]mission=(\d+)/.exec(href), at = found ? Number(found[1]) : 0;
    C.save.d = 1; C.writeSave();
    M.toMain(); M.select(at); M.show('briefing'); M.skipRadio();
  }
  const first = document.querySelector('#menu button');
  if (first) first.focus();
  globalThis.__treadlineVisualOpened = true;
  return 'TREADLINE-VISUAL-OPEN ' + (document.activeElement && document.activeElement.textContent);
}
if (__tfMark === 'deployed') {
  globalThis.__treadlineVisualDeployed = true;
  return 'TREADLINE-VISUAL-DEPLOYED';
}
const s = C.bridge().state;
return 'TREADLINE-VISUAL ' + __tfMark + ' mode=' + s.mode + ' arena=' + s.arena + ' gameMode=' + s.gameMode;
