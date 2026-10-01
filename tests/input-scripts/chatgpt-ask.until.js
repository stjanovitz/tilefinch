// First wait: composer usable. Later waits: actual assistant content.
// Live marks can advance in the supervisor before the page diagnostic runs,
// so __tfMark is not a reliable phase boundary. Latch the first success;
// a replacement conversation document is recognized by its user message.
// DOM content is not proof of panel presentation; inspect the frame too.
if (globalThis.__tfE2EComposerReached || document.querySelector('[data-message-role="user"]')) {
  var bot = document.querySelector('[data-message-role="assistant"]');
  // Attribution headings and recovery/status text are not an answer.
  var content = bot && bot.querySelector('[data-assistant-markdown]');
  return !!content && !bot.hidden && content.textContent.trim().length > 0;
}
var ready = document.documentElement.dataset.octaneHomeBehaviorReady === '1';
if (ready) globalThis.__tfE2EComposerReached = true;
return ready;
