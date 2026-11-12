// A parser-run script that removes the body: the load still completes, as
// in Chromium (measured 2026-11-10 in the built-in browser pane, Chrome
// 156). The parser goes on inserting into the removed body, so the markup
// after the script lands there and its script never runs; the document
// keeps html with its head only.
const failures = [];
const check = (name, read) => {
  try { if (!read()) failures.push(name); }
  catch (error) { failures.push(name + ": " + error); }
};
const removed = globalThis.removedBody;
check("the script saw no body after removing it", () => globalThis.afterRemove === "null");
check("document.body stays null", () => document.body === null);
check("html keeps its head only", () =>
  Array.from(document.documentElement.childNodes, (node) => node.nodeName).join(",") === "HEAD");
check("later markup lands in the removed body", () =>
  removed instanceof HTMLBodyElement && !removed.isConnected
    && removed.querySelector("#after")?.textContent === "after"
    && document.getElementById("after") === null);
check("a script in the removed body does not run", () => globalThis.late === undefined);
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
