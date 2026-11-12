// JR-22: offsetTop and offsetLeft are document positions, not viewport ones:
// the classic offsetParent loop gives the same answer before and after the
// page scrolls, body.offsetTop is its margin, and offsetWidth/offsetHeight
// are integers.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};
const $ = (id) => document.getElementById(id);
const pageY = (el) => { let y = 0; for (let at = el; at; at = at.offsetParent) y += at.offsetTop; return y; };
const target = $("target"), fixed = $("fixed"), odd = $("odd");
const before = { body: document.body.offsetTop, html: document.documentElement.offsetTop,
  y: pageY(target), top: target.offsetTop, fixed: fixed.offsetTop };
scrollTo({ top: 300, behavior: "instant" });
const scrolled = scrollY;
check("page scrolled", () => scrolled > 0);
check("body.offsetTop is its margin", () => before.body === 8 && document.body.offsetTop === 8);
check("html.offsetTop is 0", () => before.html === 0 && document.documentElement.offsetTop === 0);
check("offset loop ignores scrolling (" + before.y + " -> " + pageY(target) + ")",
  () => pageY(target) === before.y && before.y === 8 + 600);
check("offsetTop against body", () => target.offsetTop === before.top && before.top === 600);
check("fixed offsetTop is its viewport position", () => before.fixed === 20 && fixed.offsetTop === 20);
check("offset sizes are integers (" + odd.offsetWidth + "x" + odd.offsetHeight + ")",
  () => Number.isInteger(odd.offsetWidth) && Number.isInteger(odd.offsetHeight)
    && odd.offsetWidth === 101 && odd.offsetHeight === 50);
scrollTo({ top: 0, behavior: "instant" });
globalThis.pocSummary = failures.length
  ? "FAIL " + failures.join(" | ") + " [scrollY=" + scrolled + "]" : "OK";
