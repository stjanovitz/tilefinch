/* FileReader, built the first time a page reads the global (a ROM-backed
   lazy module; see Module boundaries in README.md). The host lends the
   dom.js share and the trusted-event marker for this evaluation only: a
   reader's events are trusted, dispatched through EventTarget's own
   dispatch, and its tasks are cancelled with the cancel that reaches every
   timer. Worker teardown aborts a reader through the hook this publishes. */
(() => {
  /* Loaded after the page may have replaced these globals: build errors
     and platform objects with the constructors hardening.js captured. */
  const { defineProperty, getOwnPropertyDescriptor, apply, EventTarget,
    DOMException, Blob, claimHooks } =
    globalThis.__tilefinchEventInterfaces;
  const publish = claimHooks?.(),
    domShared = globalThis.__tilefinchDomShared,
    trustedEvent = globalThis.__tilefinchTrustedEvent,
    createEvent = globalThis.__tilefinchCreateEvent,
    fileReaderDispatch = domShared.dispatchSimpleEvent,
    tilefinchBlobBytes = globalThis.__tilefinchBlobBytes,
    tilefinchBlobText = globalThis.__tilefinchBlobText,
    tilefinchBlobSize = getOwnPropertyDescriptor(Blob.prototype, "size").get,
    fromCharCode = String.fromCharCode;
  let fileReaderState;
  class FileReader extends EventTarget {
    #state;
    static { fileReaderState = reader => reader.#state; }
    constructor() {
      super();
      this.#state = { readyState: 0, result: null, error: null, task: 0, generation: 0 };
      for (const name of [
        "onloadstart",
        "onprogress",
        "onload",
        "onabort",
        "onerror",
        "onloadend",
      ])
        this[name] = null;
    }
    get readyState() { return this.#state.readyState; }
    get result() { return this.#state.result; }
    get error() { return this.#state.error; }
    #emit(type, loaded = 0, total = 0) {
      const event = trustedEvent(createEvent(type, {
        lengthComputable: true,
        loaded,
        total,
      }, "ProgressEvent"));
      fileReaderDispatch(this, event);
      try {
        const handler = this["on" + type];
        if (typeof handler === "function")
          globalThis.__tilefinchRunTask("file-reader:" + type, handler, this, [event]);
      } catch (error) {
        globalThis.__tilefinchReportUncaught(error, "file-reader:" + type);
      }
    }
    #read(blob, convert) {
      const state = this.#state,
        total = apply(tilefinchBlobSize, blob, []);
      if (state.readyState === 1)
        throw new DOMException("A read is already active", "InvalidStateError");
      const generation = state.generation + 1,
        current = () => state.generation === generation && state.readyState === 1,
        fail = error => {
          if (!current()) return;
          if (state.task) domShared.cancelTimer(state.task);
          state.task = 0;
          state.error = error;
          state.result = null;
          state.readyState = 2;
          this.#emit("error", 0, total);
          if (state.generation === generation && state.readyState !== 1)
            this.#emit("loadend", 0, total);
        };
      let phase = 0;
      const step = () => {
        if (!current()) return;
        state.task = 0;
        if (phase < 2) {
          const type = phase++ === 0 ? "loadstart" : "progress";
          /* Keep one continuation admitted before invoking author code.
             Separate tasks allow its promise jobs to abort before progress
             or completion, without retaining a second task queue. */
          state.task = __tilefinchRealmGlobal.__tilefinchScheduleTask(step);
          try {
            this.#emit(type, type === "loadstart" ? 0 : total, total);
            if (current() && !state.task) fail(new DOMException(
              "File read task quota exceeded", "QuotaExceededError"));
          } catch (error) {
            if (!current()) throw error;
            fail(error);
          }
          return;
        }
        try {
          const result = convert(blob);
          if (!current()) return;
          state.result = result;
          state.readyState = 2;
          this.#emit("load", total, total);
        } catch (error) {
          if (!current()) throw error;
          fail(error);
          return;
        }
        if (state.generation === generation && state.readyState !== 1)
          this.#emit("loadend", total, total);
      };
      const task = __tilefinchRealmGlobal.__tilefinchScheduleTask(step);
      /* A refused reservation must leave EMPTY/DONE usable for a retry,
         rather than publishing a LOADING state with no task to finish it. */
      if (!task) throw new DOMException("File read task quota exceeded", "QuotaExceededError");
      state.generation = generation;
      state.task = task;
      state.readyState = 1;
      state.result = state.error = null;
    }
    readAsArrayBuffer(blob) {
      this.#read(blob, (source) => tilefinchBlobBytes(source).slice().buffer);
    }
    readAsText(blob, encoding = "utf-8") {
      this.#read(blob, (source) => tilefinchBlobText(source, encoding));
    }
    readAsBinaryString(blob) {
      this.#read(blob, (source) => {
        const bytes = tilefinchBlobBytes(source);
        let binary = "";
        for (let at = 0; at < bytes.length; at += 8192)
          binary += apply(fromCharCode, null, bytes.subarray(at, at + 8192));
        return binary;
      });
    }
    readAsDataURL(blob) {
      this.#read(blob, (source) => {
        const bytes = tilefinchBlobBytes(source);
        return "data:" +
          (source.type || "application/octet-stream") +
          ";base64," +
          /* The native is a frozen global, which a page cannot replace. */
          (bytes.length ? __tilefinchBase64EncodeBytes(bytes) : "");
      });
    }
    abort() {
      const state = this.#state;
      if (state.readyState !== 1) {
        state.result = null;
        return;
      }
      const generation = ++state.generation;
      domShared.cancelTimer(state.task);
      state.task = 0;
      state.readyState = 2;
      state.result = null;
      this.#emit("abort");
      if (state.generation === generation && state.readyState !== 1)
        this.#emit("loadend");
    }
  }
  for (const [name, value] of [["EMPTY", 0], ["LOADING", 1], ["DONE", 2]]) {
    defineProperty(FileReader, name, { __proto__: null, value });
    defineProperty(FileReader.prototype, name, { __proto__: null, value });
  }
  globalThis.FileReader = FileReader;
  publish?.("__tilefinchFileReaderInterface", FileReader);
  /* Worker teardown must cancel an owner-backed FileReader without emitting
     abort/loadend into a realm that has already been retired. */
  publish?.("__tilefinchAbortFileReaderForWorker", (reader) => {
    let state;
    try { state = fileReaderState(reader); } catch (_) { return false; }
    ++state.generation;
    if (state.task) domShared.cancelTimer(state.task);
    state.task = 0;
    state.readyState = 2;
    state.result = null;
    state.error = new DOMException("Worker is terminated", "AbortError");
    return true;
  });
})();
