#!/usr/bin/env node
"use strict";

/* Verify that an acquired trace is the exact source corpus plus one
 * unambiguous successful route for every digest-authorized plan key.  Record
 * parsing, URL normalization, signatures, and route ranking are delegated to
 * capture-reference.js, the authoritative offline-reference implementation.
 * The validated plan is received on stdin so this gate cannot rediscover or
 * broaden network authority. */

const fs = require("node:fs");
const path = require("node:path");
const reference = require("./capture-reference.js");

const MAX_INPUT_BYTES = 1024 * 1024;
const SHA256 = /^[0-9a-f]{64}$/;
const INTEGER = /^(?:0|[1-9][0-9]*)$/;
const ECMASCRIPT_DATE_MAX_MS = 8640000000000000;

class ClosureError extends Error {}

function parseArguments(argv) {
  const options = Object.create(null);
  for (let index = 0; index < argv.length; index += 2) {
    const option = argv[index];
    const value = argv[index + 1];
    if (!option || !option.startsWith("--") || value === undefined
        || value.startsWith("--") || Object.hasOwn(options, option)) {
      throw new ClosureError("invalid or duplicate closure argument");
    }
    options[option] = value;
  }
  const expected = new Set([
    "--source", "--merged", "--source-sha256", "--merged-sha256",
    "--source-records", "--merged-records", "--origin-ms",
  ]);
  if (Object.keys(options).length !== expected.size
      || Object.keys(options).some((key) => !expected.has(key))) {
    throw new ClosureError("closure arguments differ from the exact contract");
  }
  for (const key of ["--source-sha256", "--merged-sha256"]) {
    if (!SHA256.test(options[key])) throw new ClosureError(`${key} is invalid`);
  }
  for (const key of ["--source-records", "--merged-records", "--origin-ms"]) {
    if (!INTEGER.test(options[key])) throw new ClosureError(`${key} is invalid`);
    options[key] = Number(options[key]);
    if (!Number.isSafeInteger(options[key]) || options[key] <= 0) {
      throw new ClosureError(`${key} is outside the safe positive range`);
    }
    if (key === "--origin-ms" && options[key] > ECMASCRIPT_DATE_MAX_MS) {
      throw new ClosureError(`${key} exceeds the ECMAScript Date range`);
    }
  }
  return options;
}

function readPlan() {
  const input = fs.readFileSync(0);
  if (input.length === 0 || input.length > MAX_INPUT_BYTES) {
    throw new ClosureError("validated acquisition plan exceeds its stdin bound");
  }
  let parsed;
  try { parsed = JSON.parse(input.toString("utf8")); }
  catch (error) { throw new ClosureError(`cannot parse validated plan: ${error.message}`); }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)
      || Object.keys(parsed).length !== 1 || !Array.isArray(parsed.requests)
      || parsed.requests.length === 0) {
    throw new ClosureError("validated plan has an unsupported shape");
  }
  const keys = [];
  const seen = new Set();
  for (const [index, request] of parsed.requests.entries()) {
    if (!request || typeof request !== "object" || Array.isArray(request)
        || !["method,url", "method,request_origin,url"].includes(Object.keys(request).sort().join(","))
        || !["GET", "HEAD"].includes(request.method)
        || typeof request.url !== "string" || !request.url.startsWith("https://")) {
      throw new ClosureError(`validated plan request ${index} is invalid`);
    }
    const url = reference.normalizeUrl(request.url, `validated plan request ${index}`);
    const key = `${request.method}\0${url}`;
    const origin = request.request_origin;
    if (origin !== undefined && (typeof origin !== "string" || origin !== "" &&
        (!origin.startsWith("https://") || new URL(origin).origin !== origin)))
      throw new ClosureError("validated plan has an invalid Origin");
    const variantKey = origin === undefined ? key : `${key}\0${origin}`;
    if (seen.has(variantKey)) throw new ClosureError("validated plan repeats a route key");
    seen.add(variantKey);
    keys.push({ method: request.method, url, key, origin });
  }
  return keys;
}

function routeSnapshot(route) {
  return JSON.stringify({
    rank: route.rank, mode: route.mode, ambiguous: route.ambiguous,
    candidates: route.candidates.map((record) => ({
      id: record.id, method: record.method, url: record.url,
      request_origin: record.requestOrigin,
      success: record.success, status: record.status, signature: record.signature,
    })),
  });
}

function main(argv) {
  const options = parseArguments(argv);
  const planned = readPlan();
  const originVariants = planned.some(request => request.origin !== undefined);
  if (originVariants && planned.some(request => request.origin === undefined))
    throw new ClosureError("validated plan mixes Origin-aware and legacy requests");
  const source = reference.loadTrace(path.resolve(options["--source"]));
  const merged = reference.loadTrace(path.resolve(options["--merged"]));
  if (source.digest !== options["--source-sha256"]
      || merged.digest !== options["--merged-sha256"]
      || source.records.length !== options["--source-records"]
      || merged.records.length !== options["--merged-records"]
      || source.originMs !== options["--origin-ms"]
      || merged.originMs !== options["--origin-ms"]
      || merged.records.length !== source.records.length + planned.length) {
    throw new ClosureError("trace count, origin, or digest differs from authority");
  }
  for (let index = 0; index < source.records.length; index += 1) {
    const before = source.records[index];
    const after = merged.records[index];
    if (!after || before.id !== after.id || before.method !== after.method
        || before.url !== after.url || before.signature !== after.signature
        || before.requestOrigin !== after.requestOrigin) {
      throw new ClosureError(`source record ${before.id} changed during acquisition`);
    }
  }
  for (const [key, route] of source.routes) {
    const retained = merged.routes.get(key);
    const preserved = originVariants && planned.some(request => request.key === key)
      ? [...new Set(route.records.map(record => record.requestOrigin))].every(origin => {
          const before = reference.selectRequestOriginRoute(route, origin);
          const after = reference.selectRequestOriginRoute(retained, origin);
          return before && after && routeSnapshot(before) === routeSnapshot(after);
        }) : retained && routeSnapshot(retained) === routeSnapshot(route);
    if (!preserved) {
      throw new ClosureError("a pre-existing response-keyed route changed");
    }
  }
  for (const [index, request] of planned.entries()) {
    const original = source.routes.get(request.key);
    if (original && (request.origin === undefined
        || reference.selectRequestOriginRoute(original, request.origin))) {
      throw new ClosureError(`planned route already exists: ${request.method} ${request.url}`);
    }
    const whole = merged.routes.get(request.key);
    const route = request.origin === undefined ? whole
      : reference.selectRequestOriginRoute(whole, request.origin);
    const expectedId = String(source.records.length + index).padStart(4, "0");
    if (!route || route.ambiguous || route.rank !== 2 || route.mode !== "reusable"
        || route.candidates.length !== 1 || route.selected !== route.candidates[0]
        || route.candidates[0].id !== expectedId
        || route.candidates[0].method !== request.method
        || route.candidates[0].url !== request.url
        || request.origin !== undefined && route.candidates[0].requestOrigin !== request.origin) {
      throw new ClosureError(`planned route is not uniquely replayable: ${request.method} ${request.url}`);
    }
  }
  const expectedKeys = new Set([...source.routes.keys(), ...planned.map(request => request.key)]);
  if (merged.routes.size !== expectedKeys.size
      || !originVariants && merged.ambiguousRoutes !== source.ambiguousRoutes) {
    throw new ClosureError("acquisition added an unexpected or ambiguous route");
  }
  return {
    schema: originVariants ? 2 : 1,
    ...(originVariants ? {origin_variants_verified:true} : {}),
    verified: true,
    source_records: source.records.length,
    merged_records: merged.records.length,
    acquired_routes: planned.length,
    source_ambiguous_routes: source.ambiguousRoutes,
    merged_ambiguous_routes: merged.ambiguousRoutes,
    source_trace_sha256: source.digest,
    merged_trace_sha256: merged.digest,
    origin_ms: source.originMs,
  };
}

try {
  process.stdout.write(`${JSON.stringify(main(process.argv.slice(2)))}\n`);
} catch (error) {
  process.stderr.write(`trace-acquisition-closure-error: ${error.message}\n`);
  process.exitCode = 1;
}
