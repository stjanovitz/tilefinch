"use strict";
const fs = require("node:fs"), path = require("node:path"), crypto = require("node:crypto");
const RULES_FILE = path.join(__dirname, "../src/inline_tracker_rules.inc");
function loadRules() {
  const bytes = fs.readFileSync(RULES_FILE);
  if (bytes.length > 16384) throw Error("inline tracker rules exceed bound");
  const lines = bytes.toString("utf8").replace(/\/\*[\s\S]*?\*\//g, "").split("\n").filter(s => s.trim());
  if (!lines.length || lines.length > 16) throw Error("invalid inline tracker rule count");
  const rules = lines.map(line => {
    const m = /^INLINE_TRACKER_RULE\((.*)\)$/.exec(line.trim());
    if (!m) throw Error("invalid shared inline tracker rule");
    const fields = JSON.parse(`[${m[1]}]`);
    if (fields.length !== 7 || fields.some(s => typeof s !== "string" || !s.length || s.length > 256 || /[^\x20-\x7e]/.test(s)))
      throw Error("invalid inline tracker rule fields");
    const [id, domain, property, prefix, ...markers] = fields;
    if (!/^[a-z0-9.-]+$/.test(domain) || !/^[A-Za-z_$][\w$]*$/.test(property)
        || !prefix.startsWith(`window.${property} = (window.${property} || 0)`)) throw Error("unsafe inline tracker trigger");
    return {id, domain, property, prefix, markers};
  });
  return {rules, sha256:crypto.createHash("sha256").update(bytes).digest("hex")};
}
function matchRule(host, source, rules) {
  if (typeof source !== "string" || source.length > 524288) return null;
  const text = source.replace(/^[ \t\r\n\f]*/, "");
  for (const rule of rules) {
    if (host !== rule.domain && !host.endsWith("." + rule.domain)) continue;
    if (!text.startsWith(rule.prefix) || !rule.markers.every(token => text.includes(token))) continue;
    let bytes = 0;
    for (let i = 0; i < source.length; i++) {
      const c = source.charCodeAt(i);
      bytes += c < 128 ? 1 : c < 2048 ? 2 : 3;
      if (c >= 0xd800 && c <= 0xdbff && source.charCodeAt(i+1) >= 0xdc00 && source.charCodeAt(i+1) <= 0xdfff) { bytes++; i++; }
      if (bytes > 524288) return null;
    }
    return rule.id;
  }
  return null;
}
/* Abort only at the matching bundle's first read, before any author effect.
   The DOM and retained response remain unchanged. Errors not issued by this
   closure are never swallowed. Modules and external scripts are excluded. */
function installPolicy(configuration, match) {
  const rules = configuration.enabled === false ? [] : configuration.rules.filter(rule => location.hostname === rule.domain || location.hostname.endsWith("."+rule.domain));
  const errors = new WeakSet(), guards = [], descriptors = Object.getOwnPropertyDescriptor;
  const hasError = WeakSet.prototype.has.bind(errors), addError = WeakSet.prototype.add.bind(errors);
  const PolicyError = Error, resolve = Promise.resolve.bind(Promise);
  const prevent = Event.prototype.preventDefault, stop = Event.prototype.stopImmediatePropagation;
  const currentScript = descriptors(Document.prototype, "currentScript").get;
  const textContent = descriptors(Node.prototype, "textContent").get;
  const getAttribute = Element.prototype.getAttribute;
  const report = globalThis.__tilefinchInlineTrackerBlocked;
  let blocked = 0, failed = false;
  addEventListener("error", event => {
    if (hasError(event.error)) { prevent.call(event); stop.call(event); }
  }, true);
  for (const property of new Set(rules.map(rule => rule.property))) {
    if (descriptors(globalThis, property)) { failed = true; continue; }
    let value;
    const get = function() {
      const node = currentScript.call(document);
      if (node && getAttribute.call(node, "src") === null) {
        const type = (getAttribute.call(node, "type") || "").split(';')[0].replace(/^[ \t\r\n\f]*|[ \t\r\n\f]*$/g, '').toLowerCase();
        if (["", "application/ecmascript", "application/javascript", "application/x-ecmascript",
          "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0",
          "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
          "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript", "text/x-javascript"].includes(type)) {
          const source = textContent.call(node), id = match(location.hostname, source, rules);
          if (id) {
            if (++blocked > 32) failed = true;
            else resolve(report(id, source)).catch(() => { failed = true; });
            const error = new PolicyError("Inline script blocked by content policy");
            addError(error); throw error;
          }
        }
      }
      return value;
    };
    const set = function(next) { value = next; };
    Object.defineProperty(globalThis, property, {configurable:true, enumerable:true, get, set});
    guards.push({property, get, set});
  }
  Object.defineProperty(globalThis, "__tilefinchInlineTrackerEvidence", {value:() => ({
    version:"shared-inline-trackers-v1", rules_sha256:configuration.sha256, blocked,
    enabled:configuration.enabled !== false,
    ready:!failed && guards.every(guard => {
      const d = descriptors(globalThis, guard.property);
      return d && d.get === guard.get && d.set === guard.set;
    })}), configurable:false, writable:false});
}
module.exports = {loadRules, matchRule, installPolicy, RULES_FILE};
