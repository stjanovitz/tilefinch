/* Generated first-party replay: no captured packet or network input. */
(() => {
  const d = __treadlineDebug;
  function check(ok, label) { if (!ok) throw new Error(label); }
  const preferences = d.snapshot();
  document.getElementById('online-cancel').click();
  d.selectMode(0); d.selectClass(0); d.selectCamera(0);
  d.selectAimAssist(0); d.resetInputProbe(); d.start(); d.freezeBots(true);
  // Alternate two ordinary aim directions, retaining enough changed input
  // words to exercise import of a modest uncompressed replay.
  for (let frame = 0; frame < 240; frame++) {
    const left = (frame & 1) === 0;
    d.setKeyboard('aimLeft', left); d.setKeyboard('aimRight', !left);
    d.setKeyboard('aimUp', left); d.setKeyboard('aimDown', !left);
    d.stepSimulation(1);
  }
  d.replayStop(); d.resetInputProbe(); d.freezeBots(false);
  d.selectAimAssist(preferences.assist); d.selectCamera(preferences.cameraMode);
  const saved = d.replayState(), code = d.exportReplay();
  const split = code.indexOf('.', 4), prefix = code.slice(0, split + 1);
  const payload = code.slice(split + 1);
  check(saved.count === 240 && !saved.recording && !saved.truncated,
    'generated replay frame count');
  check(payload.length > 4096 && payload.length < 16384,
    'generated replay payload bound');
  function unchanged() {
    const state = d.replayState();
    return state.count === saved.count && state.digest === saved.digest
      && !state.active && !state.recording;
  }
  // This closure is compiled and retained before C imposes the temporary
  // headroom limit. It calls the game's actual public import function.
  globalThis.__treadlineReplayImportBounded = () => d.importReplay(code) && unchanged();
  globalThis.__treadlineReplayImportFormat = () => {
    const oldBytes = Uint8Array.from(atob(payload), ch => ch.charCodeAt(0));
    const oldView = new DataView(oldBytes.buffer);
    check(oldView.getFloat64(0, true) === 14, 'current replay version (wrapped turrets)');
    oldView.setFloat64(0, 13, true);
    let oldChecksum = 2166136261, oldBinary = '';
    for (let at = 0; at < oldBytes.length; at++) {
      oldChecksum = Math.imul(oldChecksum ^ oldBytes[at], 16777619) >>> 0;
      oldBinary += String.fromCharCode(oldBytes[at]);
    }
    check(!d.importReplay('TR1.' + oldChecksum.toString(16) + '.' + btoa(oldBinary))
      && unchanged(), 'old AI simulation version refuses valid-checksum replay');
    check(d.importReplay(' \t\n' + code + '\r\n ') && unchanged(), 'outer trim');
    let end = payload.length;
    while (payload.charCodeAt(end - 1) === 61) end--;
    check(d.importReplay(prefix + payload.slice(0, end)) && unchanged(), 'unpadded replay');
    for (const space of [' ', '\t', '\n', '\r', '\f'])
      check(!d.importReplay(prefix + payload.slice(0, 8) + space + payload.slice(8))
        && unchanged(), 'interior whitespace refusal');
    for (const invalid of [
      'tr1.' + code.slice(4), 'TR1..' + payload,
      'TR1.123456789.' + payload, 'TR1.ABCDEF.' + payload,
      prefix + payload + '===', prefix + '=' + payload,
      prefix + payload.slice(0, 8) + '=' + payload.slice(8),
      prefix + payload.slice(0, 8) + '-' + payload.slice(9),
      prefix + 'A', code.slice(0, -4) + 'AAAA'
    ]) check(!d.importReplay(invalid) && unchanged(), 'replay format refusal');
    check(d.exportReplay() === code, 'refusal preserves complete replay');
    check(d.importReplay(code) && unchanged(), 'valid retry after refusal');
    return true;
  };
  globalThis.pocSummary = 'TREADLINE-REPLAY-IMPORT-READY';
})();
