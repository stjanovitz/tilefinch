// JR-02 / JR-04: createHTMLDocument and DOMParser documents are whole
// documents whose elements are real ones: selectors, element queries,
// innerHTML both ways, head and root attributes, title; SVG parses and other
// XML reports a parsererror instead of an empty root.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};

const created = document.implementation.createHTMLDocument("Made");
check("created title", () => created.title === "Made"
  && created.head.querySelector("title") !== null);
check("created shape", () => created.documentElement.localName === "html"
  && created.documentElement.firstElementChild === created.head
  && created.head.nextElementSibling === created.body
  && created.children.length === 1);
// jQuery.parseHTML: a base element, then the markup through body.innerHTML.
check("jquery base", () => {
  const base = created.createElement("base");
  base.href = location.href;
  created.head.appendChild(base);
  return created.head.lastElementChild === base;
});
check("innerHTML set and get", () => {
  created.body.innerHTML = '<div class="c" id="i"><span data-a="1">s</span></div>';
  return created.body.innerHTML === '<div class="c" id="i"><span data-a="1">s</span></div>'
    && created.body.childNodes.length === 1;
});
check("document selectors", () => created.querySelector(".c")?.id === "i"
  && created.querySelectorAll("div > span[data-a='1']").length === 1
  && created.getElementById("i") === created.querySelector("#i"));
check("element queries", () => {
  const box = created.getElementById("i");
  return box.querySelector("span")?.textContent === "s"
    && box.matches("div.c") && box.querySelector("span").closest(".c") === box;
});
check("created owner", () => created.createElement("p").ownerDocument === created
  && created.body.ownerDocument === created);
check("adopted into page", () => {
  const moved = created.getElementById("i");
  document.getElementById("host").appendChild(moved);
  return moved.ownerDocument === document && moved.isConnected
    && created.querySelector(".c") === null;
});

const html = new DOMParser().parseFromString(
  '<!doctype html><html lang="fr" class="k"><head><meta name="a" content="tok">'
  + "<title>Parsed</title><link rel=\"stylesheet\" href=\"a.css\"></head>"
  + '<body class="page-a" data-x="1"><div class="item">one</div>'
  + '<div class="item">two</div><form name="f"></form></body></html>', "text/html");
check("parsed head", () => html.head.children.length === 3
  && html.head.querySelector("meta[name=a]")?.getAttribute("content") === "tok");
check("parsed title", () => html.title === "Parsed");
check("parsed root attributes", () => html.documentElement.lang === "fr"
  && html.documentElement.className === "k");
check("parsed body attributes", () => html.body.className === "page-a"
  && html.body.getAttribute("data-x") === "1");
check("parsed queries", () => html.querySelectorAll(".item").length === 2
  && html.querySelector(".item").textContent === "one"
  && html.forms.length === 1);
check("parsed doctype", () => html.doctype?.name === "html"
  && html.compatMode === "CSS1Compat" && html.contentType === "text/html");
check("quirks without doctype", () => new DOMParser()
  .parseFromString("<p>x</p>", "text/html").compatMode === "BackCompat");

const svg = new DOMParser().parseFromString(
  '<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">'
  + '<path d="M0 0h4"/><linearGradient id="g"/></svg>', "image/svg+xml");
check("svg root", () => svg.documentElement.localName === "svg"
  && svg.documentElement.namespaceURI === "http://www.w3.org/2000/svg"
  && svg.documentElement.getAttribute("viewBox") === "0 0 24 24");
check("svg children", () => svg.documentElement.children.length === 2
  && svg.documentElement.lastElementChild.tagName === "linearGradient");

const rss = new DOMParser().parseFromString(
  '<?xml version="1.0"?><rss><channel><item><title>A</title></item></channel></rss>',
  "application/xml");
check("xml parsererror", () => rss.documentElement.localName === "parsererror"
  && rss.getElementsByTagName("parsererror").length === 1);

// Mutating a secondary document's own children follows the DOM standard's
// pre-insertion and replace checks, run on everything the call inserts
// before anything moves. Expected rows are Chromium 152's (measured
// 2026-11-10 on createHTMLDocument): a refused call throws
// HierarchyRequestError and leaves the children as they were, and several
// arguments act as one fragment, so arguments that are already children
// leave first.
const childNames = (doc) => Array.from(doc.childNodes, (node) => node.nodeName).join(",");
const mutation = (name, run, want, error = "") => check("mutation " + name, () => {
  const doc = document.implementation.createHTMLDocument("t");
  let thrown = "";
  try { run(doc, (n) => doc.implementation.createDocumentType(n, "", "")); }
  catch (caught) { thrown = caught.name; }
  return thrown === error && childNames(doc) === want;
});
const refused = (name, run, want = "html,HTML") =>
  mutation(name, run, want, "HierarchyRequestError");
const noRoot = (doc) => doc.removeChild(doc.documentElement);
const noDoctype = (doc) => doc.removeChild(doc.doctype);
const fragment = (doc, ...nodes) => {
  const made = doc.createDocumentFragment();
  made.append(...nodes);
  return made;
};
refused("replaceChildren(new root)", (d) => d.replaceChildren(d.createElement("a")));
refused("replaceChildren(text)", (d) => d.replaceChildren(d.createTextNode("x")));
refused("replaceChildren(own root)", (d) => d.replaceChildren(d.documentElement));
refused("replaceChildren(el, el)",
  (d) => d.replaceChildren(d.createElement("a"), d.createElement("b")));
refused("replaceChildren(doctype, new root)",
  (d, type) => d.replaceChildren(type("html"), d.createElement("a")));
refused("appendChild(own root)", (d) => d.appendChild(d.documentElement));
refused("append(comment, text)",
  (d) => d.append(d.createComment("c"), d.createTextNode("x")));
refused("prepend(comment, text)",
  (d) => d.prepend(d.createComment("c"), d.createTextNode("x")));
mutation("replaceChild(root, root)",
  (d) => d.replaceChild(d.documentElement, d.documentElement), "html,HTML");
check("mutation xml replaceChildren(el)", () => {
  const xml = document.implementation.createDocument(null, null),
    root = xml.createElement("r");
  xml.replaceChildren(root);
  return xml.childNodes.length === 1 && xml.documentElement === root;
});
check("mutation refused on a parsed document", () => {
  const parsed = new DOMParser().parseFromString(
    "<!doctype html><html><body><p>keep</p></body></html>", "text/html");
  const root = parsed.documentElement, type = parsed.doctype;
  let thrown = "";
  try { parsed.replaceChildren(parsed.createTextNode("x")); }
  catch (caught) { thrown = caught.name; }
  return thrown === "HierarchyRequestError" && parsed.documentElement === root
    && parsed.doctype === type && parsed.querySelector("p")?.textContent === "keep";
});
// Further rows measured in the same browser.
mutation("replaceChildren(own root, comment)",
  (d) => d.replaceChildren(d.documentElement, d.createComment("c")), "HTML,#comment");
mutation("replaceChildren(comment, own root)",
  (d) => d.replaceChildren(d.createComment("c"), d.documentElement), "#comment,HTML");
mutation("replaceChildren(own doctype, own root)",
  (d) => d.replaceChildren(d.doctype, d.documentElement), "html,HTML");
mutation("replaceChildren()", (d) => d.replaceChildren(), "");
mutation("replaceChildren(comment)",
  (d) => d.replaceChildren(d.createComment("c")), "#comment");
mutation("empty replaceChildren(doctype, el)", (d, type) => {
  d.replaceChildren();
  d.replaceChildren(type("html"), d.createElement("a"));
}, "html,A");
mutation("no root replaceChildren(fragment(el))",
  (d) => { noRoot(d); d.replaceChildren(fragment(d, d.createElement("a"))); }, "A");
refused("replaceChildren(string)", (d) => d.replaceChildren("s"));
refused("replaceChildren(el, string)",
  (d) => d.replaceChildren(d.createElement("a"), "s"));
refused("replaceChildren(fragment(el), comment)",
  (d) => d.replaceChildren(fragment(d, d.createElement("a")), d.createComment("c")));
refused("replaceChildren(new doctype)", (d, type) => d.replaceChildren(type("h")));
refused("replaceChildren(own doctype)", (d) => d.replaceChildren(d.doctype));
mutation("append(comment, own root)",
  (d) => d.append(d.createComment("c"), d.documentElement), "html,#comment,HTML");
mutation("append(own root, comment)",
  (d) => d.append(d.documentElement, d.createComment("c")), "html,HTML,#comment");
mutation("no root append(el)",
  (d) => { noRoot(d); d.append(d.createElement("a")); }, "html,A");
mutation("no root append(comment, el)",
  (d) => { noRoot(d); d.append(d.createComment("c"), d.createElement("a")); },
  "html,#comment,A");
mutation("no root append(fragment(el), comment)", (d) => {
  noRoot(d);
  d.append(fragment(d, d.createElement("a")), d.createComment("c"));
}, "html,A,#comment");
mutation("prepend(comment)",
  (d) => d.prepend(d.createComment("c")), "#comment,html,HTML");
mutation("prepend(own first child)", (d) => {
  d.prepend(d.createComment("c"));
  d.prepend(d.firstChild);
}, "#comment,html,HTML");
mutation("no doctype prepend(doctype, comment)",
  (d, type) => { noDoctype(d); d.prepend(type("h"), d.createComment("c")); },
  "h,#comment,HTML");
refused("append(string)", (d) => d.append("s"));
refused("append(fragment(comment, text))",
  (d) => d.append(fragment(d, d.createComment("c"), d.createTextNode("x"))));
refused("empty append(el, doctype)", (d, type) => {
  d.replaceChildren();
  d.append(d.createElement("a"), type("html"));
}, "");
refused("empty append(doctype, doctype)", (d, type) => {
  d.replaceChildren();
  d.append(type("a"), type("b"));
}, "");
refused("no doctype append(doctype, comment)",
  (d, type) => { noDoctype(d); d.append(type("h"), d.createComment("c")); }, "HTML");
refused("no root prepend(el)",
  (d) => { noRoot(d); d.prepend(d.createElement("a")); }, "html");
refused("no root prepend(comment, el)",
  (d) => { noRoot(d); d.prepend(d.createComment("c"), d.createElement("a")); }, "html");
refused("no root insertBefore(el, doctype)",
  (d) => { noRoot(d); d.insertBefore(d.createElement("a"), d.doctype); }, "html");
refused("insertBefore(el, comment before doctype)", (d) => {
  noRoot(d);
  const comment = d.insertBefore(d.createComment("c"), d.doctype);
  d.insertBefore(d.createElement("a"), comment);
}, "#comment,html");
refused("no doctype appendChild(doctype)",
  (d, type) => { noDoctype(d); d.appendChild(type("h")); }, "HTML");
mutation("no doctype insertBefore(doctype, root)",
  (d, type) => { noDoctype(d); d.insertBefore(type("h"), d.documentElement); }, "h,HTML");
refused("appendChild(attribute)", (d) => d.appendChild(d.createAttribute("a")));
refused("appendChild(document)",
  (d) => d.appendChild(document.implementation.createHTMLDocument("u")));
mutation("appendChild(string)", (d) => d.appendChild("s"), "html,HTML", "TypeError");
mutation("replaceChild(doctype, doctype)",
  (d, type) => d.replaceChild(type("h"), d.doctype), "h,HTML");
mutation("replaceChild(fragment(el), root)",
  (d) => d.replaceChild(fragment(d, d.createElement("a")), d.documentElement), "html,A");
refused("replaceChild(el, doctype)", (d) => d.replaceChild(d.createElement("a"), d.doctype));
refused("replaceChild(doctype, root)",
  (d, type) => d.replaceChild(type("h"), d.documentElement));
refused("replaceChild(own doctype, root)",
  (d) => d.replaceChild(d.doctype, d.documentElement));
refused("replaceChild(fragment(el), doctype)",
  (d) => d.replaceChild(fragment(d, d.createElement("a")), d.doctype));
refused("no doctype replaceChild(doctype, comment after root)", (d, type) => {
  noDoctype(d);
  d.append(d.createComment("c"));
  d.replaceChild(type("h"), d.lastChild);
}, "HTML,#comment");
// With several arguments Chromium takes each one out of its parent (and
// empties a fragment argument) before it checks them, so a refused call
// can still lose the root or take a node from another parent.
refused("append(own root, text)",
  (d) => d.append(d.documentElement, d.createTextNode("x")), "html");
refused("replaceChildren(own root, own doctype)",
  (d) => d.replaceChildren(d.documentElement, d.doctype), "");
refused("append(attribute, own root)",
  (d) => d.append(d.createAttribute("a"), d.documentElement), "html");
refused("prepend(own doctype, own root, text)",
  (d) => d.prepend(d.doctype, d.documentElement, d.createTextNode("x")), "");
refused("append(own root)", (d) => d.append(d.documentElement));
check("mutation refused takes from other parents", () => {
  const doc = document.implementation.createHTMLDocument("t"),
    parent = doc.createElement("p"), child = parent.appendChild(doc.createElement("i")),
    made = doc.createDocumentFragment(), inner = made.appendChild(doc.createElement("b"));
  let thrown = "";
  try { doc.append(child, made, doc.createTextNode("x")); } catch (caught) { thrown = caught.name; }
  return thrown === "HierarchyRequestError" && child.parentNode === null
    && parent.childNodes.length === 0 && made.childNodes.length === 0
    && inner.parentNode === null && childNames(doc) === "html,HTML";
});

// A refused call empties a fragment or shadow root argument even when it is
// the only argument, and takes another document's root out of that
// document; an accepted one moves it (Chromium 156).
const thrownBy = (run) => { try { run(); return ""; } catch (caught) { return caught.name; } };
check("mutation refused empties a lone fragment", () => {
  const doc = document.implementation.createHTMLDocument("t"),
    made = fragment(doc, doc.createElement("a"), doc.createElement("b"));
  return thrownBy(() => doc.append(made)) === "HierarchyRequestError"
    && made.childNodes.length === 0 && childNames(doc) === "html,HTML";
});
check("mutation refused empties a shadow root", () => {
  const doc = document.implementation.createHTMLDocument("t"),
    shadow = document.createElement("div").attachShadow({ mode: "open" });
  shadow.append(document.createElement("a"));
  return thrownBy(() => doc.append(shadow, doc.createElement("b"))) === "HierarchyRequestError"
    && shadow.childNodes.length === 0 && childNames(doc) === "html,HTML";
});
check("mutation refused takes another document's root", () => {
  const doc = document.implementation.createHTMLDocument("t"),
    other = document.implementation.createHTMLDocument("u"), root = other.documentElement;
  return thrownBy(() => doc.append(root, "x")) === "HierarchyRequestError"
    && childNames(doc) === "html,HTML" && childNames(other) === "html"
    && root.parentNode === null && other.documentElement === null;
});
check("mutation refused takes a parsed document's root", () => {
  const doc = document.implementation.createHTMLDocument("t"),
    parsed = new DOMParser().parseFromString("<p>x</p>", "text/html");
  return thrownBy(() => doc.append(parsed.documentElement, "x")) === "HierarchyRequestError"
    && childNames(doc) === "html,HTML" && childNames(parsed) === "";
});
check("mutation refused into new Document takes the root", () => {
  const made = new Document(), other = document.implementation.createHTMLDocument("u");
  return thrownBy(() => made.append(other.documentElement, "x")) === "HierarchyRequestError"
    && childNames(made) === "" && childNames(other) === "html";
});
check("mutation new Document append(other root) moves it", () => {
  const made = new Document(), other = document.implementation.createHTMLDocument("u"),
    root = other.documentElement;
  made.append(root);
  return made.documentElement === root && made.childNodes.length === 1
    && childNames(other) === "html" && other.documentElement === null
    && root.ownerDocument === made;
});

// Documents made by createDocument and new Document() use the same check,
// with the same Chromium 152 outcomes (measured 2026-11-10).
const localNames = (doc) =>
  Array.from(doc.childNodes, (node) => node.localName || node.nodeName).join(",");
for (const [kind, make] of [
  ["xml", () => document.implementation.createDocument(null, "root")],
  ["new Document", () => {
    const made = new Document();
    made.appendChild(made.createElement("root"));
    return made;
  }],
]) {
  const row = (name, run, want, error = "") => check(kind + " mutation " + name, () => {
    const doc = make();
    let thrown = "";
    try { run(doc); } catch (caught) { thrown = caught.name; }
    return thrown === error && localNames(doc) === want;
  });
  const no = (name, run, want = "root") => row(name, run, want, "HierarchyRequestError");
  const type = () => document.implementation.createDocumentType("r", "", "");
  no("replaceChildren(new root)", (d) => d.replaceChildren(d.createElement("n")));
  no("replaceChildren(el, el)",
    (d) => d.replaceChildren(d.createElement("a"), d.createElement("b")));
  no("replaceChildren(text)", (d) => d.replaceChildren(d.createTextNode("x")));
  no("replaceChildren(string)", (d) => d.replaceChildren("s"));
  row("replaceChildren(own root, comment)",
    (d) => d.replaceChildren(d.documentElement, d.createComment("c")), "root,#comment");
  row("replaceChildren()", (d) => d.replaceChildren(), "");
  no("append(comment, text)", (d) => d.append(d.createComment("c"), d.createTextNode("x")));
  no("prepend(comment, text)", (d) => d.prepend(d.createComment("c"), d.createTextNode("x")));
  no("appendChild(own root)", (d) => d.appendChild(d.documentElement));
  no("appendChild(text)", (d) => d.appendChild(d.createTextNode("x")));
  row("replaceChild(root, root)",
    (d) => d.replaceChild(d.documentElement, d.documentElement), "root");
  no("replaceChild(text, root)", (d) => d.replaceChild(d.createTextNode("x"), d.documentElement));
  row("replaceChild(el, root)", (d) => d.replaceChild(d.createElement("n"), d.documentElement), "n");
  no("append(doctype)", (d) => d.append(type()));
  row("prepend(doctype)", (d) => d.prepend(type()), "r,root");
  row("insertBefore(processing instruction, root)",
    (d) => d.insertBefore(d.createProcessingInstruction("p", "x"), d.documentElement), "p,root");
  // Chromium takes the root out before it throws.
  no("append(own root, text)", (d) => d.append(d.documentElement, d.createTextNode("x")), "");
}
check("xml mutation appendChild(cdata)", () => {
  const xml = document.implementation.createDocument(null, "root");
  let thrown = "";
  try { xml.appendChild(xml.createCDATASection("z")); } catch (caught) { thrown = caught.name; }
  return thrown === "HierarchyRequestError" && localNames(xml) === "root";
});
check("frame document mutations", () => {
  const frame = document.getElementById("host").appendChild(document.createElement("iframe")),
    doc = frame.contentDocument, before = localNames(doc);
  let first = "", second = "";
  try { doc.appendChild(doc.createTextNode("x")); } catch (caught) { first = caught.name; }
  try { doc.replaceChildren(doc.createElement("a"), doc.createElement("b")); }
  catch (caught) { second = caught.name; }
  frame.remove();
  return before !== "" && first === "HierarchyRequestError"
    && second === "HierarchyRequestError" && localNames(doc) === before;
});
// Element names in XML documents keep their case; createElement gives no
// namespace unless the document is XHTML (Chromium 152).
const xmlName = (element) => [element.nodeName, element.tagName, element.localName,
  element.namespaceURI, element.prefix].join("|");
const xhtml = "http://www.w3.org/1999/xhtml", svgNamespace = "http://www.w3.org/2000/svg";
const made = (namespace, name) => document.implementation.createDocument(namespace, name);
check("xml empty document", () => {
  const xml = made(null, null);
  return xml.childNodes.length === 0 && xml.contentType === "application/xml"
    && xml instanceof XMLDocument;
});
check("xml root name", () => xmlName(made(null, "Root").documentElement)
  === "Root|Root|Root||");
check("xml prefixed root", () => xmlName(made("urn:x", "p:Root").documentElement)
  === "p:Root|p:Root|Root|urn:x|p");
check("xhtml document", () => {
  const xml = made(xhtml, "html");
  return xml.contentType === "application/xhtml+xml"
    && xmlName(xml.documentElement) === "html|html|html|" + xhtml + "|"
    && xmlName(xml.createElement("Div")) === "Div|Div|Div|" + xhtml + "|";
});
check("svg document", () => {
  const xml = made(svgNamespace, "svg");
  return xml.contentType === "image/svg+xml"
    && xmlName(xml.documentElement) === "svg|svg|svg|" + svgNamespace + "|"
    && xml.documentElement instanceof SVGSVGElement
    && xmlName(xml.createElement("Rect")) === "Rect|Rect|Rect||";
});
check("xml root name errors", () => {
  const name = (namespace, qualified) => {
    try { made(namespace, qualified); return "none"; } catch (caught) { return caught.name; }
  };
  return name(null, "p:a") === "NamespaceError" && name(null, "1bad") === "InvalidCharacterError";
});
check("xml createElement", () => {
  const xml = made(null, null), foo = xml.createElement("Foo");
  return xmlName(foo) === "Foo|Foo|Foo||" && foo instanceof Element
    && !(foo instanceof HTMLElement) && xmlName(xml.createElement("div")) === "div|div|div||"
    && xmlName(xml.createElement("p:x")) === "p:x|p:x|p:x||";
});
check("xml createElementNS", () => {
  const xml = made(null, null);
  return xmlName(xml.createElementNS(null, "Foo")) === "Foo|Foo|Foo||"
    && xmlName(xml.createElementNS(xhtml, "Div")) === "Div|Div|Div|" + xhtml + "|"
    && xmlName(xml.createElementNS(svgNamespace, "Rect")) === "Rect|Rect|Rect|" + svgNamespace + "|"
    && xml.createElementNS(svgNamespace, "Rect") instanceof SVGElement
    && xmlName(xml.createElementNS("urn:x", "p:Foo")) === "p:Foo|p:Foo|Foo|urn:x|p";
});
check("html createElement still lower-cases", () => {
  const html = document.implementation.createHTMLDocument("");
  return xmlName(html.createElement("Foo")) === "FOO|FOO|foo|" + xhtml + "|";
});
check("xml getElementsByTagName is case-sensitive", () => {
  const xml = made(null, "Root");
  xml.documentElement.appendChild(xml.createElement("Kid"));
  return xml.getElementsByTagName("Kid").length === 1
    && xml.getElementsByTagName("kid").length === 0;
});
check("xml clone keeps the name", () => {
  const root = made("urn:x", "p:Root").documentElement.cloneNode(true);
  return xmlName(root) === "p:Root|p:Root|Root|urn:x|p";
});

check("page untouched", () => document.title === "probe"
  && document.querySelectorAll(".item").length === 0);

// Markup in a document without a browsing context never runs script.
globalThis.secondaryPwned = 0;
created.body.innerHTML = '<img src="https://img.test/x.png" onerror="secondaryPwned++">'
  + "<script>secondaryPwned++</scr" + "ipt>";
new DOMParser().parseFromString(
  '<img src="https://img.test/y.png" onerror="secondaryPwned++">', "text/html");
setTimeout(() => {
  expect("no script in secondary documents", globalThis.secondaryPwned === 0);
  globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
}, 0);
