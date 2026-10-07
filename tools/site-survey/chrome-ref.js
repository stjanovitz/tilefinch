#!/usr/bin/env node
"use strict";
// Live Chromium phone-sized reference (site survey; adapted from the news work).
// HIDE_CONSENT=1 hides (never clicks) consent overlays.
// Same contract as benchmarks/top-sites/capture-mobile-references.js:
// 480x272 CSS px, DPR 1, mobile+touch, Tilefinch's shipping compatibility UA.
// NODE_PATH must point at a node_modules with playwright (benchmarks/package.json).
// Usage: node chrome-ref.js URL OUTDIR NAME
const fs = require("fs");
const path = require("path");
const { chromium } = require("playwright");

const UA = "Mozilla/5.0 (iPhone; PlayStation Portable; Tilefinch/0.1) "
  + "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 "
  + "Mobile/15E148 Safari/604.1";

(async () => {
  const [url, out, name] = process.argv.slice(2);
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: 480, height: 272 }, deviceScaleFactor: 1,
    isMobile: true, hasTouch: true, userAgent: UA,
  });
  const page = await context.newPage();
  const pageErrors = [];
  page.on("pageerror", (e) => pageErrors.push(String(e).slice(0, 200)));
  let status = 0;
  try {
    const response = await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });
    status = response ? response.status() : 0;
  } catch (error) { console.log("goto-error", String(error).slice(0, 200)); }
  await page.waitForTimeout(6000);
  if (process.env.HIDE_CONSENT) {
    // Hide (never click) a consent overlay so the content underneath can be
    // referenced; the equivalent of Tilefinch's cookie-notice hiding.
    const removed = await page.evaluate(() => {
      let n = 0;
      for (const el of [...document.querySelectorAll("body *")]) {
        const s = getComputedStyle(el);
        if ((s.position === "fixed" || s.position === "absolute") && /onetrust|consent|truste|sp_message|privacy-manager/i.test(el.id + " " + el.className)) { el.remove(); n++; }
      }
      for (const el of [document.documentElement, document.body]) { el.style.removeProperty("overflow"); el.style.removeProperty("position"); el.style.removeProperty("height"); el.style.removeProperty("top"); }
      document.body.classList.remove("ot-overflow-hidden");
      const heading = [...document.querySelectorAll("h1,h2,h3,div,p")].find(
        (el) => (el.textContent || "").trim() === "Legal Terms and Privacy");
      if (heading) {
        let top = heading;
        for (let at = heading; at && at !== document.body; at = at.parentElement) {
          if (getComputedStyle(at).position === "fixed") top = at;
        }
        top.remove(); n++;
      }
      return n;
    });
    console.log("consent-removed", removed);
    await page.waitForTimeout(500);
  }
  await page.screenshot({ path: path.join(out, `${name}-chrome-top.png`) });
  const info = await page.evaluate(() => {
    const links = [...document.querySelectorAll("a[href]")]
      .map((a) => ({ href: a.href, text: (a.innerText || "").trim().slice(0, 90) }))
      .filter((l) => l.text.length > 25);
    const cs = (el) => getComputedStyle(el);
    const all = [...document.querySelectorAll("body *")];
    const count = (pred) => all.filter((el) => { try { return pred(cs(el)); } catch (e) { return false; } }).length;
    return {
      title: document.title, innerWidth, innerHeight,
      scrollHeight: document.documentElement.scrollHeight,
      scrollWidth: document.documentElement.scrollWidth,
      grid: count((s) => s.display.includes("grid")),
      flex: count((s) => s.display.includes("flex")),
      sticky: count((s) => s.position === "sticky"),
      fixed: count((s) => s.position === "fixed"),
      aspect: count((s) => s.aspectRatio && s.aspectRatio !== "auto"),
      gap: count((s) => s.display.includes("flex") && s.rowGap !== "normal" && s.rowGap !== "0px"),
      fonts: [...new Set(all.slice(0, 2000).map((el) => cs(el).fontFamily))].slice(0, 12),
      imgs: document.images.length,
      lazyImgs: [...document.images].filter((i) => i.loading === "lazy").length,
      srcsetImgs: [...document.images].filter((i) => i.srcset).length,
      pictures: document.querySelectorAll("picture").length,
      textLength: document.body ? document.body.innerText.length : 0,
      text: document.body ? document.body.innerText.slice(0, 6000) : "",
      links: links.slice(0, 60),
    };
  });
  info.status = status; info.url = page.url(); info.pageErrors = pageErrors;
  fs.writeFileSync(path.join(out, `${name}-chrome-info.json`), JSON.stringify(info, null, 2));
  for (const k of [1, 2, 3, 4]) {
    await page.evaluate((y) => window.scrollTo(0, y), k * 238);
    await page.waitForTimeout(700);
    await page.screenshot({ path: path.join(out, `${name}-chrome-p${k}.png`) });
  }
  await page.evaluate(() => window.scrollTo(0, 0));
  const height = Math.min(info.scrollHeight || 272, 8000);
  await page.setViewportSize({ width: 480, height });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: path.join(out, `${name}-chrome-full.png`), clip: { x: 0, y: 0, width: 480, height } });
  console.log(JSON.stringify({ status, url: info.url, innerWidth: info.innerWidth,
    scrollHeight: info.scrollHeight, title: info.title }));
  await browser.close();
})().catch((error) => { console.error(error); process.exitCode = 1; });
