/* The SubtleCrypto operations other than digest, for the secret-key
   algorithms: AES-GCM, AES-CBC and AES-CTR encryption, HMAC signatures,
   PBKDF2 and HKDF derivation, raw and JWK key import and export, key
   generation, and wrapping with the AES algorithms, and CryptoKey's
   attributes. compat.js builds them the first time a page reads
   crypto.subtle, so a realm that never does holds none of them: the factory
   builds the operations and returns the step that adds them. It passes
   SubtleCrypto and CryptoKey, which this adds the members to, its brand
   and worker-normalizer slots, the natives that hold a key's secret and
   run the primitives (src/web_crypto.c), and the BufferSource copies and
   promise functions it captured before page script ran. Algorithm names,
   parameters, usages and error names follow the Web Cryptography API; an
   operation's checks throw, and its method turns that into a rejected
   promise. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateSubtleCrypto", {
  __proto__: null,
  configurable: true,
  value(
    SubtleCrypto,
    CryptoKey,
    subtleInstances,
    normalizerSlot,
    keyNative,
    runNative,
    randomFill,
    isArrayBuffer,
    isView,
    bufferBytes,
    viewBytes,
    byteLengthOfView,
    bufferOfView,
    fillBytes,
    promiseResolve,
    promiseThen,
    promiseReject,
    keyStates,
    domShared,
  ) {
    const { TypeError, DOMException, Uint8Array, ArrayBuffer, TextEncoder,
      TextDecoder, Promise, defineProperty,
      getOwnPropertyDescriptor } = globalThis.__tilefinchEventInterfaces;
    /* Algorithm kinds, in this order, and the kinds each operation
       supports (bit = kind). */
    const NAMES = ["AES-GCM", "AES-CBC", "AES-CTR", "HMAC", "PBKDF2", "HKDF"],
      GCM = 0, CBC = 1, CTR = 2, HMAC = 3, PBKDF2 = 4,
      ENC = 7, SIGN = 8, GEN = 15, IMPORT = 63, DERIVE = 48, LENGTH = 127,
      USAGES = ["encrypt", "decrypt", "sign", "verify", "deriveKey",
        "deriveBits", "wrapKey", "unwrapKey"],
      AES_USES = 195, SIGN_USES = 12, DERIVE_USES = 48,
      HASHES = ["SHA-1", "SHA-256", "SHA-384", "SHA-512"],
      FORMATS = ["raw", "spki", "pkcs8", "jwk"],
      TAG_LENGTHS = [32, 64, 96, 104, 112, 120, 128],
      EMPTY = new Uint8Array(0);
    /* The per-job bounds and state size mirror include/tilefinch/web_crypto.h.
       The derivations of the page and all its frames and workers, running
       and waiting, share one ceiling smaller than the heap they share: each
       is charged its retained key, salt, output and progress, and the
       promise, closures, copies and records a job keeps (PBKDF2_JOB_BYTES;
       about 2.2 KB measured on the 64-bit host, less on the PSP), so the
       pending jobs' real memory stays near the ceiling and the largest
       single request still fits. Each scope runs at most PBKDF2_JOB_LIMIT
       at once. */
    const PBKDF2_INPUT_LIMIT = 1024 * 1024,
      PBKDF2_WORK_LIMIT = 10000000, PBKDF2_STATE_BYTES = 136,
      PBKDF2_JOB_LIMIT = 16, PBKDF2_JOB_BYTES = 2048,
      PBKDF2_QUEUE_BYTES = 3 * PBKDF2_INPUT_LIMIT + PBKDF2_STATE_BYTES
        + PBKDF2_JOB_BYTES,
      HASH_BYTES = [20, 32, 48, 64];
    /* Match the native key size and random-fill request bounds. */
    const KEY_BYTES_LIMIT = 1024 * 1024, RANDOM_FILL_LIMIT = 65536;
    const fail = (name, message) => {
      throw new DOMException(message, name);
    };
    const string = (value) => {
      if (typeof value === "symbol")
        throw new TypeError("Cannot convert a Symbol to a string");
      return `${value}`;
    };
    /* [EnforceRange] integers. */
    const integer = (value, maximum, what) => {
      if (value === undefined) throw new TypeError(what + " is required");
      const number = +value;
      if (!Number.isFinite(number))
        throw new TypeError(what + " is not a finite number");
      const truncated = Math.trunc(number);
      if (truncated < 0 || truncated > maximum)
        throw new TypeError(what + " is out of range");
      return truncated;
    };
    const required = (value, what) => {
      if (value === undefined) throw new TypeError(what + " is required");
      return bytesOf(value);
    };
    const format = (value) => {
      const name = string(value);
      if (!FORMATS.includes(name))
        throw new TypeError("Unsupported key format: " + name);
      return name;
    };
    const usageMask = (list) => {
      if (list === null || (typeof list !== "object"
        && typeof list !== "function"))
        throw new TypeError("Key usages must be a sequence");
      let mask = 0;
      for (const item of list) {
        const index = USAGES.indexOf(string(item));
        if (index < 0) throw new TypeError("Unsupported key usage");
        mask |= 1 << index;
      }
      return mask;
    };
    const usageList = (mask) => {
      const usages = [];
      for (let index = 0; index < 8; index++) {
        if (!(mask & (1 << index))) continue;
        defineProperty(usages, usages.length, {
          __proto__: null, value: USAGES[index],
          writable: true, enumerable: true, configurable: true,
        });
      }
      return usages;
    };
    const key = (value) => {
      const state = keyStates.get(value);
      if (!state) throw new TypeError("A CryptoKey is required");
      return state;
    };
    const checkKey = (state, kind, usage) => {
      if (state.kind !== kind)
        fail("InvalidAccessError", "The key is not a " + NAMES[kind] + " key");
      if (!(state.uses & usage))
        fail("InvalidAccessError", "The key does not support " +
          USAGES[31 - Math.clz32(usage)]);
    };
    /* Normalizes an algorithm for an operation and converts its
       parameters, reading members in their dictionary order. */
    const normalize = (algorithm, operation) => {
      const dictionary = algorithm !== null && (typeof algorithm === "object"
        || typeof algorithm === "function") ? algorithm
        : { name: string(algorithm) };
      const name = dictionary.name;
      if (name === undefined) throw new TypeError("Algorithm name is required");
      const kind = NAMES.indexOf(string(name).toUpperCase()),
        params = { kind };
      if (kind < 0 || !(operation >> kind & 1))
        fail("NotSupportedError", "Unsupported algorithm: " + string(name));
      const d = dictionary;
      if (operation === ENC) {
        if (kind === GCM) {
          if (d.additionalData !== undefined)
            params.aad = bytesOf(d.additionalData);
          params.iv = required(d.iv, "iv");
          if (d.tagLength !== undefined)
            params.tag = integer(d.tagLength, 255, "tagLength");
        } else if (kind === CBC) params.iv = required(d.iv, "iv");
        else {
          params.counter = required(d.counter, "counter");
          params.length = integer(d.length, 255, "length");
        }
      } else if (operation === DERIVE) {
        params.hash = hash(d);
        if (kind === PBKDF2) {
          params.iterations = integer(d.iterations, 4294967295, "iterations");
          const salt = d.salt;
          if (salt === undefined) throw new TypeError("salt is required");
          params.salt = bytesOf(salt, PBKDF2_INPUT_LIMIT);
          if (params.salt === null)
            fail("OperationError", "PBKDF2 salt is too large");
        } else {
          params.info = required(d.info, "info");
          params.salt = required(d.salt, "salt");
        }
      } else if (operation !== SIGN && kind === HMAC) {
        params.hash = hash(d);
        if (d.length !== undefined)
          params.length = integer(d.length, 4294967295, "length");
      } else if ((operation === GEN || operation === LENGTH) && kind < HMAC)
        params.length = integer(d.length, 65535, "length");
      return params;
    };
    const hash = (dictionary) => {
      if (dictionary.hash === undefined)
        throw new TypeError("hash is required");
      const value = dictionary.hash,
        name = value !== null && typeof value === "object" ? value.name : value;
      if (name === undefined) throw new TypeError("Algorithm name is required");
      const index = HASHES.indexOf(string(name).toUpperCase());
      if (index < 0) fail("NotSupportedError", "Unsupported hash: " + name);
      return index;
    };
    const run = (subtle, operation, state, a, b, c, d) =>
      ordered(subtle, runNative(operation, state.handle, a, b, c, d));
    /* then(value), now for a result and later for a promise of one. */
    const after = (value, then) => isArrayBuffer(value) ? then(value)
      : promiseThen(value, then);
    /* The encrypt and decrypt operations, which wrapKey and unwrapKey
       also run (their usages are checked by the caller). */
    const cipher = (params, state, data, encrypt, subtle) => {
      const kind = params.kind;
      if (kind === GCM) {
        const tag = params.tag === undefined ? 128 : params.tag;
        if (!TAG_LENGTHS.includes(tag))
          fail("OperationError", "Unsupported tagLength");
        if (!encrypt && data.length < tag / 8)
          fail("OperationError", "The data is shorter than the tag");
        return run(subtle, encrypt ? 1 : 2, state, params.iv,
          params.aad || EMPTY, data, tag / 8);
      }
      if (kind === CBC) {
        if (params.iv.length !== 16)
          fail("OperationError", "The iv must be 16 bytes");
        return run(subtle, encrypt ? 3 : 4, state, params.iv, data);
      }
      if (params.counter.length !== 16 || params.length === 0
        || params.length > 128)
        fail("OperationError", "Invalid counter or length");
      return run(subtle, 5, state, params.counter, params.length, data);
    };
    const mac = (state, data, subtle) => run(subtle, 0, state, state.hash,
      data);
    /* PBKDF2 jobs belong to the scope that asked for them: the
       SubtleCrypto of the page, a frame or a worker, each its own
       (compat.js). One task runs one slice: the first scope in turn runs
       the next slice of its first running job, and the job then goes to
       the back of its scope's jobs and the scope to the back of the turn.
       Every scope with work gets the same share of the tasks however many
       jobs it holds, so a frame or worker with a backlog cannot hold up
       the page, nor the page them. A scope runs up to 16 jobs; the rest
       wait in order, holding only what they were charged, and start as
       its jobs end. The slice task's slot is reserved (scheduler.js): a
       page that fills the task table cannot stall a derivation or make it
       finish in one task. If that slot is refused anyway, every job
       rejects. A settling slice can run page code (a then getter), which
       may start another derivation: armed keeps that to one task. A
       scope that goes away stops its jobs (stopScope). Intrusive links
       keep a job that needs another slice from allocating. The scope's
       other operations take their turns the same way (ordered). */
    const queue = () => ({ __proto__: null, head: null, tail: null }),
      put = (list, entry) => {
        if (list.tail) list.tail.next = entry;
        else list.head = entry;
        list.tail = entry;
      },
      take = (list) => {
        const entry = list.head;
        list.head = entry.next;
        if (!list.head) list.tail = null;
        entry.next = null;
        return entry;
      },
      turns = queue(), scopes = new Map();
    let armed = false, stopping = false, jobBytes = 0;
    const wake = (scope) => {
      if (scope.queued || !scope.run.head) return;
      scope.queued = true;
      put(turns, scope);
    };
    const open = (subtle) => {
      let scope = scopes.get(subtle);
      if (!scope)
        scopes.set(subtle, scope = { __proto__: null, next: null,
          key: subtle, run: queue(), wait: queue(), order: queue(),
          running: 0, ordered: 0, queued: false, stopped: false });
      return scope;
    };
    /* A scope with no work left is forgotten. */
    const idle = (scope) => {
      if (!scope.running && !scope.wait.head && !scope.ordered
        && scopes.get(scope.key) === scope) scopes.delete(scope.key);
    };
    const start = (scope, entry) => {
      entry.running = true;
      scope.running++;
      put(scope.run, entry);
      wake(scope);
    };
    const sliceTurn = () => {
      while (turns.head) {
        const scope = take(turns);
        scope.queued = false;
        if (!scope.run.head) continue;
        const entry = take(scope.run);
        if (entry.step()) put(scope.run, entry);
        wake(scope);
        break;
      }
      armed = false;
      armSlices();
    };
    /* Ends every job of scope: rejects them when error is given, or drops
       them unsettled when nothing can observe them any more. */
    const stopScope = (scope, error) => {
      if (scopes.get(scope.key) === scope) scopes.delete(scope.key);
      scope.stopped = true;
      for (const list of [scope.run, scope.wait])
        while (list.head) take(list).end(error);
    };
    const stopped = () => new DOMException("The operation was stopped",
      "OperationError");
    const armSlices = () => {
      if (armed || !turns.head) return;
      try {
        armed = !!domShared.scheduleReservedTask(sliceTurn, 0);
      } catch (_) {}
      if (!armed) {
        stopping = true;
        for (const scope of scopes.values()) stopScope(scope, stopped());
        turns.head = turns.tail = null;
        stopping = false;
      }
    };
    /* A stopped scope (a terminated worker's, a removed, navigated or
       closed frame document's) stays stopped: what code still holding its
       SubtleCrypto asks for later rejects at once. A null observable
       resumes it (frames.js: the frame's realm is live again). */
    const stoppedScopes = globalThis.__tilefinchCreatePrivateWeakMap();
    domShared.stopCryptoJobs = (subtle, observable) => {
      if (!subtle) return;
      if (observable === null) return stoppedScopes.delete(subtle);
      stoppedScopes.set(subtle, true);
      const scope = scopes.get(subtle);
      if (scope) stopScope(scope, observable ? stopped() : null);
    };
    /* PBKDF2 gives a promise: its derivation runs in slices of a few
       milliseconds (src/web_crypto.c), in those tasks, and the first in
       this call when no other derivation is pending, so a long one leaves
       input and the page's other tasks their turns and stops with the
       page. A running job holds its progress (136 bytes), then its
       output. */
    const derive = (params, state, length, subtle) => {
      if (length === null || length % 8)
        fail("OperationError", "length must be a multiple of 8");
      if (params.kind === PBKDF2) {
        if (length === 0 || params.iterations === 0)
          fail("OperationError", "length and iterations must not be zero");
        const outputBytes = length / 8, hashBytes = HASH_BYTES[params.hash],
          blocks = ((outputBytes - 1) / hashBytes | 0) + 1;
        if (outputBytes > PBKDF2_INPUT_LIMIT ||
            params.iterations > PBKDF2_WORK_LIMIT / blocks)
          fail("OperationError", "PBKDF2 derivation is too large");
        let { salt } = params, handle = state.handle, job = null;
        const hash = params.hash, iterations = params.iterations,
          workBytes = PBKDF2_STATE_BYTES + outputBytes,
          charge = workBytes + byteLengthOfView(salt) + state.secretBytes
            + PBKDF2_JOB_BYTES;
        if (stopping || charge > PBKDF2_QUEUE_BYTES - jobBytes)
          fail("OperationError", "Pending PBKDF2 derivations use too much memory");
        let scope;
        return new Promise((resolve, reject) => {
          const entry = { __proto__: null, next: null, running: false };
          /* Settles with error or output; a null error drops the job. */
          entry.end = (error, output) => {
            if (job) fillBytes(job, 0);
            job = salt = handle = null;
            jobBytes -= charge;
            if (entry.running) {
              entry.running = false;
              scope.running--;
              while (!scope.stopped && scope.running < PBKDF2_JOB_LIMIT
                && scope.wait.head) start(scope, take(scope.wait));
            }
            idle(scope);
            if (error) reject(error);
            else if (error === undefined) resolve(output);
          };
          /* One slice: true while more remain. */
          entry.step = () => {
            let output;
            try {
              job ||= new Uint8Array(workBytes);
              const status = runNative(6, handle, hash, salt, iterations, job);
              if (status === null)
                fail("OperationError", "The operation failed");
              if (!status) return true;
              output = bufferOfView(bufferBytes(bufferOfView(job),
                PBKDF2_STATE_BYTES, outputBytes));
            } catch (error) {
              entry.end(error);
              return false;
            }
            entry.end(undefined, output);
            return false;
          };
          scope = open(subtle);
          jobBytes += charge;
          if (scope.running >= PBKDF2_JOB_LIMIT) put(scope.wait, entry);
          else if (armed) start(scope, entry);
          else {
            entry.running = true;
            scope.running++;
            if (entry.step()) {
              put(scope.run, entry);
              wake(scope);
            }
          }
          armSlices();
        });
      }
      if (length === 0) return ordered(subtle, new ArrayBuffer(0));
      return run(subtle, 7, state, params.hash, params.salt, params.info,
        length / 8);
    };
    /* Encryption, signatures, digests and HKDF settle in the order a
       scope asks for them, failures too. One whose work is over a slice
       comes back from the native as a job (src/web_crypto.c) that runs a
       slice in each of the scope's turns; while one is pending, the
       scope's later results wait behind it, and each job starts as the one
       before ends, the results already known before it settling at once.
       A job holds copies of its input in native memory, and its backend
       contexts only from its first slice. */
    const ordered = (subtle, result) => {
      if (stoppedScopes.get(subtle)) throw stopped();
      let scope = scopes.get(subtle);
      const ready = result === null || isArrayBuffer(result);
      if (ready && !scope?.ordered) {
        if (result === null) fail("OperationError", "The operation failed");
        return result;
      }
      return new Promise((resolve, reject) => {
        scope ||= open(subtle);
        const entry = { __proto__: null, next: null, running: false, ready };
        /* Settles with output or error; a null error drops it. A known
           result settles as itself. */
        entry.settle = (error, output) => {
          if (error === undefined && ready) {
            output = result;
            if (output === null) error = new DOMException(
              "The operation failed", "OperationError");
          }
          result = null;
          scope.ordered--;
          if (error) reject(error);
          else if (error === undefined) resolve(output);
        };
        entry.end = (error, output) => {
          entry.settle(error, output);
          if (entry.running) {
            entry.running = false;
            /* A stopped scope's waiting operations end as this one does,
               or are dropped if it succeeded. */
            if (scope.stopped)
              while (scope.order.head) take(scope.order).settle(error || null);
            else {
              while (scope.order.head?.ready) take(scope.order).settle();
              if (scope.order.head) {
                const next = take(scope.order);
                next.running = true;
                put(scope.run, next);
                wake(scope);
              }
            }
          }
          idle(scope);
        };
        entry.step = () => {
          let output = result;
          try {
            output = runNative(9, output);
            if (output === false) return true;
            if (output === null) fail("OperationError", "The operation failed");
          } catch (error) {
            entry.end(error);
            return false;
          }
          entry.end(undefined, output);
          return false;
        };
        if (scope.ordered++) put(scope.order, entry);
        else {
          entry.running = true;
          put(scope.run, entry);
          wake(scope);
        }
        armSlices();
      });
    };
    domShared.orderCrypto = ordered;
    const keyLength = (params) => {
      const kind = params.kind;
      if (kind < HMAC) {
        if (params.length !== 128 && params.length !== 192
          && params.length !== 256)
          fail("OperationError", "AES key length must be 128, 192 or 256");
        return params.length;
      }
      if (kind > HMAC) return null;
      const length = params.length === undefined
        ? (params.hash < 2 ? 512 : 1024) : params.length;
      if (!length) fail("OperationError", "length must not be zero");
      return length;
    };
    const base64url = (buffer) => (buffer.byteLength
      ? __tilefinchBase64EncodeBytes(new Uint8Array(buffer)) : "")
      .replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    const fromBase64url = (text) => {
      let binary;
      try {
        if (!/^[A-Za-z0-9_-]*$/.test(text)) throw 0;
        binary = __tilefinchBase64DecodeString(
          text.replace(/-/g, "+").replace(/_/g, "/"));
      } catch (_) {
        fail("DataError", "The JWK k member is not base64url");
      }
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      return bytes;
    };
    /* A JsonWebKey dictionary's members this module reads. */
    const jwkOf = (value) => {
      if (value === undefined || value === null) value = {};
      else if (typeof value !== "object" && typeof value !== "function")
        throw new TypeError("A JsonWebKey dictionary is required");
      const optional = (member) =>
        member === undefined ? undefined : string(member);
      const jwk = { alg: optional(value.alg) },
        ext = value.ext;
      jwk.ext = ext === undefined ? undefined : !!ext;
      jwk.k = optional(value.k);
      const operations = value.key_ops;
      if (operations !== undefined) {
        if (operations === null || typeof operations !== "object")
          throw new TypeError("key_ops must be a sequence");
        jwk.key_ops = [];
        for (const operation of operations) jwk.key_ops.push(string(operation));
      }
      jwk.kty = optional(value.kty);
      jwk.use = optional(value.use);
      return jwk;
    };
    /* Imports raw bytes (which it zeroes) or a JWK dictionary. */
    const importKey = (form, data, params, extractable, mask) => {
      const kind = params.kind, aes = kind < HMAC;
      let bytes = form === "raw" ? data : EMPTY, alg;
      try {
        if (mask & ~(aes ? AES_USES : kind === HMAC ? SIGN_USES
          : DERIVE_USES))
          fail("SyntaxError", "Unsupported key usage for " + NAMES[kind]);
        if (kind > HMAC) {
          if (form !== "raw")
            fail("NotSupportedError", NAMES[kind] + " keys import as raw only");
          if (extractable)
            fail("SyntaxError", NAMES[kind] + " keys cannot be extractable");
        }
        if (form === "jwk") {
          if (data.kty !== "oct" || data.k === undefined)
            fail("DataError", "The JWK is not a secret key");
          if (mask && data.use !== undefined
            && data.use !== (aes ? "enc" : "sig"))
            fail("DataError", "The JWK use member does not allow this key");
          if (data.key_ops !== undefined) {
            let listed = 0;
            for (const operation of data.key_ops) {
              /* An unknown operation shifts by -1: a negative bit. */
              const bit = 1 << USAGES.indexOf(operation);
              if (bit > 0 && listed & bit)
                fail("DataError", "The JWK key_ops repeats a usage");
              if (bit > 0) listed |= bit;
            }
            if (mask & ~listed)
              fail("DataError", "The JWK key_ops does not allow these usages");
          }
          if (data.ext === false && extractable)
            fail("DataError", "The JWK is not extractable");
          alg = data.alg;
          bytes = fromBase64url(data.k);
        } else if (form !== "raw")
          fail("NotSupportedError", "Unsupported key format for "
            + NAMES[kind]);
        let bits = bytes.length * 8;
        if (aes) {
          if (bits !== 128 && bits !== 192 && bits !== 256)
            fail("DataError", "AES keys are 128, 192 or 256 bits");
          if (alg !== undefined && alg !== "A" + bits + NAMES[kind].slice(4))
            fail("DataError", "The JWK alg member does not match");
        } else if (kind === HMAC) {
          if (!bits) fail("DataError", "HMAC keys must not be empty");
          if (params.length !== undefined) {
            if (params.length > bits || params.length <= bits - 8)
              fail("DataError", "length does not match the key data");
            bits = params.length;
          }
          if (alg !== undefined && alg !== "HS" + HASHES[params.hash].slice(4))
            fail("DataError", "The JWK alg member does not match");
        }
        if (!mask) fail("SyntaxError", "Key usages must not be empty");
        const handle = keyNative(bytes);
        if (handle === null) fail("DataError", "The key data is too large");
        return makeKey(handle, byteLengthOfView(bytes), kind, params.hash,
          bits, extractable, mask);
      } finally {
        fillBytes(bytes, 0);
      }
    };
    const makeKey = (handle, secretBytes, kind, hash, bits, extractable, uses) => {
      const algorithm = kind < HMAC ? { name: NAMES[kind], length: bits }
        : kind === HMAC ? { name: "HMAC", length: bits, hash: { name: HASHES[hash] } }
        : { name: NAMES[kind] };
      const created = { __proto__: CryptoKey.prototype };
      /* Only this private state can access the native secret. The public
         algorithm and usages objects are mutable views, not the underlying
         metadata serialized by structured clone. */
      keyStates.set(created, {
        type: "secret", extractable, algorithm, usages: usageList(uses),
        uses, kind, hash, bits, handle, secretBytes,
      });
      return created;
    };
    domShared.cloneCryptoKey = (value) => {
      const state = keyStates.get(value);
      if (state === undefined) return undefined;
      return makeKey(keyNative(state.handle, true), state.secretBytes,
        state.kind, state.hash, state.bits, state.extractable, state.uses);
    };
    domShared.cryptoKeySize = (value) => {
      const state = keyStates.get(value);
      return state === undefined ? undefined : 128 + state.secretBytes;
    };
    /* The window wire contains only offsets. This separate, private buffer
       carries the key material and immutable metadata, never public views
       such as algorithm or usages. All byte access is through built-ins
       captured above, or direct indexing of genuine Uint8Arrays. */
    const FRAME_KEY_HEADER = 12,
      put32 = (bytes, offset, value) => {
        for (let i = 0; i < 4; i++) bytes[offset + i] = value >>> (8 * i);
      },
      get32 = (bytes, offset) => (
        bytes[offset] | bytes[offset + 1] << 8 |
        bytes[offset + 2] << 16 | bytes[offset + 3] << 24) >>> 0;
    domShared.frameKeySize = (value) => {
      const state = keyStates.get(value);
      return state === undefined ? undefined : FRAME_KEY_HEADER + state.secretBytes;
    };
    domShared.writeFrameKey = (value, bytes, offset) => {
      const state = keyStates.get(value);
      bytes[offset] = state.kind;
      bytes[offset + 1] = state.hash === undefined ? 0 : state.hash + 1;
      bytes[offset + 2] = state.extractable ? 1 : 0;
      bytes[offset + 3] = state.uses;
      put32(bytes, offset + 4, state.bits);
      put32(bytes, offset + 8, state.secretBytes);
      const secret = new Uint8Array(keyNative(state.handle));
      try {
        for (let i = 0; i < state.secretBytes; i++)
          bytes[offset + FRAME_KEY_HEADER + i] = secret[i];
      } finally {
        fillBytes(secret, 0);
      }
    };
    domShared.readFrameKey = (bytes, offset) => {
      const size = byteLengthOfView(bytes);
      if (!(offset >= 0 && offset <= size - FRAME_KEY_HEADER))
        fail("DataCloneError", "Invalid message key offset");
      const length = get32(bytes, offset + 8),
        bits = get32(bytes, offset + 4),
        kind = bytes[offset], hash = bytes[offset + 1],
        extractable = bytes[offset + 2], uses = bytes[offset + 3];
      if (length > size - offset - FRAME_KEY_HEADER ||
          kind > 5 || hash > 4 || extractable > 1)
        fail("DataCloneError", "Invalid message key state");
      const secret = new Uint8Array(bufferOfView(bytes),
        offset + FRAME_KEY_HEADER, length);
      return makeKey(keyNative(secret), length, kind,
        hash ? hash - 1 : undefined, bits, !!extractable, uses);
    };
    const exportKey = (form, state) => {
      if (state.kind > HMAC)
        fail("NotSupportedError", NAMES[state.kind] + " keys cannot be exported");
      if (!state.extractable)
        fail("InvalidAccessError", "The key is not extractable");
      if (form === "raw") return keyNative(state.handle);
      if (form !== "jwk")
        fail("NotSupportedError", "Secret keys export as raw or jwk only");
      const raw = new Uint8Array(keyNative(state.handle)),
        jwk = {
          alg: state.kind === HMAC ? "HS" + HASHES[state.hash].slice(4)
            : "A" + raw.length * 8 + NAMES[state.kind].slice(4),
          ext: true,
          k: base64url(raw.buffer),
          key_ops: usageList(state.uses),
          kty: "oct",
        };
      raw.fill(0);
      return jwk;
    };
    const bytesOf = (value, maximum) => {
      if (isArrayBuffer(value)) return bufferBytes(value, 0, undefined, maximum);
      if (isView(value)) return viewBytes(value, maximum);
      throw new TypeError("A BufferSource is required");
    };
    const operations = {
      encrypt: (args, subtle) => {
        const state = key(args[1]), data = bytesOf(args[2]),
          params = normalize(args[0], ENC);
        checkKey(state, params.kind, 1);
        return cipher(params, state, data, true, subtle);
      },
      decrypt: (args, subtle) => {
        const state = key(args[1]), data = bytesOf(args[2]),
          params = normalize(args[0], ENC);
        checkKey(state, params.kind, 2);
        return cipher(params, state, data, false, subtle);
      },
      sign: (args, subtle) => {
        const state = key(args[1]), data = bytesOf(args[2]),
          params = normalize(args[0], SIGN);
        checkKey(state, params.kind, 4);
        return mac(state, data, subtle);
      },
      verify: (args, subtle) => {
        const state = key(args[1]), signature = bytesOf(args[2]),
          data = bytesOf(args[3]), params = normalize(args[0], SIGN);
        checkKey(state, params.kind, 8);
        return after(mac(state, data, subtle), (buffer) => {
          const expected = new Uint8Array(buffer);
          if (expected.length !== signature.length) return false;
          let difference = 0;
          for (let i = 0; i < expected.length; i++)
            difference |= expected[i] ^ signature[i];
          return difference === 0;
        });
      },
      generateKey: (args) => {
        const extractable = !!args[1], mask = usageMask(args[2]),
          params = normalize(args[0], GEN), kind = params.kind;
        if (mask & ~(kind < HMAC ? AES_USES : SIGN_USES))
          fail("SyntaxError", "Unsupported key usage for " + NAMES[kind]);
        const bits = keyLength(params);
        if (bits > KEY_BYTES_LIMIT * 8)
          fail("DataError", "The key data is too large");
        const length = (bits + 7) >>> 3, bytes = new Uint8Array(length);
        try {
          if (length <= RANDOM_FILL_LIMIT) randomFill(bytes);
          else {
            const buffer = bufferOfView(bytes);
            for (let offset = 0; offset < length; offset += RANDOM_FILL_LIMIT) {
              const remaining = length - offset;
              randomFill(new Uint8Array(buffer, offset,
                remaining < RANDOM_FILL_LIMIT ? remaining : RANDOM_FILL_LIMIT));
            }
          }
        } catch (error) {
          fillBytes(bytes, 0);
          throw error;
        }
        if (bits & 7) bytes[length - 1] &= 0xff << (8 - (bits & 7));
        if (kind === HMAC) params.length = bits;
        return importKey("raw", bytes, params, extractable, mask);
      },
      deriveBits: (args, subtle) => {
        const state = key(args[1]), length = args[2] === undefined
          || args[2] === null ? null : args[2] >>> 0,
          params = normalize(args[0], DERIVE);
        checkKey(state, params.kind, 32);
        return derive(params, state, length, subtle);
      },
      deriveKey: (args, subtle) => {
        const state = key(args[1]), extractable = !!args[3],
          mask = usageMask(args[4]), params = normalize(args[0], DERIVE),
          target = normalize(args[2], IMPORT),
          length = normalize(args[2], LENGTH);
        checkKey(state, params.kind, 16);
        const derived = derive(params, state, keyLength(length), subtle),
          made = (secret) => importKey("raw", new Uint8Array(secret), target,
            extractable, mask);
        return after(derived, made);
      },
      importKey: (args) => {
        const form = format(args[0]), raw = args[1];
        let data;
        if (isArrayBuffer(raw) || isView(raw)) {
          if (form === "jwk")
            throw new TypeError("A jwk import needs a JsonWebKey dictionary");
          data = bytesOf(raw);
        } else {
          if (form !== "jwk") throw new TypeError("A BufferSource is required");
          data = jwkOf(raw);
        }
        const extractable = !!args[3], mask = usageMask(args[4]);
        return importKey(form, data, normalize(args[2], IMPORT), extractable,
          mask);
      },
      exportKey: (args) => exportKey(format(args[0]), key(args[1])),
      wrapKey: (args, subtle) => {
        const form = format(args[0]), state = key(args[1]),
          wrapping = key(args[2]), params = normalize(args[3], ENC);
        checkKey(wrapping, params.kind, 64);
        const exported = exportKey(form, state),
          bytes = form === "jwk"
            ? new TextEncoder().encode(JSON.stringify(exported))
            : new Uint8Array(exported);
        try {
          return cipher(params, wrapping, bytes, true, subtle);
        } finally {
          bytes.fill(0);
        }
      },
      unwrapKey: (args, subtle) => {
        const form = format(args[0]), wrapped = bytesOf(args[1]),
          unwrapping = key(args[2]), extractable = !!args[5],
          mask = usageMask(args[6]), params = normalize(args[3], ENC),
          target = normalize(args[4], IMPORT);
        checkKey(unwrapping, params.kind, 128);
        return after(cipher(params, unwrapping, wrapped, false, subtle),
          (buffer) => {
            const bytes = new Uint8Array(buffer);
            let data = bytes;
            if (form === "jwk") {
              try {
                data = jwkOf(JSON.parse(new TextDecoder("utf-8",
                  { fatal: true }).decode(bytes)));
              } catch (_) {
                fail("DataError", "The unwrapped key is not a JWK");
              } finally {
                bytes.fill(0);
              }
            }
            return importKey(form, data, target, extractable, mask);
          });
      },
    };
    /* An operation's result as a promise, its checks' errors as a
       rejection; a worker's ArrayBuffer results go through its realm's
       normalizer. */
    const call = (subtle, name, args) => {
      try {
        if (!subtleInstances.has(subtle))
          throw new TypeError("Illegal invocation");
        if (stoppedScopes.get(subtle)) throw stopped();
        const normalizers = normalizerSlot.get(subtle),
          result = promiseResolve(operations[name](args, subtle));
        return normalizers ? promiseThen(result, (value) =>
          isArrayBuffer(value) ? normalizers.output(value) : value) : result;
      } catch (error) {
        return promiseReject(error);
      }
    };
    const methods = {
      encrypt(algorithm, key, data) {
        return call(this, "encrypt", arguments);
      },
      decrypt(algorithm, key, data) {
        return call(this, "decrypt", arguments);
      },
      sign(algorithm, key, data) {
        return call(this, "sign", arguments);
      },
      verify(algorithm, key, signature, data) {
        return call(this, "verify", arguments);
      },
      generateKey(algorithm, extractable, keyUsages) {
        return call(this, "generateKey", arguments);
      },
      deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages) {
        return call(this, "deriveKey", arguments);
      },
      deriveBits(algorithm, baseKey) {
        return call(this, "deriveBits", arguments);
      },
      importKey(format, keyData, algorithm, extractable, keyUsages) {
        return call(this, "importKey", arguments);
      },
      exportKey(format, key) {
        return call(this, "exportKey", arguments);
      },
      wrapKey(format, key, wrappingKey, wrapAlgorithm) {
        return call(this, "wrapKey", arguments);
      },
      unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm,
        unwrappedKeyAlgorithm, extractable, keyUsages) {
        return call(this, "unwrapKey", arguments);
      },
    };
    const attribute = (key) => {
      const state = keyStates.get(key);
      if (!state) throw new TypeError("Illegal invocation");
      return state;
    };
    const attributes = {
      get type() {
        return attribute(this).type;
      },
      get extractable() {
        return attribute(this).extractable;
      },
      get algorithm() {
        return attribute(this).algorithm;
      },
      get usages() {
        return attribute(this).usages;
      },
    };
    /* Adding the members is a step of its own: one that ran out of memory
       part way is run again, adding the same functions. */
    return () => {
      for (const name of ["type", "extractable", "algorithm", "usages"])
        defineProperty(CryptoKey.prototype, name, {
          __proto__: null,
          configurable: true,
          get: getOwnPropertyDescriptor(attributes, name).get,
        });
      for (const name of ["encrypt", "decrypt", "sign", "verify",
        "generateKey", "deriveKey", "deriveBits", "importKey", "exportKey",
        "wrapKey", "unwrapKey"])
        defineProperty(SubtleCrypto.prototype, name, {
          __proto__: null,
          configurable: true,
          writable: true,
          value: methods[name],
        });
    };
  },
});
