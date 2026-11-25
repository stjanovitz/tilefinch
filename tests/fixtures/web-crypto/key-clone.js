pocSummary = "pending";
(async () => {
  const check = (condition, label) => {
    if (!condition) throw new Error(label);
  };
  const rejects = async (promise, name, label) => {
    try { await promise; } catch (error) {
      check(error.name === name, label + ":" + error.name);
      return;
    }
    throw new Error(label + ":resolved");
  };
  const subtle = crypto.subtle;
  const algorithm = { name: "HMAC", hash: "SHA-256" };
  const data = new Uint8Array([1, 2, 3]);
  const keyBytes = new Uint8Array(8192).fill(7);
  const source = await subtle.importKey("raw", keyBytes, algorithm, false, ["sign"]);
  const sign = async (key) => String(new Uint8Array(
    await subtle.sign("HMAC", key, data)));
  const expected = await sign(source);
  const checkKey = async (key, label) => {
    check(key instanceof CryptoKey && key !== source, label + ":brand");
    check(key.type === "secret" && !key.extractable
      && key.algorithm.name === "HMAC" && key.algorithm.length === 65536
      && key.algorithm.hash.name === "SHA-256" && String(key.usages) === "sign",
      label + ":attributes");
    check(await sign(key) === expected, label + ":material");
    await rejects(subtle.exportKey("raw", key), "InvalidAccessError", label + ":export");
    await rejects(subtle.verify("HMAC", key, data, data), "InvalidAccessError",
      label + ":verify");
  };
  /* Serialize internal state, not mutable attribute objects or expandos. */
  source.algorithm.hash.name = "SHA-1";
  source.usages[0] = "verify";
  Object.defineProperty(source, "extra", { enumerable: true,
    get() { throw new Error("clone touched a key's expando"); } });
  const copied = structuredClone([source, source]);
  check(copied[0] === copied[1], "clone:alias");
  await checkKey(copied[0], "clone");

  const buffer = new ArrayBuffer(8);
  let transferError;
  try { structuredClone({}, { transfer: [buffer, source] }); }
  catch (error) { transferError = error.name; }
  check(transferError === "DataCloneError" && buffer.byteLength === 8,
    "CryptoKey is not transferable");

  const request = (r) => new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const opening = indexedDB.open("crypto", 1);
  opening.onupgradeneeded = () => opening.result.createObjectStore("keys");
  const db = await request(opening);
  check(globalThis.__tilefinchWorkerCloneIntrinsics.cryptoKeySize === undefined
    && !("__tilefinchIndexedDBKeySizeReader" in globalThis)
    && !("__tilefinchSetIndexedDBKeySizeReader" in globalThis),
    "idb:the key size reader must stay private");
  const write = db.transaction("keys", "readwrite");
  const complete = new Promise((resolve) => { write.oncomplete = resolve; });
  const stored = [source, source];
  const put = request(write.objectStore("keys").put(stored, 1));
  stored[0] = null;
  await put;
  await complete;
  const stats = globalThis.__tilefinchIndexedDBStats;
  check(stats.bytes >= keyBytes.length && stats.bytes < keyBytes.length + 512,
    "idb:charge key bytes once per alias group");
  const read = () => request(db.transaction("keys").objectStore("keys").get(1));
  const first = await read();
  check(first[0] === first[1], "idb:alias");
  await checkKey(first[0], "idb");
  first[0].algorithm.hash.name = "SHA-1";
  first[0].usages[0] = "verify";
  const second = await read();
  check(second[0] !== first[0], "idb:fresh copy");
  await checkKey(second[0], "idb:second read");
  db.close();

  const url = URL.createObjectURL(new Blob([`
    onmessage = async ({data: [first, second]}) => {
      try {
        if (first !== second || !(first instanceof CryptoKey))
          throw new Error("worker brand or alias");
        const sig = await crypto.subtle.sign("HMAC", first, new Uint8Array([1, 2, 3]));
        postMessage({key: first, signature: String(new Uint8Array(sig))});
      } catch (error) { postMessage({error: error.name + ":" + error.message}); }
    };
  `]));
  const worker = new Worker(url);
  const returned = await new Promise((resolve, reject) => {
    worker.onmessage = ({data}) => resolve(data);
    worker.onerror = () => reject(new Error("worker error"));
    worker.postMessage([source, source]);
  });
  worker.terminate();
  URL.revokeObjectURL(url);
  check(!returned.error && returned.signature === expected, "worker:" + returned.error);
  await checkKey(returned.key, "worker");
  pocSummary = "KEY-CLONE-OK";
})().catch((error) => { pocSummary = error.name + ":" + error.message; });
