/* Script-side detached documents: the XML documents
   DOMImplementation.createDocument() makes, new Document(), the template
   contents owner, an element whose name native nodes cannot represent, and
   their clones. Their nodes are script objects over platform.js's detached
   node prototypes. Built the first time a realm makes one, so a realm that
   never does pays nothing for them. platform.js passes the realm values
   and detached-node helpers it captured before page script ran; everything
   else here is read when a document is made or used, exactly as it was
   when this lived in platform.js. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateDetachedDocument", {
  __proto__: null,
  configurable: true,
  value({
    Object,
    String,
    Map,
    TypeError,
    DOMException,
    Node,
    Document,
    DocumentType,
    Element,
    HTMLDocument,
    XMLDocument,
    HTMLElement,
    HTMLBodyElement,
    HTMLFrameSetElement,
    SVGElement,
    ShadowRoot,
    domShared,
    secondaryDocumentPrototype,
    elementPrototype,
    textPrototype,
    htmlNamespace,
    checkElementName,
    checkQualifiedName,
    checkDoctypeName,
    asciiLower,
    listCopy,
    listOfType,
    planDocumentInsert,
    insert,
    detach,
    adoptOwner,
  }) {
    const detachedDocument = (
      title,
      xml = false,
      suppliedDoctype = null,
      namespace = null,
      qualifiedName = "",
      initialize = true,
      compatMode = "CSS1Compat",
    ) => {
      const doc = Object.create(
          secondaryDocumentPrototype(
            xml ? XMLDocument.prototype : HTMLDocument.prototype,
          ),
        ),
        /* `tag` is the qualified name as given: createElement lower-cases
           it in an HTML document, createElementNS never. */
        makeElement = (tag, namespace = htmlNamespace, prefix = null) => {
          tag = String(tag);
          const normalizedNamespace =
              namespace === null || namespace === ""
                ? null
                : String(namespace),
            base =
              normalizedNamespace === htmlNamespace
                ? tag === "body"
                  ? HTMLBodyElement.prototype
                  : tag === "frameset"
                    ? HTMLFrameSetElement.prototype
                    : HTMLElement.prototype
                : normalizedNamespace === "http://www.w3.org/2000/svg"
                  ? SVGElement.prototype
                  : Element.prototype,
            detachedPrototype = elementPrototype(base),
            /*
             * Detached documents have no browsing context and therefore do
             * not own a second set of interface prototypes. Keep the
             * platform operations as own descriptors while exposing the
             * standard interface prototype directly, as the DOM requires.
             */
            node = Object.create(base);
          const detachedDescriptors =
            Object.getOwnPropertyDescriptors(detachedPrototype);
          for (const descriptor of Object.values(detachedDescriptors))
            descriptor.configurable = true;
          Object.defineProperties(node, detachedDescriptors);
          Object.defineProperties(node, {
            __detachedOwner: { value: doc, writable: true },
            __detachedTag: { value: tag },
            __detachedPrefix: { value: prefix },
            __detachedNamespace: {
              value: normalizedNamespace,
              writable: true,
            },
            __detachedChildren: { value: [], writable: true },
            __detachedParent: { value: null, writable: true },
            __detachedAttributes: { value: new Map() },
            __detachedAttributeNodes: { value: [], writable: true },
          });
          return node;
        },
        makeData = (value, type, name) => {
          const node = Object.create(textPrototype);
          Object.defineProperties(node, {
            __detachedOwner: { value: doc, writable: true },
            __detachedParent: { value: null, writable: true },
            data: { value: String(value), writable: true },
            nodeType: { value: type },
            nodeName: { value: name },
          });
          node.cloneNode = () => makeData(node.data, type, name);
          return node;
        },
        walk = (root, selector, output) => {
          const children =
            listCopy(root.__detachedChildren || root.childNodes || []);
          for (let at = 0; at < children.length; at++) {
            const child = children[at];
            if (child.nodeType !== Node.ELEMENT_NODE) continue;
            const match =
              selector[0] === "#"
                ? child.getAttribute("id") === selector.slice(1)
                : child.namespaceURI === htmlNamespace && !xml
                  ? child.localName === selector.toLowerCase()
                  : child.tagName === selector;
            if (match) output[output.length] = child;
            walk(child, selector, output);
          }
        };
      Object.defineProperty(doc, "__detachedChildren", {
        value: [],
        writable: true,
      });
      const rootElement = () =>
          listOfType(doc.__detachedChildren, Node.ELEMENT_NODE, true),
        directElement = (root, names) => {
          if (
            !root ||
            root.namespaceURI !== htmlNamespace ||
            root.localName !== "html"
          )
            return null;
          return (
            root.children.find(
              (child) =>
                child.namespaceURI === htmlNamespace &&
                names.includes(child.localName),
            ) || null
          );
        };
      Object.defineProperties(doc, {
        nodeType: { value: Node.DOCUMENT_NODE },
        nodeName: { value: "#document" },
        ownerDocument: { value: null },
        contentType: {
          value: !xml ? "text/html"
            : namespace === htmlNamespace ? "application/xhtml+xml"
            : namespace === "http://www.w3.org/2000/svg" ? "image/svg+xml"
            : "application/xml",
        },
        compatMode: { value: compatMode },
        defaultView: { value: null, configurable: true },
        documentElement: { get: rootElement },
        doctype: {
          configurable: true,
          get() {
            return listOfType(this.__detachedChildren, Node.DOCUMENT_TYPE_NODE, true);
          },
        },
        head: {
          get() {
            return directElement(rootElement(), ["head"]);
          },
        },
        body: {
          get() {
            return directElement(rootElement(), ["body", "frameset"]);
          },
          set(value) {
            if (!(value instanceof Element))
              throw new TypeError("Body must be an element");
            if (
              !(value instanceof HTMLBodyElement) &&
              !(value instanceof HTMLFrameSetElement)
            )
              throw new DOMException(
                "Body must be body or frameset",
                "HierarchyRequestError",
              );
            if (value.namespaceURI !== htmlNamespace)
              throw new DOMException(
                "Body must be in the HTML namespace",
                "HierarchyRequestError",
              );
            const root = rootElement();
            if (!root)
              throw new DOMException(
                "Document has no root element",
                "HierarchyRequestError",
              );
            const current = this.body;
            if (current) root.replaceChild(value, current);
            else root.insertBefore(value, root.firstChild);
          },
        },
        scrollingElement: {
          get() {
            const root = this.documentElement;
            if (!root) return null;
            if (this.compatMode !== "BackCompat") return root;
            if (
              root.namespaceURI !== htmlNamespace ||
              root.localName !== "html"
            )
              return null;
            const body = this.body;
            if (!body) return null;
            const rootOverflow = String(
                root.style?.overflow || "visible",
              ).toLowerCase(),
              bodyOverflow = String(
                body.style?.overflow || "visible",
              ).toLowerCase();
            return rootOverflow === "visible" ||
              bodyOverflow === "visible" ||
              bodyOverflow === "clip"
              ? body
              : null;
          },
        },
        title: {
          get() {
            const item = this.querySelector("title");
            return item
              ? String(item.textContent).replace(/\s+/g, " ").trim()
              : "";
          },
          set(value) {
            let item = this.querySelector("title");
            if (!item) {
              const head = this.head;
              if (!head) return;
              item = this.createElement("title");
              head.appendChild(item);
            }
            item.textContent = String(value);
          },
        },
        childNodes: {
          get() {
            return domShared.detachedChildNodes(this);
          },
        },
        children: {
          get() {
            if (!this.__detachedChildrenCollection)
              Object.defineProperty(this, "__detachedChildrenCollection", {
                value: __tilefinchLiveHTMLCollection(() =>
                  listOfType(this.__detachedChildren, Node.ELEMENT_NODE),
                ),
              });
            return this.__detachedChildrenCollection;
          },
        },
        firstChild: {
          get() {
            return this.__detachedChildren[0] || null;
          },
        },
        lastChild: {
          get() {
            return (
              this.__detachedChildren[this.__detachedChildren.length - 1] ||
              null
            );
          },
        },
      });
      /* An XML document's elements have no namespace unless it is an
         XHTML document, and keep their case. */
      doc.createElement = (tag) => xml
        ? makeElement(checkElementName(String(tag)),
          doc.contentType === "application/xhtml+xml" ? htmlNamespace : null)
        : makeElement(asciiLower(checkElementName(String(tag))));
      doc.createElementNS = (namespace, tag) => {
        const qualified = checkQualifiedName(namespace, tag, true);
        return makeElement(qualified.name, qualified.namespace, qualified.prefix);
      };
      doc.createAttribute = (name) =>
        __tilefinchCreateAttribute(
          doc,
          xml ? String(name) : asciiLower(name),
        );
      doc.createAttributeNS = (namespace, name) =>
        __tilefinchCreateAttribute(doc, name, namespace, true);
      doc.createTextNode = (value) => {
        const node = Object.create(textPrototype);
        Object.defineProperties(node, {
          __detachedOwner: { value: doc, writable: true },
          __detachedParent: { value: null, writable: true },
          data: { value: String(value), writable: true },
        });
        return node;
      };
      doc.createComment = (value) =>
        makeData(value, Node.COMMENT_NODE, "#comment");
      doc.createCDATASection = (value) =>
        makeData(value, Node.CDATA_SECTION_NODE, "#cdata-section");
      doc.createProcessingInstruction = (target, value) =>
        makeData(value, Node.PROCESSING_INSTRUCTION_NODE, String(target));
      doc.createDocumentType = (name, publicId = "", systemId = "") => {
        const node = Object.create(DocumentType.prototype);
        Object.defineProperties(node, {
          __detachedOwner: { value: doc, writable: true },
          __detachedParent: { value: null, writable: true },
          nodeType: { value: Node.DOCUMENT_TYPE_NODE },
          nodeName: { get() { return this.name; } },
          name: { value: checkDoctypeName(name) },
          publicId: { value: String(publicId) },
          systemId: { value: String(systemId) },
          ownerDocument: { get() { return this.__detachedOwner; } },
          parentNode: { get() { return this.__detachedParent; } },
          childNodes: { get() { return domShared.detachedChildNodes(this); } },
        });
        node.cloneNode = function () {
          return this.__detachedOwner.createDocumentType(
            this.name,
            this.publicId,
            this.systemId,
          );
        };
        return node;
      };
      doc.createDocumentFragment = () => {
        const node = makeElement("fragment", "");
        Object.defineProperty(node, "nodeType", {
          value: Node.DOCUMENT_FRAGMENT_NODE,
        });
        return node;
      };
      doc.appendChild = (node) => doc.insertBefore(node, null);
      doc.removeChild = (node) => {
        if (node?.__detachedParent !== doc)
          throw new DOMException("Node is not a child", "NotFoundError");
        return detach(node);
      };
      doc.insertBefore = (node, child) => {
        const plan = planDocumentInsert(
          doc.__detachedChildren, node, false, child ?? null);
        for (const item of plan.items) insert(doc, item, plan.before);
        return node;
      };
      /* Checked whole before the old child leaves; the insertions go
         through insertBefore, which compat.js gives mutation records. */
      doc.replaceChild = (node, child) => {
        if (!(child instanceof Node)) throw new TypeError("Node required");
        const plan = planDocumentInsert(
          doc.__detachedChildren, node, false, null, child);
        doc.removeChild(child);
        for (const item of plan.items) doc.insertBefore(item, plan.before);
        return child;
      };
      doc.replaceChildren = (...values) => {
        const { items } = planDocumentInsert(
          doc.__detachedChildren, values, true, null);
        for (const child of [...doc.__detachedChildren]) doc.removeChild(child);
        for (const item of items) doc.insertBefore(item, null);
      };
      doc.querySelector = (selector) => {
        const output = [];
        walk(doc, String(selector), output);
        return output[0] || null;
      };
      doc.querySelectorAll = (selector) => {
        const output = [];
        walk(doc, String(selector), output);
        return output;
      };
      doc.getElementById = (id) => doc.querySelector("#" + String(id));
      doc.getElementsByTagName = (tag) =>
        __tilefinchLiveHTMLCollection(() => doc.querySelectorAll(String(tag)));
      doc.adoptNode = (node) => {
        if (node instanceof Document)
          throw new DOMException("Documents cannot be adopted", "NotSupportedError");
        if (node instanceof ShadowRoot)
          throw new DOMException("Shadow roots cannot be adopted", "HierarchyRequestError");
        if (node?.parentNode?.removeChild) node.parentNode.removeChild(node);
        else detach(node);
        adoptOwner(node, doc);
        return node;
      };
      doc.cloneNode = (deep = false) => {
        const clone = detachedDocument(
          undefined,
          xml,
          null,
          null,
          "",
          false,
        );
        Object.setPrototypeOf(clone, Object.getPrototypeOf(doc));
        if (deep)
          for (const child of doc.__detachedChildren)
            clone.appendChild(child.cloneNode(true));
        return clone;
      };
      if (suppliedDoctype) doc.appendChild(suppliedDoctype);
      if (xml) {
        if (qualifiedName)
          doc.appendChild(doc.createElementNS(namespace, qualifiedName));
      } else if (initialize) {
        const html = doc.appendChild(doc.createElement("html")),
          head = html.appendChild(doc.createElement("head"));
        if (title !== undefined) {
          const item = head.appendChild(doc.createElement("title"));
          item.textContent = String(title);
        }
        html.appendChild(doc.createElement("body"));
      }
      return doc;
    };
    return detachedDocument;
  },
});
