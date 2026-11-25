/* Markup serialization in script: XMLSerializer, getHTML() on elements and
   shadow roots, HTMLElement outerText, and the innerHTML and outerHTML
   getters of script-side elements (the nodes of createDocument, new
   Document() and empty <iframe> documents). Loaded on the first use of any
   of those names, so a realm that never serializes pays nothing for it.
   Page nodes keep the native innerHTML serializer; this walks any node
   through its DOM accessors, iteratively, so a deep tree cannot exhaust
   the stack. compat.js takes __tilefinchMarkup for the script-side
   getters. */
(() => {
  /* Loaded after the page may have replaced these globals: build
     errors, promises and platform objects with the constructors
     hardening.js captured, and publish private hooks with its
     defineProperty. */
  const { defineProperty, TypeError, Map, Set } =
    globalThis.__tilefinchEventInterfaces;
  const XHTML = "http://www.w3.org/1999/xhtml",
    SVG = "http://www.w3.org/2000/svg",
    MATHML = "http://www.w3.org/1998/Math/MathML",
    XML = "http://www.w3.org/XML/1998/namespace",
    XLINK = "http://www.w3.org/1999/xlink",
    /* Loaded after the page may have replaced these globals: the
       interfaces hardening.js captured. */
    {
      Node: NodeType, ShadowRoot: ShadowRootType, DOMException: Exception,
      Element, HTMLElement,
    } = globalThis.__tilefinchEventInterfaces,
    pageDocument = document,
    words = (text) => new Set(text.split(" ")),
    voids = words(
      "area base basefont bgsound br col embed frame hr img input keygen " +
        "link meta param source track wbr",
    ),
    rawText = words("style script xmp iframe noembed noframes plaintext"),
    entities = {
      "&": "&amp;",
      "<": "&lt;",
      ">": "&gt;",
      '"': "&quot;",
      " ": "&nbsp;",
      "\t": "&#9;",
      "\n": "&#10;",
      "\r": "&#13;",
    },
    /* Chromium's entity sets: HTML text and attributes escape a no-break
       space; XML attributes escape tab, line feed and carriage return. */
    patterns = [/[&<> ]/g, /[&<>" ]/g, /[&<>]/g, /[&<>"\t\n\r]/g],
    escape = (text, set) =>
      String(text).replace(patterns[set], (c) => entities[c]),
    childrenOf = (node) =>
      node.namespaceURI === XHTML && node.localName === "template" &&
      node.content
        ? node.content.childNodes
        : node.childNodes,
    shadowTemplate = (root) =>
      '<template shadowrootmode="' + root.mode + '"' +
      (root.delegatesFocus ? ' shadowrootdelegatesfocus=""' : "") +
      (root.serializable ? ' shadowrootserializable=""' : "") +
      (root.clonable ? ' shadowrootclonable=""' : "") + ">",
    /* `root` itself when `self`, else its content (the children, after
       the shadow root getHTML() was asked to include, if it has one);
       `hosts` maps a host to that root. */
    markup = (root, xml, self = false, hosts = null) => {
      let out = "",
        visited = 0;
      const stack = [],
        pushChildren = (list, context, prefixes) => {
          for (let at = list.length - 1; at >= 0; at--)
            stack.push([list[at], context, prefixes]);
        },
        /* Pushes `node`'s content; returns the shadow root's opening
           template tag, for the caller to emit first. */
        pushContent = (node, context, prefixes) => {
          pushChildren(childrenOf(node), context, prefixes);
          const shadow = hosts?.get(node);
          if (!shadow) return "";
          stack.push("</template>");
          pushChildren(shadow.childNodes, context, prefixes);
          return shadowTemplate(shadow);
        };
      if (self) stack.push([root, null, null]);
      else if (!xml) out += pushContent(root, null, null);
      /* XML children start with no bindings, so each declares its own
         namespace, as in Chromium (WPT domparsing/innerhtml-03.xhtml). */
      else pushChildren(childrenOf(root), null, null);
      while (stack.length) {
        const item = stack.pop();
        if (typeof item === "string") {
          out += item;
          continue;
        }
        if (++visited > 1 << 20)
          throw new Exception("Markup exceeds implementation limits",
                              "NotSupportedError");
        const [node, context, inherited] = item;
        switch (node.nodeType) {
          case NodeType.ELEMENT_NODE:
            out += xml
              ? xmlElement(node, context, inherited, stack, pushChildren)
              : htmlElement(node, stack, pushContent);
            break;
          case NodeType.TEXT_NODE: {
            const parent = node.parentNode;
            out +=
              !xml && parent?.namespaceURI === XHTML &&
              (rawText.has(parent.localName) ||
                (parent.localName === "noscript" &&
                  parent.ownerDocument === pageDocument))
                ? node.data
                : escape(node.data, xml ? 2 : 0);
            break;
          }
          case NodeType.CDATA_SECTION_NODE:
            out += xml ? "<![CDATA[" + node.data + "]]>" : escape(node.data, 0);
            break;
          case NodeType.COMMENT_NODE:
            out += "<!--" + node.data + "-->";
            break;
          case NodeType.PROCESSING_INSTRUCTION_NODE:
            out += "<?" + node.target + " " + node.data + (xml ? "?>" : ">");
            break;
          case NodeType.DOCUMENT_TYPE_NODE:
            out += "<!DOCTYPE " + node.name + (!xml ? ">"
              : (node.publicId ? ' PUBLIC "' + node.publicId + '"'
                : node.systemId ? " SYSTEM" : "") +
                (node.systemId ? ' "' + node.systemId + '"' : "") + ">");
            break;
          case NodeType.DOCUMENT_NODE:
          case NodeType.DOCUMENT_FRAGMENT_NODE:
            pushChildren(node.childNodes, context, inherited);
            break;
        }
      }
      return out;
    },
    htmlElement = (node, stack, pushContent) => {
      const ns = node.namespaceURI,
        local = node.localName,
        tag =
          ns === XHTML || ns === SVG || ns === MATHML || !node.prefix
            ? local
            : node.prefix + ":" + local;
      let text = "<" + tag;
      for (const attribute of node.attributes || [])
        text += " " + attribute.name + '="' + escape(attribute.value, 1) + '"';
      text += ">";
      if (ns === XHTML && voids.has(local)) return text;
      stack.push("</" + tag + ">");
      return text + pushContent(node, null, null);
    },
    xmlElement = (node, context, inherited, stack, pushChildren) => {
      const ns = node.namespaceURI ?? null,
        prefix = node.prefix ?? null,
        local = node.localName,
        name = prefix ? prefix + ":" + local : local,
        attributes = [...(node.attributes || [])];
      let prefixes = inherited || Object.create(null),
        own = false,
        declared = "",
        generated = 0,
        ownDefault;
      const bind = (key, value) => {
        if (!own) {
          prefixes = Object.create(prefixes);
          own = true;
        }
        prefixes[key] = value;
      };
      for (const attribute of attributes) {
        const attributeName = attribute.name;
        if (attributeName === "xmlns") ownDefault = attribute.value;
        else if (attributeName.startsWith("xmlns:"))
          bind(attributeName.slice(6), attribute.value);
      }
      if (prefix) {
        if (prefix !== "xml" && prefixes[prefix] !== ns) {
          bind(prefix, ns);
          declared += " xmlns:" + prefix + '="' + escape(ns ?? "", 3) + '"';
        }
      } else if (ns !== context && ownDefault === undefined)
        declared += ' xmlns="' + escape(ns ?? "", 3) + '"';
      const childContext =
        ownDefault !== undefined ? ownDefault || null : prefix ? context : ns;
      let text = "";
      for (const attribute of attributes) {
        let attributeName = attribute.name,
          attributeNs = attribute.namespaceURI ?? null;
        const colon = attributeName.indexOf(":"),
          attributePrefix = colon > 0 ? attributeName.slice(0, colon) : null;
        /* A parsed SVG xlink:href can carry no namespace here: bind it. */
        if (attributeNs === null && attributePrefix === "xlink")
          attributeNs = XLINK;
        if (
          attributeNs !== null && attributeNs !== XML &&
          attributeName !== "xmlns" && attributePrefix !== "xmlns"
        ) {
          let chosen = attributePrefix;
          if (!chosen || prefixes[chosen] !== attributeNs) {
            chosen = null;
            for (const key in prefixes)
              if (prefixes[key] === attributeNs) {
                chosen = key;
                break;
              }
            if (!chosen) {
              if (attributePrefix && !(attributePrefix in prefixes))
                chosen = attributePrefix;
              else {
                do { chosen = "ns" + ++generated; }
                while (chosen in prefixes);
              }
              bind(chosen, attributeNs);
              /* Declared just before the attribute, as Chromium does. */
              text += " xmlns:" + chosen + '="' + escape(attributeNs, 3) +
                '"';
            }
          }
          attributeName =
            chosen + ":" + (colon > 0 ? attributeName.slice(colon + 1)
              : attributeName);
        }
        text += " " + attributeName + '="' + escape(attribute.value, 3) + '"';
      }
      text = "<" + name + declared + text;
      const children = childrenOf(node);
      if (!children.length)
        return text + (ns !== XHTML ? "/>"
          : voids.has(local) ? " />" : "></" + name + ">");
      stack.push("</" + name + ">");
      pushChildren(children, childContext, prefixes);
      return text + ">";
    },
    method = (target, name, value) =>
      defineProperty(target, name, {
        configurable: true,
        writable: true,
        value,
      });
  class XMLSerializer {
    serializeToString(node) {
      if (!(node instanceof NodeType))
        throw new TypeError(
          "Failed to execute 'serializeToString' on 'XMLSerializer': " +
            "parameter 1 is not of type 'Node'.",
        );
      return node.nodeType === NodeType.ATTRIBUTE_NODE
        ? ""
        : markup(node, true, true);
    }
  }
  globalThis.XMLSerializer = XMLSerializer;
  /* shadowRoots names the roots to include, open or closed. A root is
     never serializable here (attachShadow ignores that option and a root
     has no serializable attribute), so serializableShadowRoots adds none,
     and declarative roots are not parsed back (setHTMLUnsafe is
     innerHTML). Element's and ShadowRoot's getHTML are separate members,
     each refusing the other's receivers, as in Chromium. */
  const getHTML = (node, options) => {
    if (options != null && typeof options !== "object" &&
        typeof options !== "function")
      throw new TypeError("Failed to execute 'getHTML': " +
                          "parameter 1 is not of type 'GetHTMLOptions'.");
    let hosts = null;
    for (const root of options?.shadowRoots || []) {
      if (!(root instanceof ShadowRootType))
        throw new TypeError("Failed to execute 'getHTML': " +
                            "shadowRoots must hold ShadowRoot objects.");
      (hosts ||= new Map()).set(root.host, root);
    }
    /* HTML even in an XML document, whose innerHTML is XML. */
    return hosts || node.ownerDocument?.contentType !== "text/html"
      ? markup(node, false, false, hosts) : node.innerHTML;
  };
  method(Element.prototype, "getHTML", {
    getHTML(options = undefined) {
      if (!(this instanceof Element)) throw new TypeError("Illegal invocation");
      return getHTML(this, options);
    },
  }.getHTML);
  method(ShadowRootType.prototype, "getHTML", {
    getHTML(options = undefined) {
      if (!(this instanceof ShadowRootType))
        throw new TypeError("Illegal invocation");
      return getHTML(this, options);
    },
  }.getHTML);
  const mergeNext = (node) => {
    const next = node?.nextSibling;
    if (node?.nodeType === NodeType.TEXT_NODE &&
        next?.nodeType === NodeType.TEXT_NODE) {
      node.data += next.data;
      next.remove();
    }
  };
  defineProperty(HTMLElement.prototype, "outerText", {
    configurable: true,
    enumerable: true,
    get() {
      return this.innerText;
    },
    /* HTML's outerText setter: the text replaces the element, line breaks
       become <br>, and the text merges with neighbouring text. */
    set(value) {
      const parent = this.parentNode;
      if (!parent || parent.nodeType === NodeType.DOCUMENT_NODE)
        throw new Exception(
          "The element has no parent.", "NoModificationAllowedError");
      const owner = this.ownerDocument,
        next = this.nextSibling,
        previous = this.previousSibling,
        fragment = owner.createDocumentFragment();
      String(value).split(/\r\n|\r|\n/).forEach((line, at) => {
        if (at) fragment.appendChild(owner.createElement("br"));
        if (line) fragment.appendChild(owner.createTextNode(line));
      });
      if (!fragment.firstChild)
        fragment.appendChild(owner.createTextNode(""));
      parent.replaceChild(fragment, this);
      mergeNext(next ? next.previousSibling : null);
      mergeNext(previous);
    },
  });
  defineProperty(globalThis, "__tilefinchMarkup", { __proto__: null,
    configurable: true,
    value: markup,
  });
})();
