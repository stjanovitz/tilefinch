// The page's own document has replaceChildren, with the DOM standard's
// pre-insert checks (at most one element child and one doctype, the
// doctype first, nothing but elements, comments, processing instructions
// and doctypes). Rows not marked otherwise are Chromium's (measured
// 2026-11-10 in the built-in browser pane, Chrome 156).
const failures = [];
const check = (name, read) => {
  try { if (!read()) failures.push(name); }
  catch (error) { failures.push(name + ": " + error); }
};
const childNames = () => Array.from(document.childNodes, (node) => node.nodeName).join(",");
const root = document.documentElement, doctype = document.doctype;
const outcome = (run) => {
  try { run(); return "ok " + childNames(); }
  catch (error) { return error.name + " " + childNames(); }
};
check("replaceChildren is a Document member", () =>
  typeof document.replaceChildren === "function"
    && document.replaceChildren === Document.prototype.replaceChildren
    && !Object.hasOwn(document, "replaceChildren"));
const refused = "HierarchyRequestError html,HTML";
for (const [name, run] of [
  ["text", () => document.replaceChildren(document.createTextNode("x"))],
  ["string", () => document.replaceChildren("s")],
  ["new root", () => document.replaceChildren(document.createElement("html"))],
  ["own root", () => document.replaceChildren(root)],
  ["element, element",
    () => document.replaceChildren(document.createElement("a"), document.createElement("b"))],
  ["own doctype", () => document.replaceChildren(doctype)],
]) {
  check("refused replaceChildren(" + name + ")", () => outcome(run) === refused
    && document.documentElement === root && document.doctype === doctype);
}
check("replaceChildren(doctype, root) keeps both", () =>
  outcome(() => document.replaceChildren(doctype, root)) === "ok html,HTML"
    && document.documentElement === root && document.body !== null
    && document.getElementById("host") !== null);
check("documents made by script keep their own replaceChildren", () => {
  const made = document.implementation.createHTMLDocument("t");
  made.replaceChildren(made.createComment("c"));
  return made.childNodes.length === 1 && made.firstChild.nodeType === Node.COMMENT_NODE;
});
// Not as Chromium: the page's document takes no new children (Chromium
// would leave html,#comment and #comment here), so these refuse and keep
// the document as it was.
check("replaceChildren(root, comment) refuses: no new children", () =>
  outcome(() => document.replaceChildren(root, document.createComment("c"))) === refused);
check("replaceChildren(comment) refuses: no new children", () =>
  outcome(() => document.replaceChildren(document.createComment("c"))) === refused);
// Several arguments leave their parents before the check, so a refused
// call still takes the doctype out (Chromium's order).
check("refused replaceChildren(own doctype, text) takes the doctype out", () =>
  outcome(() => document.replaceChildren(doctype, document.createTextNode("x")))
    === "HierarchyRequestError HTML"
    && document.doctype === null && doctype.parentNode === null
    && document.documentElement === root);
// Repeated arguments act as one fragment holding the root once:
// append(root, root) and prepend(root, root) leave the document as it was
// (the doctype is gone by now), append(root) alone would give it a second
// element.
for (const [name, run] of [["append", () => document.append(root, root)],
  ["prepend", () => document.prepend(root, root)]]) {
  check(name + "(root, root) changes nothing", () => {
    const before = childNames();
    return outcome(run) === "ok " + before && document.documentElement === root
      && root.isConnected;
  });
}
check("refused append(root)", () => {
  const before = childNames();
  return outcome(() => document.append(root)) === "HierarchyRequestError " + before
    && document.documentElement === root;
});
check("refused append(other document's root, text) takes it out", () => {
  const other = document.implementation.createHTMLDocument("u"), otherRoot = other.documentElement,
    before = childNames();
  return outcome(() => document.append(otherRoot, "x")) === "HierarchyRequestError " + before
    && document.documentElement === root
    && otherRoot.parentNode === null && other.documentElement === null
    && Array.from(other.childNodes, (node) => node.nodeName).join(",") === "html";
});
// Last, as it empties the page: no arguments removes every child.
check("replaceChildren() empties the document", () =>
  outcome(() => document.replaceChildren()) === "ok "
    && document.documentElement === null && root.parentNode === null
    && !root.isConnected);
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
