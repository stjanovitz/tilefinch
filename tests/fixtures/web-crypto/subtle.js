/* SubtleCrypto secret-key operations: RFC 4231 HMAC, RFC 6070 PBKDF2,
   RFC 5869 HKDF and the GCM-spec and NIST SP 800-38A AES vectors, then
   the Web Cryptography API's argument, usage and error rules. Sets
   pocSummary to SUBTLE-OK or lists the failed checks. */
(() => {
  pocSummary = "pending";
  const subtle = crypto.subtle, failures = [];
  const hex = (value) => Array.from(new Uint8Array(value),
    (byte) => byte.toString(16).padStart(2, "0")).join("");
  const bytes = (text) => new Uint8Array(text.match(/../g)
    .map((pair) => parseInt(pair, 16)));
  const ascii = (text) => new TextEncoder().encode(text);
  const expect = (name, actual, wanted) => {
    if (actual !== wanted) failures.push(name + "=" + String(actual));
  };
  /* Resolves to the error name, or "resolved". */
  const rejection = (promise) => promise.then(() => "resolved",
    (error) => error && error.name);
  const checks = [];
  const check = (name, promise, wanted) => checks.push(
    Promise.resolve(promise).then((value) => expect(name, value, wanted),
      (error) => failures.push(name + "!" + (error && error.name) + ":"
        + (error && error.message))));
  const raw = (keyBytes, algorithm, usages, extractable = false) =>
    subtle.importKey("raw", keyBytes, algorithm, extractable, usages);

  /* Shape. */
  expect("CryptoKey", typeof CryptoKey, "function");
  let constructed = "none";
  try { new CryptoKey(); } catch (error) { constructed = error.name; }
  expect("new CryptoKey", constructed, "TypeError");
  expect("importKey.length", subtle.importKey.length, 5);
  expect("unwrapKey.length", subtle.unwrapKey.length, 7);
  expect("deriveBits.length", subtle.deriveBits.length, 2);

  /* HMAC, RFC 4231 test cases 1, 2 and 6 and RFC 2202 case 1. */
  const hmac = (key, hash, data) => raw(key, { name: "HMAC", hash },
    ["sign"]).then((k) => subtle.sign("HMAC", k, data)).then(hex);
  const key0b = new Uint8Array(20).fill(0x0b), hiThere = ascii("Hi There");
  check("hmac-sha256-1", hmac(key0b, "SHA-256", hiThere),
    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  check("hmac-sha384-1", hmac(key0b, "SHA-384", hiThere),
    "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
    + "faea9ea9076ede7f4af152e8b2fa9cb6");
  check("hmac-sha512-1", hmac(key0b, { name: "sha-512" }, hiThere),
    "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
    + "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
  check("hmac-sha1-1", hmac(key0b, "SHA-1", hiThere),
    "b617318655057264e28bc0b6fb378c8ef146be00");
  check("hmac-sha256-2", hmac(ascii("Jefe"), "SHA-256",
    ascii("what do ya want for nothing?")),
    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  const long = new Uint8Array(131).fill(0xaa),
    longData = ascii("Test Using Larger Than Block-Size Key - Hash Key First");
  check("hmac-sha256-6", hmac(long, "SHA-256", longData),
    "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  check("hmac-sha512-6", hmac(long, "SHA-512", longData),
    "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
    + "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598");
  check("hmac-verify", raw(key0b, { name: "HMAC", hash: "SHA-256" },
    ["verify"]).then((k) => Promise.all([
      subtle.verify("HMAC", k, bytes(
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
        hiThere),
      subtle.verify("HMAC", k, new Uint8Array(32), hiThere),
      subtle.verify("HMAC", k, new Uint8Array(31), hiThere)])
    .then(String)), "true,false,false");

  /* PBKDF2, RFC 6070, and the SHA-256 form. */
  const pbkdf2 = (password, salt, iterations, bits, hash = "SHA-1") =>
    raw(password, "PBKDF2", ["deriveBits"]).then((k) => subtle.deriveBits(
      { name: "PBKDF2", hash, salt, iterations }, k, bits)).then(hex);
  const password = ascii("password"), salt = ascii("salt");
  check("pbkdf2-1", pbkdf2(password, salt, 1, 160),
    "0c60c80f961f0e71f3a9b524af6012062fe037a6");
  check("pbkdf2-2", pbkdf2(password, salt, 2, 160),
    "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
  check("pbkdf2-4096", pbkdf2(password, salt, 4096, 160),
    "4b007901b765489abead49d926f721d065a429c1");
  check("pbkdf2-long", pbkdf2(ascii("passwordPASSWORDpassword"),
    ascii("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 200),
    "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
  check("pbkdf2-nul", pbkdf2(ascii("pass\0word"), ascii("sa\0lt"), 4096, 128),
    "56fa6aa75548099dcc37d7f03425e0c3");
  check("pbkdf2-sha256", pbkdf2(password, salt, 1, 256, "SHA-256"),
    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

  /* HKDF, RFC 5869 test cases 1 to 4. */
  const hkdf = (ikm, hash, saltBytes, info, bits) =>
    raw(ikm, "HKDF", ["deriveBits"]).then((k) => subtle.deriveBits(
      { name: "HKDF", hash, salt: saltBytes, info }, k, bits)).then(hex);
  const ikm = new Uint8Array(22).fill(0x0b),
    hkdfSalt = bytes("000102030405060708090a0b0c"),
    info = bytes("f0f1f2f3f4f5f6f7f8f9");
  check("hkdf-1", hkdf(ikm, "SHA-256", hkdfSalt, info, 336),
    "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
    + "34007208d5b887185865");
  check("hkdf-3", hkdf(ikm, "SHA-256", new Uint8Array(0), new Uint8Array(0),
    336), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
    + "9d201395faa4b61a96c8");
  check("hkdf-4", hkdf(new Uint8Array(11).fill(0x0b), "SHA-1", hkdfSalt,
    info, 336), "085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155"
    + "fda2c22e422478d305f3f896");
  check("hkdf-zero", hkdf(ikm, "SHA-256", hkdfSalt, info, 0), "");
  /* Test case 2: 80-byte inputs, 82 bytes out. */
  const run80 = (from) => Uint8Array.from({ length: 80 }, (_, i) => from + i);
  check("hkdf-2", hkdf(run80(0), "SHA-256", run80(0x60), run80(0xb0), 656),
    "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
    + "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
    + "cc30c58179ec3e87c14c01d5c1f3434f1d87");
  /* The work limit: full SHA-256 and SHA-512 outputs fit, and one block
     of a 1 MiB info, but not 255 blocks of it. */
  check("hkdf-work", Promise.all([
    hkdf(ikm, "SHA-256", hkdfSalt, info, 255 * 256).then((h) => h.length),
    hkdf(ikm, "SHA-512", hkdfSalt, info, 255 * 512).then((h) => h.length),
    hkdf(ikm, "SHA-256", hkdfSalt, new Uint8Array(1 << 20), 256)
      .then((h) => h.length),
    rejection(hkdf(ikm, "SHA-512", hkdfSalt, new Uint8Array(1 << 20),
      255 * 512))])
    .then(String), "16320,32640,64,OperationError");

  /* AES-GCM: the GCM specification's test cases 2 and 4. */
  const zero16 = new Uint8Array(16);
  check("gcm-2", raw(zero16, "AES-GCM", ["encrypt"]).then((k) =>
    subtle.encrypt({ name: "AES-GCM", iv: new Uint8Array(12) }, k, zero16))
    .then(hex), "0388dace60b6a392f328c2b971b2fe78"
    + "ab6e47d42cec13bdf53a67b21257bddf");
  const gcmKey = bytes("feffe9928665731c6d6a8f9467308308"),
    gcmIv = bytes("cafebabefacedbaddecaf888"),
    gcmAad = bytes("feedfacedeadbeeffeedfacedeadbeefabaddad2"),
    gcmPlain = "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
      + "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
    gcmSealed = "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
      + "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"
      + "5bc94fbc3221a5db94fae95ae7121a47";
  const gcm = { name: "AES-GCM", iv: gcmIv, additionalData: gcmAad };
  const gcmKeyPromise = raw(gcmKey, "aes-gcm", ["encrypt", "decrypt"]);
  check("gcm-4", gcmKeyPromise.then((k) => subtle.encrypt(gcm, k,
    bytes(gcmPlain))).then(hex), gcmSealed);
  check("gcm-4-open", gcmKeyPromise.then((k) => subtle.decrypt(gcm, k,
    bytes(gcmSealed))).then(hex), gcmPlain);
  check("gcm-4-tag96", gcmKeyPromise.then((k) => subtle.encrypt(
    { ...gcm, tagLength: 96 }, k, bytes(gcmPlain))).then(hex),
    gcmSealed.slice(0, -8));
  check("gcm-tampered", gcmKeyPromise.then((k) => {
    const sealed = bytes(gcmSealed);
    sealed[3] ^= 1;
    return rejection(subtle.decrypt(gcm, k, sealed));
  }), "OperationError");
  check("gcm-bad-tag-length", gcmKeyPromise.then((k) => rejection(
    subtle.encrypt({ ...gcm, tagLength: 100 }, k, zero16))),
    "OperationError");
  check("gcm-short", gcmKeyPromise.then((k) => rejection(
    subtle.decrypt(gcm, k, new Uint8Array(15)))), "OperationError");
  check("gcm-no-iv", gcmKeyPromise.then((k) => rejection(
    subtle.encrypt({ name: "AES-GCM" }, k, zero16))), "TypeError");

  /* AES-CBC and AES-CTR: NIST SP 800-38A F.2.1 and F.5.1. */
  const nistKey = bytes("2b7e151628aed2a6abf7158809cf4f3c"),
    nistPlain = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
      + "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
    cbcIv = bytes("000102030405060708090a0b0c0d0e0f"),
    cbcSealed = "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
      + "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"
      + "8cb82807230e1321d3fae00d18cc2012";
  const cbcKey = raw(nistKey, "AES-CBC", ["encrypt", "decrypt"]),
    cbc = { name: "AES-CBC", iv: cbcIv };
  check("cbc", cbcKey.then((k) => subtle.encrypt(cbc, k, bytes(nistPlain)))
    .then(hex), cbcSealed);
  check("cbc-open", cbcKey.then((k) => subtle.decrypt(cbc, k,
    bytes(cbcSealed))).then(hex), nistPlain);
  check("cbc-odd", cbcKey.then((k) => subtle.encrypt(cbc, k, ascii("abc"))
    .then((sealed) => subtle.decrypt(cbc, k, sealed))).then(hex), "616263");
  check("cbc-bad-padding", cbcKey.then((k) => rejection(subtle.decrypt(cbc, k,
    bytes(cbcSealed.slice(0, 128))))), "OperationError");
  check("cbc-iv", cbcKey.then((k) => rejection(subtle.encrypt(
    { name: "AES-CBC", iv: new Uint8Array(8) }, k, zero16))),
    "OperationError");
  const ctrKey = raw(nistKey, "AES-CTR", ["encrypt", "decrypt"]),
    counter = bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
  check("ctr", ctrKey.then((k) => subtle.encrypt({ name: "AES-CTR", counter,
    length: 64 }, k, bytes(nistPlain))).then(hex),
    "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
    + "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
  /* An 8-bit counter wraps from ff to 00 without carrying into the next
     byte, so its second block is the first block of counter ...fe00. */
  check("ctr-wrap", ctrKey.then((k) => Promise.all([
    subtle.encrypt({ name: "AES-CTR", counter, length: 8 }, k,
      new Uint8Array(32)),
    subtle.encrypt({ name: "AES-CTR", counter: bytes(
      "f0f1f2f3f4f5f6f7f8f9fafbfcfdfe00"), length: 128 }, k,
      new Uint8Array(16))]))
    .then(([wrapped, direct]) => hex(wrapped).slice(32) === hex(direct)),
    true);
  check("ctr-reuse", ctrKey.then((k) => rejection(subtle.encrypt(
    { name: "AES-CTR", counter, length: 1 }, k, new Uint8Array(48)))),
    "OperationError");
  check("ctr-length", ctrKey.then((k) => rejection(subtle.encrypt(
    { name: "AES-CTR", counter, length: 0 }, k, zero16))), "OperationError");

  /* Keys: attributes, export, generation and derivation. */
  check("attributes", raw(nistKey, { name: "AES-GCM" },
    ["decrypt", "encrypt", "decrypt"], true).then((k) => [
    k instanceof CryptoKey, Object.prototype.toString.call(k), k.type,
    k.extractable, k.algorithm.name, k.algorithm.length, k.usages.join(),
    k.algorithm === k.algorithm, k.usages === k.usages,
    Object.keys(k).length, JSON.stringify(k)].join()),
    "true,[object CryptoKey],secret,true,AES-GCM,128,encrypt,decrypt,true,"
    + "true,0,{}");
  check("export-raw", raw(nistKey, "AES-CBC", ["encrypt"], true)
    .then((k) => subtle.exportKey("raw", k)).then(hex),
    "2b7e151628aed2a6abf7158809cf4f3c");
  check("export-jwk", raw(key0b, { name: "HMAC", hash: "SHA-256" },
    ["verify", "sign"], true).then((k) => subtle.exportKey("jwk", k))
    .then(JSON.stringify), '{"alg":"HS256","ext":true,'
    + '"k":"CwsLCwsLCwsLCwsLCwsLCwsLCws","key_ops":["sign","verify"],'
    + '"kty":"oct"}');
  check("non-extractable", raw(nistKey, "AES-GCM", ["encrypt"])
    .then((k) => Promise.all([rejection(subtle.exportKey("raw", k)),
      rejection(subtle.exportKey("jwk", k))]).then(String)),
    "InvalidAccessError,InvalidAccessError");
  check("jwk-import", subtle.importKey("jwk", { kty: "oct", alg: "A128CBC",
    k: "K34VFiiu0qar9xWICc9PPA", ext: true }, "AES-CBC", true, ["encrypt"])
    .then((k) => subtle.exportKey("raw", k)).then(hex),
    "2b7e151628aed2a6abf7158809cf4f3c");
  check("jwk-alg", rejection(subtle.importKey("jwk", { kty: "oct",
    alg: "A256GCM", k: "K34VFiiu0qar9xWICc9PPA" }, "AES-GCM", false,
    ["encrypt"])), "DataError");
  check("jwk-ext", rejection(subtle.importKey("jwk", { kty: "oct",
    k: "K34VFiiu0qar9xWICc9PPA", ext: false }, "AES-GCM", true,
    ["encrypt"])), "DataError");
  check("jwk-key-ops", rejection(subtle.importKey("jwk", { kty: "oct",
    k: "K34VFiiu0qar9xWICc9PPA", key_ops: ["encrypt"] }, "AES-GCM", false,
    ["decrypt"])), "DataError");
  check("jwk-kty", rejection(subtle.importKey("jwk", { kty: "RSA",
    k: "K34VFiiu0qar9xWICc9PPA" }, "AES-GCM", false, ["encrypt"])),
    "DataError");
  check("jwk-buffer", rejection(subtle.importKey("jwk", nistKey, "AES-GCM",
    false, ["encrypt"])), "TypeError");
  check("generate-aes", subtle.generateKey({ name: "AES-GCM", length: 256 },
    true, ["encrypt", "decrypt"]).then((k) => subtle.exportKey("raw", k)
    .then((exported) => [k.algorithm.length, exported.byteLength,
      k.usages.join()].join())), "256,32,encrypt,decrypt");
  check("generate-hmac", subtle.generateKey({ name: "HMAC", hash: "SHA-512" },
    false, ["sign"]).then((k) => [k.algorithm.length, k.algorithm.hash.name,
    k.extractable].join()), "1024,SHA-512,false");
  const hmacLength = (length) => ({ name: "HMAC", hash: "SHA-256", length });
  const maxKeyBits = 8 * 1024 * 1024;
  check("generate-hmac-too-large", rejection(subtle.generateKey(
    hmacLength(maxKeyBits + 1), false, ["sign"])), "DataError");
  check("generate-hmac-uint32", rejection(subtle.generateKey(
    hmacLength(0xffffffff), false, ["sign"])), "DataError");
  try {
    crypto.getRandomValues(new Uint8Array(65537));
    expect("public-random-quota", "resolved", "QuotaExceededError");
  } catch (error) {
    expect("public-random-quota", error.name, "QuotaExceededError");
  }
  const poison = () => { throw new Error("Unexpected public method"); };
  let generatedHmac;
  try {
    crypto.getRandomValues = Uint8Array.prototype.fill =
      Uint8Array.prototype.subarray = poison;
    generatedHmac = subtle.generateKey(hmacLength(maxKeyBits - 1), true,
      ["sign"]);
  } finally {
    delete crypto.getRandomValues;
    delete Uint8Array.prototype.fill;
    delete Uint8Array.prototype.subarray;
  }
  check("generate-hmac-chunked", generatedHmac.then((k) =>
    subtle.exportKey("raw", k).then((exported) => {
      const keyBytes = new Uint8Array(exported);
      let filled = true;
      for (let offset = 0; offset < keyBytes.length; offset += 65536)
        filled &&= keyBytes.subarray(offset, offset + 16).some((b) => b !== 0);
      return k.algorithm.length === maxKeyBits - 1 &&
        keyBytes.length === maxKeyBits / 8 && filled &&
        (keyBytes[keyBytes.length - 1] & 1) === 0;
    })), true);
  check("derive-key", raw(password, "PBKDF2", ["deriveKey"]).then((k) =>
    subtle.deriveKey({ name: "PBKDF2", hash: "SHA-256", salt,
      iterations: 1 }, k, { name: "AES-CBC", length: 256 }, true,
    ["encrypt"])).then((k) => subtle.exportKey("raw", k)).then(hex),
    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
  check("wrap", Promise.all([raw(nistKey, "AES-GCM", ["encrypt"], true),
    raw(zero16, "AES-GCM", ["wrapKey", "unwrapKey"])]).then(([k, w]) => {
    const params = { name: "AES-GCM", iv: new Uint8Array(12) };
    return subtle.wrapKey("jwk", k, w, params).then((wrapped) =>
      subtle.unwrapKey("jwk", wrapped, w, params, "AES-GCM", true,
        ["encrypt"])).then((u) => subtle.exportKey("raw", u)).then(hex);
  }), "2b7e151628aed2a6abf7158809cf4f3c");

  /* Errors. */
  check("unknown-algorithm", rejection(raw(nistKey, "AES-KW", ["wrapKey"])),
    "NotSupportedError");
  check("sign-with-aes", raw(nistKey, "AES-GCM", ["encrypt"]).then((k) =>
    rejection(subtle.sign("AES-GCM", k, zero16))), "NotSupportedError");
  check("wrong-algorithm", raw(nistKey, "AES-CBC", ["encrypt"]).then((k) =>
    rejection(subtle.encrypt({ name: "AES-GCM", iv: zero16 }, k, zero16))),
    "InvalidAccessError");
  check("wrong-usage", raw(nistKey, "AES-CBC", ["encrypt"]).then((k) =>
    rejection(subtle.decrypt(cbc, k, bytes(cbcSealed)))),
    "InvalidAccessError");
  check("bad-usage", rejection(raw(nistKey, "AES-GCM", ["sign"])),
    "SyntaxError");
  check("empty-usages", rejection(raw(nistKey, "AES-GCM", [])),
    "SyntaxError");
  check("unknown-usage", rejection(raw(nistKey, "AES-GCM", ["frobnicate"])),
    "TypeError");
  check("aes-length", rejection(raw(new Uint8Array(15), "AES-GCM",
    ["encrypt"])), "DataError");
  check("hmac-empty", rejection(raw(new Uint8Array(0),
    { name: "HMAC", hash: "SHA-256" }, ["sign"])), "DataError");
  check("hmac-no-hash", rejection(raw(key0b, "HMAC", ["sign"])), "TypeError");
  check("hmac-bad-hash", rejection(raw(key0b, { name: "HMAC", hash: "MD5" },
    ["sign"])), "NotSupportedError");
  check("pbkdf2-extractable", rejection(raw(password, "PBKDF2",
    ["deriveBits"], true)), "SyntaxError");
  check("pbkdf2-export", raw(password, "PBKDF2", ["deriveBits"]).then((k) =>
    rejection(subtle.exportKey("raw", k))), "NotSupportedError");
  check("pbkdf2-jwk", rejection(subtle.importKey("jwk", {}, "PBKDF2", false,
    ["deriveBits"])), "NotSupportedError");
  check("pbkdf2-zero", raw(password, "PBKDF2", ["deriveBits"]).then((k) =>
    Promise.all([
      rejection(subtle.deriveBits({ name: "PBKDF2", hash: "SHA-1", salt,
        iterations: 0 }, k, 160)),
      rejection(subtle.deriveBits({ name: "PBKDF2", hash: "SHA-1", salt,
        iterations: 1 }, k, 12)),
      rejection(subtle.deriveBits({ name: "PBKDF2", hash: "SHA-1", salt,
        iterations: 1 }, k))]).then(String)),
    "OperationError,OperationError,OperationError");
  check("pbkdf2-admission", raw(password, "PBKDF2", ["deriveBits"]).then((k) =>
    Promise.all([
      rejection(subtle.deriveBits({ name: "PBKDF2", hash: "SHA-256", salt,
        iterations: 1 }, k, 32 * 1024 * 1024 * 8)),
      rejection(subtle.deriveBits({ name: "PBKDF2", hash: "SHA-1", salt,
        iterations: 5000001 }, k, 168)),
    ]).then(String)), "OperationError,OperationError");
  check("generate-length", rejection(subtle.generateKey({ name: "AES-CBC",
    length: 100 }, true, ["encrypt"])), "OperationError");
  check("generate-pbkdf2", rejection(subtle.generateKey("PBKDF2", false,
    ["deriveBits"])), "NotSupportedError");
  check("not-a-key", rejection(subtle.encrypt(cbc, {}, zero16)), "TypeError");
  check("bad-format", rejection(subtle.importKey("pem", nistKey, "AES-GCM",
    false, ["encrypt"])), "TypeError");
  check("spki-secret", rejection(subtle.importKey("spki", nistKey, "AES-GCM",
    false, ["encrypt"])), "NotSupportedError");
  check("illegal-receiver", rejection(SubtleCrypto.prototype.encrypt.call({},
    cbc, null, zero16)), "TypeError");
  let getter = "none";
  try {
    Object.getOwnPropertyDescriptor(CryptoKey.prototype, "type").get.call({});
  } catch (error) {
    getter = error.name;
  }
  expect("getter-receiver", getter, "TypeError");

  Promise.all(checks).then(() => {
    pocSummary = failures.length ? "SUBTLE-BAD " + failures.join(" ")
      : "SUBTLE-OK";
  });
})();
