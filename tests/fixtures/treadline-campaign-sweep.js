/* Headless campaign difficulty sweep. A bot-league proxy drives the player
   tank with the existing AI at a chosen skill, so the numbers measure
   combat difficulty; objective-only missions (escort, demolition, salvage,
   bank-only targets) mostly time out because the proxy only hunts. */
(() => {
  const d = __treadlineDebug, C = d.campaign, B = C.bridge();
  const LIMIT = 240;
  let run = null;
  globalThis.__campaignSweep = {
    begin(mission, difficulty, proxy, seed) {
      C.loadSave(''); C.save.d = difficulty; C.save.f = 0; C.setRadio(2);
      d.beginLeague(seed, proxy, 0, 0, 0);
      B.state.difficultyChoice = proxy;
      if (mission >= 25) {
        // Reference rows: Quick Match Survival arena 1 and Convoy Escort,
        // played by the same proxy against bots at the column difficulty.
        C.menu.toMain(); B.state.gameMode = mission === 25 ? 0 : 2;
        B.state.difficultyChoice = difficulty;
        d.start();
        for (let id = 1; id < 6; id++) B.tanks[id].difficulty = difficulty;
        B.tanks[0].difficulty = proxy;
        run = {mission, difficulty, proxy, seed, quick: true, start: B.state.time};
        return true;
      }
      C.startMission(mission, false);
      const player = B.tanks[0];
      const dx = ((seed % 5) - 2) * .35, dz = (((seed / 5) | 0) % 3 - 1) * .35;
      if (!d.circleBlocked(player.x + dx, player.z + dz, player.collisionRadius))
        d.setTankPosition(0, player.x + dx, player.z + dz);
      run = {mission, difficulty, proxy, seed};
      return true;
    },
    step(frames) {
      for (let at = 0; at < frames; at++) {
        const mode = B.state.mode;
        const quickDone = run.quick && ((run.mission === 25 && B.state.arena > 0)
          || B.state.time - run.start >= LIMIT);
        if (mode === 'victory' || mode === 'game-over' || quickDone
            || (!run.quick && C.runtime.t >= LIMIT)) {
          d.stepSimulation(1);
          return true;
        }
        d.stepSimulation(1);
      }
      return false;
    },
    result() {
      const M = C.runtime;
      if (run.quick) {
        const won = (run.mission === 25 && B.state.arena > 0) || B.state.mode === 'victory';
        return {mission: run.mission === 25 ? 'QM-SURVIVAL-A1' : 'QM-CONVOY', difficulty: run.difficulty,
          proxy: run.proxy, seed: run.seed, won, end: won ? 'victory' : B.state.mode === 'game-over' ? 'game-over' : 'timeout',
          seconds: Math.round((B.state.time - run.start) * 10) / 10, deaths: 3 - B.state.lives, kills: B.state.kills, medals: 0};
      }
      const won = M.ended ? !!(M.result && M.result.won) : false;
      return {mission: C.missions[run.mission].id, difficulty: run.difficulty,
        proxy: run.proxy, seed: run.seed, won, end: M.ended ? B.state.mode : 'timeout',
        seconds: Math.round(M.t * 10) / 10, deaths: M.deaths, kills: M.kills,
        medals: M.result ? M.result.medals & 7 : 0};
    },
    finish() { d.finishLeague(); C.menu.toMain(); },
  };
  globalThis.pocSummary = 'TREADLINE-CAMPAIGN-SWEEP-READY';
})();
