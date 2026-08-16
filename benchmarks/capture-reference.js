#!/usr/bin/env node
"use strict";

/*
 * Capture a deterministic desktop-browser visual reference from a retained
 * Tilefinch HTTP trace.  Every HTTP(S) request is fulfilled from decoded
 * NNNN.meta/.body records through Playwright routing; an unmatched or
 * conflicting request is aborted and makes the result ineligible.  The
 * browser therefore keeps each logical HTTPS URL and origin without gaining
 * an escape hatch to the live network.
 *
 * Playwright is loaded only for capture mode.  --inspect-trace is dependency
 * free and is used by the fast tooling tests.
 */

const crypto = require("node:crypto");
const fs = require("node:fs");
const net = require("node:net");
const path = require("node:path");
const childProcess = require("node:child_process");

const TRACE_META = /^\d{4}\.meta$/;
const SAFE_HEADER = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
const SAFE_NAME = /^[A-Za-z0-9][A-Za-z0-9_.-]*$/;
const TRACE_DIGEST = /^[0-9a-f]{64}$/;
const MAX_TRACE_RECORDS = 4096;
const MAX_RESPONSE_HEADERS = 512;
const MAX_RESPONSE_COOKIES = 16;
const MAX_SET_COOKIE_BYTES = 4095;
const MAX_COOKIE_EXPIRY_SECONDS = 253402300799;
const MAX_DIAGNOSTIC_URL = 2048;
const MAX_DIAGNOSTIC_ENTRIES = 512;
const MAX_DIAGNOSTIC_COUNTER_KEYS = 64;
const MAX_POLICY_DENIALS = 4096;
const MAX_METADATA_BYTES = 1024 * 1024;
const MAX_MANIFEST_BYTES = 1024 * 1024;
const MAX_RECORD_BYTES = 64 * 1024 * 1024;
const MAX_TRACE_BYTES = 512 * 1024 * 1024;
const MAX_DATE_MS = 8640000000000000;
/* Cross-engine visual replay envelope. Keep visual_scenario.py and the native
   interactive lab CLI validation synchronized with these exact ceilings. */
const MAX_REPLAY_TICKS = 1000;
const MAX_REPLAY_TICK_MS = 60000;
const MAX_REPLAY_HOST_ELAPSED_MS = MAX_REPLAY_TICKS * MAX_REPLAY_TICK_MS;
const MAX_REPLAY_OBSERVATIONS = 1000000;
const MAX_REPLAY_DOMAIN_ELAPSED_MS =
  MAX_REPLAY_HOST_ELAPSED_MS + MAX_REPLAY_OBSERVATIONS;
const REPLAY_ENVIRONMENT_VERSION = "deterministic-hermetic-v3";
const REPLAY_CLOCK_VERSION = "playwright-clock-paused-v2";
const REPLAY_CLOCK_CONTRACT = "dual-domain-ms-call-v2";
const REPLAY_CLOCK_SCOPE = "top-level-realm-v1";
const REPLAY_CLOCK_WALL_SOURCES = Object.freeze([
  "date_now", "date_function", "date_constructor",
]);
const REPLAY_CLOCK_MONOTONIC_OBSERVATION_SOURCES = Object.freeze([
  "performance_now", "performance_mark", "performance_measure",
  "animation_timeline", "idle_deadline_time_remaining",
]);
const REPLAY_CLOCK_MONOTONIC_SAMPLE_SOURCES = Object.freeze([
  "animation_frame", "event_timestamp", "intersection_observer",
  "idle_callback_start",
]);
const REPLAY_CLOCK_SOURCES = Object.freeze([
  "date_now", "date_function", "date_constructor", "performance_now",
  "performance_mark", "performance_measure", "animation_timeline",
  "idle_deadline_time_remaining", "animation_frame", "event_timestamp",
  "intersection_observer", "idle_callback_start",
]);
const REPLAY_RNG_VERSION = "splitmix64-url-scope-v1";
const REPLAY_SEED_SOURCE = "trace-origin-ms-v1";
const REPLAY_INTL_CONTRACT = "bounded-en-us-utc-v1";
const READ_ONLY_POLICY_VERSION = "get-head-only-v3";
const READ_ONLY_PREFLIGHT_POLICY = "cors-preflight-before-network-v1";
const OFFLINE_CAPABILITY_POLICY_VERSION = "offline-capabilities-v2";
const OFFLINE_CAPABILITY_SURFACE_VERSION = "realm-entrypoints-unavailable-v1";
const OFFLINE_CAPABILITY_SURFACE_EVIDENCE = Object.freeze({
  version: OFFLINE_CAPABILITY_SURFACE_VERSION,
  dedicated_worker_constructor: "unavailable",
  shared_worker_constructor: "unavailable",
  shadow_realm_constructor: "unavailable",
  audio_worklet_node_constructor: "unavailable",
  worklet_constructor: "unavailable",
  css_worklet_loaders: "unavailable",
  audio_worklet_loader: "unavailable",
  shared_storage_worklet_loader: "unavailable",
  service_worker_registration: "unavailable",
  service_worker_interfaces: "unavailable",
});
const RESPONSE_SCHEDULER_VERSION = "admission-generations-v7";
const RESPONSE_SCHEDULER_TERMINAL_BOUNDARY = "all-http-terminal-v1";
const MAX_RESPONSE_SCHEDULER_REQUESTS = MAX_TRACE_RECORDS * 4;
const MAX_RETAINED_DELAY_PUMPS = 1000000;
const MAX_RESPONSE_SCHEDULER_PROBES = 64;
const RESPONSE_SCHEDULER_PROBE_TIMEOUT_MS = 2000;
const MAX_RESPONSE_SCHEDULER_PROBE_TIMEOUT_MS = 120000;
const RESPONSE_SCHEDULER_PREPARATION_TIMEOUT_MS = 15000;
const MAX_RESPONSE_SCHEDULER_PREPARATION_TIMEOUT_MS = 120000;
const MAX_RESPONSE_SCHEDULER_DRIVE_STEPS = MAX_RESPONSE_SCHEDULER_REQUESTS * 2;
const MAX_RESPONSE_SCHEDULER_PUMPS = 16_384_000_000;
const MAX_RESPONSE_SCHEDULER_WORK_UNITS = 16_384_000_000;
const DEFAULT_HOST_OPERATION_TIMEOUT_MS = 15000;
const MAX_HOST_OPERATION_TIMEOUT_MS = 120000;
const NORMALIZER_OUTPUT_LIMIT = 1024 * 1024;
const TEXT_METRICS_RUN_LIMIT = 4096;
const TEXT_METRICS_NODE_LIMIT = 8192;
const TEXT_METRICS_TEXT_LIMIT = 1024;
const ROUTE_SELECTION_VERSION = "ranked-occurrence-v2";
const READ_ONLY_METHODS = new Set(["GET", "HEAD"]);
const UINT256_MASK = (1n << 256n) - 1n;
const MANIFEST_FIELDS = [
  "scenario", "url", "replay_dir", "trace_sha256", "expected_http",
  "required_title", "required_state_marker", "fallback_markers",
  "interstitial_markers", "device_width", "device_height", "css_width",
  "css_height", "scale_numerator", "scale_denominator", "checkpoints",
  "reference_state", "limit_mb", "ticks", "tick_ms", "max_download_kb",
  "script_timeout_ms", "script_heap_mb", "script_total_mb", "script_file_kb",
  "script_count", "min_stylesheets_loaded", "min_images_loaded",
  "min_scripts_loaded", "min_network_completions", "max_pending",
  "blocked_origins", "engine_scripts", "hydration_selector",
];
/* Trailing columns that older manifests may omit; absent means "-". */
const OPTIONAL_MANIFEST_FIELDS = 3;
const STRIPPED_HEADERS = new Set([
  "connection", "content-encoding", "content-length", "keep-alive",
  "proxy-authenticate", "proxy-authorization", "proxy-connection", "te",
  /* Repeated Set-Cookie fields cannot be comma-folded.  They are replayed in
     retained order through BrowserContext.addCookies instead. */
  "set-cookie", "set-cookie2", "trailer", "transfer-encoding", "upgrade",
]);

const COOKIE_NAME = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const COOKIE_VALUE = /^[\x21\x23-\x2B\x2D-\x3A\x3C-\x5B\x5D-\x7E]*$/;
const COOKIE_MONTHS = new Map([
  ["jan", 0], ["feb", 1], ["mar", 2], ["apr", 3], ["may", 4], ["jun", 5],
  ["jul", 6], ["aug", 7], ["sep", 8], ["oct", 9], ["nov", 10], ["dec", 11],
]);
const COOKIE_WEEKDAYS = new Set([
  "sun", "sunday", "mon", "monday", "tue", "tues", "tuesday",
  "wed", "wednesday", "thu", "thur", "thurs", "thursday",
  "fri", "friday", "sat", "saturday",
]);

class CaptureError extends Error {}

function parseArguments(argv) {
  const result = {
    browser: "chromium",
    manifest: path.join(__dirname, "visual-scenarios.tsv"),
    timeoutMs: 15000,
    settleMs: 750,
    python: "python3",
  };
  const value = (option, index) => {
    if (index + 1 >= argv.length || argv[index + 1].startsWith("--")) {
      throw new CaptureError(`${option} requires a value`);
    }
    return argv[index + 1];
  };
  for (let i = 0; i < argv.length; i += 1) {
    const option = argv[i];
    if (option === "--help" || option === "-h") result.help = true;
    else if (option === "--inspect-trace") result.inspectTrace = value(option, i++);
    else if (option === "--manifest") result.manifest = value(option, i++);
    else if (option === "--scenario") result.scenario = value(option, i++);
    else if (option === "--trace-root") result.traceRoot = value(option, i++);
    else if (option === "--output-root") result.outputRoot = value(option, i++);
    else if (option === "--text-metrics-output") {
      result.textMetricsOutput = value(option, i++);
    }
    else if (option === "--browser") result.browser = value(option, i++);
    else if (option === "--executable") result.executable = value(option, i++);
    else if (option === "--python") result.python = value(option, i++);
    else if (option === "--timeout-ms") result.timeoutMs = Number(value(option, i++));
    else if (option === "--settle-ms") result.settleMs = Number(value(option, i++));
    else throw new CaptureError(`unknown option ${option}`);
  }
  if (!Number.isInteger(result.timeoutMs) || result.timeoutMs < 1000
      || result.timeoutMs > 120000) {
    throw new CaptureError("--timeout-ms must be an integer in 1000..120000");
  }
  if (!Number.isInteger(result.settleMs) || result.settleMs < 0
      || result.settleMs > 10000) {
    throw new CaptureError("--settle-ms must be an integer in 0..10000");
  }
  return result;
}

function usage() {
  return [
    "usage:",
    "  node benchmarks/capture-reference.js --inspect-trace TRACE_DIR",
    "  node benchmarks/capture-reference.js --scenario NAME --trace-root DIR \\",
    "       --output-root DIR [--manifest FILE] [--browser chromium]",
    "       [--text-metrics-output FILE]",
    "",
    "Capture mode requires the Playwright package and a selected browser binary.",
  ].join("\n");
}

function parseMetadata(filename) {
  let text;
  try {
    if (fs.statSync(filename).size > MAX_METADATA_BYTES) {
      throw new CaptureError(`${filename}: metadata exceeds ${MAX_METADATA_BYTES} bytes`);
    }
    text = fs.readFileSync(filename, "utf8");
  } catch (error) {
    throw new CaptureError(`cannot read metadata ${filename}: ${error.message}`);
  }
  const metadata = Object.create(null);
  for (const [index, line] of text.split(/\r?\n/).entries()) {
    if (!line || line.startsWith("#")) continue;
    const equals = line.indexOf("=");
    if (equals <= 0) {
      throw new CaptureError(`${filename}:${index + 1}: expected key=value`);
    }
    const key = line.slice(0, equals);
    if (Object.prototype.hasOwnProperty.call(metadata, key)) {
      throw new CaptureError(`${filename}:${index + 1}: duplicate key ${key}`);
    }
    metadata[key] = line.slice(equals + 1);
  }
  return metadata;
}

function metadataInteger(metadata, key, filename) {
  if (!Object.prototype.hasOwnProperty.call(metadata, key)
      || !/^-?\d+$/.test(metadata[key])) {
    throw new CaptureError(`${filename}: missing or invalid ${key}`);
  }
  const value = Number(metadata[key]);
  if (!Number.isSafeInteger(value)) {
    throw new CaptureError(`${filename}: ${key} is outside the safe range`);
  }
  return value;
}

function normalizeUrl(value, filename = "URL") {
  let parsed;
  try {
    parsed = new URL(value);
  } catch (_error) {
    throw new CaptureError(`${filename}: invalid absolute URL`);
  }
  if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
    throw new CaptureError(`${filename}: URL must use HTTP(S)`);
  }
  parsed.hash = "";
  return parsed.href;
}

function replayEnvironment(trace) {
  if (!trace || !Number.isSafeInteger(trace.originMs)
      || trace.originMs <= 0 || trace.originMs > MAX_DATE_MS) {
    throw new CaptureError("deterministic replay requires retained trace origin-ms metadata");
  }
  const seed = crypto.createHash("sha256")
    .update(`${REPLAY_RNG_VERSION}\0${REPLAY_SEED_SOURCE}\0${trace.originMs}`)
    .digest();
  return {
    version: REPLAY_ENVIRONMENT_VERSION,
    clockVersion: REPLAY_CLOCK_VERSION,
    clockContract: REPLAY_CLOCK_CONTRACT,
    clockScope: REPLAY_CLOCK_SCOPE,
    rngVersion: REPLAY_RNG_VERSION,
    seedSource: REPLAY_SEED_SOURCE,
    intlContract: REPLAY_INTL_CONTRACT,
    originMs: trace.originMs,
    seedSha256: seed.toString("hex"),
    seedU64: String(trace.originMs),
  };
}

/* This function is serialized into every Chromium realm by addInitScript, so
   it deliberately has no closure dependencies. Playwright Clock owns timer
   delivery and the paused base clock. This layer implements the shared native/
   reference dual-domain-ms-call-v2 contract and entropy stream while
   preserving the original API descriptors and native-shaped callables. */
function installReplayEnvironment(configuration) {
  const MAX_WALL_CLOCK_MS = 8640000000000000;
  if (configuration.intlContract !== "bounded-en-us-utc-v1") {
    throw new Error("deterministic replay requires the bounded Intl contract");
  }
  if (configuration.clockContract !== "dual-domain-ms-call-v2"
      || configuration.clockScope !== "top-level-realm-v1") {
    throw new Error("deterministic replay requires the dual-domain clock contract");
  }
  const MASK = (1n << 64n) - 1n;
  const descriptorOwner = (object, property) => {
    for (let current = object; current; current = Object.getPrototypeOf(current)) {
      if (Object.prototype.hasOwnProperty.call(current, property)) return current;
    }
    return null;
  };
  const replaceValue = (object, property, value) => {
    const owner = descriptorOwner(object, property);
    if (!owner) return false;
    const descriptor = Object.getOwnPropertyDescriptor(owner, property);
    if (!descriptor || !("value" in descriptor)) return false;
    Object.defineProperty(owner, property, { ...descriptor, value });
    return true;
  };
  const replaceGetter = (object, property, get) => {
    const owner = descriptorOwner(object, property);
    if (!owner) return false;
    const descriptor = Object.getOwnPropertyDescriptor(owner, property);
    if (!descriptor || typeof descriptor.get !== "function") return false;
    Object.defineProperty(owner, property, { ...descriptor, get });
    return true;
  };

  const NativeDate = globalThis.Date;
  const playwrightDateNow = NativeDate.now;
  const nativeDateToString = NativeDate.prototype.toString;
  const originMs = Number(configuration.originMs);
  if (!Number.isSafeInteger(originMs) || originMs <= 0 || originMs > MAX_WALL_CLOCK_MS) {
    throw new Error("deterministic replay origin exceeds the ECMAScript Date domain");
  }
  let hostElapsedMs = 0;
  let wallElapsedMs = 0;
  let monotonicElapsedMs = 0;
  let wallObservations = 0;
  let monotonicObservations = 0;
  let monotonicSamples = 0;
  const clockSources = {
    date_now: 0,
    date_function: 0,
    date_constructor: 0,
    performance_now: 0,
    performance_mark: 0,
    performance_measure: 0,
    animation_timeline: 0,
    idle_deadline_time_remaining: 0,
    animation_frame: 0,
    event_timestamp: 0,
    intersection_observer: 0,
    idle_callback_start: 0,
  };
  let playwrightMonotonicNow = null;
  let playwrightMonotonicOrigin = 0;
  const requireClockSource = (source) => {
    if (!Object.prototype.hasOwnProperty.call(clockSources, source)) {
      throw new Error("deterministic replay clock observation source is invalid");
    }
  };
  const observeMonotonic = (source) => {
    requireClockSource(source);
    const value = monotonicElapsedMs;
    monotonicElapsedMs += 1;
    monotonicObservations += 1;
    clockSources[source] += 1;
    return value;
  };
  const sampleMonotonic = (source) => {
    requireClockSource(source);
    monotonicSamples += 1;
    clockSources[source] += 1;
    return monotonicElapsedMs;
  };
  const observeWallClock = (source) => {
    requireClockSource(source);
    const value = wallElapsedMs >= MAX_WALL_CLOCK_MS - originMs
      ? MAX_WALL_CLOCK_MS : originMs + wallElapsedMs;
    wallElapsedMs += 1;
    wallObservations += 1;
    clockSources[source] += 1;
    return value;
  };

  const nativeNowDescriptor = Object.getOwnPropertyDescriptor(NativeDate, "now");
  if (!nativeNowDescriptor || typeof nativeNowDescriptor.value !== "function") {
    throw new Error("deterministic replay requires Date.now");
  }
  Object.defineProperty(NativeDate, "now", {
    ...nativeNowDescriptor,
    value: new Proxy(nativeNowDescriptor.value, {
      apply() { return observeWallClock("date_now"); },
    }),
  });
  const DeterministicDate = new Proxy(NativeDate, {
    apply() {
      const value = Reflect.construct(
        NativeDate, [observeWallClock("date_function")],
      );
      return Reflect.apply(nativeDateToString, value, []);
    },
    construct(target, argumentsList, newTarget) {
      const values = argumentsList.length === 0
        ? [observeWallClock("date_constructor")] : argumentsList;
      return Reflect.construct(target, values, newTarget);
    },
  });
  const globalDateDescriptor = Object.getOwnPropertyDescriptor(globalThis, "Date");
  if (!globalDateDescriptor || !("value" in globalDateDescriptor)) {
    throw new Error("deterministic replay requires the global Date constructor");
  }
  Object.defineProperty(globalThis, "Date", {
    ...globalDateDescriptor, value: DeterministicDate,
  });
  const dateConstructorDescriptor = Object.getOwnPropertyDescriptor(
    NativeDate.prototype, "constructor",
  );
  if (!dateConstructorDescriptor || !("value" in dateConstructorDescriptor)) {
    throw new Error("deterministic replay requires Date.prototype.constructor");
  }
  Object.defineProperty(NativeDate.prototype, "constructor", {
    ...dateConstructorDescriptor, value: DeterministicDate,
  });

  if (globalThis.Performance && Performance.prototype) {
    const nowOwner = descriptorOwner(performance, "now");
    const nowDescriptor = nowOwner && Object.getOwnPropertyDescriptor(nowOwner, "now");
    if (!nowDescriptor || typeof nowDescriptor.value !== "function") {
      throw new Error("deterministic replay requires performance.now");
    }
    playwrightMonotonicNow = () => Reflect.apply(nowDescriptor.value, performance, []);
    playwrightMonotonicOrigin = playwrightMonotonicNow();
    Object.defineProperty(nowOwner, "now", {
      ...nowDescriptor,
      value: new Proxy(nowDescriptor.value, {
        apply(target, thisArgument) {
          /* Preserve native illegal-invocation behavior before returning the
             deterministic value. */
          Reflect.apply(target, thisArgument, []);
          return observeMonotonic("performance_now");
        },
      }),
    });
    const timeOriginOwner = descriptorOwner(performance, "timeOrigin");
    const timeOriginDescriptor = timeOriginOwner
      && Object.getOwnPropertyDescriptor(timeOriginOwner, "timeOrigin");
    if (timeOriginDescriptor && typeof timeOriginDescriptor.get === "function") {
      Object.defineProperty(timeOriginOwner, "timeOrigin", {
        ...timeOriginDescriptor,
        get: new Proxy(timeOriginDescriptor.get, {
          apply(target, thisArgument) {
            Reflect.apply(target, thisArgument, []);
            return originMs;
          },
        }),
      });
    }
    for (const name of ["getEntries", "getEntriesByType", "getEntriesByName"]) {
      const owner = descriptorOwner(performance, name);
      const descriptor = owner && Object.getOwnPropertyDescriptor(owner, name);
      if (!descriptor || typeof descriptor.value !== "function") continue;
      Object.defineProperty(owner, name, {
        ...descriptor,
        value: new Proxy(descriptor.value, {
          apply(target, thisArgument, argumentsList) {
            Reflect.apply(target, thisArgument, argumentsList);
            return [];
          },
        }),
      });
    }
    /* Do not merely hide Chromium's timeline: mark()/measure() would continue
       growing its host store behind the empty getters.  Mirror the native
       128-entry store (one protected navigation slot plus oldest-non-navigation
       eviction) and keep all public discovery normalized empty. */
    class ReplayPerformanceEntry {
      constructor(name, entryType, startTime = 0, duration = 0) {
        this.name = String(name);
        this.entryType = String(entryType);
        this.startTime = Number(startTime);
        this.duration = Number(duration);
      }
      toJSON() {
        return {
          name: this.name, entryType: this.entryType,
          startTime: this.startTime, duration: this.duration,
        };
      }
    }
    class ReplayPerformanceResourceTiming extends ReplayPerformanceEntry {
      constructor(name, initiatorType = "other") {
        super(name, "resource", 0, 0);
        this.initiatorType = String(initiatorType);
        this.nextHopProtocol = "h2";
        this.transferSize = 0;
        this.encodedBodySize = 0;
        this.decodedBodySize = 0;
      }
    }
    class ReplayPerformanceNavigationTiming extends ReplayPerformanceResourceTiming {
      constructor(name) {
        super(name, "navigation");
        this.entryType = "navigation";
        this.type = "navigate";
        this.redirectCount = 0;
        this.domInteractive = 0;
        this.domContentLoadedEventStart = 0;
        this.domContentLoadedEventEnd = 0;
        this.loadEventStart = 0;
        this.loadEventEnd = 0;
      }
    }
    const performanceClassFacade = (name, Constructor) => {
      for (const property of Reflect.ownKeys(Constructor.prototype)) {
        if (property === "constructor") continue;
        const descriptor = Object.getOwnPropertyDescriptor(Constructor.prototype, property);
        if (typeof descriptor.value === "function") descriptor.value = new Proxy(descriptor.value, {});
        Object.defineProperty(Constructor.prototype, property, descriptor);
      }
      Object.defineProperty(Constructor, "name", { value: name, configurable: true });
      const facade = new Proxy(Constructor, {});
      const descriptor = Object.getOwnPropertyDescriptor(Constructor.prototype, "constructor");
      Object.defineProperty(Constructor.prototype, "constructor", {
        ...descriptor, value: facade,
      });
      const globalDescriptor = Object.getOwnPropertyDescriptor(globalThis, name);
      Object.defineProperty(globalThis, name, globalDescriptor && "value" in globalDescriptor
        ? { ...globalDescriptor, value: facade }
        : { value: facade, writable: true, enumerable: false, configurable: true });
      return facade;
    };
    performanceClassFacade("PerformanceEntry", ReplayPerformanceEntry);
    performanceClassFacade("PerformanceResourceTiming", ReplayPerformanceResourceTiming);
    performanceClassFacade("PerformanceNavigationTiming", ReplayPerformanceNavigationTiming);
    const replayPerformanceEntries = [new ReplayPerformanceNavigationTiming(
      String(globalThis.location && location.href || "about:blank"),
    )];
    const createPerformanceEntry = (name, entryType, startTime, duration, detail) => {
      const entry = new ReplayPerformanceEntry(name, entryType, startTime, duration);
      entry.detail = detail;
      return entry;
    };
    const appendPerformanceEntry = (entry) => {
      if (replayPerformanceEntries.length >= 128) {
        const index = replayPerformanceEntries.findIndex(
          (value) => value.entryType !== "navigation",
        );
        if (index < 0) return entry;
        replayPerformanceEntries.splice(index, 1);
      }
      replayPerformanceEntries.push(entry);
      return entry;
    };
    const performanceMarkTime = (name) => {
      const expected = String(name);
      for (let index = replayPerformanceEntries.length - 1; index >= 0; index -= 1) {
        const entry = replayPerformanceEntries[index];
        if (entry.entryType === "mark" && entry.name === expected) return entry.startTime;
      }
      throw new DOMException(`The mark ${expected} does not exist`, "SyntaxError");
    };
    const performanceTimestamp = (value) => typeof value === "string"
      ? performanceMarkTime(value) : Number(value);
    const replacePerformanceMethod = (name, implementation) => {
      const owner = descriptorOwner(performance, name);
      const descriptor = owner && Object.getOwnPropertyDescriptor(owner, name);
      if (!descriptor || typeof descriptor.value !== "function") return;
      Object.defineProperty(owner, name, {
        ...descriptor,
        value: new Proxy(descriptor.value, {
          apply(target, thisArgument, argumentsList) {
            if (thisArgument !== performance) {
              return Reflect.apply(target, thisArgument, argumentsList);
            }
            return implementation(argumentsList);
          },
        }),
      });
    };
    replacePerformanceMethod("mark", (argumentsList) => {
      const name = String(argumentsList[0]);
      const options = argumentsList.length < 2 || argumentsList[1] === undefined
        ? {} : argumentsList[1];
      const start = options.startTime === undefined
        ? observeMonotonic("performance_mark") : Number(options.startTime);
      if (!Number.isFinite(start) || start < 0) {
        throw new TypeError("startTime must be a finite nonnegative number");
      }
      return appendPerformanceEntry(
        createPerformanceEntry(name, "mark", start, 0, options.detail),
      );
    });
    replacePerformanceMethod("measure", (argumentsList) => {
      const name = String(argumentsList[0]);
      const startOrOptions = argumentsList[1];
      const endMark = argumentsList[2];
      let start = 0;
      let end;
      let detail;
      if (typeof startOrOptions === "object" && startOrOptions !== null) {
        const hasStart = startOrOptions.start !== undefined;
        const hasEnd = startOrOptions.end !== undefined;
        const hasDuration = startOrOptions.duration !== undefined;
        if ((hasStart && hasEnd && hasDuration) || (!hasStart && !hasEnd)) {
          throw new TypeError("Invalid measure options");
        }
        start = hasStart ? performanceTimestamp(startOrOptions.start) : NaN;
        end = hasEnd ? performanceTimestamp(startOrOptions.end) : NaN;
        const duration = hasDuration ? Number(startOrOptions.duration) : NaN;
        if (hasDuration && !Number.isFinite(duration)) {
          throw new TypeError("duration must be finite");
        }
        if (!hasStart) start = end - duration;
        else if (!hasEnd) end = hasDuration
          ? start + duration : observeMonotonic("performance_measure");
        detail = startOrOptions.detail;
      } else {
        if (startOrOptions !== undefined) start = performanceTimestamp(startOrOptions);
        end = endMark !== undefined ? performanceTimestamp(endMark)
          : observeMonotonic("performance_measure");
      }
      if (!Number.isFinite(start) || !Number.isFinite(end)) {
        throw new TypeError("measure timestamps must be finite");
      }
      return appendPerformanceEntry(
        createPerformanceEntry(name, "measure", start, Math.max(0, end - start), detail),
      );
    });
    replacePerformanceMethod("clearMarks", (argumentsList) => {
      const name = argumentsList[0];
      for (let index = replayPerformanceEntries.length - 1; index >= 0; index -= 1) {
        const entry = replayPerformanceEntries[index];
        if (entry.entryType === "mark" && (name === undefined || entry.name === String(name))) {
          replayPerformanceEntries.splice(index, 1);
        }
      }
    });
    replacePerformanceMethod("clearMeasures", (argumentsList) => {
      const name = argumentsList[0];
      for (let index = replayPerformanceEntries.length - 1; index >= 0; index -= 1) {
        const entry = replayPerformanceEntries[index];
        if (entry.entryType === "measure" && (name === undefined || entry.name === String(name))) {
          replayPerformanceEntries.splice(index, 1);
        }
      }
    });
    replacePerformanceMethod("clearResourceTimings", () => {
      for (let index = replayPerformanceEntries.length - 1; index >= 0; index -= 1) {
        if (replayPerformanceEntries[index].entryType === "resource") {
          replayPerformanceEntries.splice(index, 1);
        }
      }
    });
    replacePerformanceMethod("setResourceTimingBufferSize", () => undefined);
  }

  const utf8 = (value) => new TextEncoder().encode(String(value));
  const feed = (state, byte) => ((state ^ BigInt(byte)) * 0x100000001b3n) & MASK;
  const feedU64LE = (state, value) => {
    let result = state;
    for (let index = 0; index < 8; index += 1) {
      result = feed(result, Number((value >> BigInt(index * 8)) & 255n));
    }
    return result;
  };
  const avalanche = (input) => {
    let value = input & MASK;
    value = ((value ^ (value >> 30n)) * 0xbf58476d1ce4e5b9n) & MASK;
    value = ((value ^ (value >> 27n)) * 0x94d049bb133111ebn) & MASK;
    return (value ^ (value >> 31n)) & MASK;
  };
  const serializedUrl = String(globalThis.location && location.href || "about:blank");
  const urlBytes = utf8(serializedUrl);
  let framed = 0xcbf29ce484222325n;
  for (const byte of utf8(configuration.rngVersion)) framed = feed(framed, byte);
  framed = feed(framed, 0);
  framed = feedU64LE(framed, BigInt(configuration.seedU64));
  framed = feed(framed, globalThis.top === globalThis ? 0 : 1);
  framed = feedU64LE(framed, BigInt(urlBytes.length));
  for (const byte of urlBytes) framed = feed(framed, byte);
  let entropyState = avalanche(framed);
  if (entropyState === 0n) entropyState = 1n;
  const nextEntropy = () => {
    entropyState = (entropyState + 0x9e3779b97f4a7c15n) & MASK;
    return avalanche(entropyState);
  }
  const originalRandom = Math.random;
  replaceValue(Math, "random", new Proxy(originalRandom, {
    apply() { return Number(nextEntropy() >> 11n) / 9007199254740992; },
  }));

  if (globalThis.Crypto) {
    const integerTags = new Set([
      "[object Int8Array]", "[object Uint8Array]", "[object Uint8ClampedArray]",
      "[object Int16Array]", "[object Uint16Array]",
      "[object Int32Array]", "[object Uint32Array]",
      "[object BigInt64Array]", "[object BigUint64Array]",
    ]);
    const fillRandomValues = (array) => {
      if (!ArrayBuffer.isView(array)
          || !integerTags.has(Object.prototype.toString.call(array))) {
        throw new DOMException(
          "The provided ArrayBufferView is not an integer array", "TypeMismatchError",
        );
      }
      if (array.byteLength > 65536) {
        throw new DOMException(
          "The requested length exceeds 65,536 bytes", "QuotaExceededError",
        );
      }
      const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
      for (let offset = 0; offset < bytes.length;) {
        const value = nextEntropy();
        for (let byte = 0; byte < 8 && offset < bytes.length; byte += 1) {
          bytes[offset++] = Number((value >> BigInt(byte * 8)) & 255n);
        }
      }
      return array;
    };
    const randomValuesOwner = descriptorOwner(globalThis.crypto, "getRandomValues");
    const randomValuesDescriptor = randomValuesOwner
      && Object.getOwnPropertyDescriptor(randomValuesOwner, "getRandomValues");
    if (!randomValuesDescriptor || typeof randomValuesDescriptor.value !== "function") {
      throw new Error("deterministic replay requires Crypto.getRandomValues");
    }
    Object.defineProperty(randomValuesOwner, "getRandomValues", {
      ...randomValuesDescriptor,
      value: new Proxy(randomValuesDescriptor.value, {
        apply(target, thisArgument, argumentsList) {
          Reflect.apply(target, thisArgument, [new Uint8Array(0)]);
          return fillRandomValues(argumentsList[0]);
        },
      }),
    });
    if (typeof globalThis.crypto.randomUUID === "function") {
      const uuidOwner = descriptorOwner(globalThis.crypto, "randomUUID");
      const uuidDescriptor = uuidOwner
        && Object.getOwnPropertyDescriptor(uuidOwner, "randomUUID");
      Object.defineProperty(uuidOwner, "randomUUID", {
        ...uuidDescriptor,
        value: new Proxy(uuidDescriptor.value, {
          apply(target, thisArgument, argumentsList) {
            Reflect.apply(randomValuesDescriptor.value, thisArgument, [new Uint8Array(0)]);
          const bytes = fillRandomValues(new Uint8Array(16));
          bytes[6] = (bytes[6] & 15) | 64;
          bytes[8] = (bytes[8] & 63) | 128;
          const hex = Array.from(bytes, (item) => item.toString(16).padStart(2, "0")).join("");
          return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-` +
            `${hex.slice(16, 20)}-${hex.slice(20)}`;
          },
        }),
      });
    }
  }

  if (globalThis.AnimationTimeline) {
    const descriptor = Object.getOwnPropertyDescriptor(
      AnimationTimeline.prototype, "currentTime",
    );
    if (descriptor && typeof descriptor.get === "function") {
      Object.defineProperty(AnimationTimeline.prototype, "currentTime", {
        ...descriptor,
        get: new Proxy(descriptor.get, {
          apply(target, thisArgument) {
            Reflect.apply(target, thisArgument, []);
            return observeMonotonic("animation_timeline");
          },
        }),
      });
    }
  }
  if (typeof globalThis.requestAnimationFrame === "function") {
    const original = globalThis.requestAnimationFrame;
    let lastHostFrameTimestamp = NaN;
    let lastReplayFrameTimestamp = 0;
    replaceValue(globalThis, "requestAnimationFrame", new Proxy(original, {
      apply(target, thisArgument, argumentsList) {
        const callback = argumentsList[0];
        if (typeof callback !== "function") {
          return Reflect.apply(target, thisArgument, argumentsList);
        }
        const deterministicCallback = new Proxy(callback, {
          apply(callbackTarget, callbackThis, callbackArguments) {
            const hostTimestamp = Number(callbackArguments[0]);
            if (!Object.is(hostTimestamp, lastHostFrameTimestamp)) {
              lastHostFrameTimestamp = hostTimestamp;
              lastReplayFrameTimestamp = sampleMonotonic("animation_frame");
            }
            return Reflect.apply(
              callbackTarget, callbackThis,
              [lastReplayFrameTimestamp],
            );
          },
        });
        return Reflect.apply(target, thisArgument, [deterministicCallback]);
      },
    }));
  }

  /* Stable platform timestamp samples never advance the monotonic domain.
     A WeakMap preserves one Event.timeStamp value per native event, including
     browser-created events which do not pass through the global constructor. */
  if (globalThis.Event && Event.prototype) {
    const NativeEvent = globalThis.Event;
    const eventTimestamps = new WeakMap();
    const seedEventTimestamp = (event) => {
      if (event && typeof event === "object" && !eventTimestamps.has(event)) {
        eventTimestamps.set(event, sampleMonotonic("event_timestamp"));
      }
      return event;
    };
    const owner = descriptorOwner(Event.prototype, "timeStamp");
    const descriptor = owner && Object.getOwnPropertyDescriptor(owner, "timeStamp");
    if (!descriptor || typeof descriptor.get !== "function") {
      throw new Error("deterministic replay requires Event.timeStamp");
    }
    Object.defineProperty(owner, "timeStamp", {
      ...descriptor,
      get: new Proxy(descriptor.get, {
        apply(target, thisArgument) {
          /* Preserve the native receiver check. */
          Reflect.apply(target, thisArgument, []);
          seedEventTimestamp(thisArgument);
          return eventTimestamps.get(thisArgument);
        },
      }),
    });
    /* QuickJS samples in the base Event constructor. Chromium's platform
       subclasses do not call a replaceable JavaScript Event binding, so wrap
       every exposed Event constructor and seed exactly once after native
       construction. This also covers author-created events never read or
       dispatched before evidence collection. */
    const eventConstructors = [];
    for (const name of Object.getOwnPropertyNames(globalThis)) {
      const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
      const Constructor = descriptor && "value" in descriptor
        ? descriptor.value : null;
      if (typeof Constructor !== "function" || !Constructor.prototype) continue;
      let eventPrototype = false;
      try {
        eventPrototype = Constructor === NativeEvent
          || Constructor.prototype instanceof NativeEvent;
      } catch (_error) {}
      if (eventPrototype && descriptor.configurable) {
        eventConstructors.push([name, descriptor, Constructor]);
      }
    }
    for (const [name, descriptor, Constructor] of eventConstructors) {
      const facade = new Proxy(Constructor, {
        construct(target, argumentsList, newTarget) {
          return seedEventTimestamp(
            Reflect.construct(target, argumentsList, newTarget),
          );
        },
      });
      Object.defineProperty(globalThis, name, { ...descriptor, value: facade });
      const constructorDescriptor = Object.getOwnPropertyDescriptor(
        Constructor.prototype, "constructor",
      );
      if (constructorDescriptor && "value" in constructorDescriptor
          && constructorDescriptor.configurable) {
        Object.defineProperty(Constructor.prototype, "constructor", {
          ...constructorDescriptor, value: facade,
        });
      }
    }
    if (globalThis.EventTarget && EventTarget.prototype) {
      const dispatchOwner = descriptorOwner(EventTarget.prototype, "dispatchEvent");
      const dispatchDescriptor = dispatchOwner
        && Object.getOwnPropertyDescriptor(dispatchOwner, "dispatchEvent");
      if (!dispatchDescriptor || typeof dispatchDescriptor.value !== "function") {
        throw new Error("deterministic replay requires EventTarget.dispatchEvent");
      }
      Object.defineProperty(dispatchOwner, "dispatchEvent", {
        ...dispatchDescriptor,
        value: new Proxy(dispatchDescriptor.value, {
          apply(target, thisArgument, argumentsList) {
            seedEventTimestamp(argumentsList[0]);
            return Reflect.apply(target, thisArgument, argumentsList);
          },
        }),
      });
    }
    /* Browser-created events enter the page at dispatch rather than through an
       author-visible constructor. Seed common platform dispatches in the first
       capture listener; the getter remains a truthful fallback for uncommon
       event types and entries obtained from browser APIs without dispatch. */
    if (typeof globalThis.addEventListener === "function") {
      const seedDispatchedEvent = (event) => { seedEventTimestamp(event); };
      for (const type of [
        "abort", "beforeinput", "blur", "change", "click", "compositionend",
        "compositionstart", "compositionupdate", "DOMContentLoaded", "error",
        "focus", "focusin", "focusout", "hashchange", "input", "keydown",
        "keypress", "keyup", "load", "message", "mousedown", "mousemove",
        "mouseup", "offline", "online", "pagehide", "pageshow", "pointercancel",
        "pointerdown", "pointermove", "pointerup", "popstate", "resize", "scroll",
        "submit", "touchcancel", "touchend", "touchmove", "touchstart", "unload",
        "visibilitychange", "wheel",
      ]) {
        globalThis.addEventListener(type, seedDispatchedEvent, {
          capture: true, passive: true,
        });
      }
    }
  }

  /* Every delivered IntersectionObserver callback is one batch and therefore
     owns one stable timestamp shared by all of its entries. */
  if (typeof globalThis.IntersectionObserver === "function"
      && globalThis.IntersectionObserverEntry
      && IntersectionObserverEntry.prototype) {
    const OriginalIntersectionObserver = globalThis.IntersectionObserver;
    const entryTimestamps = new WeakMap();
    const timeOwner = descriptorOwner(IntersectionObserverEntry.prototype, "time");
    const timeDescriptor = timeOwner
      && Object.getOwnPropertyDescriptor(timeOwner, "time");
    if (!timeDescriptor || typeof timeDescriptor.get !== "function") {
      throw new Error("deterministic replay requires IntersectionObserverEntry.time");
    }
    Object.defineProperty(timeOwner, "time", {
      ...timeDescriptor,
      get: new Proxy(timeDescriptor.get, {
        apply(target, thisArgument) {
          Reflect.apply(target, thisArgument, []);
          if (!entryTimestamps.has(thisArgument)) {
            entryTimestamps.set(
              thisArgument, sampleMonotonic("intersection_observer"),
            );
          }
          return entryTimestamps.get(thisArgument);
        },
      }),
    });
    const globalDescriptor = Object.getOwnPropertyDescriptor(
      globalThis, "IntersectionObserver",
    );
    const DeterministicIntersectionObserver = new Proxy(
      OriginalIntersectionObserver, {
        construct(target, argumentsList, newTarget) {
          const callback = argumentsList[0];
          if (typeof callback !== "function") {
            return Reflect.construct(target, argumentsList, newTarget);
          }
          const wrapped = new Proxy(callback, {
            apply(callbackTarget, callbackThis, callbackArguments) {
              const entries = callbackArguments[0];
              const timestamp = sampleMonotonic("intersection_observer");
              /* Pages may invoke their callback directly with arbitrary
                 arguments (not an entry list); only genuine entry objects
                 can key the timestamp map. */
              if (Array.isArray(entries)) {
                for (const entry of entries) {
                  if (entry && typeof entry === "object") {
                    entryTimestamps.set(entry, timestamp);
                  }
                }
              }
              return Reflect.apply(
                callbackTarget, callbackThis, callbackArguments,
              );
            },
          });
          return Reflect.construct(
            target, [wrapped, ...argumentsList.slice(1)], newTarget,
          );
        },
      },
    );
    Object.defineProperty(globalThis, "IntersectionObserver", {
      ...globalDescriptor,
      value: DeterministicIntersectionObserver,
    });
    const observerConstructorDescriptor = Object.getOwnPropertyDescriptor(
      OriginalIntersectionObserver.prototype, "constructor",
    );
    if (observerConstructorDescriptor && "value" in observerConstructorDescriptor) {
      Object.defineProperty(OriginalIntersectionObserver.prototype, "constructor", {
        ...observerConstructorDescriptor, value: DeterministicIntersectionObserver,
      });
    }
  }

  if (typeof globalThis.requestIdleCallback === "function"
      && globalThis.IdleDeadline && IdleDeadline.prototype) {
    const idleStarts = new WeakMap();
    const originalRequestIdleCallback = globalThis.requestIdleCallback;
    const remainingOwner = descriptorOwner(IdleDeadline.prototype, "timeRemaining");
    const remainingDescriptor = remainingOwner
      && Object.getOwnPropertyDescriptor(remainingOwner, "timeRemaining");
    if (!remainingDescriptor || typeof remainingDescriptor.value !== "function") {
      throw new Error("deterministic replay requires IdleDeadline.timeRemaining");
    }
    Object.defineProperty(remainingOwner, "timeRemaining", {
      ...remainingDescriptor,
      value: new Proxy(remainingDescriptor.value, {
        apply(target, thisArgument) {
          Reflect.apply(target, thisArgument, []);
          if (!idleStarts.has(thisArgument)) {
            idleStarts.set(
              thisArgument, sampleMonotonic("idle_callback_start"),
            );
          }
          return Math.max(0, 8 - (
            observeMonotonic("idle_deadline_time_remaining")
              - idleStarts.get(thisArgument)
          ));
        },
      }),
    });
    replaceValue(globalThis, "requestIdleCallback", new Proxy(
      originalRequestIdleCallback, {
        apply(target, thisArgument, argumentsList) {
          const callback = argumentsList[0];
          if (typeof callback !== "function") {
            return Reflect.apply(target, thisArgument, argumentsList);
          }
          const wrapped = new Proxy(callback, {
            apply(callbackTarget, callbackThis, callbackArguments) {
              const deadline = callbackArguments[0];
              if (!idleStarts.has(deadline)) {
                idleStarts.set(
                  deadline, sampleMonotonic("idle_callback_start"),
                );
              }
              return Reflect.apply(
                callbackTarget, callbackThis, callbackArguments,
              );
            },
          });
          return Reflect.apply(
            target, thisArgument, [wrapped, ...argumentsList.slice(1)],
          );
        },
      },
    ));
  }

  /* QuickJS enters deterministic replay through the browser's deliberately
     bounded Intl compatibility layer.  Installing only deterministic default
     dates over Chromium's host ICU would make the two engines observably
     different (and would allow host ICU/CLDR revisions to perturb captures).
     Keep this implementation in behavioral lockstep with
     browser_bootstrap_compat and browser_deterministic_replay_surface_bootstrap.
     Function proxies preserve browser-native source shape while the bounded
     classes own the deterministic semantics. */
  const localeTag = (value) => String(value === undefined ? "en-US" : value)
    .replace(/_/g, "-");
  const supportedLocales = (locales) => (Array.isArray(locales) ? locales : [locales])
    .filter((value) => value !== undefined).map(localeTag);
  const localeOf = (locales) => localeTag(Array.isArray(locales) ? locales[0] : locales);
  const nativeShape = (value) => new Proxy(value, {});
  const shapePrototype = (prototype) => {
    for (const property of Reflect.ownKeys(prototype)) {
      if (property === "constructor") continue;
      const descriptor = Object.getOwnPropertyDescriptor(prototype, property);
      if (!descriptor) continue;
      if (typeof descriptor.value === "function") {
        descriptor.value = nativeShape(descriptor.value);
      }
      if (typeof descriptor.get === "function") descriptor.get = nativeShape(descriptor.get);
      if (typeof descriptor.set === "function") descriptor.set = nativeShape(descriptor.set);
      Object.defineProperty(prototype, property, descriptor);
    }
  };
  const callable = (name, Constructor) => {
    shapePrototype(Constructor.prototype);
    function boundedConstructor(...argumentsList) {
      return Reflect.construct(Constructor, argumentsList);
    }
    Object.defineProperty(boundedConstructor, "name", { value: name, configurable: true });
    Object.defineProperty(boundedConstructor, "length", { value: 0, configurable: true });
    Object.defineProperty(boundedConstructor, "prototype", {
      value: Constructor.prototype, writable: false, enumerable: false, configurable: false,
    });
    const facade = new Proxy(boundedConstructor, {
      apply(_target, _thisArgument, argumentsList) {
        return Reflect.construct(Constructor, argumentsList);
      },
      construct(_target, argumentsList) {
        return Reflect.construct(Constructor, argumentsList);
      },
    });
    const constructorDescriptor = Object.getOwnPropertyDescriptor(
      Constructor.prototype, "constructor",
    );
    Object.defineProperty(Constructor.prototype, "constructor", {
      ...constructorDescriptor, value: facade,
    });
    if (typeof Constructor.supportedLocalesOf === "function") {
      Object.defineProperty(facade, "supportedLocalesOf", {
        value: nativeShape(Constructor.supportedLocalesOf),
        writable: true, enumerable: false, configurable: true,
      });
    }
    return facade;
  };
  const constructOnly = (name, Constructor) => {
    shapePrototype(Constructor.prototype);
    Object.defineProperty(Constructor, "name", { value: name, configurable: true });
    if (typeof Constructor.supportedLocalesOf === "function") {
      const descriptor = Object.getOwnPropertyDescriptor(Constructor, "supportedLocalesOf");
      Object.defineProperty(Constructor, "supportedLocalesOf", {
        ...descriptor, value: nativeShape(descriptor.value),
      });
    }
    const facade = new Proxy(Constructor, {});
    const constructorDescriptor = Object.getOwnPropertyDescriptor(
      Constructor.prototype, "constructor",
    );
    Object.defineProperty(Constructor.prototype, "constructor", {
      ...constructorDescriptor, value: facade,
    });
    return facade;
  };

  class BoundedLocale {
    constructor(tag) {
      const parts = localeTag(tag).split("-");
      if (!/^[A-Za-z]{2,8}$/.test(parts[0] || "")) throw new RangeError("Invalid language tag");
      this.language = parts[0].toLowerCase();
      this.script = "";
      this.region = "";
      for (const part of parts.slice(1)) {
        if (!this.script && /^[A-Za-z]{4}$/.test(part)) {
          this.script = part[0].toUpperCase() + part.slice(1).toLowerCase();
        } else if (!this.region && /^([A-Za-z]{2}|[0-9]{3})$/.test(part)) {
          this.region = part.toUpperCase();
        }
      }
      this.baseName = [this.language, this.script, this.region].filter(Boolean).join("-");
    }
    toString() { return this.baseName; }
    maximize() { return new BoundedLocale(this.baseName); }
    minimize() { return new BoundedLocale(this.baseName); }
  }

  class BoundedNumberFormat {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.options = options || {};
    }
    format(value) {
      let number = Number(value);
      if (this.options.style === "percent") number *= 100;
      const minimum = this.options.minimumFractionDigits === undefined
        ? 0 : Number(this.options.minimumFractionDigits);
      const maximum = this.options.maximumFractionDigits === undefined
        ? Math.max(minimum, 3) : Number(this.options.maximumFractionDigits);
      let result = Number.isFinite(number)
        ? number.toFixed(Math.min(20, maximum)) : "NaN";
      if (maximum > minimum && result.includes(".")) {
        while (result.endsWith("0") && result.split(".")[1].length > minimum) {
          result = result.slice(0, -1);
        }
        if (result.endsWith(".")) result = result.slice(0, -1);
      }
      if (this.options.useGrouping !== false) {
        const pair = result.split(".");
        pair[0] = pair[0].replace(/\B(?=(\d{3})+(?!\d))/g, ",");
        result = pair.join(".");
      }
      if (this.options.style === "percent") result += "%";
      if (this.options.style === "currency") {
        result = `${this.options.currency || "USD"} ${result}`;
      }
      return result;
    }
    formatToParts(value) { return [{ type: "integer", value: this.format(value) }]; }
    resolvedOptions() {
      return {
        locale: this.locale, numberingSystem: "latn", style: this.options.style || "decimal",
        minimumFractionDigits: this.options.minimumFractionDigits || 0,
        maximumFractionDigits: this.options.maximumFractionDigits === undefined
          ? 3 : this.options.maximumFractionDigits,
        useGrouping: this.options.useGrouping !== false,
        notation: "standard", signDisplay: "auto",
      };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  class BoundedPluralRules {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.options = options || {};
    }
    select(value) {
      const number = Math.abs(Number(value));
      if (this.options.type === "ordinal") {
        const mod10 = number % 10;
        const mod100 = number % 100;
        if (mod10 === 1 && mod100 !== 11) return "one";
        if (mod10 === 2 && mod100 !== 12) return "two";
        if (mod10 === 3 && mod100 !== 13) return "few";
        return "other";
      }
      return number === 1 ? "one" : "other";
    }
    resolvedOptions() {
      return {
        locale: this.locale, type: this.options.type || "cardinal",
        pluralCategories: this.options.type === "ordinal"
          ? ["few", "one", "two", "other"] : ["one", "other"],
      };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  class BoundedDateTimeFormat {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.options = options || {};
    }
    _date(value) {
      const date = value === undefined ? new Date() : new Date(value);
      if (!Number.isFinite(date.getTime())) throw new RangeError("Invalid time value");
      return date;
    }
    format(value) {
      const date = this._date(value);
      if (this.options.timeStyle || this.options.hour !== undefined) {
        const pad = (part) => String(part).padStart(2, "0");
        const hour = date.getUTCHours();
        return `${hour % 12 || 12}:${pad(date.getUTCMinutes())}:` +
          `${pad(date.getUTCSeconds())} ${hour < 12 ? "AM" : "PM"}`;
      }
      return `${date.getUTCMonth() + 1}/${date.getUTCDate()}/${date.getUTCFullYear()}`;
    }
    formatToParts(value) {
      const date = this._date(value);
      if (this.options.timeStyle || this.options.hour !== undefined) {
        return [{ type: "literal", value: this.format(date) }];
      }
      return [
        { type: "month", value: String(date.getUTCMonth() + 1) },
        { type: "literal", value: "/" },
        { type: "day", value: String(date.getUTCDate()) },
        { type: "literal", value: "/" },
        { type: "year", value: String(date.getUTCFullYear()) },
      ];
    }
    resolvedOptions() {
      return {
        locale: this.locale, calendar: "gregory", numberingSystem: "latn",
        timeZone: "UTC", year: "numeric", month: "numeric", day: "numeric",
      };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  class BoundedCollator {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.usage = options.usage === "search" ? "search" : "sort";
      this.sensitivity = ["base", "accent", "case", "variant"].includes(options.sensitivity)
        ? options.sensitivity : "variant";
      this.ignorePunctuation = Boolean(options.ignorePunctuation);
      this.numeric = Boolean(options.numeric);
      this.caseFirst = ["upper", "lower", "false"].includes(options.caseFirst)
        ? options.caseFirst : "false";
      this._boundCompare = null;
    }
    get compare() {
      if (!this._boundCompare) this._boundCompare = nativeShape((left, right) => this._compare(left, right));
      return this._boundCompare;
    }
    _fold(value) {
      let result = String(value);
      if (this.sensitivity === "base" || this.sensitivity === "case") {
        result = result.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
      }
      if (this.sensitivity === "base" || this.sensitivity === "accent") {
        result = result.toLowerCase();
      }
      if (this.ignorePunctuation) {
        result = result.replace(/[\s!"#$%&'()*+,./:;<=>?@[\\\]^_`{|}~-]+/g, "");
      }
      return result;
    }
    _compare(left, right) {
      const first = this._fold(left);
      const second = this._fold(right);
      if (first === second) return 0;
      if (this.numeric) {
        const firstParts = first.match(/\d+|\D+/g) || [];
        const secondParts = second.match(/\d+|\D+/g) || [];
        const length = Math.min(firstParts.length, secondParts.length);
        for (let index = 0; index < length; index += 1) {
          if (firstParts[index] === secondParts[index]) continue;
          if (/^\d+$/.test(firstParts[index]) && /^\d+$/.test(secondParts[index])) {
            const firstNumber = Number(firstParts[index]);
            const secondNumber = Number(secondParts[index]);
            if (firstNumber !== secondNumber) return firstNumber < secondNumber ? -1 : 1;
          }
          return firstParts[index] < secondParts[index] ? -1 : 1;
        }
        if (firstParts.length !== secondParts.length) {
          return firstParts.length < secondParts.length ? -1 : 1;
        }
      }
      return first < second ? -1 : 1;
    }
    resolvedOptions() {
      return {
        locale: this.locale, usage: this.usage, sensitivity: this.sensitivity,
        ignorePunctuation: this.ignorePunctuation, collation: "default",
        numeric: this.numeric, caseFirst: this.caseFirst,
      };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  const relativeUnits = {
    second: "second", seconds: "second", minute: "minute", minutes: "minute",
    hour: "hour", hours: "hour", day: "day", days: "day", week: "week", weeks: "week",
    month: "month", months: "month", quarter: "quarter", quarters: "quarter",
    year: "year", years: "year",
  };
  const relativeAutomatic = {
    second: { 0: "now" }, day: { "-1": "yesterday", 0: "today", 1: "tomorrow" },
    week: { "-1": "last week", 0: "this week", 1: "next week" },
    month: { "-1": "last month", 0: "this month", 1: "next month" },
    quarter: { "-1": "last quarter", 0: "this quarter", 1: "next quarter" },
    year: { "-1": "last year", 0: "this year", 1: "next year" },
  };
  class BoundedRelativeTimeFormat {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.style = ["long", "short", "narrow"].includes(options.style)
        ? options.style : "long";
      this.numeric = options.numeric === "auto" ? "auto" : "always";
    }
    format(value, unit) {
      const number = Number(value);
      const name = relativeUnits[String(unit)];
      if (!name) throw new RangeError("Invalid unit");
      if (!Number.isFinite(number)) throw new RangeError("Invalid value");
      const automatic = this.numeric === "auto"
        ? relativeAutomatic[name]?.[String(number)] : undefined;
      if (automatic !== undefined) return automatic;
      const magnitude = Math.abs(number);
      const label = this.style === "narrow"
        ? ({ second: "s", minute: "m", hour: "h", day: "d", week: "w",
          month: "mo", quarter: "q", year: "y" })[name]
        : name + (magnitude === 1 ? "" : "s");
      return number < 0 ? `${magnitude} ${label} ago` : `in ${magnitude} ${label}`;
    }
    formatToParts(value, unit) {
      const result = this.format(value, unit);
      const number = String(Math.abs(Number(value)));
      const index = result.indexOf(number);
      return index < 0 ? [{ type: "literal", value: result }] : [
        { type: "literal", value: result.slice(0, index) },
        { type: "integer", value: number, unit: relativeUnits[String(unit)] },
        { type: "literal", value: result.slice(index + number.length) },
      ];
    }
    resolvedOptions() {
      return { locale: this.locale, style: this.style, numeric: this.numeric,
        numberingSystem: "latn" };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  class BoundedListFormat {
    constructor(locales, options = {}) {
      this.locale = localeOf(locales);
      this.type = ["conjunction", "disjunction", "unit"].includes(options.type)
        ? options.type : "conjunction";
      this.style = ["long", "short", "narrow"].includes(options.style)
        ? options.style : "long";
    }
    _items(values) {
      const items = [];
      for (const value of values) {
        if (typeof value !== "string") throw new TypeError("List items must be strings");
        if (items.length >= 4096) throw new RangeError("List item limit exceeded");
        items.push(value);
      }
      return items;
    }
    _separator(final = false) {
      if (this.type === "unit") return this.style === "long" ? ", " : " ";
      if (this.type === "disjunction") return final ? " or " : ", ";
      return final ? " and " : ", ";
    }
    format(values) {
      const items = this._items(values);
      if (items.length < 2) return items[0] || "";
      if (items.length === 2) return items[0] + this._separator(true) + items[1];
      return items.slice(0, -1).join(this._separator(false)) + "," +
        this._separator(true) + items[items.length - 1];
    }
    formatToParts(values) {
      const items = this._items(values);
      const parts = [];
      for (let index = 0; index < items.length; index += 1) {
        if (index) {
          parts.push({
            type: "literal",
            value: items.length === 2 ? this._separator(true)
              : (index === items.length - 1 ? `,${this._separator(true)}`
                : this._separator(false)),
          });
        }
        parts.push({ type: "element", value: items[index] });
      }
      return parts;
    }
    resolvedOptions() { return { locale: this.locale, type: this.type, style: this.style }; }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  const displayLanguages = {
    en: "English", es: "Spanish", fr: "French", de: "German", it: "Italian",
    ja: "Japanese", ko: "Korean", pt: "Portuguese", ru: "Russian",
    zh: "Chinese", ar: "Arabic", hi: "Hindi",
  };
  const displayRegions = {
    US: "United States", GB: "United Kingdom", CA: "Canada", DE: "Germany",
    FR: "France", JP: "Japan", CN: "China", IN: "India",
  };
  class BoundedDisplayNames {
    constructor(locales, options = {}) {
      if (!options || !options.type) throw new TypeError("type is required");
      this.locale = localeOf(locales);
      this.type = String(options.type);
      this.style = ["long", "short", "narrow"].includes(options.style)
        ? options.style : "long";
      this.fallback = options.fallback === "none" ? "none" : "code";
      this.languageDisplay = options.languageDisplay === "dialect" ? "dialect" : "standard";
    }
    of(code) {
      const original = String(code);
      let value;
      if (this.type === "language") value = displayLanguages[original.toLowerCase().split("-")[0]];
      else if (this.type === "region") value = displayRegions[original.toUpperCase()];
      else if (this.type === "currency") {
        value = ({ USD: "US Dollar", EUR: "Euro", GBP: "British Pound",
          JPY: "Japanese Yen" })[original.toUpperCase()];
      } else if (this.type === "dateTimeField") {
        value = ({ year: "year", month: "month", week: "week", day: "day",
          hour: "hour", minute: "minute", second: "second" })[original];
      } else if (this.type === "script" || this.type === "calendar") value = undefined;
      else throw new RangeError("Invalid type");
      return value === undefined ? (this.fallback === "none" ? undefined : original) : value;
    }
    resolvedOptions() {
      return {
        locale: this.locale, style: this.style, type: this.type,
        fallback: this.fallback, languageDisplay: this.languageDisplay,
      };
    }
    static supportedLocalesOf(locales) { return supportedLocales(locales); }
  }

  const boundedIntl = {
    Locale: constructOnly("Locale", BoundedLocale),
    NumberFormat: callable("NumberFormat", BoundedNumberFormat),
    PluralRules: constructOnly("PluralRules", BoundedPluralRules),
    DateTimeFormat: callable("DateTimeFormat", BoundedDateTimeFormat),
    Collator: callable("Collator", BoundedCollator),
    RelativeTimeFormat: constructOnly("RelativeTimeFormat", BoundedRelativeTimeFormat),
    ListFormat: constructOnly("ListFormat", BoundedListFormat),
    DisplayNames: constructOnly("DisplayNames", BoundedDisplayNames),
    getCanonicalLocales: nativeShape(function getCanonicalLocales(locales) {
      return supportedLocales(locales);
    }),
  };
  const intlDescriptor = Object.getOwnPropertyDescriptor(globalThis, "Intl");
  Object.defineProperty(globalThis, "Intl", intlDescriptor && "value" in intlDescriptor
    ? { ...intlDescriptor, value: boundedIntl }
    : { value: boundedIntl, writable: true, enumerable: false, configurable: true });

  if (globalThis.PerformanceObserver) {
    const OriginalPerformanceObserver = globalThis.PerformanceObserver;
    const supportedTypesDescriptor = Object.getOwnPropertyDescriptor(
      OriginalPerformanceObserver, "supportedEntryTypes",
    );
    class ReplayPerformanceObserver {
      constructor(callback) {
        if (typeof callback !== "function") throw new TypeError("callback required");
        this.callback = callback;
      }
      observe() {}
      disconnect() {}
      takeRecords() { return []; }
    }
    for (const name of ["observe", "disconnect", "takeRecords"]) {
      const descriptor = Object.getOwnPropertyDescriptor(
        ReplayPerformanceObserver.prototype, name,
      );
      Object.defineProperty(ReplayPerformanceObserver.prototype, name, {
        ...descriptor, value: new Proxy(descriptor.value, {}),
      });
    }
    Object.defineProperty(ReplayPerformanceObserver, "name", {
      value: "PerformanceObserver", configurable: true,
    });
    Object.defineProperty(ReplayPerformanceObserver, "supportedEntryTypes", {
      value: [], writable: false,
      enumerable: supportedTypesDescriptor ? supportedTypesDescriptor.enumerable : true,
      configurable: true,
    });
    const DeterministicPerformanceObserver = new Proxy(ReplayPerformanceObserver, {});
    const constructorDescriptor = Object.getOwnPropertyDescriptor(
      ReplayPerformanceObserver.prototype, "constructor",
    );
    Object.defineProperty(ReplayPerformanceObserver.prototype, "constructor", {
      ...constructorDescriptor, value: DeterministicPerformanceObserver,
    });
    const observerGlobalDescriptor = Object.getOwnPropertyDescriptor(
      globalThis, "PerformanceObserver",
    );
    Object.defineProperty(globalThis, "PerformanceObserver", {
      ...observerGlobalDescriptor, value: DeterministicPerformanceObserver,
    });
  }
  Object.defineProperty(globalThis, "__tilefinchReplayClockEvidence", {
    value: () => {
      return {
        contract: configuration.clockContract,
        scope: configuration.clockScope,
        originMs,
        hostElapsedMs,
        playwrightElapsedMs: Math.max(
          0, Math.trunc(playwrightMonotonicNow === null
            ? Reflect.apply(playwrightDateNow, NativeDate, []) - originMs
            : playwrightMonotonicNow() - playwrightMonotonicOrigin),
        ),
        wallElapsedMs,
        monotonicElapsedMs,
        wallObservations,
        monotonicObservations,
        monotonicSamples,
        clockSources: { ...clockSources },
      };
    },
    configurable: false, writable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchAdvanceReplayClock", {
    value: (milliseconds) => {
      if (!Number.isSafeInteger(milliseconds) || milliseconds < 0) {
        throw new TypeError("deterministic replay clock advance must be a nonnegative integer");
      }
      hostElapsedMs += milliseconds;
      wallElapsedMs += milliseconds;
      monotonicElapsedMs += milliseconds;
    },
    configurable: false, writable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchReplayEnvironment", {
    value: configuration.version, configurable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchReplayClock", {
    value: configuration.clockContract, configurable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchReplayIntl", {
    value: configuration.intlContract, configurable: false, enumerable: false,
  });
}

/* Prevent mutating requests before they reach Chromium's network stack.  The
   route handler remains a hard backstop: any non-GET/HEAD request that evades
   these web-platform shims is aborted and poisons qualification. */
function installReadOnlyPolicy(configuration) {
  if (!configuration || configuration.preflightPolicy
      !== "cors-preflight-before-network-v1") {
    throw new Error("read-only policy requires the CORS preflight boundary");
  }
  const reportBinding = globalThis.__tilefinchReadOnlyDenied;
  if (typeof reportBinding !== "function") {
    throw new Error("read-only denial binding is unavailable");
  }
  const methodName = (value) => String(value || "GET").toUpperCase();
  const safe = (method) => method === "GET" || method === "HEAD";
  const descriptorOwner = (object, property) => {
    for (let current = object; current; current = Object.getPrototypeOf(current)) {
      if (Object.prototype.hasOwnProperty.call(current, property)) return current;
    }
    return null;
  };
  const replaceValue = (object, property, value) => {
    const owner = descriptorOwner(object, property);
    const descriptor = owner && Object.getOwnPropertyDescriptor(owner, property);
    if (!descriptor || !("value" in descriptor)) {
      throw new Error(`read-only policy cannot replace ${String(property)}`);
    }
    Object.defineProperty(owner, property, { ...descriptor, value });
  };
  const report = (method, kind, value) => {
    let target = String(value || "");
    try { target = new URL(target, globalThis.document && document.baseURI).href; }
    catch (_error) {}
    try {
      const pending = reportBinding(methodName(method), String(kind), target);
      if (pending && typeof pending.catch === "function") pending.catch(() => {});
    } catch (_error) {}
  };
  const corsUnsafeByte = (value) => /[\x00-\x08\x0A-\x1F\x60\x7F"():<>?@[\\\]{}]/
    .test(String(value));
  const corsLanguageValue = (value) => !/^[0-9A-Za-z *,\-.;=]*$/
    .test(String(value));
  const corsSafelistedHeader = (name, value) => {
    const lower = String(name).toLowerCase();
    const text = String(value);
    if (new TextEncoder().encode(text).byteLength > 128) return false;
    if (lower === "accept") return !corsUnsafeByte(text);
    if (lower === "accept-language" || lower === "content-language") {
      return !corsLanguageValue(text);
    }
    if (lower === "content-type") {
      if (corsUnsafeByte(text)) return false;
      const parts = text.split(";");
      const essence = parts.shift().trim().toLowerCase();
      if (![
        "application/x-www-form-urlencoded", "multipart/form-data", "text/plain",
      ].includes(essence)) return false;
      const parameterName = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
      const parameterValue = /^(?:[!#$%&'*+.^_`|~0-9A-Za-z-]+|"(?:[\t\x20-\x21\x23-\x5B\x5D-\x7E]|\\[\t\x20-\x7E])*")$/;
      const names = new Set();
      for (const rawParameter of parts) {
        const equals = rawParameter.indexOf("=");
        if (equals <= 0) return false;
        const name = rawParameter.slice(0, equals).trim().toLowerCase();
        const value = rawParameter.slice(equals + 1).trim();
        if (!parameterName.test(name) || !parameterValue.test(value)
            || names.has(name)) return false;
        names.add(name);
      }
      return true;
    }
    if (lower === "range") {
      return /^bytes=[0-9]+-[0-9]*$/.test(text.trim());
    }
    return false;
  };
  const corsPreflightRequired = (url, mode, headers, uploadListeners = false) => {
    let target;
    try { target = new URL(String(url), globalThis.document && document.baseURI); }
    catch (_error) { return false; }
    /* Same-origin requests never preflight, whatever their mode. */
    try {
      if (globalThis.location && target.origin === location.origin) return false;
    } catch (_error) {}
    if (String(mode) !== "cors") return false;
    if (uploadListeners) return true;
    for (const [name, value] of headers) {
      if (!corsSafelistedHeader(name, value)) return true;
    }
    return false;
  };
  const rejectPreflight = (method, kind, url) => {
    report(method, `${kind}-cors-preflight`, url);
    return new TypeError(
      "CORS-preflight request blocked before Chromium network synthesis",
    );
  };

  if (typeof globalThis.fetch === "function") {
    const realFetch = globalThis.fetch;
    replaceValue(globalThis, "fetch", new Proxy(realFetch, {
      apply(target, thisArgument, argumentsList) {
        const [input, init] = argumentsList;
        let request;
        try { request = new Request(input, init); }
        catch (error) { return Promise.reject(error); }
        const method = methodName(request.method);
        if (!safe(method)) {
          report(method, "fetch", request.url);
          return Promise.reject(new TypeError(
            "non-read-only fetch blocked during offline capture",
          ));
        }
        if (corsPreflightRequired(
          request.url, request.mode, request.headers, false,
        )) {
          return Promise.reject(rejectPreflight(method, "fetch", request.url));
        }
        return Reflect.apply(target, thisArgument, [request]);
      },
    }));
  }

  /* fetchLater can outlive the evidence snapshot and context teardown. Block
     every method, including GET, instead of treating it as ordinary fetch. */
  if (typeof globalThis.fetchLater === "function") {
    const realFetchLater = globalThis.fetchLater;
    replaceValue(globalThis, "fetchLater", new Proxy(realFetchLater, {
      apply(_target, _thisArgument, argumentsList) {
        const [input, init] = argumentsList;
        const method = methodName(init && init.method !== undefined
          ? init.method : (input && input.method));
        report(method, "fetchlater", input && input.url !== undefined ? input.url : input);
        throw new DOMException("fetchLater is unavailable during offline capture", "NetworkError");
      },
    }));
  }

  if (globalThis.XMLHttpRequest) {
    const requests = new WeakMap();
    const uploads = new WeakMap();
    const realOpen = XMLHttpRequest.prototype.open;
    const realSetRequestHeader = XMLHttpRequest.prototype.setRequestHeader;
    const realSend = XMLHttpRequest.prototype.send;
    const forbiddenHeader = (name) => {
      const lower = String(name).toLowerCase();
      return [
        "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie",
        "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin",
        "referer", "set-cookie", "te", "trailer", "transfer-encoding",
        "upgrade", "via",
      ].includes(lower) || lower.startsWith("proxy-") || lower.startsWith("sec-");
    };
    replaceValue(XMLHttpRequest.prototype, "open", new Proxy(realOpen, {
      apply(target, thisArgument, argumentsList) {
        const [method, url] = argumentsList;
        const result = Reflect.apply(target, thisArgument, argumentsList);
        const state = {
          method: methodName(method), url: String(url), headers: new Headers(),
          uploadListeners: new Map(),
        };
        requests.set(thisArgument, state);
        try {
          const upload = thisArgument.upload;
          const uploadState = uploads.get(upload) || {
            listeners: new Map(), request: null,
          };
          uploadState.request = state;
          state.uploadListeners = uploadState.listeners;
          uploads.set(upload, uploadState);
        } catch (_error) {}
        return result;
      },
    }));
    replaceValue(XMLHttpRequest.prototype, "setRequestHeader", new Proxy(
      realSetRequestHeader, {
        apply(target, thisArgument, argumentsList) {
          const result = Reflect.apply(target, thisArgument, argumentsList);
          const state = requests.get(thisArgument);
          const [name, value] = argumentsList;
          if (state && !forbiddenHeader(name)) state.headers.append(name, value);
          return result;
        },
      },
    ));
    if (globalThis.XMLHttpRequestUpload && XMLHttpRequestUpload.prototype) {
      const uploadAdd = XMLHttpRequestUpload.prototype.addEventListener;
      const uploadRemove = XMLHttpRequestUpload.prototype.removeEventListener;
      if (typeof uploadAdd === "function" && typeof uploadRemove === "function") {
        replaceValue(XMLHttpRequestUpload.prototype, "addEventListener", new Proxy(
          uploadAdd, {
            apply(target, thisArgument, argumentsList) {
              const result = Reflect.apply(target, thisArgument, argumentsList);
              /* The replaced descriptor lives on EventTarget.prototype (its
                 owner), so every page listener registration passes through
                 here -- including bare global addEventListener(...) calls
                 whose strict-mode receiver is undefined.  Only genuine
                 upload targets participate in preflight mirroring; anything
                 else must not become a WeakMap key. */
              if (!(thisArgument instanceof XMLHttpRequestUpload)) {
                return result;
              }
              let state = uploads.get(thisArgument);
              if (!state) {
                state = { listeners: new Map(), request: null };
                uploads.set(thisArgument, state);
              }
              const type = String(argumentsList[0]);
              const callback = argumentsList[1];
              if (typeof callback === "function"
                  || (callback && typeof callback.handleEvent === "function")) {
                if (!state.listeners.has(type)) {
                  state.listeners.set(type, new Set());
                }
                state.listeners.get(type).add(callback);
              }
              return result;
            },
          },
        ));
        replaceValue(XMLHttpRequestUpload.prototype, "removeEventListener", new Proxy(
          uploadRemove, {
            apply(target, thisArgument, argumentsList) {
              /* Chromium's upload-listener flag is internal and listener
                 removal includes capture/options matching. Retain the
                 conservative page-observed flag after a successful add so an
                 imprecise mirror can never miss a real preflight. */
              return Reflect.apply(target, thisArgument, argumentsList);
            },
          },
        ));
      }
    }
    replaceValue(XMLHttpRequest.prototype, "send", new Proxy(realSend, {
      apply(target, thisArgument, argumentsList) {
        const request = requests.get(thisArgument) || {
          method: "GET", url: "", headers: new Headers(), uploadListeners: new Map(),
        };
        if (!safe(request.method)) {
          report(request.method, "xmlhttprequest", request.url);
          throw new DOMException(
            "non-read-only XMLHttpRequest blocked during offline capture", "NetworkError",
          );
        }
        let hasUploadListeners = [...request.uploadListeners.values()]
          .some((listeners) => listeners.size !== 0);
        try {
          const upload = thisArgument.upload;
          hasUploadListeners ||= [
            "loadstart", "progress", "abort", "error", "load", "timeout", "loadend",
          ].some((type) => typeof upload[`on${type}`] === "function");
        } catch (_error) {}
        let targetUrl = request.url;
        try { targetUrl = new URL(request.url, document.baseURI).href; }
        catch (_error) {}
        if (corsPreflightRequired(
          targetUrl, "cors", request.headers, hasUploadListeners,
        )) {
          throw new DOMException(
            rejectPreflight(request.method, "xmlhttprequest", targetUrl).message,
            "NetworkError",
          );
        }
        return Reflect.apply(target, thisArgument, argumentsList);
      },
    }));
  }

  if (globalThis.Navigator && "sendBeacon" in Navigator.prototype) {
    const realSendBeacon = Navigator.prototype.sendBeacon;
    replaceValue(Navigator.prototype, "sendBeacon", new Proxy(realSendBeacon, {
      apply(target, thisArgument, argumentsList) {
        const [url] = argumentsList;
        if (!(thisArgument instanceof Navigator)) {
          return Reflect.apply(target, thisArgument, argumentsList);
        }
        report("POST", "beacon", url);
        return false;
      },
    }));
  }

  if (globalThis.HTMLFormElement) {
    const effectiveFormMethod = (form, submitter) => methodName(
      submitter && submitter.formMethod ? submitter.formMethod : form.method,
    );
    const localFormMethod = (method) => method === "DIALOG";
    const effectiveFormUrl = (form, submitter) =>
      submitter && submitter.formAction ? submitter.formAction : form.action;
    const realSubmit = HTMLFormElement.prototype.submit;
    replaceValue(HTMLFormElement.prototype, "submit", new Proxy(realSubmit, {
      apply(target, thisArgument, argumentsList) {
        if (!(thisArgument instanceof HTMLFormElement)) {
          return Reflect.apply(target, thisArgument, argumentsList);
        }
        const method = effectiveFormMethod(thisArgument, null);
        if (safe(method) || localFormMethod(method)) {
          return Reflect.apply(target, thisArgument, argumentsList);
        }
        report(method, "form-submit", effectiveFormUrl(thisArgument, null));
        return undefined;
      },
    }));
    if ("requestSubmit" in HTMLFormElement.prototype) {
      const realRequestSubmit = HTMLFormElement.prototype.requestSubmit;
      replaceValue(HTMLFormElement.prototype, "requestSubmit", new Proxy(realRequestSubmit, {
        apply(target, thisArgument, argumentsList) {
          const [submitter] = argumentsList;
          if (!(thisArgument instanceof HTMLFormElement)) {
            return Reflect.apply(target, thisArgument, argumentsList);
          }
          const method = effectiveFormMethod(thisArgument, submitter);
          if (safe(method) || localFormMethod(method)) {
            return Reflect.apply(target, thisArgument, argumentsList);
          }
          report(method, "form-request-submit", effectiveFormUrl(thisArgument, submitter));
          return undefined;
        },
      }));
    }
    globalThis.addEventListener("submit", (event) => {
      const form = event.target;
      if (!(form instanceof HTMLFormElement)) return;
      const method = effectiveFormMethod(form, event.submitter);
      if (safe(method) || localFormMethod(method)) return;
      event.preventDefault();
      report(method, "form-event", effectiveFormUrl(form, event.submitter));
    }, true);
  }

  Object.defineProperty(globalThis, "__tilefinchReadOnlyPolicy", {
    value: configuration.version, configurable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchReadOnlyPreflightPolicy", {
    value: configuration.preflightPolicy, configurable: false, enumerable: false,
  });
}

/* Deny creation of realms and delayed/background capabilities that do not
   inherit context.addInitScript. Normal page/iframe realms retain deterministic
   entropy; every bypass surface is either normalized locally or blocked and
   reported through one bounded host ledger. */
function installOfflineCapabilityPolicy(configuration) {
  const reportBinding = globalThis.__tilefinchBlockedCapability;
  if (typeof reportBinding !== "function") {
    throw new Error("offline capability denial binding is unavailable");
  }
  if (!configuration || typeof configuration.version !== "string"
      || typeof configuration.surfaceVersion !== "string") {
    throw new Error("offline capability policy configuration is invalid");
  }
  const descriptorOwner = (object, property) => {
    for (let current = object; current; current = Object.getPrototypeOf(current)) {
      if (Object.prototype.hasOwnProperty.call(current, property)) return current;
    }
    return null;
  };
  const replaceValue = (object, property, value) => {
    const owner = descriptorOwner(object, property);
    const descriptor = owner && Object.getOwnPropertyDescriptor(owner, property);
    if (!descriptor || !("value" in descriptor)) {
      throw new Error(`offline capability policy cannot replace ${String(property)}`);
    }
    Object.defineProperty(owner, property, { ...descriptor, value });
  };
  const report = (kind, value) => {
    try {
      const pending = reportBinding(String(kind), String(value || ""));
      if (pending && typeof pending.catch === "function") pending.catch(() => {});
    } catch (_error) {}
  };
  /* A throwing constructor is still a positive feature signal. Sites can take
     a timing-dependent worker path after `typeof SharedWorker === "function"`,
     and even a correctly denied attempt then leaves a random blob URL in the
     diagnostic ledger. Remove native realm entry points before page code so
     feature detection and behavior are both stable. Every removed descriptor
     must be configurable; a future browser that cannot provide this boundary
     fails its init script instead of silently weakening the capture. */
  const removeSurface = (object, property) => {
    if (!object) return;
    const owner = descriptorOwner(object, property);
    if (!owner) return;
    const descriptor = Object.getOwnPropertyDescriptor(owner, property);
    if (!descriptor || descriptor.configurable !== true
        || !Reflect.deleteProperty(owner, property)
        || property in object) {
      throw new Error(`offline capability policy cannot remove ${String(property)}`);
    }
  };

  /* Retain prototype backstops for worklet objects discovered before their
     getters are removed. They are not reachable by page code after a healthy
     install, but this also closes browser aliases that share the prototype. */
  const workletPrototypes = new Set();
  if (globalThis.Worklet && Worklet.prototype) workletPrototypes.add(Worklet.prototype);
  for (const name of ["paintWorklet", "layoutWorklet", "animationWorklet"]) {
    try {
      const worklet = globalThis.CSS && CSS[name];
      if (worklet) workletPrototypes.add(Object.getPrototypeOf(worklet));
    } catch (_error) {}
  }
  try {
    const worklet = globalThis.sharedStorage && sharedStorage.worklet;
    if (worklet) workletPrototypes.add(Object.getPrototypeOf(worklet));
  } catch (_error) {}
  for (const prototype of workletPrototypes) {
    const original = prototype && prototype.addModule;
    if (typeof original !== "function") continue;
    replaceValue(prototype, "addModule", new Proxy(original, {
      apply(_target, _thisArgument, argumentsList) {
        const [url] = argumentsList;
        report("WORKLET", url);
        return Promise.reject(
          new DOMException("worklets are unavailable during offline capture", "NetworkError"),
        );
      },
    }));
  }

  for (const name of [
    "Worker", "SharedWorker", "ShadowRealm", "AudioWorkletNode", "Worklet",
    "SharedStorage", "SharedStorageWorklet", "SharedStorageWorkletGlobalScope",
  ]) {
    removeSurface(globalThis, name);
  }
  removeSurface(globalThis, "sharedStorage");
  for (const name of ["paintWorklet", "layoutWorklet", "animationWorklet"]) {
    if (globalThis.CSS) removeSurface(CSS, name);
  }
  if (globalThis.BaseAudioContext && BaseAudioContext.prototype) {
    removeSurface(BaseAudioContext.prototype, "audioWorklet");
  }
  if (globalThis.Navigator && Navigator.prototype) {
    removeSurface(Navigator.prototype, "serviceWorker");
  } else if (globalThis.navigator) {
    removeSurface(navigator, "serviceWorker");
  }
  for (const name of [
    "ServiceWorker", "ServiceWorkerContainer", "ServiceWorkerRegistration",
  ]) {
    removeSurface(globalThis, name);
  }

  const surfaceEvidence = Object.freeze({
    version: configuration.surfaceVersion,
    dedicated_worker_constructor:
      !("Worker" in globalThis) ? "unavailable" : "exposed",
    shared_worker_constructor:
      !("SharedWorker" in globalThis) ? "unavailable" : "exposed",
    shadow_realm_constructor:
      !("ShadowRealm" in globalThis) ? "unavailable" : "exposed",
    audio_worklet_node_constructor:
      !("AudioWorkletNode" in globalThis) ? "unavailable" : "exposed",
    worklet_constructor:
      !("Worklet" in globalThis) ? "unavailable" : "exposed",
    css_worklet_loaders: !globalThis.CSS || [
      "paintWorklet", "layoutWorklet", "animationWorklet",
    ].every((name) => !(name in CSS)) ? "unavailable" : "exposed",
    audio_worklet_loader: !globalThis.BaseAudioContext
      || !("audioWorklet" in BaseAudioContext.prototype) ? "unavailable" : "exposed",
    shared_storage_worklet_loader: !("sharedStorage" in globalThis)
      && ["SharedStorage", "SharedStorageWorklet", "SharedStorageWorkletGlobalScope"]
        .every((name) => !(name in globalThis)) ? "unavailable" : "exposed",
    service_worker_registration: !globalThis.navigator
      || !("serviceWorker" in navigator) ? "unavailable" : "exposed",
    service_worker_interfaces: [
      "ServiceWorker", "ServiceWorkerContainer", "ServiceWorkerRegistration",
    ].every((name) => !(name in globalThis)) ? "unavailable" : "exposed",
  });
  if (Object.values(surfaceEvidence).some((value) => value === "exposed")) {
    throw new Error("offline capability realm surface remains exposed");
  }

  if (globalThis.SubtleCrypto && SubtleCrypto.prototype) {
    for (const method of ["generateKey", "encrypt", "sign", "wrapKey"]) {
      const original = SubtleCrypto.prototype[method];
      if (typeof original !== "function") continue;
      replaceValue(SubtleCrypto.prototype, method, new Proxy(original, {
        apply(target, thisArgument, argumentsList) {
          if (!(thisArgument instanceof SubtleCrypto)) {
            return Reflect.apply(target, thisArgument, argumentsList);
          }
          let algorithm = argumentsList[0];
          if (method === "wrapKey") algorithm = argumentsList[3];
          const name = typeof algorithm === "string" ? algorithm
            : (algorithm && typeof algorithm.name === "string" ? algorithm.name : "unknown");
          report(`WEBCRYPTO-${method.toUpperCase()}`, name);
          return Promise.reject(new DOMException(
            `${method} is unavailable during deterministic offline capture`, "NotSupportedError",
          ));
        },
      }));
    }
  }

  Object.defineProperty(globalThis, "__tilefinchOfflineCapabilityPolicy", {
    value: configuration.version, configurable: false, enumerable: false,
  });
  Object.defineProperty(globalThis, "__tilefinchOfflineCapabilityEvidence", {
    value: surfaceEvidence, configurable: false, enumerable: false, writable: false,
  });
}

function offlineCapabilityEvidenceReady(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const expectedKeys = Object.keys(OFFLINE_CAPABILITY_SURFACE_EVIDENCE).sort();
  const actualKeys = Object.keys(value).sort();
  return actualKeys.length === expectedKeys.length
    && actualKeys.every((key, index) => key === expectedKeys[index])
    && expectedKeys.every(
      (key) => value[key] === OFFLINE_CAPABILITY_SURFACE_EVIDENCE[key],
    );
}

/* Keep transport constructors native-shaped while denying their construction
   before any socket/peer state exists. */
function installOfflineProtocolPolicy() {
  const blockedProtocol = globalThis.__tilefinchBlockedProtocol;
  if (typeof blockedProtocol !== "function") {
    throw new Error("offline protocol denial binding is unavailable");
  }
  const report = (kind, value) => {
    try {
      const pending = blockedProtocol(kind, String(value));
      if (pending && typeof pending.catch === "function") pending.catch(() => {});
    } catch (_error) {}
  };
  const replaceConstructor = (name, kind, serialize = String) => {
    const Original = globalThis[name];
    if (typeof Original !== "function") return;
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    if (!descriptor || !("value" in descriptor)) {
      throw new Error(`offline protocol policy cannot replace ${name}`);
    }
    const Offline = new Proxy(Original, {
      construct(_target, argumentsList) {
        let value = "";
        try { value = serialize(argumentsList[0]); } catch (_error) {}
        report(kind, value);
        throw new DOMException(
          `${kind} is unavailable during offline reference capture`, "NetworkError",
        );
      },
    });
    Object.defineProperty(globalThis, name, { ...descriptor, value: Offline });
  };
  replaceConstructor("WebSocket", "WEBSOCKET");
  replaceConstructor("RTCPeerConnection", "WEBRTC", (value) => JSON.stringify(value || null));
  replaceConstructor(
    "webkitRTCPeerConnection", "WEBRTC", (value) => JSON.stringify(value || null),
  );
  replaceConstructor("WebTransport", "WEBTRANSPORT");
}

function incrementBoundedCounter(counter, rawKey) {
  const key = String(rawKey || "unknown").slice(0, 256);
  if (Object.prototype.hasOwnProperty.call(counter, key)) {
    counter[key] += 1;
  } else if (Object.keys(counter).length < MAX_RESPONSE_SCHEDULER_REQUESTS) {
    counter[key] = 1;
  } else {
    counter.__counter_overflow__ = (counter.__counter_overflow__ || 0) + 1;
  }
}

function diagnosticOrigin(value) {
  try {
    const parsed = new URL(String(value));
    return parsed.protocol === "http:" || parsed.protocol === "https:"
      ? parsed.origin : parsed.protocol;
  } catch (_error) { return "invalid"; }
}

/* String.prototype.localeCompare() is host-locale and ICU dependent. Replay
   ordering is a wire-format contract, so compare the UTF-16 code units that
   JavaScript strings actually contain instead. */
function compareCodeUnitStrings(left, right) {
  const leftLength = left.length;
  const rightLength = right.length;
  const sharedLength = Math.min(leftLength, rightLength);
  for (let index = 0; index < sharedLength; index += 1) {
    const difference = left.charCodeAt(index) - right.charCodeAt(index);
    if (difference !== 0) return difference;
  }
  return leftLength - rightLength;
}

function createRequestDiagnostics(hardLimit = Number.POSITIVE_INFINITY) {
  const entries = [];
  const byMethod = Object.create(null);
  const byResourceType = Object.create(null);
  const byOrigin = Object.create(null);
  let total = 0;
  let multisetSum = 0n;
  const compareEntry = (left, right) =>
    compareCodeUnitStrings(left.classification, right.classification)
    || compareCodeUnitStrings(left.method, right.method)
    || compareCodeUnitStrings(left.resource_type, right.resource_type)
    || compareCodeUnitStrings(left.url_sha256, right.url_sha256)
    || compareCodeUnitStrings(left.url, right.url);
  const sortedCounter = (counter) => {
    const ordered = Object.entries(counter)
      .sort(([left], [right]) => compareCodeUnitStrings(left, right));
    const retained = ordered.slice(0, MAX_DIAGNOSTIC_COUNTER_KEYS);
    const omitted = ordered.slice(MAX_DIAGNOSTIC_COUNTER_KEYS)
      .reduce((total, [, count]) => total + count, 0);
    if (omitted !== 0) retained.push(["__other__", omitted]);
    return Object.fromEntries(retained);
  };
  return {
    record(classification, method, resourceType, value) {
      const canonicalMethod = String(method || "UNKNOWN").toUpperCase().slice(0, 64);
      const canonicalType = String(resourceType || "unknown").toLowerCase().slice(0, 64);
      const url = String(value);
      const urlHash = crypto.createHash("sha256").update(url).digest("hex");
      const eventHash = crypto.createHash("sha256").update(JSON.stringify([
        String(classification), canonicalMethod, canonicalType, url,
      ])).digest("hex");
      multisetSum = (multisetSum + BigInt(`0x${eventHash}`)) & UINT256_MASK;
      total += 1;
      incrementBoundedCounter(byMethod, canonicalMethod);
      incrementBoundedCounter(byResourceType, canonicalType);
      incrementBoundedCounter(byOrigin, diagnosticOrigin(url));
      const truncated = url.length > MAX_DIAGNOSTIC_URL;
      entries.push({
        classification: String(classification), method: canonicalMethod,
        resource_type: canonicalType,
        url: truncated ? url.slice(0, MAX_DIAGNOSTIC_URL) : url,
        url_sha256: urlHash, url_truncated: truncated,
      });
      entries.sort(compareEntry);
      if (entries.length > MAX_DIAGNOSTIC_ENTRIES) entries.pop();
    },
    summary() {
      const retained = entries.length;
      const multiset = multisetSum.toString(16).padStart(64, "0");
      return {
        total, retained, truncated: Math.max(0, total - retained),
        overflow: total > hardLimit,
        multiset_sha256: crypto.createHash("sha256")
          .update(`tilefinch-diagnostic-multiset-v1\0${total}\0${multiset}`).digest("hex"),
        by_method: sortedCounter(byMethod),
        by_resource_type: sortedCounter(byResourceType),
        by_origin: sortedCounter(byOrigin),
        entries: entries.map((entry) => ({ ...entry })),
      };
    },
  };
}

function buildReadOnlyAcquisitionPlan(summary) {
  const requests = new Map();
  let unplannable = summary.truncated;
  for (const entry of summary.entries) {
    if (entry.classification !== "unmatched") {
      unplannable += 1;
      continue;
    }
    if (!READ_ONLY_METHODS.has(entry.method) || entry.url_truncated) {
      unplannable += 1;
      continue;
    }
    const key = `${entry.method}\0${entry.url}`;
    if (!requests.has(key)) {
      requests.set(key, {
        method: entry.method, url: entry.url, url_sha256: entry.url_sha256,
        resource_types: new Set(), occurrences: 0,
      });
    }
    const planned = requests.get(key);
    planned.resource_types.add(entry.resource_type);
    planned.occurrences += 1;
  }
  const ordered = [...requests.values()].sort((left, right) =>
    compareCodeUnitStrings(left.method, right.method)
      || compareCodeUnitStrings(left.url, right.url));
  return {
    mode: "exact-get-head-plan-v1", complete: unplannable === 0,
    request_count: ordered.length, unplannable,
    requests: ordered.map((entry) => ({
      method: entry.method, url: entry.url, url_sha256: entry.url_sha256,
      resource_types: [...entry.resource_types].sort(), occurrences: entry.occurrences,
    })),
  };
}

/* Bound every host/Playwright promise which is part of replay publication.
   Once a deadline or rejection becomes terminal, no new qualifying operation
   is admitted. A timed-out promise is an orphan: its eventual settlement is
   tracked, but it cannot settle the wrapper promise or run caller ledger code. */
function createHostOperationTracker(configuration = {}) {
  if (!configuration || typeof configuration !== "object"
      || Array.isArray(configuration)) {
    throw new CaptureError("host operation tracker options are invalid");
  }
  const timeoutMs = configuration.timeoutMs === undefined
    ? DEFAULT_HOST_OPERATION_TIMEOUT_MS : configuration.timeoutMs;
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1
      || timeoutMs > MAX_HOST_OPERATION_TIMEOUT_MS) {
    throw new CaptureError("host operation tracker timeout is invalid");
  }
  const schedule = configuration.setTimeout === undefined
    ? setTimeout : configuration.setTimeout;
  const cancel = configuration.clearTimeout === undefined
    ? clearTimeout : configuration.clearTimeout;
  const onTerminalFailure = configuration.onTerminalFailure === undefined
    ? () => {} : configuration.onTerminalFailure;
  if (typeof schedule !== "function" || typeof cancel !== "function"
      || typeof onTerminalFailure !== "function") {
    throw new CaptureError("host operation tracker hooks are invalid");
  }
  let started = 0;
  let completed = 0;
  let rejected = 0;
  let timedOut = 0;
  let pending = 0;
  let orphaned = 0;
  let orphanPending = 0;
  let lateCompletions = 0;
  let terminalFailures = 0;
  let rejectedAfterTerminal = 0;
  let terminalError = null;
  let terminalLabel = "";
  let closed = false;

  const terminalize = (error, label) => {
    const failure = error instanceof CaptureError ? error : new CaptureError(
      `host operation ${label} failed: ${error && error.message
        ? error.message : String(error)}`,
    );
    if (terminalError === null) {
      terminalError = failure;
      terminalLabel = String(label);
      terminalFailures += 1;
      try { onTerminalFailure(failure, terminalLabel); } catch (_error) {}
    }
    return terminalError;
  };

  const run = (label, operation, options = {}) => {
    const canonicalLabel = String(label || "operation").slice(0, 128);
    if (typeof operation !== "function") {
      return Promise.reject(new CaptureError("host operation factory is invalid"));
    }
    const allowAfterTerminal = options.allowAfterTerminal === true;
    const terminalOnFailure = options.terminalOnFailure !== false;
    if (closed || (terminalError !== null && !allowAfterTerminal)) {
      rejectedAfterTerminal += 1;
      return Promise.reject(terminalError || new CaptureError(
        "host operation tracker is closed",
      ));
    }
    started += 1;
    pending += 1;
    return new Promise((resolve, rejectPromise) => {
      let settled = false;
      let orphan = false;
      let timer;
      const finish = (ok, value) => {
        if (orphan) {
          orphanPending -= 1;
          lateCompletions += 1;
          return;
        }
        if (settled) return;
        settled = true;
        cancel(timer);
        pending -= 1;
        if (ok) {
          completed += 1;
          resolve(value);
          return;
        }
        rejected += 1;
        const failure = terminalOnFailure
          ? terminalize(value, canonicalLabel) : value;
        rejectPromise(failure);
      };
      timer = schedule(() => {
        if (settled) return;
        settled = true;
        orphan = true;
        pending -= 1;
        timedOut += 1;
        orphaned += 1;
        orphanPending += 1;
        const failure = terminalize(new CaptureError(
          `host operation ${canonicalLabel} exceeded ${timeoutMs}ms deadline`,
        ), canonicalLabel);
        rejectPromise(failure);
      }, timeoutMs);
      let promise;
      try { promise = Promise.resolve(operation()); }
      catch (error) { promise = Promise.reject(error); }
      promise.then(
        (value) => finish(true, value),
        (error) => finish(false, error),
      );
    });
  };

  return {
    run,
    failTerminal(error, label = "external") {
      return terminalize(error, label);
    },
    isTerminal() { return terminalError !== null; },
    close() { closed = true; return this.summary(); },
    summary() {
      return {
        version: "bounded-host-operations-v1",
        timeout_ms: timeoutMs,
        started, completed, rejected, timed_out: timedOut, pending,
        orphaned, orphan_pending: orphanPending,
        late_completions: lateCompletions,
        terminal_failures: terminalFailures,
        rejected_after_terminal: rejectedAfterTerminal,
        terminal_label: terminalLabel,
        closed,
        ready: terminalFailures === 0 && rejected === 0 && timedOut === 0
          && pending === 0 && orphaned === 0 && orphanPending === 0,
      };
    },
  };
}

/* Keep exported helpers usable by dependency-free tests while routing every
   capture-owned Playwright promise through the publication ledger. */
function runTrackedHostOperation(hostOperations, label, operation, options) {
  if (hostOperations === undefined || hostOperations === null) {
    return Promise.resolve().then(operation);
  }
  if (typeof hostOperations.run !== "function") {
    return Promise.reject(new CaptureError("host operation tracker is unavailable"));
  }
  return hostOperations.run(label, operation, options);
}

function createResponseScheduler(configuration = {}) {
  const options = typeof configuration === "number"
    ? { limit: configuration } : configuration;
  if (!options || typeof options !== "object" || Array.isArray(options)) {
    throw new CaptureError("response scheduler options are invalid");
  }
  const boundedOption = (name, fallback, minimum, maximum) => {
    const value = options[name] === undefined ? fallback : options[name];
    if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
      throw new CaptureError(`response scheduler ${name} is invalid`);
    }
    return value;
  };
  const limit = boundedOption(
    "limit", MAX_RESPONSE_SCHEDULER_REQUESTS, 1,
    MAX_RESPONSE_SCHEDULER_REQUESTS,
  );
  const maxAdmissionProbes = boundedOption(
    "maxAdmissionProbes", MAX_RESPONSE_SCHEDULER_PROBES, 2,
    MAX_RESPONSE_SCHEDULER_PROBES,
  );
  const maxDriveSteps = boundedOption(
    "maxDriveSteps", MAX_RESPONSE_SCHEDULER_DRIVE_STEPS, 1,
    MAX_RESPONSE_SCHEDULER_DRIVE_STEPS,
  );
  const maximumRetainedWork = limit * MAX_RETAINED_DELAY_PUMPS;
  const maxPumps = boundedOption(
    "maxPumps", Math.min(MAX_RESPONSE_SCHEDULER_PUMPS, maximumRetainedWork),
    1, MAX_RESPONSE_SCHEDULER_PUMPS,
  );
  const maxWorkUnits = boundedOption(
    "maxWorkUnits", Math.min(
      MAX_RESPONSE_SCHEDULER_WORK_UNITS, maximumRetainedWork,
    ),
    1, MAX_RESPONSE_SCHEDULER_WORK_UNITS,
  );
  if (maxPumps > maximumRetainedWork || maxWorkUnits > maximumRetainedWork) {
    throw new CaptureError(
      "response scheduler pump/work bounds exceed possible retained work",
    );
  }
  const probeTimeoutMs = boundedOption(
    "probeTimeoutMs", RESPONSE_SCHEDULER_PROBE_TIMEOUT_MS, 1,
    MAX_RESPONSE_SCHEDULER_PROBE_TIMEOUT_MS,
  );
  const prepareTimeoutMs = boundedOption(
    "prepareTimeoutMs", RESPONSE_SCHEDULER_PREPARATION_TIMEOUT_MS, 1,
    MAX_RESPONSE_SCHEDULER_PREPARATION_TIMEOUT_MS,
  );
  const probe = options.probe === undefined ? async () => {}
    : options.probe;
  const activity = options.activity === undefined ? () => 0
    : options.activity;
  const onTerminalFailure = options.onTerminalFailure === undefined
    ? () => {} : options.onTerminalFailure;
  if (typeof probe !== "function" || typeof activity !== "function"
      || typeof onTerminalFailure !== "function") {
    throw new CaptureError("response scheduler probe/activity hooks are invalid");
  }

  let staged = [];
  const preparing = new Set();
  const admitted = [];
  const exactOrderHash = crypto.createHash("sha256")
    .update("tilefinch-response-schedule-v5\0");
  const semanticOrderHash = crypto.createHash("sha256")
    .update("tilefinch-semantic-delivery-order-v1\0");
  const rawCallbackHash = crypto.createHash("sha256")
    .update("tilefinch-browser-callback-order-v1\0");
  let exactOrderEntries = 0;
  let semanticOrderEntries = 0;
  let rawCallbackEntries = 0;
  let generation = 0;
  let enqueued = 0;
  let completed = 0;
  let maxPending = 0;
  const inFlight = new Set();
  let batches = 0;
  let semanticPumps = 0;
  let fastForwardedPumps = 0;
  let pumpWorkUnits = 0;
  let retainedDelayWorkUnits = 0;
  let terminalDelayWorkUnits = 0;
  let retainedAdmissions = 0;
  let terminalAdmissions = 0;
  let failures = 0;
  let overflow = false;
  let terminalFailure = null;
  let terminalFailures = 0;
  let rejectedAfterTerminal = 0;
  let controlTail = Promise.resolve();
  let pendingControls = 0;

  const pending = () => staged.length + preparing.size
    + admitted.length + inFlight.size;
  const observeActivity = () => {
    const value = activity();
    if (!Number.isSafeInteger(value) || value < 0) {
      throw new CaptureError("response scheduler activity counter is invalid");
    }
    return value;
  };
  const identityKey = (identity) => {
    if (!identity || typeof identity !== "object" || Array.isArray(identity)
        || typeof identity.routeKey !== "string" || identity.routeKey.length === 0
        || identity.routeKey.length > MAX_METADATA_BYTES + 65
        || typeof identity.resourceType !== "string"
        || identity.resourceType.length === 0 || identity.resourceType.length > 64
        || !Number.isSafeInteger(identity.occurrence) || identity.occurrence < 0
        || !Number.isSafeInteger(identity.browserOrdinal) || identity.browserOrdinal < 0) {
      throw new CaptureError("response scheduler request identity is invalid");
    }
    return JSON.stringify([
      identity.routeKey, identity.resourceType, identity.occurrence,
      identity.browserOrdinal,
    ]);
  };
  const compareItems = (left, right) =>
    compareCodeUnitStrings(left.identity.routeKey, right.identity.routeKey)
    || left.identity.occurrence - right.identity.occurrence
    || compareCodeUnitStrings(
      left.identity.resourceType, right.identity.resourceType,
    )
    || left.browserOrdinal - right.browserOrdinal;
  const completion = (item, disposition) => JSON.stringify([
    item.generation, item.identity.routeKey, item.identity.resourceType,
    item.identity.occurrence, item.identity.browserOrdinal,
    item.identityKey, item.admissionKind, item.recordId, disposition,
  ]);
  const semanticCompletion = (item, disposition) => JSON.stringify([
    item.generation, item.identity.routeKey, item.identity.resourceType,
    item.identity.occurrence, item.admissionKind, item.recordId, disposition,
  ]);
  const appendHashEntry = (hash, entries, value) => {
    if (entries !== 0) hash.update("\n");
    hash.update(value);
    return entries + 1;
  };
  const recordCompletion = (item, disposition) => {
    exactOrderEntries = appendHashEntry(
      exactOrderHash, exactOrderEntries, completion(item, disposition),
    );
    semanticOrderEntries = appendHashEntry(
      semanticOrderHash, semanticOrderEntries,
      semanticCompletion(item, disposition),
    );
  };
  const rejectItem = (item, error, disposition = "failed") => {
    if (item.settled) return false;
    item.settled = true;
    failures += 1;
    completed += 1;
    recordCompletion(item, disposition);
    item.reject(error);
    return true;
  };
  const resolveItem = (item, value, disposition) => {
    if (item.settled) return false;
    item.settled = true;
    completed += 1;
    recordCompletion(item, disposition);
    item.resolve(value);
    return true;
  };
  const failPending = (error) => {
    const waiting = new Set([
      ...staged, ...preparing, ...admitted, ...inFlight,
    ]);
    staged = [];
    admitted.length = 0;
    for (const item of waiting) rejectItem(item, error, "scheduler-failed");
  };
  const terminalSchedulerFailure = (error, label) => {
    overflow = true;
    const failure = error instanceof CaptureError ? error : new CaptureError(
      `response scheduler ${label} failed: ${error && error.message
        ? error.message : String(error)}`,
    );
    if (terminalFailure === null) {
      terminalFailure = failure;
      terminalFailures += 1;
      failPending(failure);
      try { onTerminalFailure(failure, String(label)); } catch (_error) {}
    }
    return terminalFailure;
  };
  const checkCounterAddition = (current, addition, bound, label) => {
    if (!Number.isSafeInteger(addition) || addition < 0
        || current > bound - addition) {
      const error = new CaptureError(`response scheduler exceeded its ${label} bound`);
      throw terminalSchedulerFailure(error, label);
    }
    return current + addition;
  };

  const runAdmissionProbe = async () => {
    let timer = null;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new CaptureError(
        "response scheduler quiescence probe timed out",
      )), probeTimeoutMs);
    });
    try {
      await Promise.race([Promise.resolve().then(() => probe()), timeout]);
    } catch (error) {
      throw terminalSchedulerFailure(error, "quiescence probe");
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
  };

  const runPreparation = async (item) => {
    /* Invoke synchronous admission preparation in the same turn as v7 did;
       only thenables need a host deadline. This keeps valid replay generation
       and claim timing byte-compatible while closing the async hang. */
    const prepared = item.prepare();
    if (!prepared || typeof prepared.then !== "function") return prepared;
    let timer = null;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new CaptureError(
        "response scheduler admission preparation timed out",
      )), prepareTimeoutMs);
    });
    try {
      return await Promise.race([Promise.resolve(prepared), timeout]);
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
  };

  const closeAdmissionGenerationInternal = async () => {
    if (terminalFailure !== null) throw terminalFailure;
    let previousEnqueued = enqueued;
    let previousActivity;
    try { previousActivity = observeActivity(); }
    catch (error) { throw terminalSchedulerFailure(error, "activity observation"); }
    let stable = 0;
    for (let pass = 0; pass < maxAdmissionProbes; pass += 1) {
      await runAdmissionProbe();
      let currentActivity;
      try { currentActivity = observeActivity(); }
      catch (error) { throw terminalSchedulerFailure(error, "activity observation"); }
      if (previousEnqueued === enqueued && previousActivity === currentActivity) {
        stable += 1;
      } else {
        stable = 0;
        previousEnqueued = enqueued;
        previousActivity = currentActivity;
      }
      if (stable === 2) break;
    }
    if (stable !== 2) {
      const error = new CaptureError(
        "response scheduler admission generation did not reach bounded quiescence",
      );
      throw terminalSchedulerFailure(error, "admission quiescence");
    }
    if (staged.length === 0) return 0;
    const closing = staged;
    staged = [];
    generation += 1;
    closing.sort(compareItems);
    for (const item of closing) {
      item.generation = generation;
      preparing.add(item);
    }
    maxPending = Math.max(maxPending, pending());
    for (const item of closing) {
      if (item.settled) {
        preparing.delete(item);
        continue;
      }
      if (terminalFailure !== null) {
        rejectItem(item, terminalFailure, "scheduler-failed");
        preparing.delete(item);
        continue;
      }
      try {
        const prepared = await runPreparation(item);
        preparing.delete(item);
        if (item.settled) continue;
        if (terminalFailure !== null) {
          rejectItem(item, terminalFailure, "scheduler-failed");
          continue;
        }
        if (!prepared || typeof prepared !== "object"
            || !prepared.record || typeof prepared.work !== "function") {
          throw new CaptureError("response scheduler admission is incomplete");
        }
        const delay = Number(prepared.record.asyncDelayPumps || 0);
        if (!Number.isSafeInteger(delay) || delay < 0
            || delay > MAX_RETAINED_DELAY_PUMPS) {
          throw new CaptureError("response scheduler retained delay is invalid");
        }
        item.recordId = String(prepared.record.id);
        if (!/^\d{4}$/.test(item.recordId)) {
          throw new CaptureError("response scheduler record id is invalid");
        }
        item.delay = Math.max(1, delay);
        item.admissionKind = prepared.admissionKind === undefined
          ? "retained" : String(prepared.admissionKind);
        if (item.admissionKind !== "retained"
            && item.admissionKind !== "terminal") {
          throw new CaptureError("response scheduler admission kind is invalid");
        }
        const scheduledDelayWorkUnits = retainedDelayWorkUnits
          + terminalDelayWorkUnits;
        if (scheduledDelayWorkUnits > maxWorkUnits - item.delay) {
          overflow = true;
          throw new CaptureError("response scheduler exceeded its delay work bound");
        }
        if (item.admissionKind === "retained") {
          retainedAdmissions += 1;
          retainedDelayWorkUnits += item.delay;
        } else {
          terminalAdmissions += 1;
          terminalDelayWorkUnits += item.delay;
        }
        item.remainingPumps = item.delay;
        item.work = prepared.work;
        admitted.push(item);
      } catch (error) {
        preparing.delete(item);
        const failure = terminalSchedulerFailure(error, "admission preparation");
        rejectItem(item, failure, "admission-failed");
      }
    }
    maxPending = Math.max(maxPending, pending());
    if (terminalFailure !== null) throw terminalFailure;
    return closing.length;
  };

  const pumpOnceInternal = async ({ fastForward = false } = {}) => {
    await closeAdmissionGenerationInternal();
    if (admitted.length === 0) return 0;
    if (fastForward) {
      const skip = Math.max(0,
        Math.min(...admitted.map((item) => item.remainingPumps)) - 1,
      );
      if (skip > 0) {
        semanticPumps = checkCounterAddition(
          semanticPumps, skip, maxPumps, "semantic pump",
        );
        fastForwardedPumps = checkCounterAddition(
          fastForwardedPumps, skip, maxPumps, "fast-forward pump",
        );
        pumpWorkUnits = checkCounterAddition(
          pumpWorkUnits, admitted.length * skip, maxWorkUnits, "pump work",
        );
        for (const item of admitted) item.remainingPumps -= skip;
      }
    }
    semanticPumps = checkCounterAddition(
      semanticPumps, 1, maxPumps, "semantic pump",
    );
    pumpWorkUnits = checkCounterAddition(
      pumpWorkUnits, admitted.length, maxWorkUnits, "pump work",
    );
    for (const item of admitted) item.remainingPumps -= 1;
    const batch = admitted.filter((item) => item.remainingPumps === 0)
      .sort((left, right) => left.generation - right.generation
        || Number(left.recordId) - Number(right.recordId)
        || compareItems(left, right));
    if (batch.length === 0) return 0;
    const due = new Set(batch);
    for (let index = admitted.length - 1; index >= 0; index -= 1) {
      if (due.has(admitted[index])) admitted.splice(index, 1);
    }
    for (const item of batch) inFlight.add(item);
    batches += 1;
    for (const item of batch) {
      try {
        if (terminalFailure !== null) {
          rejectItem(item, terminalFailure, "scheduler-failed");
          continue;
        }
        const value = await item.work();
        const disposition = typeof value === "string" ? value : "completed";
        if (terminalFailure !== null) {
          rejectItem(item, terminalFailure, "scheduler-failed");
        } else {
          resolveItem(item, value, disposition);
        }
      } catch (error) {
        const failure = terminalSchedulerFailure(error, "delivery work");
        rejectItem(item, failure);
      } finally {
        inFlight.delete(item);
      }
    }
    if (terminalFailure !== null) throw terminalFailure;
    return batch.length;
  };

  const driveToIdleInternal = async () => {
    for (let step = 0; step < maxDriveSteps; step += 1) {
      await closeAdmissionGenerationInternal();
      if (admitted.length === 0) return;
      await pumpOnceInternal({ fastForward: true });
    }
    const error = new CaptureError("response scheduler exceeded its idle drive bound");
    throw terminalSchedulerFailure(error, "idle drive");
  };

  const driveUntilSettledInternal = async (operation, label = "operation") => {
    if (!operation || typeof operation.then !== "function") {
      throw new CaptureError("response scheduler drive requires a promise");
    }
    let settled = false;
    let result;
    let operationError;
    Promise.resolve(operation).then(
      (value) => { settled = true; result = value; },
      (error) => { settled = true; operationError = error; },
    );
    for (let step = 0; step < maxDriveSteps && !settled; step += 1) {
      await closeAdmissionGenerationInternal();
      if (admitted.length !== 0) {
        await pumpOnceInternal({ fastForward: true });
      } else {
        await Promise.resolve();
      }
    }
    if (!settled) {
      const error = new CaptureError(
        `response scheduler exceeded its ${label} drive bound`,
      );
      throw terminalSchedulerFailure(error, `${label} drive`);
    }
    if (operationError !== undefined) throw operationError;
    return result;
  };

  /* Public control operations share one promise lane. This preserves the
     synchronous enqueue surface while preventing close/pump/drive callers
     from concurrently mutating staged, preparing, and admitted queues. */
  const serializeControl = (operation) => {
    if (pendingControls >= maxDriveSteps) {
      return Promise.reject(terminalSchedulerFailure(
        new CaptureError("response scheduler exceeded its control queue bound"),
        "control queue",
      ));
    }
    pendingControls += 1;
    const current = controlTail.then(operation, operation);
    controlTail = current.catch(() => {});
    current.then(
      () => { pendingControls -= 1; },
      () => { pendingControls -= 1; },
    );
    return current;
  };

  return {
    enqueue(record, identity, work) {
      return this.enqueueAdmission(identity, () => ({ record, work }));
    },
    enqueueAdmission(identity, prepare) {
      if (typeof prepare !== "function") {
        return Promise.reject(new CaptureError("response scheduler admission is invalid"));
      }
      if (terminalFailure !== null) {
        rejectedAfterTerminal += 1;
        return Promise.reject(terminalFailure);
      }
      const key = identityKey(identity);
      enqueued += 1;
      if (enqueued > limit) {
        return Promise.reject(terminalSchedulerFailure(
          new CaptureError("response scheduler exceeded its request bound"),
          "request admission",
        ));
      }
      return new Promise((resolve, reject) => {
        rawCallbackEntries = appendHashEntry(
          rawCallbackHash, rawCallbackEntries, JSON.stringify([
            identity.browserOrdinal, identity.routeKey,
            identity.resourceType, identity.occurrence,
          ]),
        );
        staged.push({
          identity: { ...identity }, identityKey: key,
          browserOrdinal: identity.browserOrdinal,
          generation: 0, admissionKind: "retained", recordId: "0000",
          prepare, work: null,
          delay: 0, remainingPumps: 0, settled: false, resolve, reject,
        });
        maxPending = Math.max(maxPending, pending());
      });
    },
    closeAdmissionGeneration() {
      return serializeControl(closeAdmissionGenerationInternal);
    },
    pumpOnce(options) {
      return serializeControl(() => pumpOnceInternal(options));
    },
    driveUntilSettled(operation, label = "operation") {
      return serializeControl(
        () => driveUntilSettledInternal(operation, label),
      );
    },
    driveToIdle() {
      return serializeControl(driveToIdleInternal);
    },
    failTerminal(error, label = "external") {
      return terminalSchedulerFailure(error, label);
    },
    isTerminal() { return terminalFailure !== null; },
    whenIdle() {
      return serializeControl(async () => {
        if (terminalFailure !== null) throw terminalFailure;
        if (pending() === 0) return;
        await driveToIdleInternal();
      });
    },
    summary() {
      return {
        version: RESPONSE_SCHEDULER_VERSION,
        terminal_boundary: RESPONSE_SCHEDULER_TERMINAL_BOUNDARY,
        ordering: "semantic-pump,admission-generation,record-id,exact-request-identity",
        admission_ordering:
          "exact-route-key,global-route-occurrence,resource-type,playwright-callback-ordinal",
        admission_probe: "bounded-playwright-context-roundtrip-quiescence",
        order_digest_version: "exact-request-identity-v3",
        browser_ordinal_semantics: "raw-playwright-route-callback-v1",
        request_limit: limit,
        retained_delay_limit_pumps: MAX_RETAINED_DELAY_PUMPS,
        semantic_pump_limit: maxPumps,
        pump_work_limit: maxWorkUnits,
        drive_step_limit: maxDriveSteps,
        pump_work_units: pumpWorkUnits,
        scheduled_delay_work_units:
          retainedDelayWorkUnits + terminalDelayWorkUnits,
        retained_delay_work_units: retainedDelayWorkUnits,
        terminal_delay_work_units: terminalDelayWorkUnits,
        retained_admissions: retainedAdmissions,
        terminal_admissions: terminalAdmissions,
        batches,
        generations: generation, semantic_pumps: semanticPumps,
        fast_forwarded_pumps: fastForwardedPumps,
        admission_probe_stable_passes: 2,
        admission_probe_limit: maxAdmissionProbes,
        admission_probe_timeout_ms: probeTimeoutMs,
        enqueued, completed, pending: pending(),
        max_pending: maxPending, overflow,
        failures, terminal_failures: terminalFailures,
        rejected_after_terminal: rejectedAfterTerminal,
        exact_order_count: exactOrderEntries,
        semantic_delivery_count: semanticOrderEntries,
        raw_callback_count: rawCallbackEntries,
        ready: !overflow && failures === 0
          && terminalFailures === 0 && rejectedAfterTerminal === 0
          && completed === enqueued && pending() === 0
          && exactOrderEntries === completed
          && semanticOrderEntries === completed
          && rawCallbackEntries === enqueued
          && retainedAdmissions + terminalAdmissions === enqueued
          && pumpWorkUnits
            === retainedDelayWorkUnits + terminalDelayWorkUnits,
        order_sha256: exactOrderHash.copy().digest("hex"),
        semantic_delivery_order_sha256:
          semanticOrderHash.copy().digest("hex"),
        raw_callback_arrival_sha256:
          rawCallbackHash.copy().digest("hex"),
      };
    },
  };
}

function cookieDefaultPath(parsedUrl) {
  const pathname = parsedUrl.pathname;
  if (!pathname || pathname[0] !== "/" || pathname === "/") return "/";
  const lastSlash = pathname.lastIndexOf("/");
  return lastSlash <= 0 ? "/" : pathname.slice(0, lastSlash);
}

function validCookieDomain(value) {
  if (value.length === 0 || value.length > 253 || value.endsWith(".")) return false;
  return value.split(".").every((label) => label.length > 0 && label.length <= 63
    && /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label));
}

function parseCookieDate(value, label = "Expires") {
  const source = value.trim();
  let match = /^([A-Za-z]+),\s*(\d{1,2}) ([A-Za-z]{3}) (\d{4}) (\d{2}):(\d{2}):(\d{2}) GMT$/i.exec(source);
  if (!match) {
    match = /^([A-Za-z]+),\s*(\d{1,2})-([A-Za-z]{3})-(\d{2}|\d{4}) (\d{2}):(\d{2}):(\d{2}) GMT$/i.exec(source);
  }
  let weekday;
  let day;
  let monthName;
  let yearText;
  let hour;
  let minute;
  let second;
  if (match) {
    [, weekday, day, monthName, yearText, hour, minute, second] = match;
  } else {
    match = /^([A-Za-z]+) ([A-Za-z]{3})\s+(\d{1,2}) (\d{2}):(\d{2}):(\d{2}) (\d{4})$/i.exec(source);
    if (!match) throw new CaptureError(`${label}: unsupported or invalid Expires date`);
    [, weekday, monthName, day, hour, minute, second, yearText] = match;
  }
  if (!COOKIE_WEEKDAYS.has(weekday.toLowerCase())) {
    throw new CaptureError(`${label}: invalid Expires weekday`);
  }
  const month = COOKIE_MONTHS.get(monthName.toLowerCase());
  let year = Number(yearText);
  if (yearText.length === 2) year += year >= 70 ? 1900 : 2000;
  const numericDay = Number(day);
  const numericHour = Number(hour);
  const numericMinute = Number(minute);
  const numericSecond = Number(second);
  if (month === undefined || year < 1601 || year > 9999
      || numericDay < 1 || numericDay > 31 || numericHour > 23
      || numericMinute > 59 || numericSecond > 59) {
    throw new CaptureError(`${label}: invalid Expires date component`);
  }
  const milliseconds = Date.UTC(
    year, month, numericDay, numericHour, numericMinute, numericSecond,
  );
  const verified = new Date(milliseconds);
  if (verified.getUTCFullYear() !== year || verified.getUTCMonth() !== month
      || verified.getUTCDate() !== numericDay || verified.getUTCHours() !== numericHour
      || verified.getUTCMinutes() !== numericMinute
      || verified.getUTCSeconds() !== numericSecond) {
    throw new CaptureError(`${label}: invalid Expires calendar date`);
  }
  return milliseconds / 1000;
}

function parseSetCookie(value, sourceUrl, label = "Set-Cookie") {
  if (typeof value !== "string" || Buffer.byteLength(value, "utf8") > MAX_SET_COOKIE_BYTES
      || /[^\x20-\x7e]/.test(value)) {
    throw new CaptureError(`${label}: cookie exceeds its bound or contains non-ASCII bytes`);
  }
  const normalizedSource = normalizeUrl(sourceUrl, label);
  const parsedSource = new URL(normalizedSource);
  const fields = value.split(";");
  const pair = fields.shift().trim();
  const equals = pair.indexOf("=");
  if (equals <= 0) throw new CaptureError(`${label}: malformed cookie pair`);
  const name = pair.slice(0, equals).trim();
  const cookieValue = pair.slice(equals + 1).trim();
  if (!COOKIE_NAME.test(name)) throw new CaptureError(`${label}: invalid cookie name`);
  const quoted = cookieValue.length >= 2 && cookieValue.startsWith('"')
    && cookieValue.endsWith('"');
  const valueBody = quoted ? cookieValue.slice(1, -1) : cookieValue;
  if ((!quoted && (cookieValue.startsWith('"') || cookieValue.endsWith('"')))
      || !COOKIE_VALUE.test(valueBody)) {
    throw new CaptureError(`${label}: invalid cookie value`);
  }

  let domainAttribute = null;
  let cookiePath = cookieDefaultPath(parsedSource);
  let pathAttributePresent = false;
  let secure = false;
  let httpOnly = false;
  let sameSite = null;
  let maxAge = null;
  let expires = null;
  const seen = new Set();
  for (let index = 0; index < fields.length; index += 1) {
    const field = fields[index].trim();
    if (!field) {
      if (index === fields.length - 1) continue;
      throw new CaptureError(`${label}: empty cookie attribute`);
    }
    const attributeEquals = field.indexOf("=");
    const attributeName = (attributeEquals < 0 ? field : field.slice(0, attributeEquals)).trim();
    const attributeValue = attributeEquals < 0 ? null : field.slice(attributeEquals + 1).trim();
    const lower = attributeName.toLowerCase();
    if (!SAFE_HEADER.test(attributeName)) {
      throw new CaptureError(`${label}: invalid ${attributeName || "attribute"}`);
    }
    /* Real servers can repeat attributes with mixed casing; browsers apply
       last-wins, so tolerate duplicates the
       same way instead of rejecting the trace. */
    seen.add(lower);
    if (lower === "secure" || lower === "httponly") {
      if (attributeEquals >= 0) throw new CaptureError(`${label}: ${attributeName} must be a flag`);
      if (lower === "secure") secure = true; else httpOnly = true;
    } else if (lower === "domain") {
      if (attributeValue === null || attributeValue.length === 0) {
        throw new CaptureError(`${label}: Domain requires a value`);
      }
      let domain = attributeValue.toLowerCase();
      if (domain.startsWith(".")) domain = domain.slice(1);
      if (!validCookieDomain(domain) || net.isIP(domain) !== 0
          || !(parsedSource.hostname === domain || parsedSource.hostname.endsWith(`.${domain}`))) {
        throw new CaptureError(`${label}: Domain does not domain-match its response URL`);
      }
      domainAttribute = domain;
    } else if (lower === "path") {
      if (attributeValue === null || !attributeValue.startsWith("/")
          || /[^\x20-\x7e]|;/.test(attributeValue)) {
        throw new CaptureError(`${label}: invalid Path`);
      }
      cookiePath = attributeValue;
      pathAttributePresent = true;
    } else if (lower === "samesite") {
      if (attributeValue === null) throw new CaptureError(`${label}: SameSite requires a value`);
      const canonical = { strict: "Strict", lax: "Lax", none: "None" }[attributeValue.toLowerCase()];
      if (!canonical) throw new CaptureError(`${label}: invalid SameSite value`);
      sameSite = canonical;
    } else if (lower === "max-age") {
      if (attributeValue === null || !/^-?\d+$/.test(attributeValue)) {
        throw new CaptureError(`${label}: invalid Max-Age`);
      }
      maxAge = Number(attributeValue);
      if (!Number.isSafeInteger(maxAge)) throw new CaptureError(`${label}: Max-Age is outside the safe range`);
    } else if (lower === "expires") {
      if (attributeValue === null) throw new CaptureError(`${label}: Expires requires a value`);
      expires = parseCookieDate(attributeValue, label);
    } else {
      throw new CaptureError(`${label}: unsupported cookie attribute ${attributeName}`);
    }
  }
  if (secure && parsedSource.protocol !== "https:") {
    throw new CaptureError(`${label}: Secure cookie came from a non-HTTPS URL`);
  }
  if (sameSite === "None" && !secure) {
    throw new CaptureError(`${label}: SameSite=None requires Secure`);
  }
  if (name.startsWith("__Secure-") && (!secure || parsedSource.protocol !== "https:")) {
    throw new CaptureError(`${label}: invalid __Secure- cookie scope`);
  }
  if (name.startsWith("__Host-") && (!secure || domainAttribute !== null
      || !pathAttributePresent || cookiePath !== "/" || parsedSource.protocol !== "https:")) {
    throw new CaptureError(`${label}: invalid __Host- cookie scope`);
  }
  return {
    name, value: cookieValue, sourceUrl: normalizedSource,
    domain: domainAttribute === null ? parsedSource.hostname : `.${domainAttribute}`,
    path: cookiePath, hostOnly: domainAttribute === null,
    secure, httpOnly, sameSite, maxAge, expires,
  };
}

function responseCookies(metadata, filename) {
  const declared = metadataInteger(metadata, "set-cookie-count", filename);
  if (declared < 0 || declared > MAX_RESPONSE_COOKIES) {
    throw new CaptureError(`${filename}: response cookie count is outside 0..${MAX_RESPONSE_COOKIES}`);
  }
  const mode = metadata["cookie-values"];
  if (mode !== undefined && mode !== "redacted" && mode !== "raw") {
    throw new CaptureError(`${filename}: invalid cookie-values mode`);
  }
  if (declared > 0 && mode === undefined) {
    throw new CaptureError(`${filename}: response cookies require cookie-values metadata`);
  }
  const values = new Map();
  const urls = new Map();
  for (const [key, value] of Object.entries(metadata)) {
    if (key === "set-cookie-count") continue;
    let match = /^set-cookie-(\d+)$/.exec(key);
    let target = values;
    if (!match) {
      match = /^set-cookie-url-(\d+)$/.exec(key);
      target = urls;
    }
    if (!match) {
      if (key.startsWith("set-cookie-") || key.startsWith("set-cookie-url-")) {
        throw new CaptureError(`${filename}: invalid response cookie index`);
      }
      continue;
    }
    const index = Number(match[1]);
    if (!Number.isSafeInteger(index) || match[1] !== String(index)
        || index >= MAX_RESPONSE_COOKIES || target.has(index)) {
      throw new CaptureError(`${filename}: invalid response cookie index`);
    }
    target.set(index, value);
  }
  if (values.size !== declared || urls.size !== declared) {
    throw new CaptureError(`${filename}: response cookie count mismatch`);
  }
  const cookies = [];
  for (let index = 0; index < declared; index += 1) {
    if (!values.has(index) || !urls.has(index)) {
      throw new CaptureError(`${filename}: response cookie sequence has a gap at ${index}`);
    }
    const cookie = parseSetCookie(
      values.get(index), urls.get(index), `${filename}: set-cookie-${index}`,
    );
    if (mode === "redacted") {
      const quoted = cookie.value.length >= 2 && cookie.value.startsWith('"')
        && cookie.value.endsWith('"');
      const redactedValue = quoted ? cookie.value.slice(1, -1) : cookie.value;
      if (!/^x*$/.test(redactedValue)) {
        throw new CaptureError(`${filename}: set-cookie-${index} is not deterministically redacted`);
      }
    }
    cookie.valueMode = mode;
    cookies.push(cookie);
  }
  return cookies;
}

function traceInfo(traceDirectory, expectedRecordCount = null) {
  const filename = path.join(traceDirectory, "trace.meta");
  const metadata = parseMetadata(filename);
  if (metadata["capture-complete"] !== "yes") {
    throw new CaptureError(`${filename}: capture-complete must be exactly yes`);
  }
  const recordCount = metadataInteger(metadata, "record-count", filename);
  if (recordCount < 1 || recordCount > MAX_TRACE_RECORDS
      || (expectedRecordCount !== null && recordCount !== expectedRecordCount)) {
    throw new CaptureError(`${filename}: record-count does not match the exact retained sequence`);
  }
  if (metadata["psp-http-trace-clock"] !== "1") {
    throw new CaptureError(`${filename}: invalid replay clock version`);
  }
  const rawOrigin = metadata["origin-ms"];
  const origin = Number(rawOrigin);
  if (!/^\d+$/.test(String(rawOrigin || "")) || !Number.isSafeInteger(origin)
      || origin <= 0 || origin > MAX_DATE_MS) {
    throw new CaptureError(`${filename}: invalid replay clock origin`);
  }
  return { originMs: origin, recordCount };
}

function traceOriginMs(traceDirectory) {
  return traceInfo(traceDirectory).originMs;
}

function cookieReplayOperation(cookie, nowSeconds, originMs, responseDateSeconds = null) {
  if (!Number.isFinite(nowSeconds) || nowSeconds <= 0) {
    throw new CaptureError("cookie replay clock is invalid");
  }
  let replayExpires = null;
  let remove = false;
  if (cookie.maxAge !== null) {
    if (cookie.maxAge <= 0) remove = true;
    else replayExpires = nowSeconds + cookie.maxAge;
  } else if (cookie.expires !== null) {
    const baseline = responseDateSeconds === null
      ? (Number.isSafeInteger(originMs) && originMs > 0 ? originMs / 1000 : null)
      : responseDateSeconds;
    if (!Number.isFinite(baseline) || baseline <= 0) {
      throw new CaptureError("Expires cookie requires a retained response Date or trace origin-ms");
    }
    const retainedTtl = cookie.expires - baseline;
    if (retainedTtl <= 0) remove = true;
    else replayExpires = nowSeconds + retainedTtl;
  }
  const scope = { name: cookie.name, domain: cookie.domain, path: cookie.path };
  if (remove) return { action: "delete", scope };
  if (replayExpires !== null
      && (!Number.isFinite(replayExpires) || replayExpires > MAX_COOKIE_EXPIRY_SECONDS)) {
    throw new CaptureError("cookie replay expiry is outside the browser range");
  }
  const replay = {
    name: cookie.name, value: cookie.value, domain: cookie.domain, path: cookie.path,
    httpOnly: cookie.httpOnly, secure: cookie.secure,
  };
  if (cookie.sameSite !== null) replay.sameSite = cookie.sameSite;
  if (replayExpires !== null) replay.expires = replayExpires;
  return { action: "add", cookie: replay };
}

async function applyResponseCookies(
  context, cookies, nowSeconds, originMs, responseDateSeconds = null,
) {
  for (const cookie of cookies) {
    const operation = cookieReplayOperation(
      cookie, nowSeconds, originMs, responseDateSeconds,
    );
    if (operation.action === "add") {
      if (typeof context.addCookies !== "function") {
        throw new CaptureError("offline capture requires BrowserContext.addCookies");
      }
      await context.addCookies([operation.cookie]);
    } else {
      if (typeof context.clearCookies !== "function") {
        throw new CaptureError("offline capture requires filtered BrowserContext.clearCookies");
      }
      await context.clearCookies(operation.scope);
    }
  }
}

function fnv1a64(body) {
  let value = 0xcbf29ce484222325n;
  for (const byte of body) {
    value ^= BigInt(byte);
    value = BigInt.asUintN(64, value * 0x100000001b3n);
  }
  return value.toString(16).padStart(16, "0");
}

function responseHeaders(metadata, filename) {
  const declared = metadataInteger(metadata, "response-header-count", filename);
  if (declared < 0 || declared > MAX_RESPONSE_HEADERS) {
    throw new CaptureError(`${filename}: response header count is outside 0..${MAX_RESPONSE_HEADERS}`);
  }
  const indexed = [];
  for (const [key, value] of Object.entries(metadata)) {
    const match = /^response-header-(\d+)$/.exec(key);
    if (match) {
      const number = Number(match[1]);
      if (!Number.isSafeInteger(number) || number >= MAX_RESPONSE_HEADERS) {
        throw new CaptureError(`${filename}: response header index is outside the supported range`);
      }
      indexed.push([number, value]);
    }
  }
  indexed.sort((a, b) => a[0] - b[0]);
  if (declared !== indexed.length) {
    throw new CaptureError(`${filename}: response header count mismatch`);
  }
  const headers = [];
  for (let i = 0; i < indexed.length; i += 1) {
    const [number, line] = indexed[i];
    if (number !== i || !line.includes(":")) {
      throw new CaptureError(`${filename}: malformed response-header-${number}`);
    }
    const colon = line.indexOf(":");
    const name = line.slice(0, colon).trim();
    const value = line.slice(colon + 1).trim();
    if (!SAFE_HEADER.test(name) || /[\r\n]/.test(value)) {
      throw new CaptureError(`${filename}: unsafe response-header-${number}`);
    }
    const lower = name.toLowerCase();
    if (lower === "set-cookie" || lower === "set-cookie2") {
      throw new CaptureError(`${filename}: response cookies must use structured numbered fields`);
    }
    if (!STRIPPED_HEADERS.has(lower)) headers.push([name, value]);
  }
  return headers;
}

function responseCookieDateBaseline(headers, cookies, filename) {
  if (!cookies.some((cookie) => cookie.expires !== null && cookie.maxAge === null)) {
    return null;
  }
  const dates = headers.filter(([name]) => name.toLowerCase() === "date")
    .map(([, value]) => value);
  if (dates.length > 1) {
    throw new CaptureError(`${filename}: multiple response Date headers are ambiguous for cookie expiry`);
  }
  if (dates.length === 0) return null;
  try {
    return parseCookieDate(dates[0], `${filename}: response Date`);
  } catch (error) {
    throw new CaptureError(`${filename}: malformed response Date for cookie expiry: ${error.message}`);
  }
}

function loadRecord(metaPath) {
  const metadata = parseMetadata(metaPath);
  const method = String(metadata.method || "").toUpperCase();
  if (!SAFE_HEADER.test(method)) {
    throw new CaptureError(`${metaPath}: missing or invalid method`);
  }
  const url = normalizeUrl(metadata.url || "", metaPath);
  const status = metadataInteger(metadata, "status", metaPath);
  if (status !== 0 && (status < 100 || status > 599)) {
    throw new CaptureError(`${metaPath}: status is outside 100..599`);
  }
  const expectedLength = metadataInteger(metadata, "length", metaPath);
  if (expectedLength < 0 || expectedLength > MAX_RECORD_BYTES) {
    throw new CaptureError(`${metaPath}: length is outside 0..${MAX_RECORD_BYTES}`);
  }
  const bodyPath = metaPath.replace(/\.meta$/, ".body");
  let body = Buffer.alloc(0);
  if (fs.existsSync(bodyPath)) body = fs.readFileSync(bodyPath);
  else if (expectedLength !== 0) {
    throw new CaptureError(`${metaPath}: missing ${path.basename(bodyPath)}`);
  }
  if (body.length !== expectedLength) {
    throw new CaptureError(`${metaPath}: declared length does not match body`);
  }
  if (metadata["response-body-hash"] !== undefined
      && metadata["response-body-hash"] !== fnv1a64(body)) {
    throw new CaptureError(`${metaPath}: response body hash mismatch`);
  }
  if (metadata.success !== "0" && metadata.success !== "1") {
    throw new CaptureError(`${metaPath}: invalid success flag`);
  }
  const contentType = String(metadata["content-type"] || "").trim();
  if (/[\r\n]/.test(contentType)) {
    throw new CaptureError(`${metaPath}: unsafe content type`);
  }
  const headers = responseHeaders(metadata, metaPath);
  const effectiveUrl = metadata["effective-url"]
    ? normalizeUrl(metadata["effective-url"], metaPath) : url;
  if (effectiveUrl !== url) {
    throw new CaptureError(`${metaPath}: collapsed redirect/effective URL cannot be replayed faithfully`);
  }
  const cookies = responseCookies(metadata, metaPath);
  const responseDateSeconds = responseCookieDateBaseline(headers, cookies, metaPath);
  const asyncDelayPumps = metadata["async-delay-pumps"] === undefined
    ? 0 : metadataInteger(metadata, "async-delay-pumps", metaPath);
  if (asyncDelayPumps < 0 || asyncDelayPumps > 1000000) {
    throw new CaptureError(`${metaPath}: async-delay-pumps is outside its bound`);
  }
  const externalCancel = metadata["external-cancel"] === undefined
    ? false : metadata["external-cancel"] === "1";
  const transportTimeout = metadata["transport-timeout"] === undefined
    ? false : metadata["transport-timeout"] === "1";
  if ((metadata["external-cancel"] !== undefined
       && !["0", "1"].includes(metadata["external-cancel"]))
      || (metadata["transport-timeout"] !== undefined
          && !["0", "1"].includes(metadata["transport-timeout"]))) {
    throw new CaptureError(`${metaPath}: invalid retained cancellation semantics`);
  }
  if (metadata.success === "1"
      && (status === 0 || externalCancel || transportTimeout)) {
    throw new CaptureError(`${metaPath}: successful response has failure-only transport state`);
  }
  const signature = crypto.createHash("sha256").update(JSON.stringify({
    success: metadata.success === "1", status, contentType,
    headers, body_sha256: crypto.createHash("sha256").update(body).digest("hex"),
    cookies, responseDateSeconds, asyncDelayPumps, externalCancel,
    transportTimeout, error: String(metadata.error || ""),
  })).digest("hex");
  return {
    id: path.basename(metaPath, ".meta"), method, url,
    success: metadata.success === "1", status, contentType,
    headers, body, cookies, responseDateSeconds, asyncDelayPumps,
    externalCancel, transportTimeout, signature,
  };
}

function selectRoute(records) {
  const rank = (record) => record.success && record.status !== 0
    ? 2 : (record.status !== 0 ? 1 : 0);
  const selectedRank = records.reduce(
    (maximum, record) => Math.max(maximum, rank(record)), -1,
  );
  const candidates = records.filter((record) => rank(record) === selectedRank)
    .sort((left, right) => Number(left.id) - Number(right.id));
  const signatures = new Set(candidates.map((record) => record.signature));
  return {
    version: ROUTE_SELECTION_VERSION,
    rank: selectedRank,
    mode: candidates.length === 1 ? "reusable" : "occurrence-sequence",
    selected: candidates.length === 1 ? candidates[0] : null,
    candidates,
    ambiguous: signatures.size > 1,
  };
}

function claimRouteRecord(route, occurrences, key) {
  if (!route || route.candidates.length === 0) {
    return { record: null, exhausted: false, reason: "unclaimable-route" };
  }
  if (route.mode === "reusable") {
    return { record: route.candidates[0], exhausted: false, occurrence: 0 };
  }
  const occurrence = occurrences.get(key) || 0;
  if (occurrence >= route.candidates.length) {
    return { record: null, exhausted: true, occurrence };
  }
  occurrences.set(key, occurrence + 1);
  return { record: route.candidates[occurrence], exhausted: false, occurrence };
}

function retainedFailureAbortCode(record) {
  if (!record || record.success) {
    throw new CaptureError("retained failure abort requires a failed trace record");
  }
  if (record.externalCancel) return "aborted";
  if (record.transportTimeout) return "timedout";
  if (record.status !== 0) return "blockedbyresponse";
  return "failed";
}

async function deliverReplayRecord(record, operations) {
  if (!record || !operations || typeof operations.abort !== "function"
      || typeof operations.applyCookies !== "function"
      || typeof operations.fulfill !== "function") {
    throw new CaptureError("retained replay delivery operations are incomplete");
  }
  if (!record.success) {
    await operations.abort(retainedFailureAbortCode(record));
    return "rejected";
  }
  await operations.applyCookies();
  await operations.fulfill();
  return "served";
}

function teardownEvidenceSnapshot(ledger, responseScheduler, activity, lifecycle = null) {
  return {
    activity: activity.value,
    requests: ledger.requests,
    scheduled: ledger.scheduled,
    matched: ledger.matched,
    served: ledger.served,
    rejected: ledger.rejected,
    unmatched: ledger.unmatched,
    conflicts: ledger.conflicts,
    invalid: ledger.invalid,
    route_selection_version: ROUTE_SELECTION_VERSION,
    occurrence_claims: ledger.occurrence_claims,
    reusable_claims: ledger.reusable_claims,
    occurrence_exhausted: ledger.occurrence_exhausted,
    active: ledger.active,
    scheduler: responseScheduler.summary(),
    late_callbacks: lifecycle === null ? null : {
      closing: lifecycle.closingCallbacks,
      closed: lifecycle.closedCallbacks,
      binding: lifecycle.lateBindingCallbacks,
      handlers: lifecycle.handlers.size,
      terminal_failures: lifecycle.terminalFailures || 0,
    },
  };
}

function teardownEvidenceReady(snapshot) {
  const late = snapshot && snapshot.late_callbacks;
  return Boolean(snapshot && snapshot.active === 0
    && snapshot.scheduler && snapshot.scheduler.pending === 0
    && late && late.closing === 0 && late.closed === 0
    && late.binding === 0 && late.handlers === 0
    && late.terminal_failures === 0);
}

function teardownEvidenceChanges(before, after, phase, limit = 64) {
  if (!Number.isSafeInteger(limit) || limit < 0 || limit > 64) {
    throw new CaptureError("teardown evidence change limit is invalid");
  }
  const changes = [];
  const visit = (field, left, right) => {
    const leftObject = left && typeof left === "object" && !Array.isArray(left);
    const rightObject = right && typeof right === "object" && !Array.isArray(right);
    if (leftObject && rightObject) {
      const keys = [...new Set([...Object.keys(left), ...Object.keys(right)])].sort();
      for (const key of keys) visit(field ? `${field}.${key}` : key, left[key], right[key]);
      return;
    }
    if (JSON.stringify(left) === JSON.stringify(right)) return;
    if (changes.length >= limit) {
      throw new CaptureError("teardown evidence change count exceeds its bound");
    }
    changes.push({ phase, field, before: left, after: right });
  };
  visit("", before, after);
  return changes;
}

function traceDigest(traceDirectory) {
  const root = fs.realpathSync(traceDirectory);
  const files = [];
  let totalBytes = 0;
  function visit(directory) {
    for (const name of fs.readdirSync(directory).sort()) {
      const filename = path.join(directory, name);
      const stat = fs.lstatSync(filename);
      if (stat.isSymbolicLink()) {
        throw new CaptureError(`${filename}: trace contains a symlink`);
      }
      if (stat.isDirectory()) visit(filename);
      else if (stat.isFile()) {
        totalBytes += stat.size;
        if (totalBytes > MAX_TRACE_BYTES) {
          throw new CaptureError(`${root}: trace exceeds ${MAX_TRACE_BYTES} bytes`);
        }
        files.push([filename, stat]);
      }
      else throw new CaptureError(`${filename}: trace contains a non-regular file`);
    }
  }
  visit(root);
  if (files.length === 0 || !fs.existsSync(path.join(root, "trace.meta"))) {
    throw new CaptureError(`${root}: incomplete trace`);
  }
  const hash = crypto.createHash("sha256");
  hash.update(Buffer.from("tilefinch-http-trace-v1\0", "utf8"));
  for (const [filename, stat] of files) {
    const relative = Buffer.from(path.relative(root, filename).split(path.sep).join("/"));
    const nameLength = Buffer.alloc(4);
    nameLength.writeUInt32BE(relative.length);
    const size = Buffer.alloc(8);
    size.writeBigUInt64BE(BigInt(stat.size));
    hash.update(nameLength); hash.update(relative); hash.update(size);
    const descriptor = fs.openSync(filename, "r");
    try {
      const buffer = Buffer.allocUnsafe(Math.min(1024 * 1024, Math.max(1, stat.size)));
      let offset = 0;
      while (offset < stat.size) {
        const count = fs.readSync(
          descriptor, buffer, 0, Math.min(buffer.length, stat.size - offset), offset,
        );
        if (count <= 0) throw new CaptureError(`${filename}: short read during digest`);
        hash.update(buffer.subarray(0, count));
        offset += count;
      }
    } finally {
      fs.closeSync(descriptor);
    }
  }
  return hash.digest("hex");
}

function loadTrace(traceDirectory) {
  const root = fs.realpathSync(traceDirectory);
  // Audit every trace entry before reading record metadata or bodies so a
  // symlink cannot make inspection touch bytes outside the retained corpus.
  const digest = traceDigest(root);
  const metaPaths = fs.readdirSync(root).filter((name) => TRACE_META.test(name)).sort();
  if (metaPaths.length === 0) throw new CaptureError(`${root}: no trace records`);
  if (metaPaths.length > MAX_TRACE_RECORDS) {
    throw new CaptureError(`${root}: trace exceeds ${MAX_TRACE_RECORDS} records`);
  }
  for (let index = 0; index < metaPaths.length; index += 1) {
    const expected = `${String(index).padStart(4, "0")}.meta`;
    if (metaPaths[index] !== expected) {
      throw new CaptureError(`${root}: trace record sequence is not contiguous at ${expected}`);
    }
  }
  const info = traceInfo(root, metaPaths.length);
  const originMs = info.originMs;
  const records = metaPaths.map((name) => loadRecord(path.join(root, name)));
  if (originMs === null && records.some((record) => record.responseDateSeconds === null
      && record.cookies.some((cookie) => cookie.expires !== null && cookie.maxAge === null))) {
    throw new CaptureError(
      `${root}: Expires cookie requires a retained response Date or trace origin-ms metadata`,
    );
  }
  const grouped = new Map();
  for (const record of records) {
    const key = `${record.method}\0${record.url}`;
    if (!grouped.has(key)) grouped.set(key, []);
    grouped.get(key).push(record);
  }
  const routes = new Map();
  for (const [key, candidates] of grouped) routes.set(key, selectRoute(candidates));
  return {
    root, records, routes, digest, originMs,
    ambiguousRoutes: [...routes.values()].filter((route) => route.ambiguous).length,
  };
}

function parseMarkerList(value) {
  if (value === "-") return [];
  const markers = value.split("||").map((item) => item.trim());
  if (markers.length === 0 || markers.length > 64
      || markers.some((item) => item.length === 0 || item.length > 4096)) {
    throw new CaptureError("marker lists use non-empty values separated by ||");
  }
  return markers;
}

function parseCheckpoints(value) {
  const items = value.split("|");
  if (items.length > 64 || items.some((item) => item.length > 4096)) {
    throw new CaptureError("checkpoint list exceeds the capture bound");
  }
  if (items.length === 3 && items[0] === "top" && items[1] === "-"
      && items[2] === "bottom") {
    return [
      { name: "top", kind: "top", target: null },
      { name: "bottom", kind: "bottom", target: null },
    ];
  }
  if (items.length < 3 || items[0] !== "top" || items.at(-1) !== "bottom") {
    throw new CaptureError("checkpoints must start with top, end with bottom, and include a semantic target");
  }
  return items.map((item, index) => {
    if (index === 0 || index === items.length - 1) {
      return { name: item, kind: item, target: null };
    }
    const colon = item.indexOf(":");
    if (colon <= 0) throw new CaptureError(`invalid checkpoint ${item}`);
    const kind = item.slice(0, colon);
    const target = item.slice(colon + 1);
    if (!["anchor", "selector", "text"].includes(kind) || target.length === 0) {
      throw new CaptureError(`invalid checkpoint ${item}`);
    }
    return { name: `${kind}-${index}`, kind, target };
  });
}

function safeRelative(value, label) {
  if (!value || value === "-" || path.isAbsolute(value)
      || value.split(/[\\/]/).includes("..") || value.includes("\0")) {
    throw new CaptureError(`${label} must be a safe relative path`);
  }
  return value;
}

function resolveWithin(rootValue, relative, label) {
  const root = fs.realpathSync(rootValue);
  const resolved = fs.realpathSync(path.resolve(root, relative));
  const fromRoot = path.relative(root, resolved);
  if (fromRoot === ".." || fromRoot.startsWith(`..${path.sep}`)
      || path.isAbsolute(fromRoot)) {
    throw new CaptureError(`${label} escapes its root`);
  }
  return resolved;
}

function parseBlockedOrigins(raw, manifestPath) {
  if (!raw || raw === "-") return [];
  const hosts = raw.split("|");
  if (hosts.length > 64) {
    throw new CaptureError(`${manifestPath}: blocked_origins lists more than 64 hosts`);
  }
  const HOSTNAME = /^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$/;
  for (const host of hosts) {
    if (!HOSTNAME.test(host)) {
      throw new CaptureError(
        `${manifestPath}: blocked_origins entry is not a lowercase hostname: ${host}`);
    }
  }
  return hosts;
}

function blockedHostMatches(hostname, blockedHosts) {
  return blockedHosts.some((entry) =>
    hostname === entry || hostname.endsWith(`.${entry}`));
}

function loadScenario(manifestPath, wanted) {
  if (fs.statSync(manifestPath).size > MAX_MANIFEST_BYTES) {
    throw new CaptureError(`${manifestPath}: manifest exceeds ${MAX_MANIFEST_BYTES} bytes`);
  }
  const lines = fs.readFileSync(manifestPath, "utf8").trimEnd().split(/\r?\n/)
    .filter((line) => line && !line.startsWith("#"));
  if (lines.length < 2) throw new CaptureError(`${manifestPath}: empty manifest`);
  const fields = lines[0].split("\t");
  if (fields.length < MANIFEST_FIELDS.length - OPTIONAL_MANIFEST_FIELDS
      || fields.length > MANIFEST_FIELDS.length
      || fields.some((field, index) => field !== MANIFEST_FIELDS[index])) {
    throw new CaptureError(`${manifestPath}: manifest columns differ from the visual contract`);
  }
  for (let i = 1; i < lines.length; i += 1) {
    const values = lines[i].split("\t");
    if (values.length !== fields.length) {
      throw new CaptureError(`${manifestPath}:${i + 1}: manifest column count mismatch`);
    }
    const row = Object.fromEntries(fields.map((field, index) => [field, values[index]]));
    if (row.scenario !== wanted) continue;
    const number = (name, minimum, maximum) => {
      const raw = row[name];
      const value = Number(raw);
      if (!/^(?:0|[1-9][0-9]*)$/.test(raw)
          || !Number.isSafeInteger(value) || value < minimum || value > maximum) {
        throw new CaptureError(`${manifestPath}: ${name} must be an integer in ${minimum}..${maximum}`);
      }
      return value;
    };
    if (!SAFE_NAME.test(row.scenario)) {
      throw new CaptureError(`${manifestPath}: invalid scenario name`);
    }
    if (!TRACE_DIGEST.test(row.trace_sha256)) {
      throw new CaptureError(`${manifestPath}: trace_sha256 must be 64 lowercase hexadecimal digits`);
    }
    if (!row.required_title || row.required_title === "-"
        || row.required_title.length > 4096
        || !row.required_state_marker || row.required_state_marker === "-"
        || row.required_state_marker.length > 4096) {
      throw new CaptureError(`${manifestPath}: title and state marker are required`);
    }
    const deviceWidth = number("device_width", 1, 16384);
    const deviceHeight = number("device_height", 1, 16384);
    const cssWidth = number("css_width", 1, 16384);
    const cssHeight = number("css_height", 1, 16384);
    const scaleNumerator = number("scale_numerator", 1, 16384);
    const scaleDenominator = number("scale_denominator", 1, 16384);
    const scale = scaleNumerator / scaleDenominator;
    if (Math.round(cssWidth * scale) !== deviceWidth
        || Math.round(cssHeight * scale) !== deviceHeight) {
      throw new CaptureError(`${manifestPath}: CSS viewport and scale do not produce the device geometry`);
    }
    safeRelative(row.reference_state, "reference_state");
    number("limit_mb", 4, 512);
    const ticks = number("ticks", 0, MAX_REPLAY_TICKS);
    const tickMs = number("tick_ms", 1, MAX_REPLAY_TICK_MS);
    number("max_download_kb", 1, 65536);
    number("script_timeout_ms", 1, 300000);
    number("script_heap_mb", 1, 256);
    number("script_total_mb", 1, 128);
    number("script_file_kb", 1, 8192);
    number("script_count", 1, 256);
    return {
      name: row.scenario, url: normalizeUrl(row.url, manifestPath),
      replayDir: safeRelative(row.replay_dir, "replay_dir"), digest: row.trace_sha256,
      expectedHttp: number("expected_http", 100, 599), requiredTitle: row.required_title,
      requiredMarker: row.required_state_marker,
      fallbackMarkers: parseMarkerList(row.fallback_markers),
      interstitialMarkers: parseMarkerList(row.interstitial_markers),
      deviceWidth, deviceHeight, cssWidth, cssHeight,
      scaleNumerator, scaleDenominator, ticks, tickMs,
      checkpoints: parseCheckpoints(row.checkpoints),
      minStylesheets: number("min_stylesheets_loaded", 0, 1000000),
      minImages: number("min_images_loaded", 0, 1000000),
      minScripts: number("min_scripts_loaded", 0, 1000000),
      minNetwork: number("min_network_completions", 0, 1000000),
      maxPending: number("max_pending", 0, 1000000),
      blockedOrigins: parseBlockedOrigins(row.blocked_origins, manifestPath),
    };
  }
  throw new CaptureError(`scenario not found: ${wanted}`);
}

function compactRanges(values) {
  const numbers = [...new Set(values.map((value) => Number(value)))].sort((a, b) => a - b);
  const ranges = [];
  for (let i = 0; i < numbers.length;) {
    const start = numbers[i];
    let end = start;
    while (i + 1 < numbers.length && numbers[i + 1] === end + 1) end = numbers[++i];
    ranges.push(start === end ? String(start).padStart(4, "0")
      : `${String(start).padStart(4, "0")}-${String(end).padStart(4, "0")}`);
    i += 1;
  }
  return ranges.join(",");
}

function inspectTrace(traceDirectory) {
  const trace = loadTrace(traceDirectory);
  return {
    trace_dir: trace.root, trace_sha256: trace.digest,
    records: trace.records.length, routes: trace.routes.size,
    ambiguous_routes: trace.ambiguousRoutes,
    occurrence_routes: [...trace.routes.values()]
      .filter((route) => route.mode === "occurrence-sequence").length,
    route_selection_version: ROUTE_SELECTION_VERSION,
    response_cookies: trace.records.reduce((count, record) => count + record.cookies.length, 0),
    clock_origin_ms: trace.originMs,
    urls: [...new Set([...trace.routes.keys()]
      .map((key) => key.slice(key.indexOf("\0") + 1)))].sort(),
  };
}

function playwrightHeaders(record) {
  const headers = Object.create(null);
  for (const [name, value] of record.headers) {
    const lower = name.toLowerCase();
    headers[lower] = headers[lower] === undefined ? value : `${headers[lower]}, ${value}`;
  }
  if (record.contentType && headers["content-type"] === undefined) {
    headers["content-type"] = record.contentType;
  }
  headers["content-length"] = String(record.body.length);
  return headers;
}

async function settlePage(page, milliseconds) {
  const started = Date.now();
  const minimum = started + milliseconds;
  const deadline = started + Math.max(milliseconds, 250) + 1000;
  let previous = "";
  let stable = 0;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(
      resolve, Math.min(25, Math.max(1, deadline - Date.now())),
    ));
    const state = await page.evaluate(() => {
      const root = document.scrollingElement || document.documentElement;
      return `${root.scrollHeight}:${document.images.length}:` +
        `${[...document.images].filter((image) => image.complete).length}:${document.styleSheets.length}`;
    });
    if (state === previous) stable += 1; else stable = 0;
    previous = state;
    if (Date.now() >= minimum && stable >= 2) return;
  }
  throw new CaptureError("page did not reach bounded visual stability");
}

async function settleCheckpoint(page, checkpoint, milliseconds, isNetworkIdle) {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const requested = await checkpointPosition(page, checkpoint);
    await settlePage(page, milliseconds);
    const state = await page.evaluate(() => {
      const root = document.scrollingElement || document.documentElement;
      return {
        scrollY: window.scrollY,
        maximum: Math.max(0, root.scrollHeight - innerHeight),
      };
    });
    const positioned = checkpoint.kind === "top" ? state.scrollY === 0
      : (checkpoint.kind === "bottom" ? state.scrollY === state.maximum
        : state.scrollY === requested);
    if (positioned && isNetworkIdle()) {
      if (["top", "bottom"].includes(checkpoint.kind)) return state.scrollY;
      const confirmed = await checkpointPosition(page, checkpoint);
      if (confirmed === state.scrollY && isNetworkIdle()) return confirmed;
    }
  }
  throw new CaptureError(`${checkpoint.kind} checkpoint did not settle`);
}

async function settleCapture(page, milliseconds, isNetworkIdle) {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    await settlePage(page, milliseconds);
    if (isNetworkIdle()) return;
  }
  throw new CaptureError("network did not become idle during bounded capture");
}

async function checkpointPosition(page, checkpoint) {
  if (checkpoint.kind === "top") {
    return page.evaluate(() => { window.scrollTo(0, 0); return window.scrollY; });
  }
  if (checkpoint.kind === "bottom") {
    let state = null;
    for (let i = 0; i < 4; i += 1) {
      state = await page.evaluate(() => {
        const root = document.scrollingElement || document.documentElement;
        window.scrollTo(0, root.scrollHeight);
        return { scrollY: window.scrollY, maximum: Math.max(0, root.scrollHeight - innerHeight) };
      });
      await new Promise((resolve) => setImmediate(resolve));
      if (state.scrollY === state.maximum) break;
    }
    if (state === null || state.scrollY !== state.maximum) {
      throw new CaptureError("bottom checkpoint did not reach the maximum scroll offset");
    }
    return state.scrollY;
  }
  const result = await page.evaluate(({ kind, target }) => {
    let element = null;
    if (kind === "anchor") element = document.getElementById(target);
    else if (kind === "selector") element = document.querySelector(target);
    else if (kind === "text") {
      const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT);
      for (let node = walker.nextNode(); node; node = walker.nextNode()) {
        if (node.children.length === 0 && node.textContent.includes(target)) { element = node; break; }
      }
    }
    if (!element) return null;
    const y = Math.max(0, Math.floor(element.getBoundingClientRect().top + scrollY));
    window.scrollTo(0, y);
    return { requested: y, actual: window.scrollY };
  }, checkpoint);
  if (result === null) throw new CaptureError(`${checkpoint.kind} checkpoint target was not found`);
  return result.actual;
}

async function runScenarioClock(
  context, scheduler, ticks, tickMs, hostOperations = null,
) {
  if (!context.clock || typeof context.clock.runFor !== "function") {
    throw new CaptureError("offline capture requires Playwright Clock.runFor");
  }
  for (let tick = 0; tick < ticks; tick += 1) {
    /* Match script_runtime_advance(): every existing realm receives the full
       logical tick before Playwright releases timers/rAF scheduled within it.
       A realm created during that release starts at zero, like a newly-created
       native ScriptRuntime, and joins the next manifest tick. */
    for (const page of context.pages()) {
      for (const frame of page.frames()) {
        await runTrackedHostOperation(
          hostOperations, "replay-frame-clock-advance", () => frame.evaluate((milliseconds) => {
          const advance = globalThis.__tilefinchAdvanceReplayClock;
          if (typeof advance !== "function") {
            throw new Error("deterministic replay clock advance is unavailable");
          }
          advance(milliseconds);
          }, tickMs),
        );
      }
    }
    await runTrackedHostOperation(
      hostOperations, "playwright-clock-advance",
      () => context.clock.runFor(tickMs),
    );
    /* One manifest tick owns exactly one semantic response pump. Requests
       admitted by timer/rAF callbacks cannot be advanced by incidental host
       turns; remaining delayed work is fast-forwarded only at an explicit
       quiescence boundary. */
    await scheduler.pumpOnce();
  }
}

function replayClockEvidenceReady(clockEvidence, originMs, hostElapsedMs) {
  const sources = clockEvidence && clockEvidence.clockSources;
  const sourceKeys = sources && typeof sources === "object"
    ? Object.keys(sources).sort() : [];
  const expectedSourceKeys = [...REPLAY_CLOCK_SOURCES].sort();
  const sourcesReady = sourceKeys.length === expectedSourceKeys.length
    && sourceKeys.every((key, index) => key === expectedSourceKeys[index])
    && expectedSourceKeys.every((key) => Number.isSafeInteger(sources[key])
      && sources[key] >= 0 && sources[key] <= MAX_REPLAY_OBSERVATIONS);
  const sourceTotal = (keys) => sourcesReady
    ? keys.reduce((total, key) => total + sources[key], 0) : -1;
  const wallSourceTotal = sourceTotal(REPLAY_CLOCK_WALL_SOURCES);
  const monotonicObservationTotal = sourceTotal(
    REPLAY_CLOCK_MONOTONIC_OBSERVATION_SOURCES,
  );
  const monotonicSampleTotal = sourceTotal(
    REPLAY_CLOCK_MONOTONIC_SAMPLE_SOURCES,
  );
  const allSourceTotal = sourceTotal(REPLAY_CLOCK_SOURCES);
  return clockEvidence !== null && typeof clockEvidence === "object"
    && Number.isSafeInteger(hostElapsedMs)
    && hostElapsedMs >= 0 && hostElapsedMs <= MAX_REPLAY_HOST_ELAPSED_MS
    && clockEvidence.contract === REPLAY_CLOCK_CONTRACT
    && clockEvidence.scope === REPLAY_CLOCK_SCOPE
    && clockEvidence.originMs === originMs
    && clockEvidence.hostElapsedMs === hostElapsedMs
    && clockEvidence.playwrightElapsedMs === hostElapsedMs
    && Number.isSafeInteger(clockEvidence.wallElapsedMs)
    && clockEvidence.wallElapsedMs >= hostElapsedMs
    && clockEvidence.wallElapsedMs <= MAX_REPLAY_DOMAIN_ELAPSED_MS
    && Number.isSafeInteger(clockEvidence.monotonicElapsedMs)
    && clockEvidence.monotonicElapsedMs >= hostElapsedMs
    && clockEvidence.monotonicElapsedMs <= MAX_REPLAY_DOMAIN_ELAPSED_MS
    && Number.isSafeInteger(clockEvidence.wallObservations)
    && clockEvidence.wallObservations >= 0
    && Number.isSafeInteger(clockEvidence.monotonicObservations)
    && clockEvidence.monotonicObservations >= 0
    && Number.isSafeInteger(clockEvidence.monotonicSamples)
    && clockEvidence.monotonicSamples >= 0
    && sourcesReady
    && allSourceTotal <= MAX_REPLAY_OBSERVATIONS
    && wallSourceTotal === clockEvidence.wallObservations
    && monotonicObservationTotal === clockEvidence.monotonicObservations
    && monotonicSampleTotal === clockEvidence.monotonicSamples
    && clockEvidence.wallElapsedMs
      === hostElapsedMs + clockEvidence.wallObservations
    && clockEvidence.monotonicElapsedMs
      === hostElapsedMs + clockEvidence.monotonicObservations;
}

async function evaluateCaptureEvidence(page, scenario, hostOperations = null) {
  return runTrackedHostOperation(
    hostOperations, "capture-evidence-evaluate", () => page.evaluate(
      ({ requiredMarker, fallbackMarkers, interstitialMarkers }) => {
        const bodyText = document.body ? document.body.innerText : "";
        return {
          title: document.title,
          requiredMarker: bodyText.includes(requiredMarker),
          fallback: fallbackMarkers.some((marker) => bodyText.includes(marker)),
          interstitial: interstitialMarkers.some((marker) => bodyText.includes(marker)),
          stylesheets: document.styleSheets.length,
          images: [...document.images].filter(
            (image) => image.complete && image.naturalWidth > 0,
          ).length,
          deferredImages: [...document.images].filter(
            (image) => !image.complete,
          ).length,
          userAgent: navigator.userAgent,
          platform: navigator.platform,
          /* The layout viewport is what screenshots render and what the
             manifest declares.  documentElement.clientWidth avoids the
             visual-width drift when mobile Chrome auto-zooms overflowing
             content.  Its clientHeight is not a viewport measurement in
             quirks mode (it can be the full scrolling document, as on HN),
             while innerHeight remains the CSS viewport height. */
          innerWidth: document.documentElement.clientWidth || innerWidth,
          innerHeight,
          devicePixelRatio,
          replayEnvironment: globalThis.__tilefinchReplayEnvironment || "",
          replayClock: globalThis.__tilefinchReplayClock || "",
          replayIntl: globalThis.__tilefinchReplayIntl || "",
          readOnlyPolicy: globalThis.__tilefinchReadOnlyPolicy || "",
          readOnlyPreflightPolicy:
            globalThis.__tilefinchReadOnlyPreflightPolicy || "",
          capabilityPolicy: globalThis.__tilefinchOfflineCapabilityPolicy || "",
          capabilityEvidence:
            globalThis.__tilefinchOfflineCapabilityEvidence || null,
          performanceEntriesEmpty: performance.getEntries().length === 0
            && performance.getEntriesByType("navigation").length === 0
            && performance.getEntriesByType("resource").length === 0
            && performance.getEntriesByType("paint").length === 0,
          performanceTimeOrigin: performance.timeOrigin,
          clockEvidence:
            typeof globalThis.__tilefinchReplayClockEvidence === "function"
              ? globalThis.__tilefinchReplayClockEvidence() : null,
        };
      },
      {
        requiredMarker: scenario.requiredMarker,
        fallbackMarkers: scenario.fallbackMarkers,
        interstitialMarkers: scenario.interstitialMarkers,
      },
    ),
  );
}

async function drainRouteHandlers(scheduler, lifecycle, limit = 64) {
  for (let pass = 0; pass < limit; pass += 1) {
    await scheduler.driveToIdle();
    await Promise.resolve();
    if (lifecycle.handlers.size === 0 && scheduler.summary().pending === 0) {
      await scheduler.closeAdmissionGeneration();
      await Promise.resolve();
      if (lifecycle.handlers.size === 0 && scheduler.summary().pending === 0) return;
    }
  }
  throw new CaptureError("route handlers did not reach a bounded drain");
}

async function boundedDrainRouteHandlers(hostOperations, scheduler, lifecycle) {
  if (!hostOperations || typeof hostOperations.run !== "function") {
    throw new CaptureError("route handler drain requires host operation bounds");
  }
  return hostOperations.run(
    "route-handler-drain", () => drainRouteHandlers(scheduler, lifecycle),
  );
}

async function freezeAndDrainContext(
  context, scheduler, ledger, activity, lifecycle, hostOperations,
) {
  await boundedDrainRouteHandlers(hostOperations, scheduler, lifecycle);
  if (ledger.active !== 0 || scheduler.summary().pending !== 0) {
    throw new CaptureError("offline capture still has active response delivery");
  }
  const pages = context.pages();
  if (pages.length !== 1) {
    throw new CaptureError("offline capture must freeze exactly one context page");
  }
  const before = activity.value;
  for (let pass = 0; pass < 3; pass += 1) {
    await scheduler.closeAdmissionGeneration();
    await boundedDrainRouteHandlers(hostOperations, scheduler, lifecycle);
  }
  if (ledger.active !== 0 || scheduler.summary().pending !== 0
      || activity.value !== before) {
    throw new CaptureError("late route or capability activity survived the freeze boundary");
  }
  return pages;
}

function normalizePng(raw, output, scenario, python, timeoutMs) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1
      || timeoutMs > MAX_HOST_OPERATION_TIMEOUT_MS) {
    throw new CaptureError("reference normalizer timeout is invalid");
  }
  const script = path.join(__dirname, "normalize-reference-frame.py");
  try {
    const completed = childProcess.spawnSync(python, [script, raw, output,
      "--width", String(scenario.deviceWidth), "--height", String(scenario.deviceHeight),
      "--force"],
    {
      encoding: "utf8", timeout: timeoutMs, killSignal: "SIGKILL",
      maxBuffer: NORMALIZER_OUTPUT_LIMIT,
    });
    if (completed.error) {
      if (completed.error.code === "ETIMEDOUT") {
        throw new CaptureError(
          `reference normalization exceeded ${timeoutMs}ms deadline and was terminated`,
        );
      }
      throw new CaptureError(
        `reference normalization could not execute: ${completed.error.message}`,
      );
    }
    if (completed.signal) {
      throw new CaptureError(
        `reference normalization was terminated by ${completed.signal}`,
      );
    }
    if (completed.status !== 0) {
      throw new CaptureError(`reference normalization failed: ${completed.stderr || completed.stdout}`);
    }
  } finally {
    try { fs.unlinkSync(raw); }
    catch (error) { if (error.code !== "ENOENT") throw error; }
  }
}

async function collectReferenceTextMetrics(page, scenario, hostOperations = null) {
  const collect = () => page.evaluate((limits) => {
    const runs = [];
    let nodeCount = 0;
    let eligibleRuns = 0;
    let skippedLong = 0;
    let truncated = false;
    const fixed = (value) => {
      const number = Number(value);
      if (!Number.isFinite(number)) return null;
      const scaled = Math.round(number * 64);
      return Number.isSafeInteger(scaled) ? scaled : null;
    };
    const root = document.body || document.documentElement;
    if (!root) {
      return {
        schema: "tilefinch-text-metrics-v1", source: "chromium",
        viewport: { width: innerWidth, height: innerHeight },
        document_height: 0, eligible_runs: 0, truncated: false, runs,
        run_count: 0,
      };
    }
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (nodeCount >= limits.nodeLimit) { truncated = true; break; }
      const nodeIndex = nodeCount++;
      const parent = node.parentElement;
      if (!parent || !node.data || !/\S/u.test(node.data)) continue;
      const style = getComputedStyle(parent);
      if (style.display === "none" || style.visibility === "hidden"
          || Number(style.opacity) === 0) continue;
      let tokenIndex = 0;
      for (const match of node.data.matchAll(/\S+/gu)) {
        const text = match[0];
        const start = match.index;
        const end = start + text.length;
        const range = document.createRange();
        range.setStart(node, start);
        range.setEnd(node, end);
        const rectangles = [...range.getClientRects()].filter(
          (rect) => rect.width > 0 && rect.height > 0
            && rect.right > 0 && rect.left < innerWidth,
        );
        for (let fragment = 0; fragment < rectangles.length; fragment += 1) {
          eligibleRuns += 1;
          if (text.length > limits.textLimit) {
            skippedLong += 1;
            continue;
          }
          if (runs.length >= limits.runLimit) {
            truncated = true;
            continue;
          }
          const rect = rectangles[fragment];
          const lineHeight = style.lineHeight === "normal"
            ? null : fixed(parseFloat(style.lineHeight));
          const letterSpacing = style.letterSpacing === "normal"
            ? 0 : fixed(parseFloat(style.letterSpacing));
          runs.push({
            index: runs.length, node_index: nodeIndex,
            token_index: tokenIndex, fragment_index: fragment,
            text, x_26_6: fixed(rect.left + scrollX),
            y_26_6: fixed(rect.top + scrollY),
            advance_26_6: fixed(rect.width),
            rect_height_26_6: fixed(rect.height),
            font_size_26_6: fixed(parseFloat(style.fontSize)),
            line_height_26_6: lineHeight,
            letter_spacing_26_6: letterSpacing,
            word_spacing_26_6: style.wordSpacing === "normal"
              ? 0 : fixed(parseFloat(style.wordSpacing)),
            font_family: style.fontFamily,
            font_weight: style.fontWeight,
            font_style: style.fontStyle,
          });
        }
        tokenIndex += 1;
      }
    }
    const documentHeight = Math.max(
      document.documentElement ? document.documentElement.scrollHeight : 0,
      document.body ? document.body.scrollHeight : 0,
    );
    return {
      schema: "tilefinch-text-metrics-v1", source: "chromium",
      viewport: { width: innerWidth, height: innerHeight },
      document_height: documentHeight, eligible_runs: eligibleRuns,
      skipped_long: skippedLong,
      truncated: truncated || skippedLong !== 0,
      runs, run_count: runs.length,
    };
  }, {
    runLimit: TEXT_METRICS_RUN_LIMIT,
    nodeLimit: TEXT_METRICS_NODE_LIMIT,
    textLimit: TEXT_METRICS_TEXT_LIMIT,
  });
  return hostOperations === null
    ? collect() : hostOperations.run("reference-text-metrics", collect);
}

function launchBrowser(browserType, launch, hostOperations) {
  if (!browserType || typeof browserType.launch !== "function") {
    return Promise.reject(new CaptureError("Playwright browser launch is unavailable"));
  }
  return runTrackedHostOperation(
    hostOperations, "browser-launch", async () => {
      const launched = await browserType.launch(launch);
      if (hostOperations && typeof hostOperations.isTerminal === "function"
          && hostOperations.isTerminal()) {
        try { await launched.close(); } catch (_error) {}
      }
      return launched;
    },
  );
}

async function captureReference(options) {
  if (!options.scenario || !options.traceRoot || !options.outputRoot) {
    throw new CaptureError("capture mode requires --scenario, --trace-root, and --output-root");
  }
  if (options.browser !== "chromium") {
    throw new CaptureError("offline reference capture requires Chromium");
  }
  const scenario = loadScenario(path.resolve(options.manifest), options.scenario);
  const traceDirectory = resolveWithin(
    path.resolve(options.traceRoot), scenario.replayDir, "replay_dir",
  );
  const trace = loadTrace(traceDirectory);
  if (scenario.digest !== trace.digest) {
    throw new CaptureError(`trace digest mismatch: manifest=${scenario.digest} actual=${trace.digest}`);
  }
  const environment = replayEnvironment(trace);
  const outputRoot = path.resolve(options.outputRoot);
  fs.mkdirSync(outputRoot, { recursive: true });
  const outputDirectory = path.join(outputRoot, scenario.name);
  fs.mkdirSync(outputDirectory, { recursive: true });
  const outputFromRoot = path.relative(fs.realpathSync(outputRoot), fs.realpathSync(outputDirectory));
  if (outputFromRoot === ".." || outputFromRoot.startsWith(`..${path.sep}`)
      || path.isAbsolute(outputFromRoot)) {
    throw new CaptureError("scenario output directory escapes its root");
  }
  for (const stale of ["reference-state.json", "reference-diagnostic.json"]) {
    try { fs.unlinkSync(path.join(outputDirectory, stale)); }
    catch (error) { if (error.code !== "ENOENT") throw error; }
  }
  const textMetricsPath = options.textMetricsOutput
    ? path.resolve(options.textMetricsOutput) : null;
  if (textMetricsPath !== null) {
    try { fs.unlinkSync(textMetricsPath); }
    catch (error) { if (error.code !== "ENOENT") throw error; }
  }
  let playwright;
  try { playwright = require("playwright"); }
  catch (_error) { throw new CaptureError("capture mode requires the Playwright package"); }
  const browserType = playwright[options.browser];
  if (!browserType || typeof browserType.launch !== "function") {
    throw new CaptureError(`unsupported Playwright browser ${options.browser}`);
  }
  const launch = { headless: true };
  if (options.browser === "chromium") {
    launch.args = [
      "--disable-background-networking", "--disable-component-update",
      "--disable-domain-reliability", "--disable-sync",
      "--host-resolver-rules=MAP * ~NOTFOUND", "--metrics-recording-only",
      "--no-default-browser-check", "--no-first-run",
    ];
  }
  if (options.executable) launch.executablePath = options.executable;
  launch.args.push("--disable-quic");
  let browser = null;
  let context = null;
  const routeLifecycle = {
    phase: "OPEN", nextBrowserOrdinal: 0, handlers: new Set(),
    closingCallbacks: 0, closedCallbacks: 0, lateBindingCallbacks: 0,
    terminalFailures: 0,
  };
  let responseScheduler = null;
  const hostOperations = createHostOperationTracker({
    timeoutMs: options.timeoutMs,
    onTerminalFailure: (error, label) => {
      routeLifecycle.terminalFailures += 1;
      if (routeLifecycle.phase === "OPEN") routeLifecycle.phase = "CLOSING";
      if (responseScheduler !== null) {
        responseScheduler.failTerminal(error, `host ${label}`);
      }
    },
  });
  const runHost = (label, operation, settings) =>
    hostOperations.run(label, operation, settings);
  try {
  browser = await launchBrowser(browserType, launch, hostOperations);
  context = await runHost("browser-context-create", () => browser.newContext({
      viewport: { width: scenario.cssWidth, height: scenario.cssHeight },
      screen: { width: scenario.cssWidth, height: scenario.cssHeight },
      deviceScaleFactor: scenario.scaleNumerator / scenario.scaleDenominator,
      /* The fidelity oracle is Chrome *device emulation* at this size
         (what an Android Chrome would render), so viewport metas are
         honoured exactly as the engine honours them. */
      isMobile: true, hasTouch: true,
      locale: "en-US", timezoneId: "UTC", colorScheme: "light",
      serviceWorkers: "block", javaScriptEnabled: true,
    }));
  if (!context.clock || typeof context.clock.install !== "function"
      || typeof context.clock.pauseAt !== "function") {
    throw new CaptureError("offline capture requires Playwright Clock support");
  }
  await runHost(
    "playwright-clock-install",
    () => context.clock.install({ time: environment.originMs }),
  );
  await runHost(
    "playwright-clock-pause", () => context.clock.pauseAt(environment.originMs),
  );
  /* Rebase all persistent cookies against one replay instant.  This retains
     Max-Age and captured Expires TTLs without reviving their stale wall-clock
     timestamps differently for concurrent responses. */
  const cookieReplayEpochSeconds = environment.originMs / 1000;
  const ledger = {
    mode: "cdp-response-keyed", records: trace.records.length, routes: trace.routes.size,
    requests: 0, scheduled: 0, matched: 0, served: 0, rejected: 0,
    unmatched: 0, conflicts: 0, invalid: 0, blocked: 0,
    active: 0, claimed: new Set(), resource_types: Object.create(null),
    served_resource_types: Object.create(null),
    occurrence_claims: 0, reusable_claims: 0, occurrence_exhausted: 0,
  };
  const routeOccurrences = new Map();
  const requestOccurrences = new Map();
  const syntheticHttpTerminalRecord = Object.freeze({
    id: "0000", asyncDelayPumps: 1,
  });
  const unexpectedDiagnostics = createRequestDiagnostics();
  const policyDiagnostics = createRequestDiagnostics(MAX_POLICY_DENIALS);
  const capabilityDiagnostics = createRequestDiagnostics(MAX_POLICY_DENIALS);
  const blockedDiagnostics = createRequestDiagnostics(MAX_POLICY_DENIALS);
  let preflightPolicyDenials = 0;
  const activity = { value: 0 };
  responseScheduler = createResponseScheduler({
    /* BrowserContext.cookies() is a read-only round trip on the same
       Playwright connection that dispatches BrowserContext route callbacks.
       Two unchanged probes form a bounded candidate admission generation;
       they are not a cross-target protocol fence. Exact repeated-capture
       equality remains the authority for deterministic reference state. */
    probe: async () => { await context.cookies(); },
    activity: () => activity.value,
    onTerminalFailure: (error, label) => {
      hostOperations.failTerminal(error, `scheduler ${label}`);
    },
  });
  const recordUnexpected = (
    classification, method, resourceType, value, field = "unmatched",
  ) => {
    activity.value += 1;
    ledger.requests += 1;
    ledger[field] += 1;
    unexpectedDiagnostics.record(classification, method, resourceType, value);
  };
  if (typeof context.routeWebSocket !== "function") {
    throw new CaptureError("offline capture requires Playwright WebSocket routing support");
  }
  const trackRouteHandler = (operation) => {
    routeLifecycle.handlers.add(operation);
    operation.then(
      () => routeLifecycle.handlers.delete(operation),
      () => routeLifecycle.handlers.delete(operation),
    );
    return operation;
  };
  const closeLateRoute = async (route, phase) => {
    if (phase === "CLOSING") routeLifecycle.closingCallbacks += 1;
    else routeLifecycle.closedCallbacks += 1;
    try {
      await runHost("late-route-abort", () => route.abort("blockedbyclient"), {
        allowAfterTerminal: true,
      });
    } catch (_) {}
  };
  await context.routeWebSocket("**/*", (socket) => {
    const phase = routeLifecycle.phase;
    const operation = (async () => {
      if (phase !== "OPEN") {
        if (phase === "CLOSING") routeLifecycle.closingCallbacks += 1;
        else routeLifecycle.closedCallbacks += 1;
        try {
          await runHost("late-websocket-close", () => socket.close({
            code: 1008, reason: "offline reference capture",
          }), { allowAfterTerminal: true });
        }
        catch (_) {}
        return;
      }
      recordUnexpected("unmatched", "WEBSOCKET", "websocket", socket.url());
      await runHost("websocket-close", () => socket.close({
        code: 1008, reason: "offline reference capture",
      }));
    })();
    return trackRouteHandler(operation);
  });
  const handleRoute = async (route, phase, browserOrdinal) => {
    if (phase !== "OPEN") {
      await closeLateRoute(route, phase);
      return;
    }
    activity.value += 1;
    const request = route.request();
    const requestUrl = request.url();
    if (!/^https?:/i.test(requestUrl)) {
      let protocol = "";
      try { protocol = new URL(requestUrl).protocol; } catch (_error) {}
      if (["about:", "blob:", "data:"].includes(protocol)) {
        await runHost("route-continue", () => route.continue());
      } else {
        recordUnexpected(
          "invalid", request.method(), request.resourceType(), requestUrl, "invalid",
        );
        await runHost("invalid-protocol-abort", () => route.abort("blockedbyclient"));
      }
      return;
    }

    if (scenario.blockedOrigins.length > 0) {
      let blockedHostname = "";
      try { blockedHostname = new URL(requestUrl).hostname.toLowerCase(); }
      catch (_error) {}
      if (blockedHostMatches(blockedHostname, scenario.blockedOrigins)) {
        /* Declared ad/telemetry hosts mint unique URLs per visit and can
           never converge to a hermetic trace.  Abort them outside the
           replay ledger so they neither count as unmatched evidence nor
           enter acquisition plans; the separate blocked channel keeps the
           evidence auditable. */
        ledger.blocked += 1;
        blockedDiagnostics.record(
          "blocked-origin", request.method(), request.resourceType(), requestUrl,
        );
        await runHost("blocked-origin-abort", () => route.abort("blockedbyclient"));
        return;
      }
    }

    const requestMethod = request.method().toUpperCase();
    const resourceType = request.resourceType();
    ledger.requests += 1;
    ledger.active += 1;
    ledger.resource_types[resourceType] =
      (ledger.resource_types[resourceType] || 0) + 1;
    let normalized = "";
    let routeKey;
    let invalidUrl = false;
    try {
      normalized = normalizeUrl(requestUrl);
      routeKey = `${requestMethod}\0${normalized}`;
    } catch (_error) {
      invalidUrl = true;
      routeKey = `INVALID\0${requestMethod}\0${crypto.createHash("sha256")
        .update(requestUrl).digest("hex")}`;
    }
    const requestOccurrence = requestOccurrences.get(routeKey) || 0;
    requestOccurrences.set(routeKey, requestOccurrence + 1);
    const identity = {
      routeKey, resourceType, occurrence: requestOccurrence, browserOrdinal,
    };
    ledger.scheduled += 1;

    const terminalAdmission = (
      classification, field, diagnosticUrl, label, disposition,
      { preflight = false, abortCode = "blockedbyclient" } = {},
    ) => responseScheduler.enqueueAdmission(identity, () => {
      ledger[field] += 1;
      unexpectedDiagnostics.record(
        classification, requestMethod, resourceType, diagnosticUrl,
      );
      if (preflight) preflightPolicyDenials += 1;
      return {
        record: syntheticHttpTerminalRecord,
        admissionKind: "terminal",
        work: async () => {
          await runHost(label, () => route.abort(abortCode));
          return disposition;
        },
      };
    });

    try {
      if (invalidUrl) {
        await terminalAdmission(
          "invalid", "invalid", requestUrl, "invalid-url-abort", "invalid-url",
        );
      } else if (!READ_ONLY_METHODS.has(requestMethod)) {
        const preflight = requestMethod === "OPTIONS"
          && request.headers()["access-control-request-method"] !== undefined;
        await terminalAdmission(
          preflight ? "synthetic-preflight-escaped-page-policy"
            : "route-policy-rejected",
          "rejected", normalized, "policy-route-abort", "policy-rejected",
          { preflight },
        );
      } else {
        const selected = trace.routes.get(routeKey);
        if (!selected) {
          await terminalAdmission(
            "unmatched", "unmatched", normalized,
            "unmatched-route-abort", "unmatched",
          );
        } else {
          await responseScheduler.enqueueAdmission(identity, () => {
            /* Retained occurrences are claimed only after every HTTP terminal
               request in the Playwright-channel admission wave is sorted. */
            const claim = claimRouteRecord(selected, routeOccurrences, routeKey);
            if (claim.record === null) {
              unexpectedDiagnostics.record(
                claim.exhausted ? "occurrence-exhausted" : claim.reason,
                requestMethod, resourceType, normalized,
              );
              ledger.conflicts += 1;
              if (claim.exhausted) ledger.occurrence_exhausted += 1;
              return {
                record: selected.candidates[0],
                admissionKind: "terminal",
                work: async () => {
                  await runHost(
                    "occurrence-conflict-abort",
                    () => route.abort("blockedbyclient"),
                  );
                  return claim.exhausted ? "occurrence-exhausted" : "unclaimable";
                },
              };
            }
            const record = claim.record;
            if (selected.mode === "occurrence-sequence") ledger.occurrence_claims += 1;
            else ledger.reusable_claims += 1;
            if (record.success && !record.status) {
              unexpectedDiagnostics.record(
                "invalid-record", requestMethod, resourceType, normalized,
              );
              ledger.invalid += 1;
              return {
                record,
                admissionKind: "terminal",
                work: async () => {
                  await runHost(
                    "invalid-record-abort", () => route.abort("blockedbyclient"),
                  );
                  return "invalid-record";
                },
              };
            }
            ledger.matched += 1;
            ledger.claimed.add(record.id);
            return {
              record,
              admissionKind: "retained",
              work: async () => {
                const disposition = await deliverReplayRecord(record, {
                  abort: (code) => runHost(
                    "retained-route-abort", () => route.abort(code),
                  ),
                  applyCookies: () => runHost(
                    "response-add-cookies", () => applyResponseCookies(
                      context, record.cookies, cookieReplayEpochSeconds,
                      trace.originMs, record.responseDateSeconds,
                    ),
                  ),
                  fulfill: () => runHost("response-fulfill", () => route.fulfill({
                    status: record.status,
                    headers: playwrightHeaders(record), body: record.body,
                  })),
                });
                if (hostOperations.isTerminal() || routeLifecycle.phase !== "OPEN") {
                  return "terminal-delivery";
                }
                if (disposition === "rejected") {
                  ledger.rejected += 1;
                } else {
                  ledger.served += 1;
                  ledger.served_resource_types[resourceType] =
                    (ledger.served_resource_types[resourceType] || 0) + 1;
                }
                return disposition;
              },
            };
          });
        }
      }
    } catch (_error) {
      if (!hostOperations.isTerminal() && routeLifecycle.phase === "OPEN") {
        ledger.invalid += 1;
        unexpectedDiagnostics.record(
          "replay-operation-failure", requestMethod, resourceType,
          normalized || requestUrl,
        );
      }
      try {
        await runHost("failed-route-abort", () => route.abort("failed"), {
          allowAfterTerminal: true,
        });
      } catch (_) {}
    }
    if (!hostOperations.isTerminal() && routeLifecycle.phase === "OPEN") {
      ledger.active -= 1;
      activity.value += 1;
    }
  };
  await context.route("**/*", (route) => {
    /* Playwright invokes the user route callback synchronously before its
       first await. Capture phase and Playwright-channel ordinal at that boundary
       so host promise scheduling cannot move a request between generations. */
    const phase = routeLifecycle.phase;
    const browserOrdinal = routeLifecycle.nextBrowserOrdinal++;
    return trackRouteHandler(handleRoute(route, phase, browserOrdinal));
  });
  const recordCapability = (kind, value) => {
    if (routeLifecycle.phase !== "OPEN") {
      routeLifecycle.lateBindingCallbacks += 1;
      return;
    }
    capabilityDiagnostics.record("capability-denied", kind, kind, value);
    recordUnexpected("unmatched", kind, String(kind).toLowerCase(), value);
  };
  await context.exposeBinding("__tilefinchBlockedProtocol", (_source, kind, value) => {
    recordCapability(kind, value);
  });
  await context.exposeBinding("__tilefinchBlockedCapability", (_source, kind, value) => {
    recordCapability(kind, value);
    return null;
  });
  await context.exposeBinding(
    "__tilefinchReadOnlyDenied", (_source, method, kind, value) => {
      if (routeLifecycle.phase !== "OPEN") {
        routeLifecycle.lateBindingCallbacks += 1;
        return null;
      }
      activity.value += 1;
      if (scenario.blockedOrigins.length > 0) {
        let deniedHostname = "";
        try { deniedHostname = new URL(String(value)).hostname.toLowerCase(); }
        catch (_error) {}
        if (blockedHostMatches(deniedHostname, scenario.blockedOrigins)) {
          /* A denied write to a declared blocked host is expected traffic
             suppression, not read-only-policy evidence. */
          ledger.blocked += 1;
          blockedDiagnostics.record("blocked-origin-policy", method, kind, value);
          return null;
        }
      }
      policyDiagnostics.record("policy-denied", method, kind, value);
      if (String(kind).includes("cors-preflight")) preflightPolicyDenials += 1;
      return null;
    },
  );
  await context.addInitScript(installReplayEnvironment, environment);
  await context.addInitScript(installReadOnlyPolicy, {
    version: READ_ONLY_POLICY_VERSION,
    preflightPolicy: READ_ONLY_PREFLIGHT_POLICY,
  });
  await context.addInitScript(installOfflineCapabilityPolicy, {
    version: OFFLINE_CAPABILITY_POLICY_VERSION,
    surfaceVersion: OFFLINE_CAPABILITY_SURFACE_VERSION,
  });
  await context.addInitScript(installOfflineProtocolPolicy);
  const page = await context.newPage();
  /* Diagnostic evidence only: surface page-side failures (script errors,
     failed dynamic imports) in the reference state so stalled SPA
     captures are debuggable.  Bounded; never affects eligibility. */
  const pageErrors = [];
  const recordPageError = (text) => {
    if (pageErrors.length < 32) pageErrors.push(String(text).slice(0, 500));
  };
  page.on("pageerror", (error) => recordPageError(
    error && error.stack ? error.stack : error,
  ));
  page.on("console", (message) => {
    if (message.type() === "error") recordPageError(message.text());
  });
  let navigationResponse = null;
  let failure = null;
  try {
    navigationResponse = await responseScheduler.driveUntilSettled(
      page.goto(scenario.url, {
        waitUntil: "domcontentloaded", timeout: options.timeoutMs,
      }),
      "navigation",
    );
    await responseScheduler.driveUntilSettled(
      page.waitForLoadState("load", { timeout: Math.min(options.timeoutMs, 5000) }),
      "load-state",
    );
    await runHost(
      "initial-settle-boundary", () => responseScheduler.driveUntilSettled(
        settleCapture(page, options.settleMs, () => ledger.active === 0),
        "initial-settle",
      ),
    );
    await runHost(
      "scenario-clock-advance", () => runScenarioClock(
        context, responseScheduler, scenario.ticks, scenario.tickMs,
        hostOperations,
      ),
    );
    await runHost(
      "post-clock-settle-boundary", () => responseScheduler.driveUntilSettled(
        settleCapture(page, options.settleMs, () => ledger.active === 0),
        "post-clock-settle",
      ),
    );
  } catch (error) { failure = `navigation:${error.message}`; }

  const checkpointStates = [];
  if (!failure) {
    for (const [index, checkpoint] of scenario.checkpoints.entries()) {
      try {
        const scrollY = await runHost(
          `checkpoint-${checkpoint.name}-settle-boundary`,
          () => responseScheduler.driveUntilSettled(
            settleCheckpoint(
              page, checkpoint, options.settleMs, () => ledger.active === 0,
            ),
            `checkpoint-${checkpoint.name}`,
          ),
        );
        const filename = checkpoint.kind === "top" ? "top.png"
          : (checkpoint.kind === "bottom" ? "bottom.png" : `${checkpoint.name}.png`);
        const raw = path.join(outputDirectory, `.${filename}.raw.png`);
        const final = path.join(outputDirectory, filename);
        await runHost(
          `screenshot-${checkpoint.name}-boundary`,
          () => responseScheduler.driveUntilSettled(
            page.screenshot({
              path: raw, type: "png", animations: "disabled", caret: "hide",
            }),
            `screenshot-${checkpoint.name}`,
          ),
        );
        await runHost(
          `normalize-${checkpoint.name}`,
          () => normalizePng(
            raw, final, scenario, options.python, options.timeoutMs,
          ),
        );
        checkpointStates.push({
          name: checkpoint.name, kind: checkpoint.kind, target: checkpoint.target,
          scroll_y: scrollY, frame: filename, format: "png-rgb8",
          width: scenario.deviceWidth, height: scenario.deviceHeight,
        });
      } catch (error) { failure = `checkpoint:${checkpoint.name}:${error.message}`; break; }
    }
  }
  if (!failure) {
    try {
      await runHost(
        "evidence-settle-boundary", () => responseScheduler.driveUntilSettled(
          settleCapture(page, options.settleMs, () => ledger.active === 0),
          "evidence-settle",
        ),
      );
      await freezeAndDrainContext(
        context, responseScheduler, ledger, activity, routeLifecycle,
        hostOperations,
      );
    } catch (error) { failure = `evidence:${error.message}`; }
  }
  const emptyPageState = {
    title: "", requiredMarker: false, fallback: false, interstitial: false,
    stylesheets: 0, images: 0, deferredImages: 0, userAgent: "", platform: "",
    innerWidth: 0, innerHeight: 0, devicePixelRatio: 0,
    replayEnvironment: "", replayClock: "", replayIntl: "", readOnlyPolicy: "",
    readOnlyPreflightPolicy: "",
    capabilityPolicy: "", capabilityEvidence: null, performanceEntriesEmpty: false,
    performanceTimeOrigin: 0, clockEvidence: null,
  };
  let pageState = emptyPageState;
  let textMetrics = null;
  if (!failure) {
    try {
      pageState = await evaluateCaptureEvidence(
        page, scenario, hostOperations,
      );
    } catch (error) { failure = `evidence:${error.message}`; }
  }
  if (!failure && textMetricsPath !== null) {
    try {
      textMetrics = await collectReferenceTextMetrics(
        page, scenario, hostOperations,
      );
    } catch (error) { failure = `text-metrics:${error.message}`; }
  }
  if (!failure) {
    try {
      await freezeAndDrainContext(
        context, responseScheduler, ledger, activity, routeLifecycle,
        hostOperations,
      );
    } catch (error) { failure = `evidence:${error.message}`; }
  }
  const browserVersion = browser.version();
  const observedUrlBeforeTeardown = page.url();
  const navigationStatus = navigationResponse === null ? 0 : navigationResponse.status();
  /* Close admission synchronously before the context-close command. Existing
     OPEN tickets may finish, while callbacks captured from this point onward
     can only be bounded and aborted; they cannot mutate qualifying replay
     evidence or claim a retained occurrence. */
  routeLifecycle.phase = "CLOSING";
  try {
    await boundedDrainRouteHandlers(
      hostOperations, responseScheduler, routeLifecycle,
    );
  } catch (error) {
    if (!failure) failure = `teardown:drain:${error.message}`;
  }
  const teardownBaseline = teardownEvidenceSnapshot(
    ledger, responseScheduler, activity, routeLifecycle,
  );
  const teardownChanges = [];
  let teardownReady = false;
  let contextCloseSucceeded = false;
  let browserCloseSucceeded = false;
  try {
    await runHost("context-close", () => context.close(), {
      allowAfterTerminal: true,
    });
    contextCloseSucceeded = true;
    context = null;
  } catch (error) {
    if (!failure) failure = `teardown:context:${error.message}`;
  }
  if (routeLifecycle.handlers.size !== 0) {
    try {
      await runHost(
        "late-route-handler-settle",
        () => Promise.allSettled([...routeLifecycle.handlers]),
        { allowAfterTerminal: true },
      );
    } catch (error) {
      if (!failure) failure = `teardown:handlers:${error.message}`;
    }
  }
  routeLifecycle.phase = "CLOSED";
  await Promise.resolve();
  const teardownFinal = teardownEvidenceSnapshot(
    ledger, responseScheduler, activity, routeLifecycle,
  );
  teardownChanges.push(...teardownEvidenceChanges(
    teardownBaseline, teardownFinal, "context-close",
  ));
  teardownReady = teardownChanges.length === 0
    && teardownEvidenceReady(teardownBaseline)
    && teardownEvidenceReady(teardownFinal);
  if (!teardownReady && !failure) {
    failure = "teardown:late route or binding activity changed final evidence";
  }
  try {
    await runHost("browser-close", () => browser.close(), {
      allowAfterTerminal: true,
    });
    browserCloseSucceeded = true;
    browser = null;
  } catch (error) {
    teardownReady = false;
    if (!failure) failure = `teardown:browser:${error.message}`;
  }
  /* The qualifying state is not assembled or atomically published until the
     page, context, and browser are gone and their final ledgers are stable. */
  for (let pass = 0; pass < 3; pass += 1) {
    await new Promise((resolve) => setImmediate(resolve));
  }
  const teardownAfterBrowser = teardownEvidenceSnapshot(
    ledger, responseScheduler, activity, routeLifecycle,
  );
  teardownChanges.push(...teardownEvidenceChanges(
    teardownFinal, teardownAfterBrowser, "browser-close",
    64 - teardownChanges.length,
  ));
  if (teardownChanges.length !== 0
      || !teardownEvidenceReady(teardownAfterBrowser)) {
    teardownReady = false;
    if (!failure) failure = "teardown:activity arrived after browser closure";
  }
  const hostOperationSummary = hostOperations.close();
  if (!hostOperationSummary.ready || !contextCloseSucceeded
      || !browserCloseSucceeded) {
    teardownReady = false;
    if (!failure) failure = "teardown:host operation boundary is unhealthy";
  }
  const fallback = pageState.fallback;
  const interstitial = pageState.interstitial;
  const unexpectedSummary = unexpectedDiagnostics.summary();
  const policySummary = policyDiagnostics.summary();
  const capabilitySummary = capabilityDiagnostics.summary();
  const blockedSummary = blockedDiagnostics.summary();
  const schedulerSummary = responseScheduler.summary();
  const acquisitionPlan = buildReadOnlyAcquisitionPlan(unexpectedSummary);
  const scripts = ledger.served_resource_types.script || 0;
  const resources = {
    ready: false,
    stylesheets_loaded: pageState.stylesheets,
    images_loaded: pageState.images,
    scripts_loaded: scripts,
    network_completions: ledger.served,
    /* An incomplete loading=lazy image may intentionally remain dormant when
       none of the declared checkpoints approaches it.  It is not active
       network work and must not make an otherwise quiescent capture
       perpetually unready.  Retain the count as diagnostic evidence while
       the qualification-facing pending value mirrors actual replay work. */
    deferred_images: pageState.deferredImages,
    pending: ledger.active,
  };
  const cleanLedger = ledger.requests === ledger.scheduled
    && ledger.scheduled === ledger.matched
    && ledger.matched === ledger.served + ledger.rejected
    && ledger.unmatched === 0 && ledger.conflicts === 0 && ledger.invalid === 0
    && ledger.served > 0;
  const expectedVirtualMs = scenario.ticks * scenario.tickMs;
  const clockEvidence = pageState.clockEvidence;
  const environmentReady = pageState.replayEnvironment === REPLAY_ENVIRONMENT_VERSION
    && pageState.replayClock === REPLAY_CLOCK_CONTRACT
    && pageState.replayIntl === REPLAY_INTL_CONTRACT
    && pageState.performanceEntriesEmpty
    && pageState.performanceTimeOrigin === environment.originMs
    && replayClockEvidenceReady(
      clockEvidence, environment.originMs, expectedVirtualMs,
    );
  const readOnlyPolicyReady = pageState.readOnlyPolicy === READ_ONLY_POLICY_VERSION
    && pageState.readOnlyPreflightPolicy === READ_ONLY_PREFLIGHT_POLICY
    && preflightPolicyDenials === 0 && !policySummary.overflow;
  const capabilityPolicyReady =
    pageState.capabilityPolicy === OFFLINE_CAPABILITY_POLICY_VERSION
    && offlineCapabilityEvidenceReady(pageState.capabilityEvidence)
    && capabilitySummary.total === 0 && !capabilitySummary.overflow;
  const schedulerReady = !schedulerSummary.overflow
    && schedulerSummary.ready === true
    && schedulerSummary.failures === 0
    && schedulerSummary.pending === 0
    && schedulerSummary.enqueued === schedulerSummary.completed
    && schedulerSummary.completed === ledger.scheduled
    && schedulerSummary.retained_admissions === ledger.matched
    && schedulerSummary.terminal_admissions
      === ledger.scheduled - ledger.matched
    && schedulerSummary.pump_work_units
      === schedulerSummary.scheduled_delay_work_units
    && schedulerSummary.scheduled_delay_work_units
      === schedulerSummary.retained_delay_work_units
        + schedulerSummary.terminal_delay_work_units;
  const hostOperationsReady = hostOperationSummary.ready
    && hostOperationSummary.closed === true;
  resources.ready = cleanLedger && environmentReady && readOnlyPolicyReady
    && capabilityPolicyReady && schedulerReady && hostOperationsReady
    && teardownReady
    && resources.pending === 0;
  const expectedScale = scenario.scaleNumerator / scenario.scaleDenominator;
  const viewportReady = pageState.innerWidth === scenario.cssWidth
    && pageState.innerHeight === scenario.cssHeight
    && Math.abs(pageState.devicePixelRatio - expectedScale) < 1e-6;
  const observedUrl = observedUrlBeforeTeardown;
  const captureUrl = /^https?:/i.test(observedUrl) ? normalizeUrl(observedUrl) : "";
  /* SPAs rewrite the address with the history API without loading a new
     document.  The capture is still the declared document when the
     navigated response matches the manifest URL, exactly one document
     request went through replay routing, and the rewritten address stays
     on the same origin. */
  let navigatedUrl = "";
  try {
    if (navigationResponse !== null) {
      navigatedUrl = normalizeUrl(navigationResponse.url());
    }
  } catch (_error) {}
  let captureUrlSameOrigin = false;
  try {
    captureUrlSameOrigin = captureUrl !== ""
      && new URL(captureUrl).origin === new URL(scenario.url).origin;
  } catch (_error) {}
  const documentRequests = ledger.resource_types.document || 0;
  const captureUrlReady = captureUrl === scenario.url
    || (navigatedUrl === scenario.url && documentRequests <= 1
        && captureUrlSameOrigin);
  const navigationReady = navigationStatus === scenario.expectedHttp;
  const eligibilityReasons = [
    ...(failure ? [failure] : []),
    ...(cleanLedger ? [] : ["reference-replay-ledger-unhealthy"]),
    ...(environmentReady ? [] : ["reference-replay-environment-missing"]),
    ...(readOnlyPolicyReady ? [] : ["reference-read-only-policy-unhealthy"]),
    ...(capabilityPolicyReady ? [] : ["reference-capability-policy-unhealthy"]),
    ...(schedulerReady ? [] : ["reference-response-scheduler-unhealthy"]),
    ...(hostOperationsReady ? [] : ["reference-host-operations-unhealthy"]),
    ...(teardownReady ? [] : ["reference-teardown-publication-unhealthy"]),
    ...(viewportReady ? [] : ["reference-viewport-not-observed"]),
    ...(captureUrlReady ? [] : ["reference-capture-url-mismatch"]),
    ...(navigationReady ? [] : ["reference-http-mismatch"]),
    ...(pageState.title.includes(scenario.requiredTitle)
      ? [] : ["reference-title-mismatch"]),
    ...(pageState.requiredMarker ? [] : ["reference-state-marker-missing"]),
    ...(fallback ? ["reference-fallback"] : []),
    ...(interstitial ? ["reference-interstitial"] : []),
    ...(resources.stylesheets_loaded >= scenario.minStylesheets
      ? [] : ["reference-stylesheets-below-minimum"]),
    ...(resources.images_loaded >= scenario.minImages
      ? [] : ["reference-images-below-minimum"]),
    ...(resources.scripts_loaded >= scenario.minScripts
      ? [] : ["reference-scripts-below-minimum"]),
    ...(resources.network_completions >= scenario.minNetwork
      ? [] : ["reference-network-completions-below-minimum"]),
    ...(resources.pending <= scenario.maxPending
      ? [] : ["reference-resources-pending"]),
    ...(checkpointStates.length === scenario.checkpoints.length
      ? [] : ["reference-checkpoints-incomplete"]),
  ];
  const ready = eligibilityReasons.length === 0;
  const persistedLedger = {
    mode: ledger.mode, records: ledger.records, routes: ledger.routes,
    claimed: ledger.claimed.size,
    requests: ledger.requests, scheduled: ledger.scheduled,
    matched: ledger.matched, served: ledger.served,
    rejected: ledger.rejected,
    unmatched: ledger.unmatched, conflicts: ledger.conflicts, invalid: ledger.invalid,
    route_selection_version: ROUTE_SELECTION_VERSION,
    occurrence_claims: ledger.occurrence_claims,
    reusable_claims: ledger.reusable_claims,
    occurrence_exhausted: ledger.occurrence_exhausted,
    shape_mismatches: 0, shape_comparison: "not-collected",
    claimed_routes: [...ledger.claimed].map(Number).sort((a, b) => a - b),
    claimed_route_ranges: compactRanges([...ledger.claimed]),
    unmatched_urls: unexpectedSummary.entries
      .filter((entry) => entry.classification === "unmatched").slice(0, 16)
      .map((entry) => `${entry.method} ${entry.url}`),
    unexpected_requests: unexpectedSummary,
    blocked: ledger.blocked,
    blocked_origins: scenario.blockedOrigins,
    blocked_requests: blockedSummary,
  };
  const topBottomCoincident = scenario.checkpoints.length === 2
    && checkpointStates.length === 2
    && checkpointStates[0].scroll_y === 0 && checkpointStates[1].scroll_y === 0;
  const state = {
    schema: 2, scenario: scenario.name, trace_sha256: scenario.digest,
    url: scenario.url, capture_url: captureUrl,
    capture_transport: "cdp-response-keyed",
    http_status: navigationStatus,
    title: pageState.title,
    state_markers: pageState.requiredMarker ? [scenario.requiredMarker] : [],
    fallback, interstitial, top_bottom_coincident: topBottomCoincident,
    viewport: {
      device: { width: scenario.deviceWidth, height: scenario.deviceHeight },
      css: { width: scenario.cssWidth, height: scenario.cssHeight },
      scale: { numerator: scenario.scaleNumerator, denominator: scenario.scaleDenominator },
      observed: {
        width: pageState.innerWidth, height: pageState.innerHeight,
        device_pixel_ratio: pageState.devicePixelRatio,
      },
    },
    resources, replay_ledger: persistedLedger,
    replay_environment: {
      version: environment.version, origin_ms: environment.originMs,
      clock_version: environment.clockVersion,
      clock_contract: environment.clockContract,
      clock_scope: environment.clockScope,
      rng_version: environment.rngVersion,
      seed_source: environment.seedSource,
      intl_surface: environment.intlContract,
      seed_u64: environment.seedU64,
      seed_sha256: environment.seedSha256,
      ticks: scenario.ticks, tick_ms: scenario.tickMs,
      host_elapsed_ms: clockEvidence && clockEvidence.hostElapsedMs,
      wall_elapsed_ms: clockEvidence && clockEvidence.wallElapsedMs,
      monotonic_elapsed_ms: clockEvidence && clockEvidence.monotonicElapsedMs,
      wall_observations: clockEvidence && clockEvidence.wallObservations,
      monotonic_observations:
        clockEvidence && clockEvidence.monotonicObservations,
      monotonic_samples: clockEvidence && clockEvidence.monotonicSamples,
      clock_sources: clockEvidence && clockEvidence.clockSources,
      performance_entries: "normalized-empty-v1",
      document_timeline: REPLAY_CLOCK_CONTRACT,
      animation_frame: REPLAY_CLOCK_CONTRACT,
    },
    read_only_policy: {
      version: READ_ONLY_POLICY_VERSION,
      preflight_policy: READ_ONLY_PREFLIGHT_POLICY,
      ready: readOnlyPolicyReady,
      denied_before_network: policySummary.total,
      preflight_denied_before_network: preflightPolicyDenials,
      diagnostics: policySummary,
    },
    offline_capability_policy: {
      version: OFFLINE_CAPABILITY_POLICY_VERSION,
      ready: capabilityPolicyReady,
      worker_realms: "api-unavailable-before-page-script",
      shared_worker_realms: "api-unavailable-before-page-script",
      worklet_realms: "api-unavailable-before-module-load",
      shadow_realms: "api-unavailable-before-page-script",
      service_workers: "api-unavailable-and-browser-context-blocked",
      delayed_fetch: "blocked-before-network",
      randomized_webcrypto: "blocked-before-operation",
      surface_evidence: pageState.capabilityEvidence,
      diagnostics: capabilitySummary,
    },
    response_scheduler: { ...schedulerSummary, ready: schedulerReady },
    host_operations: hostOperationSummary,
    publication_boundary: {
      version: "close-then-publish-v1", ready: teardownReady,
      context_closed: contextCloseSucceeded,
      browser_closed: browserCloseSucceeded,
      final_activity: activity.value,
      late_callbacks: teardownAfterBrowser.late_callbacks,
      teardown_changes: teardownChanges,
    },
    acquisition_plan: acquisitionPlan,
    page_errors: pageErrors,
    browser: { engine: options.browser, version: browserVersion, user_agent: pageState.userAgent,
      platform: pageState.platform, locale: "en-US", timezone: "UTC" },
    checkpoints: checkpointStates,
    capture_ready: ready,
    failure,
    eligibility_reasons: eligibilityReasons,
  };
  const stateName = ready ? "reference-state.json" : "reference-diagnostic.json";
  const statePath = path.join(outputDirectory, stateName);
  const temporaryState = path.join(outputDirectory, `.${stateName}.${process.pid}.tmp`);
  fs.writeFileSync(temporaryState, `${JSON.stringify(state, null, 2)}\n`, { mode: 0o600 });
  fs.renameSync(temporaryState, statePath);
  if (ready && textMetricsPath !== null && textMetrics !== null) {
    fs.mkdirSync(path.dirname(textMetricsPath), { recursive: true });
    const temporaryMetrics = `${textMetricsPath}.${process.pid}.tmp`;
    fs.writeFileSync(
      temporaryMetrics, `${JSON.stringify(textMetrics, null, 2)}\n`,
      { mode: 0o600 },
    );
    fs.renameSync(temporaryMetrics, textMetricsPath);
  }
  return { ready, state, output: path.join(outputDirectory, stateName) };
  } finally {
    if (context !== null) {
      try {
        await runHost("context-close-finally", () => context.close(), {
          allowAfterTerminal: true,
        });
      } catch (_error) {}
    }
    if (browser !== null) {
      try {
        await runHost("browser-close-finally", () => browser.close(), {
          allowAfterTerminal: true,
        });
      } catch (_error) {}
    }
  }
}

async function main(argv) {
  const options = parseArguments(argv);
  if (options.help) { process.stdout.write(`${usage()}\n`); return 0; }
  if (options.inspectTrace) {
    process.stdout.write(`${JSON.stringify(inspectTrace(options.inspectTrace), null, 2)}\n`);
    return 0;
  }
  const result = await captureReference(options);
  process.stdout.write(`${JSON.stringify({ ready: result.ready, output: result.output,
    replay_ledger: result.state.replay_ledger }, null, 2)}\n`);
  return result.ready ? 0 : 3;
}

if (require.main === module) {
  main(process.argv.slice(2)).then((code) => { process.exitCode = code; })
    .catch((error) => { process.stderr.write(`reference-capture-error=${error.message}\n`); process.exitCode = 2; });
}

module.exports = {
  CaptureError, applyResponseCookies, buildReadOnlyAcquisitionPlan, compactRanges,
  boundedDrainRouteHandlers,
  claimRouteRecord, cookieReplayOperation, createRequestDiagnostics,
  createHostOperationTracker, createResponseScheduler, inspectTrace,
  deliverReplayRecord, teardownEvidenceChanges, teardownEvidenceReady,
  teardownEvidenceSnapshot,
  evaluateCaptureEvidence, collectReferenceTextMetrics,
  launchBrowser, normalizePng,
  installOfflineCapabilityPolicy, installOfflineProtocolPolicy,
  installReadOnlyPolicy, installReplayEnvironment,
  loadRecord, loadScenario, loadTrace, normalizeUrl,
  offlineCapabilityEvidenceReady,
  parseArguments, parseCookieDate, parseSetCookie, responseCookies,
  replayClockEvidenceReady, replayEnvironment, responseCookieDateBaseline,
  retainedFailureAbortCode,
  runScenarioClock,
  selectRoute, settlePage, traceDigest, traceInfo, traceOriginMs,
};
