"use strict";

/* Treadline's index.html: a game script the browser skips or cannot load
   (its element gets an error event) must end the "Starting..." wait with a
   message and give queued Page controls back, as docs/GAME_PROFILE.md
   advises. Runs the page's own inline boot script against a small DOM. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const html = fs.readFileSync(path.join(__dirname,
  "../examples/treadline-arena/index.html"), "utf8");
const inline = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)]
  .map((match) => match[1]).find((text) => text.includes("__treadlineDeployQueued"));
assert(inline, "index.html has the boot script");
const sources = [...html.matchAll(/<script defer src="([^"]+)"><\/script>/g)]
  .map((match) => match[1]);
assert(sources.includes("game.js") && sources.length >= 7, "deferred game scripts");

function page({ready = false} = {}) {
  const listeners = new Map();
  const element = (id) => ({
    id, textContent: "", disabled: false, attributes: new Map(),
    setAttribute(name, value) { this.attributes.set(name, String(value)); },
    removeAttribute(name) { this.attributes.delete(name); },
    addEventListener(type, handler) { listeners.set(`${id}:${type}`, handler); },
  });
  const elements = {play: element("play"), message: element("message"),
    "game-shell": element("game-shell")};
  elements.play.textContent = "Deploy";
  const scripts = sources.map((source) => element("script:" + source));
  const calls = {fullscreen: 0, exits: 0};
  const document = {
    fullscreenElement: null,
    getElementById: (id) => elements[id] || null,
    querySelectorAll: (selector) => selector === "script[src]" ? scripts : [],
    exitFullscreen() { calls.exits++; this.fullscreenElement = null; return Promise.resolve(); },
  };
  const context = vm.createContext({document, navigator: {tilefinch: {
    requestPageControls(target) {
      calls.fullscreen++; document.fullscreenElement = target; return Promise.resolve();
    }}}, Promise});
  context.globalThis = context;
  context.__treadlineBootReady = ready;
  vm.runInContext(inline, context);
  const fire = (key) => listeners.get(key)();
  return {context, elements, calls, fire, document};
}

// A Deploy pressed before the scripts are ready queues and claims controls.
{
  const p = page();
  p.fire("play:click");
  assert.equal(p.elements.play.textContent, "Starting...");
  assert.equal(p.calls.fullscreen, 1);
  // game.js is skipped: its element's error event ends the wait.
  p.fire("script:game.js:error");
  assert.equal(p.context.__treadlineBootFailed, true);
  assert.equal(p.elements.play.textContent, "Unavailable");
  assert.equal(p.elements.play.disabled, true);
  assert(!p.elements.play.attributes.has("aria-busy"));
  assert.match(p.elements.message.textContent, /could not load/);
  assert.equal(p.calls.exits, 1, "Page controls go back to the browser");
  // A later press no longer claims controls or shows "Starting...".
  p.fire("play:click");
  assert.equal(p.calls.fullscreen, 1);
  assert.equal(p.elements.play.textContent, "Unavailable");
}

// Every deferred game script is watched, not only game.js.
for (const source of sources) {
  const p = page();
  p.fire(`script:${source}:error`);
  assert.equal(p.context.__treadlineBootFailed, true, source);
}

// An error after the game is running changes nothing.
{
  const p = page({ready: true});
  p.fire("script:game.js:error");
  assert.equal(p.context.__treadlineBootFailed, undefined);
  assert.equal(p.elements.play.textContent, "Deploy");
}
console.log("Treadline boot failure: ok");
