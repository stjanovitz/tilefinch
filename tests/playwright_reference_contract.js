#!/usr/bin/env node
"use strict";

/* Optional pinned-Chromium integration check for the shared deterministic
   clock/RNG contract. The dependency-free Python suite covers the same pure
   functions; this file verifies Chromium descriptor/brand behavior. */

const assert = require("node:assert/strict");
const childProcess = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const capture = require("../benchmarks/capture-reference.js");

const MANIFEST_FIELDS = [
  "scenario", "url", "replay_dir", "trace_sha256", "expected_http",
  "required_title", "required_state_marker", "fallback_markers",
  "interstitial_markers", "device_width", "device_height", "css_width",
  "css_height", "scale_numerator", "scale_denominator", "checkpoints",
  "reference_state", "limit_mb", "ticks", "tick_ms", "max_download_kb",
  "script_timeout_ms", "script_heap_mb", "script_total_mb", "script_file_kb",
  "script_count", "min_stylesheets_loaded", "min_images_loaded",
  "min_scripts_loaded", "min_network_completions", "max_pending",
];

function writeTraceRecord(directory, index, record) {
  const body = Buffer.isBuffer(record.body)
    ? record.body : Buffer.from(String(record.body || ""));
  const headers = record.headers || [];
  const lines = [
    `method=${record.method || "GET"}`,
    `url=${record.url}`,
    "success=1",
    `status=${record.status || 200}`,
    `length=${body.length}`,
    `content-type=${record.contentType || "application/octet-stream"}`,
    `response-header-count=${headers.length}`,
    ...headers.map(([name, value], offset) => `response-header-${offset}=${name}: ${value}`),
    "set-cookie-count=0",
    `async-delay-pumps=${record.asyncDelayPumps || 1}`,
  ];
  const stem = String(index).padStart(4, "0");
  fs.writeFileSync(path.join(directory, `${stem}.meta`), `${lines.join("\n")}\n`);
  fs.writeFileSync(path.join(directory, `${stem}.body`), body);
}

function runHermeticCapture({ scenario, url, title, marker, records }) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "tilefinch-reference-contract-"));
  try {
    const traceRoot = path.join(root, "traces");
    const traceDirectory = path.join(traceRoot, scenario);
    const outputRoot = path.join(root, "output");
    fs.mkdirSync(traceDirectory, { recursive: true });
    fs.writeFileSync(path.join(traceDirectory, "trace.meta"), [
      "psp-http-trace-clock=1",
      "origin-ms=1700000000042",
      "capture-complete=yes",
      `record-count=${records.length}`,
      "",
    ].join("\n"));
    records.forEach((record, index) => writeTraceRecord(traceDirectory, index, record));
    const digest = capture.traceDigest(traceDirectory);
    const manifest = path.join(root, "scenarios.tsv");
    const row = {
      scenario, url, replay_dir: scenario, trace_sha256: digest,
      expected_http: "200", required_title: title, required_state_marker: marker,
      fallback_markers: "-", interstitial_markers: "-",
      device_width: "160", device_height: "120", css_width: "160", css_height: "120",
      scale_numerator: "1", scale_denominator: "1",
      checkpoints: "top|selector:body|bottom",
      reference_state: `${scenario}/reference-state.json`, limit_mb: "4",
      ticks: "2", tick_ms: "16", max_download_kb: "1024",
      script_timeout_ms: "10000", script_heap_mb: "4", script_total_mb: "1",
      script_file_kb: "256", script_count: "8", min_stylesheets_loaded: "0",
      min_images_loaded: "0", min_scripts_loaded: "0",
      min_network_completions: "1", max_pending: "0",
    };
    fs.writeFileSync(
      manifest,
      `${MANIFEST_FIELDS.join("\t")}\n${MANIFEST_FIELDS.map((field) => row[field]).join("\t")}\n`,
    );
    const completed = childProcess.spawnSync(process.execPath, [
      path.join(__dirname, "..", "benchmarks", "capture-reference.js"),
      "--manifest", manifest,
      "--scenario", scenario,
      "--trace-root", traceRoot,
      "--output-root", outputRoot,
      "--browser", "chromium",
      "--settle-ms", "50",
      "--timeout-ms", "10000",
    ], {
      cwd: path.join(__dirname, ".."),
      env: process.env,
      encoding: "utf8",
      timeout: 45000,
    });
    assert.equal(completed.signal, null, completed.stderr || completed.stdout);
    const stateName = completed.status === 0
      ? "reference-state.json" : "reference-diagnostic.json";
    const statePath = path.join(outputRoot, scenario, stateName);
    assert.equal(fs.existsSync(statePath), true, [
      `capture exited ${completed.status}`,
      completed.stdout,
      completed.stderr,
    ].join("\n"));
    return {
      status: completed.status,
      state: JSON.parse(fs.readFileSync(statePath, "utf8")),
      stdout: completed.stdout,
      stderr: completed.stderr,
    };
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

async function main() {
  const { chromium } = require("playwright");
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ locale: "en-US", timezoneId: "UTC" });
  const originMs = 1700000000042;
  await context.clock.install({ time: originMs });
  await context.clock.pauseAt(originMs);
  await context.route("https://example.test/**", (route) => route.fulfill({
    status: 200, contentType: "text/html", body: "<title>contract</title>",
  }));
  const page = await context.newPage();
  await page.goto("https://example.test/contract");
  const before = await page.evaluate(() => {
    const flags = (descriptor) => ({
      configurable: descriptor.configurable,
      enumerable: descriptor.enumerable,
      writable: descriptor.writable,
    });
    return {
      random: flags(Object.getOwnPropertyDescriptor(Math, "random")),
      randomValues: flags(Object.getOwnPropertyDescriptor(Crypto.prototype, "getRandomValues")),
      uuid: flags(Object.getOwnPropertyDescriptor(Crypto.prototype, "randomUUID")),
      date: flags(Object.getOwnPropertyDescriptor(globalThis, "Date")),
    };
  });
  await page.evaluate(capture.installReplayEnvironment, {
    version: "deterministic-hermetic-v3",
    clockVersion: "playwright-clock-paused-v2",
    clockContract: "dual-domain-ms-call-v2",
    clockScope: "top-level-realm-v1",
    rngVersion: "splitmix64-url-scope-v1",
    seedSource: "trace-origin-ms-v1",
    intlContract: "bounded-en-us-utc-v1",
    seedSha256: "0".repeat(64),
    seedU64: String(0x0123456789abcdefn),
    originMs,
  });
  const initial = await page.evaluate(() => [
    performance.timeOrigin, Date.now(), performance.now(),
  ]);
  await page.evaluate(() => globalThis.__tilefinchAdvanceReplayClock(16));
  const timer = page.evaluate(() => new Promise((resolve) => {
    setTimeout(() => {
      const values = [Date.now(), performance.now()];
      requestAnimationFrame((timestamp) => resolve([
        ...values, timestamp, Date.now(), performance.now(), document.timeline.currentTime,
      ]));
    }, 10);
  }));
  await context.clock.runFor(16);
  const vector = [...initial, ...await timer];
  assert.deepEqual(vector, [
    1700000000042, 1700000000042, 0,
    1700000000059, 17, 18, 1700000000060, 18, 19,
  ]);
  const shape = await page.evaluate(() => {
    const flags = (descriptor) => ({
      configurable: descriptor.configurable,
      enumerable: descriptor.enumerable,
      writable: descriptor.writable,
    });
    for (let index = 0; index < 200; index += 1) {
      performance.mark(`bounded-${index}`, { startTime: index, detail: index });
    }
    let evictedMarkError = "";
    try { performance.measure("evicted", "bounded-72", "bounded-199"); }
    catch (error) { evictedMarkError = error.name; }
    const retainedMeasure = performance.measure("retained", "bounded-73", "bounded-199");
    performance.clearMarks("bounded-199");
    let clearedMarkError = "";
    try { performance.measure("cleared", "bounded-199"); }
    catch (error) { clearedMarkError = error.name; }
    performance.clearMeasures("retained");
    let observerCalls = 0;
    const observerCallback = () => { observerCalls += 1; };
    const observer = new PerformanceObserver(observerCallback);
    observer.observe({ entryTypes: ["mark", "measure"] });
    performance.mark("observer-hidden", { startTime: 250 });
    const observerRecords = observer.takeRecords();
    observer.disconnect();
    const observerDescriptor = Object.getOwnPropertyDescriptor(
      PerformanceObserver, "supportedEntryTypes",
    );
    return {
      random: flags(Object.getOwnPropertyDescriptor(Math, "random")),
      randomValues: flags(Object.getOwnPropertyDescriptor(Crypto.prototype, "getRandomValues")),
      uuid: flags(Object.getOwnPropertyDescriptor(Crypto.prototype, "randomUUID")),
      date: flags(Object.getOwnPropertyDescriptor(globalThis, "Date")),
      uuidOwn: Object.prototype.hasOwnProperty.call(crypto, "randomUUID"),
      native: [Math.random, crypto.getRandomValues, crypto.randomUUID, Date]
        .every((value) => Function.prototype.toString.call(value).includes("[native code]")),
      dateInstance: new Date() instanceof Date,
      dateConstructor: Date.prototype.constructor === Date,
      entries: [performance.getEntries(), performance.getEntriesByType("navigation")],
      observerTypes: PerformanceObserver.supportedEntryTypes,
      observerDescriptor: {
        configurable: observerDescriptor.configurable,
        enumerable: observerDescriptor.enumerable,
        writable: observerDescriptor.writable,
      },
      performanceBound: {
        evictedMarkError, clearedMarkError, observerCalls,
        observerRecords, observerCallbackRetained: observer.callback === observerCallback,
        retained: retainedMeasure.toJSON(),
        detailOwn: Object.prototype.hasOwnProperty.call(retainedMeasure, "detail"),
        visible: performance.getEntries(),
        native: [performance.mark, performance.measure, performance.clearMarks,
          performance.clearMeasures, PerformanceObserver,
          PerformanceObserver.prototype.takeRecords]
          .every((value) => Function.prototype.toString.call(value).includes("[native code]")),
      },
      intl: (() => {
        const instant = 1700000000042;
        const date = new Intl.DateTimeFormat("en-US", { timeZone: "UTC" });
        return {
          keys: Object.keys(Intl),
          date: date.format(instant),
          parts: date.formatToParts(instant).map((part) => `${part.type}:${part.value}`),
          resolved: date.resolvedOptions(),
          time: new Intl.DateTimeFormat("en-US", {
            timeZone: "UTC", hour: "numeric",
          }).format(instant),
          number: new Intl.NumberFormat("en-US").format(1234.5),
          percent: new Intl.NumberFormat("en-US", { style: "percent" }).format(.25),
          currency: new Intl.NumberFormat("en-US", {
            style: "currency", currency: "USD",
          }).format(1234.5),
          plurals: [
            new Intl.PluralRules("en-US").select(1),
            new Intl.PluralRules("en-US").select(2),
            new Intl.PluralRules("en-US", { type: "ordinal" }).select(2),
          ],
          locale: String(new Intl.Locale("en_US")),
          native: Object.values(Intl).filter((value) => typeof value === "function")
            .every((value) => Function.prototype.toString.call(value).includes("[native code]")),
        };
      })(),
    };
  });
  assert.deepEqual(
    { random: shape.random, randomValues: shape.randomValues, uuid: shape.uuid, date: shape.date },
    before,
  );
  assert.equal(shape.uuidOwn, false);
  assert.equal(shape.native, true);
  assert.equal(shape.dateInstance, true);
  assert.equal(shape.dateConstructor, true);
  assert.deepEqual(shape.entries, [[], []]);
  assert.deepEqual(shape.observerTypes, []);
  assert.deepEqual(shape.observerDescriptor, {
    configurable: true, enumerable: true, writable: false,
  });
  assert.deepEqual(shape.performanceBound, {
    evictedMarkError: "SyntaxError",
    clearedMarkError: "SyntaxError",
    observerCalls: 0,
    observerRecords: [],
    observerCallbackRetained: true,
    retained: {
      name: "retained", entryType: "measure", startTime: 73, duration: 126,
    },
    detailOwn: true,
    visible: [],
    native: true,
  });
  assert.deepEqual(shape.intl, {
    keys: [
      "Locale", "NumberFormat", "PluralRules", "DateTimeFormat", "Collator",
      "RelativeTimeFormat", "ListFormat", "DisplayNames", "getCanonicalLocales",
    ],
    date: "11/14/2023",
    parts: ["month:11", "literal:/", "day:14", "literal:/", "year:2023"],
    resolved: {
      locale: "en-US", calendar: "gregory", numberingSystem: "latn", timeZone: "UTC",
      year: "numeric", month: "numeric", day: "numeric",
    },
    time: "10:13:20 PM",
    number: "1,234.5",
    percent: "25%",
    currency: "USD 1,234.5",
    plurals: ["one", "other", "two"],
    locale: "en-US",
    native: true,
  });
  await context.close();
  const limitOrigin = 8_639_999_999_999_998;
  const limitContext = await browser.newContext({ locale: "en-US", timezoneId: "UTC" });
  await limitContext.clock.install({ time: limitOrigin });
  await limitContext.clock.pauseAt(limitOrigin);
  await limitContext.route("https://date-limit.test/**", (route) => route.fulfill({
    status: 200, contentType: "text/html", body: "<title>date limit</title>",
  }));
  const limitPage = await limitContext.newPage();
  await limitPage.goto("https://date-limit.test/");
  await limitPage.evaluate(capture.installReplayEnvironment, {
    version: "deterministic-hermetic-v3",
    clockVersion: "playwright-clock-paused-v2",
    clockContract: "dual-domain-ms-call-v2",
    clockScope: "top-level-realm-v1",
    rngVersion: "splitmix64-url-scope-v1",
    seedSource: "trace-origin-ms-v1",
    intlContract: "bounded-en-us-utc-v1",
    seedSha256: "0".repeat(64),
    seedU64: String(limitOrigin),
    originMs: limitOrigin,
  });
  const limitVector = await limitPage.evaluate(() => {
    const called = Date();
    return [Date.now(), new Date().getTime(), Date.parse(called), Date.now(), performance.now()];
  });
  assert.deepEqual(limitVector, [
    8_639_999_999_999_999,
    8_640_000_000_000_000,
    8_639_999_999_999_000,
    8_640_000_000_000_000,
    0,
  ]);
  await limitPage.evaluate(() => globalThis.__tilefinchAdvanceReplayClock(1000));
  await limitContext.clock.runFor(1000);
  const limitEvidence = await limitPage.evaluate(
    () => globalThis.__tilefinchReplayClockEvidence(),
  );
  assert.deepEqual(limitEvidence, {
    contract: "dual-domain-ms-call-v2",
    scope: "top-level-realm-v1",
    originMs: limitOrigin,
    hostElapsedMs: 1000,
    playwrightElapsedMs: 1000,
    wallElapsedMs: 1004,
    monotonicElapsedMs: 1001,
    wallObservations: 4,
    monotonicObservations: 1,
    monotonicSamples: 0,
    clockSources: {
      date_now: 2,
      date_function: 1,
      date_constructor: 1,
      performance_now: 1,
      performance_mark: 0,
      performance_measure: 0,
      animation_timeline: 0,
      idle_deadline_time_remaining: 0,
      animation_frame: 0,
      event_timestamp: 0,
      intersection_observer: 0,
      idle_callback_start: 0,
    },
  });
  await limitContext.close();

  const capabilityContext = await browser.newContext({ serviceWorkers: "block" });
  const capabilityReports = [];
  const capabilityRequests = [];
  await capabilityContext.exposeBinding(
    "__tilefinchBlockedCapability",
    (_source, kind, value) => { capabilityReports.push([kind, value]); },
  );
  await capabilityContext.addInitScript(capture.installOfflineCapabilityPolicy, {
    version: "offline-capabilities-v2",
    surfaceVersion: "realm-entrypoints-unavailable-v1",
  });
  capabilityContext.on("request", (request) => {
    capabilityRequests.push([request.resourceType(), request.url()]);
  });
  await capabilityContext.route("https://capability.test/**", (route) => route.fulfill({
    status: 200,
    contentType: "text/html",
    body: [
      "<body><script>",
      "const blob=URL.createObjectURL(new Blob(['postMessage(1)']));",
      "if(typeof Worker==='function')new Worker(blob);",
      "if(typeof SharedWorker==='function')new SharedWorker(blob);",
      "URL.revokeObjectURL(blob);",
      "const child=document.createElement('iframe');document.body.append(child);",
      "globalThis.__childCapability={",
      "worker:typeof child.contentWindow.Worker,",
      "shared:typeof child.contentWindow.SharedWorker,",
      "service:'serviceWorker' in child.contentWindow.navigator,",
      "evidence:child.contentWindow.__tilefinchOfflineCapabilityEvidence};",
      "</script></body>",
    ].join(""),
  }));
  const capabilityPage = await capabilityContext.newPage();
  await capabilityPage.goto("https://capability.test/contract");
  const capability = await capabilityPage.evaluate(() => ({
    constructors: [
      "Worker", "SharedWorker", "ShadowRealm", "AudioWorkletNode", "Worklet",
    ].map((name) => [name, name in globalThis, typeof globalThis[name]]),
    cssWorklets: ["paintWorklet", "layoutWorklet", "animationWorklet"]
      .map((name) => [name, Boolean(globalThis.CSS && name in CSS)]),
    audioWorklet: !globalThis.BaseAudioContext
      || !("audioWorklet" in BaseAudioContext.prototype),
    sharedStorageWorklet: !("sharedStorage" in globalThis)
      && ["SharedStorage", "SharedStorageWorklet", "SharedStorageWorkletGlobalScope"]
        .every((name) => !(name in globalThis)),
    serviceWorker: !("serviceWorker" in navigator),
    serviceWorkerInterfaces: [
      "ServiceWorker", "ServiceWorkerContainer", "ServiceWorkerRegistration",
    ].every((name) => !(name in globalThis)),
    marker: globalThis.__tilefinchOfflineCapabilityPolicy,
    evidence: globalThis.__tilefinchOfflineCapabilityEvidence,
    evidenceFrozen: Object.isFrozen(globalThis.__tilefinchOfflineCapabilityEvidence),
    child: globalThis.__childCapability,
  }));
  assert.deepEqual(capability.constructors, [
    ["Worker", false, "undefined"],
    ["SharedWorker", false, "undefined"],
    ["ShadowRealm", false, "undefined"],
    ["AudioWorkletNode", false, "undefined"],
    ["Worklet", false, "undefined"],
  ]);
  assert.equal(capability.cssWorklets.every((item) => item[1] === false), true);
  assert.equal(capability.audioWorklet, true);
  assert.equal(capability.sharedStorageWorklet, true);
  assert.equal(capability.serviceWorker, true);
  assert.equal(capability.serviceWorkerInterfaces, true);
  assert.equal(capability.marker, "offline-capabilities-v2");
  assert.equal(capability.evidenceFrozen, true);
  assert.equal(capture.offlineCapabilityEvidenceReady(capability.evidence), true);
  assert.deepEqual(capability.child, {
    worker: "undefined", shared: "undefined", service: false,
    evidence: capability.evidence,
  });
  assert.deepEqual(capabilityReports, []);
  assert.deepEqual(capabilityRequests, [["document", "https://capability.test/contract"]]);
  await capabilityContext.close();
  await browser.close();

  /* In quirks mode documentElement.clientHeight can expose the full document
     instead of the layout viewport.  A tall legacy page such as Hacker News
     must remain reference-eligible at the declared device height. */
  const quirksViewport = runHermeticCapture({
    scenario: "quirks-viewport",
    url: "https://quirks-viewport.test/",
    title: "Quirks Viewport",
    marker: "quirks-viewport-marker",
    records: [{
      url: "https://quirks-viewport.test/",
      contentType: "text/html",
      body: [
        "<meta name='viewport' content='width=device-width, initial-scale=1'>",
        "<title>Quirks Viewport</title><body style='margin:0'>",
        "<div style='height:1000px'>quirks-viewport-marker</div></body>",
      ].join(""),
      headers: [["cache-control", "no-store"]],
    }],
  });
  assert.equal(
    quirksViewport.status, 0,
    `${quirksViewport.stderr || quirksViewport.stdout}\n${JSON.stringify(quirksViewport.state, null, 2)}`,
  );
  assert.equal(quirksViewport.state.capture_ready, true);
  assert.deepEqual(quirksViewport.state.viewport.observed, {
    width: 160, height: 120, device_pixel_ratio: 1,
  });

  /* Playwright Chromium used to synthesize a successful 204 for an intercepted
     CORS preflight before BrowserContext.route observed it. Cover the harder
     redirect escape (same-origin request, retained cross-origin redirect),
     malformed Content-Type values, and deprecated client-hint names. None may
     reach Chromium routing or turn a hidden response into qualifying replay. */
  const preflightDocument = [
    "<!doctype html><title>Preflight Pending</title><body>preflight-pending<script>",
    "const fetchProbe=(url,headers)=>fetch(url,{headers}).then(()=>false,()=>true);",
    "const xhrProbe=(url,name,value)=>new Promise(resolve=>{const xhr=new XMLHttpRequest();",
    "try{xhr.open('GET',url);xhr.setRequestHeader(name,value);",
    "xhr.onload=()=>resolve(false);xhr.onerror=()=>resolve(false);xhr.send();}",
    "catch(_error){resolve(true);}});",
    "Promise.all([",
    "fetchProbe('/redirect',{'X-Tilefinch-Probe':'one'}),",
    "fetchProbe('/malformed-fetch',{'Content-Type':'text/plain; charset'}),",
    "fetchProbe('/dpr-fetch',{'DPR':'1'}),",
    "fetchProbe('/downlink-fetch',{'Downlink':'1.5'}),",
    "xhrProbe('/malformed-xhr','Content-Type','text/plain; charset'),",
    "xhrProbe('/dpr-xhr','DPR','1'),xhrProbe('/downlink-xhr','Downlink','1.5')",
    "]).then(results=>{const blocked=results.every(Boolean);",
    "document.title=blocked?'Preflight Blocked':'Preflight Escaped';",
    "document.body.textContent=blocked?'preflight-blocked':'preflight-escaped';});",
    "</script></body>",
  ].join("");
  const preflight = runHermeticCapture({
    scenario: "preflight-boundary",
    url: "https://preflight-origin.test/",
    title: "Preflight",
    marker: "preflight-escaped",
    records: [
      {
        url: "https://preflight-origin.test/",
        contentType: "text/html",
        body: preflightDocument,
        headers: [["cache-control", "no-store"]],
      },
      {
        url: "https://preflight-origin.test/redirect",
        status: 302,
        contentType: "text/plain",
        body: "",
        headers: [
          ["location", "https://redirect-target.test/final"],
          ["cache-control", "no-store"],
        ],
      },
      {
        url: "https://redirect-target.test/final",
        contentType: "text/plain",
        body: "must-not-be-reached-through-a-hidden-preflight",
        headers: [
          ["access-control-allow-origin", "https://preflight-origin.test"],
          ["cache-control", "no-store"],
        ],
      },
    ],
  });
  assert.equal(preflight.status, 3, preflight.stderr || preflight.stdout);
  assert.equal(preflight.state.capture_ready, false);
  assert.equal(preflight.state.title, "Preflight Blocked");
  assert.deepEqual(preflight.state.state_markers, []);
  assert.equal(preflight.state.replay_ledger.requests, 1);
  assert.equal(preflight.state.replay_ledger.served, 1);
  assert.equal(preflight.state.replay_ledger.claimed, 1);
  assert.equal(preflight.state.read_only_policy.denied_before_network, 7);
  assert.equal(preflight.state.read_only_policy.preflight_denied_before_network, 7);
  assert.equal(
    preflight.state.eligibility_reasons.includes("reference-replay-ledger-unhealthy")
      || preflight.state.eligibility_reasons.includes("reference-read-only-policy-unhealthy"),
    true,
  );

  /* Retained occurrences belong to method+URL, not to a resource-type
     substream. The first browser callback below is a script and the second is
     fetch; reversing their distinct responses makes the author marker fail. */
  const sharedUrl = "https://shared-resource.test/value";
  const occurrenceDocument = [
    "<!doctype html><title>Occurrence Contract</title><body>occurrence-pending<script>",
    "let scriptDone=false,fetchDone=false,fetched='';",
    "const finish=()=>{if(scriptDone&&fetchDone)document.body.textContent=",
    "String(globalThis.scriptRecord)+'|'+fetched;};",
    "const startFetch=()=>fetch('", sharedUrl, "').then(response=>response.text()).then(value=>{",
    "fetched=value;fetchDone=true;finish();},()=>{fetched='fetch-error';fetchDone=true;finish();});",
    "const script=document.createElement('script');script.src='", sharedUrl, "';",
    "script.onload=()=>{scriptDone=true;startFetch();};",
    "script.onerror=()=>{scriptDone=true;globalThis.scriptRecord='script-error';startFetch();};",
    "document.head.append(script);",
    "</script></body>",
  ].join("");
  const occurrence = runHermeticCapture({
    scenario: "cross-resource-occurrence",
    url: "https://occurrence-origin.test/",
    title: "Occurrence Contract",
    marker: "script-first|fetch-second",
    records: [
      {
        url: "https://occurrence-origin.test/",
        contentType: "text/html",
        body: occurrenceDocument,
        headers: [["cache-control", "no-store"]],
      },
      {
        url: sharedUrl,
        contentType: "text/javascript",
        body: "globalThis.scriptRecord='script-first';",
        headers: [
          ["access-control-allow-origin", "*"],
          ["cache-control", "no-store"],
        ],
      },
      {
        url: sharedUrl,
        contentType: "text/plain",
        body: "fetch-second",
        headers: [
          ["access-control-allow-origin", "*"],
          ["cache-control", "no-store"],
        ],
      },
    ],
  });
  assert.equal(
    occurrence.status, 0,
    `${occurrence.stderr || occurrence.stdout}\n${JSON.stringify(occurrence.state, null, 2)}`,
  );
  assert.equal(occurrence.state.capture_ready, true);
  assert.deepEqual(occurrence.state.state_markers, ["script-first|fetch-second"]);
  assert.equal(occurrence.state.replay_ledger.occurrence_claims, 2);
  assert.equal(occurrence.state.replay_ledger.served, 3);
  assert.equal(occurrence.state.replay_ledger.claimed, 3);

  /* Reverse lexical arrival of two unmatched GETs must still deliver their
     terminal failures through the same semantic boundary before the matched
     follow-up. Raw Playwright callback evidence remains explicit and exact. */
  const unifiedTerminalDocument = [
    "<!doctype html><title>Unified Pending</title><body>unified-pending<script>",
    "const failures=[];const missing=(url,label)=>fetch(url).catch(()=>{failures.push(label);});",
    "Promise.all([missing('/z-missing','z-missing'),missing('/a-missing','a-missing')])",
    ".then(()=>fetch('/matched')).then(response=>response.text()).then(value=>{",
    "const marker=failures.join(',')+'|'+value;document.title='Unified '+marker;",
    "document.body.textContent=marker;});",
    "</script></body>",
  ].join("");
  const unifiedRecords = [
    {
      url: "https://unified-terminal.test/",
      contentType: "text/html",
      body: unifiedTerminalDocument,
      headers: [["cache-control", "no-store"]],
    },
    {
      url: "https://unified-terminal.test/matched",
      contentType: "text/plain",
      body: "matched-ok",
      headers: [["cache-control", "no-store"]],
    },
  ];
  const runUnified = () => runHermeticCapture({
    scenario: "unified-http-terminal",
    url: "https://unified-terminal.test/",
    title: "Unified",
    marker: "a-missing,z-missing|matched-ok",
    records: unifiedRecords,
  });
  const unifiedA = runUnified();
  const unifiedB = runUnified();
  for (const unified of [unifiedA, unifiedB]) {
    assert.equal(unified.status, 3, unified.stderr || unified.stdout);
    assert.equal(unified.state.capture_ready, false);
    assert.equal(
      unified.state.title.includes("a-missing,z-missing|matched-ok"), true,
    );
    assert.deepEqual(
      unified.state.state_markers, ["a-missing,z-missing|matched-ok"],
    );
    assert.equal(unified.state.replay_ledger.requests, 4);
    assert.equal(unified.state.replay_ledger.scheduled, 4);
    assert.equal(unified.state.replay_ledger.matched, 2);
    assert.equal(unified.state.replay_ledger.served, 2);
    assert.equal(unified.state.replay_ledger.unmatched, 2);
    assert.equal(unified.state.response_scheduler.ready, true);
    assert.equal(unified.state.response_scheduler.retained_admissions, 2);
    assert.equal(unified.state.response_scheduler.terminal_admissions, 2);
    assert.equal(unified.state.response_scheduler.scheduled_delay_work_units, 4);
    assert.equal(unified.state.response_scheduler.pump_work_units, 4);
  }
  for (const field of [
    "semantic_delivery_order_sha256", "raw_callback_arrival_sha256", "order_sha256",
  ]) {
    assert.equal(
      unifiedA.state.response_scheduler[field],
      unifiedB.state.response_scheduler[field],
    );
  }
  process.stdout.write(`${JSON.stringify({
    vector, limitVector, limitEvidence: "monotonic-continues", shape: "native-compatible",
    capability: "realm-entrypoints-unavailable", preflight: "blocked-and-poisoned",
    viewport: "quirks-layout-height", occurrences: "global-first-request-order",
    terminals: "unified-http-delivery",
  })}\n`);
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exitCode = 1;
});
