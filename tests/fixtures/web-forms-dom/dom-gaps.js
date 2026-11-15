// DOM conformance gaps real pages hit, with Chromium's results as the
// expectations: markup of script-side documents' elements (createDocument,
// new Document(), an empty <iframe>), markup set on SVG and MathML
// elements (innerHTML, insertAdjacentHTML, outerHTML), the fragment
// parser's context, XMLSerializer, getHTML() with shadow roots, outerText,
// DocumentFragment's missing innerHTML, and prefixed createElementNS names.
const failures = [];
const check = (name, read, expected) => {
  try {
    const actual = read();
    if (actual !== expected)
      failures.push(name + ": " + JSON.stringify(actual));
  } catch (error) {
    failures.push(name + ": " + error);
  }
};
const xhtml = "http://www.w3.org/1999/xhtml",
  svgNs = "http://www.w3.org/2000/svg",
  serialize = (node) => new XMLSerializer().serializeToString(node);

// The serializer module loads on first use only.
check("lazy until used", () =>
  typeof Object.getOwnPropertyDescriptor(globalThis, "XMLSerializer").get,
  "function");

const xml = document.implementation.createDocument(null, "root");
check("xml empty root", () =>
  xml.documentElement.innerHTML + "|" + xml.documentElement.outerHTML,
  "|<root/>");
const kid = xml.createElement("Kid");
kid.setAttribute("a", '1<"&\n');
kid.appendChild(xml.createTextNode("t<&>"));
xml.documentElement.appendChild(kid);
xml.documentElement.appendChild(xml.createElementNS("urn:x", "p:Q"));
check("xml innerHTML", () => xml.documentElement.innerHTML,
  '<Kid a="1&lt;&quot;&amp;&#10;">t&lt;&amp;&gt;</Kid><p:Q xmlns:p="urn:x"/>');
check("xml outerHTML", () => kid.outerHTML,
  '<Kid a="1&lt;&quot;&amp;&#10;">t&lt;&amp;&gt;</Kid>');
check("xml document", () => serialize(xml),
  '<root><Kid a="1&lt;&quot;&amp;&#10;">t&lt;&amp;&gt;</Kid>' +
  '<p:Q xmlns:p="urn:x"/></root>');
check("new Document element", () => {
  const made = new Document().createElementNS("urn:y", "Foo");
  made.appendChild(made.ownerDocument.createComment("c"));
  return made.outerHTML;
}, '<Foo xmlns="urn:y"><!--c--></Foo>');

const frame = document.createElement("iframe");
document.body.appendChild(frame);
check("empty frame element", () => {
  const made = frame.contentDocument.createElement("div");
  made.innerHTML = "<i>1</i><br>";
  made.setAttribute("title", 'a "b');
  return made.innerHTML + "|" + made.outerHTML;
}, '<i>1</i><br>|<div title="a&nbsp;&quot;b"><i>1</i><br></div>');
check("empty frame body", () => {
  const body = frame.contentDocument.body;
  body.innerHTML = "<p>x</p>";
  return body.innerHTML + "|" + frame.contentDocument.documentElement.outerHTML;
}, "<p>x</p>|<html><head></head><body><p>x</p></body></html>");

const svg = document.createElementNS(svgNs, "svg");
svg.setAttribute("viewBox", "0 0 10 10");
svg.innerHTML = '<rect width="5"/><use xlink:href="#a"/>';
check("svg innerHTML parses as SVG", () =>
  [...svg.childNodes].map((node) => node.localName + ":" +
    (node.namespaceURI === svgNs)).join(","),
  "rect:true,use:true");
check("parsed svg innerHTML", () => {
  const box = document.createElement("div");
  box.innerHTML = "<svg></svg><math></math>";
  box.firstChild.innerHTML = "<path d='M0'/>";
  box.lastChild.innerHTML = "<mi>x</mi>";
  return box.firstChild.firstChild.namespaceURI + " " +
    box.lastChild.firstChild.namespaceURI;
}, svgNs + " http://www.w3.org/1998/Math/MathML");
const kinds = (parent) => [...parent.childNodes].map((node) =>
  node.nodeType === 1
    ? node.localName + ":" + ({ [svgNs]: "svg", [xhtml]: "html",
        "http://www.w3.org/1998/Math/MathML": "math" }[node.namespaceURI] ||
        node.namespaceURI)
    : node.nodeName).join(",");
check("insertAdjacentHTML in svg", () => {
  const box = document.createElement("div");
  box.innerHTML = "<svg><g></g></svg><math><mi>x</mi></math>";
  const g = box.firstChild.firstChild;
  g.insertAdjacentHTML("beforeend", "<rect/>");
  g.insertAdjacentHTML("afterend", "<circle/><p>out</p>");
  g.insertAdjacentHTML("beforebegin", "<path/>");
  box.lastChild.firstChild.insertAdjacentHTML("afterend", "<mn>2</mn>");
  return kinds(g) + "|" + kinds(box.firstChild) + "|" + kinds(box.lastChild);
}, "rect:svg|path:svg,g:svg,circle:svg,p:html|mi:math,mn:math");
check("outerHTML in svg", () => {
  const box = document.createElement("div");
  box.innerHTML = "<svg><g></g><text>t</text></svg>";
  box.firstChild.firstChild.outerHTML = "<rect/><foreignObject><b>x</b>" +
    "</foreignObject>";
  const foreign = box.firstChild.childNodes[1];
  return kinds(box.firstChild) + "|" + kinds(foreign);
}, "rect:svg,foreignObject:svg,text:svg|b:html");
check("adjacent markup in a parsed svg document", () => {
  const parsed = new DOMParser().parseFromString(
    '<svg xmlns="' + svgNs + '"><g/></svg>', "image/svg+xml");
  const g = parsed.documentElement.firstChild;
  g.insertAdjacentHTML("afterbegin", "<rect/>");
  g.outerHTML = "<circle/>";
  return kinds(parsed.documentElement);
}, "circle:svg");
check("script-side svg document markup", () => {
  const made = document.implementation.createDocument(svgNs, "svg");
  made.documentElement.innerHTML = "<rect/>";
  made.documentElement.insertAdjacentHTML("beforeend", "<circle/>");
  made.documentElement.firstChild.outerHTML = "<path/>";
  return kinds(made.documentElement);
}, "path:svg,circle:svg");
check("html context is body", () => {
  const made = document.implementation.createHTMLDocument("t");
  made.documentElement.insertAdjacentHTML("beforeend", "<p>y</p>");
  return kinds(made.documentElement);
}, "head:html,body:html,p:html");
check("fragment parent is a body context", () => {
  const fragment = document.createDocumentFragment();
  fragment.appendChild(document.createElement("span")).outerHTML =
    "<td>a</td><i>b</i>";
  return kinds(fragment);
}, "#text,i:html");
check("adjacent scripts stay inert", () => {
  const box = document.createElement("div");
  document.body.appendChild(box);
  box.insertAdjacentHTML("beforeend", "<script>window.adjacentRan=1<\/script>");
  box.remove();
  return String(window.adjacentRan);
}, "undefined");
check("no parent", () => {
  const names = [];
  for (const position of ["beforebegin", "afterend"])
    try {
      document.createElement("i").insertAdjacentHTML(position, "<b></b>");
      names.push("accepted");
    } catch (error) { names.push(error.name); }
  try {
    document.documentElement.outerHTML = "<p></p>";
    names.push("accepted");
  } catch (error) { names.push(error.name); }
  const lone = document.createElement("i");
  lone.outerHTML = "<b></b>";
  return names.join(",") + "," + lone.localName;
}, "NoModificationAllowedError,NoModificationAllowedError," +
  "NoModificationAllowedError,i");
check("DocumentFragment has no innerHTML", () => {
  const parsed = new DOMParser().parseFromString("<p>x</p>", "text/html");
  const fragments = [document.createDocumentFragment(),
    parsed.createDocumentFragment(),
    document.implementation.createHTMLDocument("t").createDocumentFragment(),
    document.createRange().createContextualFragment("<b>x</b>"),
    document.createElement("template").content];
  return fragments.map((fragment) => typeof fragment.innerHTML +
    ("innerHTML" in fragment)).join(",") + "|" +
    ("innerHTML" in DocumentFragment.prototype) + "," +
    Object.hasOwn(ShadowRoot.prototype, "innerHTML");
}, "undefinedfalse,undefinedfalse,undefinedfalse,undefinedfalse," +
  "undefinedfalse|false,true");
check("svg XMLSerializer", () => serialize(svg),
  '<svg xmlns="' + svgNs + '" viewBox="0 0 10 10"><rect width="5"/>' +
  '<use xmlns:xlink="http://www.w3.org/1999/xlink" xlink:href="#a"/></svg>');

para.insertAdjacentHTML("beforeend", "<br><img src=x>");
check("html element XMLSerializer", () => serialize(para),
  '<p xmlns="' + xhtml + '" id="para">a<b>b</b>c<br /><img src="x" /></p>');
check("fragment XMLSerializer", () => {
  const fragment = document.createDocumentFragment();
  fragment.append("a<", document.createElement("hr"));
  return serialize(fragment);
}, 'a&lt;<hr xmlns="' + xhtml + '" />');
check("XMLSerializer argument", () => {
  try { serialize({}); return "accepted"; } catch (error) { return error.name; }
}, "TypeError");

check("XMLSerializer generated prefix avoids declarations", () => {
  const doc = document.implementation.createDocument("urn:root", "root"),
    root = doc.documentElement;
  root.setAttributeNS("http://www.w3.org/2000/xmlns/", "xmlns:ns1", "urn:a");
  root.setAttributeNS("urn:b", "value", "test");
  return serialize(root);
}, '<root xmlns="urn:root" xmlns:ns1="urn:a" xmlns:ns2="urn:b" ns2:value="test"/>');

check("XMLSerializer generated prefix avoids inherited declarations", () => {
  const doc = document.implementation.createDocument("urn:root", "root"),
    root = doc.documentElement,
    child = doc.createElementNS("urn:root", "child");
  root.setAttributeNS("http://www.w3.org/2000/xmlns/", "xmlns:ns1", "urn:a");
  root.setAttributeNS("http://www.w3.org/2000/xmlns/", "xmlns:ns2", "urn:b");
  child.setAttributeNS("urn:c", "value", "test");
  root.appendChild(child);
  return serialize(root);
}, '<root xmlns="urn:root" xmlns:ns1="urn:a" xmlns:ns2="urn:b"><child xmlns:ns3="urn:c" ns3:value="test"/></root>');

const host = document.createElement("div");
host.innerHTML = "<span>light</span>";
const shadow = host.attachShadow({ mode: "closed" });
shadow.innerHTML = "<b>shadow</b><slot></slot>";
check("getHTML", () => para.getHTML(), 'a<b>b</b>c<br><img src="x">');
check("getHTML leaves shadow roots out", () => host.getHTML(),
  "<span>light</span>");
check("getHTML with shadowRoots", () => host.getHTML({ shadowRoots: [shadow] }),
  '<template shadowrootmode="closed"><b>shadow</b><slot></slot></template>' +
  "<span>light</span>");
check("getHTML of an ancestor", () => {
  const wrap = document.createElement("section");
  wrap.appendChild(host);
  return wrap.getHTML({ shadowRoots: [shadow] });
}, '<div><template shadowrootmode="closed"><b>shadow</b><slot></slot>' +
  "</template><span>light</span></div>");
check("serializableShadowRoots adds no root", () =>
  host.getHTML({ serializableShadowRoots: true }), "<span>light</span>");
check("ShadowRoot getHTML", () => shadow.getHTML(), "<b>shadow</b><slot></slot>");

check("outerText get", () => para.outerText, para.innerText);
check("outerText set", () => {
  const box = document.createElement("div");
  box.innerHTML = "x<span>y</span>z";
  box.firstElementChild.outerText = "a\nb";
  return box.innerHTML + "|" + box.childNodes.length;
}, "xa<br>bz|3");
check("outerText set empty", () => {
  const box = document.createElement("div");
  box.innerHTML = "<span>y</span>";
  box.firstChild.outerText = "";
  return box.childNodes.length + ":" + box.firstChild.nodeType;
}, "1:3");
check("outerText needs a parent", () => {
  try {
    document.createElement("i").outerText = "q";
    return "accepted";
  } catch (error) {
    return error.name;
  }
}, "NoModificationAllowedError");

check("createElementNS prefix", () => {
  const made = document.createElementNS("urn:x", "p:a");
  return [made.prefix, made.localName, made.tagName, made.namespaceURI].join(",");
}, "p,a,p:a,urn:x");
check("document.replaceChildren", () => typeof document.replaceChildren,
  "function");

globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
