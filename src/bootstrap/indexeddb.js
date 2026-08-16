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
  const databases = new Map();
  const arrayIncludes = Array.prototype.includes;
  const intrinsicApply = Reflect.apply;
  /* Record identity must not be author-controllable. A page that replaces
     JSON.stringify makes records.delete(keyToken(...)) miss the entry while
     the byte and record subtraction still runs, so stats.bytes and
     stats.records walk negative and the 16 MiB quota stops bounding
     anything. */
  const trustedJSONStringify = JSON.stringify;
  const scheduleDatabaseTask = (callback) => {
    const id = setTimeout(callback, 0);
    if (!id) queueMicrotask(callback);
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
      return structuredClone(value);
    } catch (_) {
      throw fail("The value could not be cloned", "DataCloneError");
    }
  };
  const retainedSize = (value, depth = 0, seen = new Set()) => {
    if (depth > 8)
      throw fail("The value is nested too deeply", "DataCloneError");
    if (value === null || value === undefined) return 8;
    if (typeof value === "string") return 16 + value.length * 2;
    if (typeof value === "number" || typeof value === "boolean") return 16;
    if (value instanceof Date) return 24;
    if (value instanceof ArrayBuffer) return 32 + value.byteLength;
    if (ArrayBuffer.isView(value)) return 48 + value.byteLength;
    if (typeof value !== "object" || seen.has(value))
      throw fail("The value could not be cloned", "DataCloneError");
    seen.add(value);
    let total = 32;
    if (Array.isArray(value)) {
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
    seen.delete(value);
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
  const keyToken = (key) => {
    if (key instanceof Date) return `date:${key.getTime()}`;
    if (Array.isArray(key)) return `array:${trustedJSONStringify(key)}`;
    return `${typeof key}:${String(key)}`;
  };
  const validateKey = (key, depth = 0) => {
    if (typeof key === "string") return key;
    if (typeof key === "number" && Number.isFinite(key)) return key;
    if (key instanceof Date && Number.isFinite(key.getTime())) return key;
    if (Array.isArray(key) && depth < 8 && key.length <= 16) {
      for (const item of key) validateKey(item, depth + 1);
      return key;
    }
    throw fail("The key is not a supported IndexedDB key", "DataError");
  };
  const cloneKey = (key) =>
    key instanceof Date
      ? new Date(key.getTime())
      : Array.isArray(key)
        ? key.map(cloneKey)
        : key;
  const compareKeys = (left, right) => {
    if (left instanceof Date) left = left.getTime();
    if (right instanceof Date) right = right.getTime();
    if (Array.isArray(left) && Array.isArray(right)) {
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
    if (left === right) return 0;
    if (typeof left === typeof right) return left < right ? -1 : 1;
    return String(left) < String(right) ? -1 : 1;
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

  class TilefinchIDBEventTarget {
    constructor() {
      this._listeners = new Map();
      this._lastDispatchError = null;
    }
    addEventListener(type, callback) {
      if (typeof callback !== "function") return;
      const key = String(type);
      if (!this._listeners.has(key)) this._listeners.set(key, []);
      const list = this._listeners.get(key);
      if (!list.includes(callback)) list.push(callback);
    }
    removeEventListener(type, callback) {
      const list = this._listeners.get(String(type));
      if (!list) return;
      const index = list.indexOf(callback);
      if (index >= 0) list.splice(index, 1);
    }
    dispatchEvent(event) {
      const type = String((event && event.type) || "");
      if (!type) throw new TypeError("Event type is required");
      event.target = this;
      event.currentTarget = this;
      this._lastDispatchError = null;
      const invoke = (callback) => {
        try {
          globalThis.__tilefinchRunTask(
            "indexeddb:" + type,
            callback,
            this,
            [event],
          );
        } catch (error) {
          if (this._lastDispatchError === null) this._lastDispatchError = error;
          __tilefinchReportUncaught(error, `IndexedDB ${type}`);
        }
      };
      const handler = this[`on${type}`];
      if (typeof handler === "function") invoke(handler);
      for (const callback of [...(this._listeners.get(type) || [])])
        invoke(callback);
      return !event.defaultPrevented;
    }
    _dispatch(type, init = {}) {
      const event = new Event(type, { cancelable: type === "error" });
      Object.assign(event, init);
      return this.dispatchEvent(event);
    }
  }

  class IDBRequest extends TilefinchIDBEventTarget {
    constructor(source = null, transaction = null) {
      super();
      this.source = source;
      this.transaction = transaction;
      this.readyState = "pending";
      this.result = undefined;
      this.error = null;
      this.onsuccess = null;
      this.onerror = null;
    }
    _success(result) {
      if (this.readyState === "done") return;
      this.result = result;
      this.error = null;
      this.readyState = "done";
      this._dispatch("success");
    }
    _failure(error) {
      if (this.readyState === "done") return true;
      this.result = undefined;
      this.error =
        error instanceof DOMException
          ? error
          : fail(String(error), "UnknownError");
      this.readyState = "done";
      return this._dispatch("error");
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
      this.lower = lower;
      this.upper = upper;
      this.lowerOpen = !!lowerOpen;
      this.upperOpen = !!upperOpen;
    }
    includes(key) {
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
      return new IDBKeyRange(value, value, false, false);
    }
    static lowerBound(value, open = false) {
      return new IDBKeyRange(value, undefined, open, false);
    }
    static upperBound(value, open = false) {
      return new IDBKeyRange(undefined, value, false, open);
    }
    static bound(lower, upper, lowerOpen = false, upperOpen = false) {
      if (compareKeys(lower, upper) > 0)
        throw fail("Lower bound exceeds upper bound", "DataError");
      return new IDBKeyRange(lower, upper, lowerOpen, upperOpen);
    }
  }
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
      if (upgrade) queueMicrotask(() => this._maybeComplete());
      else runTransactionQueue(state);
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
      for (const operation of operations) queueMicrotask(operation);
      queueMicrotask(() => this._maybeComplete());
    }
    objectStore(name) {
      name = boundedName(name, 128, "Object store name");
      if (this._state === "finished")
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
      if (this._state === "finished")
        throw fail("Transaction is inactive", "InvalidStateError");
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
      if (this._state === "finished")
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
        if (this._state !== "active") {
          request._failure(fail("Transaction aborted", "AbortError"));
          this._pending--;
          return;
        }
        try {
          request._success(operation());
        } catch (error) {
          const mayAbort = request._failure(error);
          if (mayAbort) this._abort(request.error);
        }
        this._pending--;
        this._maybeComplete();
      };
      if (this._state === "pending") {
        this.db._state.pendingRequests++;
        this._operations.push(run);
      } else queueMicrotask(run);
      return request;
    }
    _openCursor(source, entriesFactory, withValue, direction) {
      if (this._state === "finished")
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
        if (this._state !== "active") {
          request._failure(fail("Transaction aborted", "AbortError"));
          this._finishCursor();
          return;
        }
        let entries;
        try {
          entries = entriesFactory();
        } catch (error) {
          const mayAbort = request._failure(error);
          if (mayAbort) this._abort(request.error);
          this._finishCursor();
          return;
        }
        if (!entries.length) {
          request._success(null);
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
      } else queueMicrotask(run);
      return request;
    }
    _finishCursor() {
      if (this._pending > 0) this._pending--;
      this._maybeComplete();
    }
    _maybeComplete() {
      if (
        this._state !== "active" ||
        this._pending !== 0 ||
        this._completionScheduled
      )
        return;
      this._completionScheduled = true;
      scheduleDatabaseTask(() => {
        this._completionScheduled = false;
        if (this._state !== "active" || this._pending !== 0) return;
        this._state = "finished";
        this._snapshots.clear();
        this._upgradeSnapshot = null;
        this._releaseLocks();
        this._dispatch("complete");
      });
    }
    _abort(error) {
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
      if (this._operations.length) {
        const operations = this._operations;
        this._operations = [];
        if (wasPending) this.db._state.pendingRequests -= operations.length;
        for (const operation of operations) queueMicrotask(operation);
      }
      this._releaseLocks();
      this._dispatch("error");
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
      if (key === undefined && this.autoIncrement) key = this._store.nextKey++;
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
      const copied = clone(value);
      this._store.records.set(token, {
        key: cloneKey(key),
        value: copied,
        bytes,
      });
      this._store.bytes += delta;
      stats.bytes += delta;
      if (!previous) stats.records++;
      stats.peakBytes = Math.max(stats.peakBytes, stats.bytes);
      return cloneKey(key);
    }
    put(value, key) {
      return this.transaction._request(
        this,
        () => this._storeValue(value, key, true),
        true,
      );
    }
    add(value, key) {
      return this.transaction._request(
        this,
        () => this._storeValue(value, key, false),
        true,
      );
    }
    get(query) {
      return this.transaction._request(this, () => {
        const record =
          query instanceof IDBKeyRange
            ? this._entries(query, 1)[0]
            : this._store.records.get(keyToken(validateKey(query)));
        return record ? clone(record.value) : undefined;
      });
    }
    getKey(query) {
      return this.transaction._request(this, () => {
        const record =
          query instanceof IDBKeyRange
            ? this._entries(query, 1)[0]
            : this._store.records.get(keyToken(validateKey(query)));
        return record ? cloneKey(record.key) : undefined;
      });
    }
    getAll(query, count) {
      return this.transaction._request(this, () =>
        this._entries(query, count).map((record) => clone(record.value)),
      );
    }
    getAllKeys(query, count) {
      return this.transaction._request(this, () =>
        this._entries(query, count).map((record) => cloneKey(record.key)),
      );
    }
    count(query) {
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
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (String(direction).startsWith("prev")) entries.reverse();
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
        let keys = keyAtPath(record.value, this.keyPath);
        keys = this.multiEntry && Array.isArray(keys) ? keys : [keys];
        for (const key of keys) {
          if (key !== undefined && matchesQuery(key, query))
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
      return this.transaction._request(this, () => {
        const entry = this._entries(query, 1)[0];
        return entry ? clone(entry.record.value) : undefined;
      });
    }
    getKey(query) {
      return this.transaction._request(this, () => {
        const entry = this._entries(query, 1)[0];
        return entry ? cloneKey(entry.record.key) : undefined;
      });
    }
    getAll(query, count) {
      return this.transaction._request(this, () =>
        this._entries(query, count).map((entry) => clone(entry.record.value)),
      );
    }
    getAllKeys(query, count) {
      return this.transaction._request(this, () =>
        this._entries(query, count).map((entry) => cloneKey(entry.record.key)),
      );
    }
    count(query) {
      return this.transaction._request(this, () => this._entries(query).length);
    }
    openCursor(query, direction = "next") {
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (String(direction).startsWith("prev")) entries.reverse();
          return entries;
        },
        true,
        direction,
      );
    }
    openKeyCursor(query, direction = "next") {
      return this.transaction._openCursor(
        this,
        () => {
          const entries = this._entries(query);
          if (String(direction).startsWith("prev")) entries.reverse();
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
      this.request.readyState = "pending";
      if (this._position >= this._entries.length) {
        this.request._success(null);
        this._finish();
        return;
      }
      this._sync();
      this.request._success(this);
      const generation = ++this._generation;
      scheduleDatabaseTask(() => {
        if (generation === this._generation && !this._continued) this._finish();
      });
    }
    _move(position) {
      if (this._finished || this._transaction._state !== "active")
        throw fail("Transaction is inactive", "TransactionInactiveError");
      if (this._continued)
        throw fail("Cursor is already iterating", "InvalidStateError");
      this._continued = true;
      this._position = position;
      queueMicrotask(() => this._emit());
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
      this._dispatch("close");
    }
  }

  class IDBFactory {
    open(name, version) {
      name = boundedName(name, 256, "Database name");
      const request = new IDBOpenDBRequest();
      stats.opens++;
      queueMicrotask(() => {
        let state = databases.get(name);
        const requestedVersion =
          version === undefined ? undefined : Number(version);
        if (
          requestedVersion !== undefined &&
          (!Number.isInteger(requestedVersion) || requestedVersion <= 0)
        ) {
          request._failure(new TypeError("Version must be a positive integer"));
          return;
        }
        if (!state) {
          if (databases.size >= DATABASE_LIMIT) {
            request._failure(
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
          };
          databases.set(name, state);
        }
        const finishOpen = (removeFailedDatabase) => {
          state.opening = false;
          const next = state.openQueue.shift();
          if (next) {
            state.opening = true;
            queueMicrotask(next);
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
            request._failure(
              fail("Requested version is too old", "VersionError"),
            );
            finishOpen(false);
            return;
          }
          const connection = new IDBDatabase(state);
          request.result = connection;
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
              request._failure(error);
              finishOpen(oldVersion === 0);
              return;
            }
            connection._upgradeTransaction = transaction;
            request.transaction = transaction;
            transaction.addEventListener("complete", () => {
              state.version = requested;
              connection.version = requested;
              connection._upgradeTransaction = null;
              request.transaction = null;
              request._success(connection);
              finishOpen(false);
            });
            transaction.addEventListener("abort", () => {
              connection._upgradeTransaction = null;
              request.transaction = null;
              connection.close();
              request._failure(
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
            request._success(connection);
            finishOpen(false);
          }
        };
        if (state.opening) {
          if (state.openQueue.length >= OPEN_QUEUE_LIMIT) {
            request._failure(
              fail("Database open queue quota exceeded", "QuotaExceededError"),
            );
            return;
          }
          state.openQueue.push(performOpen);
        } else {
          state.opening = true;
          performOpen();
        }
      });
      return request;
    }
    deleteDatabase(name) {
      name = boundedName(name, 256, "Database name");
      const request = new IDBOpenDBRequest();
      stats.deletes++;
      queueMicrotask(() => {
        const state = databases.get(name);
        if (state) {
          for (const connection of [...state.connections]) connection.close();
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
        request._success(undefined);
      });
      return request;
    }
    cmp(first, second) {
      return compareKeys(first, second);
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
})();
