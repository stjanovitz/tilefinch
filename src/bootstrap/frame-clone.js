/* Cross-document postMessage crosses independent QuickJS runtimes. JSON is
   not a structured-clone transport: it drops undefined and non-finite
   numbers, destroys aliases/cycles, and turns typed arrays into records.
   Keep a small, versioned graph wire instead. The final 64 KiB native queue
   bound remains authoritative; the limits here stop hostile graphs before
   serialization creates proportional intermediate state. Built the first
   time a realm posts or receives a window or frame message, so a realm
   whose script never does pays nothing for it. platform.js captures the
   realm values this module reads before page script runs and passes them
   in; DOMException comes from the table hardening.js captured. The body
   reads no other global, calls no page-replaceable method on its own
   arrays and maps, and walks arrays by index. It walks a page's Map and
   Set with their captured forEach (an iterator's next() is a page-patchable
   prototype method) and reads a view's buffer, offset and length through
   the captured getters. A wire's fields are JSON: an object where a string
   or number belongs is refused rather than converted, which would call a
   page-patchable toString or valueOf. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateFrameClone", {
  __proto__: null,
  configurable: true,
  value({
    constructors: { Array, BigInt, Date, Map, Number, RegExp, Set, String },
    errorTypes,
    viewTypes,
    ordinaryClonePrototypeKind,
    isProxy,
    base64Encode,
    base64Decode,
    TrustedUint8Array,
    trustedCharCodeAt,
    trustedStringSlice,
    trustedNumberIsNaN,
    trustedNumberIsInteger,
    trustedObjectIs,
    trustedObjectTag,
    trustedObjectKeys,
    trustedObjectPrototype,
    trustedDefineProperty,
    trustedArrayIsArray,
    trustedArrayBufferIsView,
    trustedDateTime,
    trustedRegExpSource,
    trustedRegExpFlags,
    trustedMapForEach,
    trustedMapGet,
    trustedMapSet,
    trustedSetForEach,
    trustedSetAdd,
    trustedTypedArrayBuffer,
    trustedTypedArrayByteOffset,
    trustedTypedArrayLength,
    trustedTypedArrayName,
    trustedDataViewBuffer,
    trustedDataViewByteOffset,
    trustedDataViewByteLength,
    trustedJSONStringify,
    trustedJSONParse,
  }) {
    const { DOMException } = globalThis.__tilefinchEventInterfaces,
      frameCloneMagic = "tilefinch-clone-v1",
      frameCloneObjectLimit = 512,
      frameClonePropertyLimit = 4096,
      frameCloneDepthLimit = 64,
      frameCloneBinaryLimit = 48 * 1024,
      frameCloneWireLimit = 64 * 1024,
      frameCloneError = (message) =>
        new DOMException(String(message), "DataCloneError"),
      field = (value) => {
        if (value !== null && typeof value === "object")
          throw frameCloneError("message contains an invalid field");
        return value;
      },
      frameCloneBytesToBase64 = (bytes) => {
        if (trustedTypedArrayLength(bytes) > frameCloneBinaryLimit)
          throw frameCloneError("message binary data exceeds bounded size");
        return base64Encode(bytes);
      },
      frameCloneBytesFromBase64 = (text) => {
        const binary = base64Decode(String(field(text)));
        if (binary.length > frameCloneBinaryLimit)
          throw frameCloneError("message binary data exceeds bounded size");
        const bytes = new TrustedUint8Array(binary.length);
        for (let at = 0; at < binary.length; at++)
          bytes[at] = trustedCharCodeAt(binary, at);
        return bytes;
      },
      encodeFrameMessage = (value) => {
        const memory = new Map(),
          nodes = [];
        let propertyCount = 0,
          diagnosticEvent = "";
        const encode = (item, depth) => {
          if (depth > frameCloneDepthLimit)
            throw frameCloneError("message nesting exceeds bounded depth");
          if (item === null || typeof item === "string" ||
              typeof item === "boolean") return item;
          if (item === undefined) return ["u"];
          if (typeof item === "number") {
            if (trustedNumberIsNaN(item)) return ["n", "nan"];
            if (item === Infinity) return ["n", "+inf"];
            if (item === -Infinity) return ["n", "-inf"];
            if (trustedObjectIs(item, -0)) return ["n", "-0"];
            return item;
          }
          if (typeof item === "bigint") return ["bi", String(item)];
          if (typeof item === "function" || typeof item === "symbol")
            throw frameCloneError("message contains an unsupported value");
          /* A Proxy is never structured-cloneable. Reject it before tag,
             prototype, or key inspection so page traps cannot run as a side
             effect of cross-realm postMessage. */
          if (isProxy(item))
            throw frameCloneError("Proxy values could not be cloned");
          const seen = trustedMapGet(memory, item);
          if (seen !== undefined) return ["r", seen];
          if (nodes.length >= frameCloneObjectLimit)
            throw frameCloneError("message object count exceeds bounded size");
          const id = nodes.length,
            tag = trustedObjectTag(item);
          trustedMapSet(memory, item, id);
          nodes[id] = null;
          if (tag === "[object ArrayBuffer]") {
            nodes[id] = {
              t: "b",
              v: frameCloneBytesToBase64(new TrustedUint8Array(item)),
            };
            return ["r", id];
          }
          if (trustedArrayBufferIsView(item)) {
            /* A view that is not a typed array is a DataView. */
            const name = trustedTypedArrayName(item);
            nodes[id] = name === undefined ? {
              t: "v", c: "DataView",
              b: encode(trustedDataViewBuffer(item), depth + 1),
              o: trustedDataViewByteOffset(item),
              l: trustedDataViewByteLength(item),
            } : {
              t: "v", c: name,
              b: encode(trustedTypedArrayBuffer(item), depth + 1),
              o: trustedTypedArrayByteOffset(item),
              l: trustedTypedArrayLength(item),
            };
            return ["r", id];
          }
          if (tag === "[object Date]") {
            nodes[id] = { t: "d", v: trustedDateTime(item) };
            return ["r", id];
          }
          if (tag === "[object RegExp]") {
            nodes[id] = {
              t: "x", s: trustedRegExpSource(item), f: trustedRegExpFlags(item),
            };
            return ["r", id];
          }
          if (tag === "[object Map]") {
            const entries = [];
            trustedMapForEach(item, (entryValue, entryKey) => {
              if (++propertyCount > frameClonePropertyLimit)
                throw frameCloneError("message item count exceeds bounded size");
              entries[entries.length] = [
                encode(entryKey, depth + 1), encode(entryValue, depth + 1),
              ];
            });
            nodes[id] = { t: "m", e: entries };
            return ["r", id];
          }
          if (tag === "[object Set]") {
            const entries = [];
            trustedSetForEach(item, (entry) => {
              if (++propertyCount > frameClonePropertyLimit)
                throw frameCloneError("message item count exceeds bounded size");
              entries[entries.length] = encode(entry, depth + 1);
            });
            nodes[id] = { t: "s", e: entries };
            return ["r", id];
          }
          if (trustedStringSlice(tag, -6) === "Error]") {
            nodes[id] = {
              t: "e", n: String(item.name || "Error"),
              m: String(item.message || ""), s: String(item.stack || ""),
            };
            return ["r", id];
          }
          const isArray = trustedArrayIsArray(item),
            prototype = trustedObjectPrototype(item);
          /* Structured clone serializes ordinary class instances as ordinary
             objects in the receiving realm. Requiring Object.prototype or a
             null prototype incorrectly rejected those instances even though
             their enumerable data is cloneable. Keep platform/native objects
             outside the bounded profile through the same prototype classifier
             used by the Worker clone path. */
          if (!isArray && !(tag === "[object Object]" &&
                            ordinaryClonePrototypeKind(prototype) !== 0))
            throw frameCloneError("message contains an unsupported object");
          if (isArray && item.length > frameClonePropertyLimit)
            throw frameCloneError("message array length exceeds bounded size");
          const properties = [],
            keys = trustedObjectKeys(item);
          if (propertyCount + keys.length > frameClonePropertyLimit)
            throw frameCloneError("message property count exceeds bounded size");
          propertyCount += keys.length;
          for (let at = 0; at < keys.length; at++) {
            const key = keys[at],
              propertyValue = item[key],
              encoded = encode(propertyValue, depth + 1);
            properties[at] = [key, encoded];
            if (depth === 0 && key === "event" &&
                typeof propertyValue === "string")
              diagnosticEvent = trustedStringSlice(propertyValue, 0, 64);
          }
          nodes[id] = {
            t: isArray ? "a" : "o",
            l: isArray ? item.length : 0,
            p: properties,
          };
          return ["r", id];
        };
        const root = encode(value, 0),
          envelope = {
            tilefinchClone: frameCloneMagic,
            event: diagnosticEvent,
            root,
            nodes,
          },
          wire = trustedJSONStringify(envelope);
        if (wire.length > frameCloneWireLimit)
          throw frameCloneError("message exceeds bounded wire size");
        return wire;
      },
      decodeFrameMessage = (wire) => {
        const text = String(field(wire));
        if (text.length > frameCloneWireLimit)
          throw frameCloneError("message exceeds bounded wire size");
        const envelope = trustedJSONParse(text);
        if (!envelope || envelope.tilefinchClone !== frameCloneMagic ||
            !trustedArrayIsArray(envelope.nodes))
          return envelope;
        if (envelope.nodes.length > frameCloneObjectLimit)
          throw frameCloneError("message object count exceeds bounded size");
        const nodes = new Array(envelope.nodes.length),
          source = envelope.nodes;
        let decodedItemCount = 0;
        for (let at = 0; at < source.length; at++) {
          const node = source[at];
          if (!node || typeof node.t !== "string")
            throw frameCloneError("message contains an invalid node");
          if (node.t === "a") {
            const length = Number(field(node.l));
            if (!trustedNumberIsInteger(length) || length < 0 ||
                length > frameClonePropertyLimit)
              throw frameCloneError("message array length exceeds bounded size");
            nodes[at] = new Array(length);
          }
          else if (node.t === "o") nodes[at] = {};
          else if (node.t === "b")
            nodes[at] = trustedTypedArrayBuffer(frameCloneBytesFromBase64(node.v));
          else if (node.t === "d") nodes[at] = new Date(Number(field(node.v)));
          else if (node.t === "x")
            nodes[at] = new RegExp(field(node.s), field(node.f));
          else if (node.t === "m") nodes[at] = new Map();
          else if (node.t === "s") nodes[at] = new Set();
          else if (node.t === "e") {
            const ErrorType = errorTypes[String(field(node.n))] ||
              errorTypes.Error;
            nodes[at] = new ErrorType(String(field(node.m) || ""));
            try { nodes[at].stack = String(field(node.s) || ""); } catch (_) {}
          } else if (node.t !== "v")
            throw frameCloneError("message contains an invalid node type");
        }
        const decode = (item) => {
          if (!trustedArrayIsArray(item)) return item;
          if (item[0] === "u") return undefined;
          if (item[0] === "bi") return BigInt(field(item[1]));
          if (item[0] === "n") {
            if (item[1] === "nan") return NaN;
            if (item[1] === "+inf") return Infinity;
            if (item[1] === "-inf") return -Infinity;
            if (item[1] === "-0") return -0;
          }
          if (item[0] !== "r" || !trustedNumberIsInteger(item[1]) ||
              item[1] < 0 || item[1] >= nodes.length)
            throw frameCloneError("message contains an invalid reference");
          return nodes[item[1]];
        };
        for (let at = 0; at < source.length; at++) {
          const node = source[at];
          if (node.t === "v") {
            /* A view's buffer is a binary ("b") node, nothing else. */
            const buffer = decode(node.b),
              constructor = viewTypes[field(node.c)];
            if (!trustedArrayIsArray(node.b) || node.b[0] !== "r" ||
                source[node.b[1]].t !== "b" || typeof constructor !== "function")
              throw frameCloneError("message contains an invalid view");
            nodes[at] = node.c === "DataView"
              ? new constructor(buffer, Number(field(node.o)),
                Number(field(node.l)))
              : new constructor(buffer, Number(field(node.o)),
                Number(field(node.l)));
          }
        }
        for (let at = 0; at < source.length; at++) {
          const node = source[at], target = nodes[at];
          if (node.t === "a" || node.t === "o") {
            if (!trustedArrayIsArray(node.p) ||
                node.p.length > frameClonePropertyLimit ||
                decodedItemCount > frameClonePropertyLimit - node.p.length)
              throw frameCloneError("message contains invalid properties");
            decodedItemCount += node.p.length;
            for (let index = 0; index < node.p.length; index++) {
              const pair = node.p[index];
              if (!trustedArrayIsArray(pair) || pair.length !== 2)
                throw frameCloneError("message contains an invalid property");
              trustedDefineProperty(target, String(field(pair[0])), {
                configurable: true, enumerable: true, writable: true,
                value: decode(pair[1]),
              });
            }
          } else if (node.t === "m") {
            if (!trustedArrayIsArray(node.e) ||
                decodedItemCount > frameClonePropertyLimit - node.e.length)
              throw frameCloneError("message contains invalid map entries");
            decodedItemCount += node.e.length;
            for (let index = 0; index < node.e.length; index++) {
              const pair = node.e[index];
              if (!trustedArrayIsArray(pair) || pair.length !== 2)
                throw frameCloneError("message contains an invalid map entry");
              trustedMapSet(target, decode(pair[0]), decode(pair[1]));
            }
          } else if (node.t === "s") {
            if (!trustedArrayIsArray(node.e) ||
                decodedItemCount > frameClonePropertyLimit - node.e.length)
              throw frameCloneError("message contains invalid set entries");
            decodedItemCount += node.e.length;
            for (let index = 0; index < node.e.length; index++)
              trustedSetAdd(target, decode(node.e[index]));
          }
        }
        return decode(envelope.root);
      };
    return { encode: encodeFrameMessage, decode: decodeFrameMessage };
  },
});
