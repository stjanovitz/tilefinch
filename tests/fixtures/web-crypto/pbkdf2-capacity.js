/* The largest PBKDF2 request (a 1 MiB key and salt, 1 MiB of output) is
   charged the whole pending-derivation ceiling, so it is admitted only
   when no other derivation holds a charge. It stays pending until the
   realm goes. */
(async () => {
  pocSummary = "pending";
  const big = new Uint8Array(1024 * 1024),
    key = await crypto.subtle.importKey("raw", big, "PBKDF2", false,
      ["deriveBits"]);
  let state = "admitted";
  crypto.subtle.deriveBits({ name: "PBKDF2", hash: "SHA-512", salt: big,
    iterations: 1 }, key, 8 * 1024 * 1024)
    .then(() => { state = "finished"; }, (e) => { state = e.name; });
  await new Promise((resolve) => setTimeout(resolve, 0));
  pocSummary = state;
})().catch((e) => { pocSummary = "ERR-" + e.name + ":" + e.message; });
