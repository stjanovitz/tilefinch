(() => {
  const fail = message => { throw new Error(message); };
  const closures = [];
  for (let index = 0; index < 256; index++) {
    closures.push(function captured(left, right) {
      return left + right + index;
    });
  }

  const first = closures[0];
  const last = closures[closures.length - 1];
  if (first.length !== 2 || first.name !== 'captured') fail('metadata');
  if (first(2, 3) !== 5 || last(2, 3) !== 260) fail('capture');

  const lengthDescriptor = Object.getOwnPropertyDescriptor(first, 'length');
  const nameDescriptor = Object.getOwnPropertyDescriptor(first, 'name');
  const prototypeDescriptor =
    Object.getOwnPropertyDescriptor(first, 'prototype');
  if (!lengthDescriptor.configurable || lengthDescriptor.enumerable ||
      lengthDescriptor.writable) fail('length descriptor');
  if (!nameDescriptor.configurable || nameDescriptor.enumerable ||
      nameDescriptor.writable) fail('name descriptor');
  if (!prototypeDescriptor.writable || prototypeDescriptor.enumerable ||
      prototypeDescriptor.configurable) fail('prototype descriptor');
  if (first.prototype.constructor !== first) fail('lazy prototype');

  const instance = new last(1, 2);
  if (!(instance instanceof last)) fail('constructor');
  Object.defineProperty(first, 'name', {value: 'changed'});
  if (first.name !== 'changed' || last.name !== 'captured') fail('isolation');

  const arrow = value => value + 1;
  if (Object.hasOwn(arrow, 'prototype')) fail('arrow prototype');
  globalThis.pocSummary =
    `${closures.length}:${last(2, 3)}:${first.name}:${arrow(4)}`;
})();
