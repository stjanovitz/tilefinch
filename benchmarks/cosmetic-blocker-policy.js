"use strict";
const fs = require("node:fs"), path = require("node:path"), crypto = require("node:crypto");
const RULES_FILE = path.join(__dirname, "../src/cosmetic_blocker_css.inc");
function loadRules() {
  const bytes = fs.readFileSync(RULES_FILE);
  if (bytes.length > 4096) throw Error("cosmetic source exceeds bound");
  const css = bytes.toString("utf8").trim().split("\n").map(line => {
    const value = JSON.parse(line);
    if (typeof value !== "string") throw Error("invalid cosmetic rule");
    return value;
  }).join("");
  if (!css.length || Buffer.byteLength(css) >= 1024) throw Error("cosmetic stylesheet exceeds native bound");
  return {css, sha256:crypto.createHash("sha256").update(bytes).digest("hex")};
}
function installPolicy(configuration) {
  const style = document.createElement("style");
  style.textContent = configuration.css;
  let applied = false, expectedRule = null;
  const install = () => {
    if (document.documentElement && !applied) {
      document.documentElement.appendChild(style);
      if (style.sheet && style.sheet.cssRules.length === 1)
        expectedRule = style.sheet.cssRules[0].cssText;
      applied = true;
    }
  };
  if (configuration.enabled) {
    // document.write/open may replace a child frame's initial skeleton.
    // Apply to the parsed document, before settled capture, not that skeleton.
    if (document.readyState === "loading")
      addEventListener("DOMContentLoaded", install, {once:true});
    else install();
  }
  Object.defineProperty(globalThis, "__tilefinchCosmeticEvidence", {value:() => ({
    version:"shared-cosmetic-v1", rules_sha256:configuration.sha256,
    enabled:configuration.enabled,
    ready:!configuration.enabled || (applied && style.isConnected
      && style.textContent === configuration.css && !style.disabled
      && style.sheet !== null && style.sheet.cssRules.length === 1
      && expectedRule !== null && style.sheet.cssRules[0].cssText === expectedRule),
  }), configurable:false, writable:false});
}
module.exports = {loadRules, installPolicy, RULES_FILE};
