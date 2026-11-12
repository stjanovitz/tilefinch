// JR-15: Range.toString() is the text between the boundary points, and a
// start set after the end (or an end before the start) collapses the
// range.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};
document.getElementById("host").innerHTML =
  '<p id="p">Hello <b>big</b> world</p><div id="c">a<!--x-->b<i>c</i></div>';
const p = document.getElementById("p"), c = document.getElementById("c");
check("across text nodes", () => {
  const range = document.createRange();
  range.setStart(p.firstChild, 2);
  range.setEnd(p.lastChild, 3);
  return range.toString() === "llo big wo";
});
check("element boundaries", () => {
  const range = document.createRange();
  range.setStart(p, 1);
  range.setEnd(p, 2);
  return range.toString() === "big";
});
check("whole contents", () => {
  const range = document.createRange();
  range.selectNodeContents(p);
  return range.toString() === "Hello big world";
});
check("comments are not text", () => {
  const range = document.createRange();
  range.selectNodeContents(c);
  return range.toString() === "abc";
});
check("same text node", () => {
  const range = document.createRange();
  range.setStart(p.firstChild, 1);
  range.setEnd(p.firstChild, 4);
  return range.toString() === "ell";
});
check("start after end collapses", () => {
  const range = document.createRange();
  range.setStart(p.firstChild, 0);
  range.setEnd(p.firstChild, 2);
  range.setStart(p.lastChild, 3);
  return range.collapsed && range.endContainer === p.lastChild
    && range.endOffset === 3 && range.toString() === "";
});
check("end before start collapses", () => {
  const range = document.createRange();
  range.setStart(p.lastChild, 2);
  range.setEnd(p.firstChild, 1);
  return range.collapsed && range.startContainer === p.firstChild
    && range.startOffset === 1;
});
check("another tree collapses", () => {
  const range = document.createRange();
  range.setStart(p.firstChild, 0);
  range.setEnd(p.firstChild, 2);
  const detached = document.createTextNode("zz");
  range.setStart(detached, 1);
  return range.collapsed && range.endContainer === detached;
});
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
