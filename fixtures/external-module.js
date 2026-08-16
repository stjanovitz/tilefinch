import { markModule } from './module-dependency.js';
markModule(globalThis.scriptOrder);
setTimeout(() => import('./dynamic-module.js').then(module => {
  module.markDynamic(globalThis.scriptOrder);
  globalThis.pocSummary += '|dynamic=yes';
}), 5);
