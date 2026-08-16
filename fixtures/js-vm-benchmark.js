(() => {
  const operations = {
    add: (left, right) => (left + right) | 0,
    xor: (left, right) => left ^ right,
    subtract: (left, right) => (left - right) | 0,
    multiply: (left, right) => Math.imul(left, right),
    rotate: (left, right) => (left << (right & 31)) |
      (left >>> (32 - (right & 31))),
    mask: (left, right) => left & right,
  };
  const names = Object.keys(operations);
  const program = new Uint8Array(4096);
  for (let index = 0; index < program.length; index++) {
    program[index] = (Math.imul(index, 73) + 19) & 255;
  }
  let accumulator = 0x13579bdf;
  let programCounter = 0;
  const iterations = 50000000;
  for (globalThis.vmIterations = 0;
       globalThis.vmIterations < iterations;
       globalThis.vmIterations++) {
    const opcode = program[programCounter++ & 4095];
    const operand = program[programCounter++ & 4095] | 1;
    const operation = operations[names[opcode % names.length]];
    accumulator = operation(accumulator, operand);
  }
  globalThis.pocSummary = `${vmIterations}:${accumulator}`;
})();
