"use strict";

const fs = require("fs");
const path = require("path");

const NON_PAGE_CATEGORIES = new Set([
  "Advertisements",
  "APIs",
  "Content Servers",
  "Trackers/Analytics",
]);

function readRanking(filename) {
  const rows = [];
  for (const line of fs.readFileSync(filename, "utf8").split(/\r?\n/)) {
    if (!line || line.startsWith("#") || line.startsWith("rank\t")) continue;
    const [rankText, domain, categoriesText = ""] = line.split("\t");
    const rank = Number.parseInt(rankText, 10);
    if (!Number.isInteger(rank) || !domain) {
      throw new Error(`invalid ranking row: ${line}`);
    }
    rows.push({
      rank,
      domain,
      categories: categoriesText.split(";").filter(Boolean),
    });
  }
  rows.sort((left, right) => left.rank - right.rank);
  return rows;
}

function isLikelyPage(entry) {
  return entry.categories.length === 0
    || entry.categories.some((category) => !NON_PAGE_CATEGORIES.has(category));
}

function readSupportedProperties(filenames) {
  const properties = new Set();
  for (const filename of Array.isArray(filenames) ? filenames : [filenames]) {
    const source = fs.readFileSync(filename, "utf8");
    const matcher = /\{\s*"([^"]+)"\s*,\s*\d+\s*,\s*style_property_/g;
    for (let match = matcher.exec(source); match;
         match = matcher.exec(source)) {
      properties.add(match[1]);
    }
    const retainedMatcher =
      /static const char \*const (?:retained_layout_properties|retained_presentation_names)\s*\[[^\]]*\]\s*=\s*\{([\s\S]*?)\};/g;
    for (const retained of source.matchAll(retainedMatcher)) {
      for (const match of retained[1].matchAll(/"([^"]+)"/g)) {
        properties.add(match[1]);
      }
    }
  }
  if (properties.size < 25) {
    throw new Error("could not read Tilefinch's implemented property tables");
  }
  return properties;
}

function newFeatureSummary() {
  return {
    properties: Object.create(null),
    atRules: Object.create(null),
    functions: Object.create(null),
    pseudos: Object.create(null),
    selectorFeatures: Object.create(null),
    webApis: Object.create(null),
  };
}

function bump(counter, key, count = 1) {
  if (!key) return;
  counter[key] = (counter[key] || 0) + count;
}

function scanCss(cssTexts) {
  const totals = newFeatureSummary();
  const identifierStart = (character) => /[a-zA-Z_-]/.test(character);
  const identifierPart = (character) => /[\w-]/.test(character);
  const scanSelector = (selector) => {
    selector = selector.trim();
    if (!selector || selector.startsWith("@")) return;
    if (selector.includes(">")) bump(totals.selectorFeatures, "child (>)");
    if (selector.includes("+")) bump(totals.selectorFeatures, "adjacent (+)");
    if (selector.includes("~")) bump(totals.selectorFeatures, "sibling (~)");
    if (selector.includes("[")) bump(totals.selectorFeatures, "attribute");
    if (selector.includes("::")) bump(totals.selectorFeatures, "pseudo-element");
    if (selector.includes(":is(")) bump(totals.selectorFeatures, ":is()");
    if (selector.includes(":where(")) bump(totals.selectorFeatures, ":where()");
    if (selector.includes(":not(")) bump(totals.selectorFeatures, ":not()");
    if (selector.includes(":has(")) bump(totals.selectorFeatures, ":has()");
  };

  for (const original of cssTexts) {
    const css = String(original || "");
    let statementStart = 0;
    let inDeclaration = false;
    for (let at = 0; at < css.length;) {
      if (css[at] === "/" && css[at + 1] === "*") {
        const end = css.indexOf("*/", at + 2);
        at = end < 0 ? css.length : end + 2;
        continue;
      }
      if (css[at] === "\"" || css[at] === "'") {
        const quote = css[at++];
        while (at < css.length) {
          if (css[at] === "\\") at += 2;
          else if (css[at++] === quote) break;
        }
        continue;
      }
      if (css[at] === "@") {
        let end = at + 1;
        while (end < css.length && identifierPart(css[end])) end += 1;
        bump(totals.atRules, css.slice(at + 1, end).toLowerCase());
        at = end;
        continue;
      }
      if (css[at] === "{") {
        scanSelector(css.slice(statementStart, at).toLowerCase());
        statementStart = at + 1;
        inDeclaration = false;
        at += 1;
        continue;
      }
      if (css[at] === ";" || css[at] === "}") {
        statementStart = at + 1;
        inDeclaration = false;
        at += 1;
        continue;
      }
      if (css[at] === ":") {
        const candidate = css.slice(statementStart, at)
          .replace(/\/\*[\s\S]*?\*\//g, "").trim().toLowerCase();
        if (candidate.length <= 64
            && /^(?:--|-\w+-)?[a-zA-Z_][\w-]*$/.test(candidate)) {
          bump(totals.properties, candidate);
          inDeclaration = true;
          at += 1;
          continue;
        }
        if (!inDeclaration) {
          let end = at + 1;
          if (css[end] === ":") end += 1;
          const start = end;
          while (end < css.length && identifierPart(css[end])) end += 1;
          bump(totals.pseudos, css.slice(start, end).toLowerCase());
          at = end;
          continue;
        }
      }
      if (inDeclaration && identifierStart(css[at])) {
        let end = at + 1;
        while (end < css.length && identifierPart(css[end])) end += 1;
        let next = end;
        while (next < css.length && /\s/.test(css[next])) next += 1;
        if (css[next] === "(") {
          const name = css.slice(at, end).toLowerCase();
          if (name !== "url") bump(totals.functions, name);
        }
        at = end;
        continue;
      }
      at += 1;
    }
  }
  return totals;
}

/*
 * These are compatibility signals, not proof that a complete API is needed.
 * Only source ranges that V8 reports as executed are scanned, and the caller
 * discards source immediately after deriving these aggregate token counts.
 * Keep the names exact so minified local identifiers do not create substring
 * matches.
 */
const WEB_API_CONSTRUCTORS = [
  "AbortController", "AbortSignal", "AudioContext", "BroadcastChannel",
  "CacheStorage", "CompressionStream", "CSSStyleSheet", "DOMParser",
  "EventSource", "FileReader", "FormData", "IntersectionObserver",
  "MediaSource", "MessageChannel", "MutationObserver", "Notification",
  "OffscreenCanvas", "PaymentRequest", "PerformanceObserver",
  "ReadableStream", "ResizeObserver", "RTCPeerConnection", "SharedWorker",
  "TextDecoder", "TextEncoder", "TransformStream", "URLPattern",
  "URLSearchParams", "WebAssembly", "WebSocket", "Worker",
  "WritableStream",
];

const WEB_API_MEMBER_PROBES = [
  ["adoptedStyleSheets", /(?:document|shadowRoot|this)\s*\.\s*adoptedStyleSheets\b/g],
  ["attachShadow", /\.attachShadow\s*\(/g],
  ["caches", /\bcaches\s*\./g],
  ["cookieStore", /\bcookieStore\s*\./g],
  ["crypto", /\bcrypto\s*\./g],
  ["customElements", /\bcustomElements\s*\./g],
  ["fetch", /\bfetch\s*\(/g],
  ["indexedDB", /\bindexedDB\s*\./g],
  ["localStorage", /\blocalStorage(?:\s*\.|\s*\[)/g],
  ["matchMedia", /\bmatchMedia\s*\(/g],
  ["queueMicrotask", /\bqueueMicrotask\s*\(/g],
  ["requestAnimationFrame", /\brequestAnimationFrame\s*\(/g],
  ["requestIdleCallback", /\brequestIdleCallback\s*\(/g],
  ["scheduler", /\bscheduler\s*\.\s*(?:postTask|yield)\b/g],
  ["sessionStorage", /\bsessionStorage(?:\s*\.|\s*\[)/g],
  ["showModal", /\.showModal\s*\(/g],
  ["showPopover", /\.showPopover\s*\(/g],
  ["structuredClone", /\bstructuredClone\s*\(/g],
  ["visualViewport", /\bvisualViewport\s*\./g],
  ["clipboard", /\bnavigator\s*(?:\.\s*clipboard|\[\s*["']clipboard["']\s*\])/g],
  ["deviceMemory", /\bnavigator\s*(?:\.\s*deviceMemory|\[\s*["']deviceMemory["']\s*\])/g],
  ["geolocation", /\bnavigator\s*(?:\.\s*geolocation|\[\s*["']geolocation["']\s*\])/g],
  ["hardwareConcurrency", /\bnavigator\s*(?:\.\s*hardwareConcurrency|\[\s*["']hardwareConcurrency["']\s*\])/g],
  ["locks", /\bnavigator\s*(?:\.\s*locks|\[\s*["']locks["']\s*\])/g],
  ["permissions", /\bnavigator\s*(?:\.\s*permissions|\[\s*["']permissions["']\s*\])/g],
  ["sendBeacon", /\bnavigator\s*(?:\.\s*sendBeacon|\[\s*["']sendBeacon["']\s*\])/g],
  ["serviceWorker", /\bnavigator\s*(?:\.\s*serviceWorker|\[\s*["']serviceWorker["']\s*\])/g],
  ["share", /\bnavigator\s*(?:\.\s*share|\[\s*["']share["']\s*\])/g],
  ["userAgentData", /\bnavigator\s*(?:\.\s*userAgentData|\[\s*["']userAgentData["']\s*\])/g],
];

function executedCoverageRanges(entry) {
  const ranges = [];
  for (const fn of entry.functions || []) {
    for (const range of fn.ranges || []) {
      if (range.count > 0 && range.endOffset > range.startOffset) {
        ranges.push([range.startOffset, range.endOffset]);
      }
    }
  }
  ranges.sort((left, right) => left[0] - right[0] || right[1] - left[1]);
  const merged = [];
  for (const range of ranges) {
    const previous = merged[merged.length - 1];
    if (previous && range[0] <= previous[1]) {
      if (range[1] > previous[1]) previous[1] = range[1];
    } else {
      merged.push(range.slice());
    }
  }
  return merged;
}

function scanExecutedJavaScript(entries, maxExecutedBytes = 4 * 1024 * 1024) {
  const totals = Object.create(null);
  let remaining = maxExecutedBytes;
  for (const entry of entries || []) {
    const source = String(entry.source || "");
    if (!source || remaining === 0) continue;
    for (const [start, end] of executedCoverageRanges(entry)) {
      if (remaining === 0) break;
      const boundedEnd = Math.min(end, start + remaining, source.length);
      if (boundedEnd <= start) continue;
      const executed = source.slice(start, boundedEnd);
      remaining -= boundedEnd - start;
      for (const name of WEB_API_CONSTRUCTORS) {
        const matcher = new RegExp(`(^|[^\\w$])${name}([^\\w$]|$)`, "g");
        let count = 0;
        while (matcher.exec(executed)) count += 1;
        if (count) bump(totals, name, count);
      }
      for (const [name, expression] of WEB_API_MEMBER_PROBES) {
        expression.lastIndex = 0;
        let count = 0;
        while (expression.exec(executed)) count += 1;
        if (count) bump(totals, name, count);
      }
    }
  }
  return totals;
}

function addSiteFeatures(aggregate, siteFeatures) {
  for (const family of Object.keys(aggregate)) {
    for (const [name, occurrences] of Object.entries(siteFeatures[family])) {
      const destination = aggregate[family][name]
        || { sites: 0, occurrences: 0 };
      destination.sites += 1;
      destination.occurrences += occurrences;
      aggregate[family][name] = destination;
    }
  }
}

function sortedFeatures(counter) {
  return Object.entries(counter).sort((left, right) =>
    right[1].sites - left[1].sites
    || right[1].occurrences - left[1].occurrences
    || left[0].localeCompare(right[0]));
}

function markdownTable(headers, rows) {
  const escape = (value) => String(value).replace(/\|/g, "\\|");
  return [
    `| ${headers.map(escape).join(" | ")} |`,
    `| ${headers.map(() => "---").join(" | ")} |`,
    ...rows.map((row) => `| ${row.map(escape).join(" | ")} |`),
  ].join("\n");
}

function topFeatureRows(counter, denominator, limit = 20, filter = null) {
  return sortedFeatures(counter)
    .filter(([name, value]) => !filter || filter(name, value))
    .slice(0, limit)
    .map(([name, value]) => [
      `\`${name}\``,
      value.sites,
      `${((value.sites / Math.max(denominator, 1)) * 100).toFixed(1)}%`,
      value.occurrences,
    ]);
}

function countOutcomes(sites) {
  const outcomes = Object.create(null);
  for (const site of sites) bump(outcomes, site.outcome);
  return outcomes;
}

function renderReport(result) {
  const usable = result.summary.usable;
  const supported = new Set(result.tilefinch.supportedProperties);
  const unsupportedFilter = (name) =>
    !name.startsWith("--") && !name.startsWith("-") && !supported.has(name);
  const outcomeRows = Object.entries(countOutcomes(result.sites))
    .sort((left, right) => right[1] - left[1])
    .map(([outcome, count]) => [outcome, count]);
  const failedRows = result.sites
    .filter((site) => site.outcome !== "usable")
    .map((site) => [
      site.rank,
      site.domain,
      site.outcome,
      site.status || "—",
      site.error || site.title || "—",
    ]);
  const computedRows = sortedFeatures(result.computed.display)
    .slice(0, 15)
    .map(([name, value]) => [
      `\`${name}\``,
      value.sites,
      `${((value.sites / Math.max(usable, 1)) * 100).toFixed(1)}%`,
      value.occurrences,
    ]);

  return `# US top-sites web-feature census

Generated ${result.generatedAt} from the public Cloudflare Radar US top-100
ranking dated ${result.rankingDate}. This is a compatibility-prioritization
sample, not a market-share claim.

## Method

- ${result.summary.ranked} ranked domains were read.
- ${result.summary.filtered} obvious CDN, advertising, analytics, or API-only
  entries were excluded by category before navigation.
- ${result.summary.attempted} likely page origins were visited in headless
  Chromium at a 480×272 mobile viewport.
- Common image, media, and font URL extensions were blocked. Authored CSS was
  sampled from the final document's readable CSSOM and inline style attributes,
  bounded to ${result.bounds.maxStylesheets} stylesheets and
  ${result.bounds.maxCssBytesPerSite} decoded CSS bytes per site. Cross-origin
  sheets that Chromium did not expose through CSSOM were omitted; final
  computed-layout counts still reflect their applied cascade.
- Counts below use **site presence** as the primary measure. Occurrence totals
  are included only as supporting detail.
- Unsupported-property candidates are compared with the property names in
  Tilefinch's checked-in computed and retained-layout property tables. They
  still require semantic review:
  browser defaults, aliases, and ignorable enhancements can make a syntactic
  miss low priority.
- Page bodies and stylesheet bodies were not retained.

## Reachability

${markdownTable(["Outcome", "Sites"], outcomeRows)}

${result.summary.usable} sites contributed to the feature consensus.

${failedRows.length ? `### Excluded after navigation

${markdownTable(["Rank", "Domain", "Outcome", "HTTP", "Detail"], failedRows)}
` : ""}
## Authored CSS consensus

### Properties

${markdownTable(
    ["Property", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.authored.properties, usable, 25),
  )}

### Common properties not in Tilefinch's dispatch table

${markdownTable(
    ["Property", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.authored.properties, usable, 30, unsupportedFilter),
  )}

### At-rules

${markdownTable(
    ["At-rule", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.authored.atRules, usable, 20),
  )}

### CSS functions

${markdownTable(
    ["Function", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.authored.functions, usable, 20),
  )}

### Selector features

${markdownTable(
    ["Feature", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.authored.selectorFeatures, usable, 20),
  )}

## Executed JavaScript API signals

Only V8 coverage ranges that executed before the bounded snapshot are scanned.
These token-presence counts identify APIs worth semantic review; they do not
prove that every occurrence reached an API call or that a complete
implementation is required.

${markdownTable(
    ["API token", "Sites", "Usable sites", "Executed occurrences"],
    topFeatureRows(result.authored.webApis, usable, 40),
  )}

## Final computed layout

### Display modes

${markdownTable(
    ["Display", "Sites", "Usable sites", "Elements sampled"],
    computedRows,
  )}

### Positioning modes

${markdownTable(
    ["Position", "Sites", "Usable sites", "Elements sampled"],
    topFeatureRows(result.computed.position, usable, 12),
  )}

## HTML consensus

### Elements

${markdownTable(
    ["Element", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.html.tags, usable, 25),
  )}

### Attributes

${markdownTable(
    ["Attribute", "Sites", "Usable sites", "Occurrences"],
    topFeatureRows(result.html.attributes, usable, 25),
  )}

## Interpretation

Use the site-presence columns to choose future WPT additions and generic
correctness work. Do not implement a feature solely because its raw occurrence
count is high, and do not add domain-specific engine behavior from this report.
The separate five-site secondary acceptance manifest is deliberately opt-in and
is not part of routine builds or tests.
`;
}

module.exports = {
  addSiteFeatures,
  isLikelyPage,
  newFeatureSummary,
  readRanking,
  readSupportedProperties,
  renderReport,
  scanCss,
  scanExecutedJavaScript,
  sortedFeatures,
};
