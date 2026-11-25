/* Namespace-aware Attr writes and the bounded weak index used to copy
   mirrors. Ordinary realms need neither: build this only on the first
   namespace operation that can create mirror state. The eager consumer
   supplies its private bridges; the hook publisher hands it the factory
   without exposing it to author script. */
globalThis.__tilefinchEventInterfaces.claimHooks()?.("__tilefinchDOMAttributesFactory",
  function (
    realmGlobal, ancestorApply, emptyTaskArguments, nativeNodeReceiverDescriptors,
    namespaceMirrorRef,
    derefReference, createAttributeState, emptyNamespaceAttributes,
    cloneDepthLimit, ShadowRoot, Attr,
    attributeState, attributeKey, attributeRecord,
    nativeSetAttribute, nativeRemoveAttribute,
    invalidateInlineEventHandler, HTMLIFrameElement, HTMLDetailsElement,
    HTMLInputElement, domShared, enforceDetailsGroup,
  ) {
    const { DOMException, TypeError, RangeError, Map } = realmGlobal.__tilefinchEventInterfaces;
    /* Only a bounded weak index backs the clone preflight. If a page has
       too many mirror owners to index, copying still works by falling
       back to a full traversal. Ordinary Attr caches are not indexed. */
    const namespaceMirrorOwnerLimit = 128;
    const namespaceAttributes = (node, create = false) => {
        let values = mirror?.get(node);
        if (!values && create) {
          if (!mirror) {
            mirror = realmGlobal.__tilefinchCreatePrivateWeakMap();
            namespaceMirrorOwners = namespaceMirrorRef ? createAttributeState(null) : null;
          }
          values = [];
          mirror.set(node, values);
          if (namespaceMirrorOwners) {
            if (namespaceMirrorOwnerCount >= namespaceMirrorOwnerLimit)
              pruneNamespaceMirrorOwners();
            if (namespaceMirrorOwnerCount < namespaceMirrorOwnerLimit)
              namespaceMirrorOwners[namespaceMirrorOwnerCount++] = new namespaceMirrorRef(node);
            else
              namespaceMirrorOwners = null;
          }
          namespaceMirrorLiveOwnerCount++;
        }
        /* Ordinary HTML reads need no namespace mirror. Keep the empty case
           shared until a write actually introduces mirrored attributes. */
        return values || emptyNamespaceAttributes;
      },
      pruneNamespaceMirrorOwners = (removedOwner) => {
        let write = 0;
        for (let index = 0; index < namespaceMirrorOwnerCount; index++) {
          const ref = namespaceMirrorOwners[index];
          const owner = derefReference(ref);
          if (!owner) namespaceMirrorLiveOwnerCount--;
          else if (owner !== removedOwner)
            namespaceMirrorOwners[write++] = ref;
        }
        for (let index = write; index < namespaceMirrorOwnerCount; index++)
          delete namespaceMirrorOwners[index];
        namespaceMirrorOwnerCount = write;
      },
      dropNamespaceMirror = (owner, values, index) => {
        values.splice(index, 1);
        if (values.length) return;
        mirror.delete(owner);
        /* After overflow, collected owners are no longer observable, so
           this is an upper bound. Explicit removals can still prove that
           no mirrors remain and restart the bounded index. */
        if (--namespaceMirrorLiveOwnerCount === 0) {
          mirror = namespaceMirrorOwners = undefined;
          namespaceMirrorOwnerCount = 0;
          return;
        }
        if (namespaceMirrorOwners) pruneNamespaceMirrorOwners(owner);
      },
      /* Only walk a source subtree with mirrors. Check the owners' current
         parent chains so a move needs no ancestor bookkeeping and unrelated
         clones do not wrap their descendants. */
      hasNamespaceMirrors = (source, deep) => {
        if (mirror?.get(source)?.length) return true;
        if (!deep) return false;
        if (namespaceMirrorOwners === null) return true;
        for (let index = 0; index < namespaceMirrorOwnerCount; index++)
          /* Native cloning has already verified that no copied descendant
             can be more than cloneDepthLimit links from this root. */
          for (let at = derefReference(namespaceMirrorOwners[index]), depth = 0;
               at && depth <= cloneDepthLimit && !(at instanceof ShadowRoot);
               at = ancestorApply(nativeNodeReceiverDescriptors.parentNode.get,
                                  at, emptyTaskArguments), depth++)
            if (at === source) return true;
        return false;
      },
      /* The native clone copies each host value under its lower-case name;
         the mirror keeps the attributes' independent namespace and case. */
      copyNamespaceMirrors = (source, copy, deep) => {
        if (!hasNamespaceMirrors(source, deep)) return;
        const stack = [source, copy];
        while (stack.length) {
          const to = stack.pop(), from = stack.pop(),
            records = mirror?.get(from);
          for (let at = 0; records && at < records.length; at++) {
            const record = records[at];
            namespaceAttributes(to, true).push(attributeRecord(to,
              record.__tilefinchAttributeName, record.__tilefinchAttributeValue,
              record.__tilefinchAttributeNamespace,
              record.__tilefinchAttributePrefix));
          }
          for (let a = deep && from.firstElementChild,
                 b = deep && to.firstElementChild; a && b;
               a = a.nextElementSibling, b = b.nextElementSibling)
            stack.push(a, b);
        }
      };
    let mirror, namespaceMirrorOwners, namespaceMirrorOwnerCount = 0,
      namespaceMirrorLiveOwnerCount = 0;
    /* Native layout has one lower-case slot for a qualified name. When
       several DOM attributes share it, project the first in list order and
       keep every namespace/local-name pair independently in the mirror. */
    const nativeAttributeName = (name) => name.toLowerCase(),
      projectAttribute = (owner, name, attributes) => {
        const projected = attributes.find((item) =>
          nativeAttributeName(item.name) === name);
        if (projected)
          nativeSetAttribute(owner.__handle, projected.name, projected.value);
        else
          nativeRemoveAttribute(owner.__handle, name);
      },
      writeAttributeNS = (owner, namespace, name, localName, prefix, value,
                          supplied = null, byName = false) => {
        const attributes = [...owner.attributes],
          index = attributes.findIndex((item) =>
            item.namespaceURI === namespace && item.localName === localName),
          existing = attributes[index] || null;
        if (supplied && existing === supplied) return supplied;
        const oldValue = existing?.value ?? null,
          /* setAttributeNS changes the existing value; setAttributeNode
             instead installs the supplied Attr, including its prefix. */
          hostName = nativeAttributeName(!supplied && existing ? existing.name : name),
          oldHostName = existing && nativeAttributeName(existing.name),
          mirrors = namespaceAttributes(owner),
          mirrorIndex = mirrors.indexOf(existing),
          conflict = attributes.find((item) => item !== existing &&
            nativeAttributeName(item.name) === hostName),
          collision = conflict && !mirrors.includes(conflict) ? conflict : null,
          mirrored = mirrorIndex >= 0 || namespace !== null ||
            name !== hostName || !!conflict;
        if ((mirrored && mirrorIndex < 0 ? 1 : 0) + (collision ? 1 : 0) >
            64 - mirrors.length)
          throw new RangeError("attribute limit exceeded");

        const state = attributeState(owner, true),
          record = supplied || existing || attributeRecord(owner, name, value, namespace, prefix);
        if (supplied) {
          const order = (state.order ||= []), at = order.indexOf(existing);
          if (at >= 0) order[at] = supplied;
          else order.push(supplied);
          (state.objects ||= new Map()).set(attributeKey(namespace, localName), supplied);
          supplied.__tilefinchAttributeOwner = owner;
          supplied.__tilefinchAttributeOwnerDocument = owner.ownerDocument;
          if (existing) existing.__tilefinchAttributeOwner = null;
        } else {
          record.__tilefinchAttributeValue = value;
        }
        if (index >= 0) attributes[index] = record;
        else attributes.push(record);
        if (mirrored) {
          const values = namespaceAttributes(owner, true);
          if (collision) values.push(collision);
          if (mirrorIndex >= 0) values[mirrorIndex] = record;
          else values.push(record);
          if (conflict) values.sort((a, b) =>
            attributes.indexOf(a) - attributes.indexOf(b));
        }
        state.memo = null;
        if (oldHostName && oldHostName !== hostName)
          projectAttribute(owner, oldHostName, attributes);
        projectAttribute(owner, hostName, attributes);

        if (namespace === null) {
          realmGlobal.__tilefinchCanvasAttributeChanged?.(owner, localName);
          if (supplied || byName) {
            if (localName === "id") realmGlobal.__tilefinchExposeNamedProperty(value);
            if (localName === "style" && owner.__detachedOwner)
              realmGlobal.__tilefinchInvalidateDetachedStyle(owner);
            invalidateInlineEventHandler(owner, localName);
            if (owner instanceof HTMLIFrameElement && owner.isConnected &&
                (localName === "srcdoc" || (localName === "src" && !owner.hasAttribute("srcdoc"))))
              realmGlobal.__tilefinchLoadLocalFrame?.(owner, true);
          }
        }
        realmGlobal.__tilefinchCustomElementAttributeChanged?.(
          owner, localName, oldValue, value, namespace);
        realmGlobal.__tilefinchNotifyMutation?.(
          owner, "attributes", localName, [], [], oldValue, null, null, namespace);
        if (namespace === null &&
            (localName === "open" && oldValue === null || localName === "name") &&
            owner instanceof HTMLDetailsElement) {
          if (localName === "open") domShared.detailsOpenChanged?.(owner, true);
          enforceDetailsGroup(owner, localName === "open");
        }
        if (namespace === null && owner instanceof HTMLInputElement &&
            (localName === "type" || localName === "name" || localName === "form" || localName === "checked"))
          domShared.inputAttributeChanged?.(owner, localName, oldValue, value);
        return existing;
      },
      setNamedAttribute = (map, attribute) => {
        if (!(attribute instanceof Attr))
          throw new TypeError("setNamedItem requires an Attr");
        const owner = map.__tilefinchAttributeOwner,
          { namespaceURI, name, localName, prefix, value, ownerElement } = attribute;
        if (ownerElement && ownerElement !== owner)
          throw new DOMException("Attribute is already in use", "InUseAttributeError");
        const detached = !owner.__handle && owner.__detachedAttributeNodes;
        if (detached) {
          const index = detached.findIndex((item) =>
              item.namespaceURI === namespaceURI && item.localName === localName),
            existing = detached[index] || null;
          if (existing === attribute) return attribute;
          if (existing) {
            detached[index] = attribute;
            existing.__tilefinchAttributeOwner = null;
          } else {
            detached.push(attribute);
          }
          attribute.__tilefinchAttributeOwner = owner;
          attribute.__tilefinchAttributeOwnerDocument = owner.ownerDocument;
          if (namespaceURI === null && localName === "style")
            realmGlobal.__tilefinchInvalidateDetachedStyle(owner);
          realmGlobal.__tilefinchCustomElementAttributeChanged?.(
            owner, localName, existing?.value ?? null, value, namespaceURI);
          return existing;
        }
        return writeAttributeNS(owner, namespaceURI, name, localName, prefix, value, attribute);
      };
    return {
      __proto__: null,
      namespaceAttributes,
      readNamespaceMirror: (node) => mirror?.get(node),
      dropNamespaceMirror, copyNamespaceMirrors, projectAttribute,
      writeAttributeNS, setNamedAttribute,
    };
  });
