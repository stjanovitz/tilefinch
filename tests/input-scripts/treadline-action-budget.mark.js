const d = __treadlineDebug;
if (__tfMark === 'configure') {
  d.selectMode(3); d.setAudioCurveMode(1); d.enableKillcam(true);
  return 'TREADLINE-ACTION-CONFIGURED';
}
if (__tfMark === 'quick-match') {
  const focused = document.activeElement;
  if (!focused || focused.id !== 'play')
    throw new Error('Quick Match did not focus Deploy: ' + (focused && (focused.id || focused.textContent)));
  return 'TREADLINE-ACTION-QUICK-MATCH';
}
if (__tfMark === 'heavy-setup') {
  if (d.snapshot().mode !== 'playing') throw new Error('Deploy did not reach gameplay: ' + d.snapshot().mode);
  d.setWave(5);
  const s = d.snapshot();
  if (!s.boss || s.enemies !== 5) throw new Error('Heavy boss setup missing');
  return 'TREADLINE-ACTION-HEAVY ' + JSON.stringify({boss: s.boss,
    enemies: s.enemies, seed: s.arenaSeed, controls: s.controls});
}
if (__tfMark === 'webgl-measure-start') {
  d.stopInputProfile();
  return 'TREADLINE-ACTION-CLOCKS-OFF';
}
if (__tfMark === 'post-play') {
  const s = d.snapshot();
  if (!s.killcamFrames) throw new Error('Killcam history not exercised');
  return 'TREADLINE-ACTION-COMPLETE ' + JSON.stringify({mode: s.mode,
    killcam: s.killcamFrames});
}
return 'TREADLINE-ACTION-MARK';
