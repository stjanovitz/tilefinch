#!/usr/bin/env node
"use strict";

const assert = require("assert");
const path = require("path");
const {
  isLikelyPage,
  readRanking,
  readSupportedProperties,
  scanCss,
  scanExecutedJavaScript,
} = require("./census-features");

const ranking = readRanking(path.join(
  __dirname, "2026-07-23-cloudflare-radar-us.tsv",
));
assert.strictEqual(ranking.length, 100);
assert.strictEqual(ranking[0].domain, "google.com");
assert.strictEqual(ranking[99].domain, "taboola.com");
assert.strictEqual(
  isLikelyPage({ categories: ["Content Servers", "Trackers/Analytics"] }),
  false,
);
assert.strictEqual(
  isLikelyPage({ categories: ["Business", "Trackers/Analytics"] }),
  true,
);

const supported = readSupportedProperties([
  path.join(__dirname, "../../src/style_properties/dispatch.inc"),
  path.join(__dirname, "../../src/style_sheet/declarations.inc"),
]);
assert(supported.has("display"));
assert(supported.has("grid-template-columns"));
assert(supported.has("container-type"));
assert(supported.has("column-count"));
assert(supported.has("fill"));
assert(supported.has("stroke"));

const features = scanCss([
  "@media (width < 40rem) { .item:is([open], :hover) > span { "
    + "display: grid; container-type: inline-size; width: calc(100% - 2px) } }",
]);
assert.strictEqual(features.properties.display, 1);
assert.strictEqual(features.properties["container-type"], 1);
assert.strictEqual(features.atRules.media, 1);
assert.strictEqual(features.functions.calc, 1);
assert.strictEqual(features.selectorFeatures[":is()"], 1);
assert.strictEqual(features.selectorFeatures["child (>)"], 1);

const webApis = scanExecutedJavaScript([{
  source: "fetch('/'); neverRun(); requestAnimationFrame(() => {});"
    + "navigator.permissions.query({name:'clipboard-read'});",
  functions: [{
    ranges: [
      { count: 1, startOffset: 0, endOffset: 11 },
      { count: 0, startOffset: 12, endOffset: 23 },
      { count: 1, startOffset: 24, endOffset: 112 },
    ],
  }],
}]);
assert.strictEqual(webApis.fetch, 1);
assert.strictEqual(webApis.requestAnimationFrame, 1);
assert.strictEqual(webApis.permissions, 1);
assert.strictEqual(webApis.neverRun, undefined);

process.stdout.write("top-sites census feature tests passed\n");
