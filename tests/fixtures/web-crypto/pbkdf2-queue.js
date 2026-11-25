/* The pending-derivation ceiling refuses the request that would pass it
   at once, without displacing the five admitted jobs, whose 512 KiB salts
   it charges. They are still pending when the host tears down this
   realm. */
(() => {
  pocSummary = "pending";
  const subtle = crypto.subtle, enc = new TextEncoder(),
    salt = new Uint8Array(512 * 1024);
  subtle.importKey("raw", enc.encode("password"), "PBKDF2", false,
    ["deriveBits"]).then((key) => {
    const derive = () => subtle.deriveBits({
      name: "PBKDF2",
      hash: "SHA-256",
      salt,
      iterations: 10000000,
    }, key, 128);
    for (let i = 0; i < 5; i++)
      derive().then(() => { pocSummary = "finished early"; },
        (error) => { pocSummary = "early refusal " + error.name; });
    return derive().then(() => "not refused", (error) => error.name);
  }).then((result) => {
    if (pocSummary === "pending") pocSummary = result;
  });
})();
