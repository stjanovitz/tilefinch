#!/usr/bin/env node
"use strict";

/* Live visual references are intentionally local-only.  This tool commits
   the capture contract, not third-party pixels. */
const fs = require("fs");
const path = require("path");
const { isLikelyPage, readRanking } = require("./census-features");

const MOBILE_USER_AGENT =
  "Mozilla/5.0 (iPhone; PlayStation Portable; Tilefinch/0.1) "
  + "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 "
  + "Mobile/15E148 Safari/604.1";

const AUDIT_SCROLL_MAX_PASSES = 8;
const AUDIT_SCROLL_SETTLE_MS = 80;
const AUDIT_SCROLL_TOLERANCE_PX = 2;
const AUDIT_SCROLL_STABLE_SAMPLES = 2;

function argumentsFrom(argv) {
  const result = {
    ranking: path.join(__dirname, "2026-07-23-cloudflare-radar-us.tsv"),
    output: "/tmp/tilefinch-mobile-references",
    limit: 50,
    concurrency: 3,
    timeoutMs: 20000,
    settleMs: 1800,
    fullAudit: false,
    domain: "",
  };
  for (let at = 2; at < argv.length;) {
    if (argv[at] === "--full-audit") {
      result.fullAudit = true;
      at += 1;
      continue;
    }
    const key = {
      "--ranking": "ranking", "--output": "output", "--limit": "limit",
      "--concurrency": "concurrency", "--timeout-ms": "timeoutMs",
      "--settle-ms": "settleMs", "--domain": "domain",
    }[argv[at]];
    if (!key || argv[at + 1] === undefined) {
      throw new Error(`invalid argument ${argv[at] || "<missing>"}`);
    }
    result[key] = ["ranking", "output", "domain"].includes(key)
      ? argv[at + 1] : Number(argv[at + 1]);
    at += 2;
  }
  return result;
}

function auditScrollPositions(maxScroll) {
  const bounded = Math.max(0, Math.floor(Number(maxScroll) || 0));
  return [
    ["top", 0],
    ["middle", Math.floor(bounded / 2)],
    ["bottom", bounded],
  ];
}

async function pageScrollMetrics(page) {
  return page.evaluate(() => {
    const root = document.documentElement;
    const body = document.body;
    const scrollHeight = Math.max(
      root ? root.scrollHeight : 0,
      root ? root.offsetHeight : 0,
      body ? body.scrollHeight : 0,
      body ? body.offsetHeight : 0,
    );
    return {
      scrollY: Math.max(0, Math.round(window.scrollY || 0)),
      scrollHeight,
      maxScroll: Math.max(0, scrollHeight - window.innerHeight),
    };
  });
}

function auditScrollTarget(position, maxScroll) {
  const bounded = Math.max(0, Math.floor(Number(maxScroll) || 0));
  if (position === "top") return 0;
  if (position === "middle") return Math.floor(bounded / 2);
  if (position === "bottom") return bounded;
  return NaN;
}

function classifyAuditScroll(position, requestedScrollY, metrics,
                             tolerancePx = AUDIT_SCROLL_TOLERANCE_PX) {
  const requested = Number(requestedScrollY);
  const actual = metrics && typeof metrics.scrollY === "number"
    ? metrics.scrollY : NaN;
  const maxScroll = metrics && typeof metrics.maxScroll === "number"
    ? metrics.maxScroll : NaN;
  const tolerance = Math.max(0, Math.floor(Number(tolerancePx) || 0));
  const desired = auditScrollTarget(position, maxScroll);
  if (!Number.isFinite(requested) || !Number.isFinite(actual)
      || !Number.isFinite(maxScroll) || !Number.isFinite(desired)) {
    return {
      valid: false, reason: "invalid-scroll-metrics", desiredScrollY: 0,
      requestedScrollY: requested, actualScrollY: actual, deltaPx: null,
    };
  }
  if (Math.abs(requested - desired) > tolerance) {
    return {
      valid: false, reason: "document-extent-changed",
      desiredScrollY: desired, requestedScrollY: requested,
      actualScrollY: actual, deltaPx: actual - desired,
    };
  }
  if (Math.abs(actual - desired) > tolerance) {
    return {
      valid: false, reason: "scroll-position-not-converged",
      desiredScrollY: desired, requestedScrollY: requested,
      actualScrollY: actual, deltaPx: actual - desired,
    };
  }
  return {
    valid: true, reason: "valid", desiredScrollY: desired,
    requestedScrollY: requested, actualScrollY: actual,
    deltaPx: actual - desired,
  };
}

async function forceAuditScroll(page, scrollY) {
  await page.evaluate((target) => {
    let style = document.getElementById("tilefinch-audit-scroll-contract");
    if (!style) {
      style = document.createElement("style");
      style.id = "tilefinch-audit-scroll-contract";
      style.textContent = [
        "html,body,*{scroll-behavior:auto!important;",
        "scroll-snap-type:none!important;scroll-snap-align:none!important;",
        "scroll-snap-stop:normal!important;overflow-anchor:none!important}",
      ].join("");
      (document.head || document.documentElement).appendChild(style);
    }
    const root = document.documentElement;
    const body = document.body;
    if (root) {
      root.style.setProperty("scroll-behavior", "auto", "important");
      root.style.setProperty("scroll-snap-type", "none", "important");
      root.style.setProperty("overflow-anchor", "none", "important");
    }
    if (body) {
      body.style.setProperty("scroll-behavior", "auto", "important");
      body.style.setProperty("scroll-snap-type", "none", "important");
      body.style.setProperty("overflow-anchor", "none", "important");
    }
    window.scrollTo({
      left: 0, top: Math.max(0, Math.floor(target)), behavior: "instant",
    });
  }, scrollY);
}

async function convergeAuditScroll(page, position, initialTarget) {
  let metrics = await pageScrollMetrics(page);
  let requested = Number.isFinite(initialTarget)
    ? Math.max(0, Math.floor(initialTarget))
    : auditScrollTarget(position, metrics.maxScroll);
  let previous = null;
  let stableSamples = 0;
  let classification = classifyAuditScroll(position, requested, metrics);
  for (let pass = 0; pass < AUDIT_SCROLL_MAX_PASSES; pass += 1) {
    /* Middle and bottom are defined against the current document extent.
       Lazy content may change that extent, so each bounded pass derives a
       fresh target instead of preserving a stale label. Top is always zero. */
    requested = auditScrollTarget(position, metrics.maxScroll);
    await forceAuditScroll(page, requested);
    await page.waitForTimeout(AUDIT_SCROLL_SETTLE_MS);
    metrics = await pageScrollMetrics(page);
    classification = classifyAuditScroll(position, requested, metrics);
    const stable = classification.valid && previous
      && Math.abs(metrics.maxScroll - previous.maxScroll)
        <= AUDIT_SCROLL_TOLERANCE_PX
      && Math.abs(metrics.scrollY - previous.scrollY)
        <= AUDIT_SCROLL_TOLERANCE_PX;
    stableSamples = stable ? stableSamples + 1 : (classification.valid ? 1 : 0);
    if (stableSamples >= AUDIT_SCROLL_STABLE_SAMPLES) {
      return {
        ...classification, metrics, attempts: pass + 1,
        stableSamples,
      };
    }
    previous = metrics;
  }
  return {
    ...classification, valid: false,
    reason: classification.valid ? "scroll-position-not-stable"
      : classification.reason,
    metrics, attempts: AUDIT_SCROLL_MAX_PASSES, stableSamples,
  };
}

async function captureFullAudit(page, name, options, metadata) {
  let metrics = await pageScrollMetrics(page);
  const captures = {};
  for (const [position, initialTarget] of auditScrollPositions(metrics.maxScroll)) {
    const framePath = path.join(options.output, `${name}-${position}.png`);
    const invalidPath = path.join(
      options.output, `${name}-${position}-invalid.png`,
    );
    const pendingPath = path.join(
      options.output, `${name}-${position}-pending.png`,
    );
    /* Never leave a prior run's canonical frame behind when this run cannot
       prove the requested checkpoint. The invalid bitmap remains available
       under a diagnostic name, but compositors cannot mistake it for an
       audited top/middle/bottom reference. */
    for (const stale of [framePath, invalidPath, pendingPath]) {
      fs.rmSync(stale, { force: true });
    }
    const convergence = await convergeAuditScroll(
      page, position, initialTarget,
    );
    await page.screenshot({
      path: pendingPath, type: "png", fullPage: false,
      animations: "disabled",
    });
    metrics = await pageScrollMetrics(page);
    const afterCapture = classifyAuditScroll(
      position, convergence.requestedScrollY, metrics,
    );
    const valid = convergence.valid && afterCapture.valid;
    let reason = convergence.reason;
    if (convergence.valid && !afterCapture.valid) {
      reason = "scroll-position-changed-during-capture";
    }
    const finalPath = valid ? framePath : invalidPath;
    fs.renameSync(pendingPath, finalPath);
    captures[position] = {
      valid,
      status: valid ? "valid" : reason,
      targetScrollY: convergence.requestedScrollY,
      requestedScrollY: convergence.requestedScrollY,
      actualScrollY: metrics.scrollY,
      actualBeforeCaptureScrollY: convergence.metrics.scrollY,
      desiredScrollY: afterCapture.desiredScrollY,
      deltaPx: afterCapture.deltaPx,
      scrollHeight: metrics.scrollHeight,
      maxScroll: metrics.maxScroll,
      attempts: convergence.attempts,
      stableSamples: convergence.stableSamples,
      tolerancePx: AUDIT_SCROLL_TOLERANCE_PX,
      file: path.basename(finalPath),
    };
  }
  const invalidCaptures = Object.entries(captures)
    .filter(([, capture]) => !capture.valid)
    .map(([position]) => position);
  metadata.audit = {
    mode: "full", valid: invalidCaptures.length === 0,
    invalidCaptures, captures,
  };
}

const CONSENT_ROOT_SELECTOR =
  "dialog,[role=dialog],[id*=cookie i],[class*=cookie i],"
  + "[id*=consent i],[class*=consent i]";

function isConsentRootText(text) {
  return /cookie|consent|privacy choices/i.test((text || "").slice(0, 2048));
}

async function dismissConsent(page) {
  await page.keyboard.press("Escape").catch(() => {});
  let dismissed = false;
  const rejectLabels = [
    /reject all/i, /decline all/i, /only necessary/i,
    /continue without accepting/i, /do not consent/i, /^close$/i,
  ];
  const roots = page.locator(CONSENT_ROOT_SELECTOR);
  const rootCount = Math.min(await roots.count().catch(() => 0), 16);
  for (let rootIndex = 0; rootIndex < rootCount && !dismissed;
       rootIndex += 1) {
    const root = roots.nth(rootIndex);
    if (!await root.isVisible({ timeout: 100 }).catch(() => false)) continue;
    const text = await root.textContent({ timeout: 100 }).catch(() => "");
    if (!isConsentRootText(text)) continue;
    for (let pass = 0; pass < 2; pass += 1) {
      for (const label of rejectLabels) {
        const candidate = root.getByRole("button", { name: label }).first();
        if (await candidate.isVisible({ timeout: 100 }).catch(() => false)) {
          await candidate.click({ timeout: 500 }).catch(() => {});
          await page.waitForTimeout(120);
          dismissed = true;
          break;
        }
      }
      if (dismissed) break;
      const choices = root.getByRole("button", {
        name: /manage choices|more choices|cookie settings|preferences/i,
      }).first();
      if (!await choices.isVisible({ timeout: 100 }).catch(() => false)) break;
      await choices.click({ timeout: 500 }).catch(() => {});
      await page.waitForTimeout(150);
    }
  }
  /* Some managers animate a rejected panel away, while others offer no
     reject/close action on their first panel.  Hide any still-present,
     explicit cookie/consent root for the visual oracle, matching Tilefinch's
     optional cosmetic rule without accepting anything. */
  await page.evaluate(() => {
    const candidates = document.querySelectorAll(
      "dialog,[role=dialog],[id*=cookie i],[class*=cookie i],"
      + "[id*=consent i],[class*=consent i]",
    );
    for (const element of candidates) {
      const text = (element.textContent || "").slice(0, 2048);
      if (/cookie|consent|privacy choices/i.test(text)) {
        element.style.setProperty("display", "none", "important");
      }
    }
    document.documentElement.style.removeProperty("overflow");
    if (document.body) document.body.style.removeProperty("overflow");
  }).catch(() => {});
}

async function capture(browser, entry, options) {
  const context = await browser.newContext({
    viewport: { width: 480, height: 272 },
    screen: { width: 480, height: 272 },
    deviceScaleFactor: 1,
    isMobile: true,
    hasTouch: true,
    userAgent: MOBILE_USER_AGENT,
    locale: "en-US",
    timezoneId: "America/Los_Angeles",
    colorScheme: "light",
    serviceWorkers: "block",
  });
  const page = await context.newPage();
  const metadata = {
    rank: entry.rank, domain: entry.domain, url: `https://${entry.domain}/`,
    status: 0, finalUrl: "", title: "", error: "", innerWidth: 0,
    innerHeight: 0, devicePixelRatio: 0, userAgent: "",
  };
  try {
    const response = await page.goto(metadata.url, {
      waitUntil: "domcontentloaded", timeout: options.timeoutMs,
    });
    metadata.status = response ? response.status() : 0;
    await page.waitForTimeout(options.settleMs);
    await dismissConsent(page);
    Object.assign(metadata, await page.evaluate(() => ({
      finalUrl: location.href,
      title: document.title,
      innerWidth: document.documentElement.clientWidth || innerWidth,
      innerHeight,
      devicePixelRatio,
      userAgent: navigator.userAgent,
    })));
    if (metadata.devicePixelRatio !== 1) {
      throw new Error(
        `invalid mobile DPR ${metadata.devicePixelRatio}`,
      );
    }
  } catch (error) {
    metadata.error = String(error && error.message ? error.message : error);
  }
  const name = `${String(entry.rank).padStart(2, "0")}-${entry.domain}`;
  if (options.fullAudit) {
    /* Do not let an older top-only frame masquerade as this run if somebody
       invokes the ordinary compositor on the full-audit directory. */
    fs.rmSync(path.join(options.output, `${name}.png`), { force: true });
  }
  const screenshot = options.fullAudit
    ? captureFullAudit(page, name, options, metadata)
    : page.screenshot({
        path: path.join(options.output, `${name}.png`),
        type: "png",
        fullPage: false,
        animations: "disabled",
      });
  await screenshot.catch((error) => {
    metadata.error += `${metadata.error ? "; " : ""}screenshot: ${error.message}`;
  });
  fs.writeFileSync(
    path.join(options.output, `${name}.json`),
    `${JSON.stringify(metadata, null, 2)}\n`,
  );
  await context.close();
  return metadata;
}

async function main() {
  const options = argumentsFrom(process.argv);
  fs.mkdirSync(options.output, { recursive: true });
  let entries = readRanking(path.resolve(options.ranking)).filter(isLikelyPage);
  if (options.domain) {
    entries = entries.filter((entry) => entry.domain === options.domain);
  }
  entries = entries.slice(0, options.limit);
  const { chromium } = require("playwright");
  const browser = await chromium.launch({ headless: true });
  let cursor = 0;
  const results = new Array(entries.length);
  const workers = Array.from(
    { length: Math.min(options.concurrency, entries.length) },
    async () => {
      for (;;) {
        const index = cursor++;
        if (index >= entries.length) return;
        const result = await capture(browser, entries[index], options);
        results[index] = result;
        process.stdout.write(
          `${String(result.rank).padStart(2, "0")} ${result.domain.padEnd(28)} `
          + `${result.status || "ERR"} ${result.innerWidth || 0}px dpr=${result.devicePixelRatio || 0}`
          + `${result.audit && !result.audit.valid
            ? ` audit-invalid=${result.audit.invalidCaptures.join(",")}` : ""}`
          + `${result.error ? ` ${result.error.slice(0, 80)}` : ""}\n`,
        );
      }
    },
  );
  try {
    await Promise.all(workers);
  } finally {
    await browser.close();
  }
  const contract = {
    width: 480, height: 272, deviceScaleFactor: 1,
    isMobile: true, hasTouch: true, userAgent: MOBILE_USER_AGENT,
  };
  if (options.fullAudit) contract.auditMode = "full";
  fs.writeFileSync(
    path.join(options.output, "capture-summary.json"),
    `${JSON.stringify({ contract, results }, null, 2)}\n`,
  );
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}

module.exports = {
  argumentsFrom,
  auditScrollPositions,
  auditScrollTarget,
  classifyAuditScroll,
  isConsentRootText,
};
