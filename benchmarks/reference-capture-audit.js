"use strict";

/* Capture-only helpers. No live fetching, response rewriting, or page mutation. */
const MAX_REQUESTS = 16384;
const MAX_IMAGES = 4096;
const MAX_ANCESTORS = 256;
const { EventEmitter } = require("node:events");

/* Exact request shape for a retained, credential-free CORS permission check.
   Browser-generated OPTIONS is admitted only for a read-only target method;
   unknown shapes remain unmatched, never synthetic successful grants. */
function preflightShape(headers) {
  const origin = headers.origin, method = headers["access-control-request-method"];
  const names = String(headers["access-control-request-headers"] || "");
  if (typeof origin !== "string" || !["GET", "HEAD"].includes(method)
      || origin.length > 2048 || names.length > 1024) return null;
  try { if (new URL(origin).origin !== origin || !origin.startsWith("https://")) return null; }
  catch (_) { return null; }
  const fields = names === "" ? [] : names.split(",").map((name) => name.trim().toLowerCase());
  if (fields.some((name) => !/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name))) return null;
  return { origin, method, headers: [...new Set(fields)].sort().join(",") };
}

/* OOPIFs have separate Fetch domains. Use the documented non-flattened CDP
   target tunnel so every frame stays paused until hermetic routing is ready.
   This does not alter site isolation or rewrite cross-origin responses. */
async function installInterception(root, onRequest, runHost, track, onFailure = () => {}, audit = null, workerSetup = null) {
  const sessions = new Set([root]);
  const children = new Map();
  const pending = new Map();
  let nextId = 0;
  let failures = 0, overflow = false;
  const fail = (error) => { failures += 1; onFailure(error); };
  const attach = async (session, worker = false, networkOwner = session) => {
    if (audit) {
      session.on("Network.requestWillBeSent", (event) => audit.networkBegin(networkOwner, event));
      session.on("Network.loadingFinished", (event) => audit.networkEnd(networkOwner, event));
      session.on("Network.loadingFailed", (event) => audit.networkEnd(networkOwner, event, true));
      await runHost("request-audit-enable", () => session.send("Network.enable"));
    }
    session.on("Fetch.requestPaused", (event) => {
      if (audit) audit.networkIntercepted(session, event);
      onRequest(session, event);
    });
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
    session.on("Target.attachedToTarget", ({ sessionId, targetInfo, waitingForDebugger }) => {
      if (targetInfo.type !== "iframe" && !(workerSetup && targetInfo.type === "worker")) return;
      track((async () => {
        if (sessions.size >= 64) { overflow = true; throw new Error("capture exceeds 64 frame targets"); }
        const child = new EventEmitter();
        child._captureKey = `${session._captureKey || "root"}\0${sessionId}`;
        child._captureTargetId = targetInfo.targetId;
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
        let resumeWorker = null;
        if (targetInfo.type === "worker") {
          if (!waitingForDebugger) throw new Error("worker target was not paused before author execution");
          resumeWorker = await workerSetup(child, targetInfo);
        }
        await attach(child, targetInfo.type === "worker", targetInfo.type === "worker" ? networkOwner : child);
        if (resumeWorker) await resumeWorker();
        await runHost("frame-target-resume", () => child.send("Runtime.runIfWaitingForDebugger"));
      })().catch((error) => { fail(error); throw error; }));
    });
    session.on("Target.detachedFromTarget", ({ sessionId }) => {
      const key = `${session._captureKey || "root"}\0${sessionId}`;
      const child = children.get(key);
      if (!child) return;
      if (child._captureWorkerDetached) child._captureWorkerDetached();
      children.delete(key); // Count all admitted targets, not only live targets.
      for (const [command, promise] of pending) {
        if (command.startsWith(`${key}\0`)) {
          pending.delete(command); promise.reject(new Error("capture frame target detached"));
        }
      }
    });
    if (!worker) await runHost("redirect-interception-enable", () => session.send("Fetch.enable", {
      patterns: [{ urlPattern: "http://*", requestStage: "Request" },
        { urlPattern: "https://*", requestStage: "Request" }],
    }));
    if (!worker) await runHost("redirect-interception-cache-disable", () => session.send("Network.setCacheDisabled", {
      cacheDisabled: true,
    }));
    await runHost("frame-target-auto-attach", () => session.send("Target.setAutoAttach", {
      autoAttach: true, waitForDebuggerOnStart: true, flatten: false,
      filter: [{ type: "iframe", exclude: false },
        ...(workerSetup ? [{type:"worker", exclude:false}] : []), { exclude: true }],
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
function cdpRoute(session, event, onAbort = () => {}, onFulfill = () => {}) {
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
      postDataBuffer: () => typeof event.request.postData === "string"
        && Buffer.byteLength(event.request.postData,"utf8") <= 16384
        ? Buffer.from(event.request.postData,"utf8") : null,
    }),
    abort: (code) => {
      onAbort(event.request.method, event.request.url);
      return send("Fetch.failRequest", { errorReason: errors[code] || "Failed" });
    },
    continue: () => send("Fetch.continueRequest", {}),
    fulfill: async ({ status, headers: responseHeaders, body }) => {
      await send("Fetch.fulfillRequest", {
      responseCode: status,
      responseHeaders: Object.entries(responseHeaders).map(([name, value]) =>
        ({ name, value: String(value) })),
      body: Buffer.from(body).toString("base64"),
      });
      onFulfill();
    },
  };
}

function createRequestAudit() {
  const pending = new Set();
  const failures = [];
  const expectedFailures = new Map();
  let observed = 0;
  let overflow = false;
  const networks = new Map();
  const networkKey = (session, id) => `${session._captureKey || "root"}\0${id}`;
  const fulfilled = new Set();
  const intercepted = new Set();
  const deniedPreflightParents = new Map();
  const abortedPreflights = new Map();
  let preflightDeniedParents = 0;
  const linkPreflight = (permissionKey, permission) => {
    const ticket = abortedPreflights.get(permissionKey);
    if (!ticket || !permission?.preflightParent || permission.method !== "OPTIONS"
        || permission.url !== ticket.url) return;
    ticket.permission = permission;
    const parentKey = `${permissionKey.slice(0, permissionKey.lastIndexOf("\0") + 1)}${permission.preflightParent}`;
    if (deniedPreflightParents.size >= MAX_REQUESTS && !deniedPreflightParents.has(parentKey)) {
      overflow = true; return;
    }
    deniedPreflightParents.set(parentKey, ticket);
  };
  const result = {
    networkBegin(session, event) {
      const key = networkKey(session, event.requestId);
      if (event.redirectResponse) {
        pending.delete(key);
        fulfilled.delete(key);
        intercepted.delete(key);
      }
      if (!/^https?:/i.test(event.request.url)) return;
      observed += 1;
      if (networks.size >= MAX_REQUESTS && !networks.has(key)) { overflow = true; return; }
      networks.set(key, { url: event.request.url, method: event.request.method,
        type: String(event.type || "other").toLowerCase(), frameId: event.frameId,
        ...(event.initiator?.type === "preflight"
          && typeof event.initiator.requestId === "string"
          ? { preflightParent: event.initiator.requestId } : {}) });
      pending.add(key);
      linkPreflight(key, networks.get(key));
    },
    networkIntercepted(session, event) {
      if (!event.networkId) return;
      if (intercepted.size >= MAX_REQUESTS) { overflow = true; return; }
      intercepted.add(networkKey(session, event.networkId));
    },
    networkAborted(session, event, { declaredPolicy = false } = {}) {
      if (!declaredPolicy || event.request?.method !== "OPTIONS" || !event.networkId) return;
      const permissionKey = networkKey(session, event.networkId);
      const headers = Object.fromEntries(Object.entries(event.request.headers || {})
        .map(([name, value]) => [name.toLowerCase(), String(value)]));
      /* Chromium reports the parent XHR/fetch as failed too, although it
         never reaches Fetch interception after its preflight is refused.
         Require CDP's exact initiator link, method, URL and frame; URL-only
         failure tickets must not excuse an unrelated request. */
      if (!/^[A-Z]+$/.test(headers["access-control-request-method"] || "")) return;
      if (abortedPreflights.size >= MAX_REQUESTS) { overflow = true; return; }
      /* Fetch.requestPaused can precede Network.requestWillBeSent. Retain
         the exact denied ticket until its CDP initiator arrives. A preflight
         may omit frameId; an ID it does supply must match the parent. */
      abortedPreflights.set(permissionKey, { permissionKey,
        url: event.request.url, method: headers["access-control-request-method"],
        frameId: event.frameId, confirmed: false, final: false });
      linkPreflight(permissionKey, networks.get(permissionKey));
    },
    networkFulfilled(session, event) {
      if (!event.networkId) { overflow = true; return; }
      if (fulfilled.size >= MAX_REQUESTS) { overflow = true; return; }
      fulfilled.add(networkKey(session, event.networkId));
    },
    networkEnd(session, event, failed = false) {
      let key = networkKey(session, event.requestId), request = networks.get(key);
      /* OOPIF document navigation begins in the parent target, but its
         completion can arrive only on the newly attached child. Transfer
         that exact document ticket, never an unrelated same-ID request. */
      if (!request && session._captureTargetId) {
        const suffix = `\0${event.requestId}`;
        const candidates = [...networks].filter(([id, value]) => id.endsWith(suffix)
          && value.type === "document" && value.frameId === session._captureTargetId);
        if (candidates.length === 1) [key, request] = candidates[0];
        else if (candidates.length > 1) overflow = true;
      }
      if (!request) return; // Non-HTTP work is outside the replay ledger.
      const deniedTicket = deniedPreflightParents.get(key);
      if (deniedTicket && !deniedTicket.final) {
        deniedTicket.parentEnd = { session, event, failed };
        return;
      }
      const permissionTicket = abortedPreflights.get(key);
      if (permissionTicket) {
        permissionTicket.final = true;
        permissionTicket.confirmed = failed && event.errorText === "net::ERR_BLOCKED_BY_CLIENT";
        linkPreflight(key, request);
      }
      pending.delete(key);
      if (failed) {
        const expectedKey = `${request.method}\0${request.url}`;
        const expected = expectedFailures.get(expectedKey) || 0;
        if (expected) expectedFailures.set(expectedKey, expected - 1);
        /* A browser cancellation after a successful hermetic fulfillment is
           not a missing response. Unknown cancellations and transport errors
           still fail. Visual completeness remains independently mandatory. */
        const canceledReplay = event.canceled === true && fulfilled.has(key);
        const permissionFrame = deniedTicket?.frameId ?? deniedTicket?.permission?.frameId;
        const deniedParent = deniedTicket?.confirmed === true && !intercepted.has(key)
          && deniedTicket.url === request.url && deniedTicket.method === request.method
          && (permissionFrame === undefined || permissionFrame === request.frameId)
          && event.errorText === "net::ERR_BLOCKED_BY_CLIENT";
        if (deniedParent) preflightDeniedParents += 1;
        if (failures.length >= 512) overflow = true;
        else failures.push({ ...request, expected: expected > 0 || canceledReplay || deniedParent,
          reason: expected > 0 ? "declared-policy" : canceledReplay ? "canceled-replayed-response"
            : deniedParent ? "denied-preflight-parent" : "unexpected",
          error: String(event.errorText || "request failed").slice(0, 256) });
      }
      fulfilled.delete(key);
      intercepted.delete(key);
      if (deniedTicket) abortedPreflights.delete(deniedTicket.permissionKey);
      deniedPreflightParents.delete(key);
      networks.delete(key);
      if (permissionTicket?.parentEnd) {
        const deferred = permissionTicket.parentEnd;
        delete permissionTicket.parentEnd;
        result.networkEnd(deferred.session, deferred.event, deferred.failed);
      }
    },
  };
  return Object.assign(result, {
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
    summary(interceptedCount) {
      return { version: preflightDeniedParents ? "browser-request-audit-v3" : "browser-request-audit-v2", observed, intercepted:interceptedCount,
        ...(preflightDeniedParents ? { preflight_denied_parents: preflightDeniedParents } : {}),
        ...(pending.size ? {pending_requests:Array.from(pending).slice(0, 32).map(key => {
          const request = networks.get(key);
          return request ? {...request, fulfilled:fulfilled.has(key), intercepted:intercepted.has(key)}
            : {method:typeof key.method === "function" ? key.method() : "unknown",
               url:typeof key.url === "function" ? key.url().slice(0,2048) : "unknown"};
        })} : {}),
        pending: pending.size, overflow, failures: failures.slice(),
        ready: !overflow && pending.size === 0 && deniedPreflightParents.size === 0
          && abortedPreflights.size === 0
          && observed === interceptedCount + preflightDeniedParents
          && failures.every((failure) => failure.expected) };
    },
  });
}

function createOriginEvidence({ variants = false } = {}) {
  const entries = new Map();
  let overflow = false, conflicts = false, total = 0;
  return {
    record(method, url, requestOrigin, resourceType = "unknown") {
      const origin = String(requestOrigin || "");
      if (variants) {
        const key = `${method}\0${url}\0${origin}\0${resourceType}`;
        if (++total > 1000000) { overflow = true; return; }
        const previous = entries.get(key);
        if (previous) previous.occurrences += 1;
        else if (entries.size >= 512) overflow = true;
        else entries.set(key, { method, url, request_origin: origin,
          resource_type: resourceType, occurrences: 1 });
        return;
      }
      const key = `${method}\0${url}`;
      const previous = entries.get(key);
      if (previous && previous.request_origin !== origin) conflicts = true;
      else if (!previous) {
        if (entries.size >= 512) overflow = true;
        else entries.set(key, { method, url, request_origin: origin });
      }
    },
    summary() {
      return { version: variants ? "unmatched-request-origin-v2" : "unmatched-request-origin-v1",
        overflow, ...(variants ? {total} : {conflicts}),
        entries: [...entries.values()].sort((a, b) => {
          const suffix = row => variants ? `\0${row.request_origin}\0${row.resource_type}` : "";
          const left = `${a.method}\0${a.url}${suffix(a)}`, right = `${b.method}\0${b.url}${suffix(b)}`;
          return left < right ? -1 : left > right ? 1 : 0;
        }) };
    },
  };
}

/* Runs in the page. Include only paintable images intersecting the viewport
   unless the caller explicitly captured the full document. Hidden/lazy content
   elsewhere is not made mandatory for ordinary checkpoint captures. */
function visualEvidence({ fullDocument = false, imageLimit = 4096, includeHeadings = true,
  retainedFailures = [] } = {}) {
  const limit = Math.max(0, Math.min(4096, imageLimit));
  const all = [], headingNodes = [], roots = [document];
  let visited = 0, traversalTruncated = false;
  /* document.images/querySelectorAll omit open component trees. Traverse
     each root once, with independent DOM/root bounds, including hidden
     elements so visibility below remains the authority. */
  for (let rootIndex = 0; rootIndex < roots.length; rootIndex++) {
    const walker = document.createTreeWalker(roots[rootIndex], NodeFilter.SHOW_ELEMENT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (++visited > 65536) { traversalTruncated = true; break; }
      if (node instanceof HTMLImageElement) {
        if (all.length >= limit) traversalTruncated = true;
        else all.push(node);
      }
      if (includeHeadings && /^(H1|H2|H3)$/.test(node.tagName)) {
        if (headingNodes.length >= limit) traversalTruncated = true;
        else headingNodes.push(node);
      }
      if (node.shadowRoot) {
        if (roots.length >= 256) traversalTruncated = true;
        else roots.push(node.shadowRoot);
      }
    }
    if (visited > 65536) break;
  }
  const images = [];
  const failures = new Set(retainedFailures.slice(0, 4096));
  let incomplete = 0;
  let truncated = traversalTruncated;
  const parent = (node) => node.assignedSlot || node.parentElement
    || node.getRootNode()?.host || null;
  for (let index = 0; index < Math.min(all.length, limit); index += 1) {
    const image = all[index];
    const rect = image.getBoundingClientRect();
    if (!rect.width || !rect.height || !image.currentSrc && !image.src) continue;
    if (!image.checkVisibility({ opacityProperty: true, visibilityProperty: true,
      contentVisibilityAuto: true })) continue;
    let left = rect.left, right = rect.right, top = rect.top, bottom = rect.bottom;
    let ancestor = parent(image);
    for (let depth = 0; ancestor && depth < 256; depth += 1, ancestor = parent(ancestor)) {
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
    const src = image.currentSrc || image.src;
    const retainedFailure = !complete && image.complete && failures.has(src);
    if (!complete && !retainedFailure) incomplete += 1;
    images.push({ src, complete, settled: complete || retainedFailure,
      retained_http_failure: retainedFailure,
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
async function collectVisualEvidence(page, { fullDocument = false, retainedFailures = [] } = {}, runHost) {
  const frames = page.frames();
  const main = page.mainFrame();
  const result = await runHost("visual-main-frame", () => main.evaluate(visualEvidence, { fullDocument, retainedFailures }));
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
      { fullDocument: false, imageLimit: remaining, includeHeadings: false, retainedFailures })) : null;
    if (evidence) remaining = Math.max(0, remaining - evidence.image_count);
    childFrames.push({ index, paintable, visual_evidence: evidence });
  }
  return { ...result, child_frames: childFrames, frame_overflow: false,
    ready: result.ready && childFrames.every((entry) => !entry.paintable || entry.visual_evidence.ready) };
}
module.exports = { preflightShape, installInterception, cdpRoute, createRequestAudit, createOriginEvidence, retainedUserAgent, visualEvidence, collectVisualEvidence,
  MAX_REQUESTS, MAX_IMAGES, MAX_ANCESTORS };
