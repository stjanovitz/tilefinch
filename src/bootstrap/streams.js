(() => {
  const streamQueueLimit = 64,
    streamByteLimit = 256 * 1024,
    compressionByteLimit = 1024 * 1024,
    compressionNative = globalThis.__tilefinchCompressionRun,
    largeStreamEndpoints = new WeakSet(),
    readableStreamStates = new WeakMap(),
    readableState = (stream) => {
      const state = readableStreamStates.get(stream);
      if (!state) throw new TypeError("invalid readable stream");
      return state;
    },
    chunkBytes = (value) => {
      if (value instanceof ArrayBuffer) return value.byteLength;
      if (ArrayBuffer.isView(value)) return value.byteLength;
      if (typeof value === "string")
        return Math.min(streamByteLimit + 1, value.length * 2);
      return 64;
    },
    closeReadableStream = (stream) => {
      const state = readableState(stream);
      if (state.status !== "readable") return;
      state.status = "closed";
      while (state.reads.length)
        state.reads.shift().resolve({ done: true, value: undefined });
      state.closeResolve();
    },
    failReadableStream = (stream, reason) => {
      const state = readableState(stream);
      if (state.status !== "readable") return;
      state.status = "errored";
      state.error = reason;
      state.queue.length = 0;
      state.queueBytes = 0;
      while (state.reads.length) state.reads.shift().reject(reason);
      state.closeReject(reason);
    },
    requestReadableStreamPull = (stream) => {
      const state = readableState(stream);
      if (
        state.status !== "readable" ||
        state.pulling ||
        typeof state.source.pull !== "function" ||
        (!state.reads.length && state.queue.length)
      )
        return;
      state.pulling = true;
      try {
        Promise.resolve(state.source.pull(state.controller)).then(
          () => {
            state.pulling = false;
          },
          (reason) => {
            state.pulling = false;
            failReadableStream(stream, reason);
          },
        );
      } catch (reason) {
        state.pulling = false;
        failReadableStream(stream, reason);
      }
    },
    readReadableStream = (stream) => {
      const state = readableState(stream);
      if (state.queue.length) {
        const entry = state.queue.shift();
        state.queueBytes -= entry.bytes;
        requestReadableStreamPull(stream);
        return Promise.resolve({ done: false, value: entry.value });
      }
      if (state.status === "closed")
        return Promise.resolve({ done: true, value: undefined });
      if (state.status === "errored") return Promise.reject(state.error);
      if (state.reads.length >= streamQueueLimit)
        return Promise.reject(new RangeError("pending read limit exceeded"));
      const promise = new Promise((resolve, reject) => {
        state.reads.push({ resolve, reject });
      });
      requestReadableStreamPull(stream);
      return promise;
    },
    cancelReadableStream = (stream, reason) => {
      const state = readableState(stream);
      if (state.status === "closed") return Promise.resolve();
      if (state.status === "errored") return Promise.reject(state.error);
      state.queue.length = 0;
      state.queueBytes = 0;
      closeReadableStream(stream);
      try {
        return Promise.resolve(
          typeof state.source.cancel === "function"
            ? state.source.cancel(reason)
            : undefined,
        );
      } catch (error) {
        return Promise.reject(error);
      }
    };
  class ReadableStreamDefaultReader {
    constructor(stream) {
      if (!(stream instanceof ReadableStream))
        throw new TypeError("invalid readable stream");
      if (stream.locked) throw new TypeError("stream is locked");
      this._stream = stream;
      const state = readableState(stream);
      state.locked = true;
      this.closed = state.closedPromise;
    }
    read() {
      if (!this._stream)
        return Promise.reject(new TypeError("reader has no stream"));
      return readReadableStream(this._stream);
    }
    cancel(reason) {
      if (!this._stream)
        return Promise.reject(new TypeError("reader has no stream"));
      return cancelReadableStream(this._stream, reason);
    }
    releaseLock() {
      if (!this._stream) return;
      const state = readableState(this._stream);
      if (state.reads.length) {
        const error = new TypeError("reader lock released");
        for (const read of state.reads.splice(0)) read.reject(error);
      }
      state.locked = false;
      this._stream = null;
    }
  }
  class ReadableStream {
    constructor(source = {}) {
      let closeResolve, closeReject;
      const closedPromise = new Promise((resolve, reject) => {
        closeResolve = resolve;
        closeReject = reject;
      });
      const stream = this,
        state = {
          closeReject,
          closeResolve,
          closedPromise,
          controller: null,
          error: null,
          locked: false,
          pulling: false,
          queue: [],
          queueBytes: 0,
          reads: [],
          source: source || {},
          status: "readable",
        };
      readableStreamStates.set(this, state);
      state.controller = {
        get desiredSize() {
          const limit = largeStreamEndpoints.has(state.source)
            ? compressionByteLimit : streamByteLimit;
          return limit - state.queueBytes;
        },
        enqueue(value) {
          if (state.status !== "readable")
            throw new TypeError("stream is not readable");
          const bytes = chunkBytes(value);
          if (state.reads.length) {
            const read = state.reads.shift();
            read.resolve({ done: false, value });
            return;
          }
          if (
            state.queue.length >= streamQueueLimit ||
            state.queueBytes + bytes >
              (largeStreamEndpoints.has(state.source)
                ? compressionByteLimit : streamByteLimit)
          )
            throw new RangeError("stream queue limit exceeded");
          state.queue.push({ value, bytes });
          state.queueBytes += bytes;
        },
        close() {
          closeReadableStream(stream);
        },
        error(reason) {
          failReadableStream(stream, reason);
        },
      };
      try {
        const started =
          typeof state.source.start === "function"
            ? state.source.start(state.controller)
            : undefined;
        Promise.resolve(started).then(
          () => requestReadableStreamPull(this),
          (reason) => failReadableStream(this, reason),
        );
        if (
          typeof state.source.start !== "function" &&
          typeof state.source.pull !== "function"
        )
          closeReadableStream(this);
      } catch (reason) {
        failReadableStream(this, reason);
      }
    }
    get locked() {
      return readableState(this).locked;
    }
    pipeThrough(transform, options) {
      if (!transform || !transform.readable || !transform.writable)
        throw new TypeError("invalid transform");
      this.pipeTo(transform.writable, options).catch(() => {});
      return transform.readable;
    }
    pipeTo(destination, options = {}) {
      if (!destination || typeof destination.getWriter !== "function")
        return Promise.reject(new TypeError("invalid destination"));
      const writer = destination.getWriter(),
        reader = this.getReader(),
        signal = options.signal;
      return (async () => {
        try {
          for (;;) {
            if (signal?.aborted) throw signal.reason;
            const { done, value } = await reader.read();
            if (done) break;
            await writer.write(value);
          }
          if (!options.preventClose) await writer.close();
        } catch (error) {
          if (!options.preventAbort) await writer.abort(error);
          if (!options.preventCancel) await reader.cancel(error);
          throw error;
        } finally {
          try {
            reader.releaseLock();
          } catch (_) {}
          writer.releaseLock();
        }
      })();
    }
    getReader(options = {}) {
      if (options && options.mode !== undefined)
        throw new RangeError("BYOB readers are not supported");
      return new ReadableStreamDefaultReader(this);
    }
    cancel(reason) {
      if (this.locked)
        return Promise.reject(new TypeError("stream is locked"));
      return cancelReadableStream(this, reason);
    }
    tee() {
      if (this.locked) throw new TypeError("stream is locked");
      const reader = this.getReader();
      let left, right;
      const pump = async () => {
        const result = await reader.read();
        if (result.done) {
          left.close();
          right.close();
        } else {
          left.enqueue(result.value);
          right.enqueue(result.value);
        }
      };
      return [
        new ReadableStream({
          start(controller) {
            left = controller;
          },
          pull: pump,
        }),
        new ReadableStream({
          start(controller) {
            right = controller;
          },
          pull: pump,
        }),
      ];
    }
    values(options = {}) {
      const reader = this.getReader();
      return {
        async next() {
          const result = await reader.read();
          if (result.done) reader.releaseLock();
          return result;
        },
        async return() {
          if (!options.preventCancel) await reader.cancel();
          reader.releaseLock();
          return { done: true, value: undefined };
        },
        [Symbol.asyncIterator]() {
          return this;
        },
      };
    }
    [Symbol.asyncIterator]() {
      return this.values();
    }
  }
  class WritableStreamDefaultWriter {
    constructor(stream) {
      if (!(stream instanceof WritableStream))
        throw new TypeError("invalid writable stream");
      if (stream.locked) throw new TypeError("stream is locked");
      this._stream = stream;
      stream.locked = true;
      this.ready = Promise.resolve();
      this.closed = stream._closedPromise;
    }
    write(value) {
      if (!this._stream)
        return Promise.reject(new TypeError("writer has no stream"));
      return this._stream._write(value);
    }
    close() {
      if (!this._stream)
        return Promise.reject(new TypeError("writer has no stream"));
      return this._stream._close();
    }
    abort(reason) {
      if (!this._stream)
        return Promise.reject(new TypeError("writer has no stream"));
      return this._stream._abort(reason);
    }
    releaseLock() {
      if (!this._stream) return;
      this._stream.locked = false;
      this._stream = null;
    }
  }
  class WritableStream {
    constructor(sink = {}) {
      this._sink = sink || {};
      this._state = "writable";
      this._queuedCount = 0;
      this._queuedBytes = 0;
      this._chain = Promise.resolve();
      this.locked = false;
      let closeResolve, closeReject;
      this._closedPromise = new Promise((resolve, reject) => {
        closeResolve = resolve;
        closeReject = reject;
      });
      this._closeResolve = closeResolve;
      this._closeReject = closeReject;
      try {
        if (typeof this._sink.start === "function")
          this._chain = Promise.resolve(this._sink.start(this));
      } catch (reason) {
        this._state = "errored";
        this._chain = Promise.reject(reason);
        closeReject(reason);
      }
    }
    _write(value) {
      if (this._state !== "writable")
        return Promise.reject(new TypeError("stream is not writable"));
      const bytes = chunkBytes(value);
      if (
        this._queuedCount >= streamQueueLimit ||
        this._queuedBytes + bytes >
          (largeStreamEndpoints.has(this._sink)
            ? compressionByteLimit : streamByteLimit)
      )
        return Promise.reject(new RangeError("write queue limit exceeded"));
      this._queuedCount++;
      this._queuedBytes += bytes;
      const operation = this._chain.then(() =>
        typeof this._sink.write === "function"
          ? this._sink.write(value, this)
          : undefined,
      );
      this._chain = operation.then(
        () => {
          this._queuedCount--;
          this._queuedBytes -= bytes;
        },
        (reason) => {
          this._queuedCount--;
          this._queuedBytes -= bytes;
          this._state = "errored";
          this._closeReject(reason);
          throw reason;
        },
      );
      return operation;
    }
    _close() {
      if (this._state !== "writable")
        return Promise.reject(new TypeError("stream is not writable"));
      this._state = "closing";
      const operation = this._chain.then(() =>
        typeof this._sink.close === "function"
          ? this._sink.close()
          : undefined,
      );
      this._chain = operation.then(
        () => {
          this._state = "closed";
          this._closeResolve();
        },
        (reason) => {
          this._state = "errored";
          this._closeReject(reason);
          throw reason;
        },
      );
      return operation;
    }
    _abort(reason) {
      if (this._state === "closed") return Promise.resolve();
      this._state = "errored";
      this._closeReject(reason);
      try {
        return Promise.resolve(
          typeof this._sink.abort === "function"
            ? this._sink.abort(reason)
            : undefined,
        );
      } catch (error) {
        return Promise.reject(error);
      }
    }
    getWriter() {
      return new WritableStreamDefaultWriter(this);
    }
    abort(reason) {
      if (this.locked)
        return Promise.reject(new TypeError("stream is locked"));
      return this._abort(reason);
    }
    close() {
      if (this.locked)
        return Promise.reject(new TypeError("stream is locked"));
      return this._close();
    }
  }
  class TransformStream {
    constructor(transformer = {}) {
      let readableController;
      const source = {
        start(controller) {
          readableController = controller;
          if (typeof transformer.start === "function")
            return transformer.start(controller);
        },
      }, sink = {
        write(value) {
          if (typeof transformer.transform === "function")
            return transformer.transform(value, readableController);
          readableController.enqueue(value);
        },
        close() {
          return Promise.resolve(
            typeof transformer.flush === "function"
              ? transformer.flush(readableController)
              : undefined,
          ).then(() => readableController.close());
        },
        abort(reason) {
          readableController.error(reason);
        },
      };
      if (largeStreamEndpoints.has(transformer)) {
        largeStreamEndpoints.add(source);
        largeStreamEndpoints.add(sink);
      }
      this.readable = new ReadableStream(source);
      this.writable = new WritableStream(sink);
    }
  }
  Object.defineProperty(ReadableStream.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "ReadableStream",
  });
  globalThis.ReadableStream = ReadableStream;
  globalThis.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
  globalThis.WritableStream = WritableStream;
  globalThis.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
  globalThis.TransformStream = TransformStream;
  globalThis.TextEncoderStream = class TextEncoderStream extends TransformStream {
    constructor() {
      super({
        transform(value, controller) {
          controller.enqueue(new TextEncoder().encode(String(value)));
        },
      });
      this.encoding = "utf-8";
    }
  };
  globalThis.TextDecoderStream = class TextDecoderStream extends TransformStream {
    constructor(label = "utf-8", options = {}) {
      const decoder = new TextDecoder(label, options);
      super({
        transform(value, controller) {
          controller.enqueue(decoder.decode(value, { stream: true }));
        },
        flush(controller) {
          const tail = decoder.decode();
          if (tail) controller.enqueue(tail);
        },
      });
      this.encoding = decoder.encoding;
      this.fatal = decoder.fatal;
      this.ignoreBOM = decoder.ignoreBOM;
    }
  };
  const compressionStates = new WeakMap(),
    compressionChunk = (value) => {
      let source;
      if (value instanceof ArrayBuffer) {
        source = new Uint8Array(value);
      } else if (ArrayBuffer.isView(value)) {
        source = new Uint8Array(
          value.buffer, value.byteOffset, value.byteLength);
      } else {
        throw new TypeError("compression input must be a BufferSource");
      }
      const copy = new Uint8Array(source.byteLength);
      copy.set(source);
      return copy;
    },
    compressionFormat = (value) => {
      const format = String(value);
      if (format !== "deflate" && format !== "deflate-raw" &&
          format !== "gzip")
        throw new TypeError("unsupported compression format");
      return format;
    },
    compressionTransform = (format, decompress) => {
      const chunks = [];
      let bytes = 0;
      const transformer = {
        transform(value) {
          const chunk = compressionChunk(value);
          if (chunks.length >= streamQueueLimit ||
              chunk.byteLength > compressionByteLimit - bytes)
            throw new RangeError("compression input exceeds bounded size");
          chunks.push(chunk);
          bytes += chunk.byteLength;
        },
        flush(controller) {
          let input;
          if (chunks.length === 1) {
            input = chunks[0];
          } else {
            input = new Uint8Array(bytes);
            let offset = 0;
            for (const chunk of chunks) {
              input.set(chunk, offset);
              offset += chunk.byteLength;
            }
          }
          chunks.length = 0;
          bytes = 0;
          const output = compressionNative(decompress ? 1 : 0, format, input);
          controller.enqueue(new Uint8Array(output));
        },
      };
      largeStreamEndpoints.add(transformer);
      return new TransformStream(transformer);
    };
  class CompressionStream {
    constructor(format) {
      if (typeof compressionNative !== "function")
        throw new TypeError("CompressionStream is unavailable");
      const transform = compressionTransform(
        compressionFormat(format), false);
      compressionStates.set(this, transform);
    }
    get readable() {
      const state = compressionStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.readable;
    }
    get writable() {
      const state = compressionStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.writable;
    }
  }
  class DecompressionStream {
    constructor(format) {
      if (typeof compressionNative !== "function")
        throw new TypeError("DecompressionStream is unavailable");
      const transform = compressionTransform(
        compressionFormat(format), true);
      compressionStates.set(this, transform);
    }
    get readable() {
      const state = compressionStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.readable;
    }
    get writable() {
      const state = compressionStates.get(this);
      if (!state) throw new TypeError("Illegal invocation");
      return state.writable;
    }
  }
  Object.defineProperty(CompressionStream.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "CompressionStream",
  });
  Object.defineProperty(DecompressionStream.prototype, Symbol.toStringTag, {
    configurable: true,
    value: "DecompressionStream",
  });
  if (typeof compressionNative === "function") {
    globalThis.CompressionStream = CompressionStream;
    globalThis.DecompressionStream = DecompressionStream;
  }
  /* This module is lazy and therefore runs after hardening.js has taken its
     eager-interface snapshot. Mark each standards-visible interface here so
     function reflection matches native browser interfaces without forcing the
     Streams module into every page realm during startup. */
  const markNative = globalThis.__tilefinchMarkNativeFunction;
  const nativeStreamConstructors = [
    ReadableStream,
    ReadableStreamDefaultReader,
    WritableStream,
    WritableStreamDefaultWriter,
    TransformStream,
    globalThis.TextEncoderStream,
    globalThis.TextDecoderStream,
  ];
  if (typeof compressionNative === "function")
    nativeStreamConstructors.push(CompressionStream, DecompressionStream);
  for (const constructor of nativeStreamConstructors) {
    markNative(constructor);
    for (const key of Reflect.ownKeys(constructor.prototype)) {
      const descriptor = Object.getOwnPropertyDescriptor(
        constructor.prototype,
        key,
      );
      markNative(descriptor?.value);
      markNative(descriptor?.get);
      markNative(descriptor?.set);
    }
  }
})();
