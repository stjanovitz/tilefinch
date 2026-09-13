(() => {
  "use strict";
  const available = globalThis.__tilefinchWasmAvailable,
    compileNative = globalThis.__tilefinchWasmCompile,
    instantiateNative = globalThis.__tilefinchWasmInstantiate,
    validateNative = globalThis.__tilefinchWasmValidate,
    moduleInfoNative = globalThis.__tilefinchWasmModuleInfo,
    memoryGrowNative = globalThis.__tilefinchWasmMemoryGrow,
    globalGetNative = globalThis.__tilefinchWasmGlobalGet,
    globalSetNative = globalThis.__tilefinchWasmGlobalSet,
    detachBufferNative = globalThis.__tilefinchWasmDetachBuffer,
    snapshotSourceNative = globalThis.__tilefinchWasmSnapshotSource,
    consumeStreamingResponse = globalThis.__tilefinchConsumeWasmResponse,
    createPrivateWeakMap = globalThis.__tilefinchCreatePrivateWeakMap;
  if (
    typeof available !== "function" ||
    typeof compileNative !== "function" ||
    typeof instantiateNative !== "function" ||
    typeof validateNative !== "function" ||
    typeof moduleInfoNative !== "function" ||
    typeof memoryGrowNative !== "function" ||
    typeof globalGetNative !== "function" ||
    typeof globalSetNative !== "function" ||
    typeof detachBufferNative !== "function" ||
    typeof snapshotSourceNative !== "function" ||
    typeof consumeStreamingResponse !== "function" ||
    typeof createPrivateWeakMap !== "function" ||
    !available()
  ) return;

  const PAGE_BYTES = 65536,
    MAX_MEMORY_PAGES = 16,
    MAX_TABLE_ELEMENTS = 1024,
    moduleHandles = createPrivateWeakMap(),
    instanceHandles = createPrivateWeakMap(),
    memoryHandles = createPrivateWeakMap(),
    nativeMemoryWrappers = createPrivateWeakMap(),
    tableHandles = createPrivateWeakMap(),
    globalHandles = createPrivateWeakMap(),
    wasmFunctions = createPrivateWeakMap(),
    resolve = Promise.resolve.bind(Promise),
    reject = Promise.reject.bind(Promise),
    validImports = (imports) => imports !== null &&
      (typeof imports === "object" || typeof imports === "function"),
    errorMessage = (error) => error && typeof error.message === "string"
      ? error.message : "WebAssembly compilation failed",
    toWasmString = (value) => `${value}`,
    toBoundedIndex = (value, maximum, label) => {
      let number = +value;
      if (Object.is(number, -0)) number = 0;
      number = Math.trunc(number);
      if (!Number.isFinite(number) || number < 0)
        throw new TypeError(`${label} must be a finite unsigned number`);
      if (number > maximum)
        throw new RangeError(`${label} exceeds Tilefinch limits`);
      return number;
    },
    toI64 = (value) => {
      /* WebAssembly uses ToBigInt rather than the BigInt constructor's more
         permissive conversion.  BigInt.asIntN itself applies ToBigInt, so it
         also rejects an object whose primitive value is a Number. */
      return BigInt.asIntN(64, value);
    },
    toNumber = (value) => {
      /* Unary plus performs the abstract ToNumber operation.  Number() has a
         special BigInt conversion which WebAssembly must not inherit. */
      return +value;
    },
    valueForType = (type, value) => {
      switch (type) {
        case "i32": return toNumber(value) | 0;
        case "i64": return toI64(value);
        case "f32": return Math.fround(toNumber(value));
        case "f64": return toNumber(value);
        case "externref": return value;
        case "anyfunc":
          if (value === null || wasmFunctions.get(value)) return value;
          throw new TypeError("WebAssembly function or null expected");
        default: throw new TypeError("unsupported WebAssembly value type");
      }
    };

  const makeWasmError = (name) => {
    const Constructor = function(message, options) {
      const error = arguments.length === 0 || message === undefined
        ? new Error() : new Error(toWasmString(message));
      const target = new.target || Constructor,
        prototype = target && (typeof target.prototype === "object" ||
          typeof target.prototype === "function") && target.prototype !== null
          ? target.prototype : Constructor.prototype;
      Object.setPrototypeOf(error, prototype);
      if (options !== null && typeof options === "object" && "cause" in options)
        Object.defineProperty(error, "cause", {
          configurable: true, writable: true, value: options.cause,
        });
      return error;
    };
    Object.defineProperties(Constructor.prototype, {
      constructor: { configurable: true, writable: true, value: Constructor },
      name: { configurable: true, writable: true, value: name },
      message: { configurable: true, writable: true, value: "" },
    });
    Object.setPrototypeOf(Constructor.prototype, Error.prototype);
    Object.setPrototypeOf(Constructor, Error);
    Object.defineProperties(Constructor, {
      name: { configurable: true, value: name },
      length: { configurable: true, value: 1 },
      prototype: { writable: false },
    });
    return Constructor;
  },
    CompileError = makeWasmError("CompileError"),
    LinkError = makeWasmError("LinkError"),
    RuntimeError = makeWasmError("RuntimeError");

  const moduleHandle = (module) => {
    const handle = moduleHandles.get(module);
    if (handle === undefined) throw new TypeError("WebAssembly.Module expected");
    return handle;
  },
    cloneModule = (value) => {
      const handle = moduleHandles.get(value);
      if (handle === undefined) return null;
      const clone = Object.create(Module.prototype);
      moduleHandles.set(clone, handle);
      return clone;
    };
  /* Dedicated Workers share Tilefinch's bounded realm implementation, so an
     immutable compiled module can safely share its native handle while still
     receiving the distinct JavaScript wrapper required by structured clone.
     Other Wasm objects remain non-cloneable. */
  Object.defineProperty(globalThis, "__tilefinchCloneWasmModule", {
    configurable: false, enumerable: false, writable: false,
    value: cloneModule,
  });

  class Module {
    constructor(source) {
      if (arguments.length === 0)
        throw new TypeError("WebAssembly source is required");
      let handle;
      try { handle = compileNative(source); }
      catch (error) {
        if (error instanceof TypeError || error instanceof RangeError) throw error;
        throw new CompileError(errorMessage(error));
      }
      moduleHandles.set(this, handle);
    }
    static imports(module) { return moduleInfoNative(moduleHandle(module), 0); }
    static exports(module) { return moduleInfoNative(moduleHandle(module), 1); }
    static customSections(module, sectionName) {
      if (arguments.length < 2) throw new TypeError("custom section name is required");
      return moduleInfoNative(moduleHandle(module), 2, toWasmString(sectionName));
    }
  }
  Object.defineProperty(Module.prototype, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly.Module",
  });

  const memoryHandle = (memory) => {
    const handle = memoryHandles.get(memory);
    if (handle === undefined) throw new TypeError("WebAssembly.Memory expected");
    return handle;
  };
  class Memory {
    constructor(descriptor) {
      if (descriptor === null ||
          (typeof descriptor !== "object" && typeof descriptor !== "function"))
        throw new TypeError("WebAssembly.Memory descriptor expected");
      /* Keep conversion interleaved with each descriptor read.  The JS API
         makes this observable to Proxy descriptors and feature probes.
         Tilefinch admits the ordinary 32-bit address space; memory64 remains
         outside the bounded PSP profile rather than being silently treated
         as i32. */
      const addressValue = descriptor.address,
        address = addressValue === undefined ? "i32"
          : toWasmString(addressValue);
      if (address !== "i32")
        throw new TypeError("unsupported WebAssembly memory address type");
      const initialValue = descriptor.initial;
      if (initialValue === undefined)
        throw new TypeError("WebAssembly.Memory initial is required");
      const initial = toBoundedIndex(initialValue, MAX_MEMORY_PAGES,
          "WebAssembly memory"),
        maximumValue = descriptor.maximum,
        maximum = maximumValue === undefined ? MAX_MEMORY_PAGES
          : toBoundedIndex(maximumValue, MAX_MEMORY_PAGES,
              "WebAssembly memory"),
        shared = Boolean(descriptor.shared);
      if (shared)
        throw new TypeError("shared WebAssembly memory is unsupported");
      if (maximum < initial)
        throw new RangeError("WebAssembly memory maximum is below initial");
      memoryHandles.set(this, {
        buffer: new ArrayBuffer(initial * PAGE_BYTES), maximum,
      });
    }
    get buffer() { return memoryHandle(this).buffer; }
    grow(delta) {
      const handle = memoryHandle(this),
        pages = toBoundedIndex(delta, MAX_MEMORY_PAGES,
          "WebAssembly memory growth");
      if (handle.__tilefinchWasmMemory)
        return memoryGrowNative(handle.__tilefinchWasmInstance, pages);
      const oldBuffer = handle.buffer,
        oldPages = oldBuffer.byteLength / PAGE_BYTES,
        nextPages = oldPages + pages;
      if (nextPages > handle.maximum)
        throw new RangeError("WebAssembly memory maximum exceeded");
      const next = new ArrayBuffer(nextPages * PAGE_BYTES);
      new Uint8Array(next).set(new Uint8Array(oldBuffer));
      detachBufferNative(oldBuffer);
      handle.buffer = next;
      return oldPages;
    }
  }
  Object.defineProperty(Memory.prototype, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly.Memory",
  });
  const wrapMemory = (native) => {
    let memory = nativeMemoryWrappers.get(native);
    if (memory !== undefined) return memory;
    memory = Object.create(Memory.prototype);
    memoryHandles.set(memory, native);
    nativeMemoryWrappers.set(native, memory);
    return memory;
  };

  class Table {
    constructor(descriptor, value = undefined) {
      if (descriptor === null ||
          (typeof descriptor !== "object" && typeof descriptor !== "function"))
        throw new TypeError("WebAssembly.Table descriptor expected");
      /* Web IDL dictionary members are observed in lexicographic order. */
      const elementValue = descriptor.element,
        element = toWasmString(elementValue);
      if (element !== "anyfunc" && element !== "externref")
        throw new TypeError("unsupported WebAssembly table element type");
      const addressValue = descriptor.address,
        address = addressValue === undefined ? "i32"
          : toWasmString(addressValue);
      if (address !== "i32")
        throw new TypeError("unsupported WebAssembly table address type");
      const initialValue = descriptor.initial;
      if (initialValue === undefined)
        throw new TypeError("WebAssembly.Table initial is required");
      const initial = toBoundedIndex(initialValue, MAX_TABLE_ELEMENTS,
          "WebAssembly table"),
        maximumValue = descriptor.maximum,
        maximum = maximumValue === undefined ? MAX_TABLE_ELEMENTS
          : toBoundedIndex(maximumValue, MAX_TABLE_ELEMENTS,
              "WebAssembly table");
      if (maximum < initial)
        throw new RangeError("WebAssembly table maximum is below initial");
      const valueOmitted = arguments.length < 2;
      if (element !== "externref" && valueOmitted) value = null;
      if (element !== "externref" && value !== null && !wasmFunctions.get(value))
        throw new TypeError("WebAssembly function or null expected");
      const entries = new Array(initial);
      entries.fill(value);
      tableHandles.set(this, { element, maximum, entries });
    }
    get length() {
      const handle = tableHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Table expected");
      return handle.entries.length;
    }
    get(index) {
      const handle = tableHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Table expected");
      index = toBoundedIndex(index, MAX_TABLE_ELEMENTS, "table index");
      if (index >= handle.entries.length)
        throw new RangeError("WebAssembly table index is out of bounds");
      return handle.entries[index];
    }
    set(index, value = undefined) {
      const handle = tableHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Table expected");
      index = toBoundedIndex(index, MAX_TABLE_ELEMENTS, "table index");
      if (index >= handle.entries.length)
        throw new RangeError("WebAssembly table index is out of bounds");
      const valueOmitted = arguments.length < 2;
      if (handle.element !== "externref" && valueOmitted) value = null;
      if (handle.element !== "externref" && value !== null && !wasmFunctions.get(value))
        throw new TypeError("WebAssembly function or null expected");
      handle.entries[index] = value;
    }
    grow(delta, value = undefined) {
      const handle = tableHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Table expected");
      delta = toBoundedIndex(delta, MAX_TABLE_ELEMENTS, "table growth");
      const valueOmitted = arguments.length < 2;
      if (handle.element !== "externref" && valueOmitted) value = null;
      if (handle.element !== "externref" && value !== null && !wasmFunctions.get(value))
        throw new TypeError("WebAssembly function or null expected");
      const old = handle.entries.length;
      if (delta > handle.maximum - old)
        throw new RangeError("WebAssembly table maximum exceeded");
      for (let i = 0; i < delta; i++) handle.entries.push(value);
      return old;
    }
  }
  Object.defineProperty(Table.prototype, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly.Table",
  });

  class Global {
    constructor(descriptor, value = undefined) {
      if (descriptor === null ||
          (typeof descriptor !== "object" && typeof descriptor !== "function"))
        throw new TypeError("WebAssembly.Global descriptor expected");
      /* `mutable` precedes `value` in Web IDL dictionary order. */
      const mutableValue = descriptor.mutable,
        typeValue = descriptor.value,
        type = toWasmString(typeValue), mutable = Boolean(mutableValue),
        omitted = arguments.length < 2,
        initial = type === "i64" && value === undefined ? 0n
          : type === "anyfunc" && omitted ? null
          : type === "externref" && omitted ? undefined
          : value === undefined && type !== "externref" ? 0 : value;
      globalHandles.set(this, {
        type, mutable, value: valueForType(type, initial),
      });
    }
    get value() {
      const handle = globalHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Global expected");
      return handle.native
        ? globalGetNative(handle.instance, handle.name)
        : handle.value;
    }
    set value(value) {
      const handle = globalHandles.get(this);
      if (handle === undefined) throw new TypeError("WebAssembly.Global expected");
      if (!handle.mutable) throw new TypeError("WebAssembly.Global is immutable");
      const converted = valueForType(handle.type, value);
      if (handle.native) globalSetNative(handle.instance, handle.name, converted);
      else handle.value = converted;
    }
    valueOf() { return this.value; }
  }
  Object.defineProperty(Global.prototype, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly.Global",
  });
  const wrapGlobal = (native) => {
    const global = Object.create(Global.prototype);
    globalHandles.set(global, {
      native: true,
      instance: native.__tilefinchWasmInstance,
      name: native.__tilefinchWasmName,
      type: native.__tilefinchWasmType,
      mutable: native.__tilefinchWasmMutable,
    });
    return global;
  };

  const wrapExports = (nativeExports) => {
    const exports = Object.create(null), functionKeys = [], functionValues = [];
    for (const name of Object.keys(nativeExports)) {
      const value = nativeExports[name];
      if (value && value.__tilefinchWasmMemory) exports[name] = wrapMemory(value);
      else if (value && value.__tilefinchWasmGlobal) exports[name] = wrapGlobal(value);
      else if (typeof value === "function") {
        const functionKey = value.__tilefinchWasmIndex === undefined
          ? value : value.__tilefinchWasmIndex;
        let callable;
        for (let i = 0; i < functionKeys.length; i++) {
          if (functionKeys[i] === functionKey) {
            callable = functionValues[i];
            break;
          }
        }
        if (callable === undefined) {
          callable = (...args) => {
            try { return value(...args); }
            catch (error) {
              if (error && error.__tilefinchWasmRuntimeTrap === true)
                throw new RuntimeError(errorMessage(error));
              throw error;
            }
          };
          Object.defineProperties(callable, {
            name: { configurable: true,
              value: value.__tilefinchWasmIndex === undefined
                ? value.name : String(value.__tilefinchWasmIndex) },
            length: { configurable: true, value: value.length },
          });
          globalThis.__tilefinchMarkNativeFunction(callable);
          const slot = functionKeys.length;
          functionKeys[slot] = functionKey;
          functionValues[slot] = callable;
          wasmFunctions.set(callable, true);
        }
        exports[name] = callable;
      }
      else exports[name] = value;
    }
    return Object.freeze(exports);
  };
  class Instance {
    constructor(module, imports = {}) {
      if (!validImports(imports))
        throw new TypeError("WebAssembly imports must be an object");
      let handle;
      try { handle = instantiateNative(moduleHandle(module), imports); }
      catch (error) {
        if (error instanceof TypeError || error instanceof RangeError) throw error;
        if (error && error.__tilefinchWasmRuntimeTrap === true)
          throw new RuntimeError(errorMessage(error));
        if (error && error.__tilefinchWasmLinkError === true) {
          const message = errorMessage(error);
          /* Pinned WAMR distinguishes start-function traps from ordinary
             instantiation errors with its stable `Exception:` marker. */
          if (message.includes("Exception:")) throw new RuntimeError(message);
          throw new LinkError(message);
        }
        throw error;
      }
      instanceHandles.set(this, {
        handle, exports: wrapExports(handle.exports),
      });
    }
    get exports() {
      const record = instanceHandles.get(this);
      if (record === undefined)
        throw new TypeError("WebAssembly.Instance expected");
      return record.exports;
    }
  }
  Object.defineProperty(Instance.prototype, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly.Instance",
  });

  const compile = (source) => {
      let snapshot;
      try { snapshot = snapshotSourceNative(source); }
      catch (error) { return reject(error); }
      return resolve().then(() => new Module(snapshot));
    },
    instantiate = (source, imports = {}) => {
      if (!validImports(imports))
        return reject(new TypeError("WebAssembly imports must be an object"));
      if (moduleHandles.get(source) !== undefined) {
        /* The Module overload performs import-object property access before
           it returns.  The byte overload keeps compilation and linking in a
           later promise job. */
        try { return resolve(new Instance(source, imports)); }
        catch (error) { return reject(error); }
      }
      let snapshot;
      try { snapshot = snapshotSourceNative(source); }
      catch (error) { return reject(error); }
      return resolve().then(() => {
        const module = new Module(snapshot);
        return { module, instance: new Instance(module, imports) };
      });
    },
    validate = (source) => validateNative(source),
    streamingBytes = (source) =>
      resolve(source).then(consumeStreamingResponse),
    compileStreaming = (source) => streamingBytes(source).then(compile),
    instantiateStreaming = (source, imports = {}) =>
      streamingBytes(source).then((bytes) => instantiate(bytes, imports));

  /* Web IDL namespaces are ordinary objects, including inherited Object
     helpers such as hasOwnProperty. */
  const namespace = {};
  const namespaceFunctions = new Set([
    "compile", "compileStreaming", "instantiate", "instantiateStreaming",
    "validate",
  ]);
  for (const [name, value] of [
    ["compile", compile], ["compileStreaming", compileStreaming],
    ["instantiate", instantiate], ["instantiateStreaming", instantiateStreaming],
    ["validate", validate], ["Module", Module], ["Instance", Instance],
    ["Memory", Memory], ["Table", Table], ["Global", Global],
    ["CompileError", CompileError], ["LinkError", LinkError],
    ["RuntimeError", RuntimeError],
  ]) Object.defineProperty(namespace, name, {
    configurable: true, enumerable: namespaceFunctions.has(name),
    writable: true, value,
  });
  Object.defineProperty(namespace, Symbol.toStringTag, {
    configurable: true, value: "WebAssembly",
  });
  Object.defineProperty(globalThis, "WebAssembly", {
    value: namespace, writable: true, configurable: true,
  });

  /* WebAssembly's JS API deliberately exposes its reflection helpers and
     prototype operations as enumerable Web IDL members. Class syntax makes
     them non-enumerable by default, so normalize the descriptors here. */
  for (const [owner, names] of [
    [Module, ["imports", "exports", "customSections"]],
    [Instance.prototype, ["exports"]],
    [Memory.prototype, ["buffer", "grow"]],
    [Table.prototype, ["length", "get", "set", "grow"]],
    [Global.prototype, ["value", "valueOf"]],
  ]) for (const name of names) {
    const descriptor = Object.getOwnPropertyDescriptor(owner, name);
    if (descriptor) Object.defineProperty(owner, name, {
      ...descriptor, enumerable: true,
    });
  }

  const markNative = globalThis.__tilefinchMarkNativeFunction;
  for (const constructor of [
    Module, Instance, Memory, Table, Global,
    CompileError, LinkError, RuntimeError,
  ]) {
    markNative(constructor);
    for (const key of Reflect.ownKeys(constructor))
      markNative(Object.getOwnPropertyDescriptor(constructor, key)?.value);
    for (const key of Reflect.ownKeys(constructor.prototype)) {
      const descriptor = Object.getOwnPropertyDescriptor(constructor.prototype, key);
      markNative(descriptor?.value);
      markNative(descriptor?.get);
      markNative(descriptor?.set);
    }
  }
  for (const fn of [compile, compileStreaming, instantiate,
    instantiateStreaming, validate, cloneModule]) markNative(fn);
})();
