(() => {
  if (globalThis.indexedDB !== undefined) return;

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
  const arrayIncludes = Array.prototype.includes;
  const arrayJoin = Array.prototype.join;
  const intrinsicApply = Reflect.apply;
  /* Record identity must not be author-controllable. A page that replaces
     JSON.stringify makes records.delete(keyToken(...)) miss the entry while
     the byte and record subtraction still runs, so stats.bytes and
     stats.records walk negative and the 16 MiB quota stops bounding
     anything. */
  const trustedJSONStringify = JSON.stringify;
  const trustedStructuredClone = globalThis.__tilefinchCloneWorkerValue,
    trustedCloneIntrinsics = globalThis.__tilefinchWorkerCloneIntrinsics.owner,
    trustedScheduleTimeout = globalThis.__tilefinchScheduleTimeout;
  const scheduleDatabaseTask = (callback) => {
    const id = intrinsicApply(trustedScheduleTimeout, globalThis, [callback, 0]);
    return id !== 0;
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
  const statsView = {};
  for (const key of Object.keys(stats)) {
    Object.defineProperty(statsView, key, {
      enumerable: true,
      get: () => stats[key],
    });
  }
  Object.freeze(statsView);
  Object.defineProperty(globalThis, "__tilefinchIndexedDBStats", {
    value: statsView,
    writable: false,
    configurable: false,
  });

  const fail = (message, name) => new DOMException(message, name);
  const clone = (value) => {
    try {
      return intrinsicApply(trustedStructuredClone, globalThis, [
        value, trustedCloneIntrinsics,
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
    if (value instanceof Date) return 24;
    if (value instanceof RegExp)
      return 32 + value.source.length * 2 + value.flags.length * 2;
    if (value instanceof Blob)
      return 48 + value.size + value.type.length * 2 +
        (typeof File !== "undefined" && value instanceof File
          ? value.name.length * 2 + 16
          : 0);
    if (value instanceof ArrayBuffer) return 32 + value.byteLength;
    if (ArrayBuffer.isView(value)) return 48 + value.byteLength;
    if (typeof value !== "object")
      throw fail("The value could not be cloned", "DataCloneError");
    /* Structured serialization preserves aliases and cycles. Count a bounded
       reference for an already visited object instead of rejecting a value
       the clone algorithm accepts, and keep the set for the whole graph so
       aliases are not charged as duplicate retained payloads. */
    if (seen.has(value)) return 8;
    seen.add(value);
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
  const binaryKeyBytes = (key) => {
    if (key instanceof ArrayBuffer) return new Uint8Array(key);
    if (ArrayBuffer.isView(key))
      return new Uint8Array(key.buffer, key.byteOffset, key.byteLength);
    return null;
  };
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
    if (key instanceof Date) return ["date", key.getTime()];
    const bytes = binaryKeyBytes(key);
    if (bytes !== null)
      return ["binary", intrinsicApply(arrayJoin, bytes, [","])];
    if (Array.isArray(key)) {
      const parts = [];
      for (const item of key) parts.push(keyTokenValue(item));
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
    if (key instanceof Date && Number.isFinite(key.getTime())) return key;
    const bytes = binaryKeyBytes(key);
    if (bytes !== null && bytes.byteLength <= KEY_BINARY_BYTE_LIMIT) return key;
    if (Array.isArray(key) && depth < 8 && key.length <= 16) {
      for (const item of key) validateKey(item, depth + 1);
      return key;
    }
    throw fail("The key is not a supported IndexedDB key", "DataError");
  };
  const cloneKey = (key) => {
    if (key instanceof Date) return new Date(key.getTime());
    const bytes = binaryKeyBytes(key);
    if (bytes !== null) {
      const copy = new Uint8Array(bytes.byteLength);
      copy.set(bytes);
      return copy.buffer;
    }
    if (Array.isArray(key)) {
      const copy = [];
      for (const item of key) copy.push(cloneKey(item));
      return copy;
    }
    return key;
  };
  const keyTypeRank = (key) => {
    if (typeof key === "number") return 0;
    if (key instanceof Date) return 1;
    if (typeof key === "string") return 2;
    if (binaryKeyBytes(key) !== null) return 3;
    if (Array.isArray(key)) return 4;
    return -1;
  };
  const compareKeys = (left, right) => {
    const leftRank = keyTypeRank(left),
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
      left = left.getTime();
      right = right.getTime();
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
      let next = current[part];
      if (next === undefined) {
        next = {};
        Object.defineProperty(current, part, {
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
    Object.defineProperty(current, parts[parts.length - 1], {
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
      if (!schema.unique) continue;
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
  const nameList = (values) =>
    new DOMStringList(...Array.from(values, String).sort());
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

  const idbHandlerStates = new WeakMap();
  class TilefinchIDBEventTarget extends EventTarget {
    constructor() {
      super();
      idbHandlerStates.set(this, new Map());
      this._lastDispatchError = null;
    }
    dispatchEvent(event) {
      const type = String((event && event.type) || "");
      if (!type) throw new TypeError("Event type is required");
      this._lastDispatchError = null;
      const parents = [];
      if (event.bubbles && this instanceof IDBRequest && this.transaction) {
        parents.push(this.transaction);
        if (this.transaction.db) parents.push(this.transaction.db);
      } else if (event.bubbles && this instanceof IDBTransaction && this.db) {
        parents.push(this.db);
      }
      globalThis.__tilefinchPrepareEvent(event, this, [this, ...parents]);
      const observeError = (error) => {
        if (this._lastDispatchError === null) this._lastDispatchError = error;
      };
      if (!event.__stopped) {
        globalThis.__tilefinchInvokeEventTarget(
          this, event, true, Event.AT_TARGET, observeError);
        if (!event.__immediateStopped)
          globalThis.__tilefinchInvokeEventTarget(
            this, event, false, Event.AT_TARGET, observeError);
      }
      if (event.bubbles && !event.__stopped)
        for (const parent of parents) {
          globalThis.__tilefinchInvokeEventTarget(
            parent, event, false, Event.BUBBLING_PHASE, observeError);
          if (event.__stopped) break;
        }
      globalThis.__tilefinchFinishEventDispatch(event);
      return !event.defaultPrevented;
    }
    _dispatch(type, init = {}) {
      const event = new Event(type, {
        bubbles: type === "error",
        cancelable: type === "error",
      });
      Object.assign(event, init);
      return this.dispatchEvent(event);
    }
  }
  for (const type of [
    "abort", "blocked", "close", "complete", "error", "success",
    "upgradeneeded", "versionchange",
  ])
    Object.defineProperty(TilefinchIDBEventTarget.prototype, `on${type}`, {
      configurable: true,
      enumerable: true,
      get() {
        return idbHandlerStates.get(this)?.get(type)?.callback || null;
      },
      set(callback) {
        const state = idbHandlerStates.get(this);
        if (!state) throw new TypeError("Illegal invocation");
        const next = typeof callback === "function" ? callback : null;
        let entry = state.get(type);
        if (!entry && next) {
          entry = {
            callback: next,
            listener: (event) => entry.callback?.call(this, event),
          };
          state.set(type, entry);
          EventTarget.prototype.addEventListener.call(
            this, type, entry.listener);
        } else if (entry && !next) {
          EventTarget.prototype.removeEventListener.call(
            this, type, entry.listener);
          state.delete(type);
        } else if (entry) entry.callback = next;
      },
    });

  const idbRequestStates = new WeakMap();
  const setIDBRequestTransaction = (request, transaction) => {
    idbRequestStates.get(request).transaction = transaction;
  };
  const prepareIDBUpgradeRequest = (request, result, transaction) => {
    const state = idbRequestStates.get(request);
    state.result = result;
    state.error = null;
    state.transaction = transaction;
    state.readyState = "done";
  };
  const resetIDBRequest = (request) => {
    const state = idbRequestStates.get(request);
    state.result = undefined;
    state.error = null;
    state.readyState = "pending";
  };
  const succeedIDBRequest = (request, result, redispatch = false) => {
    const state = idbRequestStates.get(request);
    if (state.readyState === "done" && !redispatch) return null;
    state.result = result;
    state.error = null;
    state.readyState = "done";
    request._dispatch("success");
    return request._lastDispatchError;
  };
  const failIDBRequest = (request, error) => {
    const state = idbRequestStates.get(request);
    if (state.readyState === "done") return true;
    state.result = undefined;
    state.error =
      error instanceof DOMException
        ? error
        : fail(String(error), "UnknownError");
    state.readyState = "done";
    return request._dispatch("error");
  };
  class IDBRequest extends TilefinchIDBEventTarget {
    constructor(source = null, transaction = null) {
      super();
      idbRequestStates.set(this, {
        source,
        transaction,
        readyState: "pending",
        result: undefined,
        error: null,
      });
      this.onsuccess = null;
      this.onerror = null;
    }
    get source() {
      const state = idbRequestStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.source;
    }
    get transaction() {
      const state = idbRequestStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.transaction;
    }
    get readyState() {
      const state = idbRequestStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.readyState;
    }
    get result() {
      const state = idbRequestStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (state.readyState !== "done")
        throw fail("The request is still pending", "InvalidStateError");
      return state.result;
    }
    get error() {
      const state = idbRequestStates.get(this);
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
    constructor(lower, upper, lowerOpen, upperOpen) {
      this.lower = lower === undefined
        ? undefined
        : cloneKey(validateKey(lower));
      this.upper = upper === undefined
        ? undefined
        : cloneKey(validateKey(upper));
      this.lowerOpen = !!lowerOpen;
      this.upperOpen = !!upperOpen;
    }
    includes(key) {
      key = validateKey(key);
      if (this.lower !== undefined) {
        const compared = compareKeys(key, this.lower);
        if (compared < 0 || (compared === 0 && this.lowerOpen)) return false;
      }
      if (this.upper !== undefined) {
        const compared = compareKeys(key, this.upper);
        if (compared > 0 || (compared === 0 && this.upperOpen)) return false;
      }
      return true;
    }
    static only(value) {
      value = validateKey(value);
      return new IDBKeyRange(value, value, false, false);
    }
    static lowerBound(value, open = false) {
      value = validateKey(value);
      return new IDBKeyRange(value, undefined, open, false);
    }
    static upperBound(value, open = false) {
      value = validateKey(value);
      return new IDBKeyRange(undefined, value, false, open);
    }
    static bound(lower, upper, lowerOpen = false, upperOpen = false) {
      lower = validateKey(lower);
      upper = validateKey(upper);
      const compared = compareKeys(lower, upper);
      if (compared > 0 || (compared === 0 && (lowerOpen || upperOpen)))
        throw fail("Lower bound exceeds upper bound", "DataError");
      return new IDBKeyRange(lower, upper, lowerOpen, upperOpen);
    }
  }
  const snapshotQuery = (query) => {
    if (query === undefined || query === null) return query;
    if (query instanceof IDBKeyRange)
      return new IDBKeyRange(
        query.lower,
        query.upper,
        query.lowerOpen,
        query.upperOpen,
      );
    return cloneKey(validateKey(query));
  };
  const matchesQuery = (key, query) =>
    query === undefined ||
    query === null ||
    (query instanceof IDBKeyRange
      ? query.includes(key)
      : compareKeys(key, query) === 0);
  const transactionScopesOverlap = (left, right) => {
    for (const name of left.objectStoreNames) {
      if (right.objectStoreNames.contains(name)) return true;
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
      this.objectStoreNames = nameList(stores);
      this._upgrade = upgrade;
      this._state = upgrade ? "active" : "pending";
      this._pending = 0;
      this._operations = [];
      this._completionScheduled = false;
      this._commitRequested = false;
      this._acceptingRequests = true;
      this._abortSequence = null;
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
         The first later task closes that window before any queued database
         operation re-opens it for its own success/error dispatch. */
      if (!scheduleDatabaseTask(() => {
        if (this._abortSequence) {
          this._drainAbortSequence(this._abortSequence, false);
          return;
        }
        if (this._state !== "finished") this._acceptingRequests = false;
      })) {
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
    _scheduleOperation(operation, sequence = null) {
      return scheduleDatabaseTask(() => {
        if (sequence && sequence.aborting) {
          this._drainAbortSequence(sequence, false);
          return;
        }
        if (this._state === "finished") {
          operation();
          return;
        }
        this._acceptingRequests = true;
        try {
          operation();
        } finally {
          queueMicrotask(() => {
            if (this._state !== "finished") this._acceptingRequests = false;
          });
        }
      });
    }
    _activate() {
      if (this._state !== "pending") return;
      this._state = "active";
      const state = this.db._state;
      state.activeTransactions.add(this);
      if (this.mode === "readwrite") {
        for (const name of this.objectStoreNames) {
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
      queueMicrotask(() => this._maybeComplete());
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
    objectStore(name) {
      name = boundedName(name, 128, "Object store name");
      if (this._state === "finished" || this._state === "committing"
          || this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (!this._upgrade && !this.objectStoreNames.contains(name))
        throw fail(
          "Object store is outside transaction scope",
          "NotFoundError",
        );
      const store = this.db._state.stores.get(name);
      if (!store) throw fail("Object store does not exist", "NotFoundError");
      return new IDBObjectStore(this, store);
    }
    abort() {
      this._abort(fail("Transaction aborted", "AbortError"));
    }
    commit() {
      if (this._state !== "active" && this._state !== "pending")
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
    _request(source, operation, write = false) {
      if (this._state === "finished" || this._state === "committing"
          || this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (this._pending >= REQUEST_LIMIT)
        throw fail("Transaction request quota exceeded", "QuotaExceededError");
      if (write && this.mode === "readonly")
        throw fail("Transaction is read only", "ReadOnlyError");
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
          this._pending--;
          return;
        }
        try {
          const dispatchError = succeedIDBRequest(request, operation());
          if (dispatchError !== null) {
            this._pending--;
            this._abort(fail(
              "A request success handler threw an exception",
              "AbortError",
            ));
            return;
          }
        } catch (error) {
          const mayAbort = failIDBRequest(request, error);
          if (mayAbort) this._abort(request.error);
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
      if (this._state === "finished" || this._state === "committing"
          || this._commitRequested || !this._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
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
          const mayAbort = failIDBRequest(request, error);
          if (mayAbort) this._abort(request.error);
          this._finishCursor();
          return;
        }
        if (!entries.length) {
          succeedIDBRequest(request, null);
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
        this._completionScheduled
      )
        return;
      this._completionScheduled = true;
      queueMicrotask(() => {
        this._completionScheduled = false;
        if ((this._state !== "active" && this._state !== "committing")
            || this._pending !== 0)
          return;
        this._state = "committing";
        if (!scheduleDatabaseTask(() => {
          if (this._state !== "committing" || this._pending !== 0) return;
          this._state = "finished";
          this._snapshots.clear();
          this._upgradeSnapshot = null;
          this._releaseLocks();
          this._dispatch("complete");
        })) this._abort(fail(
          "Database task quota exceeded", "QuotaExceededError"));
      });
    }
    _abort(error, sequence = null) {
      if (this._state === "finished") return;
      const wasPending = this._state === "pending";
      this._state = "finished";
      this.error =
        error instanceof DOMException
          ? error
          : fail(String(error), "AbortError");
      if (this._upgradeSnapshot) {
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
      } else {
        for (const [store, snapshot] of this._snapshots) {
          stats.records += snapshot.records.size - store.records.size;
          stats.bytes += snapshot.bytes - store.bytes;
          store.records = snapshot.records;
          store.bytes = snapshot.bytes;
          store.nextKey = snapshot.nextKey;
        }
      }
      this._snapshots.clear();
      this._upgradeSnapshot = null;
      if (sequence) return;
      if (this._operations.length) {
        const operations = this._operations;
        this._operations = [];
        if (wasPending) this.db._state.pendingRequests -= operations.length;
        for (const operation of operations)
          if (!this._scheduleOperation(operation)) operation();
      }
      this._releaseLocks();
      this._dispatch("abort");
    }
    _releaseLocks() {
      const state = this.db._state;
      if (state.upgrading === this) state.upgrading = null;
      state.activeTransactions.delete(this);
      for (const name of this._lockedStores) {
        if (state.writeLocks.get(name) === this) state.writeLocks.delete(name);
      }
      this._lockedStores = [];
      queueMicrotask(() => runTransactionQueue(state));
    }
  }

  class IDBObjectStore {
    constructor(transaction, store) {
      this.transaction = transaction;
      this._store = store;
      this.name = store.name;
      this.keyPath = store.keyPath;
      this.autoIncrement = store.autoIncrement;
    }
    get indexNames() {
      return nameList(this._store.indexes.keys());
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
      if (key === undefined && this.keyPath !== null)
        key = keyAtPath(value, this.keyPath);
      const generated = key === undefined && this.autoIncrement;
      if (generated) {
        if (!Number.isFinite(this._store.nextKey))
          throw fail("The key generator is exhausted", "ConstraintError");
        key = this._store.nextKey;
        if (this.keyPath !== null) injectKeyAtPath(value, this.keyPath, key);
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
      if (
        this.autoIncrement && typeof key === "number" &&
        Number.isFinite(key) && key >= this._store.nextKey
      ) {
        this._store.nextKey = key >= 9007199254740992
          ? Infinity
          : Math.floor(key) + 1;
      }
      this._store.bytes += delta;
      stats.bytes += delta;
      if (!previous) stats.records++;
      stats.peakBytes = Math.max(stats.peakBytes, stats.bytes);
      return cloneKey(key);
    }
    _preparedWrite(value, key, overwrite) {
      /* Refuse an obviously over-bound BufferSource in the queued request
         without first allocating a second multi-megabyte copy. This keeps a
         normal quota refusal recoverable under the PSP heap while ordinary
         values are still cloned synchronously at the API boundary. */
      const directBytes = value instanceof ArrayBuffer
          ? value.byteLength
          : ArrayBuffer.isView(value) ? value.byteLength : 0,
        copiedKey = key === undefined ? undefined : cloneKey(validateKey(key));
      if (directBytes > RECORD_BYTE_LIMIT - 128)
        return this.transaction._request(
          this,
          () => {
            stats.quotaErrors++;
            throw fail("IndexedDB storage quota exceeded", "QuotaExceededError");
          },
          true,
        );
      const copied = clone(value);
      return this.transaction._request(
        this,
        () => this._storeValue(copied, copiedKey, overwrite),
        true,
      );
    }
    put(value, key) {
      return this._preparedWrite(value, key, true);
    }
    add(value, key) {
      return this._preparedWrite(value, key, false);
    }
    get(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => {
        const record =
          query instanceof IDBKeyRange
            ? this._entries(query, 1)[0]
            : this._store.records.get(keyToken(validateKey(query)));
        return record ? clone(record.value) : undefined;
      });
    }
    getKey(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => {
        const record =
          query instanceof IDBKeyRange
            ? this._entries(query, 1)[0]
            : this._store.records.get(keyToken(validateKey(query)));
        return record ? cloneKey(record.key) : undefined;
      });
    }
    getAll(query, count) {
      query = snapshotQuery(query);
      count = count === undefined ? undefined : Number(count);
      return this.transaction._request(this, () =>
        this._entries(query, count).map((record) => clone(record.value)),
      );
    }
    getAllKeys(query, count) {
      query = snapshotQuery(query);
      count = count === undefined ? undefined : Number(count);
      return this.transaction._request(this, () =>
        this._entries(query, count).map((record) => cloneKey(record.key)),
      );
    }
    count(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => {
        if (query === undefined || query === null)
          return this._store.records.size;
        if (!(query instanceof IDBKeyRange))
          return this._store.records.has(keyToken(validateKey(query))) ? 1 : 0;
        let count = 0;
        for (const record of this._store.records.values())
          if (query.includes(record.key)) count++;
        return count;
      });
    }
    delete(query) {
      query = snapshotQuery(query);
      return this.transaction._request(
        this,
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
            stats.bytes -= record.bytes;
            stats.records--;
          }
          return undefined;
        },
        true,
      );
    }
    clear() {
      return this.transaction._request(
        this,
        () => {
          this.transaction._snapshot(this._store);
          stats.records -= this._store.records.size;
          stats.bytes -= this._store.bytes;
          this._store.records.clear();
          this._store.bytes = 0;
          return undefined;
        },
        true,
      );
    }
    index(name) {
      name = boundedName(name, 128, "Index name");
      const schema = this._store.indexes.get(name);
      if (!schema) throw fail("Index does not exist", "NotFoundError");
      return new IDBIndex(this.transaction, this, schema);
    }
    createIndex(name, keyPath, options = {}) {
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
      const schema = {
        name,
        keyPath,
        multiEntry: !!options.multiEntry,
        unique: !!options.unique,
      };
      if (schema.unique) {
        const seen = new Set();
        for (const record of this._store.records.values())
          for (const key of indexKeysForValue(record.value, schema)) {
            const token = keyToken(key);
            if (seen.has(token))
              throw fail(
                "Existing records violate the unique index",
                "ConstraintError",
              );
            seen.add(token);
          }
      }
      this._store.indexes.set(name, schema);
      return new IDBIndex(this.transaction, this, schema);
    }
    deleteIndex(name) {
      if (!this.transaction._upgrade)
        throw fail(
          "Indexes can only be deleted during upgrade",
          "InvalidStateError",
        );
      name = boundedName(name, 128, "Index name");
      if (!this._store.indexes.delete(name))
        throw fail("Index does not exist", "NotFoundError");
    }
    openCursor(query, direction = "next") {
      return this._cursorRequest(query, direction, true);
    }
    openKeyCursor(query, direction = "next") {
      return this._cursorRequest(query, direction, false);
    }
    _cursorRequest(query, direction, withValue) {
      query = snapshotQuery(query);
      direction = String(direction);
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (direction.startsWith("prev")) entries.reverse();
          return entries;
        },
        withValue,
        direction,
      );
    }
  }

  class IDBIndex {
    constructor(transaction, objectStore, schema) {
      this.transaction = transaction;
      this.objectStore = objectStore;
      this._schema = schema;
      this.name = schema.name;
      this.keyPath = schema.keyPath;
      this.multiEntry = schema.multiEntry;
      this.unique = schema.unique;
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
    get(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => {
        const entry = this._entries(query, 1)[0];
        return entry ? clone(entry.record.value) : undefined;
      });
    }
    getKey(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => {
        const entry = this._entries(query, 1)[0];
        return entry ? cloneKey(entry.record.key) : undefined;
      });
    }
    getAll(query, count) {
      query = snapshotQuery(query);
      count = count === undefined ? undefined : Number(count);
      return this.transaction._request(this, () =>
        this._entries(query, count).map((entry) => clone(entry.record.value)),
      );
    }
    getAllKeys(query, count) {
      query = snapshotQuery(query);
      count = count === undefined ? undefined : Number(count);
      return this.transaction._request(this, () =>
        this._entries(query, count).map((entry) => cloneKey(entry.record.key)),
      );
    }
    count(query) {
      query = snapshotQuery(query);
      return this.transaction._request(this, () => this._entries(query).length);
    }
    openCursor(query, direction = "next") {
      query = snapshotQuery(query);
      direction = String(direction);
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (direction.startsWith("prev")) entries.reverse();
          return entries;
        },
        true,
        direction,
      );
    }
    openKeyCursor(query, direction = "next") {
      query = snapshotQuery(query);
      direction = String(direction);
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (direction.startsWith("prev")) entries.reverse();
          return entries;
        },
        false,
        direction,
      );
    }
  }

  class IDBCursor {
    constructor(request, source, entries, withValue, direction, transaction) {
      this.request = request;
      this.source = source;
      this.direction = direction;
      this._transaction = transaction;
      this._entries = entries;
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
      this.key = entry
        ? cloneKey(entry.indexKey === undefined ? record.key : entry.indexKey)
        : undefined;
      this.primaryKey = record ? cloneKey(record.key) : undefined;
      if (this._withValue)
        this.value = record ? clone(record.value) : undefined;
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
        if (succeedIDBRequest(this.request, null) !== null)
          this._transaction._abort(fail(
            "A cursor success handler threw an exception",
            "AbortError",
          ));
        this._finish();
        return;
      }
      this._sync();
      if (succeedIDBRequest(this.request, this) !== null) {
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
    _move(position) {
      if (this._finished || this._transaction._state !== "active"
          || !this._transaction._acceptingRequests)
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (this._continued)
        throw fail("Cursor is already iterating", "InvalidStateError");
      this._continued = true;
      this._position = position;
      if (!this._transaction._scheduleOperation(() => this._emit())) {
        this._continued = false;
        throw fail("Database task quota exceeded", "QuotaExceededError");
      }
    }
    continue(key) {
      let position = this._position + 1;
      if (key !== undefined)
        while (position < this._entries.length) {
          const entry = this._entries[position],
            record = entry.record || entry,
            cursorKey =
              entry.indexKey === undefined ? record.key : entry.indexKey;
          const compared = compareKeys(cursorKey, key);
          if (
            String(this.direction).startsWith("prev")
              ? compared <= 0
              : compared >= 0
          )
            break;
          position++;
        }
      this._move(position);
    }
    continuePrimaryKey() {
      this.continue();
    }
    advance(count) {
      count = Number(count);
      if (!Number.isInteger(count) || count <= 0)
        throw new TypeError("Count must be positive");
      this._move(this._position + count);
    }
    update(value) {
      const store =
        this.source instanceof IDBIndex ? this.source.objectStore : this.source;
      return store.put(value, this.primaryKey);
    }
    delete() {
      const store =
        this.source instanceof IDBIndex ? this.source.objectStore : this.source;
      return store.delete(this.primaryKey);
    }
  }
  class IDBCursorWithValue extends IDBCursor {}

  const resumeBlockedDatabaseOpen = (state) => {
    if (state.connections.size || !state.blockedOpen) return;
    const resume = state.blockedOpen;
    state.blockedOpen = null;
    resume();
  };

  class IDBDatabase extends TilefinchIDBEventTarget {
    constructor(state) {
      super();
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
      return nameList(this._state.stores.keys());
    }
    createObjectStore(name, options = {}) {
      if (
        !this._upgradeTransaction ||
        this._upgradeTransaction._state !== "active"
      )
        throw fail(
          "Object stores can only be created during upgrade",
          "InvalidStateError",
        );
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
      };
      this._state.stores.set(name, store);
      this._upgradeTransaction.objectStoreNames = nameList(
        this._state.stores.keys(),
      );
      return new IDBObjectStore(this._upgradeTransaction, store);
    }
    deleteObjectStore(name) {
      if (
        !this._upgradeTransaction ||
        this._upgradeTransaction._state !== "active"
      )
        throw fail(
          "Object stores can only be deleted during upgrade",
          "InvalidStateError",
        );
      name = boundedName(name, 128, "Object store name");
      const store = this._state.stores.get(name);
      if (!store) throw fail("Object store does not exist", "NotFoundError");
      stats.records -= store.records.size;
      stats.bytes -= store.bytes;
      this._state.stores.delete(name);
      this._upgradeTransaction.objectStoreNames = nameList(
        this._state.stores.keys(),
      );
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
      this._closed = true;
      this._state.connections.delete(this);
      resumeBlockedDatabaseOpen(this._state);
    }
  }
  const forceCloseIDBDatabase = (database) => {
    if (database._closed) return;
    database._closed = true;
    database._state.connections.delete(database);
    database._dispatch("close");
  };

  class IDBFactory {
    open(name, version) {
      name = boundedName(name, 256, "Database name");
      const request = new IDBOpenDBRequest();
      stats.opens++;
      if (!scheduleDatabaseTask(() => {
        let state = databases.get(name);
        const requestedVersion =
          version === undefined ? undefined : Number(version);
        if (
          requestedVersion !== undefined &&
          (!Number.isInteger(requestedVersion) || requestedVersion <= 0)
        ) {
          failIDBRequest(
            request, new TypeError("Version must be a positive integer"));
          return;
        }
        if (!state) {
          if (databases.size >= DATABASE_LIMIT) {
            failIDBRequest(request,
              fail("Database quota exceeded", "QuotaExceededError"),
            );
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
            opening: false,
            openQueue: [],
            blockedOpen: null,
          };
          databases.set(name, state);
        }
        const finishOpen = (removeFailedDatabase) => {
          state.opening = false;
          const next = state.openQueue.shift();
          if (next) {
            state.opening = true;
            if (!scheduleDatabaseTask(next))
              failIDBRequest(request, fail(
                "Database task quota exceeded", "QuotaExceededError"));
          } else if (removeFailedDatabase && state.version === 0) {
            databases.delete(name);
          }
        };
        const performOpen = () => {
          const requested =
            requestedVersion === undefined
              ? state.version || 1
              : requestedVersion;
          if (requested < state.version) {
            failIDBRequest(request,
              fail("Requested version is too old", "VersionError"),
            );
            finishOpen(false);
            return;
          }
          if (requested > state.version && state.connections.size) {
            for (const existing of [...state.connections])
              existing._dispatch("versionchange", {
                oldVersion: state.version,
                newVersion: requested,
              });
            if (state.connections.size) {
              request._dispatch("blocked", {
                oldVersion: state.version,
                newVersion: requested,
              });
              state.blockedOpen = () => {
                if (!scheduleDatabaseTask(performOpen)) {
                  failIDBRequest(request, fail(
                    "Database task quota exceeded",
                    "QuotaExceededError",
                  ));
                  finishOpen(false);
                }
              };
              return;
            }
          }
          const connection = new IDBDatabase(state);
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
            connection._upgradeTransaction = transaction;
            prepareIDBUpgradeRequest(request, connection, transaction);
            transaction.addEventListener("complete", () => {
              state.version = requested;
              connection.version = requested;
              connection._upgradeTransaction = null;
              setIDBRequestTransaction(request, null);
              succeedIDBRequest(request, connection, true);
              finishOpen(false);
            });
            transaction.addEventListener("abort", () => {
              connection._upgradeTransaction = null;
              setIDBRequestTransaction(request, null);
              resetIDBRequest(request);
              connection.close();
              failIDBRequest(request,
                transaction.error || fail("Upgrade aborted", "AbortError"),
              );
              finishOpen(oldVersion === 0);
            });
            request._dispatch("upgradeneeded", {
              oldVersion,
              newVersion: requested,
            });
            if (request._lastDispatchError !== null) {
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
        if (state.opening) {
          if (state.openQueue.length >= OPEN_QUEUE_LIMIT) {
            failIDBRequest(request,
              fail("Database open queue quota exceeded", "QuotaExceededError"),
            );
            return;
          }
          state.openQueue.push(performOpen);
        } else {
          state.opening = true;
          performOpen();
        }
      })) throw fail("Database task quota exceeded", "QuotaExceededError");
      return request;
    }
    deleteDatabase(name) {
      name = boundedName(name, 256, "Database name");
      const request = new IDBOpenDBRequest();
      stats.deletes++;
      if (!scheduleDatabaseTask(() => {
        const state = databases.get(name);
        if (state) {
          for (const connection of [...state.connections])
            forceCloseIDBDatabase(connection);
          const cancellation = fail(
            "Database was deleted", "AbortError");
          if (state.upgrading) state.upgrading._abort(cancellation);
          for (const transaction of [...state.activeTransactions])
            transaction._abort(cancellation);
          for (const transaction of [...state.transactionQueue])
            transaction._abort(cancellation);
          state.transactionQueue = [];
          for (const store of state.stores.values()) {
            stats.records = Math.max(
              0, stats.records - store.records.size);
            stats.bytes = Math.max(0, stats.bytes - store.bytes);
          }
          databases.delete(name);
        }
        succeedIDBRequest(request, undefined);
      })) throw fail("Database task quota exceeded", "QuotaExceededError");
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
  Object.defineProperty(globalThis, "__tilefinchAbortIndexedDBForWorker", {
    configurable: false,
    enumerable: false,
    writable: false,
    value(database) {
      if (!(database instanceof IDBDatabase) || database._closed) return false;
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
    },
  });
})();
