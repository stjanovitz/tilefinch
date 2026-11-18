/* DOM Standard traversal: NodeFilter, TreeWalker and NodeIterator.
   Loaded lazily on first use of any of those names or of
   Document.prototype.createTreeWalker/createNodeIterator, so pages that never
   walk the tree pay nothing for it at bootstrap. */
(() => {
  /* Loaded after the page may have replaced these globals: build
     errors, promises and platform objects with the constructors
     hardening.js captured. */
  const { defineProperty, Error, TypeError, DOMException, WeakMap,
    WeakRef } = globalThis.__tilefinchEventInterfaces;
  const wrap = globalThis.__tilefinchWrap,
    __tilefinchTraverse = globalThis.__tilefinchTraverse,
    nativeDescendants = globalThis.__tilefinchDescendants,
    /* Loaded after the page may have replaced the global. */
    { Document, Node } = globalThis.__tilefinchEventInterfaces;
  if (typeof wrap !== "function" || typeof __tilefinchTraverse !== "function")
    throw new Error("DOM traversal bridge is unavailable");
  /* Both walkers are live: every step starts from the current DOM, so
     mutations made while walking (including by the filter) are observed.
     Steps walk native links through __tilefinchTraverse, which passes over
     nodes that whatToShow excludes without wrapping them. Script-only nodes
     walk the DOM accessors instead, and a tree the native step cannot walk
     steps through its bounded descendant collection. */
  const FILTER_ACCEPT = 1,
    FILTER_REJECT = 2,
    FILTER_SKIP = 3,
    nodeFilterConstants = {
      __proto__: null,
      FILTER_ACCEPT,
      FILTER_REJECT,
      FILTER_SKIP,
      SHOW_ALL: 0xffffffff,
      SHOW_ELEMENT: 0x1,
      SHOW_ATTRIBUTE: 0x2,
      SHOW_TEXT: 0x4,
      SHOW_CDATA_SECTION: 0x8,
      SHOW_ENTITY_REFERENCE: 0x10,
      SHOW_ENTITY: 0x20,
      SHOW_PROCESSING_INSTRUCTION: 0x40,
      SHOW_COMMENT: 0x80,
      SHOW_DOCUMENT: 0x100,
      SHOW_DOCUMENT_TYPE: 0x200,
      SHOW_DOCUMENT_FRAGMENT: 0x400,
      SHOW_NOTATION: 0x800,
    };
  const NodeFilter = function NodeFilter() {
    throw new TypeError("Illegal constructor");
  };
  for (const name in nodeFilterConstants)
    defineProperty(NodeFilter, name, { __proto__: null, enumerable: true,
      value: nodeFilterConstants[name] });
  globalThis.NodeFilter = NodeFilter;
  /* Script hit testing shares one paint-order path for both point APIs. */
  const hitTestElements = (owner, x, y) => {
    x = Number(x);
    y = Number(y);
    if (!Number.isFinite(x) || !Number.isFinite(y))
      throw new TypeError("Coordinates must be finite");
    if (x < 0 || y < 0 || x >= innerWidth || y >= innerHeight) return [];
    const hits = [], ancestors = new Map();
    const positioned = style => /^(?:relative|absolute|fixed|sticky)$/.test(style.position);
    const placement = (node, style, order = 0) => {
      const z = Number.parseInt(style.zIndex, 10) || 0;
      const phase = z < 0 ? -1 : z > 0 || positioned(style) ? 1 : 0;
      return {node, z, phase, order};
    };
    const insideClip = (clip, rect) => {
      const match = /^inset\((.*?)\)$/i.exec(clip);
      if (match) {
        const parts = match[1].split(/\s+round\s+/i),
          tokens = parts[0].trim().split(/\s+/);
        if (tokens.length > 4 || !tokens.every(t => /^-?(?:\d+(?:\.\d+)?|\.\d+)(?:px|%)?$/.test(t)))
          return true;
        const [top, right = top, bottom = top, left = right] = tokens;
        const pixels = (v, size) => parseFloat(v) * (v.endsWith("%") ? size / 100 : 1);
        let l = pixels(left, rect.width), r = pixels(right, rect.width),
          t = pixels(top, rect.height), b = pixels(bottom, rect.height);
        if (l + r > rect.width) { l *= rect.width / (l + r); r = rect.width - l; }
        if (t + b > rect.height) { t *= rect.height / (t + b); b = rect.height - t; }
        const le = rect.left + l, to = rect.top + t,
          ri = rect.right - r, bo = rect.bottom - b;
        if (x < le || x >= ri || y < to || y >= bo) return false;
        /* The raster uses a uniform radius for round inset corners. */
        const radius = Math.min(parseFloat(parts[1]) || 0, (ri - le) / 2, (bo - to) / 2);
        if (radius > 0) {
          const dx = x - Math.max(le + radius, Math.min(ri - radius, x)),
            dy = y - Math.max(to + radius, Math.min(bo - radius, y));
          return dx * dx + dy * dy <= radius * radius;
        }
        return true;
      }
      if (/^circle\(/i.test(clip)) {
        const dx = x - rect.left - rect.width / 2, dy = y - rect.top - rect.height / 2,
          radius = Math.min(rect.width, rect.height) / 2;
        return dx * dx + dy * dy <= radius * radius;
      }
      return true;
    };
    const paintPath = (element, own) => {
      const path = [];
      let containing = "";
      for (let at = element, steps = 0; at && steps < 256;
           at = at.parentElement, steps++) {
        let info = ancestors.get(at);
        if (!info) {
          const style = at === element ? own : getComputedStyle(at),
            clip = style.getPropertyValue("clip-path"),
            transform = style.getPropertyValue("transform"),
            opacity = style.getPropertyValue("opacity"),
            context = (opacity && Number(opacity) < 1) || (transform && transform !== "none") ||
              (clip && clip !== "none") || style.position === "fixed" || style.position === "sticky" ||
              style.zIndex !== "auto" && positioned(style) || style.isolation === "isolate";
          const clips = v => /^(?:hidden|clip|scroll|auto|overlay)$/.test(v);
          info = {clip, key: context ? placement(at, style) : null,
            clipX: clips(style.overflowX), clipY: clips(style.overflowY),
            position: style.position, transformed: transform && transform !== "none",
            overflowRect: null};
          ancestors.set(at, info);
        }
        if (info.clip && info.clip !== "none" && !insideClip(info.clip, at.getBoundingClientRect()))
          return null;
        if (at !== element) {
          if (info.transformed || containing === "absolute" && info.position !== "static")
            containing = "";
          if (!containing && (info.clipX || info.clipY) &&
              at !== owner.documentElement && at !== owner.body) {
            let edges = info.overflowRect;
            if (!edges) {
              const r = at.getBoundingClientRect(), sx = r.width / (at.offsetWidth || r.width || 1),
                sy = r.height / (at.offsetHeight || r.height || 1),
                left = r.left + at.clientLeft * sx, top = r.top + at.clientTop * sy;
              info.overflowRect = edges = [left, top, left + at.clientWidth * sx,
                top + at.clientHeight * sy];
            }
            if (info.clipX && (x < edges[0] || x >= edges[2]) ||
                info.clipY && (y < edges[1] || y >= edges[3])) return null;
          }
        }
        if (!containing && (info.position === "absolute" || info.position === "fixed"))
          containing = info.position;
        if (info.key && at !== element) path.push(info.key);
      }
      return path.reverse();
    };
    let sequence = 0;
    for (const element of owner.querySelectorAll("*")) {
      const style = getComputedStyle(element);
      if (
        style.display === "none" ||
        style.visibility === "hidden" ||
        style.pointerEvents === "none" ||
        (!element.firstChild &&
          !Number.isFinite(parseFloat(style.height)) &&
          style.position === "static")
      )
        continue;
      const rect = element.getBoundingClientRect();
      let path;
      const add = paintStyle => {
        if (path === undefined) path = paintPath(element, style);
        if (path) hits.push({element, path, key: placement(element, paintStyle, sequence++)});
      };
      if (
        rect.width > 0 &&
        rect.height > 0 &&
        x >= rect.left &&
        x <= rect.right &&
        y >= rect.top &&
        y <= rect.bottom
      ) add(style);
      for (const pseudo of ["::before", "::after"]) {
        const pseudoStyle = getComputedStyle(element, pseudo);
        if (
          pseudoStyle.content === "none" ||
          pseudoStyle.display === "none" ||
          pseudoStyle.visibility === "hidden" ||
          pseudoStyle.pointerEvents === "none"
        )
          continue;
        const width = parseFloat(pseudoStyle.width),
          height = parseFloat(pseudoStyle.height);
        if (!(width > 0 && height > 0)) continue;
        const left = rect.left + (parseFloat(pseudoStyle.left) || 0),
          top = rect.top + (parseFloat(pseudoStyle.top) || 0);
        if (
          x >= left &&
          x <= left + width &&
          y >= top &&
          y <= top + height
        ) add(pseudoStyle);
      }
    }
    hits.sort((left, right) => {
      const count = Math.min(left.path.length, right.path.length);
      let index = 0;
      while (index < count && left.path[index].node === right.path[index].node) index++;
      const a = left.path[index] || left.key, b = right.path[index] || right.key;
      if (a.node === b.node)
        return left.path.length - right.path.length || a.order - b.order;
      return a.phase - b.phase || a.z - b.z ||
        (a.node.compareDocumentPosition(b.node) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1);
    });
    return [...new Set(hits.reverse().map(hit => hit.element))];
  };
  {
    const nativeTraverse = __tilefinchTraverse,
      traversalStepLimit = 1 << 20,
      traversalStates = new WeakMap(),
      traversalState = (object, kind) => {
        const state = traversalStates.get(object);
        if (!state || state.kind !== kind)
          throw new TypeError("Illegal invocation");
        return state;
      },
      /* Accessors tolerant of script-only documents whose children are
         exposed only through childNodes. */
      treeParent = (node) => (node === document ? null : node.parentNode ?? null),
      treeFirstChild = (node) => {
        const child = node.firstChild;
        if (child !== undefined) return child;
        const children = node.childNodes;
        return children?.length ? children[0] : null;
      },
      treeLastChild = (node) => {
        const child = node.lastChild;
        if (child !== undefined) return child;
        const children = node.childNodes;
        return children?.length ? children[children.length - 1] : null;
      },
      treeNextSibling = (node) => node.nextSibling ?? null,
      treePreviousSibling = (node) => node.previousSibling ?? null,
      shows = (whatToShow, node) => {
        const type = Number(node?.nodeType);
        return type >= 1 && type <= 12 && ((whatToShow >>> (type - 1)) & 1) === 1;
      },
      nativeHandle = (node) => {
        if (node === document) return 0;
        const handle = node?.__handle;
        return typeof handle === "number" && handle > 0 ? handle : -1;
      },
      /* Modes: 0 = following, 1 = following without entering `from`,
         2 = preceding. Returns undefined when only the accessors can
         answer (script-only nodes), or virtualTree when the native step
         declines the tree. */
      virtualTree = {},
      nativeStep = (root, from, whatToShow, mode) => {
        const rootHandle = nativeHandle(root),
          fromHandle =
            from === root ? 0 : from === document ? -1 : nativeHandle(from);
        if (rootHandle < 0 || fromHandle < 0) return undefined;
        let result = nativeTraverse(rootHandle, fromHandle, whatToShow, mode);
        for (let resumed = 0; result <= -5 && resumed < 4096; resumed++)
          result = nativeTraverse(
            rootHandle,
            -result - 4,
            whatToShow,
            mode === 1 ? 0 : mode,
          );
        /* A full node-handle table throws a RangeError from the step. */
        if (result === -2) return virtualTree;
        if (result === -1) return root;
        if (result === -3) return document;
        return result > 0 ? wrap(result) : null;
      },
      followingNode = (root, node, skipChildren) => {
        if (!skipChildren) {
          const child = treeFirstChild(node);
          if (child) return child;
        }
        for (
          let at = node, steps = 0;
          at && steps < traversalStepLimit;
          at = treeParent(at), steps++
        ) {
          if (at === root) return null;
          const sibling = treeNextSibling(at);
          if (sibling) return sibling;
        }
        return null;
      },
      precedingNode = (root, node) => {
        if (node === root) return null;
        let previous = treePreviousSibling(node);
        if (!previous) return treeParent(node);
        for (let steps = 0; steps < traversalStepLimit; steps++) {
          const last = treeLastChild(previous);
          if (!last) return previous;
          previous = last;
        }
        return previous;
      },
      /* A tree the native step declines steps through the bounded
         descendant collection instead. */
      snapshotStep = (state, from, mode) => {
        const root = state.root,
          whatToShow = state.whatToShow;
        let cache = state.snapshot;
        if (!cache) {
          const handle =
            root === document ? document.documentElement?.__handle : root.__handle;
          cache = state.snapshot = handle
            ? nativeDescendants(handle, whatToShow)
                .map((value) => (value instanceof Node ? value : wrap(value)))
                .filter((node) => node && node !== root)
            : [];
        }
        if (mode === 2) {
          if (from === root) return null;
          const index = cache.indexOf(from);
          if (index > 0) return cache[index - 1];
          return index === 0 && shows(whatToShow, root) ? root : null;
        }
        if (from === root) return mode === 1 ? null : cache[0] || null;
        let index = cache.indexOf(from) + 1;
        if (index === 0) return null;
        if (mode === 1)
          while (index < cache.length && from.contains?.(cache[index])) index++;
        return cache[index] || null;
      },
      step = (state, from, mode) => {
        const root = state.root,
          whatToShow = state.whatToShow;
        const native = nativeStep(root, from, whatToShow, mode);
        if (native === virtualTree) return snapshotStep(state, from, mode);
        if (native !== undefined) return native;
        let at = from;
        for (let steps = 0; steps < traversalStepLimit; steps++) {
          at =
            mode === 2
              ? precedingNode(root, at)
              : followingNode(root, at, steps === 0 && mode === 1);
          if (!at) return null;
          if (shows(whatToShow, at)) return at;
          if (mode === 2 && at === root) return null;
        }
        return null;
      },
      /* "Call a user object's operation" on a node whatToShow admits. */
      invokeFilter = (state, node) => {
        const filter = state.filter;
        if (filter === null) return FILTER_ACCEPT;
        if (state.active)
          throw new DOMException(
            "The traversal filter is already running",
            "InvalidStateError",
          );
        state.active = true;
        try {
          let result;
          if (typeof filter === "function") result = filter.call(undefined, node);
          else {
            const acceptNode = filter.acceptNode;
            if (typeof acceptNode !== "function")
              throw new TypeError("NodeFilter.acceptNode is not a function");
            result = acceptNode.call(filter, node);
          }
          return Number(result) & 0xffff;
        } finally {
          state.active = false;
        }
      },
      filterNode = (state, node) => {
        if (state.active)
          throw new DOMException(
            "The traversal filter is already running",
            "InvalidStateError",
          );
        return shows(state.whatToShow, node)
          ? invokeFilter(state, node)
          : FILTER_SKIP;
      },
      traversalArguments = (root, whatToShow, filter) => {
        if (!(root instanceof Node)) throw new TypeError("Node required");
        if (filter !== null && filter !== undefined && typeof filter !== "object" &&
            typeof filter !== "function")
          throw new TypeError("NodeFilter must be an object or function");
        return {
          root,
          whatToShow: whatToShow === undefined ? 0xffffffff : whatToShow >>> 0,
          filter: filter ?? null,
          active: false,
        };
      };
    const traverseChildren = (walker, first) => {
      const state = traversalState(walker, "TreeWalker");
      let node = first
        ? treeFirstChild(state.current)
        : treeLastChild(state.current);
      for (let steps = 0; node && steps < traversalStepLimit; steps++) {
        const result = filterNode(state, node);
        if (result === FILTER_ACCEPT) {
          state.current = node;
          return node;
        }
        if (result === FILTER_SKIP) {
          const child = first ? treeFirstChild(node) : treeLastChild(node);
          if (child) {
            node = child;
            continue;
          }
        }
        for (; node && steps < traversalStepLimit; steps++) {
          const sibling = first
            ? treeNextSibling(node)
            : treePreviousSibling(node);
          if (sibling) {
            node = sibling;
            break;
          }
          const parent = treeParent(node);
          if (!parent || parent === state.root || parent === state.current)
            return null;
          node = parent;
        }
      }
      return null;
    };
    const traverseSiblings = (walker, next) => {
      const state = traversalState(walker, "TreeWalker");
      let node = state.current;
      if (node === state.root) return null;
      for (let steps = 0; steps < traversalStepLimit; steps++) {
        let sibling = next ? treeNextSibling(node) : treePreviousSibling(node);
        for (; sibling && steps < traversalStepLimit; steps++) {
          node = sibling;
          const result = filterNode(state, node);
          if (result === FILTER_ACCEPT) {
            state.current = node;
            return node;
          }
          sibling = next ? treeFirstChild(node) : treeLastChild(node);
          if (result === FILTER_REJECT || !sibling)
            sibling = next ? treeNextSibling(node) : treePreviousSibling(node);
        }
        node = treeParent(node);
        if (!node || node === state.root) return null;
        if (filterNode(state, node) === FILTER_ACCEPT) return null;
      }
      return null;
    };
    class TreeWalker {
      constructor() {
        throw new TypeError("Illegal constructor");
      }
      get root() {
        return traversalState(this, "TreeWalker").root;
      }
      get whatToShow() {
        return traversalState(this, "TreeWalker").whatToShow;
      }
      get filter() {
        return traversalState(this, "TreeWalker").filter;
      }
      get currentNode() {
        return traversalState(this, "TreeWalker").current;
      }
      set currentNode(node) {
        const state = traversalState(this, "TreeWalker");
        if (!(node instanceof Node)) throw new TypeError("Node required");
        state.current = node;
      }
      parentNode() {
        const state = traversalState(this, "TreeWalker");
        let node = state.current;
        for (
          let steps = 0;
          node && node !== state.root && steps < traversalStepLimit;
          steps++
        ) {
          node = treeParent(node);
          if (node && filterNode(state, node) === FILTER_ACCEPT) {
            state.current = node;
            return node;
          }
        }
        return null;
      }
      firstChild() {
        return traverseChildren(this, true);
      }
      lastChild() {
        return traverseChildren(this, false);
      }
      previousSibling() {
        return traverseSiblings(this, false);
      }
      nextSibling() {
        return traverseSiblings(this, true);
      }
      previousNode() {
        const state = traversalState(this, "TreeWalker");
        let node = state.current;
        for (
          let steps = 0;
          node !== state.root && steps < traversalStepLimit;
          steps++
        ) {
          let sibling = treePreviousSibling(node);
          while (sibling && steps < traversalStepLimit) {
            node = sibling;
            let result = filterNode(state, node);
            for (
              let child = treeLastChild(node);
              result !== FILTER_REJECT && child && steps < traversalStepLimit;
              child = treeLastChild(node), steps++
            ) {
              node = child;
              result = filterNode(state, node);
            }
            if (result === FILTER_ACCEPT) {
              state.current = node;
              return node;
            }
            sibling = treePreviousSibling(node);
            steps++;
          }
          const parent = treeParent(node);
          if (node === state.root || !parent) return null;
          node = parent;
          if (filterNode(state, node) === FILTER_ACCEPT) {
            state.current = node;
            return node;
          }
        }
        return null;
      }
      nextNode() {
        const state = traversalState(this, "TreeWalker");
        if (state.active)
          throw new DOMException(
            "The traversal filter is already running",
            "InvalidStateError",
          );
        let node = state.current,
          result = FILTER_ACCEPT;
        /* Template instantiation walks every cloned node this way. */
        if (state.filter === null) {
          const root = state.root,
            rootHandle = root === document ? 0 : root.__handle,
            from = node === root ? 0 : node === document ? -1 : node.__handle;
          if (rootHandle >= 0 && from >= 0) {
            const found = nativeTraverse(rootHandle, from, state.whatToShow, 0);
            if (found === 0) return null;
            if (found > 0) return (state.current = wrap(found));
          }
          const next = step(state, node, 0);
          if (next) state.current = next;
          return next;
        }
        for (let steps = 0; steps < traversalStepLimit; steps++) {
          /* Nodes whatToShow excludes are FILTER_SKIP: the walk enters them
             without consulting the filter, so the native step passes over
             them; a rejected node's subtree is not entered. */
          const next = step(state, node, result === FILTER_REJECT ? 1 : 0);
          if (!next) return null;
          node = next;
          result = invokeFilter(state, node);
          if (result === FILTER_ACCEPT) {
            state.current = node;
            return node;
          }
        }
        return null;
      }
    }
    /* Live iterators whose reference must move when a node containing it
       is removed ("NodeIterator pre-removing steps"). Weakly held and
       bounded; an iterator past the bound keeps working but is not
       adjusted for removals. */
    const liveIterators = [],
      liveIteratorLimit = 64,
      weakIterators = typeof WeakRef === "function",
      holdIterator = (state) => {
        for (let index = liveIterators.length - 1; index >= 0; index--)
          if (weakIterators && !liveIterators[index].deref())
            liveIterators.splice(index, 1);
        if (liveIterators.length >= liveIteratorLimit) liveIterators.shift();
        liveIterators.push(weakIterators ? new WeakRef(state) : { deref: () => state });
      },
      isInclusiveAncestor = (ancestor, node) => {
        for (
          let at = node, steps = 0;
          at && steps < traversalStepLimit;
          at = treeParent(at), steps++
        )
          if (at === ancestor) return true;
        return false;
      },
      lastInclusiveDescendant = (node) => {
        for (let steps = 0; steps < traversalStepLimit; steps++) {
          const last = treeLastChild(node);
          if (!last) return node;
          node = last;
        }
        return node;
      },
      preRemove = (state, removed, parent, previous, next) => {
        if (removed === state.root || !isInclusiveAncestor(removed, state.reference))
          return;
        if (state.pointerBefore) {
          let following = next;
          for (
            let at = parent, steps = 0;
            !following && at && at !== state.root && steps < traversalStepLimit;
            at = treeParent(at), steps++
          )
            following = treeNextSibling(at);
          if (following && isInclusiveAncestor(state.root, following)) {
            state.reference = following;
            return;
          }
          state.pointerBefore = false;
        }
        const reference = previous ? lastInclusiveDescendant(previous) : parent;
        if (reference) state.reference = reference;
      };
    globalThis.__tilefinchSetNodeIteratorHooks?.({
      hitTestElements,
      active: () => liveIterators.length > 0,
      /* Called after a removal with the removed nodes (a contiguous run,
         removed in order) and the siblings around the run. */
      removed(parent, removedNodes, previousSibling, nextSibling) {
        if (!liveIterators.length || !parent) return;
        const count = Math.min(removedNodes.length, 256);
        for (const reference of liveIterators.slice()) {
          const state = reference.deref();
          if (!state) continue;
          for (let index = 0; index < count; index++)
            preRemove(
              state,
              removedNodes[index],
              parent,
              previousSibling ?? null,
              index + 1 < count ? removedNodes[index + 1] : nextSibling ?? null,
            );
        }
      },
    });
    const iteratorTraverse = (iterator, forward) => {
      const state = traversalState(iterator, "NodeIterator");
      if (state.active)
        throw new DOMException(
          "The traversal filter is already running",
          "InvalidStateError",
        );
      let node = state.reference,
        beforeNode = state.pointerBefore;
      for (let steps = 0; steps < traversalStepLimit; steps++) {
        if (forward) {
          if (!beforeNode) {
            node = step(state, node, 0);
            if (!node) return null;
          } else beforeNode = false;
        } else if (beforeNode) {
          node = step(state, node, 2);
          if (!node) return null;
        } else beforeNode = true;
        if (filterNode(state, node) === FILTER_ACCEPT) {
          state.reference = node;
          state.pointerBefore = beforeNode;
          return node;
        }
      }
      return null;
    };
    class NodeIterator {
      constructor() {
        throw new TypeError("Illegal constructor");
      }
      get root() {
        return traversalState(this, "NodeIterator").root;
      }
      get referenceNode() {
        return traversalState(this, "NodeIterator").reference;
      }
      get pointerBeforeReferenceNode() {
        return traversalState(this, "NodeIterator").pointerBefore;
      }
      get whatToShow() {
        return traversalState(this, "NodeIterator").whatToShow;
      }
      get filter() {
        return traversalState(this, "NodeIterator").filter;
      }
      nextNode() {
        return iteratorTraverse(this, true);
      }
      previousNode() {
        return iteratorTraverse(this, false);
      }
      detach() {}
    }
    for (const constructor of [TreeWalker, NodeIterator])
      defineProperty(constructor.prototype, Symbol.toStringTag, {
        configurable: true,
        value: constructor.name,
      });
    globalThis.TreeWalker = TreeWalker;
    globalThis.NodeIterator = NodeIterator;
    const createTraversal = (kind, prototype, root, whatToShow, filter) => {
      const state = traversalArguments(root, whatToShow, filter),
        object = Object.create(prototype);
      state.kind = kind;
      if (kind === "TreeWalker") state.current = root;
      else {
        state.reference = root;
        state.pointerBefore = true;
        holdIterator(state);
      }
      traversalStates.set(object, state);
      return object;
    };
    Document.prototype.createTreeWalker = function createTreeWalker(
      root,
      whatToShow = 0xffffffff,
      filter = null,
    ) {
      return createTraversal(
        "TreeWalker",
        TreeWalker.prototype,
        root,
        whatToShow,
        filter,
      );
    };
    Document.prototype.createNodeIterator = function createNodeIterator(
      root,
      whatToShow = 0xffffffff,
      filter = null,
    ) {
      return createTraversal(
        "NodeIterator",
        NodeIterator.prototype,
        root,
        whatToShow,
        filter,
      );
    };
    delete document.createTreeWalker;
    delete document.createNodeIterator;
  }
})();
