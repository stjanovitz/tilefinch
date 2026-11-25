/* Operations over a slice's work, run as jobs one chunk per slice
   (test_web_crypto.c sets the slice that short). scopeCase picks the case:
   - "order": the page starts long and short operations together; they
     settle in the order asked for, a timer runs while the first is
     pending, and each gives the bytes main's one-call code gave.
   - "worker": a worker's operations and the page's run in turn, each
     scope in its own order.
   - "terminated": a worker starts long jobs and is terminated; the page's
     own job then runs.
   - "frame": a frame starts long jobs and navigates; they stop with its
     document.
   - "failure": short operations queued behind a long one, a failing one
     among them, settle after it in order, all in the task it ends in.
   - "frame-removed": a frame starts long jobs and is removed; they
     reject, and what the page then asks of the frame's SubtleCrypto
     (encrypt, a sliced digest, PBKDF2) rejects at once. Put back, the
     frame's crypto works again. */
(async () => {
  pocSummary = "pending";
  /* The pattern, keys and known answers of test_sliced_jobs: SHA-256 of
     each output. */
  const prelude = "const subtle=crypto.subtle,n=100003,"
    + "input=new Uint8Array(n+64).map((_,i)=>(i*7+3)&255),"
    + "key32=new Uint8Array(32).map((_,i)=>0xa0+i),"
    + "iv=new Uint8Array(16).map((_,i)=>0x10*i+1),"
    + "hex=b=>[...new Uint8Array(b)].map(v=>v.toString(16).padStart(2,'0'))"
    + ".join(''),sha=b=>subtle.digest('SHA-256',b).then(hex),"
    + "aes=(name,bytes)=>subtle.importKey('raw',bytes,name,false,"
    + "['encrypt']),"
    + "gcm=()=>aes('AES-GCM',key32).then(k=>subtle.encrypt({name:'AES-GCM',"
    + "iv:iv.subarray(0,12),additionalData:input.subarray(7,5010)},k,"
    + "input.subarray(0,n))),"
    + "long=(count)=>aes('AES-GCM',key32).then(k=>{for(let i=0;i<count;"
    + "i++)subtle.encrypt({name:'AES-GCM',iv:iv.subarray(0,12)},k,"
    + "new Uint8Array(1<<20)).then(()=>report('settled'),"
    + "e=>report(e.name));report('started')});";
  const known = {
    gcm: "0308a932cfcdf9c979111cd69f170eb465757a9828fdb28606c671b10e9b38e3",
    cbc: "18be47160114b40b2d7291e643c7797ba106624a6d075aa9ab56b498a67da3c9",
    hmac1: "532be09159a2a84cfdeeee3c5be775b749b3127200af2feda1eaa015c06b5581",
    hmac384: "267bfcf504a66ee438d49ffa809a324e645611c0f4a86cd9a396959d20984d00",
    hkdf512: "98b1a5e1da87089fe3106387613c838495b399c3bec434f31a91b7f427dd975d",
    million512: "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa"
      + "973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
    million256: "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
  };
  (0, eval)(prelude.replace("const ", "var "));
  const later = (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
    million = new Uint8Array(1000000).fill(97),
    order = [],
    settle = (name, promise) => promise.then((value) => {
      order.push(name);
      return value;
    });
  if (scopeCase === "order") {
    const [gcmKey, macKey, hkdfKey, cbcKey, longKey] = await Promise.all([
      aes("AES-GCM", key32),
      subtle.importKey("raw", new TextEncoder().encode("key"),
        { name: "HMAC", hash: "SHA-256" }, false, ["sign"]),
      subtle.importKey("raw", input.subarray(0, 100), "HKDF", false,
        ["deriveBits"]),
      aes("AES-CBC", key32.subarray(0, 24)),
      subtle.importKey("raw", input.subarray(1, 201),
        { name: "HMAC", hash: "SHA-384" }, false, ["sign"])]);
    const all = [
      settle("A", subtle.encrypt({ name: "AES-GCM", iv: iv.subarray(0, 12),
        additionalData: input.subarray(7, 5010) }, gcmKey,
        input.subarray(0, n))),
      settle("B", subtle.sign("HMAC", macKey, new Uint8Array(3))),
      settle("C", subtle.digest("SHA-512", million)),
      settle("D", subtle.encrypt({ name: "AES-CBC", iv }, cbcKey,
        input.subarray(0, n))),
      settle("E", subtle.deriveBits({ name: "HKDF", hash: "SHA-512",
        salt: input.subarray(100, 300), info: input.subarray(300, 1300) },
        hkdfKey, 255 * 512)),
      settle("F", subtle.digest("SHA-1", new Uint8Array(3))),
      settle("G", subtle.sign("HMAC", longKey, input.subarray(0, n))),
    ];
    setTimeout(() => order.push("task"), 0);
    const [a, , c, d, e, , g] = await Promise.all(all);
    const wrong = [];
    if (await sha(a) !== known.gcm) wrong.push("A");
    if (hex(c) !== known.million512) wrong.push("C");
    if (await sha(d) !== known.cbc) wrong.push("D");
    if (await sha(e) !== known.hkdf512) wrong.push("E");
    if (await sha(g) !== known.hmac384) wrong.push("G");
    pocSummary = order.join() + ":" + (wrong.join() || "ok");
  } else if (scopeCase === "failure") {
    const key = await subtle.importKey("raw", key32, "AES-GCM", false,
        ["encrypt", "decrypt"]), short = iv.subarray(0, 12),
      bad = new Uint8Array(await subtle.encrypt({ name: "AES-GCM",
        iv: short }, key, new Uint8Array(32)));
    bad[bad.length - 1] ^= 1;
    let late = false;
    const note = (name, promise) => promise.then(
      () => order.push(name + (late ? "+late" : "")),
      (e) => order.push(name + "-" + e.name + (late ? "+late" : "")));
    const first = subtle.encrypt({ name: "AES-GCM", iv: short }, key,
      input.subarray(0, n));
    const all = [
      note("A", first),
      note("B", subtle.decrypt({ name: "AES-GCM", iv: short }, key, bad)),
      note("C", subtle.digest("SHA-1", new Uint8Array(3))),
      note("D", subtle.decrypt({ name: "AES-GCM", iv: short }, key, bad)),
      note("E", subtle.digest("SHA-256", new Uint8Array(3))),
    ];
    first.then(() => setTimeout(() => late = true, 0));
    await Promise.all(all);
    pocSummary = order.join();
  } else if (scopeCase === "worker") {
    const url = URL.createObjectURL(new Blob([prelude
      + "const order=[],settle=(name,p)=>p.then(v=>(order.push(name),v));"
      + "Promise.all([aes('AES-CBC',key32.subarray(0,24)),subtle.importKey("
      + "'raw',input.subarray(1,201),{name:'HMAC',hash:'SHA-1'},false,"
      + "['sign'])]).then(([c,m])=>Promise.all([settle('X',subtle.encrypt("
      + "{name:'AES-CBC',iv},c,input.subarray(0,n))),settle('Y',"
      + "subtle.digest('SHA-1',new Uint8Array(3))),settle('Z',subtle.sign("
      + "'HMAC',m,input.subarray(0,n)))])).then(([x,,z])=>"
      + "Promise.all([sha(x),sha(z)])).then(h=>postMessage(order.join()+':'"
      + "+(h[0]==='" + known.cbc + "'&&h[1]==='" + known.hmac1
      + "'?'ok':'wrong')),e=>postMessage('ERR-'+e.name));"])),
      worker = new Worker(url),
      heard = new Promise((resolve) => { worker.onmessage = (event) =>
        resolve(event.data); }),
      ours = aes("AES-GCM", key32).then((key) => Promise.all([
        settle("P", subtle.encrypt({ name: "AES-GCM", iv: iv.subarray(0, 12),
          additionalData: input.subarray(7, 5010) }, key,
          input.subarray(0, n))),
        settle("Q", subtle.digest("SHA-256", million))]));
    const [[p, q], theirs] = await Promise.all([ours, heard]);
    worker.terminate();
    URL.revokeObjectURL(url);
    pocSummary = "page:" + order.join() + ":"
      + (await sha(p) === known.gcm && hex(q) === known.million256
        ? "ok" : "wrong") + ";worker:" + theirs;
  } else if (scopeCase === "terminated") {
    const url = URL.createObjectURL(new Blob(
      ["const report=m=>postMessage(m);" + prelude + "long(4);"])),
      worker = new Worker(url), heard = [];
    await new Promise((resolve) => {
      worker.onmessage = (event) => {
        heard.push(event.data);
        resolve();
      };
    });
    worker.terminate();
    const own = await gcm().then(sha, (e) => e.name);
    await later(20);
    URL.revokeObjectURL(url);
    pocSummary = heard.join() + ":" + (own === known.gcm ? "ok" : own);
  } else if (scopeCase === "frame-removed") {
    const frame = document.createElement("iframe");
    document.body.appendChild(frame);
    await new Promise((resolve) => {
      frame.onload = resolve;
      frame.srcdoc = "<p>one</p>";
    });
    const view = frame.contentWindow, heard = [],
      held = view.crypto.subtle,
      gcmKey = await aes("AES-GCM", key32),
      pbkdf2Key = await subtle.importKey("raw", key32, "PBKDF2", false,
        ["deriveBits"]);
    globalThis.frameReport = (m) => heard.push(m);
    view.eval("const report=m=>parent.frameReport(m);" + prelude + "long(4);");
    while (!heard.length) await later(1);
    frame.remove();
    while (heard.length < 5) await later(1);
    /* Settled before a timer runs, or still pending. */
    const now = (promise) => Promise.race([
      promise.then(() => "ran", (e) => e.name),
      later(0).then(() => "pending")]);
    const late = await Promise.all([
      now(held.encrypt({ name: "AES-GCM", iv: iv.subarray(0, 12) }, gcmKey,
        new Uint8Array(1 << 20))),
      now(held.digest("SHA-256", million)),
      now(held.deriveBits({ name: "PBKDF2", hash: "SHA-256",
        salt: key32, iterations: 10000000 }, pbkdf2Key, 256))]);
    document.body.appendChild(frame);
    await later(1);
    const again = await frame.contentWindow.crypto.subtle
      .digest("SHA-256", million).then(hex, (e) => e.name);
    const own = await gcm().then(sha, (e) => e.name);
    await later(20);
    pocSummary = heard.join() + ":" + late.join() + ":"
      + (again === known.million256 ? "ok" : again) + ":"
      + (own === known.gcm ? "ok" : own);
  } else {
    const frame = document.createElement("iframe");
    document.body.appendChild(frame);
    const load = (srcdoc) => new Promise((resolve) => {
      frame.onload = resolve;
      frame.srcdoc = srcdoc;
    });
    await load("<p>one</p>");
    const view = frame.contentWindow, heard = [];
    globalThis.frameReport = (m) => heard.push(m);
    view.eval("const report=m=>parent.frameReport(m);" + prelude + "long(4);");
    while (!heard.length) await later(1);
    await load("<p>two</p>");
    const own = await gcm().then(sha, (e) => e.name);
    await later(20);
    pocSummary = heard.join() + ":" + (own === known.gcm ? "ok" : own);
  }
})().catch((e) => { pocSummary = "ERR-" + e.name + ":" + e.message; });
