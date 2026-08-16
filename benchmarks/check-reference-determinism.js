#!/usr/bin/env node
"use strict";

/* Run the hermetic Chromium capture twice from the same retained trace and
   fail unless the complete state document and every published PNG are equal.
   This wrapper has no acquisition mode and never permits an existing run
   directory, preventing stale artifacts from being mistaken for proof. */

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const childProcess = require("node:child_process");

class DeterminismError extends Error {}

const STATE_BYTE_LIMIT = 4 * 1024 * 1024;
const FRAME_BYTE_LIMIT = 16 * 1024 * 1024;
const SAFE_FRAME_NAME = /^[A-Za-z0-9][A-Za-z0-9_.-]*\.png$/;
const SAFE_SCENARIO_NAME = /^[A-Za-z0-9][A-Za-z0-9_.-]*$/;
const STATE_SCHEMA = 2;
const PROOF_VERSION = "two-full-run-byte-equality-v3";
const REPLAY_ENVIRONMENT = "deterministic-hermetic-v3";
const REFERENCE_CLOCK_VERSION = "playwright-clock-paused-v2";
const CLOCK_CONTRACT = "dual-domain-ms-call-v2";
const CLOCK_SCOPE = "top-level-realm-v1";
const RNG_CONTRACT = "splitmix64-url-scope-v1";
const SEED_SOURCE = "trace-origin-ms-v1";
const INTL_CONTRACT = "bounded-en-us-utc-v1";
const MAX_DATE_MS = 8640000000000000;
const MAX_TICKS = 1000;
const MAX_TICK_MS = 60000;
const MAX_HOST_ELAPSED_MS = MAX_TICKS * MAX_TICK_MS;
const MAX_CLOCK_SOURCE_EVENTS = 1000000;
const MAX_DOMAIN_ELAPSED_MS = MAX_HOST_ELAPSED_MS + MAX_CLOCK_SOURCE_EVENTS;
const DEFAULT_CAPTURE_TIMEOUT_MS = 15000;
const DEFAULT_CAPTURE_SETTLE_MS = 750;
const MIN_CAPTURE_CHILD_DEADLINE_MS = 60000;
const MAX_CAPTURE_CHILD_DEADLINE_MS = 15 * 60 * 1000;
const CAPTURE_CHILD_GRACE_MS = 30000;
const CAPTURE_FIXED_DEADLINE_OPERATIONS = 14;
const CAPTURE_DEADLINE_OPERATIONS_PER_CHECKPOINT = 4;
const CAPTURE_FIXED_SETTLE_PASSES = 4;
const CAPTURE_SETTLE_PASSES_PER_CHECKPOINT = 3;
const WALL_SOURCES = ["date_now", "date_function", "date_constructor"];
const MONOTONIC_OBSERVATION_SOURCES = [
  "performance_now", "performance_mark", "performance_measure",
  "animation_timeline", "idle_deadline_time_remaining",
];
const MONOTONIC_SAMPLE_SOURCES = [
  "animation_frame", "event_timestamp", "intersection_observer",
  "idle_callback_start",
];
const CLOCK_SOURCES = [
  ...WALL_SOURCES, ...MONOTONIC_OBSERVATION_SOURCES, ...MONOTONIC_SAMPLE_SOURCES,
];
const REPLAY_ENVIRONMENT_FIELDS = [
  "version", "origin_ms", "clock_version", "clock_contract", "clock_scope",
  "rng_version", "seed_source", "intl_surface", "seed_u64", "seed_sha256",
  "ticks", "tick_ms", "host_elapsed_ms", "wall_elapsed_ms",
  "monotonic_elapsed_ms", "wall_observations", "monotonic_observations",
  "monotonic_samples", "clock_sources", "performance_entries",
  "document_timeline", "animation_frame",
].sort();

function boundedInteger(value, maximum, positive = false) {
  return Number.isSafeInteger(value) && value >= (positive ? 1 : 0) && value <= maximum;
}

function validateReplayEnvironment(environment) {
  if (!environment || typeof environment !== "object" || Array.isArray(environment)
      || JSON.stringify(Object.keys(environment).sort()) !== JSON.stringify(REPLAY_ENVIRONMENT_FIELDS)) {
    throw new DeterminismError("capture replay environment fields are unsupported");
  }
  if (environment.version !== REPLAY_ENVIRONMENT
      || environment.clock_version !== REFERENCE_CLOCK_VERSION
      || environment.clock_contract !== CLOCK_CONTRACT
      || environment.clock_scope !== CLOCK_SCOPE
      || environment.rng_version !== RNG_CONTRACT
      || environment.seed_source !== SEED_SOURCE
      || environment.intl_surface !== INTL_CONTRACT
      || environment.performance_entries !== "normalized-empty-v1"
      || environment.document_timeline !== CLOCK_CONTRACT
      || environment.animation_frame !== CLOCK_CONTRACT) {
    throw new DeterminismError("capture replay environment contract is unsupported");
  }
  if (!boundedInteger(environment.origin_ms, MAX_DATE_MS, true)
      || environment.seed_u64 !== String(environment.origin_ms)
      || environment.seed_sha256 !== crypto.createHash("sha256")
        .update(`${RNG_CONTRACT}\0${SEED_SOURCE}\0${environment.origin_ms}`).digest("hex")) {
    throw new DeterminismError("capture replay environment seed is invalid");
  }
  if (!boundedInteger(environment.ticks, MAX_TICKS)
      || !boundedInteger(environment.tick_ms, MAX_TICK_MS, true)
      || !boundedInteger(environment.host_elapsed_ms, MAX_HOST_ELAPSED_MS)
      || environment.host_elapsed_ms !== environment.ticks * environment.tick_ms) {
    throw new DeterminismError("capture replay environment host clock is invalid");
  }
  const sources = environment.clock_sources;
  if (!sources || typeof sources !== "object" || Array.isArray(sources)
      || JSON.stringify(Object.keys(sources).sort()) !== JSON.stringify([...CLOCK_SOURCES].sort())
      || CLOCK_SOURCES.some((name) => !boundedInteger(sources[name], MAX_CLOCK_SOURCE_EVENTS))) {
    throw new DeterminismError("capture replay environment clock sources are invalid");
  }
  const total = (names) => names.reduce((sum, name) => sum + sources[name], 0);
  const wall = total(WALL_SOURCES);
  const monotonic = total(MONOTONIC_OBSERVATION_SOURCES);
  const samples = total(MONOTONIC_SAMPLE_SOURCES);
  if (wall + monotonic + samples > MAX_CLOCK_SOURCE_EVENTS
      || environment.wall_observations !== wall
      || environment.monotonic_observations !== monotonic
      || environment.monotonic_samples !== samples
      || !boundedInteger(environment.wall_elapsed_ms, MAX_DOMAIN_ELAPSED_MS)
      || !boundedInteger(environment.monotonic_elapsed_ms, MAX_DOMAIN_ELAPSED_MS)
      || environment.wall_elapsed_ms !== environment.host_elapsed_ms + wall
      || environment.monotonic_elapsed_ms !== environment.host_elapsed_ms + monotonic) {
    throw new DeterminismError("capture replay environment clock closure is invalid");
  }
}

function readRegularFile(filename, byteLimit, label) {
  let before;
  try { before = fs.lstatSync(filename, { bigint: true }); }
  catch (error) { throw new DeterminismError(`${filename}: ${label} cannot be inspected: ${error}`); }
  if (!before.isFile() || before.isSymbolicLink()) {
    throw new DeterminismError(`${filename}: ${label} must be a regular non-symlink file`);
  }
  if (before.size > BigInt(byteLimit)) {
    throw new DeterminismError(`${filename}: ${label} exceeds its byte bound`);
  }
  const noFollow = fs.constants.O_NOFOLLOW || 0;
  let descriptor;
  try { descriptor = fs.openSync(filename, fs.constants.O_RDONLY | noFollow); }
  catch (error) { throw new DeterminismError(`${filename}: ${label} cannot be opened safely: ${error}`); }
  try {
    const opened = fs.fstatSync(descriptor, { bigint: true });
    if (!opened.isFile() || opened.dev !== before.dev || opened.ino !== before.ino) {
      throw new DeterminismError(`${filename}: ${label} changed before it was opened`);
    }
    if (opened.size > BigInt(byteLimit)) {
      throw new DeterminismError(`${filename}: ${label} exceeds its byte bound`);
    }
    const chunks = [];
    let total = 0;
    while (total <= byteLimit) {
      const chunk = Buffer.allocUnsafe(Math.min(64 * 1024, byteLimit + 1 - total));
      const count = fs.readSync(descriptor, chunk, 0, chunk.length, null);
      if (count === 0) break;
      chunks.push(chunk.subarray(0, count));
      total += count;
    }
    const after = fs.fstatSync(descriptor, { bigint: true });
    const pathAfter = fs.lstatSync(filename, { bigint: true });
    if (
      !pathAfter.isFile() || pathAfter.isSymbolicLink()
      || pathAfter.dev !== opened.dev || pathAfter.ino !== opened.ino
      || after.size !== opened.size || after.size !== BigInt(total)
      || after.mtimeNs !== opened.mtimeNs || after.ctimeNs !== opened.ctimeNs
    ) {
      throw new DeterminismError(`${filename}: ${label} changed while read`);
    }
    if (total > byteLimit) {
      throw new DeterminismError(`${filename}: ${label} exceeds its byte bound`);
    }
    return Buffer.concat(chunks, total);
  } finally {
    fs.closeSync(descriptor);
  }
}

function sha256Bytes(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function regularFiles(root, suffix) {
  return fs.readdirSync(root).filter((name) => name.endsWith(suffix)).sort().map((name) => {
    if (suffix === ".png" && !SAFE_FRAME_NAME.test(name)) {
      throw new DeterminismError(`${name}: proof frame name is unsafe`);
    }
    const filename = path.join(root, name);
    const stat = fs.lstatSync(filename);
    if (!stat.isFile() || stat.isSymbolicLink()) {
      throw new DeterminismError(`${filename}: proof artifact must be a regular file`);
    }
    return name;
  });
}

function compareCaptureDirectories(left, right) {
  const stateNames = ["reference-state.json", "reference-diagnostic.json"];
  const leftStates = stateNames.filter((name) => fs.existsSync(path.join(left, name)));
  const rightStates = stateNames.filter((name) => fs.existsSync(path.join(right, name)));
  if (leftStates.length !== 1 || JSON.stringify(leftStates) !== JSON.stringify(rightStates)) {
    throw new DeterminismError("capture runs did not publish the same single state kind");
  }
  const stateName = leftStates[0];
  const leftState = path.join(left, stateName);
  const rightState = path.join(right, stateName);
  const leftStateBytes = readRegularFile(leftState, STATE_BYTE_LIMIT, "proof state");
  const rightStateBytes = readRegularFile(rightState, STATE_BYTE_LIMIT, "proof state");
  const stateSha256 = sha256Bytes(leftStateBytes);
  if (stateSha256 !== sha256Bytes(rightStateBytes) || !leftStateBytes.equals(rightStateBytes)) {
    throw new DeterminismError("capture state bytes differ between full runs");
  }
  const leftFrames = regularFiles(left, ".png");
  const rightFrames = regularFiles(right, ".png");
  if (JSON.stringify(leftFrames) !== JSON.stringify(rightFrames)) {
    throw new DeterminismError("capture frame sets differ between full runs");
  }
  const frames = leftFrames.map((name) => {
    const leftBytes = readRegularFile(path.join(left, name), FRAME_BYTE_LIMIT, "proof frame");
    const rightBytes = readRegularFile(path.join(right, name), FRAME_BYTE_LIMIT, "proof frame");
    const digest = sha256Bytes(leftBytes);
    if (digest !== sha256Bytes(rightBytes) || !leftBytes.equals(rightBytes)) {
      throw new DeterminismError(`${name}: frame bytes differ between full runs`);
    }
    return { name, sha256: digest };
  });
  let state;
  const stateText = leftStateBytes.toString("utf8");
  if (!Buffer.from(stateText, "utf8").equals(leftStateBytes)) {
    throw new DeterminismError("capture state is not canonical UTF-8");
  }
  try { state = JSON.parse(stateText); }
  catch (error) { throw new DeterminismError(`capture state is not valid JSON: ${error}`); }
  if (!state || typeof state !== "object" || Array.isArray(state)) {
    throw new DeterminismError("capture state root must be an object");
  }
  /* capture-reference.js publishes this exact canonical serialization. Requiring
     it here rejects duplicate keys (which JSON.parse would otherwise collapse),
     invalid UTF-8 replacement, and noncanonical hostile retained state before a
     v3 proof can be issued. */
  if (`${JSON.stringify(state, null, 2)}\n` !== stateText) {
    throw new DeterminismError("capture state is not canonical duplicate-free JSON");
  }
  if (state.schema !== STATE_SCHEMA) {
    throw new DeterminismError("capture state schema is unsupported");
  }
  if (typeof state.capture_ready !== "boolean") {
    throw new DeterminismError("capture readiness is missing");
  }
  const expectedStateName = state.capture_ready
    ? "reference-state.json" : "reference-diagnostic.json";
  if (stateName !== expectedStateName) {
    throw new DeterminismError("capture state filename does not match capture readiness");
  }
  if (state.capture_ready
      && (state.failure !== null
          || !Array.isArray(state.eligibility_reasons)
          || state.eligibility_reasons.length !== 0)) {
    throw new DeterminismError("capture-ready state retains failure evidence");
  }
  if (typeof state.scenario !== "string" || !state.scenario) {
    throw new DeterminismError("capture state scenario is missing");
  }
  if (typeof state.trace_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(state.trace_sha256)) {
    throw new DeterminismError("capture state trace digest is missing");
  }
  validateReplayEnvironment(state.replay_environment);
  if (!Array.isArray(state.checkpoints)) {
    throw new DeterminismError("capture checkpoints are missing");
  }
  const checkpointFrames = state.checkpoints.map((checkpoint) => {
    if (!checkpoint || typeof checkpoint !== "object" || Array.isArray(checkpoint)
        || typeof checkpoint.frame !== "string" || !SAFE_FRAME_NAME.test(checkpoint.frame)) {
      throw new DeterminismError("capture checkpoint frame name is unsafe");
    }
    return checkpoint.frame;
  }).sort();
  if (new Set(checkpointFrames).size !== checkpointFrames.length
      || JSON.stringify(checkpointFrames) !== JSON.stringify(leftFrames)) {
    throw new DeterminismError("capture checkpoint frame set differs from published PNGs");
  }
  return {
    version: PROOF_VERSION,
    equivalent: true,
    capture_ready: state.capture_ready === true,
    scenario: state.scenario,
    trace_sha256: state.trace_sha256,
    canonical_run: "run-a",
    comparison_run: "run-b",
    state_schema: STATE_SCHEMA,
    replay_environment: state.replay_environment,
    state: { name: stateName, sha256: stateSha256 },
    frames,
  };
}

function outputRootArguments(argv) {
  const forwarded = [];
  let root = null;
  for (let index = 0; index < argv.length; index += 1) {
    if (argv[index] === "--output-root") {
      if (root !== null || index + 1 >= argv.length || argv[index + 1].startsWith("--")) {
        throw new DeterminismError("--output-root must appear once with a value");
      }
      root = path.resolve(argv[++index]);
    } else {
      forwarded.push(argv[index]);
    }
  }
  if (root === null) throw new DeterminismError("--output-root is required");
  return { root, forwarded };
}

function validateCaptureResult(status, result, outputRoot) {
  if (![0, 3].includes(status)) {
    throw new DeterminismError(`capture exited ${status}`);
  }
  if (!result || typeof result !== "object" || Array.isArray(result)
      || JSON.stringify(Object.keys(result).sort())
        !== JSON.stringify(["output", "ready", "replay_ledger"])) {
    throw new DeterminismError("capture result fields are unsupported");
  }
  if (typeof result.ready !== "boolean") {
    throw new DeterminismError("capture result readiness is missing");
  }
  if (status !== (result.ready ? 0 : 3)) {
    throw new DeterminismError("capture exit status disagrees with result readiness");
  }
  if (typeof result.output !== "string" || !path.isAbsolute(result.output)) {
    throw new DeterminismError("capture result output is not an absolute path");
  }
  const resolvedRoot = path.resolve(outputRoot);
  const resolvedOutput = path.resolve(result.output);
  const relative = path.relative(resolvedRoot, resolvedOutput);
  const parts = relative.split(path.sep);
  const stateName = result.ready
    ? "reference-state.json" : "reference-diagnostic.json";
  if (relative === "" || path.isAbsolute(relative) || relative.startsWith(`..${path.sep}`)
      || parts.length !== 2 || !SAFE_SCENARIO_NAME.test(parts[0])
      || parts[1] !== stateName
      || resolvedOutput !== path.join(resolvedRoot, parts[0], stateName)) {
    throw new DeterminismError("capture result output does not name its state artifact");
  }
  return {
    scenario: parts[0],
    stateName,
    output: resolvedOutput,
  };
}

function validateProofCaptureBinding(proof, left, right) {
  if (!proof || typeof proof !== "object" || Array.isArray(proof)) {
    throw new DeterminismError("capture proof is malformed");
  }
  const expectedStatus = proof.capture_ready ? 0 : 3;
  const expectedStateName = proof.capture_ready
    ? "reference-state.json" : "reference-diagnostic.json";
  for (const capture of [left, right]) {
    if (capture.status !== expectedStatus
        || capture.result.ready !== proof.capture_ready
        || capture.scenario !== proof.scenario
        || capture.stateName !== expectedStateName
        || capture.stateName !== proof.state.name
        || capture.output !== capture.result.output) {
      throw new DeterminismError(
        "capture child status/result/output disagrees with parsed state proof",
      );
    }
  }
}

function lastForwardedValue(forwarded, option) {
  let result = null;
  for (let index = 0; index < forwarded.length; index += 1) {
    if (forwarded[index] === option && index + 1 < forwarded.length
        && !forwarded[index + 1].startsWith("--")) {
      result = forwarded[index + 1];
      index += 1;
    }
  }
  return result;
}

function captureChildDeadlineMs(forwarded) {
  if (!Array.isArray(forwarded)
      || forwarded.some((value) => typeof value !== "string")) {
    throw new DeterminismError("capture child arguments are invalid");
  }
  const numericOption = (name, fallback, minimum, maximum) => {
    const raw = lastForwardedValue(forwarded, name);
    if (raw === null) return fallback;
    const value = Number(raw);
    return Number.isSafeInteger(value) && value >= minimum && value <= maximum
      ? value : fallback;
  };
  const timeoutMs = numericOption(
    "--timeout-ms", DEFAULT_CAPTURE_TIMEOUT_MS, 1000, 120000,
  );
  const settleMs = numericOption(
    "--settle-ms", DEFAULT_CAPTURE_SETTLE_MS, 0, 10000,
  );
  let checkpoints = 3;
  const scenarioName = lastForwardedValue(forwarded, "--scenario");
  if (scenarioName !== null) {
    const manifestValue = lastForwardedValue(forwarded, "--manifest");
    const manifest = manifestValue === null
      ? path.join(__dirname, "visual-scenarios.tsv") : path.resolve(manifestValue);
    try {
      const capture = require("./capture-reference.js");
      checkpoints = capture.loadScenario(manifest, scenarioName).checkpoints.length;
    } catch (_error) {
      /* Invalid capture arguments fail immediately in the child. Retain a
         conservative three-checkpoint watchdog instead of making this proof
         wrapper a second, subtly different manifest parser. */
      checkpoints = 3;
    }
  }
  const operationCount = CAPTURE_FIXED_DEADLINE_OPERATIONS
    + checkpoints * CAPTURE_DEADLINE_OPERATIONS_PER_CHECKPOINT;
  const settlePasses = CAPTURE_FIXED_SETTLE_PASSES
    + checkpoints * CAPTURE_SETTLE_PASSES_PER_CHECKPOINT;
  const derived = CAPTURE_CHILD_GRACE_MS
    + operationCount * timeoutMs + settlePasses * settleMs;
  return Math.min(
    MAX_CAPTURE_CHILD_DEADLINE_MS,
    Math.max(MIN_CAPTURE_CHILD_DEADLINE_MS, derived),
  );
}

function runCaptureChild(executable, args, timeoutMs) {
  if (typeof executable !== "string" || executable.length === 0
      || !Array.isArray(args) || args.some((value) => typeof value !== "string")
      || !Number.isSafeInteger(timeoutMs) || timeoutMs < 1
      || timeoutMs > MAX_CAPTURE_CHILD_DEADLINE_MS) {
    throw new DeterminismError("capture child deadline invocation is invalid");
  }
  const completed = childProcess.spawnSync(executable, args, {
    encoding: "utf8", maxBuffer: 16 * 1024 * 1024,
    timeout: timeoutMs, killSignal: "SIGKILL",
  });
  if (completed.error) {
    if (completed.error.code === "ETIMEDOUT") {
      throw new DeterminismError(
        `capture exceeded ${timeoutMs}ms end-to-end deadline and was terminated`,
      );
    }
    throw new DeterminismError(`capture child could not execute: ${completed.error.message}`);
  }
  if (completed.signal) {
    throw new DeterminismError(`capture child was terminated by ${completed.signal}`);
  }
  return completed;
}

function captureOnce(forwarded, outputRoot) {
  fs.mkdirSync(outputRoot, { recursive: false, mode: 0o700 });
  const capture = path.join(__dirname, "capture-reference.js");
  const completed = runCaptureChild(
    process.execPath,
    [capture, ...forwarded, "--output-root", outputRoot],
    captureChildDeadlineMs(forwarded),
  );
  if (![0, 3].includes(completed.status)) {
    throw new DeterminismError(
      `capture exited ${completed.status}: ${completed.stderr || completed.stdout}`,
    );
  }
  let result;
  try { result = JSON.parse(completed.stdout); }
  catch (_error) { throw new DeterminismError("capture did not return bounded JSON evidence"); }
  if (`${JSON.stringify(result, null, 2)}\n` !== completed.stdout) {
    throw new DeterminismError("capture did not return canonical duplicate-free JSON evidence");
  }
  return {
    status: completed.status,
    result,
    ...validateCaptureResult(completed.status, result, outputRoot),
  };
}

function main(argv) {
  const { root, forwarded } = outputRootArguments(argv);
  if (fs.existsSync(root)) {
    throw new DeterminismError("determinism proof output root must not already exist");
  }
  fs.mkdirSync(root, { recursive: false, mode: 0o700 });
  const leftRoot = path.join(root, "run-a");
  const rightRoot = path.join(root, "run-b");
  const left = captureOnce(forwarded, leftRoot);
  const right = captureOnce(forwarded, rightRoot);
  if (left.status !== right.status || left.result.ready !== right.result.ready) {
    throw new DeterminismError("full runs disagree on capture qualification");
  }
  const scenario = left.scenario;
  if (scenario !== right.scenario) {
    throw new DeterminismError("capture runs disagree on scenario output");
  }
  const proof = {
    ...compareCaptureDirectories(
      path.join(leftRoot, scenario), path.join(rightRoot, scenario),
    ),
  };
  if (proof.scenario !== scenario) {
    throw new DeterminismError("capture state scenario differs from output scenario");
  }
  validateProofCaptureBinding(proof, left, right);
  const proofPath = path.join(root, "determinism-proof.json");
  const temporary = `${proofPath}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, `${JSON.stringify(proof, null, 2)}\n`, { mode: 0o600 });
  fs.renameSync(temporary, proofPath);
  process.stdout.write(`${JSON.stringify({ ...proof, output: proofPath }, null, 2)}\n`);
  return proof.capture_ready ? 0 : 3;
}

if (require.main === module) {
  try { process.exitCode = main(process.argv.slice(2)); }
  catch (error) {
    process.stderr.write(`reference-determinism-error=${error.message}\n`);
    process.exitCode = 2;
  }
}

module.exports = {
  DeterminismError,
  captureChildDeadlineMs,
  compareCaptureDirectories,
  outputRootArguments,
  runCaptureChild,
  validateCaptureResult,
  validateProofCaptureBinding,
};
