/* getBoundingClientRect() for an element native layout holds no
   authoritative box for (one outside the last layout, or under a hidden
   ancestor): a bounded synthesis from computed style, the retained
   geometry the element and its neighbours do have, and the viewport.
   Built the first time a page measures such an element, so a realm whose
   measurements are all answered by layout pays nothing for it. dom.js
   passes the realm values it captured before page script ran; everything
   else here is read when a measurement runs, exactly as it was when this
   lived in dom.js (getComputedStyle, DOMRect and the viewport globals). */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateLayoutlessGeometry", {
  __proto__: null,
  configurable: true,
  value({
    String,
    Element,
    ShadowRoot,
    ancestorLimit,
    boundedAncestorPath,
    shadowRootForHost,
  }) {
    /* `g` is the element's retained (non-authoritative) geometry and
       `style` its computed style, whose display is neither none nor
       contents. */
    return (element, g, style) => {
      const retainedWidth = Number(g.width) || 0,
        retainedHeight = Number(g.height) || 0,
        parentElement = element.parentElement,
        /* A retained box at (0, 0) is a valid authoritative position. Only
           synthesize flow when the native layout supplied no dimensions;
           otherwise a previous sibling can shift a correct box by its own
           height. */
        needsFlowFallback =
          retainedWidth <= 0 && retainedHeight <= 0,
        positionedContainingElement =
          style.position === "absolute" || style.position === "fixed"
            ? boundedAncestorPath(
                element.parentElement,
                (node) => node.parentElement,
              ).find((at) => {
                const candidate = getComputedStyle(at),
                  transformed =
                    (candidate.getPropertyValue("transform") &&
                      candidate.getPropertyValue("transform") !== "none") ||
                    String(candidate.getPropertyValue("will-change"))
                      .split(/\s*,\s*/)
                      .includes("transform") ||
                    (candidate.getPropertyValue("perspective") &&
                      candidate.getPropertyValue("perspective") !== "none") ||
                    (candidate.getPropertyValue("filter") &&
                      candidate.getPropertyValue("filter") !== "none") ||
                    String(candidate.contain).split(/\s+/).includes("paint");
                return transformed ||
                  (style.position === "absolute" &&
                    candidate.position !== "static");
              }) || null
            : null,
        containingElement =
          style.position === "absolute" || style.position === "fixed"
            ? positionedContainingElement
            : parentElement,
        containingGeometry =
          containingElement?.__tilefinchGeometryValue?.() || null,
        containingRect = containingGeometry
          ? new DOMRect(
              Number(containingGeometry.x) || 0,
              Number(containingGeometry.y) || 0,
              Number(containingGeometry.width) || 0,
              Number(containingGeometry.height) || 0,
            )
          : new DOMRect(0, 0, innerWidth, innerHeight),
        cssPixels = (value, reference, viewportFallback) => {
          value = String(value || "").trim();
          const number = parseFloat(value);
          if (!Number.isFinite(number)) return NaN;
          if (value.endsWith("%"))
            return ((reference || viewportFallback) * number) / 100;
          if (value.endsWith("em") || value.endsWith("rem"))
            return number * 16;
          return number;
        };
      let x = Number(g.x) || 0,
        y = Number(g.y) || 0,
        width = retainedWidth,
        height = retainedHeight;
      const cssWidth = cssPixels(
          style.width,
          containingRect.width,
          innerWidth,
        ),
        /* A percentage height against a containing block whose height is
           auto behaves as auto, as it does in layout. */
        cssHeight =
          style.position !== "absolute" &&
          style.position !== "fixed" &&
          String(style.height).trim().endsWith("%") &&
          containingElement &&
          getComputedStyle(containingElement).height === "auto"
            ? NaN
            : cssPixels(style.height, containingRect.height, innerHeight);
      if (Number.isFinite(cssWidth) && cssWidth > 0) width = cssWidth;
      if (Number.isFinite(cssHeight) && cssHeight > 0) height = cssHeight;
      if (
        height <= 0 &&
        !Number.isFinite(cssHeight) &&
        style.position !== "absolute" &&
        style.position !== "fixed"
      ) {
        /* A shadow root is represented natively by a display:contents
           carrier. Until native layout has consumed a dynamic shadow
           mutation, derive the host's auto block size from its in-flow
           descendants. Keep both traversal and nesting explicitly bounded:
           geometry is observable author work and must not turn a deep
           hostile tree into unbounded recursion on PSP. */
        const state = { remaining: 128 },
          measureAutoContentHeight = (container, depth) => {
            if (depth >= 16 || state.remaining <= 0) return 0;
            const shadow =
              container instanceof Element
                ? shadowRootForHost(container)
                : null;
            if (shadow)
              return measureAutoContentHeight(shadow, depth + 1);
            let blocks = 0,
              line = 0,
              child = container?.firstChild || null;
            for (
              let siblings = 0;
              child && siblings < 128 && state.remaining > 0;
              child = child.nextSibling, siblings++
            ) {
              state.remaining--;
              if (
                !(child instanceof Element) &&
                !(child instanceof ShadowRoot)
              )
                continue;
              if (child instanceof ShadowRoot) {
                const contents = measureAutoContentHeight(child, depth + 1);
                line = Math.max(line, contents);
                continue;
              }
              const childStyle = getComputedStyle(child);
              if (
                childStyle.display === "none" ||
                childStyle.position === "absolute" ||
                childStyle.position === "fixed"
              )
                continue;
              const childGeometry =
                  child.__tilefinchGeometryValue?.() || {},
                authoredHeight = cssPixels(
                  childStyle.height,
                  containingRect.height,
                  innerHeight,
                ),
                marginTop =
                  parseFloat(childStyle.getPropertyValue("margin-top")) || 0,
                marginBottom =
                  parseFloat(childStyle.getPropertyValue("margin-bottom")) ||
                  0,
                padding =
                  (parseFloat(
                    childStyle.getPropertyValue("padding-top"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("padding-bottom"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("border-top-width"),
                  ) || 0) +
                  (parseFloat(
                    childStyle.getPropertyValue("border-bottom-width"),
                  ) || 0);
              let childHeight = Number(childGeometry.height) || 0;
              if (Number.isFinite(authoredHeight) && authoredHeight > 0)
                childHeight = authoredHeight + padding;
              else if (childHeight <= 0)
                childHeight =
                  measureAutoContentHeight(child, depth + 1) + padding;
              const extent = marginTop + childHeight + marginBottom,
                inline =
                  childStyle.display === "inline" ||
                  childStyle.display === "inline-block" ||
                  childStyle.display === "inline-flex" ||
                  childStyle.display === "inline-grid";
              if (inline) line = Math.max(line, extent);
              else {
                blocks += line + extent;
                line = 0;
              }
            }
            return blocks + line;
          };
        height = measureAutoContentHeight(element, 0);
      }
      if (
        needsFlowFallback &&
        (element === document.documentElement || element === document.body)
      ) {
        x = 0;
        y = 0;
        if (width <= 0) width = innerWidth;
        if (height <= 0)
          height =
            element === document.documentElement
              ? innerHeight
              : parseFloat(style.height) || innerHeight;
      } else if (
        needsFlowFallback &&
        style.position !== "absolute" &&
        style.position !== "fixed"
      ) {
        const parentRect = containingRect,
          marginLeft =
            parseFloat(style.getPropertyValue("margin-left")) || 0,
          marginTop =
            parseFloat(style.getPropertyValue("margin-top")) || 0;
        let lineX = parentRect.left,
          lineY = parentRect.top,
          lineHeight = 0,
          blockBottom = parentRect.top;
        for (const sibling of element.parentElement?.childNodes || []) {
          if (sibling === element) break;
          if (!(sibling instanceof Element)) continue;
          const siblingStyle = getComputedStyle(sibling);
          if (
            siblingStyle.display === "none" ||
            siblingStyle.position === "absolute" ||
            siblingStyle.position === "fixed"
          )
            continue;
          const siblingGeometry =
              sibling.__tilefinchGeometryValue?.() || null,
            siblingRect = siblingGeometry
              ? new DOMRect(
                  Number(siblingGeometry.x) || 0,
                  Number(siblingGeometry.y) || 0,
                  Number(siblingGeometry.width) || 0,
                  Number(siblingGeometry.height) || 0,
                )
              : new DOMRect(),
            siblingBottom =
              parseFloat(
                siblingStyle.getPropertyValue("margin-bottom"),
              ) || 0,
            siblingRight =
              parseFloat(
                siblingStyle.getPropertyValue("margin-right"),
              ) || 0,
            inline =
              siblingStyle.display === "inline" ||
              siblingStyle.display === "inline-block";
          if (inline) {
            lineY = Math.max(lineY, blockBottom);
            lineX += siblingRect.width + siblingRight;
            lineHeight = Math.max(
              lineHeight,
              siblingRect.height + siblingBottom,
            );
          } else {
            if (lineHeight > 0) {
              blockBottom = Math.max(
                blockBottom,
                lineY + lineHeight,
              );
              lineX = parentRect.left;
              lineHeight = 0;
            }
            blockBottom = Math.max(
              blockBottom,
              siblingRect.bottom + siblingBottom,
            );
            lineY = blockBottom;
          }
        }
        const inline =
          style.display === "inline" || style.display === "inline-block";
        x = (inline ? lineX : parentRect.left) + marginLeft;
        y =
          (inline
            ? Math.max(lineY, blockBottom)
            : Math.max(blockBottom, lineY + lineHeight)) + marginTop;
        if (style.float === "right")
          x =
            parentRect.right -
            width -
            (parseFloat(style.getPropertyValue("margin-right")) || 0);
      }
      if (style.position === "absolute" || style.position === "fixed") {
        const left = cssPixels(
            style.left,
            containingRect.width,
            innerWidth,
          ),
          top = cssPixels(
            style.top,
            containingRect.height,
            innerHeight,
          ),
          origin =
            style.position === "fixed" && !element.offsetParent
              ? new DOMRect()
              : containingRect;
        if (Number.isFinite(left))
          x =
            origin.left +
            left +
            (parseFloat(style.getPropertyValue("margin-left")) || 0);
        else if (x === 0)
          x =
            origin.left +
            (parseFloat(style.getPropertyValue("margin-left")) || 0);
        if (Number.isFinite(top))
          y =
            origin.top +
            top +
            (parseFloat(style.getPropertyValue("margin-top")) || 0);
        else if (y === 0)
          y =
            origin.top +
            (parseFloat(style.getPropertyValue("margin-top")) || 0);
        /* Native retained geometry subtracts every scrolled DOM ancestor.
           CSS positioned descendants escape scroll containers between
           themselves and their containing block. Restore only those
           intervening scroll offsets; the containing block and its
           ancestors continue to move normally. */
        for (
          let at = element.parentElement, steps = 0;
          at && at !== positionedContainingElement && steps < ancestorLimit;
          at = at.parentElement, steps++
        ) {
          x += Number(at.scrollLeft) || 0;
          y += Number(at.scrollTop) || 0;
        }
      }
      const transform = String(style.transform || ""),
        pair = transform.match(
          /translate(?:3d)?\(\s*(-?[\d.]+)px(?:\s*,\s*(-?[\d.]+)px)?/,
        ),
        translateX = transform.match(
          /translateX\(\s*(-?[\d.]+)px/,
        ),
        translateY = transform.match(
          /translateY\(\s*(-?[\d.]+)px/,
        );
      x += Number(pair?.[1] || translateX?.[1]) || 0;
      y += Number(pair?.[2] || translateY?.[1]) || 0;
      if (needsFlowFallback) {
        const scrollAncestor =
          style.position === "absolute" || style.position === "fixed"
            ? element.offsetParent
            : element.parentElement;
        x -= Number(scrollAncestor?.scrollLeft) || 0;
        y -= Number(scrollAncestor?.scrollTop) || 0;
      }
      /* Oversized CSS boxes can overflow the viewport even before the
         next layout. Reporting their size here does not allocate a surface. */
      return new DOMRect(
        Number.isFinite(x) ? x : 0,
        Number.isFinite(y) ? y : 0,
        Number.isFinite(width) && width > 0 ? width : 0,
        Number.isFinite(height) && height > 0 ? height : 0,
      );
    };
  },
});
