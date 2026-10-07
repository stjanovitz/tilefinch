#!/usr/bin/env node
"use strict";
// Offline Chromium reference from a Tilefinch HTTP trace (no live network):
// every request is fulfilled by URL from NNNN.meta/.body, anything else is
// aborted. 480x272 CSS px, DPR 1, mobile+touch, Tilefinch's shipping UA.
// Adapted from the news-compat investigation. The project's
// benchmarks/capture-reference.js refuses live-site traces with collapsed
// redirects, so this is the by-inspection fallback.
// Usage: node chrome-replay.js TRACE_DIR URL OUTDIR NAME [WIDTHxHEIGHT]
const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

const UA = "Mozilla/5.0 (iPhone; PlayStation Portable; Tilefinch/0.1) "
  + "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 "
  + "Mobile/15E148 Safari/604.1";
const META = new RegExp("^[0-9]{4}[.]meta$");

function loadTrace(dir) {
  const map = new Map();
  for (const name of fs.readdirSync(dir).filter((n) => META.test(n)).sort()) {
    const meta = fs.readFileSync(path.join(dir, name), "utf8");
    const field = (k) => { const m = meta.match(new RegExp(`^${k}=(.*)$`, "m")); return m ? m[1] : ""; };
    const url = field("url").split("#")[0];
    const status = Number(field("status") || 0);
    const ok = field("success") === "1";
    if (!url || status === 0) continue;
    if (map.has(url) && map.get(url).ok) continue;
    const headers = {};
    for (const m of meta.matchAll(/^response-header-\d+=([^:]+):\s?(.*)$/gm)) {
      const k = m[1].toLowerCase();
      if (["content-encoding", "content-length", "transfer-encoding", "connection",
        "content-security-policy-report-only", "set-cookie"].includes(k)) continue;
      headers[k] = m[2];
    }
    const bodyPath = path.join(dir, name.replace(".meta", ".body"));
    const body = fs.existsSync(bodyPath) ? fs.readFileSync(bodyPath) : Buffer.alloc(0);
    map.set(url, { status, headers, body, ok });
  }
  return map;
}

(async () => {
  const [dir, url, out, name, size] = process.argv.slice(2);
  const [w, h] = (size || "480x272").split("x").map(Number);
  const desktop = w > 800;
  fs.mkdirSync(out, { recursive: true });
  const trace = loadTrace(dir);
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: w, height: h }, deviceScaleFactor: 1,
    isMobile: !desktop, hasTouch: !desktop, userAgent: UA,
  });
  let served = 0, missed = 0; const missedUrls = [];
  await context.route("**/*", async (route) => {
    const u = route.request().url().split("#")[0];
    const hit = trace.get(u);
    if (!hit) { missed++; if (missedUrls.length < 60) missedUrls.push(u.slice(0, 160)); return route.abort(); }
    served++;
    return route.fulfill({ status: hit.status, headers: hit.headers, body: hit.body });
  });
  const page = await context.newPage();
  const consoleErrors = [];
  page.on("pageerror", (e) => consoleErrors.push(String(e).slice(0, 200)));
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 }).catch((e) => console.log("goto", String(e).slice(0, 160)));
  await page.waitForTimeout(6000);
  await page.screenshot({ path: path.join(out, `${name}-chrome-top.png`) });
  if (!desktop) {
    for (const k of [1, 2, 3, 4]) {
      await page.evaluate((y) => window.scrollTo(0, y), k * 238);
      await page.waitForTimeout(400);
      await page.screenshot({ path: path.join(out, `${name}-chrome-p${k}.png`) });
    }
  }
  const info = await page.evaluate(() => ({
    title: document.title, href: location.href, innerWidth, scrollHeight: document.documentElement.scrollHeight,
    scrollWidth: document.documentElement.scrollWidth, textLength: document.body ? document.body.innerText.length : 0,
    images: document.images.length,
    loadedImages: [...document.images].filter((i) => i.complete && i.naturalWidth > 0).length,
  }));
  fs.writeFileSync(path.join(out, `${name}-chrome-info.json`), JSON.stringify({ served, missed, missedUrls, consoleErrors, ...info }, null, 1));
  console.log(JSON.stringify({ served, missed, ...info, errors: consoleErrors.length }));
  await browser.close();
})().catch((e) => { console.error(e); process.exitCode = 1; });
