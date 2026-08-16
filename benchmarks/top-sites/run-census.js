#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const {
  addSiteFeatures,
  isLikelyPage,
  newFeatureSummary,
  readRanking,
  readSupportedProperties,
  renderReport,
  scanExecutedJavaScript,
} = require("./census-features");
const { scanCss } = require("./census-features");

const ROOT = path.resolve(__dirname, "../..");

function parseArguments(argv) {
  const options = {
    ranking: path.join(__dirname, "2026-07-23-cloudflare-radar-us.tsv"),
    output: "/tmp/tilefinch-top-sites-census",
    limit: 100,
    concurrency: 4,
    timeoutMs: 12000,
    settleMs: 800,
    maxStylesheets: 32,
    maxCssBytesPerSite: 1536 * 1024,
    maxCssBytesPerSheet: 256 * 1024,
  };
  for (let at = 2; at < argv.length; at += 1) {
    const name = argv[at];
    const value = argv[++at];
    if (!value || !name.startsWith("--")) throw new Error(`invalid argument ${name}`);
    const key = {
      "--ranking": "ranking",
      "--output": "output",
      "--limit": "limit",
      "--concurrency": "concurrency",
      "--timeout-ms": "timeoutMs",
      "--settle-ms": "settleMs",
    }[name];
    if (!key) throw new Error(`unknown argument ${name}`);
    options[key] = ["ranking", "output"].includes(key) ? value : Number(value);
  }
  for (const key of ["limit", "concurrency", "timeoutMs", "settleMs"]) {
    if (!Number.isFinite(options[key]) || options[key] < 1) {
      throw new Error(`invalid --${key}: ${options[key]}`);
    }
  }
  return options;
}

function mergePresence(aggregate, values) {
  for (const [name, occurrences] of Object.entries(values)) {
    const current = aggregate[name] || { sites: 0, occurrences: 0 };
    current.sites += 1;
    current.occurrences += occurrences;
    aggregate[name] = current;
  }
}

function classifySnapshot(snapshot, status, error) {
  if (error) return "navigation-error";
  if (!status || status >= 400) return "http-error";
  if (!snapshot || snapshot.elementCount < 10) return "empty";
  const signal = `${snapshot.title}\n${snapshot.bodySignal}`.toLowerCase();
  if (/just a moment|attention required|enable javascript and cookies|access denied|unusual traffic/.test(signal)) {
    return "challenge";
  }
  return "usable";
}

function bounded(promise, timeoutMs, label) {
  let timer = null;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(
      () => reject(new Error(`${label} exceeded ${timeoutMs}ms`)),
      timeoutMs,
    );
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

async function inspectSite(browser, entry, options, onContext) {
  const context = await browser.newContext({
    viewport: { width: 480, height: 272 },
    screen: { width: 480, height: 272 },
    deviceScaleFactor: 1,
    isMobile: true,
    hasTouch: true,
    locale: "en-US",
    colorScheme: "light",
    serviceWorkers: "block",
    javaScriptEnabled: true,
  });
  onContext(context);
  const page = await context.newPage();
  await page.coverage.startJSCoverage({
    /* Exclude Playwright's own anonymous evaluation helpers. Page inline
       scripts retain the document URL and remain covered. */
    reportAnonymousScripts: false,
    resetOnNavigation: false,
  });
  const cssTexts = [];
  let cssBytes = 0;
  let requestCount = 0;
  const appendCss = (value, perSourceLimit = options.maxCssBytesPerSheet) => {
    if (cssTexts.length >= options.maxStylesheets
        || cssBytes >= options.maxCssBytesPerSite) return;
    const source = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
    const length = Math.min(
      source.length,
      perSourceLimit,
      options.maxCssBytesPerSite - cssBytes,
    );
    if (length <= 0) return;
    cssTexts.push(source.subarray(0, length).toString("utf8"));
    cssBytes += length;
  };

  page.on("request", () => { requestCount += 1; });
  await page.route(
    /\.(?:avif|gif|jpe?g|mp4|otf|png|ttf|webm|webp|woff2?)(?:[?#].*)?$/i,
    (route) => route.abort().catch(() => {}),
  );

  let mainResponse = null;
  let snapshot = null;
  let error = "";
  let jsCoverage = [];
  const started = Date.now();
  try {
    mainResponse = await page.goto(`https://${entry.domain}/`, {
      waitUntil: "domcontentloaded",
      timeout: options.timeoutMs,
    });
    await page.waitForTimeout(options.settleMs);
    snapshot = await page.evaluate(({ maxStylesheets, maxCssChars }) => {
      const valueProperties = [
        "backdrop-filter", "border-image", "border-image-source",
        "border-end-end-radius", "border-end-start-radius",
        "border-start-end-radius", "border-start-start-radius", "color-scheme",
        "cursor", "hyphens", "isolation", "mix-blend-mode",
        "overscroll-behavior", "rotate", "scale", "scroll-behavior",
        "scroll-snap-align", "scroll-snap-type", "scrollbar-color",
        "scrollbar-width", "text-size-adjust", "text-wrap", "touch-action",
        "translate", "user-select",
      ];
      const counts = (values) => {
        const result = Object.create(null);
        for (const value of values) result[value] = (result[value] || 0) + 1;
        return result;
      };
      const all = Array.from(document.querySelectorAll("*"));
      const sampled = all.slice(0, 2500);
      const tags = counts(all.map((element) => element.localName));
      const attributes = Object.create(null);
      for (const element of all) {
        for (const attribute of element.attributes) {
          attributes[attribute.name] = (attributes[attribute.name] || 0) + 1;
        }
      }
      const display = [];
      const position = [];
      let visibleElements = 0;
      for (const element of sampled) {
        const style = getComputedStyle(element);
        if (style.display === "none" || style.visibility === "hidden") continue;
        const bounds = element.getBoundingClientRect();
        if (bounds.width > 0 && bounds.height > 0) visibleElements += 1;
        display.push(style.display);
        position.push(style.position);
      }
      const authoredCss = [];
      const propertyValues = Object.create(null);
      let propertyValueKinds = 0;
      let propertyRulesVisited = 0;
      const noteStyleValues = (style) => {
        if (!style || propertyValueKinds >= 512) return;
        for (const name of valueProperties) {
          let value = style.getPropertyValue(name);
          if (!value) continue;
          value = value.trim().toLowerCase();
          value = /\burl\s*\(/.test(value)
            ? "url(...)"
            : value.replace(/\s+/g, " ").slice(0, 96);
          if (!value) continue;
          const key = `${name}: ${value}`;
          if (propertyValues[key] === undefined) propertyValueKinds += 1;
          propertyValues[key] = (propertyValues[key] || 0) + 1;
          if (propertyValueKinds >= 512) break;
        }
      };
      const noteRuleValues = (rules, depth = 0) => {
        if (!rules || depth > 8 || propertyValueKinds >= 512
            || propertyRulesVisited >= 8192) return;
        for (const rule of Array.from(rules).slice(0, 4096)) {
          if (propertyRulesVisited++ >= 8192) break;
          noteStyleValues(rule.style);
          let nested = null;
          try { nested = rule.cssRules; } catch (_error) {}
          if (nested) noteRuleValues(nested, depth + 1);
          if (propertyValueKinds >= 512) break;
        }
      };
      let cssChars = 0;
      let readableStylesheets = 0;
      for (const sheet of Array.from(document.styleSheets).slice(0, maxStylesheets)) {
        let rules;
        try { rules = Array.from(sheet.cssRules); }
        catch (_error) { continue; }
        readableStylesheets += 1;
        noteRuleValues(rules);
        let sheetText = "";
        for (const rule of rules) {
          const remaining = maxCssChars - cssChars - sheetText.length;
          if (remaining <= 0) break;
          sheetText += `${rule.cssText.slice(0, remaining)}\n`;
        }
        if (sheetText) {
          authoredCss.push(sheetText);
          cssChars += sheetText.length;
        }
        if (cssChars >= maxCssChars) break;
      }
      for (const node of Array.from(document.querySelectorAll("[style]"))
        .slice(0, 1000)) noteStyleValues(node.style);
      return {
        title: document.title.slice(0, 160),
        finalUrl: location.href,
        bodySignal: (document.body?.innerText || "").slice(0, 500),
        elementCount: all.length,
        visibleElements,
        authoredCss,
        propertyValues,
        readableStylesheets,
        styleAttributes: Array.from(document.querySelectorAll("[style]"))
          .slice(0, 1000).map((node) => node.getAttribute("style") || ""),
        tags,
        attributes,
        display: counts(display),
        position: counts(position),
      };
    }, {
      maxStylesheets: options.maxStylesheets,
      maxCssChars: options.maxCssBytesPerSite,
    });
  } catch (caught) {
    error = String(caught.message || caught).split("\n")[0].slice(0, 200);
  }
  try {
    jsCoverage = await bounded(
      page.coverage.stopJSCoverage(), 3000, "JavaScript coverage stop",
    );
  } catch (_coverageError) {
    jsCoverage = [];
  }

  if (snapshot) {
    for (const text of snapshot.authoredCss) appendCss(text);
    if (snapshot.styleAttributes.length) {
      appendCss(
        snapshot.styleAttributes.map((text) => `a{${text}}`).join("\n"),
        128 * 1024,
      );
    }
  }
  const status = mainResponse ? mainResponse.status() : 0;
  const outcome = classifySnapshot(snapshot, status, error);
  const scanStarted = Date.now();
  const features = outcome === "usable"
    ? scanCss(cssTexts) : newFeatureSummary();
  if (outcome === "usable") {
    features.webApis = scanExecutedJavaScript(jsCoverage);
  }
  const featureScanMs = Date.now() - scanStarted;
  const site = {
    ...entry,
    outcome,
    status,
    finalUrl: snapshot?.finalUrl || "",
    title: snapshot?.title || "",
    error,
    durationMs: Date.now() - started,
    requestCount,
    stylesheetCount: snapshot?.readableStylesheets || 0,
    stylesheetBytes: cssBytes,
    featureScanMs,
    elementCount: snapshot?.elementCount || 0,
    visibleElements: snapshot?.visibleElements || 0,
    features,
    html: outcome === "usable" ? {
      tags: snapshot.tags,
      attributes: snapshot.attributes,
    } : { tags: {}, attributes: {} },
    propertyValues: outcome === "usable"
      ? snapshot.propertyValues : {},
    computed: outcome === "usable" ? {
      display: snapshot.display,
      position: snapshot.position,
    } : { display: {}, position: {} },
  };
  await bounded(context.close(), 3000, "browser context close").catch(() => {});
  return site;
}

function deadlineSite(entry, started, error) {
  return {
    ...entry,
    outcome: "deadline",
    status: 0,
    finalUrl: "",
    title: "",
    error: String(error.message || error).slice(0, 200),
    durationMs: Date.now() - started,
    requestCount: 0,
    stylesheetCount: 0,
    stylesheetBytes: 0,
    elementCount: 0,
    visibleElements: 0,
  };
}

async function main() {
  const options = parseArguments(process.argv);
  const ranking = readRanking(path.resolve(options.ranking)).slice(0, options.limit);
  const candidates = ranking.filter(isLikelyPage);
  const filtered = ranking.filter((entry) => !isLikelyPage(entry));
  const supported = readSupportedProperties([
    path.join(ROOT, "src/style_properties/dispatch.inc"),
    path.join(ROOT, "src/style_sheet/declarations.inc"),
  ]);
  const playwright = require("playwright");
  const sites = new Array(candidates.length);
  let cursor = 0;
  const workers = Array.from(
    { length: Math.min(options.concurrency, candidates.length) },
    async () => {
      let browser = await playwright.chromium.launch({ headless: true });
      try {
        while (true) {
          const index = cursor++;
          if (index >= candidates.length) return;
          const entry = candidates[index];
          const started = Date.now();
          let activeContext = null;
          let site;
          try {
            site = await bounded(
              inspectSite(
                browser, entry, options, (context) => { activeContext = context; },
              ),
              options.timeoutMs + options.settleMs + 7000,
              "whole-site inspection",
            );
          } catch (error) {
            site = deadlineSite(entry, started, error);
            if (activeContext) {
              await bounded(
                activeContext.close(), 2000, "deadline context close",
              ).catch(() => {});
            }
            await bounded(
              browser.close(), 3000, "deadline browser close",
            ).catch(() => {});
            browser = await playwright.chromium.launch({ headless: true });
          }
          sites[index] = site;
          process.stdout.write(
            `${String(entry.rank).padStart(3)} ${entry.domain.padEnd(28)} `
            + `${site.outcome.padEnd(16)} ${site.durationMs}ms\n`,
          );
        }
      } finally {
        await bounded(browser.close(), 3000, "worker browser close").catch(() => {});
      }
    },
  );
  await Promise.all(workers);

  const authored = newFeatureSummary();
  const html = { tags: Object.create(null), attributes: Object.create(null) };
  const computed = {
    display: Object.create(null),
    position: Object.create(null),
  };
  const authoredPropertyValues = Object.create(null);
  for (const site of sites) {
    if (site.outcome !== "usable") continue;
    addSiteFeatures(authored, site.features);
    mergePresence(html.tags, site.html.tags);
    mergePresence(html.attributes, site.html.attributes);
    mergePresence(computed.display, site.computed.display);
    mergePresence(computed.position, site.computed.position);
    mergePresence(authoredPropertyValues, site.propertyValues);
  }
  const usable = sites.filter((site) => site.outcome === "usable").length;
  const result = {
    generatedAt: new Date().toISOString(),
    rankingDate: "2026-07-23",
    source: "https://radar.cloudflare.com/domains/US",
    viewport: { width: 480, height: 272 },
    bounds: {
      maxStylesheets: options.maxStylesheets,
      maxCssBytesPerSite: options.maxCssBytesPerSite,
      timeoutMs: options.timeoutMs,
      settleMs: options.settleMs,
    },
    summary: {
      ranked: ranking.length,
      filtered: filtered.length,
      attempted: candidates.length,
      usable,
    },
    filtered,
    tilefinch: {
      supportedPropertyCount: supported.size,
      supportedProperties: Array.from(supported).sort(),
    },
    authored,
    authoredPropertyValues,
    html,
    computed,
    sites: sites.map((site) => {
      const copy = { ...site };
      delete copy.features;
      delete copy.html;
      delete copy.computed;
      delete copy.propertyValues;
      return copy;
    }),
  };

  fs.mkdirSync(path.resolve(options.output), { recursive: true });
  fs.writeFileSync(
    path.join(path.resolve(options.output), "results.json"),
    `${JSON.stringify(result, null, 2)}\n`,
  );
  fs.writeFileSync(
    path.join(path.resolve(options.output), "report.md"),
    renderReport(result),
  );
  process.stdout.write(
    `Wrote ${options.output}/report.md (${usable}/${candidates.length} usable)\n`,
  );
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
