"use strict";

/* Serialized into the captured page. Only the explicitly selected scroll
   position may change; no layout, overflow, or content is rewritten. */
function inspectScrollSurface({ selector = null, top = null, fraction = null, limit = 0, discover = false }) {
  if (discover) {
    const result = [], walker = document.createTreeWalker(document, NodeFilter.SHOW_ELEMENT);
    for (let visited = 0, node = walker.nextNode(); node && visited < 65536; node = walker.nextNode(), visited++) {
      if (node.scrollHeight <= node.clientHeight + 2 || !node.clientHeight) continue;
      const style = getComputedStyle(node), rect = node.getBoundingClientRect();
      if (!["auto", "scroll"].includes(style.overflowY) || rect.bottom <= 0 || rect.top >= innerHeight
          || rect.right <= 0 || rect.left >= innerWidth || !node.clientWidth
          || !node.checkVisibility({opacityProperty:true, visibilityProperty:true, contentVisibilityAuto:true})) continue;
      let selector = node.id ? '#' + CSS.escape(node.id) : '';
      if (!selector || document.querySelectorAll(selector).length !== 1) {
        const parts = [];
        for (let current = node, depth = 0; current && depth < 32; current = current.parentElement, depth++) {
          const peers = current.parentElement ? [...current.parentElement.children].filter(e => e.tagName === current.tagName) : [current];
          parts.unshift(current.localName + ':nth-of-type(' + (peers.indexOf(current)+1) + ')');
        }
        selector = parts.join(' > ');
      }
      result.push({selector, height:node.clientHeight, maximum:node.scrollHeight-node.clientHeight,
        bounds:{x:rect.x,y:rect.y,width:rect.width,height:rect.height}});
      if (result.length >= 16) break;
    }
    return result;
  }
  let target = document.scrollingElement || document.documentElement;
  let bounds = null;
  if (selector !== null) {
    const matches = document.querySelectorAll(selector);
    if (matches.length !== 1) throw new Error("scroll selector must identify exactly one element");
    target = matches[0];
    const rect = target.getBoundingClientRect(), style = getComputedStyle(target);
    if (target === document.body || target === document.documentElement
        || !target.checkVisibility({opacityProperty:true, visibilityProperty:true, contentVisibilityAuto:true})
        || !["auto", "scroll"].includes(style.overflowY)
        || target.clientHeight <= 0 || target.clientWidth <= 0
        || rect.right <= 0 || rect.bottom <= 0 || rect.left >= innerWidth || rect.top >= innerHeight
        || target.scrollLeft !== 0)
      throw new Error("selected scroll surface is not a visible vertical scrolling element");
    bounds = {x:rect.x, y:rect.y, width:rect.width, height:rect.height};
  }
  const height = selector === null ? innerHeight : target.clientHeight;
  const maximum = Math.max(0, target.scrollHeight - height);
  if (fraction !== null) top = Math.floor((limit ? Math.min(maximum, limit) : maximum) * fraction);
  if (top !== null) {
    if (!Number.isFinite(top) || top < 0) throw new Error("invalid scroll surface offset");
    if (selector === null) scrollTo({top, behavior:"instant"});
    else target.scrollTo({top, behavior:"instant"});
  }
  return {kind:selector === null ? "document" : "element", selector,
    y:selector === null ? scrollY : target.scrollTop, maximum, height,
    scroll_width:target.scrollWidth, scroll_height:target.scrollHeight, bounds,
    outer_x:scrollX, outer_y:scrollY};
}

module.exports = {inspectScrollSurface};
