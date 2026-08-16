(() => {
  const keys = Array.from({length: 32}, (_, index) => `operation${index}`);
  let accumulator = 0x13579bdf;
  const iterations = 500000;
  for (globalThis.vmIterations = 0;
       globalThis.vmIterations < iterations;
       globalThis.vmIterations++) {
    const helpers = {};
    for (let index = 0; index < keys.length; index++) {
      const salt = index + 1;
      helpers[keys[index]] = function(left, right) {
        return ((left ^ right) + salt) | 0;
      };
    }
    const selected = helpers[keys[vmIterations & 31]].bind(null);
    accumulator = selected(accumulator, vmIterations);
  }
  globalThis.pocSummary = `${vmIterations}:${accumulator}`;
})();
