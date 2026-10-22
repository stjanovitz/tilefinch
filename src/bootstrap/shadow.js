/* Component ownership is first-use state, not part of every tiny realm. */
Object.defineProperty(globalThis, "__tilefinchCreateShadowState", {
  configurable: true,
  value(MapType, WeakRefType, deref, apply, connected, emptyArguments,
        createPrivateSlot) {
    const byHandle = new MapType(),
      // Keep host/root cycles on the objects, not in global WeakMap values.
      peers = createPrivateSlot(),
      peerOf = (target) => peers.get(target) ?? null,
      // DOM_BRIDGE_SHADOW_ROOT_LIMIT, the native root table's bound.
      ROOT_LIMIT = 256;
    let prepared = false;
    const forHandle = handle => {
      const entry = byHandle.get(handle),
        root = entry?.root || (entry?.reference && deref
          ? apply(deref, entry.reference, emptyArguments) : null);
      if (!root) { if (entry) byHandle.delete(handle); return null; }
      if (!entry.root && connected(handle)) entry.root = root;
      return root;
    };
    return {
      peer: peerOf,
      forHost(host, connectivity) {
        if (typeof host === "number") return forHandle(host);
        const peer = peerOf(host), handle = host?.__handle;
        if (peer && connectivity !== undefined && handle !== undefined) {
          const entry = byHandle.get(Number(handle));
          if (entry?.reference) entry.root = connected(handle) ? peer : null;
        }
        return peer || (handle === undefined ? null : forHandle(Number(handle)));
      },
      retire(handle) { byHandle.delete(handle); },
      remember(host, root) {
        peers.set(host, root);
        peers.set(root, host);
        // Match the native root table's hard bound; dead weak entries must
        // not prevent later connected components from retaining identity.
        if (byHandle.size >= ROOT_LIMIT)
          for (const [handle, entry] of byHandle)
            if (!entry.root && entry.reference &&
                !apply(deref, entry.reference, emptyArguments))
              byHandle.delete(handle);
        if (host.__handle !== undefined && byHandle.size < ROOT_LIMIT) {
          let reference = null;
          try { if (WeakRefType) reference = new WeakRefType(root); }
          catch (_) { /* Retain identity if optional reclamation is refused. */ }
          byHandle.set(Number(host.__handle), { root, reference });
        }
      },
      weaken() {
        let changed = false;
        for (const [handle, entry] of byHandle)
          if (entry.root && entry.reference && !connected(handle)) {
            entry.root = null;
            changed = true;
          }
        return changed;
      },
      prepare(root, prototype, nativeLayer, fallback) {
        if (prepared) return true;
        const shared = nativeLayer(Object.getPrototypeOf(prototype));
        try { Object.setPrototypeOf(prototype, shared); prepared = true; }
        catch (_) { fallback(root); }
        return prepared;
      },
    };
  },
});
