// Evaluated at every mark of chatgpt-ask.txt (see psp_app_input_script.c).
// The first mark records clicks and errors; later marks print them with
// the home client's readiness.
var g = globalThis;
function why(e) {
  return String(e && e.name) + ': ' + String(e && e.message).slice(0, 80) +
    ' @ ' + String(e && e.stack).split('\n').slice(1, 12)
      .map(function (l) { return l.replace(/https:\/\/chatgpt\.com\/unauth-mweb\/assets\//, '').trim(); })
      .join(' < ').slice(0, 900);
}
// ChatGPT's startup watchdog reloads once when the client has not called
// __webMobileStartupRecovery.complete() with its stylesheets settled 8 s
// after the watchdog script ran; its timeout mark records which was missing.
function boot() {
  var marks = [];
  try {
    marks = performance.getEntriesByName('web-mobile-startup-timeout', 'mark')
      .map(function (m) {
        var d = m.detail || {};
        return [Math.round(m.startTime), d.clientComplete, d.stylesheetSettled,
                d.policy];
      });
  } catch (e) { marks = ['x ' + e]; }
  var ds = document.documentElement.dataset, rec = window.__webMobileStartupRecovery;
  var links = [].slice.call(document.querySelectorAll(
    'link[data-web-mobile-app-stylesheet],link[data-web-mobile-shared-stylesheet]'))
    .map(function (l) { return l.rel + (l.sheet ? '+' : '-'); });
  return JSON.stringify({rec: typeof rec, ready: ds.octaneHomeBehaviorReady || '',
    conv: ds.octaneConversationBehaviorReady || '', links: links,
    uncaught: (g.__tilefinchUncaughtErrors || []).slice(-3).map(function (e) {
      return String(e).replace(/https:\/\/chatgpt\.com\/unauth-mweb\/assets\//g, '')
        .replace(/\s+/g, ' ').slice(0, 260);
    }),
    now: Math.round(performance.now()), timeout: marks});
}
if (!g.__tfClicks) {
  g.__tfClicks = [];
  g.__tfProbe = {errors: []};
  var probe = g.__tfProbe;
  window.addEventListener('error', function (e) {
    if (probe.errors.length < 4) probe.errors.push(why(e.error || e));
  });
  window.addEventListener('unhandledrejection', function (e) {
    if (probe.errors.length < 4) probe.errors.push('rej ' + why(e.reason));
  });
  document.addEventListener('click', function (e) {
    var t = e.target;
    g.__tfClicks.push({t: t && (t.tagName + (t.id ? '#' + t.id : '') +
      (t.className ? '.' + String(t.className).split(' ')[0] : ''))});
  }, true);
  return 'hooked ' + boot();
}
var dpu = window.__webMobileDeclarativePartialUpdates, snap = {};
try { snap = dpu && dpu.getPerformanceSnapshot ? dpu.getPerformanceSnapshot() : {}; } catch (e) {}
function box(sel) {
  var e = document.querySelector(sel);
  if (!e) return null;
  var r = e.getBoundingClientRect();
  return [Math.round(r.top), Math.round(r.height)];
}
var root = document.scrollingElement || document.documentElement;
return JSON.stringify({
  applied: snap.templatesApplied, stale: snap.presentationStaleTargetCount,
  scroll: [Math.round(root.scrollTop), root.scrollHeight, root.clientHeight, Math.round(scrollY)],
  user: box('[data-message-role="user"]'),
  bot: box('[data-message-role="assistant"]'),
  text: (document.querySelector('[data-message-role="assistant"]') || {}).textContent ? document.querySelector('[data-message-role="assistant"]').textContent.trim().slice(0, 60) : '',
  errors: g.__tfProbe && g.__tfProbe.errors && g.__tfProbe.errors.slice(0, 1),
  ready: [document.documentElement.dataset.octaneHomeBehaviorReady || '',
          document.documentElement.dataset.octaneConversationBehaviorReady || '']
});
