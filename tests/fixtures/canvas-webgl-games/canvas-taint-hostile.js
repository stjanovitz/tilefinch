/* A hostile page realm for canvas-taint-scene.js (NB-04). Before any canvas
   is used, page code traps every builtin the bootstrap could move an
   image's pixels through: every Array and TypedArray prototype method and
   accessor, Symbol.species on Array and %TypedArray% (whose subclasses
   record every array a species create makes), `constructor` on the typed
   array prototypes, the array iterator, integer-index accessors on
   Object.prototype and Array.prototype, WeakMap and Map methods, and
   Function.prototype.call and apply. Each trap scans what it sees (the
   receiver, arguments and result, own data properties three levels deep)
   for two horizontally adjacent pixels of the 16x16 #x fixture, pixel
   k = ((k*53+7)&255, (k*97+131)&255, (k*29+211)&255, 255), in byte
   buffers and in runs of numbers (array elements, index stores, iterator
   values). The scene's result arrives through taintSceneDone. */
(() => {
  const R = Reflect, apply = R.apply, construct = R.construct,
    ownKeys = R.ownKeys, getDescriptor = R.getOwnPropertyDescriptor,
    define = R.defineProperty, getPrototype = R.getPrototypeOf,
    isView = ArrayBuffer.isView, isArray = Array.isArray,
    Bytes = Uint8Array, TypedArray = getPrototype(Uint8Array),
    typedPrototype = TypedArray.prototype,
    typedBuffer = getDescriptor(typedPrototype, "buffer").get,
    typedOffset = getDescriptor(typedPrototype, "byteOffset").get,
    typedByteLength = getDescriptor(typedPrototype, "byteLength").get,
    typedLength = getDescriptor(typedPrototype, "length").get,
    typedTag = getDescriptor(typedPrototype, Symbol.toStringTag).get,
    bufferLength = getDescriptor(ArrayBuffer.prototype, "byteLength").get,
    viewBuffer = getDescriptor(DataView.prototype, "buffer").get,
    viewOffset = getDescriptor(DataView.prototype, "byteOffset").get,
    viewLength = getDescriptor(DataView.prototype, "byteLength").get,
    arrayIteratorPrototype = getPrototype([][Symbol.iterator]()),
    trap = { busy: false, calls: 0, seen: 0, where: "", spies: 0 },
    captured = {};
  let inverse = 0;
  for (let odd = 1; odd < 256; odd += 2)
    if (((odd * 53) & 255) === 1) inverse = odd;
  const fixtureIndex = (red) => (((red - 7) & 255) * inverse) & 255,
    isFixture = (k, r, g, b, a) => r === ((k * 53 + 7) & 255) &&
      g === ((k * 97 + 131) & 255) && b === ((k * 29 + 211) & 255) &&
      a === 255,
    /* Two adjacent pixels of one fixture row anywhere in a run. */
    hasPair = (read, length) => {
      if (!(length >= 8)) return false;
      const limit = length < 1048576 ? length : 1048576;
      for (let at = 0; at + 8 <= limit; at++) {
        const red = read(at);
        if (typeof red !== "number") continue;
        const k = fixtureIndex(red);
        if ((k & 15) === 15) continue;
        if (isFixture(k, red, read(at + 1), read(at + 2), read(at + 3)) &&
            isFixture(k + 1, read(at + 4), read(at + 5), read(at + 6),
              read(at + 7))) return true;
      }
      return false;
    },
    saw = (where) => {
      trap.seen++;
      if (!trap.where) trap.where = where;
    },
    scanBytes = (buffer, offset, length, where) => {
      if (!(length >= 8)) return;
      const bytes = new Bytes(buffer, offset, length);
      if (hasPair((at) => bytes[at], length)) saw(where);
    },
    inspect = (value, where, depth = 0) => {
      if (value === null || (typeof value !== "object" &&
          typeof value !== "function")) return;
      if (typeof value === "function") return;
      try {
        if (isView(value)) {
          if (apply(typedTag, value, []) !== undefined) {
            scanBytes(apply(typedBuffer, value, []),
              apply(typedOffset, value, []),
              apply(typedByteLength, value, []), where + ":bytes");
            const length = apply(typedLength, value, []);
            if (hasPair((at) => value[at], length)) saw(where + ":elements");
          } else
            scanBytes(apply(viewBuffer, value, []),
              apply(viewOffset, value, []), apply(viewLength, value, []),
              where + ":view");
          return;
        }
        let length = -1;
        try { length = apply(bufferLength, value, []); } catch (error) {}
        if (length >= 0) {
          scanBytes(value, 0, length, where + ":buffer");
          return;
        }
        if (isArray(value)) {
          const count = value.length;
          if (hasPair((at) => value[at], count)) saw(where + ":array");
          if (depth < 3)
            for (let at = 0; at < count && at < 4096; at++)
              inspect(value[at], where, depth + 1);
          return;
        }
        if (depth >= 3) return;
        const keys = ownKeys(value);
        for (let at = 0; at < keys.length && at < 64; at++) {
          const descriptor = getDescriptor(value, keys[at]);
          if (descriptor && "value" in descriptor)
            inspect(descriptor.value, where, depth + 1);
        }
      } catch (error) {}
    },
    /* Runs of numbers that arrive one at a time. */
    streams = {},
    stream = (name, value) => {
      const ring = streams[name] || (streams[name] = { n: 0 });
      ring["v" + (ring.n & 7)] = value;
      ring.n++;
      if (ring.n >= 8 && hasPair((at) => ring["v" + ((ring.n - 8 + at) & 7)],
          8)) saw(name);
    },
    observe = (run) => {
      if (trap.busy) return;
      trap.busy = true;
      try { trap.calls++; run(); } finally { trap.busy = false; }
    },
    trapped = (original, self, args, label) => {
      observe(() => {
        inspect(self, label + ":this");
        for (let at = 0; at < args.length; at++)
          inspect(args[at], label + ":argument");
      });
      const result = apply(original, self, args);
      observe(() => inspect(result, label + ":result"));
      return result;
    },
    wrapAll = (owner, label, skip) => {
      for (const key of ownKeys(owner)) {
        if (key === skip) continue;
        const descriptor = getDescriptor(owner, key);
        if (!descriptor || !descriptor.configurable) continue;
        const name = label + "." + String(key);
        if (typeof descriptor.value === "function") {
          const original = descriptor.value;
          descriptor.value = function (...args) {
            return trapped(original, this, args, name);
          };
          define(owner, key, descriptor);
        } else if (typeof descriptor.get === "function") {
          const original = descriptor.get;
          descriptor.get = function () {
            return trapped(original, this, [], name);
          };
          define(owner, key, descriptor);
        }
      }
    };

  /* Species: every array a species create makes is recorded and scanned
     when the scene ends, after the bootstrap filled it. */
  const spies = {},
    spyFor = (base) => {
      if (base && base.tilefinchSpy === true) return base;
      const name = String(base && base.name);
      if (spies["s" + name]) return spies["s" + name];
      const spy = function (...args) {
        const made = construct(base, args, new.target || spy);
        captured["c" + trap.spies++] = made;
        return made;
      };
      R.setPrototypeOf(spy, base);
      spy.prototype = base.prototype;
      define(spy, "tilefinchSpy", { value: true });
      spies["s" + name] = spy;
      return spy;
    };
  for (const base of [Array, TypedArray])
    define(base, Symbol.species, {
      configurable: true,
      get() {
        observe(() => {});
        return spyFor(this);
      },
    });
  for (const name of ["Int8Array", "Uint8Array", "Uint8ClampedArray",
      "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
      "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"]) {
    const base = globalThis[name];
    if (typeof base !== "function") continue;
    define(base.prototype, "constructor", {
      configurable: true,
      get() {
        observe(() => inspect(this, name + ".constructor"));
        return base;
      },
      set(value) {
        define(this, "constructor", { value, writable: true,
          configurable: true, enumerable: true });
      },
    });
  }
  wrapAll(Array.prototype, "Array", "constructor");
  wrapAll(typedPrototype, "TypedArray", "constructor");
  wrapAll(Map.prototype, "Map", "constructor");
  wrapAll(WeakMap.prototype, "WeakMap", "constructor");
  {
    const original = arrayIteratorPrototype.next;
    define(arrayIteratorPrototype, "next", {
      configurable: true, writable: true,
      value: function next() {
        const result = apply(original, this, []);
        observe(() => {
          if (result && typeof result.value === "number")
            stream("iterator", result.value);
          else if (result) inspect(result.value, "iterator");
        });
        return result;
      },
    });
  }
  for (const owner of [Object.prototype, Array.prototype]) {
    const label = owner === Array.prototype ? "Array[index]" : "Object[index]";
    for (let index = 0; index < 64; index++)
      define(owner, index, {
        configurable: true,
        get() {
          observe(() => {});
          return undefined;
        },
        set(value) {
          observe(() => {
            if (typeof value === "number") stream(label, value);
            else inspect(value, label);
          });
          define(this, index, { value, writable: true, configurable: true,
            enumerable: true });
        },
      });
  }
  {
    const call = Function.prototype.call, applyMethod = Function.prototype.apply;
    define(Function.prototype, "call", {
      configurable: true, writable: true,
      value: function (...args) {
        return trapped(call, this, args, "Function.call");
      },
    });
    define(Function.prototype, "apply", {
      configurable: true, writable: true,
      value: function (...args) {
        return trapped(applyMethod, this, args, "Function.apply");
      },
    });
  }

  globalThis.taintSceneDone = (summary) => {
    observe(() => {
      for (const key of ownKeys(captured))
        inspect(captured[key], "species");
    });
    globalThis.pocSummary = trap.seen
      ? "TAINT-HOSTILE-SEEN:" + trap.where + ":" + trap.seen
      : trap.calls === 0 ? "TAINT-HOSTILE-FAIL:no-traps"
      : summary !== "TAINT-SCENE-PASS" ? "TAINT-HOSTILE-FAIL:" + summary
      : "TAINT-HOSTILE-PASS:traps=" + trap.calls + ",species=" + trap.spies;
  };
})();
