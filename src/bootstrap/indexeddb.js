(() => {
  /* Loaded after the page may have replaced these globals: build
     errors, promises and platform objects with the constructors
     hardening.js captured, and publish private hooks with its
     defineProperty. */
  const { defineProperty, Error, TypeError, DOMException, Promise, Map, Set,
    Array, ArrayBuffer, Uint8Array, Date, RegExp, Blob, claimHooks,
    apply: intrinsicApply } = globalThis.__tilefinchEventInterfaces;
  /* This module's hook publisher, claimed first (hardening.js); a realm
     without reserved hooks takes plain globals. */
  const privatePublish = claimHooks?.(),
    publish = privatePublish || ((name, value) => { globalThis[name] = value; });
  let cryptoKeySize, cloneForStorage, workerFactory;
  /* platform.js keeps what this returns in its private share, where the
     Worker module finds a worker's factory (IDBFactory's workerFactory). */
  privatePublish?.("__tilefinchIndexedDBKeySizeReader",
    (read, cloneBounded) => {
      cryptoKeySize = read;
      cloneForStorage = cloneBounded;
      return (reader, release) => workerFactory(reader, release);
    });
  if (globalThis.indexedDB !== undefined) return;
  /* Dispatch passes Event's phase numbers (1 capturing, 2 at target,
     3 bubbling) as literals instead of reading them from the global Event,
     which a page can replace. */
  /* This module loads on first use, after author code; hardening.js
     captured the bootstrap's event creator before that. */
  const createEvent = globalThis.__tilefinchCreateEvent,
    { EventTarget, addListener, removeListener } =
      globalThis.__tilefinchEventInterfaces;

  const DATABASE_LIMIT = 8;
  const STORE_LIMIT = 64;
  const INDEX_LIMIT = 16;
  const RECORD_LIMIT = 8192;
  const RECORD_BYTE_LIMIT = 4 * 1024 * 1024;
  const BYTE_LIMIT = 16 * 1024 * 1024;
  const REQUEST_LIMIT = 4096;
  const PENDING_REQUEST_LIMIT = 4096;
  const TRANSACTION_QUEUE_LIMIT = 128;
  const OPEN_QUEUE_LIMIT = 32;
  const KEY_BINARY_BYTE_LIMIT = 4096;
  const databases = new Map();
  const connectionQueues = new Map();
  const arrayIncludes = Array.prototype.includes;
  const arrayJoin = Array.prototype.join;
  /* Record identity must not be author-controllable. A page that replaces
     JSON.stringify makes records.delete(keyToken(...)) miss the entry while
     the byte and record subtraction still runs, so stats.bytes and
     stats.records walk negative and the 16 MiB quota stops bounding
     anything. */
  const trustedJSONStringify = JSON.stringify;
  const trustedStructuredClone = globalThis.__tilefinchCloneWorkerValue,
    trustedCloneIntrinsics = globalThis.__tilefinchWorkerCloneIntrinsics.owner,
    trustedScheduleTask = globalThis.__tilefinchScheduleTask,
    trustedQueueCheckpointContinuation =
      globalThis.__tilefinchQueueCheckpointContinuation;
  const { arrayBufferByteLength, bufferSourceBacking, dateTime, isView,
    objectTag, prototypeOf } = globalThis.__tilefinchWorkerCloneIntrinsics;
  /* A live worker realm's constructors (workerFactory), by its
     Function.prototype: IDBKeyRange's statics called on a worker's
     IDBKeyRange find them. The constructors reach the key, and QuickJS
     marks weak-map values strongly, so the worker's termination removes
     its entry, or the realm would stay for the page's life. */
  const realmReaders = globalThis.__tilefinchCreatePrivateWeakMap(),
    realmReader = (realm) => typeof realm === "function"
      && realmReaders.get(prototypeOf(realm)) || trustedCloneIntrinsics;
  delete globalThis.__tilefinchQueueCheckpointContinuation;
  let activityCloses = [];
  let activityCloseScheduled = false;
  const scheduleDatabaseTask = (callback) => {
    const id = intrinsicApply(trustedScheduleTask, globalThis, [callback]);
    return id !== 0;
  }, closeTransactionActivities = () => {
    activityCloseScheduled = false;
    const closing = activityCloses,
      count = closing.length;
    /* Detach before invoking transaction code: quota-triggered abort handlers
       can synchronously create a replacement transaction, which must enter a
       fresh batch rather than be erased with this one. */
    activityCloses = [];
    for (let index = 0; index < count; index++) {
      const transaction = closing[index];
      if (transaction._abortSequence) {
        transaction._drainAbortSequence(transaction._abortSequence, false);
      } else if (transaction._state !== "finished") {
        transaction._acceptingRequests = false;
        transaction._maybeComplete();
      }
    }
    closing.length = 0;
  }, scheduleActivityClose = (transaction) => {
    if (!intrinsicApply(arrayIncludes, activityCloses, [transaction])) {
      if (activityCloses.length >= TRANSACTION_QUEUE_LIMIT) return false;
      activityCloses[activityCloses.length] = transaction;
    }
    if (activityCloseScheduled) return true;
    try {
      intrinsicApply(
        trustedQueueCheckpointContinuation, globalThis,
        [closeTransactionActivities]);
      activityCloseScheduled = true;
      return true;
    } catch (_) {
      activityCloses.length = 0;
      return false;
    }
  };
  const stats = {
    opens: 0,
    deletes: 0,
    transactions: 0,
    requests: 0,
    records: 0,
    bytes: 0,
    peakBytes: 0,
    quotaErrors: 0,
  };
  /* A read-only view for host tests (test-probe realms only). */
  if (globalThis.__tilefinchTestProbes) {
    const statsView = {};
    for (const key of Object.keys(stats))
      defineProperty(statsView, key, {
        enumerable: true,
        get: () => stats[key],
      });
    Object.freeze(statsView);
    defineProperty(globalThis, "__tilefinchIndexedDBStats", { __proto__: null,
      value: statsView,
      writable: false,
      configurable: false,
    });
  }
  /* Read directly by the native result snapshot (see compat.js). */
  globalThis.__tilefinchHostChannel.indexedDBStats = stats;

  const fail = (message, name) => new DOMException(message, name);
  /* A read through transaction builds values and keys with its
     connection's constructors: a worker's own (IDBFactory). */
  const readerOf = (transaction) =>
    transaction && idbReader(transaction.db) || trustedCloneIntrinsics;
  const clone = (value, transaction) => {
    try {
      return intrinsicApply(trustedStructuredClone, globalThis, [
        value, readerOf(transaction),
      ]);
    } catch (_) {
      throw fail("The value could not be cloned", "DataCloneError");
    }
  };
  const retainedSize = (value, depth = 0, seen = new Set()) => {
    if (depth > 8)
      throw fail("The value is nested too deeply", "DataCloneError");
    if (value === null || value === undefined) return 8;
    if (typeof value === "string") return 16 + value.length * 2;
    if (
      typeof value === "number" || typeof value === "boolean" ||
      typeof value === "bigint"
    ) return 16;
    /* Test the private CryptoKey brand before the prototype-based checks:
       a key with an altered prototype still retains its native secret. */
    const keySize = cryptoKeySize?.(value);
    if (keySize !== undefined) {
      if (seen.has(value)) return 8;
      seen.add(value);
      return keySize;
    }
    if (value instanceof Date) return 24;
    if (value instanceof RegExp)
      return 32 + value.source.length * 2 + value.flags.length * 2;
    if (typeof value !== "object")
      throw fail("The value could not be cloned", "DataCloneError");
    /* Structured serialization preserves aliases and cycles. Count a bounded
       reference for an already visited object instead of rejecting a value
       the clone algorithm accepts, and keep the set for the whole graph so
       aliases are not charged as duplicate retained payloads. */
    if (seen.has(value)) return 8;
    seen.add(value);
    if (value instanceof Blob)
      return 48 + value.size + value.type.length * 2 +
        (typeof File !== "undefined" && value instanceof File
          ? value.name.length * 2 + 16
          : 0);
    const backing = bufferSourceBacking(value);
    if (backing === value) return 32 + arrayBufferByteLength(backing);
    if (backing !== undefined) {
      // Structured clone copies the entire backing buffer, not the view's
      // slice. Count it once, even if the record also holds the buffer itself.
      if (seen.has(backing)) return 48 + 8;
      seen.add(backing);
      return 48 + 32 + arrayBufferByteLength(backing);
    }
    let total = 32;
    if (value instanceof Map) {
      if (value.size > 1024)
        throw fail("The map exceeds the storage item limit", "DataCloneError");
      for (const [key, item] of value) {
        total += retainedSize(key, depth + 1, seen);
        total += retainedSize(item, depth + 1, seen);
        if (total > BYTE_LIMIT) break;
      }
    } else if (value instanceof Set) {
      if (value.size > 1024)
        throw fail("The set exceeds the storage item limit", "DataCloneError");
      for (const item of value) {
        total += retainedSize(item, depth + 1, seen);
        if (total > BYTE_LIMIT) break;
      }
    } else if (value instanceof Error) {
      total += value.name.length * 2 + value.message.length * 2;
      if (typeof value.stack === "string") total += value.stack.length * 2;
      if (Object.prototype.hasOwnProperty.call(value, "cause"))
        total += retainedSize(value.cause, depth + 1, seen);
    } else if (
      value instanceof Boolean || value instanceof Number ||
      value instanceof String
    ) {
      total += retainedSize(value.valueOf(), depth + 1, seen);
    } else if (Array.isArray(value)) {
      if (value.length > 65536)
        throw fail(
          "The array exceeds the storage item limit",
          "DataCloneError",
        );
      for (const item of value) {
        total += retainedSize(item, depth + 1, seen);
        if (total > BYTE_LIMIT) break;
      }
    } else if (Object.getPrototypeOf(value) === Object.prototype) {
      const keys = Object.keys(value);
      if (keys.length > 1024)
        throw fail(
          "The object exceeds the storage key limit",
          "DataCloneError",
        );
      for (const key of keys) {
        total +=
          16 + key.length * 2 + retainedSize(value[key], depth + 1, seen);
        if (total > BYTE_LIMIT) break;
      }
    } else {
      throw fail("The value could not be cloned", "DataCloneError");
    }
    return total;
  };
  const serializedSize = (key, value) =>
    64 + retainedSize(key) + retainedSize(value);
  const boundedName = (value, maximum, label) => {
    value = String(value);
    if (!value.length || value.length > maximum)
      throw fail(
        `${label} is outside the supported bound`,
        "QuotaExceededError",
      );
    return value;
  };
  const boundedKeyPath = (value) => {
    if (value === null) return null;
    if (Array.isArray(value)) {
      if (value.length > 16)
        throw fail(
          "Key path is outside the supported bound",
          "QuotaExceededError",
        );
      return value.map((item) => boundedName(item, 256, "Key path"));
    }
    return boundedName(value, 256, "Key path");
  };
  /* Key types by rank: 0 number, 1 date, 2 string, 3 binary, 4 array, -1
     none. A worker's keys are objects of its own realm, so the checks are
     brands, not instanceof: the captured getters throw for anything but a
     real Date or ArrayBuffer. instanceof and the class tag only choose
     which to try, so the browser's own copies take the shortest path and
     no real key makes a getter throw. A Date's time is left in keyTime. */
  let keyTime;
  const keyTypeRank = (key) => {
    if (typeof key === "number") return 0;
    if (typeof key === "string") return 2;
    if (typeof key !== "object" || key === null) return -1;
    if (!(key instanceof Date)) {
      if (Array.isArray(key)) return 4;
      if (isView(key)) return 3;
      if (key instanceof ArrayBuffer || objectTag(key) !== "[object Date]")
        try {
          arrayBufferByteLength(key);
          return 3;
        } catch (_) {
          return -1;
        }
    }
    try {
      keyTime = dateTime(key);
      return 1;
    } catch (_) {
      return -1;
    }
  };
  /* The bytes of a binary key (rank 3). */
  const binaryKeyBytes = (key) => isView(key)
    ? new Uint8Array(bufferSourceBacking(key), key.byteOffset, key.byteLength)
    : new Uint8Array(key);
  const keyTokenValue = (key) => {
    if (typeof key === "string") return ["string", key];
    if (typeof key === "number")
      return [
        "number",
        key === Infinity
          ? "+infinity"
          : key === -Infinity
            ? "-infinity"
            : key === 0 ? 0 : key,
      ];
    const rank = keyTypeRank(key);
    if (rank === 1) return ["date", keyTime];
    if (rank === 3)
      return ["binary", intrinsicApply(arrayJoin, binaryKeyBytes(key), [","])];
    if (rank === 4) {
      const parts = [];
      for (let at = 0; at < key.length; at++) parts[at] = keyTokenValue(key[at]);
      return ["array", parts];
    }
    /* validateKey runs before every caller reaches keyToken. Keep this
       branch fail-closed if a future caller bypasses that contract. */
    throw fail("The key is not a supported IndexedDB key", "DataError");
  };
  const keyToken = (key) => trustedJSONStringify(keyTokenValue(key));
  const validateKey = (key, depth = 0) => {
    if (typeof key === "string") return key;
    if (typeof key === "number" && !Number.isNaN(key)) return key;
    const rank = keyTypeRank(key);
    if (rank === 1 && Number.isFinite(keyTime)) return key;
    if (rank === 3 && binaryKeyBytes(key).byteLength <= KEY_BINARY_BYTE_LIMIT)
      return key;
    if (rank === 4 && depth < 8 && key.length <= 16) {
      for (let at = 0; at < key.length; at++) validateKey(key[at], depth + 1);
      return key;
    }
    throw fail("The key is not a supported IndexedDB key", "DataError");
  };
  /* A copy of a valid key built with realm's constructors: the browser's
     own for what it stores, the reader's for what a read returns. */
  const cloneKey = (key, realm = trustedCloneIntrinsics) => {
    const rank = keyTypeRank(key);
    if (rank === 1) return new realm.Date(keyTime);
    if (rank === 3)
      return bufferSourceBacking(new realm.Uint8Array(binaryKeyBytes(key)));
    if (rank === 4) {
      const copy = new realm.Array();
      for (let at = 0; at < key.length; at++) copy[at] = cloneKey(key[at], realm);
      return copy;
    }
    return key;
  };
  const compareKeys = (left, right) => {
    const leftRank = keyTypeRank(left), leftTime = keyTime,
      rightRank = keyTypeRank(right);
    if (leftRank < 0 || rightRank < 0)
      throw fail("The key is not a supported IndexedDB key", "DataError");
    if (leftRank !== rightRank) return leftRank < rightRank ? -1 : 1;
    if (leftRank === 4) {
      const length = Math.min(left.length, right.length);
      for (let index = 0; index < length; index++) {
        const compared = compareKeys(left[index], right[index]);
        if (compared) return compared;
      }
      return left.length === right.length
        ? 0
        : left.length < right.length
          ? -1
          : 1;
    }
    if (leftRank === 3) {
      const leftBytes = binaryKeyBytes(left),
        rightBytes = binaryKeyBytes(right),
        length = Math.min(leftBytes.byteLength, rightBytes.byteLength);
      for (let index = 0; index < length; index++) {
        if (leftBytes[index] !== rightBytes[index])
          return leftBytes[index] < rightBytes[index] ? -1 : 1;
      }
      return leftBytes.byteLength === rightBytes.byteLength
        ? 0
        : leftBytes.byteLength < rightBytes.byteLength ? -1 : 1;
    }
    if (leftRank === 1) {
      left = leftTime;
      right = keyTime;
    }
    if (left === right) return 0;
    return left < right ? -1 : 1;
  };
  const keyAtPath = (value, path) => {
    if (Array.isArray(path)) return path.map((item) => keyAtPath(value, item));
    let current = value;
    for (const part of String(path).split(".")) {
      if (
        current === null ||
        current === undefined ||
        !(part in Object(current))
      )
        return undefined;
      current = current[part];
    }
    return current;
  };
  const injectKeyAtPath = (value, path, key) => {
    if (Array.isArray(path) || value === null || typeof value !== "object")
      throw fail("The generated key could not be injected", "DataError");
    const parts = String(path).split(".");
    let current = value;
    for (let index = 0; index + 1 < parts.length; index++) {
      const part = parts[index];
      /* Own properties only, as the specification injects a key: an
         inherited step ("__proto__") would lead out of the value to
         Object.prototype and define the page's name there. */
      let next = Object.hasOwn(current, part) ? current[part] : undefined;
      if (next === undefined) {
        next = {};
        defineProperty(current, part, {
          configurable: true,
          enumerable: true,
          writable: true,
          value: next,
        });
      }
      if (next === null || typeof next !== "object")
        throw fail("The generated key could not be injected", "DataError");
      current = next;
    }
    defineProperty(current, parts[parts.length - 1], {
      configurable: true,
      enumerable: true,
      writable: true,
      value: cloneKey(key),
    });
  };
  const indexKeysForValue = (value, schema) => {
    let keys = keyAtPath(value, schema.keyPath);
    keys = schema.multiEntry && Array.isArray(keys) ? keys : [keys];
    const output = [],
      seen = new Set();
    for (const key of keys) {
      if (key === undefined) continue;
      try {
        validateKey(key);
      } catch (_) {
        continue;
      }
      const token = keyToken(key);
      if (seen.has(token)) continue;
      seen.add(token);
      output.push(key);
    }
    return output;
  };
  const assertUniqueIndexes = (store, value, primaryToken) => {
    for (const schema of store.indexes.values()) {
      if (!schema.unique || schema.building) continue;
      const wanted = new Set(
        indexKeysForValue(value, schema).map((key) => keyToken(key)),
      );
      if (!wanted.size) continue;
      for (const [recordToken, record] of store.records) {
        if (recordToken === primaryToken) continue;
        for (const key of indexKeysForValue(record.value, schema))
          if (wanted.has(keyToken(key)))
            throw fail("Unique index key already exists", "ConstraintError");
      }
    }
  };

  class DOMStringList extends Array {
    contains(value) {
      return intrinsicApply(arrayIncludes, this, [String(value)]);
    }
    item(index) {
      return this[Number(index)] ?? null;
    }
  }
  const nameList = values =>
    Object.freeze(new DOMStringList(...Array.from(values, String).sort()));
  const copyKeyPath = path => Array.isArray(path) ? path.slice() : path;
  const copyStore = (store) => ({
    name: store.name,
    keyPath: Array.isArray(store.keyPath) ? [...store.keyPath] : store.keyPath,
    autoIncrement: store.autoIncrement,
    nextKey: store.nextKey,
    indexes: new Map(
      [...store.indexes].map(([name, schema]) => [
        name,
        {
          ...schema,
          keyPath: Array.isArray(schema.keyPath)
            ? [...schema.keyPath]
            : schema.keyPath,
        },
      ]),
    ),
    records: new Map(store.records),
    bytes: store.bytes,
    revision: store.revision,
  });
  const stateUsage = (state) => {
    let records = 0,
      bytes = 0;
    for (const store of state.stores.values()) {
      records += store.records.size;
      bytes += store.bytes;
    }
    return { records, bytes };
  };

  const IDB_EVENT_CANCELED = 1, IDB_EVENT_HANDLER_THREW = 2;
  const dispatchIDBEvent = (target, event, author = false) => {
    const parents = [];
    if (target instanceof IDBRequest && target.transaction) {
      parents.push(target.transaction);
      if (target.transaction.db) parents.push(target.transaction.db);
    } else if (target instanceof IDBTransaction && target.db) {
      parents.push(target.db);
    }
    globalThis.__tilefinchPrepareEvent(event, target, [target, ...parents], author);
    /* Keep the exception flag on this delivery's stack. Neither `throw null`
       nor a nested dispatch on the same request can erase it. */
    let outcome = 0;
    const observeError = () => { outcome |= IDB_EVENT_HANDLER_THREW; };
    const invoke = globalThis.__tilefinchInvokeEventTarget;
    try {
      for (let i = parents.length - 1; i >= 0 && !event.__stopped; i--)
        invoke(parents[i], event, true, 1, observeError);
      if (!event.__stopped) invoke(target, event, true, 2, observeError);
      if (!event.__stopped) invoke(target, event, false, 2, observeError);
      if (event.bubbles)
        for (let i = 0; i < parents.length && !event.__stopped; i++)
          invoke(parents[i], event, false, 3, observeError);
      return outcome | (event.defaultPrevented ? IDB_EVENT_CANCELED : 0);
    } finally {
      globalThis.__tilefinchFinishEventDispatch(event);
    }
  };
  let idbHandlerState, idbRequestState, idbReader;
  class TilefinchIDBEventTarget extends EventTarget {
    #handlers = new Map();
    static {
      idbHandlerState = target => target &&
        (typeof target === "object" || typeof target === "function") &&
        #handlers in target ? target.#handlers : undefined;
    }
    dispatchEvent(event) {
      return !(dispatchIDBEvent(this, event, true) & IDB_EVENT_CANCELED);
    }
    _dispatch(type, init = {}) {
      const event = createEvent(type, {
        bubbles: type === "error" || type === "abort",
        cancelable: type === "error",
      });
      Object.assign(event, init);
      return dispatchIDBEvent(this, event);
    }
  }
  for (const type of [
    "abort", "blocked", "close", "complete", "error", "success",
    "upgradeneeded", "versionchange",
  ])
    defineProperty(TilefinchIDBEventTarget.prototype, `on${type}`, {
      configurable: true,
      enumerable: true,
      get() {
        return idbHandlerState(this)?.get(type)?.callback || null;
      },
      set(callback) {
        const state = idbHandlerState(this);
        if (!state) throw new TypeError("Illegal invocation");
        const next = typeof callback === "function" ? callback : null;
        let entry = state.get(type);
        if (!entry && next) {
          entry = {
            callback: next,
            listener: (event) => entry.callback?.call(this, event),
          };
          state.set(type, entry);
          addListener(this, type, entry.listener);
        } else if (entry && !next) {
          removeListener(this, type, entry.listener);
          state.delete(type);
        } else if (entry) entry.callback = next;
      },
    });

  const setIDBRequestTransaction = (request, transaction) => {
    idbRequestState(request).transaction = transaction;
  };
  const prepareIDBUpgradeRequest = (request, result, transaction) => {
    const state = idbRequestState(request);
    state.result = result;
    state.error = null;
    state.transaction = transaction;
    state.readyState = "done";
  };
  const resetIDBRequest = (request) => {
    const state = idbRequestState(request);
    state.result = undefined;
    state.error = null;
    state.readyState = "pending";
  };
  const succeedIDBRequest = (request, result, redispatch = false, init) => {
    const state = idbRequestState(request);
    if (state.readyState === "done" && !redispatch) return false;
    state.result = result;
    state.error = null;
    state.readyState = "done";
    return (request._dispatch("success", init) & IDB_EVENT_HANDLER_THREW) !== 0;
  };
  const failIDBRequest = (request, error) => {
    const state = idbRequestState(request);
    if (state.readyState === "done") return 0;
    state.result = undefined;
    state.error =
      error instanceof DOMException
        ? error
        : fail(String(error), "UnknownError");
    state.readyState = "done";
    return request._dispatch("error");
  };
  const abortOnIDBRequestError = (transaction, request, error) => {
    const outcome = failIDBRequest(request, error);
    // A thrown handler aborts even when a listener canceled the error event.
    if (outcome & IDB_EVENT_HANDLER_THREW)
      transaction._abort(fail("A request error handler threw an exception", "AbortError"));
    else if (!(outcome & IDB_EVENT_CANCELED)) transaction._abort(request.error);
  };
  class IDBRequest extends TilefinchIDBEventTarget {
    #state;
    static {
      /* Request results, sources and handlers can refer back to a request.
         A private slot lets ordinary cycle collection release the group. */
      idbRequestState = request => request &&
        (typeof request === "object" || typeof request === "function") &&
        #state in request ? request.#state : undefined;
    }
    constructor(source = null, transaction = null) {
      super();
      this.#state = {
        source,
        transaction,
        readyState: "pending",
        result: undefined,
        error: null,
      };
      this.onsuccess = null;
      this.onerror = null;
    }
    get source() {
      const state = idbRequestState(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.source;
    }
    get transaction() {
      const state = idbRequestState(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.transaction;
    }
    get readyState() {
      const state = idbRequestState(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.readyState;
    }
    get result() {
      const state = idbRequestState(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (state.readyState !== "done")
        throw fail("The request is still pending", "InvalidStateError");
      return state.result;
    }
    get error() {
      const state = idbRequestState(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (state.readyState !== "done")
        throw fail("The request is still pending", "InvalidStateError");
      return state.error;
    }
  }

  class IDBOpenDBRequest extends IDBRequest {
    constructor() {
      super(null, null);
      this.onblocked = null;
      this.onupgradeneeded = null;
    }
  }

  class IDBKeyRange {
    #lower; #upper; #lowerOpen; #upperOpen;
    #lowerValue; #upperValue; #realm;
    /* realm: the IDBKeyRange a static was called on, whose realm reads
       the bounds back (realmReader). */
    constructor(lower, upper, lowerOpen, upperOpen, realm) {
      this.#realm = realm;
      this.#lower = lower === undefined
        ? undefined
        : cloneKey(validateKey(lower));
      this.#upper = upper === undefined
        ? undefined
        : cloneKey(validateKey(upper));
      this.#lowerOpen = !!lowerOpen;
      this.#upperOpen = !!upperOpen;
    }
    get lower() {
      return this.#lowerValue ??= cloneKey(this.#lower, realmReader(this.#realm));
    }
    get upper() {
      return this.#upperValue ??= cloneKey(this.#upper, realmReader(this.#realm));
    }
    get lowerOpen() { return this.#lowerOpen; }
    get upperOpen() { return this.#upperOpen; }
    includes(key) {
      key = validateKey(key);
      if (this.#lower !== undefined) {
        const compared = compareKeys(key, this.#lower);
        if (compared < 0 || (compared === 0 && this.#lowerOpen)) return false;
      }
      if (this.#upper !== undefined) {
        const compared = compareKeys(key, this.#upper);
        if (compared > 0 || (compared === 0 && this.#upperOpen)) return false;
      }
      return true;
    }
    static only(value) {
      value = validateKey(value);
      return new IDBKeyRange(value, value, false, false, this);
    }
    static lowerBound(value, open = false) {
      value = validateKey(value);
      return new IDBKeyRange(value, undefined, open, false, this);
    }
    static upperBound(value, open = false) {
      value = validateKey(value);
      return new IDBKeyRange(undefined, value, false, open, this);
    }
    static bound(lower, upper, lowerOpen = false, upperOpen = false) {
      lower = validateKey(lower);
      upper = validateKey(upper);
      const compared = compareKeys(lower, upper);
      if (compared > 0 || (compared === 0 && (lowerOpen || upperOpen)))
        throw fail("Lower bound exceeds upper bound", "DataError");
      return new IDBKeyRange(lower, upper, lowerOpen, upperOpen, this);
    }
  }
  const keyRangeIncludes = IDBKeyRange.prototype.includes,
    rangeIncludes = (range, key) => intrinsicApply(keyRangeIncludes, range, [key]);
  const snapshotQuery = (query) => {
    if (query === undefined || query === null) return query;
    if (query instanceof IDBKeyRange) return query;
    return cloneKey(validateKey(query));
  };
  const requiredQuery = (query, count) => {
    if (!count) throw new TypeError("A key or key range is required");
    if (query == null) throw fail("Invalid key", "DataError");
    return snapshotQuery(query);
  };
  const boundedCount = (value) => {
    if (value === undefined) return 0;
    const count = Math.trunc(+value);
    if (!Number.isFinite(count) || count < 0 || count > 0xffffffff)
      throw new TypeError("Invalid record count");
    return count;
  };
  const storeFor = (source) => source.objectStore?._store ?? source._store;
  /* Every record mutation bumps the store revision. keyRun spans the
     latest unbroken run of mutations that all touched one key, so a store
     cursor whose own record is the only one changed (update or delete, then
     continue) keeps its snapshot instead of re-sorting every record. */
  const noteStoreMutation = (store, key) => {
    const run = store.keyRun;
    if (key !== undefined && run && run.to === store.revision && !compareKeys(run.key, key))
      run.to++;
    else store.keyRun = key === undefined ? null
      : { key, from: store.revision, to: store.revision + 1 };
    store.revision++;
  };
  const assertSource = (source) => {
    const store = storeFor(source), schema = source._schema;
    if (source.transaction.db._state.stores.get(store.name) !== store ||
        (schema && store.indexes.get(schema.name) !== schema))
      throw fail("The store or index has been deleted", "InvalidStateError");
  };
  /* The four read bindings differ in their result, not in when their
     arguments are converted or their source and transaction are checked.
     Keep the store's direct-key lookup; an index uses its own key order. */
  const readResult = (record, keys, tx) => keys
    ? cloneKey(record.key, readerOf(tx)) : clone(record.value, tx);
  const readQuery = (source, query, optional) => {
    if (typeof query === "string" || (typeof query === "number" && query === query)
        || (optional && query == null)) return query;
    assertSource(source);
    source.transaction._checkRequestState(false);
    return optional ? snapshotQuery(query) : requiredQuery(query, 1);
  };
  const readRecords = (source, query, count, many, keys, indexed) => {
    if (many) count = boundedCount(count);
    else if (!count) throw new TypeError("A key or key range is required");
    const tx = source.transaction;
    /* Numbers and strings are already immutable keys. Other inputs can
       fail conversion or run page code; check state before snapshotting
       them. _request checks again before it publishes a request. */
    query = readQuery(source, query, many);
    return tx._request(source, () => {
      if (!many && !indexed && !(query instanceof IDBKeyRange)) {
        const record = source._store.records.get(keyToken(query));
        return record ? readResult(record, keys, tx) : undefined;
      }
      const entries = source._entries(query, many ? count : 1);
      if (many) return entries.map(entry =>
        readResult(indexed ? entry.record : entry, keys, tx));
      return entries.length
        ? readResult(indexed ? entries[0].record : entries[0], keys, tx) : undefined;
    });
  };
  /* Convert Web IDL arguments first; key snapshotting follows the deleted
     and inactive checks. Enqueue checks state again after author getters. */
  const iterateRecords = (source, query, direction, withValue, indexed) => {
    if (withValue !== undefined) {
      if (typeof direction !== "string") direction = `${direction}`;
      if (direction !== "next" && direction !== "prev" &&
          direction !== "nextunique" && direction !== "prevunique")
        throw new TypeError("Invalid cursor direction");
    }
    query = readQuery(source, query, true);
    if (withValue === undefined) return source.transaction._request(source, () => {
      if (indexed) return source._entries(query).length;
      const records = source._store.records;
      if (query == null) return records.size;
      if (!(query instanceof IDBKeyRange)) return +records.has(keyToken(query));
      let count = 0;
      for (const record of records.values()) if (rangeIncludes(query, record.key)) count++;
      return count;
    });
    return source.transaction._openCursor(source, () => {
      const entries = source._entries(query);
      if (indexed && (direction === "nextunique" || direction === "prevunique")) {
        let kept = 0;
        for (const entry of entries)
          if (!kept || compareKeys(entries[kept - 1].indexKey, entry.indexKey))
            entries[kept++] = entry;
        entries.length = kept;
      }
      if (direction === "prev" || direction === "prevunique") entries.reverse();
      return entries;
    }, withValue, direction);
  };
  const matchesQuery = (key, query) =>
    query === undefined ||
    query === null ||
    (query instanceof IDBKeyRange
      ? rangeIncludes(query, key)
      : compareKeys(key, query) === 0);
  const transactionScopesOverlap = (left, right) => {
    for (const name of left._scope) {
      if (right._scope.has(name)) return true;
    }
    return false;
  };
  const transactionsConflict = (left, right) =>
    (left.mode === "readwrite" || right.mode === "readwrite") &&
    transactionScopesOverlap(left, right);
  const runTransactionQueue = (state) => {
    while (!state.upgrading && state.transactionQueue.length) {
      const transaction = state.transactionQueue[0];
      if (transaction._state === "finished") {
        state.transactionQueue.shift();
        continue;
      }
      let blocked = false;
      for (const active of state.activeTransactions) {
        if (transactionsConflict(transaction, active)) {
          blocked = true;
          break;
        }
      }
      if (blocked) return;
      state.transactionQueue.shift();
      transaction._activate();
    }
  };

  class IDBTransaction extends TilefinchIDBEventTarget {
    constructor(connection, stores, mode = "readonly", upgrade = false) {
      super();
      this.db = connection;
      this.mode = mode;
      this.durability = "default";
      this.error = null;
      this.oncomplete = null;
      this.onerror = null;
      this.onabort = null;
      this._scope = new Set(stores);
      this._storeHandles = null;
      this._upgrade = upgrade;
      this._state = upgrade ? "active" : "pending";
      this._pending = 0;
      this._operations = [];
      this._completionScheduled = false;
      this._commitRequested = false;
      this._acceptingRequests = true;
      this._abortSequence = null;
      this._operationTasks = 0;
      this._abortEventPending = false;
      this._snapshots = new Map();
      this._lockedStores = [];
      this._upgradeSnapshot = null;
      const state = connection._state;
      if (upgrade) {
        if (
          state.upgrading ||
          state.activeTransactions.size ||
          state.transactionQueue.length
        )
          throw fail(
            "A conflicting transaction is active",
            "InvalidStateError",
          );
        state.upgrading = this;
        this._upgradeSnapshot = {
          version: state.version,
          stores: new Map(
            [...state.stores].map(([name, store]) => [name, copyStore(store)]),
          ),
        };
      } else {
        if (state.transactionQueue.length >= TRANSACTION_QUEUE_LIMIT)
          throw fail("Transaction queue quota exceeded", "QuotaExceededError");
        state.transactionQueue.push(this);
      }
      stats.transactions++;
      /* The creation task remains active through its microtask checkpoint.
         A checkpoint continuation closes that window before any later task;
         a timer cannot provide this ordering when it was queued earlier. */
      if (!scheduleActivityClose(this)) {
        stats.transactions--;
        if (upgrade && state.upgrading === this) state.upgrading = null;
        else {
          const at = state.transactionQueue.indexOf(this);
          if (at >= 0) state.transactionQueue.splice(at, 1);
        }
        throw fail("Database task quota exceeded", "QuotaExceededError");
      }
      if (upgrade) queueMicrotask(() => this._maybeComplete());
      else runTransactionQueue(state);
    }
    get objectStoreNames() {
      return this._objectStoreNames ??= nameList(this._upgrade
        ? this.db._state.stores.keys() : this._scope);
    }
    _storeHandle(store) {
      const handles = this._storeHandles ??= new WeakMap();
      let handle = handles.get(store);
      if (!handle) {
        handle = new IDBObjectStore(this, store);
        handles.set(store, handle);
      }
      return handle;
    }
    _scheduleOperation(operation, sequence = null) {
      const accepted = scheduleDatabaseTask(() => {
        try {
          if (sequence && sequence.aborting) {
            this._drainAbortSequence(sequence, false);
            return;
          }
          if (this._state !== "finished") {
            this._acceptingRequests = true;
            /* Close after promise jobs of the request, before the next task. */
            if (!scheduleActivityClose(this))
              this._abort(fail(
                "Database task quota exceeded", "QuotaExceededError"));
          }
          operation();
        } finally {
          this._operationTasks--;
          this._queueAbortEvent();
        }
      });
      if (accepted) this._operationTasks++;
      return accepted;
    }
    _activate() {
      if (this._state !== "pending") return;
      this._state = "active";
      const state = this.db._state;
      state.activeTransactions.add(this);
      if (this.mode === "readwrite") {
        for (const name of this._scope) {
          state.writeLocks.set(name, this);
          this._lockedStores.push(name);
        }
      }
      const operations = this._operations;
      this._operations = [];
      state.pendingRequests -= operations.length;
      if (this._commitRequested) this._state = "committing";
      const sequence = {
        operations,
        cursor: 0,
        aborting: false,
        continuationScheduled: false,
        finished: false,
      };
      for (let at = 0; at < operations.length; at++) {
        if (!this._scheduleOperation(operations[at], sequence)) {
          /* Keep one ordered sequence for accepted and refused work. Already
             admitted callbacks become drain continuations instead of racing
             the unscheduled tail or letting abort precede request errors. */
          sequence.aborting = true;
          this._abortSequence = sequence;
          this._abort(fail(
            "Database task quota exceeded", "QuotaExceededError"), sequence);
          break;
        }
      }
      this._maybeComplete();
    }
    _drainAbortSequence(sequence, continuation) {
      if (!sequence || sequence.finished || !sequence.aborting) return;
      if (continuation) sequence.continuationScheduled = false;
      if (sequence.cursor >= sequence.operations.length) {
        this._finishAbortSequence(sequence);
        return;
      }
      /* Reserve the next bounded task before invoking an author error
         handler. The currently executing timer has already vacated one slot,
         so this cannot be starved by timers queued from that handler. The
         final continuation dispatches transaction abort in its own task. */
      if (
        sequence.cursor < sequence.operations.length &&
        !sequence.continuationScheduled
      ) {
        sequence.continuationScheduled = scheduleDatabaseTask(() =>
          this._drainAbortSequence(sequence, true));
        if (!sequence.continuationScheduled && !scheduleActivityClose(this)) {
          while (sequence.cursor < sequence.operations.length)
            sequence.operations[sequence.cursor++]();
          this._finishAbortSequence(sequence);
          return;
        }
      }
      const operation = sequence.operations[sequence.cursor++];
      operation();
    }
    _finishAbortSequence(sequence) {
      if (!sequence || sequence.finished) return;
      sequence.finished = true;
      sequence.operations = [];
      if (this._abortSequence === sequence) this._abortSequence = null;
      this._releaseLocks();
      this._dispatch("abort");
    }
    _queueAbortEvent() {
      if (!this._abortEventPending || this._operationTasks) return;
      this._abortEventPending = false;
      const dispatch = () => {
        this._releaseLocks();
        this._dispatch("abort");
      };
      if (!scheduleDatabaseTask(dispatch)) dispatch();
    }
    objectStore(name) {
      name = boundedName(name, 128, "Object store name");
      if (this._state === "finished" || this._state === "committing"
          || this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (!this._upgrade && !this._scope.has(name))
        throw fail(
          "Object store is outside transaction scope",
          "NotFoundError",
        );
      const store = this.db._state.stores.get(name);
      if (!store) throw fail("Object store does not exist", "NotFoundError");
      return this._storeHandle(store);
    }
    abort() {
      if (this._state === "finished" || this._state === "committing" ||
          this._commitRequested)
        throw fail("Transaction cannot be aborted", "InvalidStateError");
      this._abort(null);
    }
    commit() {
      if ((this._state !== "active" && this._state !== "pending") ||
          this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "InvalidStateError");
      this._commitRequested = true;
      if (this._state === "active") this._state = "committing";
      this._maybeComplete();
    }
    _snapshot(store) {
      if (
        this.mode === "readonly" ||
        this._upgradeSnapshot ||
        this._snapshots.has(store)
      )
        return;
      this._snapshots.set(store, {
        records: new Map(store.records),
        bytes: store.bytes,
        nextKey: store.nextKey,
      });
    }
    _checkRequestState(write) {
      if (this._state === "finished" || this._state === "committing"
          || this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (write && this.mode === "readonly")
        throw fail("Transaction is read only", "ReadOnlyError");
    }
    _requestAbortedDuringClone(source) {
      if (this._pending >= REQUEST_LIMIT)
        throw fail("Transaction request quota exceeded", "QuotaExceededError");
      const request = new IDBRequest(source, this);
      const deliver = () => {
        failIDBRequest(request, fail("Transaction aborted", "AbortError"));
        this._pending--;
      };
      /* A pending transaction may already have a FIFO drain for its older
         requests. Otherwise join the ordinary task queue before the abort
         event; this path never schedules the write itself or revives locks. */
      if (this._abortSequence) this._abortSequence.operations.push(deliver);
      else if (!this._scheduleOperation(deliver))
        throw fail("Database task quota exceeded", "QuotaExceededError");
      this._pending++;
      stats.requests++;
      return request;
    }
    _request(source, operation, write = false, cursor = null) {
      assertSource(source);
      this._checkRequestState(write);
      if (this._pending >= REQUEST_LIMIT)
        throw fail("Transaction request quota exceeded", "QuotaExceededError");
      const request = new IDBRequest(cursor || source, this);
      if (
        this._state === "pending" &&
        this.db._state.pendingRequests >= PENDING_REQUEST_LIMIT
      )
        throw fail(
          "Pending request queue quota exceeded",
          "QuotaExceededError",
        );
      this._pending++;
      stats.requests++;
      const run = () => {
        if (this._state !== "active" && this._state !== "committing") {
          failIDBRequest(request, fail("Transaction aborted", "AbortError"));
          this._pending--;
          return;
        }
        try {
          if (succeedIDBRequest(request, operation())) {
            this._pending--;
            this._abort(fail(
              "A request success handler threw an exception",
              "AbortError",
            ));
            return;
          }
        } catch (error) {
          abortOnIDBRequestError(this, request, error);
        }
        this._pending--;
        this._maybeComplete();
      };
      if (this._state === "pending") {
        this.db._state.pendingRequests++;
        this._operations.push(run);
      } else if (!this._scheduleOperation(run)) {
        this._pending--;
        stats.requests--;
        throw fail("Database task quota exceeded", "QuotaExceededError");
      }
      return request;
    }
    _openCursor(source, entriesFactory, withValue, direction) {
      assertSource(source);
      this._checkRequestState(false);
      if (this._pending >= REQUEST_LIMIT)
        throw fail("Transaction request quota exceeded", "QuotaExceededError");
      const request = new IDBRequest(source, this);
      if (
        this._state === "pending" &&
        this.db._state.pendingRequests >= PENDING_REQUEST_LIMIT
      )
        throw fail(
          "Pending request queue quota exceeded",
          "QuotaExceededError",
        );
      this._pending++;
      stats.requests++;
      const run = () => {
        if (this._state !== "active" && this._state !== "committing") {
          failIDBRequest(request, fail("Transaction aborted", "AbortError"));
          this._finishCursor();
          return;
        }
        let entries;
        try {
          entries = entriesFactory();
        } catch (error) {
          abortOnIDBRequestError(this, request, error);
          this._finishCursor();
          return;
        }
        if (!entries.length) {
          if (succeedIDBRequest(request, null))
            this._abort(fail("A cursor success handler threw an exception", "AbortError"));
          this._finishCursor();
          return;
        }
        const Cursor = withValue ? IDBCursorWithValue : IDBCursor;
        const cursor = new Cursor(
          request,
          source,
          entries,
          withValue,
          direction,
          this,
          entriesFactory,
        );
        cursor._emit();
      };
      if (this._state === "pending") {
        this.db._state.pendingRequests++;
        this._operations.push(run);
      } else if (!this._scheduleOperation(run)) {
        this._pending--;
        stats.requests--;
        throw fail("Database task quota exceeded", "QuotaExceededError");
      }
      return request;
    }
    _finishCursor() {
      if (this._pending > 0) this._pending--;
      this._maybeComplete();
    }
    _maybeComplete() {
      if (
        (this._state !== "active" && this._state !== "committing") ||
        this._pending !== 0 ||
        (this._acceptingRequests && !this._commitRequested) ||
        this._completionScheduled
      )
        return;
      this._completionScheduled = true;
      if (!scheduleDatabaseTask(() => {
        this._completionScheduled = false;
        if ((this._state !== "active" && this._state !== "committing")
            || this._pending !== 0
            || (this._acceptingRequests && !this._commitRequested))
          return;
        this._state = "committing";
        this._state = "finished";
        this._snapshots.clear();
        if (this._upgrade) void this.objectStoreNames;
        this._upgradeSnapshot = null;
        this._releaseLocks();
        this._dispatch("complete");
      })) {
        this._completionScheduled = false;
        this._abort(fail(
          "Database task quota exceeded", "QuotaExceededError"));
      }
    }
    _abort(error, sequence = null) {
      if (this._state === "finished") return;
      const wasPending = this._state === "pending";
      this._state = "finished";
      this.error =
        error === null || error instanceof DOMException
          ? error
          : fail(String(error), "AbortError");
      if (this._upgradeSnapshot) {
        this._rolledBackSchema = true;
        const before = stateUsage(this.db._state),
          restored = { records: 0, bytes: 0 };
        for (const store of this._upgradeSnapshot.stores.values()) {
          restored.records += store.records.size;
          restored.bytes += store.bytes;
        }
        stats.records += restored.records - before.records;
        stats.bytes += restored.bytes - before.bytes;
        this.db._state.stores = this._upgradeSnapshot.stores;
        this.db._state.version = this._upgradeSnapshot.version;
        this.db._refreshStoreNames();
        void this.objectStoreNames;
      } else {
        for (const [store, snapshot] of this._snapshots) {
          stats.records += snapshot.records.size - store.records.size;
          stats.bytes += snapshot.bytes - store.bytes;
          store.records = snapshot.records;
          store.bytes = snapshot.bytes;
          store.nextKey = snapshot.nextKey;
          store.revision++;
        }
      }
      this._snapshots.clear();
      this._upgradeSnapshot = null;
      if (sequence) return;
      if (this._operations.length) {
        const operations = this._operations;
        this._operations = [];
        if (wasPending) this.db._state.pendingRequests -= operations.length;
        /* Reuse the ordered drain: a refused timer must never deliver a
           later request ahead of one that was already admitted. */
        const pending = {operations, cursor: 0, aborting: true,
          continuationScheduled: false, finished: false};
        this._abortSequence = pending;
        if (!scheduleDatabaseTask(() => this._drainAbortSequence(pending, false))
            && !scheduleActivityClose(this)) {
          for (const operation of operations) operation();
          this._finishAbortSequence(pending);
        }
        return;
      }
      this._abortEventPending = true;
      this._queueAbortEvent();
    }
    _releaseLocks() {
      const state = this.db._state;
      if (state.upgrading === this) state.upgrading = null;
      state.activeTransactions.delete(this);
      for (const name of this._lockedStores) {
        if (state.writeLocks.get(name) === this) state.writeLocks.delete(name);
      }
      this._lockedStores = [];
      finishIDBClose(this.db);
      queueMicrotask(() => runTransactionQueue(state));
    }
  }

  class IDBObjectStore {
    #name; #initialName; #keyPath; #indexNames; #initialIndexNames;
    constructor(transaction, store) {
      this.transaction = transaction;
      this._store = store;
      this.autoIncrement = store.autoIncrement;
      this.#name = this.#initialName = store.name;
      this.#indexNames = nameList(store.indexes.keys());
      if (transaction._upgrade) this.#initialIndexNames = this.#indexNames;
    }
    get name() { return this.transaction._rolledBackSchema ? this.#initialName : this.#name; }
    set name(value) {
      const tx = this.transaction, store = this._store, db = tx.db;
      assertSource(this);
      if (!tx._upgrade)
        throw fail("Stores can only be renamed during upgrade", "InvalidStateError");
      if (tx._state !== "active" || tx._commitRequested || !tx._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      const name = boundedName(value, 128, "Object store name");
      if (name === store.name) return;
      const stores = db._state.stores;
      if (stores.has(name)) throw fail("Object store already exists", "ConstraintError");
      stores.set(name, store);
      stores.delete(store.name);
      store.name = this.#name = name;
      db._refreshStoreNames();
    }
    get keyPath() {
      return this.#keyPath === undefined
        ? this.#keyPath = copyKeyPath(this._store.keyPath) : this.#keyPath;
    }
    get indexNames() {
      return this.transaction._rolledBackSchema
        ? this.#initialIndexNames : this.#indexNames;
    }
    _refreshIndexNames() {
      this.#indexNames = nameList(this._store.indexes.keys());
    }
    _indexHandle(schema) {
      const handles = this._indexHandles ??= new WeakMap();
      let handle = handles.get(schema);
      if (!handle) {
        handle = new IDBIndex(this.transaction, this, schema);
        handles.set(schema, handle);
      }
      return handle;
    }
    _entries(query, count = 0) {
      const entries = [...this._store.records.values()]
        .filter((record) => matchesQuery(record.key, query))
        .sort((left, right) => compareKeys(left.key, right.key));
      const maximum =
        Number(count) > 0 ? Math.floor(Number(count)) : entries.length;
      return entries.slice(0, maximum);
    }
    _key(value, supplied) {
      let key = supplied;
      const path = this._store.keyPath;
      if (key === undefined && path !== null) key = keyAtPath(value, path);
      const generated = key === undefined && this.autoIncrement;
      if (generated) {
        if (!Number.isFinite(this._store.nextKey))
          throw fail("The key generator is exhausted", "ConstraintError");
        key = this._store.nextKey;
        if (path !== null) injectKeyAtPath(value, path, key);
      }
      if (key === undefined) throw fail("A key is required", "DataError");
      return validateKey(key);
    }
    _storeValue(value, supplied, overwrite) {
      this.transaction._snapshot(this._store);
      const key = this._key(value, supplied);
      const token = keyToken(key);
      const previous = this._store.records.get(token);
      if (!overwrite && previous)
        throw fail("Key already exists", "ConstraintError");
      assertUniqueIndexes(this._store, value, token);
      const bytes = serializedSize(key, value);
      const delta = bytes - (previous ? previous.bytes : 0);
      if (
        bytes > RECORD_BYTE_LIMIT ||
        (!previous && stats.records >= RECORD_LIMIT) ||
        delta > BYTE_LIMIT - stats.bytes
      ) {
        stats.quotaErrors++;
        throw fail("IndexedDB storage quota exceeded", "QuotaExceededError");
      }
      this._store.records.set(token, {
        key: cloneKey(key),
        value,
        bytes,
      });
      noteStoreMutation(this._store, key);
      if (
        this.autoIncrement && typeof key === "number" &&
        Number.isFinite(key) && key >= this._store.nextKey
      ) {
        this._store.nextKey = key >= 9007199254740992
          ? Infinity
          : Math.floor(key) + 1;
      }
      this._store.bytes += delta;
      /* Work accepted before an in-task schema deletion can still finish.
         The detached store is already absent from the database's totals. */
      if (!this._store.deleted) {
        stats.bytes += delta;
        if (!previous) stats.records++;
        stats.peakBytes = Math.max(stats.peakBytes, stats.bytes);
      }
      return cloneKey(key, readerOf(this.transaction));
    }
    _preparedWrite(value, key, overwrite, cursor = null) {
      assertSource(this);
      const tx = this.transaction;
      tx._checkRequestState(true);
      if (!cursor && this._store.keyPath !== null && key !== undefined)
        throw fail("Inline keys cannot be supplied separately", "DataError");
      const copiedKey = key === undefined ? undefined : cloneKey(validateKey(key));
      /* Enforce the backing-buffer cap while cloning so accessors are only
         visited once and over-quota bytes need not be copied first. Author
         getters run while this transaction is inactive, and can abort it. */
      let storageCopy;
      tx._acceptingRequests = false;
      /* Hold the transaction's abort notification until a successful clone
         that itself aborted has queued its request's async error. */
      tx._operationTasks++;
      try {
        storageCopy = cloneForStorage
          ? cloneForStorage(value, RECORD_BYTE_LIMIT - 128)
          : { __proto__: null, value: intrinsicApply(
              trustedStructuredClone, globalThis, [value, trustedCloneIntrinsics]) };
        if (tx._state === "finished")
          return tx._requestAbortedDuringClone(cursor || this);
      } finally {
        tx._operationTasks--;
        tx._queueAbortEvent();
        if ((tx._state === "active" || tx._state === "pending") &&
            !tx._commitRequested)
          tx._acceptingRequests = true;
      }
      tx._checkRequestState(true);
      if (storageCopy === undefined)
        return this.transaction._request(
          cursor ? cursor.source : this,
          () => {
            stats.quotaErrors++;
            throw fail("IndexedDB storage quota exceeded", "QuotaExceededError");
          },
          true,
          cursor,
        );
      const copied = storageCopy.value;
      if (!cursor && Array.isArray(this._store.keyPath))
        validateKey(keyAtPath(copied, this._store.keyPath));
      if (cursor && this._store.keyPath !== null &&
          compareKeys(validateKey(keyAtPath(copied, this._store.keyPath)), copiedKey) !== 0)
        throw fail("The cursor key cannot change", "DataError");
      return this.transaction._request(
        cursor ? cursor.source : this,
        () => this._storeValue(copied, copiedKey, overwrite),
        true,
        cursor,
      );
    }
    put(value, key) {
      if (!arguments.length) throw new TypeError("A value is required");
      return this._preparedWrite(value, key, true);
    }
    add(value, key) {
      if (!arguments.length) throw new TypeError("A value is required");
      return this._preparedWrite(value, key, false);
    }
    #read(query, count, many, keys) {
      return readRecords(this, query, count, many, keys, false);
    }
    get(query) {
      return this.#read(query, arguments.length, false, false);
    }
    getKey(query) {
      return this.#read(query, arguments.length, false, true);
    }
    getAll(query = undefined, count) {
      return this.#read(query, count, true, false);
    }
    getAllKeys(query = undefined, count) {
      return this.#read(query, count, true, true);
    }
    count(query = undefined) {
      return this.#iterate(query);
    }
    delete(query) {
      if (!arguments.length) throw new TypeError("A key or key range is required");
      assertSource(this);
      this.transaction._checkRequestState(true);
      query = requiredQuery(query, arguments.length);
      return this._deleteRecords(query);
    }
    _deleteRecords(query, cursor = null) {
      return this.transaction._request(
        cursor ? cursor.source : this,
        () => {
          this.transaction._snapshot(this._store);
          const records =
            query instanceof IDBKeyRange
              ? this._entries(query)
              : [this._store.records.get(keyToken(validateKey(query)))].filter(
                  Boolean,
                );
          for (const record of records) {
            this._store.records.delete(keyToken(record.key));
            this._store.bytes -= record.bytes;
            if (!this._store.deleted) {
              stats.bytes -= record.bytes;
              stats.records--;
            }
          }
          if (records.length)
            noteStoreMutation(this._store, query instanceof IDBKeyRange ? undefined
              : records[0].key);
          return undefined;
        },
        true,
        cursor,
      );
    }
    clear() {
      return this.transaction._request(
        this,
        () => {
          this.transaction._snapshot(this._store);
          if (!this._store.deleted) {
            stats.records -= this._store.records.size;
            stats.bytes -= this._store.bytes;
          }
          if (this._store.records.size) noteStoreMutation(this._store);
          this._store.records.clear();
          this._store.bytes = 0;
          return undefined;
        },
        true,
      );
    }
    index(name) {
      assertSource(this);
      name = boundedName(name, 128, "Index name");
      const schema = this._store.indexes.get(name);
      if (!schema) throw fail("Index does not exist", "NotFoundError");
      return this._indexHandle(schema);
    }
    createIndex(name, keyPath, options = {}) {
      assertSource(this);
      if (!this.transaction._upgrade)
        throw fail(
          "Indexes can only be created during upgrade",
          "InvalidStateError",
        );
      name = boundedName(name, 128, "Index name");
      keyPath = boundedKeyPath(keyPath);
      if (this._store.indexes.has(name))
        throw fail("Index already exists", "ConstraintError");
      if (this._store.indexes.size >= INDEX_LIMIT)
        throw fail("Index quota exceeded", "QuotaExceededError");
      const tx = this.transaction, store = this._store;
      if (tx._state !== "active" || tx._commitRequested || !tx._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      const schema = {
        name,
        keyPath,
        multiEntry: !!options.multiEntry,
        unique: !!options.unique,
      };
      if (schema.unique) {
        /* Index population follows previously queued writes. On failure the
           version change rolls back, even if the upgrade handler returned. */
        schema.building = true;
        if (tx._pending >= REQUEST_LIMIT || !tx._scheduleOperation(() => {
          tx._pending--;
          if (tx._state === "finished") return;
          if (tx.db._state.stores.get(store.name) === store &&
              store.indexes.get(schema.name) === schema) {
            try {
              const seen = new Set();
              for (const record of store.records.values())
                for (const key of indexKeysForValue(record.value, schema)) {
                  const token = keyToken(key);
                  if (seen.has(token)) throw fail("Duplicate index key", "ConstraintError");
                  seen.add(token);
                }
            } catch (error) {
              tx._abort(fail("Could not populate the index",
                error?.name === "ConstraintError" ? "ConstraintError" : "AbortError"));
              return;
            }
            schema.building = false;
          }
          tx._maybeComplete();
        }))
          throw fail("Database task quota exceeded", "QuotaExceededError");
        tx._pending++;
      }
      this._store.indexes.set(name, schema);
      this._refreshIndexNames();
      return this._indexHandle(schema);
    }
    deleteIndex(name) {
      assertSource(this);
      const tx = this.transaction;
      if (!tx._upgrade)
        throw fail(
          "Indexes can only be deleted during upgrade",
          "InvalidStateError",
        );
      if (tx._state !== "active" || tx._commitRequested || !tx._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      name = boundedName(name, 128, "Index name");
      if (!this._store.indexes.delete(name))
        throw fail("Index does not exist", "NotFoundError");
      this._refreshIndexNames();
    }
    openCursor(query = undefined, direction = "next") {
      return this.#iterate(query, direction, true);
    }
    openKeyCursor(query = undefined, direction = "next") {
      return this.#iterate(query, direction, false);
    }
    #iterate(query, direction, withValue) {
      return iterateRecords(this, query, direction, withValue, false);
    }
  }

  class IDBIndex {
    #name; #initialName; #keyPath;
    constructor(transaction, objectStore, schema) {
      this.transaction = transaction;
      this.objectStore = objectStore;
      this._schema = schema;
      this.#name = this.#initialName = schema.name;
      this.multiEntry = schema.multiEntry;
      this.unique = schema.unique;
    }
    get keyPath() { return this.#keyPath ??= copyKeyPath(this._schema.keyPath); }
    get name() {
      return this.transaction._rolledBackSchema ? this.#initialName : this.#name;
    }
    set name(value) {
      const tx = this.transaction, schema = this._schema,
        indexes = this.objectStore._store.indexes;
      assertSource(this);
      if (!tx._upgrade)
        throw fail("Indexes can only be renamed during upgrade", "InvalidStateError");
      if (tx._state !== "active" || tx._commitRequested || !tx._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      const name = boundedName(value, 128, "Index name");
      if (schema.name === name) return;
      if (indexes.has(name)) throw fail("Index already exists", "ConstraintError");
      indexes.set(name, schema);
      indexes.delete(schema.name);
      schema.name = this.#name = name;
      this.objectStore._refreshIndexNames();
    }
    _entries(query, count = 0) {
      const output = [];
      for (const record of this.objectStore._store.records.values()) {
        for (const key of indexKeysForValue(record.value, this._schema)) {
          if (matchesQuery(key, query))
            output.push({ indexKey: key, record });
        }
      }
      output.sort(
        (left, right) =>
          compareKeys(left.indexKey, right.indexKey) ||
          compareKeys(left.record.key, right.record.key),
      );
      const maximum =
        Number(count) > 0 ? Math.floor(Number(count)) : output.length;
      return output.slice(0, maximum);
    }
    #read(query, count, many, keys) {
      return readRecords(this, query, count, many, keys, true);
    }
    get(query) {
      return this.#read(query, arguments.length, false, false);
    }
    getKey(query) {
      return this.#read(query, arguments.length, false, true);
    }
    getAll(query = undefined, count) {
      return this.#read(query, count, true, false);
    }
    getAllKeys(query = undefined, count) {
      return this.#read(query, count, true, true);
    }
    count(query = undefined) {
      return this.#iterate(query);
    }
    openCursor(query = undefined, direction = "next") {
      return this.#iterate(query, direction, true);
    }
    openKeyCursor(query = undefined, direction = "next") {
      return this.#iterate(query, direction, false);
    }
    #iterate(query, direction, withValue) {
      return iterateRecords(this, query, direction, withValue, true);
    }
  }

  class IDBCursor {
    constructor(request, source, entries, withValue, direction, transaction,
                entriesFactory) {
      this.request = request;
      this.source = source;
      this.direction = direction;
      this._transaction = transaction;
      this._entries = entries;
      this._entriesFactory = entriesFactory;
      this._storeRevision = storeFor(source).revision;
      this._withValue = withValue;
      this._position = 0;
      this._continued = false;
      this._finished = false;
      this._generation = 0;
      this._sync();
    }
    _sync() {
      const entry = this._entries[this._position];
      const record = entry && (entry.record || entry);
      const realm = readerOf(this._transaction);
      this.key = entry
        ? cloneKey(entry.indexKey === undefined ? record.key : entry.indexKey,
          realm)
        : undefined;
      this.primaryKey = record ? cloneKey(record.key, realm) : undefined;
      if (this._withValue)
        this.value = record ? clone(record.value, this._transaction) : undefined;
    }
    _finish() {
      if (this._finished) return;
      this._finished = true;
      this._transaction._finishCursor();
    }
    _emit() {
      if (this._finished) return;
      this._continued = false;
      resetIDBRequest(this.request);
      if (this._position >= this._entries.length) {
        if (succeedIDBRequest(this.request, null))
          this._transaction._abort(fail(
            "A cursor success handler threw an exception",
            "AbortError",
          ));
        this._finish();
        return;
      }
      this._sync();
      if (succeedIDBRequest(this.request, this)) {
        this._transaction._abort(fail(
          "A cursor success handler threw an exception",
          "AbortError",
        ));
        this._finish();
        return;
      }
      const generation = ++this._generation;
      if (!scheduleDatabaseTask(() => {
        if (generation === this._generation && !this._continued) this._finish();
      })) this._finish();
    }
    _checkMove() {
      this._transaction._checkRequestState(false);
      assertSource(this.source);
      if (this._continued || !this._entries[this._position])
        throw fail("Cursor is not positioned on a record", "InvalidStateError");
    }
    _move(count, target, primary) {
      this._checkMove();
      const tx = this._transaction, reacquire = this._finished;
      if (reacquire && tx._pending >= REQUEST_LIMIT)
        throw fail("Transaction request quota exceeded", "QuotaExceededError");
      const revision = this._storeRevision, store = storeFor(this.source),
        old = this._entries[this._position], record = old.record || old,
        indexed = old.record !== undefined,
        unique = indexed && this.direction.endsWith("unique"),
        order = this.direction.startsWith("prev") ? -1 : 1,
        key = target === undefined ? old.indexKey ?? record.key : target,
        tie = target === undefined && indexed && !unique ? record.key : primary;
      /* Compare against an internal key, not the mutable public cursor.key. */
      const compare = entry => order * (
        compareKeys(entry.indexKey ?? (entry.record || entry).key, key) ||
        (tie === undefined ? 0 : compareKeys(entry.record.key, tie)));
      const strictlyAfter = target === undefined;
      this._continued = true;
      if (!tx._scheduleOperation(() => {
        if (this._transaction._state === "finished") {
          failIDBRequest(this.request, fail("Transaction aborted", "AbortError"));
          this._finish();
          return;
        }
        try {
          assertSource(this.source);
          const run = store.keyRun;
          if (revision !== store.revision && !indexed && run && run.from <= revision &&
              run.to === store.revision && !compareKeys(run.key, record.key))
            this._storeRevision = store.revision;
          if (this._storeRevision === store.revision && strictlyAfter) {
            this._position += count;
          } else {
            if (this._storeRevision !== store.revision) {
              this._entries = this._entriesFactory();
              this._storeRevision = store.revision;
            }
            let lo = 0, hi = this._entries.length;
            while (lo < hi) {
              const mid = (lo + hi) >>> 1, compared = compare(this._entries[mid]);
              if (compared < 0 || (strictlyAfter && !compared)) lo = mid + 1;
              else hi = mid;
            }
            this._position = lo + count - 1;
          }
        } catch (error) {
          resetIDBRequest(this.request);
          abortOnIDBRequestError(this._transaction, this.request, error);
          this._finish();
          return;
        }
        this._emit();
      })) {
        this._continued = false;
        throw fail("Database task quota exceeded", "QuotaExceededError");
      }
      // A later request can reactivate a transaction after this positioned
      // cursor released its reservation. Hold it again for the new movement.
      if (reacquire) {
        this._finished = false;
        tx._pending++;
      }
      resetIDBRequest(this.request);
    }
    continue(key) {
      if (key === undefined) return this._move(1);
      this._checkMove();
      key = cloneKey(validateKey(key));
      const order = this.direction.startsWith("prev") ? -1 : 1,
        compare = entry => order *
          compareKeys(entry.indexKey ?? (entry.record || entry).key, key);
      if (compare(this._entries[this._position]) >= 0)
        throw fail("The key must advance the cursor", "DataError");
      this._move(1, key);
    }
    continuePrimaryKey(key, primaryKey) {
      if (arguments.length < 2) throw new TypeError("Two keys are required");
      this._checkMove();
      const reverse = this.direction === "prev";
      if (!(this.source instanceof IDBIndex) ||
          (!reverse && this.direction !== "next"))
        throw fail("An index cursor with next or prev is required",
                   "InvalidAccessError");
      key = cloneKey(validateKey(key));
      primaryKey = cloneKey(validateKey(primaryKey));
      const order = reverse ? -1 : 1;
      const compare = entry => order * (
        compareKeys(entry.indexKey, key) ||
        compareKeys(entry.record.key, primaryKey));
      if (compare(this._entries[this._position]) >= 0)
        throw fail("The key pair must advance the cursor", "DataError");
      this._move(1, key, primaryKey);
    }
    advance(count) {
      count = boundedCount(count);
      if (!count) throw new TypeError("Count must be positive");
      this._move(count);
    }
    _writeKey() {
      this._transaction._checkRequestState(true);
      assertSource(this.source);
      // Retiring the cursor's keep-alive task does not clear its position.
      // Another request can still reactivate the same transaction.
      const entry = this._entries[this._position];
      if (this._continued || !entry)
        throw fail("Cursor is not positioned on a record", "InvalidStateError");
      if (!this._withValue) throw fail("Key-only cursor", "InvalidStateError");
      return (entry.record || entry).key;
    }
    update(value) {
      if (!arguments.length) throw new TypeError("A value is required");
      const key = this._writeKey();
      const store =
        this.source instanceof IDBIndex ? this.source.objectStore : this.source;
      return store._preparedWrite(value, key, true, this);
    }
    delete() {
      const key = this._writeKey();
      const store =
        this.source instanceof IDBIndex ? this.source.objectStore : this.source;
      return store._deleteRecords(key, this);
    }
  }
  class IDBCursorWithValue extends IDBCursor {}

  const resumeBlockedConnection = (state) => {
    if (state.connections.size || !state.blockedConnection) return;
    const resume = state.blockedConnection;
    state.blockedConnection = null;
    resume();
  };

  const finishIDBClose = (database) => {
    if (!database._closed) return;
    const state = database._state;
    if (state.upgrading && state.upgrading.db === database) return;
    for (const transaction of state.activeTransactions)
      if (transaction.db === database) return;
    for (const transaction of state.transactionQueue)
      if (transaction.db === database && transaction._state !== "finished")
        return;
    /* close() refuses new work immediately, but accepted transactions keep
       the connection blocking an upgrade until their ownership is retired. */
    state.connections.delete(database);
    resumeBlockedConnection(state);
  };

  /* Schema changes need the upgrade transaction (InvalidStateError), and
     an active one (TransactionInactiveError). */
  const assertUpgradeActive = (tx, message) => {
    if (!tx || tx._state === "finished") throw fail(message, "InvalidStateError");
    if (tx._state !== "active" || tx._commitRequested || !tx._acceptingRequests)
      throw fail("Transaction is inactive", "TransactionInactiveError");
  };
  class IDBDatabase extends TilefinchIDBEventTarget {
    #reader;
    static { idbReader = (database) => database.#reader; }
    constructor(state, reader) {
      super();
      this.#reader = reader;
      this._state = state;
      this.name = state.name;
      this.version = state.version;
      this.onabort = null;
      this.onclose = null;
      this.onerror = null;
      this.onversionchange = null;
      this._closed = false;
      this._upgradeTransaction = null;
      state.connections.add(this);
    }
    get objectStoreNames() {
      return this._objectStoreNames ??= nameList(this._state.stores.keys());
    }
    _refreshStoreNames() {
      this._objectStoreNames = null;
      if (this._upgradeTransaction) this._upgradeTransaction._objectStoreNames = null;
    }
    createObjectStore(name, options = {}) {
      assertUpgradeActive(this._upgradeTransaction, "Object stores can only be created during upgrade");
      name = boundedName(name, 128, "Object store name");
      if (this._state.stores.has(name))
        throw fail("Object store already exists", "ConstraintError");
      if (this._state.stores.size >= STORE_LIMIT)
        throw fail("Object store quota exceeded", "QuotaExceededError");
      const store = {
        name,
        keyPath:
          options.keyPath === undefined
            ? null
            : boundedKeyPath(options.keyPath),
        autoIncrement: !!options.autoIncrement,
        nextKey: 1,
        indexes: new Map(),
        records: new Map(),
        bytes: 0,
        revision: 0,
      };
      this._state.stores.set(name, store);
      this._refreshStoreNames();
      return this._upgradeTransaction._storeHandle(store);
    }
    deleteObjectStore(name) {
      assertUpgradeActive(this._upgradeTransaction, "Object stores can only be deleted during upgrade");
      name = boundedName(name, 128, "Object store name");
      const store = this._state.stores.get(name);
      if (!store) throw fail("Object store does not exist", "NotFoundError");
      stats.records -= store.records.size;
      stats.bytes -= store.bytes;
      store.deleted = true;
      this._state.stores.delete(name);
      this._refreshStoreNames();
    }
    transaction(storeNames, mode = "readonly") {
      if (this._closed)
        throw fail("Database connection is closed", "InvalidStateError");
      const stores =
        typeof storeNames === "string"
          ? [storeNames]
          : Array.from(storeNames || []);
      const boundedStores = stores.map((name) =>
        boundedName(name, 128, "Object store name"),
      );
      if (
        !boundedStores.length ||
        boundedStores.some((name) => !this._state.stores.has(name))
      )
        throw fail("Object store does not exist", "NotFoundError");
      const normalizedMode = String(mode);
      if (normalizedMode !== "readonly" && normalizedMode !== "readwrite")
        throw new TypeError("Invalid transaction mode");
      return new IDBTransaction(this, boundedStores, normalizedMode);
    }
    close() {
      if (this._closed) return;
      void this.objectStoreNames;
      this._closed = true;
      finishIDBClose(this);
    }
  }
  /* One FIFO for both open and delete, independent of the database state
     (which delete can retire while a later open is already queued). */
  const connectionTaskError = () =>
    fail("Database task quota exceeded", "QuotaExceededError");
  const enqueueConnection = (name, request, run) => {
    if (!scheduleDatabaseTask(() => {
      let queue = connectionQueues.get(name);
      const perform = () => run(() => {
        for (let i = 0; i < OPEN_QUEUE_LIMIT && queue.length; i++) {
          const next = queue.shift();
          if (scheduleDatabaseTask(next.perform)) return;
          failIDBRequest(next.request, connectionTaskError());
        }
        if (queue.length === 0) connectionQueues.delete(name);
      });
      if (queue) {
        if (queue.length >= OPEN_QUEUE_LIMIT) {
          failIDBRequest(request, fail(
            "Database connection queue quota exceeded", "QuotaExceededError"));
          return;
        }
        queue.push({ request, perform });
      } else if (connectionQueues.size >= DATABASE_LIMIT) {
        failIDBRequest(request, connectionTaskError());
      } else {
        queue = [];
        connectionQueues.set(name, queue);
        perform();
      }
    })) throw connectionTaskError();
  };
  const afterConnectionsClose = (state, request, newVersion, done, finish) => {
    if (!state.connections.size) {
      done();
      return;
    }
    const init = { oldVersion: state.version, newVersion };
    const checkBlocked = () => {
      if (!state.connections.size) {
        done();
        return;
      }
      state.blockedConnection = () => {
        if (!scheduleDatabaseTask(done)) {
          failIDBRequest(request, connectionTaskError());
          finish();
        }
      };
      request._dispatch("blocked", init);
    };
    let notified = false;
    for (const existing of [...state.connections]) {
      if (existing._closed) continue;
      notified = true;
      existing._dispatch("versionchange", init);
    }
    /* Only owners that received versionchange have a listener checkpoint.
       A close-pending owner with accepted writes still blocks immediately. */
    if (!notified) checkBlocked();
    else if (!scheduleDatabaseTask(checkBlocked)) {
      failIDBRequest(request, connectionTaskError());
      finish();
    }
  };

  class IDBFactory {
    #reader;
    /* A worker's factory builds the values and keys its connections read
       with the worker realm's constructors (clone, cloneKey). Only the
       Worker module makes one, with the constructors it captured
       (reader); release, at the worker's termination, forgets them. The
       constructor takes no reader, so a page's cannot run in the
       browser's requests. */
    static {
      workerFactory = (reader, release) => {
        if (!reader) return;
        const realm = prototypeOf(reader.Array);
        if (!release) {
          realmReaders.set(realm, reader);
          const factory = new IDBFactory();
          factory.#reader = reader;
          return factory;
        }
        if (realmReaders.get(realm) === reader) realmReaders.delete(realm);
      };
    }
    open(name, version) {
      const reader = #reader in this ? this.#reader : undefined;
      name = boundedName(name, 256, "Database name");
      const requestedVersion = version === undefined ? undefined : Number(version);
      if (requestedVersion !== undefined &&
          (!Number.isInteger(requestedVersion) || requestedVersion <= 0))
        throw new TypeError("Version must be a positive integer");
      const request = new IDBOpenDBRequest();
      stats.opens++;
      enqueueConnection(name, request, (finish) => {
        let state = databases.get(name);
        if (!state) {
          if (databases.size >= DATABASE_LIMIT) {
            failIDBRequest(request,
              fail("Database quota exceeded", "QuotaExceededError"),
            );
            finish();
            return;
          }
          state = {
            name,
            version: 0,
            stores: new Map(),
            connections: new Set(),
            writeLocks: new Map(),
            upgrading: null,
            activeTransactions: new Set(),
            transactionQueue: [],
            pendingRequests: 0,
            blockedConnection: null,
          };
          databases.set(name, state);
        }
        const finishOpen = (removeFailedDatabase) => {
          if (removeFailedDatabase && state.version === 0)
            databases.delete(name);
          finish();
        };
        const requested = requestedVersion ?? (state.version || 1);
        if (requested < state.version) {
          failIDBRequest(request, fail(
            "Requested version is too old", "VersionError"));
          finishOpen(false);
          return;
        }
        const performOpen = () => {
          const connection = new IDBDatabase(state, reader);
          if (requested > state.version) {
            const oldVersion = state.version;
            let transaction;
            try {
              transaction = new IDBTransaction(
                connection,
                state.stores.keys(),
                "versionchange",
                true,
              );
            } catch (error) {
              connection.close();
              failIDBRequest(request, error);
              finishOpen(oldVersion === 0);
              return;
            }
            state.version = connection.version = requested;
            connection._upgradeTransaction = transaction;
            prepareIDBUpgradeRequest(request, connection, transaction);
            addListener(transaction, "complete", () => {
              connection._upgradeTransaction = null;
              setIDBRequestTransaction(request, null);
              if (connection._closed) {
                resetIDBRequest(request);
                failIDBRequest(request,
                  fail("Database connection was closed", "AbortError"));
              } else {
                succeedIDBRequest(request, connection, true);
              }
              finishOpen(false);
            });
            addListener(transaction, "abort", () => {
              connection.version = oldVersion;
              connection._upgradeTransaction = null;
              setIDBRequestTransaction(request, null);
              resetIDBRequest(request);
              connection.close();
              /* Whatever aborted the upgrade (transaction.error), the open
                 request fails with AbortError. */
              failIDBRequest(request, fail("Upgrade aborted", "AbortError"));
              finishOpen(oldVersion === 0);
            });
            const upgradeOutcome = request._dispatch("upgradeneeded", {
              oldVersion,
              newVersion: requested,
            });
            if (upgradeOutcome & IDB_EVENT_HANDLER_THREW) {
              transaction._abort(
                fail("Upgrade handler threw an exception", "AbortError"),
              );
            } else {
              transaction._maybeComplete();
            }
          } else {
            succeedIDBRequest(request, connection);
            finishOpen(false);
          }
        };
        if (requested > state.version)
          afterConnectionsClose(state, request, requested, performOpen, finish);
        else performOpen();
      });
      return request;
    }
    deleteDatabase(name) {
      name = boundedName(name, 256, "Database name");
      const request = new IDBOpenDBRequest();
      stats.deletes++;
      enqueueConnection(name, request, (finish) => {
        const state = databases.get(name);
        const remove = () => {
          if (state) {
            const usage = stateUsage(state);
            stats.records -= usage.records;
            stats.bytes -= usage.bytes;
            databases.delete(name);
          }
          succeedIDBRequest(request, undefined, false,
                            { oldVersion: state?.version ?? 0, newVersion: null });
          finish();
        };
        if (state) afterConnectionsClose(state, request, null, remove, finish);
        else remove();
      });
      return request;
    }
    cmp(first, second) {
      return compareKeys(validateKey(first), validateKey(second));
    }
    databases() {
      return Promise.resolve(
        [...databases.values()].map((state) => ({
          name: state.name,
          version: state.version,
        })),
      );
    }
  }

  Object.assign(globalThis, {
    DOMStringList,
    IDBRequest,
    IDBOpenDBRequest,
    IDBKeyRange,
    IDBTransaction,
    IDBObjectStore,
    IDBIndex,
    IDBCursor,
    IDBCursorWithValue,
    IDBDatabase,
    IDBFactory,
  });
  globalThis.indexedDB = new IDBFactory();
  publish("__tilefinchAbortIndexedDBForWorker", (database) => {
    if (!(database instanceof IDBDatabase)
        || !database._state.connections.has(database)) return false;
    const state = database._state,
      cancellation = fail("Worker is terminated", "AbortError");
    if (state.upgrading && state.upgrading.db === database)
      state.upgrading._abort(cancellation);
    for (const transaction of [...state.activeTransactions]) {
      if (transaction.db === database) transaction._abort(cancellation);
    }
    for (const transaction of [...state.transactionQueue]) {
      if (transaction.db === database) transaction._abort(cancellation);
    }
    database.close();
    return true;
  });
})();
