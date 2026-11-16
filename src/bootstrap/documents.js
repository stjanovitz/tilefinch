/* Secondary documents: createHTMLDocument and DOMParser results. Built on
   first use, so a realm that never makes one pays for none of these
   functions. compat.js captures every realm value this module reads before
   page script runs and passes them in; nothing here reads a global. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateSecondaryDocuments", {
  __proto__: null,
  configurable: true,
  value({
    factories,
    createPrivateSlot,
    setNativeNodeOwner,
    adoptIntoDocument,
    liveCollection,
    liveChildNodes,
    ancestorLimit,
    escapeSelector,
    makeDoctype,
    asciiLower,
    createAttribute,
    secondaryDocumentPrototype,
    htmlDocumentPrototype,
    xmlDocumentPrototype,
    realm,
    Node,
    Document,
    ShadowRoot,
    Range,
    HTMLBodyElement,
    HTMLFrameSetElement,
    DOMException,
    MapType,
    objectCreate,
    defineProperty,
    defineProperties,
    arrayFrom,
    planDocumentInsert,
    listIndex,
    listCopy,
  }) {
    /* Loaded after the page may have replaced these globals: build
       errors, promises and platform objects with the constructors
       hardening.js captured. */
    const { TypeError, apply } = globalThis.__tilefinchEventInterfaces;
    const nativeDocumentState = createPrivateSlot(),
      nativeDocumentPrototypes = new MapType(),
      xhtmlNamespace = "http://www.w3.org/1999/xhtml",
      nativeDocumentOf = (receiver) => {
        const state = nativeDocumentState.get(receiver);
        if (!state) throw new TypeError("Illegal invocation");
        return state;
      },
      ownedBy = (doc, node) => {
        /* Created for `doc`: an inert node with no definition lookup, then
           tagged with its owner. */
        if (!node) return node;
        if (node.__handle !== undefined) setNativeNodeOwner(node, doc);
        else adoptIntoDocument(node, doc);
        return node;
      },
      inertCreate = (doc, create) => {
        realm.__tilefinchCustomElementCreationSuppressed =
          (realm.__tilefinchCustomElementCreationSuppressed || 0) + 1;
        try {
          return ownedBy(doc, create());
        } finally {
          realm.__tilefinchCustomElementCreationSuppressed--;
        }
      },
      /* The document's children (doctype, comments, the root element) are
         script-side; a child the page moved elsewhere leaves the list. */
      documentChildren = (doc) => {
        /* By index here and below: no page patch of the Array methods or
           the array iterator runs while a document's children change. */
        const state = nativeDocumentOf(doc), children = [];
        for (let at = 0; at < state.children.length; at++) {
          const node = state.children[at];
          if (node.__handle === undefined
              ? node.__detachedParent === doc
              : node.parentNode === null && node.ownerDocument === doc)
            children[children.length] = node;
        }
        return (state.children = children);
      },
      documentRoot = (doc) => {
        const children = documentChildren(doc);
        for (let at = 0; at < children.length; at++)
          if (children[at].nodeType === Node.ELEMENT_NODE) return children[at];
        return null;
      },
      rootChild = (doc, names) => {
        const root = documentRoot(doc);
        if (
          !root ||
          root.namespaceURI !== xhtmlNamespace ||
          root.localName !== "html"
        )
          return null;
        for (let child = root.firstElementChild; child;
             child = child.nextElementSibling)
          if (
            child.namespaceURI === xhtmlNamespace &&
            names.includes(child.localName)
          )
            return child;
        return null;
      },
      documentQueryAll = (doc, selector) => {
        selector = String(selector);
        const root = documentRoot(doc);
        if (!root) return [];
        const below = root.querySelectorAll(selector);
        if (!root.matches(selector)) return below;
        const values = [root, ...below];
        defineProperty(values, "item", {
          value: (index) => values[Number(index)] ?? null,
        });
        return values;
      },
      documentCollection = (doc, read) =>
        liveCollection(() => {
          const root = documentRoot(doc);
          return root ? read(root) : [];
        }, true),
      /* The root joins getElementsByTagName's list by the same qualified
         name test the native list uses. */
      withRoot = (root, name, list) =>
        (name = String(name)) === "*" ||
        (root.prefix ? root.prefix + ":" : "") + root.localName ===
          (root.namespaceURI === xhtmlNamespace
            ? name.replace(/[A-Z]/g, (c) => c.toLowerCase()) : name)
          ? [root, ...list]
          : [...list],
      /* Moves nodes the shared check (platform.js planDocumentInsert)
         accepted in before `before`. */
      placeDocumentNodes = (doc, { items, before }) => {
        const state = nativeDocumentOf(doc);
        for (let at = 0; at < items.length; at++) {
          const item = items[at];
          if (item.__handle !== undefined) item.parentNode?.removeChild(item);
          else factories.detach(item);
          if (item.nodeType === Node.DOCUMENT_TYPE_NODE)
            item.__detachedParent = doc;
          adoptIntoDocument(item, doc);
          const list = listCopy(state.children, item),
            found = before === null ? -1 : listIndex(list, before),
            index = found < 0 ? list.length : found,
            next = [];
          for (let n = 0; n < list.length; n++) {
            if (n === index) next[next.length] = item;
            next[next.length] = list[n];
          }
          if (index === list.length) next[next.length] = item;
          state.children = next;
        }
      },
      insertDocumentChild = (doc, node, before = null) => {
        placeDocumentNodes(doc,
          planDocumentInsert(documentChildren(doc), node, false, before));
        return node;
      },
      removeDocumentChild = (doc, node) => {
        const state = nativeDocumentOf(doc);
        if (listIndex(documentChildren(doc), node) < 0)
          throw new DOMException("Node is not a child", "NotFoundError");
        state.children = listCopy(state.children, node);
        if (node.__handle === undefined) node.__detachedParent = null;
        return node;
      },
      nativeDocumentPrototype = (xml) => {
        let proto = nativeDocumentPrototypes.get(xml);
        if (proto) return proto;
        proto = objectCreate(
          secondaryDocumentPrototype(
            xml ? xmlDocumentPrototype : htmlDocumentPrototype,
          ),
        );
        const value = (name, read) => ({
            configurable: true,
            enumerable: true,
            get: { [name]() { return read(nativeDocumentOf(this), this); } }[name],
          }),
          method = (name, call) => ({
            configurable: true,
            enumerable: true,
            writable: true,
            value: { [name](...args) {
              nativeDocumentOf(this);
              const list = [this];
              for (let at = 0; at < args.length; at++) list[at + 1] = args[at];
              return apply(call, undefined, list);
            } }[name],
          });
        defineProperties(proto, {
          /* The script-side child list, as shim documents expose it: a
             doctype leaf finds its siblings through it. */
          __detachedChildren: {
            configurable: true,
            get() {
              return documentChildren(this);
            },
          },
          nodeType: value("nodeType", () => Node.DOCUMENT_NODE),
          nodeName: value("nodeName", () => "#document"),
          ownerDocument: value("ownerDocument", () => null),
          parentNode: value("parentNode", () => null),
          parentElement: value("parentElement", () => null),
          nodeValue: value("nodeValue", () => null),
          textContent: value("textContent", () => null),
          isConnected: value("isConnected", () => true),
          contentType: value("contentType", (state) => state.contentType),
          compatMode: value("compatMode", (state) => state.compatMode),
          /* A frame's document (bindFrameDocument) has its frame's URL,
             Location and WindowProxy; other secondary documents none. */
          URL: value("URL", (state) => state.url ?? "about:blank"),
          documentURI: value("documentURI", (state) => state.url ?? "about:blank"),
          characterSet: value("characterSet", () => "UTF-8"),
          charset: value("charset", () => "UTF-8"),
          inputEncoding: value("inputEncoding", () => "UTF-8"),
          readyState: value("readyState", () => "complete"),
          defaultView: value("defaultView", (state) => state.view ?? null),
          location: value("location", (state) => state.location ?? null),
          activeElement: value("activeElement", (state, doc) =>
            doc.body || doc.documentElement),
          doctype: value("doctype", (state, doc) =>
            documentChildren(doc).find(
              (node) => node.nodeType === Node.DOCUMENT_TYPE_NODE,
            ) || null),
          documentElement: value("documentElement", (state, doc) =>
            documentRoot(doc)),
          head: value("head", (state, doc) => rootChild(doc, ["head"])),
          body: {
            configurable: true,
            enumerable: true,
            get: { body() { nativeDocumentOf(this); return rootChild(this, ["body", "frameset"]); } }.body,
            set: { body(element) {
              nativeDocumentOf(this);
              if (
                !(element instanceof HTMLBodyElement) &&
                !(element instanceof HTMLFrameSetElement)
              )
                throw new DOMException(
                  "Body must be body or frameset",
                  "HierarchyRequestError",
                );
              const root = documentRoot(this);
              if (!root)
                throw new DOMException(
                  "Document has no root element",
                  "HierarchyRequestError",
                );
              const current = this.body;
              if (current === element) return;
              if (current) root.replaceChild(element, current);
              else root.appendChild(element);
            } }.body,
          },
          title: {
            configurable: true,
            enumerable: true,
            get: { title() {
              nativeDocumentOf(this);
              const item = this.querySelector("title");
              return item
                ? String(item.textContent)
                    .replace(/[\t\n\f\r ]+/g, " ")
                    .replace(/^ | $/g, "")
                : "";
            } }.title,
            set: { title(text) {
              nativeDocumentOf(this);
              let item = this.querySelector("title");
              if (!item) {
                const head = this.head;
                if (!head) return;
                item = head.appendChild(this.createElement("title"));
              }
              item.textContent = String(text);
            } }.title,
          },
          childNodes: value("childNodes", (state, doc) =>
            state.childNodes ||= liveChildNodes(
              doc, () => documentChildren(doc))),
          children: value("children", (state, doc) =>
            state.childCollection ||= liveCollection(
              () => documentChildren(doc).filter(
                (node) => node.nodeType === Node.ELEMENT_NODE,
              ),
            )),
          firstChild: value("firstChild", (state, doc) =>
            documentChildren(doc)[0] || null),
          lastChild: value("lastChild", (state, doc) =>
            documentChildren(doc).at(-1) || null),
          firstElementChild: value("firstElementChild", (state, doc) =>
            documentRoot(doc)),
          lastElementChild: value("lastElementChild", (state, doc) =>
            documentRoot(doc)),
          childElementCount: value("childElementCount", (state, doc) =>
            documentRoot(doc) ? 1 : 0),
          forms: value("forms", (state, doc) =>
            state.forms ||= documentCollection(doc, (root) =>
              withRoot(root, "form", root.getElementsByTagName("form")))),
          images: value("images", (state, doc) =>
            state.images ||= documentCollection(doc, (root) =>
              withRoot(root, "img", root.getElementsByTagName("img")))),
          scripts: value("scripts", (state, doc) =>
            state.scripts ||= documentCollection(doc, (root) =>
              withRoot(root, "script", root.getElementsByTagName("script")))),
          embeds: value("embeds", (state, doc) =>
            state.embeds ||= documentCollection(doc, (root) =>
              withRoot(root, "embed", root.getElementsByTagName("embed")))),
          plugins: value("plugins", (state, doc) => doc.embeds),
          links: value("links", (state, doc) =>
            state.links ||= documentCollection(doc, (root) =>
              arrayFrom(documentQueryAll(doc, "a[href],area[href]")))),
          anchors: value("anchors", (state, doc) =>
            state.anchors ||= documentCollection(doc, (root) =>
              arrayFrom(documentQueryAll(doc, "a[name]")))),
          hasChildNodes: method("hasChildNodes", (doc) =>
            documentChildren(doc).length !== 0),
          getRootNode: method("getRootNode", (doc) => doc),
          contains: method("contains", (doc, node) => {
            if (node === doc) return true;
            if (!(node instanceof Node)) return false;
            let at = node;
            for (let steps = 0; at && steps < ancestorLimit; steps++) {
              const parent = at.parentNode;
              if (!parent)
                return documentChildren(doc).includes(at);
              at = parent;
            }
            return false;
          }),
          createElement: method("createElement", (doc, tag) =>
            inertCreate(doc, () =>
              nativeDocumentOf(doc).xml
                ? factories.createElementNS(null, String(tag))
                : factories.createElement(String(tag)))),
          createElementNS: method("createElementNS", (doc, namespace, tag) =>
            inertCreate(doc, () =>
              factories.createElementNS(namespace, tag))),
          createTextNode: method("createTextNode", (doc, text) =>
            ownedBy(doc, factories.createTextNode(String(text)))),
          createComment: method("createComment", (doc, text) =>
            ownedBy(doc, factories.createComment(String(text)))),
          createDocumentFragment: method("createDocumentFragment", (doc) =>
            ownedBy(doc, factories.createDocumentFragment())),
          createAttribute: method("createAttribute", (doc, name) =>
            createAttribute(
              doc,
              nativeDocumentOf(doc).xml ? String(name) : asciiLower(name),
            )),
          createAttributeNS: method("createAttributeNS", (doc, namespace, name) =>
            createAttribute(doc, name, namespace, true)),
          createCDATASection: method("createCDATASection", (doc) => {
            throw new DOMException(
              "CDATA sections are not supported in HTML documents",
              "NotSupportedError",
            );
          }),
          createRange: method("createRange", () => new Range()),
          querySelector: method("querySelector", (doc, selector) => {
            selector = String(selector);
            const root = documentRoot(doc);
            if (!root) return null;
            return root.matches(selector) ? root : root.querySelector(selector);
          }),
          querySelectorAll: method("querySelectorAll", documentQueryAll),
          getElementById: method("getElementById", (doc, id) => {
            id = String(id);
            const root = documentRoot(doc);
            if (!root || id === "") return null;
            return root.getAttribute("id") === id
              ? root
              : root.querySelector("#" + escapeSelector(id));
          }),
          getElementsByTagName: method("getElementsByTagName", (doc, name) =>
            documentCollection(doc, (root) =>
              withRoot(root, name, root.getElementsByTagName(String(name))))),
          getElementsByClassName: method("getElementsByClassName", (doc, names) =>
            documentCollection(doc, (root) => {
              const list = arrayFrom(root.getElementsByClassName(String(names))),
                wanted = String(names).split(/[\t\n\f\r ]+/).filter(Boolean);
              return wanted.length &&
                wanted.every((name) => root.classList.contains(name))
                ? [root, ...list]
                : list;
            })),
          getElementsByName: method("getElementsByName", (doc, name) =>
            documentCollection(doc, () =>
              arrayFrom(documentQueryAll(
                doc, "[name=\"" + escapeSelector(String(name)) + "\"]")))),
          appendChild: method("appendChild", (doc, node) =>
            insertDocumentChild(doc, node)),
          insertBefore: method("insertBefore", (doc, node, before) =>
            insertDocumentChild(doc, node, before ?? null)),
          removeChild: method("removeChild", removeDocumentChild),
          replaceChild: method("replaceChild", (doc, node, old) => {
            if (!(old instanceof Node)) throw new TypeError("Node required");
            const plan = planDocumentInsert(
              documentChildren(doc), node, false, null, old);
            removeDocumentChild(doc, old);
            placeDocumentNodes(doc, plan);
            return old;
          }),
          append: method("append", (doc, ...nodes) => {
            placeDocumentNodes(doc, planDocumentInsert(documentChildren(doc),
              nodes, true, null));
          }),
          prepend: method("prepend", (doc, ...nodes) => {
            placeDocumentNodes(doc, planDocumentInsert(documentChildren(doc),
              nodes, true, undefined));
          }),
          replaceChildren: method("replaceChildren", (doc, ...nodes) => {
            const plan = planDocumentInsert(documentChildren(doc), nodes, true,
              null);
            const children = listCopy(documentChildren(doc));
            for (let at = 0; at < children.length; at++)
              removeDocumentChild(doc, children[at]);
            placeDocumentNodes(doc, plan);
          }),
          adoptNode: method("adoptNode", (doc, node) => {
            if (node instanceof Document)
              throw new DOMException("Documents cannot be adopted", "NotSupportedError");
            if (node instanceof ShadowRoot)
              throw new DOMException("Shadow roots cannot be adopted", "HierarchyRequestError");
            if (!(node instanceof Node)) throw new TypeError("Node required");
            if (node.parentNode?.removeChild) node.parentNode.removeChild(node);
            else factories.detach(node);
            adoptIntoDocument(node, doc);
            return node;
          }),
          cloneNode: method("cloneNode", (doc, deep = false) => {
            const state = nativeDocumentOf(doc),
              clone = nativeDocument({
                xml: state.xml,
                contentType: state.contentType,
                compatMode: state.compatMode,
              });
            if (deep)
              for (const child of documentChildren(doc))
                insertDocumentChild(clone, child.cloneNode(true));
            return clone;
          }),
        });
        nativeDocumentPrototypes.set(xml, proto);
        return proto;
      },
      nativeDocument = ({
        xml = false,
        contentType = xml ? "application/xml" : "text/html",
        compatMode = "CSS1Compat",
      } = {}) => {
        const doc = objectCreate(nativeDocumentPrototype(xml));
        nativeDocumentState.set(doc, {
          xml,
          contentType,
          compatMode,
          children: [],
          childCollection: null,
          url: null,
          location: null,
          view: null,
        });
        return doc;
      },
      htmlDoctype = (doc) => {
        const doctype = makeDoctype("html");
        doctype.__detachedOwner = doc;
        insertDocumentChild(doc, doctype);
      },
      /* createHTMLDocument(title): doctype, html, head (with the title when
         one is given) and body. */
      createNativeHTMLDocument = (title) => {
        const doc = nativeDocument();
        htmlDoctype(doc);
        const html = doc.appendChild(doc.createElement("html")),
          head = html.appendChild(doc.createElement("head"));
        if (title !== undefined)
          head.appendChild(doc.createElement("title")).textContent = String(title);
        html.appendChild(doc.createElement("body"));
        return doc;
      },
      /* A frame's initial about:blank document (and the empty document of a
         frame whose content lives in its own runtime): html, head and body,
         no doctype. The initial about:blank document is in quirks mode. */
      createNativeFrameDocument = (standards = false) => {
        const doc = nativeDocument({
            compatMode: standards ? "CSS1Compat" : "BackCompat",
          }),
          html = doc.appendChild(doc.createElement("html"));
        html.appendChild(doc.createElement("head"));
        html.appendChild(doc.createElement("body"));
        return doc;
      },
      /* Gives a secondary document its frame: location is the frame's
         Location (its href the document URL), view its WindowProxy. false
         for a document this module did not make. */
      bindFrameDocument = (doc, location, view) => {
        const state = nativeDocumentState.get(doc);
        if (!state) return false;
        state.location = location ?? null;
        state.url = location?.href == null ? null : String(location.href);
        state.view = view ?? null;
        return true;
      },
      htmlStartTag =
        /^(?:\s|<!--[\s\S]*?-->|<!doctype[^>]*>)*<html((?:\s+[^\s"'>\/=]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s"'=<>`]+))?)*)\s*\/?>/i,
      /* DOMParser text/html: a whole-document parse. The fragment parser in
         an html context builds head and body (with the body's attributes);
         the root's own attributes, which only the document parser keeps, are
         read from its start tag by the same native attribute parser. */
      parseNativeHTMLDocument = (source) => {
        const doctype = /^(?:\s|<!--[\s\S]*?-->)*<!doctype\s+html\b/i.test(source),
          doc = nativeDocument({ compatMode: doctype ? "CSS1Compat" : "BackCompat" });
        if (doctype) htmlDoctype(doc);
        const html = doc.appendChild(doc.createElement("html"));
        html.innerHTML = source;
        const tag = htmlStartTag.exec(source);
        if (tag && tag[1]) {
          const probe = doc.createElement("div");
          probe.innerHTML = "<div" + tag[1] + "></div>";
          for (const attribute of arrayFrom(probe.firstChild?.attributes || []))
            if (!html.hasAttribute(attribute.name))
              html.setAttribute(attribute.name, attribute.value);
        }
        if (!doc.head) html.insertBefore(doc.createElement("head"), html.firstChild);
        if (!doc.body) html.appendChild(doc.createElement("body"));
        return doc;
      },
      /* DOMParser XML types. SVG is parsed by the HTML parser's foreign
         content rules, which keep SVG's case and self-closing tags; other XML
         is not parsed, and the result says so with a parsererror root, as an
         XML parser's failure does, instead of an empty document that looks
         like a successful parse. */
      parseErrorDocument = (contentType, message) => {
        const doc = nativeDocument({ xml: true, contentType }),
          root = doc.appendChild(doc.createElementNS(
            xhtmlNamespace, "parsererror"));
        root.textContent = message;
        return doc;
      },
      parseNativeSVGDocument = (source, contentType) => {
        const doc = nativeDocument({ xml: true, contentType }),
          holder = doc.createElement("div");
        holder.innerHTML = source
          .replace(/^\s*<\?xml[^>]*\?>/i, "")
          .replace(/^(\s|<!--[\s\S]*?-->)*<!doctype[^>[]*(\[[\s\S]*?\])?\s*>/i, "$1");
        const elements = arrayFrom(holder.children),
          root = elements[0];
        if (
          elements.length !== 1 ||
          root.namespaceURI !== "http://www.w3.org/2000/svg" ||
          root.localName !== "svg"
        )
          return parseErrorDocument(
            contentType, "This SVG document has no single <svg> root element.");
        doc.appendChild(root);
        return doc;
      };
    /* A refused append, prepend or replaceChildren on any document takes
       its arguments out of where they were (platform.js
       planDocumentInsert), including a child of one of these documents,
       which has no parentNode. */
    planDocumentInsert.releaseChild = (node) => {
      const doc = node.ownerDocument;
      if (nativeDocumentState.get(doc) && documentChildren(doc).includes(node))
        removeDocumentChild(doc, node);
    };
    return {
      createNativeHTMLDocument,
      createNativeFrameDocument,
      bindFrameDocument,
      parseNativeHTMLDocument,
      parseNativeSVGDocument,
      parseErrorDocument,
    };
  },
});
