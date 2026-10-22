"use strict";

/* Playwright normally resumes workers immediately on attachment. A dedicated
   CDP transport gate holds only those resume commands until our independent
   debugger session has installed the policies at the first-script boundary.
   All requests still pass through the ordinary retained-response interceptor. */
async function launchWorkerBrowser(playwright, launch) {
  const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
  const http = require("node:http"), crypto = require("node:crypto");
  const {fork} = require("node:child_process");
  const {ws:Socket, wsServer:Server} = require("playwright-core/lib/utilsBundle");
  if (!Socket || !Server) throw new Error("worker capture requires the matching Playwright CDP transport");
  const driver = {};
  for (const name of ["utilsBundle.js", "coreBundle.js"]) {
    const file = path.join(path.dirname(require.resolve("playwright-core/package.json")), "lib", name);
    if (fs.statSync(file).size > 64*1024*1024) throw new Error("capture driver exceeds identity bound");
    driver[name] = crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
  }
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "tilefinch-worker-browser-"));
  const executable = launch.executablePath || playwright.chromium.executablePath();
  if (process.platform === "win32") {
    fs.rmSync(directory, {recursive:true, force:true});
    throw new Error("isolated worker capture currently requires POSIX process ownership");
  }
  const child = fork(__filename, ["--own-browser"], {stdio:["ignore", "ignore", "ignore", "ipc"]});
  child.send({executable, directory, args:[...launch.args, "--headless", "--no-startup-window",
    "--remote-debugging-port=0", "--remote-debugging-address=127.0.0.1", "--user-data-dir="+directory]});
  let launchError = null;
  child.on("error", error => {launchError=error;});
  let server, proxy, upstream, client, browser, closed = false, forwarded = 0, heldCount = 0;
  const workers = new Map(), held = new Map();
  const cleanup = async () => {
    if (closed) return;
    closed = true;
    if (client) client.terminate();
    if (upstream) upstream.terminate();
    if (proxy) proxy.close();
    if (server) server.close();
    if (child.exitCode === null) child.kill("SIGTERM");
    await new Promise(resolve => {
      if (child.exitCode !== null) return resolve();
      const timer = setTimeout(() => {child.kill("SIGKILL"); resolve();}, 2000);
      child.once("exit", () => {clearTimeout(timer); resolve();});
    });
    fs.rmSync(directory, {recursive:true, force:true});
  };
  try {
    const deadline = Date.now()+10000;
    let port, endpoint;
    while (Date.now() < deadline) {
      if (child.exitCode !== null) throw new Error("isolated worker browser exited before startup");
      if (launchError) throw launchError;
      try {
        const values = fs.readFileSync(path.join(directory,"DevToolsActivePort"),"utf8").trim().split("\n");
        if (/^[0-9]+$/.test(values[0]) && /^\/devtools\/browser\/[a-zA-Z0-9-]+$/.test(values[1])) {
          port = Number(values[0]); endpoint = values[1]; break;
        }
      } catch (_) {}
      await new Promise(resolve => setTimeout(resolve,20));
    }
    if (!port || port > 65535) throw new Error("isolated worker browser did not publish a local endpoint");
    server = http.createServer((_request,response) => {response.writeHead(404);response.end();});
    const route = "/"+crypto.randomBytes(24).toString("hex");
    proxy = new Server({server, path:route, maxPayload:96*1024*1024});
    proxy.on("connection", socket => {
      if (client) {socket.terminate(); return;}
      client = socket;
      upstream = new Socket(`ws://127.0.0.1:${port}${endpoint}`, {maxPayload:96*1024*1024});
      const queue = []; let queueBytes = 0, received = 0;
      const send = message => {
        if (forwarded >= 1000000) throw new Error("CDP message bound exceeded");
        const value = JSON.parse(message.toString());
        if (value.method === "Runtime.runIfWaitingForDebugger" && workers.has(value.sessionId)) {
          if (++heldCount > 64) throw new Error("worker resume gate exceeds bound");
          held.set(value.id, {message:message.toString(), target:workers.get(value.sessionId)});
        } else {forwarded++; upstream.send(message.toString());}
      };
      socket.on("message", message => {
        try {
          if (upstream.readyState !== Socket.OPEN) {
            if (queue.length >= 128 || message.length > 2097152 - queueBytes)
              throw new Error("CDP startup queue exceeds bound");
            queueBytes += message.length;
            queue.push(message);
          } else send(message);
        } catch (_) {socket.terminate();upstream.terminate();}
      });
      upstream.on("open", () => {
        try {for (const message of queue) send(message);queue.length=0;queueBytes=0;}
        catch (_) {socket.terminate();upstream.terminate();}
      });
      upstream.on("message", message => {
        try {
          if (++received > 1000000) throw new Error("CDP event bound exceeded");
          const value = JSON.parse(message.toString());
          if (value.method === "Target.attachedToTarget" && value.params.targetInfo.type === "worker") {
            if (workers.size >= 64) throw new Error("CDP worker-session bound exceeded");
            workers.set(value.params.sessionId,value.params.targetInfo.targetId);
          }
          if (value.method === "Target.detachedFromTarget") workers.delete(value.params.sessionId);
          socket.send(message.toString());
        } catch (_) {socket.terminate();upstream.terminate();}
      });
      upstream.on("error", () => socket.terminate());
      socket.on("error", () => upstream.terminate());
      socket.on("close", () => upstream.terminate());
    });
    await new Promise((resolve,reject) => {server.once("error",reject);server.listen(0,"127.0.0.1",resolve);});
    browser = await playwright.chromium.connectOverCDP(`ws://127.0.0.1:${server.address().port}${route}`, {timeout:10000});
    return {browser, cleanup, release:target => {
      for (const [id, value] of held) if (value.target === target) {
        held.delete(id); forwarded++; upstream.send(value.message);
      }
    }, evidence:() => ({version:"dedicated-worker-resume-gate-v1",held:held.size,held_total:heldCount,forwarded,
      driver_sha256:driver, profile_removed:closed && !fs.existsSync(directory)})};
  } catch (error) {await cleanup(); throw error;}
}

/* These timers run only when the host advances the recorded clock. Keep the
   native task checkpoint private so author timers cannot escape to wall time. */
function installWorkerClock() {
  const Channel = MessageChannel, timers = new Map();
  let now = 0, nextId = 0, callbacks = 0, messages = 0;
  const schedule = (repeat, callback, delay, args) => {
    if (timers.size >= 256) {
      __tilefinchBlockedCapability("WORKER-TIMER-QUOTA", "");
      throw new RangeError("capture worker timer bound exceeded");
    }
    const id = ++nextId;
    delay = Math.min(2147483647, Math.max(repeat ? 1 : 0, Number(delay) || 0));
    timers.set(id, {id, due:now + delay, repeat:repeat ? delay : 0, callback, args});
    return id;
  };
  globalThis.setTimeout = (fn, delay, ...args) => schedule(false, fn, delay, args);
  globalThis.setInterval = (fn, delay, ...args) => schedule(true, fn, delay, args);
  globalThis.clearTimeout = globalThis.clearInterval = id => timers.delete(Number(id));
  const post = globalThis.postMessage;
  globalThis.postMessage = new Proxy(post, {apply(target, receiver, args) {
    messages++;
    return Reflect.apply(target, receiver, args);
  }});
  const checkpoint = () => new Promise(resolve => {
    const channel = new Channel();
    channel.port1.onmessage = () => {
      channel.port1.close(); channel.port2.close();
      resolve({clock:__tilefinchReplayClockEvidence(), timers:timers.size, callbacks, messages});
    };
    channel.port2.postMessage(null);
  });
  Object.defineProperties(globalThis, {
    __tilefinchWorkerCheckpoint:{value:checkpoint},
    __tilefinchAdvanceWorkerClock:{value:async milliseconds => {
      if (!Number.isSafeInteger(milliseconds) || milliseconds < 0)
        throw new TypeError("invalid capture worker clock advance");
      __tilefinchAdvanceReplayClock(milliseconds);
      const end = now + milliseconds;
      for (let count = 0; ; count++) {
        let first = null;
        for (const timer of timers.values())
          if (timer.due <= end && (!first || timer.due < first.due
              || (timer.due === first.due && timer.id < first.id))) first = timer;
        if (!first) break;
        if (count >= 4096) throw new RangeError("capture worker callback bound exceeded");
        now = first.due;
        if (first.repeat) first.due += first.repeat;
        else timers.delete(first.id);
        callbacks++;
        if (typeof first.callback === "function") Reflect.apply(first.callback, globalThis, first.args);
        else (0, eval)(String(first.callback));
        await Promise.resolve();
      }
      now = end;
      return checkpoint();
    }},
  });
}

function createWorkerCapture({runHost, environment, scenarioUrl, preflightPolicy, releaseTarget, terminateWorkers,
  installReplayEnvironment, installReadOnlyPolicy, installOfflineCapabilityPolicy,
  installOfflineProtocolPolicy, validateClock, report, fail}) {
  const workers = [];
  let frozen = false, failures = 0;
  const failed = error => {failures++; fail(error);};
  const evaluate = async (worker, expression) => {
    const result = await runHost("worker-runtime-evaluate", () => worker.session.send(
      worker.entryFrame ? "Debugger.evaluateOnCallFrame" : "Runtime.evaluate",
      worker.entryFrame ? {callFrameId:worker.entryFrame, expression, returnByValue:true}
        : {expression, awaitPromise:true, returnByValue:true}));
    if (result.exceptionDetails) throw new Error("capture worker evaluation failed: "
      + (result.exceptionDetails.exception?.description || result.exceptionDetails.text));
    return result.result?.value;
  };
  const verify = (worker, value) => {
    if (!value || !validateClock(value.clock, environment.originMs, worker.elapsed, environment.clockContract,
        "dedicated-worker-realm-v1") || !Number.isInteger(value.timers) || value.timers < 0
        || value.timers > 256 || !Number.isSafeInteger(value.callbacks) || value.callbacks < 0
        || !Number.isSafeInteger(value.messages) || value.messages < 0)
      throw new Error("capture worker clock/task evidence is invalid");
    worker.evidence = value;
    return value;
  };
  return {
    async setup(session, info) {
      if (frozen || workers.length >= 16) throw new Error("capture worker admission bound exceeded");
      let ready, detached;
      const worker = {session, elapsed:0, initialized:false, running:false, frozen:false, evidence:null,
        ready:new Promise(resolve => {ready=resolve;}), detached:new Promise(resolve => {detached=resolve;})};
      session._captureWorkerDetached = () => {worker.frozen = frozen; detached();};
      workers.push(worker);
      let paused;
      const firstScript = new Promise(resolve => {paused = resolve;});
      session.on("Debugger.paused", paused);
      await runHost("worker-debugger-entry-enable", () => session.send("Debugger.enable"));
      const breakpoint = await runHost("worker-debugger-entry-break", () => session.send(
        "Debugger.setInstrumentationBreakpoint", {instrumentation:"beforeScriptExecution"}));
      await runHost("worker-debugger-entry-start", () => session.send("Runtime.runIfWaitingForDebugger"));
      const entry = await runHost("worker-debugger-entry-pause", () => firstScript);
      session.removeListener("Debugger.paused", paused);
      if (entry.reason !== "instrumentation") throw new Error("worker did not stop before its first script");
      worker.entryFrame = entry.callFrames[0]?.callFrameId;
      if (!worker.entryFrame) throw new Error("worker entry frame is unavailable");
      await runHost("worker-debugger-entry-remove", () => session.send("Debugger.removeBreakpoint", {breakpointId:breakpoint.breakpointId}));
      session.on("Runtime.bindingCalled", event => {
        try {
          if (event.name !== "__tilefinchWorkerReport" || event.payload.length > 8192)
            throw new Error("capture worker binding is invalid");
          const values = JSON.parse(event.payload);
          if (!Array.isArray(values) || values.length !== 4
              || values.some(v => typeof v !== "string" || v.length > 2048))
            throw new Error("capture worker report exceeds bounds");
          report(...values);
        } catch (error) {failed(error);}
      });
      session.on("Runtime.exceptionThrown", () => failed(new Error("unhandled capture worker exception")));
      await runHost("worker-runtime-enable", () => session.send("Runtime.enable"));
      await runHost("worker-report-binding", () => session.send("Runtime.addBinding", {name:"__tilefinchWorkerReport"}));
      const config = {...environment, workerClock:true, clockScope:"dedicated-worker-realm-v1",
        workerScope:scenarioUrl + "#capture-worker-" + workers.length};
      const bootstrap = `(() => {
        const report = __tilefinchWorkerReport;
        globalThis.__tilefinchBlockedProtocol=(kind,value)=>report(JSON.stringify(["capability",String(kind),"",String(value)]));
        globalThis.__tilefinchBlockedCapability=globalThis.__tilefinchBlockedProtocol;
        globalThis.__tilefinchReadOnlyDenied=(method,kind,value)=>report(JSON.stringify(["policy",String(method),String(kind),String(value)]));
        (${installReplayEnvironment})(${JSON.stringify(config)});
        (${installReadOnlyPolicy})(${JSON.stringify({version:"get-head-only-v3", preflightPolicy})});
        (${installOfflineCapabilityPolicy})(${JSON.stringify({version:"offline-capabilities-v2",surfaceVersion:"realm-entrypoints-unavailable-v1"})});
        (${installOfflineProtocolPolicy})();
        (${installWorkerClock})();
        return {installed:__tilefinchReplayEnvironment,clock:__tilefinchReplayClockEvidence()};
      })()`;
      const initial = await evaluate(worker, bootstrap);
      if (initial?.installed !== environment.version
          || !validateClock(initial.clock, environment.originMs, 0, environment.clockContract,
            "dedicated-worker-realm-v1")) throw new Error("worker policy installation failed");
      worker.initialized = true;
      return async () => {
        await runHost("worker-debugger-entry-resume", () => session.send("Debugger.resume"));
        worker.entryFrame = null;
        releaseTarget(info.targetId);
        worker.running = true;
        ready();
      };
    },
    async advance(milliseconds) {
      for (const worker of workers) {
        if (!worker.running) continue; // A newly created realm joins the next logical tick.
        if (!worker.initialized || worker.frozen) throw new Error("capture worker is not advanceable");
        worker.elapsed += milliseconds;
        verify(worker, await evaluate(worker, `__tilefinchAdvanceWorkerClock(${milliseconds})`));
      }
    },
    async checkpoint() {
      for (const worker of workers) {
        await runHost("worker-ready-checkpoint", () => worker.ready);
        const before = verify(worker, await evaluate(worker, "__tilefinchWorkerCheckpoint()"));
        const after = verify(worker, await evaluate(worker, "__tilefinchWorkerCheckpoint()"));
        if (before.timers || after.timers || before.messages !== after.messages)
          throw new Error("capture worker has pending timers or message publication");
      }
    },
    async freeze() {
      await this.checkpoint();
      frozen = true;
      await terminateWorkers();
      for (const worker of workers) await runHost("worker-confirm-termination", () => worker.detached);
    },
    summary:() => ({version:"paused-dedicated-workers-v1", termination:"confirmed-target-detach-v1", admitted:workers.length, failures,
      frozen, ready:frozen && failures === 0 && workers.every(w => w.initialized && w.frozen && w.evidence?.timers === 0),
      realms:workers.map(w => ({elapsed_ms:w.elapsed, ...w.evidence}))}),
  };
}

module.exports = {createWorkerCapture, launchWorkerBrowser};

/* The IPC owner survives an abrupt capture-process kill. Closing its parent
   pipe terminates only this browser's process group and removes its profile. */
if (require.main === module && process.argv[2] === "--own-browser") {
  process.once("message", ({executable, directory, args}) => {
    const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
    const parent = fs.realpathSync(path.dirname(directory));
    if (parent !== fs.realpathSync(os.tmpdir())
        || !/^tilefinch-worker-browser-[a-zA-Z0-9]+$/.test(path.basename(directory))
        || fs.lstatSync(directory).isSymbolicLink()) process.exit(1);
    const browser = require("node:child_process").spawn(executable, args, {detached:true, stdio:"ignore"});
    let stopping = false, timer;
    const kill = signal => {if (browser.pid) try {process.kill(-browser.pid, signal);} catch (_) {}};
    const stop = () => {
      if (stopping) return;
      stopping = true; kill("SIGTERM");
      timer = setTimeout(() => kill("SIGKILL"), 1000);
    };
    const finish = code => {
      clearTimeout(timer);
      fs.rmSync(directory, {recursive:true, force:true});
      process.exit(code);
    };
    process.once("disconnect", stop);
    process.once("SIGTERM", stop);
    browser.once("exit", () => finish(0));
    browser.once("error", () => finish(1));
    if (!process.connected) stop();
  });
}
