/* Only an idle handle can be cancelled through cancelIdleCallback, and
   timer/frame clearing must not cancel an idle handle either. */
(() => {
  pocSummary = "pending";
  const fired = [];
  const timeout = setTimeout(() => fired.push("timer"), 0);
  const idle = requestIdleCallback(() => fired.push("idle"));
  const cancelled = requestIdleCallback(() => fired.push("cancelled"));
  const result = cancelIdleCallback(cancelled);
  cancelIdleCallback(timeout);
  // The same kind of reserved task that advances pending derivations
  // (through the test realm's probe; pages cannot reserve one).
  let progressed = false;
  cancelIdleCallback(__tilefinchTestDomShared.scheduleReservedTask(
    () => { progressed = true; }, 0));
  clearTimeout(idle);
  clearInterval(idle);
  cancelAnimationFrame(idle);
  setTimeout(() => {
    pocSummary = progressed ? fired.join(",") + ":" + result : "reserved task lost";
  }, 8);
})();
