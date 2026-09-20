(() => {
  const command = globalThis.__tilefinchOpfsCommand,
    createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap,
    blobBytes = globalThis.__tilefinchBlobBytes,
    scheduleTask = globalThis.__tilefinchScheduleTask,
    storageManager = globalThis.__tilefinchOpfsStorageManager,
    TrustedPromise = Promise,
    trustedPromiseReject = Function.call.bind(Promise.reject, Promise),
    trustedPromiseThen = Function.call.bind(Promise.prototype.then);
  if (typeof command !== "function" || typeof createPrivateWeakMap !== "function"
      || typeof blobBytes !== "function" || typeof scheduleTask !== "function")
    throw new Error("Origin-private file system bridge is unavailable");
  delete globalThis.__tilefinchOpfsCommand;
  delete globalThis.__tilefinchOpfsStorageManager;

  const OP_STAT = 0, OP_CREATE = 1, OP_READ = 2, OP_WRITE = 3,
    OP_REMOVE = 4, OP_LIST = 5,
    KIND_FILE = 1, KIND_DIRECTORY = 2,
    STATUS_OK = 0, STATUS_UNAVAILABLE = 1, STATUS_NOT_FOUND = 2,
    STATUS_TYPE = 3, STATUS_EXISTS = 4, STATUS_NOT_EMPTY = 5,
    STATUS_QUOTA = 6, STATUS_STALE = 8,
    FILE_LIMIT = 64 * 1024, WRITER_LIMIT = 4,
    token = {}, handleStates = createPrivateWeakMap(),
    writerStates = createPrivateWeakMap(), syncStates = createPrivateWeakMap(),
    activeWriterPaths = [];
  let bufferGrowths = 0,
    bufferCopiedBytes = 0;
  if (globalThis.__tilefinchRootCensus)
    Object.defineProperties(globalThis.__tilefinchRootCensus, {
      opfsBufferGrowths: { get: () => bufferGrowths },
      opfsBufferCopiedBytes: { get: () => bufferCopiedBytes },
    });

  const task = (operation) => new TrustedPromise((resolve, reject) => {
    const id = scheduleTask(() => {
      try { resolve(operation()); } catch (error) { reject(error); }
    });
    if (!id) reject(new DOMException(
      "Storage task quota exceeded", "QuotaExceededError"));
  }),
    errorFor = (status) => {
      if (status === STATUS_UNAVAILABLE)
        return new DOMException("Storage is unavailable", "SecurityError");
      if (status === STATUS_NOT_FOUND)
        return new DOMException("Entry was not found", "NotFoundError");
      if (status === STATUS_TYPE)
        return new DOMException("Entry has a different kind", "TypeMismatchError");
      if (status === STATUS_EXISTS)
        return new DOMException("Entry already exists", "InvalidModificationError");
      if (status === STATUS_NOT_EMPTY)
        return new DOMException("Directory is not empty", "InvalidModificationError");
      if (status === STATUS_QUOTA)
        return new DOMException("Storage quota exceeded", "QuotaExceededError");
      if (status === STATUS_STALE)
        return new DOMException("File was replaced", "InvalidStateError");
      return new TypeError("Invalid file-system path");
    },
    checked = (result) => {
      const status = Number(result?.[0]);
      if (status !== STATUS_OK) throw errorFor(status);
      return result;
    },
    requireName = (value) => {
      if (typeof value === "symbol")
        throw new TypeError("Cannot convert a Symbol value to a string");
      const name = String(value);
      if (!name || name === "." || name === ".."
          || name.includes("/") || name.includes("\\") || name.includes("\0"))
        throw new TypeError("Invalid file-system name");
      if (new TextEncoder().encode(name).byteLength > 96)
        throw new TypeError("File-system name exceeds bounded length");
      return name;
    },
    joinPath = (parent, name) => parent === "/"
      ? "/" + name : parent + "/" + name,
    baseName = (path) => path === "" || path === "/"
      ? "" : path.slice(path.lastIndexOf("/") + 1),
    requireHandle = (handle, kind = 0) => {
      const state = handleStates.get(handle);
      if (!state || (kind && state.kind !== kind))
        throw new TypeError("Illegal invocation");
      return state;
    },
    bytesFor = (value) => {
      if (value instanceof Blob) return blobBytes(value);
      if (value instanceof ArrayBuffer) return new Uint8Array(value.slice(0));
      if (ArrayBuffer.isView(value)) return new Uint8Array(
        value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
      return new TextEncoder().encode(String(value));
    },
    mutableBytesFor = (value) => {
      if (value instanceof ArrayBuffer) return new Uint8Array(value);
      if (ArrayBuffer.isView(value)) return new Uint8Array(
        value.buffer, value.byteOffset, value.byteLength);
      throw new TypeError("Expected an ArrayBuffer or view");
    },
    ensureCapacity = (state, length) => {
      if (length <= state.bytes.byteLength) return;
      let capacity = Math.max(64, state.bytes.byteLength);
      while (capacity < length) {
        const doubled = capacity * 2;
        capacity = doubled > FILE_LIMIT ? FILE_LIMIT : doubled;
      }
      const next = new Uint8Array(capacity);
      next.set(state.bytes.subarray(0, state.length));
      bufferGrowths++;
      bufferCopiedBytes += state.length;
      state.bytes = next;
    },
    resize = (state, length) => {
      length = Math.trunc(Number(length));
      if (!Number.isFinite(length) || length < 0)
        throw new TypeError("Invalid file size");
      if (length > FILE_LIMIT)
        throw new DOMException("File exceeds bounded size", "QuotaExceededError");
      if (length === state.length) return;
      const before = state.length;
      ensureCapacity(state, length);
      /* A shrink followed by growth must expose zero-filled bytes even though
         capacity is retained for bounded amortized appends. */
      if (length < before) state.bytes.fill(0, length, before);
      else state.bytes.fill(0, before, length);
      state.length = length;
      if (state.position > length) state.position = length;
    },
    releaseWriter = (state) => {
      if (state.closed) return;
      state.closed = true;
      const at = activeWriterPaths.indexOf(state.path);
      if (at >= 0) activeWriterPaths.splice(at, 1);
      state.bytes = new Uint8Array();
      state.length = 0;
    },
    writerState = (stream) => {
      const state = writerStates.get(stream);
      if (!state || state.closed)
        throw new DOMException("Writer is closed", "InvalidStateError");
      return state;
    },
    syncState = (handle) => {
      const state = syncStates.get(handle);
      if (!state || state.closed)
        throw new DOMException("Access handle is closed", "InvalidStateError");
      return state;
    },
    syncOffset = (state, options) => {
      const at = options?.at,
        value = at === undefined ? state.position : Number(at);
      if (!Number.isSafeInteger(value) || value < 0)
        throw new TypeError("Invalid file offset");
      return value;
    },
    writerSeek = (state, position) => {
      position = Math.trunc(Number(position));
      if (!Number.isFinite(position) || position < 0)
        throw new TypeError("Invalid seek position");
      state.position = position;
    },
    writerTruncate = (state, size) => resize(state, size),
    writerWrite = (state, value) => {
      const isBytes = value instanceof Blob || value instanceof ArrayBuffer
        || ArrayBuffer.isView(value);
      if (!isBytes && value && typeof value === "object"
          && typeof value.type === "string") {
        if (value.type === "seek") {
          writerSeek(state, value.position);
          return;
        }
        if (value.type === "truncate") {
          writerTruncate(state, value.size);
          return;
        }
        if (value.type !== "write") throw new TypeError("Unknown write command");
        if (!("data" in value))
          throw new TypeError("Write command requires data");
        if (value.position !== undefined) writerSeek(state, value.position);
        value = value.data;
      }
      const bytes = bytesFor(value), end = state.position + bytes.byteLength;
      if (end > FILE_LIMIT)
        throw new DOMException("File exceeds bounded size", "QuotaExceededError");
      if (end > state.length) resize(state, end);
      state.bytes.set(bytes, state.position);
      state.position = end;
    };

  class FileSystemHandle {
    constructor(value, path, kind) {
      if (value !== token) throw new TypeError("Illegal constructor");
      handleStates.set(this, { path, kind });
    }
    get kind() {
      return requireHandle(this).kind === KIND_FILE ? "file" : "directory";
    }
    get name() { return baseName(requireHandle(this).path); }
    isSameEntry(other) {
      return task(() => {
        const mine = requireHandle(this), theirs = handleStates.get(other);
        if (!theirs) throw new TypeError("Expected a FileSystemHandle");
        return mine.path === theirs.path && mine.kind === theirs.kind;
      });
    }
    queryPermission() {
      return task(() => {
        requireHandle(this);
        return Number(command(OP_STAT, "/")?.[0]) === STATUS_OK
          ? "granted" : "denied";
      });
    }
    requestPermission() {
      return task(() => {
        requireHandle(this);
        return Number(command(OP_STAT, "/")?.[0]) === STATUS_OK
          ? "granted" : "denied";
      });
    }
  }

  class FileSystemFileHandle extends FileSystemHandle {
    constructor(value, path) {
      if (value !== token) throw new TypeError("Illegal constructor");
      super(token, path, KIND_FILE);
    }
    getFile() {
      return task(() => {
        const state = requireHandle(this, KIND_FILE);
        const result = checked(command(OP_READ, state.path));
        return new File([new Uint8Array(result[1])], baseName(state.path), {
          lastModified: Math.max(0, Number(result[2]) || 0),
        });
      });
    }
    createWritable(options = {}) {
      return task(() => {
        const state = requireHandle(this, KIND_FILE),
          keep = !!options?.keepExistingData;
        if (activeWriterPaths.includes(state.path)
            || activeWriterPaths.length >= WRITER_LIMIT)
          throw new DOMException("File is already being modified",
                                 "NoModificationAllowedError");
        const opened = checked(command(keep ? OP_READ : OP_STAT, state.path));
        if (!keep && Number(opened[1]) !== KIND_FILE)
          throw errorFor(STATUS_TYPE);
        const bytes = keep ? new Uint8Array(opened[1]) : new Uint8Array(),
          generation = Number(opened[keep ? 3 : 4]);
        if (!Number.isSafeInteger(generation) || generation <= 0)
          throw new DOMException("File generation is unavailable", "InvalidStateError");
        activeWriterPaths.push(state.path);
        try {
          return new FileSystemWritableFileStream(
            token, state.path, bytes, generation);
        } catch (error) {
          activeWriterPaths.pop();
          throw error;
        }
      });
    }
    createSyncAccessHandle() {
      return task(() => {
        const state = requireHandle(this, KIND_FILE);
        if (activeWriterPaths.includes(state.path)
            || activeWriterPaths.length >= WRITER_LIMIT)
          throw new DOMException("File is already being modified",
                                 "NoModificationAllowedError");
        const opened = checked(command(OP_READ, state.path)),
          generation = Number(opened[3]);
        if (!Number.isSafeInteger(generation) || generation <= 0)
          throw new DOMException(
            "File generation is unavailable", "InvalidStateError");
        activeWriterPaths.push(state.path);
        try {
          return new FileSystemSyncAccessHandle(
            token, state.path, new Uint8Array(opened[1]), generation);
        } catch (error) {
          activeWriterPaths.pop();
          throw error;
        }
      });
    }
  }

  class FileSystemDirectoryHandle extends FileSystemHandle {
    constructor(value, path) {
      if (value !== token) throw new TypeError("Illegal constructor");
      super(token, path, KIND_DIRECTORY);
    }
    getFileHandle(name, options = {}) {
      return task(() => {
        const state = requireHandle(this, KIND_DIRECTORY),
          fileName = requireName(name), create = !!options?.create,
          path = joinPath(state.path, fileName);
        const found = command(OP_STAT, path), status = Number(found?.[0]);
        if (status === STATUS_NOT_FOUND && create) checked(command(
          OP_CREATE, path, KIND_FILE));
        else if (status !== STATUS_OK) throw errorFor(status);
        else if (Number(found[1]) !== KIND_FILE) throw errorFor(STATUS_TYPE);
        return new FileSystemFileHandle(token, path);
      });
    }
    getDirectoryHandle(name, options = {}) {
      return task(() => {
        const state = requireHandle(this, KIND_DIRECTORY),
          directoryName = requireName(name), create = !!options?.create,
          path = joinPath(state.path, directoryName);
        const found = command(OP_STAT, path), status = Number(found?.[0]);
        if (status === STATUS_NOT_FOUND && create) checked(command(
          OP_CREATE, path, KIND_DIRECTORY));
        else if (status !== STATUS_OK) throw errorFor(status);
        else if (Number(found[1]) !== KIND_DIRECTORY) throw errorFor(STATUS_TYPE);
        return new FileSystemDirectoryHandle(token, path);
      });
    }
    removeEntry(name, options = {}) {
      return task(() => {
        const state = requireHandle(this, KIND_DIRECTORY),
          entryName = requireName(name), path = joinPath(state.path, entryName);
        checked(command(OP_REMOVE, path, !!options?.recursive));
      });
    }
    resolve(possibleDescendant) {
      return task(() => {
        const state = requireHandle(this, KIND_DIRECTORY),
          descendant = handleStates.get(possibleDescendant);
        if (!descendant) throw new TypeError("Expected a FileSystemHandle");
        if (descendant.path === state.path) return [];
        const prefix = state.path === "/" ? "/" : state.path + "/";
        if (!descendant.path.startsWith(prefix)) return null;
        return descendant.path.slice(prefix.length).split("/");
      });
    }
    entries() {
      const state = requireHandle(this, KIND_DIRECTORY);
      let at = 0, retained = null;
      return {
        next: () => task(() => {
          if (retained === null) {
            const rows = checked(command(OP_LIST, state.path));
            retained = [];
            for (let row = 1; row + 1 < rows.length; row += 2) {
              const name = String(rows[row]), kind = Number(rows[row + 1]);
              retained.push([name, kind === KIND_FILE
                ? new FileSystemFileHandle(token, joinPath(state.path, name))
                : new FileSystemDirectoryHandle(
                  token, joinPath(state.path, name))]);
            }
          }
          return at < retained.length
            ? { value: retained[at++], done: false }
            : { value: undefined, done: true };
        }),
        [Symbol.asyncIterator]() { return this; },
      };
    }
    keys() {
      const iterator = this.entries();
      return {
        next: () => trustedPromiseThen(iterator.next(), result =>
          result.done ? result : { value: result.value[0], done: false }),
        [Symbol.asyncIterator]() { return this; },
      };
    }
    values() {
      const iterator = this.entries();
      return {
        next: () => trustedPromiseThen(iterator.next(), result =>
          result.done ? result : { value: result.value[1], done: false }),
        [Symbol.asyncIterator]() { return this; },
      };
    }
    [Symbol.asyncIterator]() { return this.entries(); }
  }

  class FileSystemWritableFileStream extends WritableStream {
    constructor(value, path, bytes, generation) {
      if (value !== token) throw new TypeError("Illegal constructor");
      let stream = null;
      super({
        write(chunk) {
          return task(() => writerWrite(writerState(stream), chunk));
        },
        close() {
          return task(() => {
            const state = writerState(stream);
            try {
              checked(command(
                OP_WRITE, state.path,
                state.bytes.subarray(0, state.length), state.generation));
            } finally { releaseWriter(state); }
          });
        },
        abort() {
          releaseWriter(writerState(stream));
        },
      });
      stream = this;
      writerStates.set(this, {
        path, bytes, length: bytes.byteLength,
        generation, position: 0, closed: false,
      });
    }
    write(value) {
      if (this.locked)
        return trustedPromiseReject(new TypeError("stream is locked"));
      try { writerState(this); } catch (error) {
        return trustedPromiseReject(error);
      }
      return this._write(value);
    }
    seek(position) {
      return this.write({ type: "seek", position });
    }
    truncate(size) {
      return this.write({ type: "truncate", size });
    }
  }

  class FileSystemSyncAccessHandle {
    constructor(value, path, bytes, generation) {
      if (value !== token) throw new TypeError("Illegal constructor");
      syncStates.set(this, {
        path, bytes, length: bytes.byteLength,
        generation, position: 0, closed: false, dirty: false,
      });
    }
    read(buffer, options = {}) {
      const state = syncState(this), target = mutableBytesFor(buffer),
        at = syncOffset(state, options);
      if (at >= state.length || target.byteLength === 0) {
        state.position = Math.min(at, state.length);
        return 0;
      }
      const count = Math.min(target.byteLength, state.length - at);
      target.set(state.bytes.subarray(at, at + count));
      state.position = at + count;
      return count;
    }
    write(buffer, options = {}) {
      const state = syncState(this), bytes = mutableBytesFor(buffer),
        at = syncOffset(state, options), end = at + bytes.byteLength;
      if (!Number.isSafeInteger(end) || end > FILE_LIMIT)
        throw new DOMException("File exceeds bounded size", "QuotaExceededError");
      if (end > state.length) resize(state, end);
      state.bytes.set(bytes, at);
      state.position = end;
      state.dirty = state.dirty || bytes.byteLength > 0;
      return bytes.byteLength;
    }
    truncate(size) {
      const state = syncState(this), before = state.length;
      resize(state, size);
      state.position = Math.min(state.position, state.length);
      state.dirty = state.dirty || state.length !== before;
    }
    getSize() { return syncState(this).length; }
    flush() {
      const state = syncState(this);
      if (!state.dirty) return;
      const result = checked(command(
        OP_WRITE, state.path,
        state.bytes.subarray(0, state.length), state.generation));
      const generation = Number(result[1]);
      if (!Number.isSafeInteger(generation) || generation <= 0)
        throw new DOMException(
          "File generation is unavailable", "InvalidStateError");
      state.generation = generation;
      state.dirty = false;
    }
    close() {
      const state = syncStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (state.closed) return;
      try { this.flush(); } finally { releaseWriter(state); }
    }
    _abort() {
      const state = syncStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      if (!state.closed) releaseWriter(state);
    }
  }

  for (const [constructor, tag, methods] of [
    [FileSystemHandle, "FileSystemHandle", ["isSameEntry", "queryPermission", "requestPermission"]],
    [FileSystemFileHandle, "FileSystemFileHandle", ["getFile", "createWritable", "createSyncAccessHandle"]],
    [FileSystemDirectoryHandle, "FileSystemDirectoryHandle", ["getFileHandle", "getDirectoryHandle", "removeEntry", "resolve", "entries", "keys", "values"]],
    [FileSystemWritableFileStream, "FileSystemWritableFileStream", ["write", "seek", "truncate"]],
    [FileSystemSyncAccessHandle, "FileSystemSyncAccessHandle", ["read", "write", "truncate", "getSize", "flush", "close"]],
  ]) {
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, {
      configurable: true, value: tag,
    });
    for (const name of methods) Object.defineProperty(
      constructor.prototype, name, {
        ...Object.getOwnPropertyDescriptor(constructor.prototype, name),
        enumerable: true,
      });
  }

  globalThis.FileSystemHandle = FileSystemHandle;
  globalThis.FileSystemFileHandle = FileSystemFileHandle;
  globalThis.FileSystemDirectoryHandle = FileSystemDirectoryHandle;
  globalThis.FileSystemWritableFileStream = FileSystemWritableFileStream;
  globalThis.FileSystemSyncAccessHandle = FileSystemSyncAccessHandle;
  Object.defineProperty(StorageManager.prototype, "getDirectory", {
    configurable: true, enumerable: true, writable: true,
    value: function getDirectory() {
      /* Brand against the platform object captured at installation.  Looking
         through the author-replaceable navigator property here could reject
         the authentic receiver or invoke an unrelated author getter. */
      if (this !== storageManager)
        return trustedPromiseReject(new TypeError("Illegal invocation"));
      return task(() => {
        checked(command(OP_STAT, "/"));
        return new FileSystemDirectoryHandle(token, "/");
      });
    },
  });
})();
