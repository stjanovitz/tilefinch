#!/usr/bin/env node
"use strict";

const assert = require("assert");
const {
  argumentsFrom,
  auditScrollPositions,
  auditScrollTarget,
  classifyAuditScroll,
  isConsentRootText,
} = require("./capture-mobile-references");

const ordinary = argumentsFrom([
  "node", "capture-mobile-references.js", "--limit", "7",
]);
assert.strictEqual(ordinary.limit, 7);
assert.strictEqual(ordinary.fullAudit, false);

const full = argumentsFrom([
  "node", "capture-mobile-references.js", "--full-audit",
  "--limit", "3", "--settle-ms", "20", "--domain", "bing.com",
]);
assert.strictEqual(full.fullAudit, true);
assert.strictEqual(full.limit, 3);
assert.strictEqual(full.settleMs, 20);
assert.strictEqual(full.domain, "bing.com");
assert.deepStrictEqual(auditScrollPositions(101), [
  ["top", 0], ["middle", 50], ["bottom", 101],
]);
assert.deepStrictEqual(auditScrollPositions(-1), [
  ["top", 0], ["middle", 0], ["bottom", 0],
]);
assert.strictEqual(auditScrollTarget("top", 5588), 0);
assert.strictEqual(auditScrollTarget("middle", 101), 50);
assert.strictEqual(auditScrollTarget("bottom", 101), 101);

assert.deepStrictEqual(
  classifyAuditScroll("top", 0, { scrollY: 0, maxScroll: 5588 }),
  {
    valid: true, reason: "valid", desiredScrollY: 0,
    requestedScrollY: 0, actualScrollY: 0, deltaPx: 0,
  },
);
assert.strictEqual(
  classifyAuditScroll(
    "top", 0, { scrollY: 2794, maxScroll: 5588 },
  ).reason,
  "scroll-position-not-converged",
);
assert.strictEqual(
  classifyAuditScroll(
    "middle", 2500, { scrollY: 2501, maxScroll: 5000 },
  ).valid,
  true,
);
assert.strictEqual(
  classifyAuditScroll(
    "bottom", 5000, { scrollY: 5000, maxScroll: 5200 },
  ).reason,
  "document-extent-changed",
);
assert.strictEqual(
  classifyAuditScroll("side", 0, { scrollY: 0, maxScroll: 0 }).reason,
  "invalid-scroll-metrics",
);

assert.strictEqual(isConsentRootText("Cookie preferences"), true);
assert.strictEqual(isConsentRootText("Manage your privacy choices"), true);
assert.strictEqual(isConsentRootText("Preferences"), false);
assert.strictEqual(
  isConsentRootText("Navigation preferences and account settings"), false,
);

assert.throws(
  () => argumentsFrom(["node", "capture-mobile-references.js", "--bogus"]),
  /invalid argument/,
);

process.stdout.write("top-sites mobile audit JavaScript tests passed\n");
