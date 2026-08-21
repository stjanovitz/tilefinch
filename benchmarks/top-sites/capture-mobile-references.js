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

function argumentsFrom(argv) {
  const result = {
    ranking: path.join(__dirname, "2026-07-23-cloudflare-radar-us.tsv"),
    output: "/tmp/tilefinch-mobile-references",
    limit: 50,
    concurrency: 3,
    timeoutMs: 20000,
    settleMs: 1800,
  };
  for (let at = 2; at < argv.length; at += 2) {
    const key = {
      "--ranking": "ranking", "--output": "output", "--limit": "limit",
      "--concurrency": "concurrency", "--timeout-ms": "timeoutMs",
      "--settle-ms": "settleMs",
    }[argv[at]];
    if (!key || argv[at + 1] === undefined) {
      throw new Error(`invalid argument ${argv[at] || "<missing>"}`);
    }
    result[key] = ["ranking", "output"].includes(key)
      ? argv[at + 1] : Number(argv[at + 1]);
  }
  return result;
}

async function dismissConsent(page) {
  await page.keyboard.press("Escape").catch(() => {});
  let dismissed = false;
  const rejectLabels = [
    /reject all/i, /decline all/i, /only necessary/i,
    /continue without accepting/i, /do not consent/i, /^close$/i,
  ];
  for (let pass = 0; pass < 2; pass += 1) {
    for (const label of rejectLabels) {
      const candidate = page.getByRole("button", { name: label }).first();
      if (await candidate.isVisible({ timeout: 100 }).catch(() => false)) {
        await candidate.click({ timeout: 500 }).catch(() => {});
        await page.waitForTimeout(120);
        dismissed = true;
        break;
      }
    }
    if (dismissed) break;
    const choices = page.getByRole("button", {
      name: /manage choices|more choices|cookie settings|preferences/i,
    }).first();
    if (!await choices.isVisible({ timeout: 100 }).catch(() => false)) break;
    await choices.click({ timeout: 500 }).catch(() => {});
    await page.waitForTimeout(150);
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
  await page.screenshot({
    path: path.join(options.output, `${name}.png`),
    type: "png",
    fullPage: false,
    animations: "disabled",
  }).catch((error) => {
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
  const entries = readRanking(path.resolve(options.ranking))
    .filter(isLikelyPage).slice(0, options.limit);
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
  fs.writeFileSync(
    path.join(options.output, "capture-summary.json"),
    `${JSON.stringify({ contract: {
      width: 480, height: 272, deviceScaleFactor: 1,
      isMobile: true, hasTouch: true, userAgent: MOBILE_USER_AGENT,
    }, results }, null, 2)}\n`,
  );
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
