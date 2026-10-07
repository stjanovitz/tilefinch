// Long-soak companion. The game's JavaScript phase clocks add work and are
// not gameplay: their 180-frame profile and report run before the window,
// and the window itself runs with the clocks off.
const d = __treadlineDebug;
if (__tfMark === 'deployed') {
  globalThis.__treadlineLongSoakDeployed = true;
  return 'TREADLINE-LONG-SOAK-DEPLOYED';
}
if (__tfMark === 'profile-start') {
  const mode = d.snapshot().mode;
  if (mode !== 'playing') throw new Error('Deploy did not start the long soak: ' + mode);
  __tilefinchStartInputProfile();
  return 'TREADLINE-LONG-SOAK-PROFILE-START';
}
if (__tfMark === 'profile-report') {
  const report = String(globalThis.__treadlinePhaseReport || globalThis.pocSummary || '');
  return report.startsWith('TREADLINE-JS-PHASES') ? report.replace(/\n/g, ' | ')
    : 'TREADLINE-LONG-SOAK-PROFILE-PENDING samples=' + d.aiProfile().samples;
}
// The measurement reset re-arms the clocks; keep this boundary work minimal.
if (__tfMark === 'webgl-measure-start') {
  d.stopInputProfile();
  return 'TREADLINE-LONG-SOAK-CLOCKS-OFF';
}
return 'TREADLINE-LONG-SOAK-MARK';
