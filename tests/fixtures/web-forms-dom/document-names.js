// Element and attribute names, and the kind of document each factory
// makes. Expected values are Chromium's (measured 2026-11-10 in the
// built-in browser pane, Chrome 156, matching the DOM standard).
const failures = [];
const check = (name, read) => {
  try { if (!read()) failures.push(name); }
  catch (error) { failures.push(name + ": " + error); }
};
const xhtml = "http://www.w3.org/1999/xhtml";
const describe = (element) => [element.nodeName, element.localName,
  element.namespaceURI, element.prefix].join("|");

// new Document() is an XML document (type "xml", content type
// application/xml); the template contents owner stays an HTML document.
check("new Document kind", () => {
  const made = new Document();
  return made.contentType === "application/xml" && made.childNodes.length === 0
    && made instanceof Document && !(made instanceof XMLDocument)
    && !(made instanceof HTMLDocument);
});
check("new Document createElement keeps case", () => {
  const made = new Document(), foo = made.createElement("Foo");
  return describe(foo) === "Foo|Foo||" && foo instanceof Element
    && !(foo instanceof HTMLElement) && describe(made.createElement("div")) === "div|div||";
});
check("new Document createAttribute keeps case",
  () => new Document().createAttribute("Foo").name === "Foo");
check("new Document getElementsByTagName is case-sensitive", () => {
  const made = new Document();
  made.appendChild(made.createElement("Root")).appendChild(made.createElement("Kid"));
  return made.getElementsByTagName("Kid").length === 1
    && made.getElementsByTagName("kid").length === 0;
});
check("new Document clone stays XML", () => {
  const made = new Document();
  made.appendChild(made.createElement("Root"));
  const clone = made.cloneNode(true);
  return clone.contentType === "application/xml" && !(clone instanceof HTMLDocument)
    && describe(clone.documentElement) === "Root|Root||";
});
check("template contents owner is HTML", () => {
  const owner = document.createElement("template").content.ownerDocument;
  return owner !== document && owner instanceof HTMLDocument
    && owner.contentType === "text/html"
    && describe(owner.createElement("Foo")) === "FOO|foo|" + xhtml + "|"
    && owner.createElement("Foo") instanceof HTMLElement;
});
check("template contents parse as HTML", () => {
  const template = document.createElement("template");
  template.innerHTML = "<P class=a>x</P>";
  const child = template.content.firstChild;
  return describe(child) === "P|p|" + xhtml + "|" && child instanceof HTMLParagraphElement;
});
check("page clone stays HTML", () => {
  const clone = document.cloneNode(false);
  return clone.contentType === "text/html"
    && describe(clone.createElement("Foo")) === "FOO|foo|" + xhtml + "|";
});

// createElementNS keeps the name's case on this (parser-made) document.
check("page createElementNS(null) keeps case", () => {
  const foo = document.createElementNS(null, "Foo");
  return describe(foo) === "Foo|Foo||" && foo.tagName === "Foo"
    && !(foo instanceof HTMLElement);
});
check("page createElementNS(other) keeps case",
  () => describe(document.createElementNS("urn:x", "p:Foo")) === "p:Foo|Foo|urn:x|p");
check("page createElementNS(html) keeps localName case", () => {
  const foo = document.createElementNS(xhtml, "Foo");
  return foo.localName === "Foo" && foo.tagName === "FOO" && foo.namespaceURI === xhtml;
});
check("page createElementNS(null) element keeps case in the tree", () => {
  const host = document.getElementById("host"),
    foo = host.appendChild(document.createElementNS(null, "Foo"));
  const kept = host.lastChild === foo && foo.localName === "Foo"
    && host.innerHTML === "<Foo></Foo>" && foo.cloneNode().localName === "Foo";
  foo.remove();
  return kept;
});
check("page createElementNS(null, prefixed) is a NamespaceError", () => {
  try { document.createElementNS(null, "p:Foo"); return false; }
  catch (error) { return error.name === "NamespaceError"; }
});
check("html document createElementNS(null) keeps case", () => {
  const doc = document.implementation.createHTMLDocument("");
  return describe(doc.createElementNS(null, "Foo")) === "Foo|Foo||";
});
check("parsed document createElementNS(null) keeps case", () => {
  const doc = new DOMParser().parseFromString("<p>x</p>", "text/html");
  return describe(doc.createElementNS(null, "Foo")) === "Foo|Foo||";
});
check("svg names keep their case", () => {
  const svg = "http://www.w3.org/2000/svg", host = document.getElementById("host");
  host.innerHTML = "<svg><linearGradient/></svg>";
  const parsed = host.firstChild.firstChild;
  host.textContent = "";
  return describe(document.createElementNS(svg, "linearGradient"))
      === "linearGradient|linearGradient|" + svg + "|"
    && describe(document.createElementNS(svg, "s:clipPath")) === "s:clipPath|clipPath|" + svg + "|s"
    && describe(parsed) === "linearGradient|linearGradient|" + svg + "|"
    && describe(parsed.cloneNode(true)) === "linearGradient|linearGradient|" + svg + "|";
});
// Lookups by an element's own name (Chromium 156). In an HTML document a
// type selector is lower-cased and must equal an HTML element's local
// name, but matches any other element case-insensitively;
// getElementsByTagName lower-cases the name for HTML elements only and
// otherwise compares the qualified name exactly.
const inHost = (build) => {
  const host = document.getElementById("host");
  host.textContent = "";
  build(host);
  return host;
};
check("svg elements are found by their own names", () => {
  const host = inHost((host) => { host.innerHTML = "<svg><defs><linearGradient id=g>" +
    "</linearGradient></defs><foreignObject id=f></foreignObject></svg>"; });
  const g = document.getElementById("g"), f = document.getElementById("f");
  const found = g.matches(g.localName) && f.matches(f.localName)
    && g.closest(g.localName) === g && host.querySelector(g.localName) === g
    && host.querySelector(f.localName) === f
    && host.querySelectorAll(g.tagName).length === 1
    && host.querySelector("lineargradient") === g && host.querySelector("LINEARGRADIENT") === g
    && g.matches("lineargradient") && f.matches("FOREIGNOBJECT")
    && host.getElementsByTagName(g.localName).length === 1
    && host.getElementsByTagName(f.tagName).length === 1
    && document.getElementsByTagName("linearGradient")[0] === g
    && host.getElementsByTagName("lineargradient").length === 0
    && host.getElementsByTagName("foreignobject").length === 0;
  host.textContent = "";
  return found;
});
check("createElementNS(null) elements are found by their own names", () => {
  let foo;
  const host = inHost((host) => { foo = host.appendChild(document.createElementNS(null, "Foo")); });
  const found = host.querySelector("Foo") === foo && foo.matches("Foo") && foo.matches("foo")
    && host.querySelector("FOO") === foo && foo.closest(foo.localName) === foo
    && host.getElementsByTagName("Foo").length === 1
    && host.getElementsByTagName("foo").length === 0;
  host.textContent = "";
  return found;
});
check("prefixed elements are found by their qualified names", () => {
  let p;
  const host = inHost((host) => { p = host.appendChild(document.createElementNS("urn:x", "p:Foo")); });
  const found = host.getElementsByTagName("p:Foo")[0] === p
    && host.getElementsByTagName(p.tagName).length === 1
    && host.getElementsByTagName("p:foo").length === 0
    && host.getElementsByTagName("Foo").length === 0
    && host.querySelector("Foo") === p && p.matches(p.localName)
    && host.querySelector("p\\:Foo") === null;
  host.textContent = "";
  return found;
});
check("live tag lists follow later elements", () => {
  const host = inHost(() => {});
  const list = host.getElementsByTagName("Foo"), before = list.length;
  host.appendChild(document.createElementNS(null, "Foo"));
  const after = list.length;
  host.textContent = "";
  return before === 0 && after === 1 && list.length === 0;
});
// An HTML element createElementNS names with ASCII upper case is an
// HTMLUnknownElement: not a script, not a div, found by no lower-case name.
check("createElementNS(html, DIV) is an unknown element", () => {
  let div;
  const host = inHost((host) => { div = host.appendChild(document.createElementNS(xhtml, "DIV")); });
  const found = String(div) === "[object HTMLUnknownElement]" && div.localName === "DIV"
    && div.tagName === "DIV" && !div.matches("div") && !div.matches("DIV")
    && host.querySelector("div") === null && host.getElementsByTagName("DIV").length === 0
    && host.getElementsByTagName("div").length === 0 && host.innerHTML === "<DIV></DIV>"
    && div.cloneNode().localName === "DIV" && String(div.cloneNode()) === String(div);
  host.textContent = "";
  return found;
});
check("createElementNS(html, SCRIPT) does not run", () => {
  const host = inHost((host) => {
    const control = document.createElement("script");
    control.textContent = "globalThis.controlRan = 1";
    host.appendChild(control);
    const script = document.createElementNS(xhtml, "SCRIPT");
    script.textContent = "globalThis.upperRan = 1";
    host.appendChild(script);
  });
  const script = host.lastChild;
  const found = globalThis.controlRan === 1 && globalThis.upperRan === undefined
    && String(script) === "[object HTMLUnknownElement]" && !script.matches("script")
    && script.localName === "SCRIPT";
  host.textContent = "";
  return found;
});
check("createElementNS(html, Foo) matches no lower-case name", () => {
  let foo;
  const host = inHost((host) => { foo = host.appendChild(document.createElementNS(xhtml, "Foo")); });
  const found = foo.localName === "Foo" && foo.tagName === "FOO" && !foo.matches("foo")
    && !foo.matches("Foo") && host.getElementsByTagName("foo").length === 0
    && host.getElementsByTagName("FOO").length === 0
    && String(foo) === "[object HTMLUnknownElement]";
  host.textContent = "";
  return found;
});
check("createElementNS(html, h:div) stays a div", () => {
  let div;
  const host = inHost((host) => { div = host.appendChild(document.createElementNS(xhtml, "h:div")); });
  const found = div instanceof HTMLDivElement && div.tagName === "H:DIV" && div.matches("div")
    && host.getElementsByTagName("h:div").length === 1
    && host.getElementsByTagName("H:DIV").length === 1
    && host.getElementsByTagName("div").length === 0;
  host.textContent = "";
  return found;
});
check("text nodes have no local name",
  () => document.createTextNode("x").localName === undefined);
check("html element of an XML document keeps a lower-case tagName", () => {
  const made = new Document().createElementNS(xhtml, "div");
  return made.tagName === "div" && made instanceof HTMLElement;
});

// Name validation (DOM standard, as Chromium): an element local name
// starting with an ASCII letter may hold anything but ASCII whitespace,
// NUL, "/" and ">"; otherwise it starts with ":", "_" or a non-ASCII code
// point and continues with ASCII alphanumerics, "-", ".", ":", "_" or
// non-ASCII. An attribute local name is non-empty without ASCII
// whitespace, NUL, "/", "=" or ">".
const outcome = (run) => {
  try { const value = run(); return value === undefined ? "ok" : String(value); }
  catch (error) { return error.name; }
};
const bad = "InvalidCharacterError";
const elementNames = [
  ["1bad", bad], ["-x", bad], ["", bad], ["a b", bad], ["a\tb", bad], ["a\fb", bad],
  ["a>", bad], ["a/", bad], ["x\0", bad], ["é!", bad], ["_!", bad], ["5", bad],
  ["_x", "_x"], [":x", ":x"], ["a:b", "a:b"], ["é", "é"], ["·x", "·x"],
  ["abc!", "abc!"], ["a.b", "a.b"], ["a=b", "a=b"], ["a\u000bb", "a\u000bb"],
  ["x y", "x y"], ["::", "::"], ["Foo", "foo"],
];
for (const [name, want] of elementNames) {
  check("createElement " + JSON.stringify(name),
    () => outcome(() => document.createElement(name).localName) === want);
  check("html document createElement " + JSON.stringify(name), () => outcome(() =>
    document.implementation.createHTMLDocument("").createElement(name).localName) === want);
  check("new Document createElement " + JSON.stringify(name), () => outcome(() =>
    new Document().createElement(name).localName) === (want === bad ? bad : name));
  check("xml document createElement " + JSON.stringify(name), () => outcome(() =>
    document.implementation.createDocument(null, null).createElement(name).localName)
    === (want === bad ? bad : name));
}
check("createElement(5)", () => outcome(() => document.createElement(5)) === bad);
check("createElement(undefined)",
  () => document.createElement(undefined).localName === "undefined");
// createElementNS splits at the first two colons; prefix and local name are
// validated separately, then the namespace rules apply.
const qualified = [
  ["1bad", bad], ["a b", bad], [":x", bad], ["a:", bad], ["::", bad], ["a::b", bad],
  ["é!", bad], ["p:1x", bad], ["_x", "|_x"], ["a:b", "a|b"], ["A:B", "A|B"],
  ["a:b:c", "a|b"], ["é", "|é"], ["a!b", "|a!b"], ["p>:x", bad],
  ["xmlns", "NamespaceError"], ["xml:a", "NamespaceError"],
];
for (const [name, want] of qualified) {
  check("createElementNS " + JSON.stringify(name), () => outcome(() => {
    const made = document.createElementNS("urn:x", name);
    return (made.prefix ?? "") + "|" + made.localName;
  }) === want);
  check("xml document createElementNS " + JSON.stringify(name), () => outcome(() => {
    const made = document.implementation.createDocument(null, null)
      .createElementNS("urn:x", name);
    return (made.prefix ?? "") + "|" + made.localName;
  }) === want);
}
const attributeNames = [
  ["", bad], ["a b", bad], ["a\tb", bad], ["a/", bad], ["a>", bad], ["a=b", bad],
  ["x\0", bad], ["1bad", "1bad"], ["-x", "-x"], [":x", ":x"], ["a:", "a:"], ["é!", "é!"],
  ["_!", "_!"], ["a:b:c", "a:b:c"], ["x y", "x y"], ["Foo", "foo"],
];
for (const [name, want] of attributeNames) {
  check("setAttribute " + JSON.stringify(name), () => outcome(() => {
    const element = document.createElement("p");
    element.setAttribute(name, "v");
    return element.attributes[0].name;
  }) === want);
  check("toggleAttribute " + JSON.stringify(name), () => outcome(() => {
    const element = document.createElement("p");
    element.toggleAttribute(name);
    return element.attributes[0].name;
  }) === want);
  check("createAttribute " + JSON.stringify(name),
    () => outcome(() => document.createAttribute(name).name) === want);
  check("xml document setAttribute " + JSON.stringify(name), () => outcome(() => {
    const element = document.implementation.createDocument(null, null).createElement("e");
    element.setAttribute(name, "v");
    return element.attributes[0].name;
  }) === (want === bad ? bad : name));
  check("xml document createAttribute " + JSON.stringify(name), () => outcome(() =>
    document.implementation.createDocument(null, null).createAttribute(name).name)
    === (want === bad ? bad : name));
}
const qualifiedAttributes = [
  ["a b", bad], [":x", bad], ["a:", bad], ["::", bad], ["a=b", bad], ["1bad", "|1bad"],
  ["a:b:c", "a|b"], ["A:B", "A|B"], ["xmlns", "NamespaceError"],
];
for (const [name, want] of qualifiedAttributes) {
  check("setAttributeNS " + JSON.stringify(name), () => outcome(() => {
    const element = document.createElement("p");
    element.setAttributeNS("urn:x", name, "v");
    const attribute = element.attributes[0];
    return (attribute.prefix ?? "") + "|" + attribute.localName;
  }) === want);
  check("createAttributeNS " + JSON.stringify(name), () => outcome(() => {
    const attribute = document.createAttributeNS("urn:x", name);
    return (attribute.prefix ?? "") + "|" + attribute.localName;
  }) === want);
}
// An element another document's createElement named with a colon keeps
// that local name when it enters the page.
check("new Document element a:b enters the page", () => {
  const host = document.body.appendChild(document.createElement("div"));
  for (const name of ["a:b", "xmlns:a", "A:b"])
    host.appendChild(new Document().createElement(name));
  host.appendChild(document.implementation.createDocument(null, null).createElement("p:q"));
  const found = Array.from(host.children, describe).join(";")
      === "a:b|a:b||;xmlns:a|xmlns:a||;A:b|A:b||;p:q|p:q||"
    && Array.from(host.children, (element) => element.tagName).join(",")
      === "a:b,xmlns:a,A:b,p:q"
    && host.innerHTML === "<a:b></a:b><xmlns:a></xmlns:a><A:b></A:b><p:q></p:q>";
  host.remove();
  return found;
});
// Reads never validate.
check("attribute reads accept any name", () => {
  const element = document.createElement("p");
  element.removeAttribute("a b");
  return element.getAttribute("1 bad") === null && element.hasAttribute("a b") === false;
});

// Doctype names may be empty but hold no ASCII whitespace, NUL or ">"
// (Chrome 156 throws InvalidCharacterError for "a b" and "a>b"; the other
// characters are the same rule). Both the page's DOMImplementation and
// another document's.
const doctypeOutcome = (implementation, name) => outcome(() =>
  implementation.createDocumentType(name, "", "").name);
for (const [label, implementation] of [
  ["page", () => document.implementation],
  ["new Document", () => new Document().implementation],
  ["createHTMLDocument", () => document.implementation.createHTMLDocument("").implementation],
]) {
  for (const name of ["a b", "a>b", "a\tb", "a\nb", "a\fb", "a\rb", "a\0b", " ", ">"])
    check(label + " createDocumentType " + JSON.stringify(name) + " throws",
      () => doctypeOutcome(implementation(), name) === "InvalidCharacterError");
  for (const name of ["", "html", "a:b", "a<b", "a/b", "\u00e9", "a\u00a0b"])
    check(label + " createDocumentType " + JSON.stringify(name) + " is kept",
      () => doctypeOutcome(implementation(), name) === name);
}
// Names fold A-Z only (Chrome 156: setAttribute "\u00c9" keeps "\u00c9",
// createElement("\u00c9\u{1f600}A") is "\u00c9\u{1f600}a").
check("setAttribute folds ASCII only", () => {
  const element = document.createElement("p");
  element.setAttribute("\u00c9", "1");
  element.setAttribute("DaTa-\u00c9", "2");
  return element.getAttributeNames().join() === "\u00c9,data-\u00c9"
    && element.attributes[0].name === "\u00c9"
    && element.getAttribute("\u00c9") === "1" && element.getAttribute("\u00e9") === null
    && element.getAttribute("DATA-\u00c9") === "2" && element.hasAttribute("\u00c9")
    && element.getAttributeNode("\u00c9")?.value === "1"
    && (element.removeAttribute("\u00e9"), element.hasAttribute("\u00c9"))
    && (element.removeAttribute("\u00c9"), !element.hasAttribute("\u00c9"));
});
check("toggleAttribute folds ASCII only", () => {
  const element = document.createElement("p");
  return element.toggleAttribute("\u00c9") === true
    && element.getAttributeNames().join() === "\u00c9"
    && element.toggleAttribute("\u00c9") === false && !element.hasAttribute("\u00c9");
});
check("createAttribute folds ASCII only", () =>
  document.createAttribute("\u00c9").name === "\u00c9"
    && document.createAttribute("A\u00c9").name === "a\u00c9"
    && document.implementation.createHTMLDocument("").createAttribute("\u00c9A").name
      === "\u00c9a");
check("createElement folds ASCII only", () =>
  document.createElement("\u00c9\u{1f600}A").localName === "\u00c9\u{1f600}a"
    && document.createElement("D\u00c9").localName === "d\u00c9"
    && document.implementation.createHTMLDocument("").createElement("\u00c9A").localName
      === "\u00c9a");
check("createElement still folds A-Z", () => {
  class UpperProbe extends HTMLElement {}
  customElements.define("x-upper-probe", UpperProbe);
  const div = document.createElement("DIV"), script = document.createElement("SCRIPT");
  return div instanceof HTMLDivElement && div.localName === "div" && div.tagName === "DIV"
    && script instanceof HTMLScriptElement && script.localName === "script"
    && document.createElement("X-Upper-PROBE") instanceof UpperProbe;
});
check("detached element attributes fold ASCII only", () => {
  const element = new Document().createElementNS(xhtml, "p");
  const html = document.implementation.createHTMLDocument("").createElement("p");
  html.setAttribute("\u00c9A", "1");
  element.setAttribute("\u00c9A", "1");
  const odd = document.createElement("\u00c9\u{1f600}A");
  odd.setAttribute("\u00c9B", "1");
  return html.getAttributeNames().join() === "\u00c9a" && html.getAttribute("\u00c9A") === "1"
    && element.getAttributeNames().join() === "\u00c9A"
    && odd.getAttributeNames().join() === "\u00c9b" && odd.getAttribute("\u00c9B") === "1";
});

globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
