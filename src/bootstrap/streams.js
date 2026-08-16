(() => {
  const streamQueueLimit = 64,
    streamByteLimit = 256 * 1024,
    chunkBytes = (value) => {
      if (value instanceof ArrayBuffer) return value.byteLength;
      if (ArrayBuffer.isView(value)) return value.byteLength;
      if (typeof value === "string")
        return Math.min(streamByteLimit + 1, value.length * 2);
      return 64;
    };
  class ReadableStreamDefaultReader {
    constructor(stream) {
      if (!(stream instanceof ReadableStream))
        throw new TypeError("invalid readable stream");
      if (stream.locked) throw new TypeError("stream is locked");
      this._stream = stream;
      stream.locked = true;
      this.closed = stream._closedPromise;
    }
    read() {
      if (!this._stream)
        return Promise.reject(new TypeError("reader has no stream"));
      return this._stream._read();
    }
    cancel(reason) {
      if (!this._stream)
        return Promise.reject(new TypeError("reader has no stream"));
      return this._stream._cancel(reason);
    }
    releaseLock() {
      if (!this._stream) return;
      if (this._stream._reads.length)
        throw new TypeError("reader has pending reads");
      this._stream.locked = false;
      this._stream = null;
    }
  }
  class ReadableStream {
    constructor(source = {}) {
      this._queue = [];
      this._queueBytes = 0;
      this._reads = [];
      this._state = "readable";
      this._error = null;
      this._source = source || {};
      this._pulling = false;
      this.locked = false;
      let closeResolve, closeReject;
      this._closedPromise = new Promise((resolve, reject) => {
        closeResolve = resolve;
        closeReject = reject;
      });
      this._closeResolve = closeResolve;
      this._closeReject = closeReject;
      const stream = this;
      this._controller = {
        get desiredSize() {
          return streamByteLimit - stream._queueBytes;
        },
        enqueue(value) {
          if (stream._state !== "readable")
            throw new TypeError("stream is not readable");
          const bytes = chunkBytes(value);
          if (stream._reads.length) {
            const read = stream._reads.shift();
            read.resolve({ done: false, value });
            return;
          }
          if (
            stream._queue.length >= streamQueueLimit ||
            stream._queueBytes + bytes > streamByteLimit
          )
            throw new RangeError("stream queue limit exceeded");
          stream._queue.push({ value, bytes });
          stream._queueBytes += bytes;
        },
        close() {
          stream._close();
        },
        error(reason) {
          stream._fail(reason);
        },
      };
      try {
        const started =
          typeof this._source.start === "function"
            ? this._source.start(this._controller)
            : undefined;
        Promise.resolve(started).then(
          () => this._requestPull(),
          (reason) => this._fail(reason),
        );
        if (
          typeof this._source.start !== "function" &&
          typeof this._source.pull !== "function"
        )
          this._close();
      } catch (reason) {
        this._fail(reason);
      }
    }
    _close() {
      if (this._state !== "readable") return;
      this._state = "closed";
      while (this._reads.length)
        this._reads.shift().resolve({ done: true, value: undefined });
      this._closeResolve();
    }
    _fail(reason) {
      if (this._state !== "readable") return;
      this._state = "errored";
      this._error = reason;
      this._queue = [];
      this._queueBytes = 0;
      while (this._reads.length) this._reads.shift().reject(reason);
      this._closeReject(reason);
    }
    _requestPull() {
      if (
        this._state !== "readable" ||
        this._pulling ||
        typeof this._source.pull !== "function" ||
        (!this._reads.length && this._queue.length)
      )
        return;
      this._pulling = true;
      try {
        Promise.resolve(this._source.pull(this._controller)).then(
          () => {
            this._pulling = false;
          },
          (reason) => {
            this._pulling = false;
            this._fail(reason);
          },
        );
      } catch (reason) {
        this._pulling = false;
        this._fail(reason);
      }
    }
    _read() {
      if (this._queue.length) {
        const entry = this._queue.shift();
        this._queueBytes -= entry.bytes;
        this._requestPull();
        return Promise.resolve({ done: false, value: entry.value });
      }
      if (this._state === "closed")
        return Promise.resolve({ done: true, value: undefined });
      if (this._state === "errored") return Promise.reject(this._error);
      if (this._reads.length >= streamQueueLimit)
        return Promise.reject(new RangeError("pending read limit exceeded"));
      const promise = new Promise((resolve, reject) => {
        this._reads.push({ resolve, reject });
      });
      this._requestPull();
      return promise;
    }
    _cancel(reason) {
      if (this._state === "closed") return Promise.resolve();
      if (this._state === "errored") return Promise.reject(this._error);
      this._queue = [];
      this._queueBytes = 0;
      this._close();
      try {
        return Promise.resolve(
          typeof this._source.cancel === "function"
            ? this._source.cancel(reason)
            : undefined,
        );
      } catch (error) {
        return Promise.reject(error);
      }
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
      return this._cancel(reason);
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
        this._queuedBytes + bytes > streamByteLimit
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
      this.readable = new ReadableStream({
        start(controller) {
          readableController = controller;
          if (typeof transformer.start === "function")
            return transformer.start(controller);
        },
      });
      this.writable = new WritableStream({
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
      });
    }
  }
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
})();
