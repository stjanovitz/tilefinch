/* PBKDF2 derivations of the page, a frame and a worker. scopeCase picks
   the case (test_web_crypto.c):
   - "terminated": a worker starts 16 long derivations and is terminated;
     the page's own derivation then runs.
   - "busy": a worker keeps 20 long derivations pending; the page's own
     derivation still finishes.
   - "frame", "frame-removed", "frame-left", "frame-closed": a frame
     starts 20 long derivations (16 running, 4 waiting), then its document
     goes: it navigates to another srcdoc (its realm is replaced, so they
     are dropped), it is removed, or the host commits a cross-origin
     document in it or closes it (test_web_crypto.c, on the frame:<handle>
     summary); its realm stays, so they reject. The page's own derivation
     then runs.
   - "parallel": the page starts 20 derivations at once; all of them
     finish with their own bytes while timers keep running. */
(async () => {
  pocSummary = "pending";
  const subtle = crypto.subtle, enc = new TextEncoder(),
    hex = (bytes) => [...new Uint8Array(bytes)]
      .map((v) => v.toString(16).padStart(2, "0")).join(""),
    later = (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
    key = await subtle.importKey("raw", enc.encode("pw"), "PBKDF2", false,
      ["deriveBits"]),
    derive = (salt, iterations) => subtle.deriveBits({ name: "PBKDF2",
      hash: "SHA-256", salt: enc.encode(salt), iterations }, key, 256),
    /* Long derivations in another scope, which report how they settle. */
    longJobs = (count) => "const s=crypto.subtle,e=new TextEncoder();"
      + "s.importKey('raw',e.encode('pw'),'PBKDF2',false,['deriveBits'])"
      + ".then(k=>{for(let i=0;i<" + count + ";i++)s.deriveBits({name:"
      + "'PBKDF2',hash:'SHA-256',salt:e.encode('s'+i),iterations:10000000},"
      + "k,256).then(()=>report('settled'),e=>report(e.name));"
      + "report('started')});";
  if (scopeCase === "terminated" || scopeCase === "busy") {
    const url = URL.createObjectURL(new Blob(
      ["const report=m=>postMessage(m);" + longJobs(
        scopeCase === "busy" ? 20 : 16)])),
      worker = new Worker(url), heard = [];
    await new Promise((resolve) => {
      worker.onmessage = (event) => {
        heard.push(event.data);
        resolve();
      };
    });
    if (scopeCase === "terminated") worker.terminate();
    const own = await derive("page", 600000).then(hex, (e) => e.name);
    await later(20);
    worker.terminate();
    URL.revokeObjectURL(url);
    pocSummary = heard.join() + ":" + own;
  } else if (scopeCase.startsWith("frame")) {
    const frame = document.createElement("iframe");
    document.body.appendChild(frame);
    const load = (srcdoc) => new Promise((resolve) => {
      frame.onload = resolve;
      frame.srcdoc = srcdoc;
    });
    await load("<p>one</p>");
    const view = frame.contentWindow, heard = [];
    globalThis.frameReport = (m) => heard.push(m);
    view.eval("const report=m=>parent.frameReport(m);" + longJobs(20));
    while (!heard.length) await later(1);
    const own = view.crypto !== crypto && view.crypto instanceof Crypto;
    if (scopeCase === "frame") await load("<p>two</p>");
    else if (scopeCase === "frame-removed") frame.remove();
    else {
      pocSummary = "frame:" + frame.__handle;
      while (pocSummary !== "pending") await later(1);
    }
    const page = await derive("page", 600000).then(hex, (e) => e.name);
    await later(20);
    pocSummary = (own ? "own-crypto" : "shared-crypto") + ":" + heard[0]
      + ":" + (heard.length - 1) + ":" + [...new Set(heard.slice(1))]
      + ":" + page;
  } else {
    let ticks = 0, first = -1;
    const interval = setInterval(() => ticks++, 1), all = [];
    for (let i = 0; i < 20; i++)
      all.push(derive("salt" + i, 100000).then((bytes) => {
        if (first < 0) first = ticks;
        return hex(bytes);
      }));
    const settled = await Promise.allSettled(all), last = ticks;
    clearInterval(interval);
    const values = settled.map((r) => r.status === "fulfilled"
      ? r.value : r.reason.name);
    pocSummary = (last > first ? "ticked" : "unticked") + ":"
      + new Set(values).size + ":" + [0, 15, 16, 19]
        .map((i) => values[i]).join();
  }
})().catch((e) => { pocSummary = "ERR-" + e.name + ":" + e.message; });
