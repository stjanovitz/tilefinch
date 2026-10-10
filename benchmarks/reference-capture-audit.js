"use strict";

/* Capture-only helpers. No live fetching, response rewriting, or page mutation. */
const MAX_REQUESTS = 16384;
const MAX_IMAGES = 4096;
const MAX_ANCESTORS = 256;
const { EventEmitter } = require("node:events");

/* OOPIFs have separate Fetch domains. Use the documented non-flattened CDP
   target tunnel so every frame stays paused until hermetic routing is ready.
   This does not alter site isolation or rewrite cross-origin responses. */
async function installInterception(root, onRequest, runHost, track, onFailure = () => {}) {
  const sessions = new Set([root]);
  const children = new Map();
  const pending = new Map();
  let nextId = 0;
  let failures = 0, overflow = false;
  const fail = (error) => { failures += 1; onFailure(error); };
  const attach = async (session) => {
    session.on("Fetch.requestPaused", (event) => onRequest(session, event));
    session.on("Target.receivedMessageFromTarget", ({ sessionId, message }) => {
      const child = children.get(`${session._captureKey || "root"}\0${sessionId}`);
      if (!child) return;
      let value;
      try { value = JSON.parse(message); }
      catch (_) { fail(new Error("capture target returned malformed protocol data")); return; }
      if (value.id !== undefined) {
        const key = `${child._captureKey}\0${value.id}`, promise = pending.get(key);
        if (!promise) return;
        pending.delete(key);
        if (value.error) promise.reject(new Error(value.error.message));
        else promise.resolve(value.result || {});
      } else if (value.method) child.emit(value.method, value.params || {});
    });
    session.on("Target.attachedToTarget", ({ sessionId, targetInfo }) => {
      if (targetInfo.type !== "iframe") return;
      track((async () => {
        if (sessions.size >= 64) { overflow = true; throw new Error("capture exceeds 64 frame targets"); }
        const child = new EventEmitter();
        child._captureKey = `${session._captureKey || "root"}\0${sessionId}`;
        child.send = (method, params = {}) => {
          if (pending.size >= 128) {
            overflow = true;
            return Promise.reject(new Error("capture target command bound exceeded"));
          }
          const id = ++nextId, key = `${child._captureKey}\0${id}`;
          return new Promise((resolve, reject) => {
            pending.set(key, { resolve, reject });
            session.send("Target.sendMessageToTarget", {
              sessionId, message: JSON.stringify({ id, method, params }),
            }).catch((error) => { pending.delete(key); reject(error); });
          });
        };
        children.set(child._captureKey, child);
        sessions.add(child);
        await attach(child);
        await runHost("frame-target-resume", () => child.send("Runtime.runIfWaitingForDebugger"));
      })().catch((error) => { fail(error); throw error; }));
    });
    session.on("Target.detachedFromTarget", ({ sessionId }) => {
      const key = `${session._captureKey || "root"}\0${sessionId}`;
      const child = children.get(key);
      if (!child) return;
      children.delete(key); // Count all admitted targets, not only live targets.
      for (const [command, promise] of pending) {
        if (command.startsWith(`${key}\0`)) {
          pending.delete(command); promise.reject(new Error("capture frame target detached"));
        }
      }
    });
    await runHost("redirect-interception-enable", () => session.send("Fetch.enable", {
      patterns: [{ urlPattern: "http://*", requestStage: "Request" },
        { urlPattern: "https://*", requestStage: "Request" }],
    }));
    await runHost("redirect-interception-cache-disable", () => session.send("Network.setCacheDisabled", {
      cacheDisabled: true,
    }));
    await runHost("frame-target-auto-attach", () => session.send("Target.setAutoAttach", {
      autoAttach: true, waitForDebuggerOnStart: true, flatten: false,
      filter: [{ type: "iframe", exclude: false }, { exclude: true }],
    }));
  };
  await attach(root);
  return { summary: () => ({ version: "hermetic-frame-targets-v1", targets: sessions.size,
    pending_commands: pending.size, failures, overflow,
    ready: !overflow && failures === 0 && pending.size === 0 }) };
}

function retainedUserAgent(records, url) {
  const record = records.find((entry) => entry.method === "GET" && entry.url === url);
  const value = record && record.userAgent;
  if (value === undefined || value === "") return undefined; // Legacy traces.
  if (typeof value !== "string" || value.length > 2048 || /[^\x20-\x7e]/.test(value)) {
    throw new Error("retained document User-Agent is invalid");
  }
  return value;
}

/* Playwright's Route is invoked only for the first URL of a redirect chain.
   Chromium Fetch interception sees every hop, keeping the original 302 and
   Location intact. The adapter deliberately exposes only the Route operations
   used by the hermetic recorder; it cannot fetch or follow a URL itself. */
function cdpRoute(session, event, onAbort = () => {}) {
  const headers = Object.fromEntries(Object.entries(event.request.headers || {})
    .map(([name, value]) => [name.toLowerCase(), String(value)]));
  const errors = {
    blockedbyclient: "BlockedByClient", blockedbyresponse: "BlockedByResponse",
    aborted: "Aborted", timedout: "TimedOut", failed: "Failed",
  };
  let completed = false;
  const send = (method, values) => {
    if (completed) throw new Error("intercepted request already completed");
    completed = true;
    return session.send(method, { requestId: event.requestId, ...values });
  };
  return {
    request: () => ({
      url: () => event.request.url,
      method: () => event.request.method,
      resourceType: () => String(event.resourceType || "other").toLowerCase(),
      headers: () => headers,
    }),
    abort: (code) => {
      onAbort(event.request.method, event.request.url);
      return send("Fetch.failRequest", { errorReason: errors[code] || "Failed" });
    },
    continue: () => send("Fetch.continueRequest", {}),
    fulfill: ({ status, headers: responseHeaders, body }) => send("Fetch.fulfillRequest", {
      responseCode: status,
      responseHeaders: Object.entries(responseHeaders).map(([name, value]) =>
        ({ name, value: String(value) })),
      body: Buffer.from(body).toString("base64"),
    }),
  };
}

function createRequestAudit() {
  const pending = new Set();
  const failures = [];
  const expectedFailures = new Map();
  let observed = 0;
  let overflow = false;
  return {
    expectFailure(method, url) {
      const key = `${method}\0${url}`;
      if (!expectedFailures.has(key) && expectedFailures.size >= MAX_REQUESTS) overflow = true;
      else expectedFailures.set(key, (expectedFailures.get(key) || 0) + 1);
    },
    begin(request) {
      if (!/^https?:/i.test(request.url())) return;
      observed += 1;
      if (pending.size >= MAX_REQUESTS) overflow = true;
      else pending.add(request);
    },
    end(request, failed = false) {
      if (!/^https?:/i.test(request.url())) return;
      pending.delete(request);
      if (failed) {
        const key = `${request.method()}\0${request.url()}`;
        const expected = expectedFailures.get(key) || 0;
        if (expected) expectedFailures.set(key, expected - 1);
        if (failures.length >= 512) overflow = true;
        else failures.push({ url: request.url().slice(0, 2048),
          method: request.method(), type: request.resourceType(),
          expected: expected > 0,
          error: String(request.failure()?.errorText || "request failed").slice(0, 256) });
      }
    },
    summary(intercepted) {
      return { version: "browser-request-audit-v1", observed, intercepted,
        pending: pending.size, overflow, failures: failures.slice(),
        ready: !overflow && pending.size === 0 && observed === intercepted
          && failures.every((failure) => failure.expected) };
    },
  };
}

function createOriginEvidence() {
  const entries = new Map();
  let overflow = false, conflicts = false;
  return {
    record(method, url, requestOrigin) {
      const key = `${method}\0${url}`;
      const origin = String(requestOrigin || "");
      const previous = entries.get(key);
      if (previous && previous.request_origin !== origin) conflicts = true;
      else if (!previous) {
        if (entries.size >= 512) overflow = true;
        else entries.set(key, { method, url, request_origin: origin });
      }
    },
    summary() {
      return { version: "unmatched-request-origin-v1", overflow, conflicts,
        entries: [...entries.values()].sort((a, b) => {
          const left = `${a.method}\0${a.url}`, right = `${b.method}\0${b.url}`;
          return left < right ? -1 : left > right ? 1 : 0;
        }) };
    },
  };
}

/* Runs in the page. Include only paintable images intersecting the viewport
   unless the caller explicitly captured the full document. Hidden/lazy content
   elsewhere is not made mandatory for ordinary checkpoint captures. */
function visualEvidence({ fullDocument = false, imageLimit = 4096, includeHeadings = true } = {}) {
  const limit = Math.max(0, Math.min(4096, imageLimit));
  const all = document.images;
  const images = [];
  let incomplete = 0;
  let truncated = all.length > limit;
  for (let index = 0; index < Math.min(all.length, limit); index += 1) {
    const image = all[index];
    const rect = image.getBoundingClientRect();
    if (!rect.width || !rect.height || !image.currentSrc && !image.src) continue;
    if (!image.checkVisibility({ opacityProperty: true, visibilityProperty: true,
      contentVisibilityAuto: true })) continue;
    let left = rect.left, right = rect.right, top = rect.top, bottom = rect.bottom;
    let ancestor = image.parentElement;
    for (let depth = 0; ancestor && depth < 256; depth += 1, ancestor = ancestor.parentElement) {
      if (ancestor === document.body || ancestor === document.documentElement) continue;
      const style = getComputedStyle(ancestor), bounds = ancestor.getBoundingClientRect();
      if (style.overflowX !== "visible") {
        left = Math.max(left, bounds.left); right = Math.min(right, bounds.right);
      }
      if (style.overflowY !== "visible") {
        top = Math.max(top, bounds.top); bottom = Math.min(bottom, bounds.bottom);
      }
    }
    if (ancestor) { truncated = true; continue; }
    if (right <= left || bottom <= top || !fullDocument
      && (right <= 0 || left >= innerWidth || bottom <= 0 || top >= innerHeight)) continue;
    const complete = image.complete && image.naturalWidth > 0 && image.naturalHeight > 0;
    if (!complete) incomplete += 1;
    images.push({ src: image.currentSrc || image.src, complete,
      x: rect.x, y: rect.y + scrollY, width: rect.width, height: rect.height });
  }
  const root = document.scrollingElement || document.documentElement;
  const html = document.documentElement, body = document.body;
  /* Match the full screenshot's document bounds, not only scrollHeight.
     Margins and non-scrolling body geometry can extend the paint extent. */
  const documentWidth = Math.max(innerWidth, root.scrollWidth,
    html.clientWidth, html.scrollWidth, html.offsetWidth,
    body ? body.clientWidth : 0, body ? body.scrollWidth : 0, body ? body.offsetWidth : 0);
  const documentHeight = Math.max(innerHeight, root.scrollHeight,
    html.clientHeight, html.scrollHeight, html.offsetHeight,
    body ? body.clientHeight : 0, body ? body.scrollHeight : 0, body ? body.offsetHeight : 0);
  const headingNodes = includeHeadings ? document.querySelectorAll("h1,h2,h3") : [];
  const headings = [];
  truncated ||= headingNodes.length > limit;
  for (let index = 0; index < Math.min(headingNodes.length, limit); index += 1) {
      const node = headingNodes[index];
      const style = getComputedStyle(node), rect = node.getBoundingClientRect();
      headings.push({ text: node.textContent.slice(0, 256), x: rect.x, y: rect.y + scrollY,
        width: rect.width, height: rect.height, font_family: style.fontFamily,
        font_weight: style.fontWeight, font_size: style.fontSize, line_height: style.lineHeight });
  }
  return { version: "paintable-image-audit-v1", full_document: fullDocument,
    scroll_y: scrollY, maximum: Math.max(0, root.scrollHeight - innerHeight),
    document_width: documentWidth, document_height: documentHeight,
    image_count: all.length, images, incomplete, truncated, headings,
    ready: !truncated && incomplete === 0 };
}

/* Evaluate each document in its own realm; do not read cross-origin child DOM
   through the top page or move an iframe's scroll position. A full-page image
   contains only the child's current viewport, not its offscreen document. */
async function collectVisualEvidence(page, { fullDocument = false } = {}, runHost) {
  const frames = page.frames();
  const main = page.mainFrame();
  const result = await runHost("visual-main-frame", () => main.evaluate(visualEvidence, { fullDocument }));
  const childFrames = [];
  if (frames.length > 64) return { ...result, child_frames: childFrames, frame_overflow: true, ready: false };
  let remaining = Math.max(0, MAX_IMAGES - result.image_count);
  for (const [index, frame] of frames.entries()) {
    if (frame === main) continue;
    let paintable = true;
    let current = frame;
    for (let depth = 0; current !== main && depth < 64; depth += 1) {
      const element = await runHost("visual-frame-element", () => current.frameElement());
      try {
        paintable = await runHost("visual-frame-visibility", () => element.evaluate((element, fullDocument) => {
          const rect = element.getBoundingClientRect();
          if (!rect.width || !rect.height || !element.checkVisibility({ opacityProperty: true,
            visibilityProperty: true, contentVisibilityAuto: true })) return false;
          let left = rect.left, right = rect.right, top = rect.top, bottom = rect.bottom;
          let ancestor = element.parentElement;
          for (let depth = 0; ancestor && depth < 256; depth += 1, ancestor = ancestor.parentElement) {
            if (ancestor === document.body || ancestor === document.documentElement) continue;
            const style = getComputedStyle(ancestor), bounds = ancestor.getBoundingClientRect();
            if (style.overflowX !== "visible") { left = Math.max(left, bounds.left); right = Math.min(right, bounds.right); }
            if (style.overflowY !== "visible") { top = Math.max(top, bounds.top); bottom = Math.min(bottom, bounds.bottom); }
          }
          if (ancestor) throw new Error("frame visibility exceeds ancestor bound");
          return right > left && bottom > top && (fullDocument
            || right > 0 && left < innerWidth && bottom > 0 && top < innerHeight);
        }, fullDocument && current.parentFrame() === main));
      } finally {
        await runHost("visual-frame-element-dispose", () => element.dispose(), { allowAfterTerminal: true });
      }
      if (!paintable) break;
      current = current.parentFrame();
      if (!current) throw new Error("visual frame detached during audit");
    }
    if (paintable && current !== main) throw new Error("visual frame exceeds ancestor bound");
    const evidence = paintable ? await runHost("visual-child-frame", () => frame.evaluate(visualEvidence,
      { fullDocument: false, imageLimit: remaining, includeHeadings: false })) : null;
    if (evidence) remaining = Math.max(0, remaining - evidence.image_count);
    childFrames.push({ index, paintable, visual_evidence: evidence });
  }
  return { ...result, child_frames: childFrames, frame_overflow: false,
    ready: result.ready && childFrames.every((entry) => !entry.paintable || entry.visual_evidence.ready) };
}
module.exports = { installInterception, cdpRoute, createRequestAudit, createOriginEvidence, retainedUserAgent, visualEvidence, collectVisualEvidence,
  MAX_REQUESTS, MAX_IMAGES, MAX_ANCESTORS };
