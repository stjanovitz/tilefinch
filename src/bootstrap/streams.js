(() => {
  let readableState;
  const streamQueueLimit = 64,
    streamByteLimit = 256 * 1024,
    compressionByteLimit = 1024 * 1024,
    compressionNative = globalThis.__tilefinchCompressionRun,
    heapSnapshot = globalThis.__tilefinchHeapMemorySnapshot,
    largeStreamEndpoints = new WeakSet(),
    handledRejection = (reason) => {
      const promise = Promise.reject(reason);
      promise.catch(() => {});
      return promise;
    },
    initializeStreamOwner = (owner, status, error) => {
      owner._closeResolve = owner._closeReject = null;
      if (status === "closed") owner.closed = Promise.resolve();
      else if (status === "errored") owner.closed = handledRejection(error);
      else {
        owner.closed = new Promise((resolve, reject) => {
          owner._closeResolve = resolve;
          owner._closeReject = reject;
        });
        owner.closed.catch(() => {});
      }
    },
    finishStreamOwner = (owner, error, failed = false) => {
      if (!owner?._closeReject) return;
      const settle = failed ? owner._closeReject : owner._closeResolve;
      owner._closeResolve = owner._closeReject = null;
      settle(error);
    },
    releaseStreamOwner = (owner, error) => {
      /* Completion belongs to this lock, not to a later reader/writer.
         Do not attach another reaction to a stream-wide pending promise. */
      if (owner._closeReject) finishStreamOwner(owner, error, true);
      else owner.closed = handledRejection(error);
    },
    chunkBytes = (value) => {
      if (value instanceof ArrayBuffer) return value.byteLength;
      if (ArrayBuffer.isView(value)) return value.byteLength;
      if (typeof value === "string") return value.length * 2;
      return 64;
    },
    /* A queue may grow past its high-water mark, as the Streams Standard
       allows (desiredSize goes negative and pulls stop); the realm's memory
       is what bounds it. Each further mark's worth of queued bytes checks
       that the realm would still keep a mark's worth of heap free. */
    queueFits = (queued, bytes, mark) => {
      const next = queued + bytes;
      if (next <= mark || Math.floor(next / mark) === Math.floor(queued / mark))
        return true;
      const values = heapSnapshot?.();
      return values ? values[0] - values[1] - bytes >= mark
        : next <= mark * 16;
    },
    closeReadableStream = (stream) => {
      const state = readableState(stream);
      if (state.status !== "readable") return;
      state.status = "closed";
      while (state.reads.length)
        state.reads.shift().resolve({ done: true, value: undefined });
      finishStreamOwner(state.reader);
    },
    failReadableStream = (stream, reason) => {
      const state = readableState(stream);
      if (state.status !== "readable") return;
      state.status = "errored";
      state.error = reason;
      state.queue.length = 0;
      state.queueBytes = 0;
      state.queueSize = 0;
      while (state.reads.length) state.reads.shift().reject(reason);
      finishStreamOwner(state.reader, reason, true);
    },
    requestReadableStreamPull = (stream) => {
      const state = readableState(stream);
      if (
        !state.started || state.closeRequested || state.status !== "readable" ||
        typeof state.source.pull !== "function" ||
        (!state.reads.length && state.highWaterMark - state.queueSize <= 0)
      )
        return;
      if (state.pulling) {
        state.pullAgain = true;
        return;
      }
      state.pulling = true;
      try {
        Promise.resolve(state.source.pull(state.controller)).then(
          () => {
            state.pulling = false;
            if (state.pullAgain) {
              state.pullAgain = false;
              requestReadableStreamPull(stream);
            }
          },
          (reason) => {
            state.pulling = false;
            state.pullAgain = false;
            failReadableStream(stream, reason);
          },
        );
      } catch (reason) {
        state.pulling = false;
        state.pullAgain = false;
        failReadableStream(stream, reason);
      }
    },
    readReadableStream = (stream) => {
      const state = readableState(stream);
      /* A body becomes disturbed when a read is attempted, not merely when a
         reader locks it. Fetch exposes that distinction through bodyUsed. */
      state.disturbed = true;
      if (state.status === "closed")
        return Promise.resolve({ done: true, value: undefined });
      if (state.status === "errored") return Promise.reject(state.error);
      if (state.queue.length) {
        const entry = state.queue.shift();
        state.queueBytes -= entry.bytes;
        state.queueSize = state.queue.length ? state.queueSize - entry.size : 0;
        if (state.closeRequested && !state.queue.length)
          closeReadableStream(stream);
        else requestReadableStreamPull(stream);
        return Promise.resolve({ done: false, value: entry.value });
      }
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
      state.disturbed = true;
      if (state.status === "closed") return Promise.resolve();
      if (state.status === "errored") return Promise.reject(state.error);
      state.queue.length = 0;
      state.queueBytes = 0;
      state.queueSize = 0;
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
      const state = readableState(stream);
      initializeStreamOwner(this, state.status, state.error);
      this._stream = stream;
      state.reader = this;
      state.locked = true;
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
      const error = new TypeError("reader lock released");
      releaseStreamOwner(this, error);
      if (state.reads.length) {
        for (const read of state.reads.splice(0)) read.reject(error);
      }
      state.reader = null;
      state.locked = false;
      this._stream = null;
    }
  }
  class ReadableStream {
    #state;
    static {
      readableState = (stream) => stream.#state;
    }
    constructor(source = {}, strategy = {}) {
      /* The queuing strategy: by default one chunk, as in the Streams
         Standard. desiredSize is the mark less the queued size; memory is
         accounted separately in queueBytes (queueFits). */
      const mark = strategy?.highWaterMark === undefined ? 1
          : Number(strategy.highWaterMark),
        size = strategy?.size;
      if (Number.isNaN(mark) || mark < 0)
        throw new RangeError("invalid highWaterMark");
      if (size !== undefined && typeof size !== "function")
        throw new TypeError("strategy size must be a function");
      const stream = this,
        state = {
          reader: null,
          controller: null,
          closeRequested: false,
          disturbed: false,
          error: null,
          highWaterMark: mark,
          locked: false,
          pullAgain: false,
          pulling: false,
          queue: [],
          queueBytes: 0,
          queueSize: 0,
          reads: [],
          source: source || {},
          started: false,
          status: "readable",
        };
      this.#state = state;
      state.controller = {
        get desiredSize() {
          return state.status === "errored" ? null
            : state.status === "closed" ? 0
            : state.highWaterMark - state.queueSize;
        },
        enqueue(value) {
          if (state.status !== "readable" || state.closeRequested)
            throw new TypeError("stream is not readable");
          const bytes = chunkBytes(value);
          if (state.reads.length) {
            const read = state.reads.shift();
            read.resolve({ done: false, value });
            requestReadableStreamPull(stream);
            return;
          }
          let chunkSize = 1;
          if (size !== undefined) {
            try {
              chunkSize = Number(size(value));
              if (!(chunkSize >= 0) || chunkSize === Infinity)
                throw new RangeError("invalid chunk size");
            } catch (reason) {
              failReadableStream(stream, reason);
              throw reason;
            }
          }
          if (!queueFits(state.queueBytes, bytes,
              largeStreamEndpoints.has(state.source)
                ? compressionByteLimit : streamByteLimit)) {
            const error = new RangeError("stream queue exceeds available memory");
            failReadableStream(stream, error);
            throw error;
          }
          state.queue.push({ value, bytes, size: chunkSize });
          state.queueBytes += bytes;
          state.queueSize += chunkSize;
          requestReadableStreamPull(stream);
        },
        close() {
          if (state.status !== "readable" || state.closeRequested)
            throw new TypeError("stream cannot close");
          state.closeRequested = true;
          if (!state.queue.length) closeReadableStream(stream);
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
          () => {
            state.started = true;
            requestReadableStreamPull(this);
          },
          (reason) => failReadableStream(this, reason),
        );
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
    async pipeTo(destination, options = {}) {
      if (!(destination instanceof WritableStream))
        throw new TypeError("invalid destination");
      const signal = options.signal;
      if (signal !== undefined && !(signal instanceof AbortSignal))
        throw new TypeError("invalid abort signal");
      const preventClose = !!options.preventClose,
        preventAbort = !!options.preventAbort,
        preventCancel = !!options.preventCancel;
      /* Admission is transactional: reject before taking either lock. */
      if (readableState(this).locked || destination.locked)
        throw new TypeError("stream is locked");
      const writer = new WritableStreamDefaultWriter(destination);
      let reader;
      try {
        reader = new ReadableStreamDefaultReader(this);
      } catch (error) {
        writer.releaseLock();
        throw error;
      }
      let abortListener = null, failure = null, rejectRead = null,
        finishing = false;
      const stop = (error) => {
        if (finishing || failure) return;
        failure = { error };
        if (rejectRead) rejectRead(error);
      };
      /* One subscription per endpoint, not a growing Promise.race chain
         attached to the same pending terminal promise for every chunk. */
      writer.closed.then(
        () => stop(new TypeError("destination closed before source")), stop);
      reader.closed.catch(stop);
      if (signal) {
        abortListener = () => stop(signal.reason);
        globalThis.__tilefinchAddAbortAlgorithm(signal, abortListener);
      }
      try {
        for (;;) {
          if (signal?.aborted) stop(signal.reason);
          if (failure) throw failure.error;
          const { done, value } = await new Promise((resolve, reject) => {
            rejectRead = reject;
            reader.read().then(resolve, reject);
          });
          rejectRead = null;
          if (failure) throw failure.error;
          if (done) break;
          await writer.write(value);
        }
        finishing = true;
        if (!preventClose) await writer.close();
      } catch (error) {
        finishing = true;
        /* A failed abort must not skip the other endpoint's cancellation. */
        const actions = [];
        if (!preventAbort) actions.push(writer.abort(error));
        if (!preventCancel) actions.push(reader.cancel(error));
        await Promise.all(actions);
        throw error;
      } finally {
        rejectRead = null;
        if (abortListener)
          globalThis.__tilefinchRemoveAbortAlgorithm(signal, abortListener);
        reader.releaseLock();
        writer.releaseLock();
      }
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
      let left,
        right,
        leftCanceled = false,
        rightCanceled = false,
        leftReason,
        rightReason,
        reading = null,
        finished = false,
        cancelPromise = null,
        cancelResolve = null,
        cancelReject = null;
      const cancellation = () => {
          if (!cancelPromise)
            cancelPromise = new Promise((resolve, reject) => {
              cancelResolve = resolve;
              cancelReject = reject;
            });
          return cancelPromise;
        },
        release = () => {
          if (finished) return;
          finished = true;
          try {
            reader.releaseLock();
          } catch (_) {}
        },
        settleCancellation = (reason, rejected = false) => {
          if (!cancelPromise) return;
          if (rejected) cancelReject(reason);
          else cancelResolve(reason);
          cancelPromise = null;
          cancelResolve = null;
          cancelReject = null;
        },
        cancelSource = () => {
          const pending = cancellation();
          Promise.resolve(reader.cancel([leftReason, rightReason])).then(
            (value) => {
              release();
              settleCancellation(value);
            },
            (reason) => {
              release();
              settleCancellation(reason, true);
            },
          );
          return pending;
        },
        pump = () => {
          if (finished || (leftCanceled && rightCanceled))
            return Promise.resolve();
          if (reading) return reading;
          const operation = reader.read().then(
            (result) => {
              if (result.done) {
                if (!leftCanceled) left.close();
                if (!rightCanceled) right.close();
                release();
                settleCancellation(undefined);
              } else {
                if (!leftCanceled) left.enqueue(result.value);
                if (!rightCanceled) right.enqueue(result.value);
              }
            },
            (reason) => {
              if (!leftCanceled) left.error(reason);
              if (!rightCanceled) right.error(reason);
              release();
              settleCancellation(undefined);
              throw reason;
            },
          );
          reading = operation.then(
            (value) => {
              reading = null;
              return value;
            },
            (reason) => {
              reading = null;
              throw reason;
            },
          );
          return reading;
        };
      return [
        new ReadableStream({
          start(controller) {
            left = controller;
          },
          pull: pump,
          cancel(reason) {
            leftCanceled = true;
            leftReason = reason;
            return rightCanceled ? cancelSource() : cancellation();
          },
        }),
        new ReadableStream({
          start(controller) {
            right = controller;
          },
          pull: pump,
          cancel(reason) {
            rightCanceled = true;
            rightReason = reason;
            return leftCanceled ? cancelSource() : cancellation();
          },
        }),
      ];
    }
    values(options = {}) {
      const preventCancel = !!options?.preventCancel;
      const reader = this.getReader();
      let finished = false, pending = 0, chain = Promise.resolve();
      const enqueue = (returning, value) => {
        if (pending >= streamQueueLimit)
          return Promise.reject(new RangeError("pending iterator limit exceeded"));
        pending++;
        const operation = chain.then(async () => {
          if (finished) return { done: true, value: returning ? value : undefined };
          if (returning) {
            finished = true;
            let cancellation;
            try {
              cancellation = preventCancel ? undefined : reader.cancel(value);
            } finally {
              /* Cancel closes the source synchronously; its cleanup promise
                 must not keep the reader lock until external work completes. */
              reader.releaseLock();
            }
            await cancellation;
            return { done: true, value };
          }
          try {
            const result = await reader.read();
            if (result.done) {
              finished = true;
              reader.releaseLock();
            }
            return result;
          } catch (error) {
            finished = true;
            reader.releaseLock();
            throw error;
          }
        });
        chain = operation.then(() => { pending--; }, () => { pending--; });
        return operation;
      };
      return {
        next() {
          return enqueue(false);
        },
        return(value) {
          return enqueue(true, value);
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
      initializeStreamOwner(this, stream._state, stream._storedError);
      this.ready = stream._state === "errored" || stream._state === "erroring"
        ? handledRejection(stream._storedError) : Promise.resolve();
      this._stream = stream;
      stream._writer = this;
      stream.locked = true;
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
      const error = new TypeError("writer lock released");
      releaseStreamOwner(this, error);
      this.ready = handledRejection(error);
      this._stream._writer = null;
      this._stream.locked = false;
      this._stream = null;
    }
  }
  class WritableStream {
    constructor(sink = {}) {
      this._sink = sink || {};
      this._state = "writable";
      this._storedError = undefined;
      this._abortPromise = null;
      this._closeStarted = false;
      this._queuedBytes = 0;
      this._chain = Promise.resolve();
      this.locked = false;
      this._writer = null;
      try {
        if (typeof this._sink.start === "function")
          this._chain = Promise.resolve(this._sink.start(this)).then(
            undefined, (reason) => this._fail(reason),
          );
      } catch (reason) {
        this._fail(reason);
      }
    }
    _fail(reason) {
      if (this._state === "writable" || this._state === "closing") {
        this._storedError = reason;
        this._state = "errored";
        if (this._writer) this._writer.ready = handledRejection(reason);
        finishStreamOwner(this._writer, reason, true);
      } else if (this._state === "erroring" && this._closeStarted) {
        this._state = "errored";
        finishStreamOwner(this._writer, this._storedError, true);
      }
    }
    _write(value) {
      if (this._state === "errored" || this._state === "erroring")
        return Promise.reject(this._storedError);
      if (this._state !== "writable")
        return Promise.reject(new TypeError("stream is not writable"));
      const bytes = chunkBytes(value);
      if (!queueFits(this._queuedBytes, bytes,
          largeStreamEndpoints.has(this._sink)
            ? compressionByteLimit : streamByteLimit))
        return Promise.reject(
          new RangeError("write queue exceeds available memory"));
      this._queuedBytes += bytes;
      const operation = this._chain.then(() => {
        if (this._state === "errored" || this._state === "erroring")
          throw this._storedError;
        return typeof this._sink.write === "function"
          ? this._sink.write(value, this)
          : undefined;
      });
      this._chain = operation.then(
        () => {
          this._queuedBytes -= bytes;
        },
        (reason) => {
          this._queuedBytes -= bytes;
          this._fail(reason);
        },
      );
      return operation;
    }
    _close() {
      if (this._state === "errored" || this._state === "erroring")
        return Promise.reject(this._storedError);
      if (this._state !== "writable")
        return Promise.reject(new TypeError("stream is not writable"));
      this._state = "closing";
      const operation = this._chain.then(() => {
        if (this._state === "errored" || this._state === "erroring")
          throw this._storedError;
        this._closeStarted = true;
        return typeof this._sink.close === "function"
          ? this._sink.close()
          : undefined;
      });
      this._chain = operation.then(
        () => {
          this._state = "closed";
          this._storedError = undefined;
          finishStreamOwner(this._writer);
        },
        (reason) => {
          this._closeFailure = reason;
          this._fail(reason);
        },
      );
      return operation;
    }
    _abort(reason) {
      if (this._state === "closed" || this._state === "errored")
        return Promise.resolve();
      if (this._state === "erroring") return this._abortPromise;
      this._state = "erroring";
      this._storedError = reason;
      if (this._writer) this._writer.ready = handledRejection(reason);
      /* The chain owns accepted operations until they retire. Queued work
         checks erroring before entering the sink; an in-flight close wins
         over abort, but an in-flight write must finish before sink cleanup. */
      this._abortPromise = this._chain.then(() => {
        if (this._state === "closed") return;
        if (this._closeStarted) throw this._closeFailure;
        this._state = "errored";
        return typeof this._sink.abort === "function"
          ? this._sink.abort(reason) : undefined;
      }).then(() => {
        if (this._state !== "closed")
          finishStreamOwner(this._writer, this._storedError, true);
      }, (error) => {
        finishStreamOwner(this._writer, this._storedError, true);
        throw error;
      });
      return this._abortPromise;
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
          let transformed;
          try {
            if (typeof transformer.transform !== "function") {
              readableController.enqueue(value);
              return;
            }
            transformed = transformer.transform(value, readableController);
          } catch (reason) {
            readableController.error(reason);
            throw reason;
          }
          return Promise.resolve(transformed).catch((reason) => {
            readableController.error(reason);
            throw reason;
          });
        },
        close() {
          let flushed;
          try {
            flushed = typeof transformer.flush === "function"
              ? transformer.flush(readableController)
              : undefined;
          } catch (reason) {
            readableController.error(reason);
            return Promise.reject(reason);
          }
          return Promise.resolve(flushed).then(
            () => readableController.close(),
            (reason) => {
              readableController.error(reason);
              throw reason;
            },
          );
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
  const consumeReadableByteStream = async (
    stream, limit = streamByteLimit, signal = null, release = false,
  ) => {
    if (!(stream instanceof ReadableStream))
      throw new TypeError("ReadableStream required");
    if (stream.locked || readableState(stream).disturbed)
      throw new TypeError("Body has already been consumed");
    if (signal) signal.throwIfAborted();
    const reader = stream.getReader(),
      chunks = [];
    let total = 0,
      aborted = false,
      abortAlgorithm = null;
    try {
      if (signal) {
        abortAlgorithm = () => {
          aborted = true;
          try { reader.cancel(signal.reason); } catch (_) {}
        };
        globalThis.__tilefinchAddAbortAlgorithm(signal, abortAlgorithm);
      }
      for (;;) {
        const result = await reader.read();
        if (aborted) throw signal.reason;
        if (result.done) break;
        if (!(result.value instanceof Uint8Array))
          throw new TypeError("Body stream chunk must be a Uint8Array");
        if (result.value.byteLength > limit - total)
          throw new RangeError("Body exceeds bounded size");
        /* Append a byte-sequence snapshot; producers retain and may mutate
         * their Uint8Array after enqueueing it. */
        chunks.push(result.value.slice());
        total += result.value.byteLength;
      }
    } finally {
      if (abortAlgorithm)
        globalThis.__tilefinchRemoveAbortAlgorithm(signal, abortAlgorithm);
      if (release) reader.releaseLock();
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return bytes;
  },
    consumeBufferedReadableByteStream = (stream, limit = streamByteLimit) => {
      const state = readableState(stream);
      if (state.locked || state.disturbed)
        throw new TypeError("Body has already been consumed");
      if (state.status === "errored") throw state.error;
      /* A close-requested queue is a complete buffered body, but its public
         closed notification must wait until this consumption drains it. */
      if (state.status !== "closed" && !state.closeRequested) return null;
      let total = 0;
      for (const entry of state.queue) {
        if (!(entry.value instanceof Uint8Array))
          throw new TypeError("Body stream chunk must be a Uint8Array");
        if (entry.value.byteLength > limit - total)
          throw new RangeError("Body exceeds bounded size");
        total += entry.value.byteLength;
      }
      const bytes = new Uint8Array(total);
      let offset = 0;
      for (const entry of state.queue) {
        bytes.set(entry.value, offset);
        offset += entry.value.byteLength;
      }
      state.queue.length = 0;
      state.queueBytes = 0;
      state.queueSize = 0;
      state.disturbed = true;
      closeReadableStream(stream);
      return bytes;
    };
  for (const [name, value] of [
    ["__tilefinchReadableStreamDisturbed", (stream) =>
      readableState(stream).disturbed],
    ["__tilefinchMarkReadableStreamDisturbed", (stream) => {
      readableState(stream).disturbed = true;
      return true;
    }],
    ["__tilefinchConsumeReadableByteStream", consumeReadableByteStream],
    ["__tilefinchConsumeBufferedReadableByteStream",
      consumeBufferedReadableByteStream],
  ]) {
    Object.defineProperty(globalThis, name, {
      configurable: false,
      enumerable: false,
      writable: false,
      value,
    });
  }
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
  const compressionChunk = (value) => {
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
    #transform;
    constructor(format) {
      if (typeof compressionNative !== "function")
        throw new TypeError("CompressionStream is unavailable");
      const transform = compressionTransform(
        compressionFormat(format), false);
      this.#transform = transform;
    }
    get readable() {
      return this.#transform.readable;
    }
    get writable() {
      return this.#transform.writable;
    }
  }
  class DecompressionStream {
    #transform;
    constructor(format) {
      if (typeof compressionNative !== "function")
        throw new TypeError("DecompressionStream is unavailable");
      const transform = compressionTransform(
        compressionFormat(format), true);
      this.#transform = transform;
    }
    get readable() {
      return this.#transform.readable;
    }
    get writable() {
      return this.#transform.writable;
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
