/* DOM0 form collections: select.options as an editable live
   HTMLOptionsCollection, form.elements and the form's named and indexed
   properties, RadioNodeList. Built on the first such read, so a realm whose
   script never asks pays nothing for them. platform.js captures every realm
   value this module reads before page script runs and passes them in. */
Object.defineProperty(globalThis, "__tilefinchCreateFormCollections", {
  configurable: true,
  value({
    selectionOf,
    selectedIndexOf,
    setSelectedIndex,
    formControlEntry,
    formControls,
    controlType,
    createPrivateSlot,
    collectionPrototype,
    doc,
    Node,
    HTMLInputElement,
    HTMLOptionElement,
    HTMLOptGroupElement,
    domException,
    MapType,
    WeakMapType,
    ProxyType,
    objectCreate,
    defineProperty,
    defineProperties,
    freeze,
  }) {
    /* RadioNodeList: the controls sharing one name, whose value is the
       checked radio button's. */
    const radioNodeList = (controls) => {
      const list = controls.slice(),
        radios = () => list.filter((control) =>
          control instanceof HTMLInputElement &&
          controlType(control) === "radio");
      defineProperties(list, {
        item: { value: (index) => list[Number(index)] ?? null },
        value: {
          get() {
            return radios().find((control) => control.checked)?.value ?? "";
          },
          set(value) {
            value = String(value);
            const chosen = radios().find((control) => control.value === value);
            if (chosen) chosen.checked = true;
          },
        },
      });
      return freeze(list);
    },
    /* HTMLFormControlsCollection's named properties: each id and name of a
       listed control, as that control or, when several share it, as a
       RadioNodeList. A name write moves the structural version (native), so
       a cached list never answers for a renamed control. */
    controlNames = (values) => {
      const groups = new MapType();
      for (const control of values)
        for (const name of [control.id, control.getAttribute?.("name")]) {
          if (!name) continue;
          const group = groups.get(name);
          if (!group) groups.set(name, [control]);
          else if (!group.includes(control)) group.push(control);
        }
      const names = new MapType();
      for (const [name, group] of groups)
        names.set(name, group.length === 1 ? group[0] : radioNodeList(group));
      return names;
    },
    /* The list form.elements hands the page: the cached controls with their
       names stamped on as own, non-enumerable properties that never shadow
       the list's members (length, item, Array methods) or its indices. Built
       once per cached list, only when the page asks. */
    publishedNames = new WeakMapType(),
    publishedFormControls = (form) => {
      const entry = formControlEntry(form);
      if (entry?.published) return entry.published;
      const list = entry ? entry.values.slice() : formControls(form),
        names = controlNames(list);
      for (const [name, control] of names)
        if (!(name in list) && String(name >>> 0) !== name)
          defineProperty(list, name, { value: control });
      defineProperty(list, "item", {
        value: (index) => list[Number(index)] ?? null,
      });
      defineProperty(list, "namedItem", {
        value: (name) => names.get(String(name)) ?? null,
      });
      freeze(list);
      publishedNames.set(list, names);
      if (entry) entry.published = list;
      return list;
    },
    /* form[name] and form[index]: the form's named and indexed properties,
       for string keys nothing on the element or its prototypes defines
       (platform.js filters the rest). */
    formNamedProperty = (form, key) => {
      const list = publishedFormControls(form);
      if (String(key >>> 0) === key) return list[key];
      return publishedNames.get(list)?.get(key);
    };
    /* HTMLSelectElement.add and HTMLOptionsCollection.add. */
    const addOption = (select, element, before = null) => {
        if (
          !(element instanceof HTMLOptionElement) &&
          !(element instanceof HTMLOptGroupElement)
        )
          throw new TypeError("Option or optgroup required");
        let reference = null;
        if (typeof before === "number" || (before !== null && !(before instanceof Node)))
          reference = selectionOf(select).options[Number(before)] ?? null;
        else if (before !== null) {
          if (!select.contains(before) || before === select)
            throw domException("Reference is not in this select", "NotFoundError");
          reference = before;
        }
        if (reference) reference.parentNode.insertBefore(element, reference);
        else select.appendChild(element);
      },
      /* remove(long index): an index outside the list removes nothing. */
      removeOptionAt = (select, index) => {
        index = Number(index) | 0;
        const options = selectionOf(select).options;
        if (index >= 0 && index < options.length) options[index].remove();
      },
      appendBlankOptions = (select, count) => {
        const owner = select.ownerDocument || doc,
          fragment = owner.createDocumentFragment();
        for (let made = 0; made < count; made++)
          fragment.appendChild(owner.createElement("option"));
        select.appendChild(fragment);
      },
      /* Setting length truncates, or appends blank options; a growth of more
         than 100,000 does nothing, as in the standard. */
      setOptionsLength = (select, value) => {
        const length = Number(value) >>> 0,
          options = selectionOf(select).options;
        if (length < options.length)
          for (const option of options.slice(length)) option.remove();
        else if (length - options.length <= 100000)
          appendBlankOptions(select, length - options.length);
      },
      setOptionAt = (select, index, value) => {
        if (value === null || value === undefined) {
          removeOptionAt(select, index);
          return;
        }
        if (!(value instanceof HTMLOptionElement))
          throw new TypeError("HTMLOptionsCollection members must be options");
        const options = selectionOf(select).options,
          gap = index - options.length;
        if (gap >= 0) {
          if (gap > 0) appendBlankOptions(select, gap);
          select.appendChild(value);
        } else if (options[index] !== value)
          options[index].parentNode.replaceChild(value, options[index]);
      },
      optionsCollectionPrototype = objectCreate(collectionPrototype, {
        [Symbol.toStringTag]: {
          configurable: true,
          value: "HTMLOptionsCollection",
        },
      }),
      /* select.options: one live HTMLOptionsCollection per select, kept on
         the select like selectedOptions. Reads use the select's memoized
         option list, so walking options[i] is linear; writes (length, an
         index, add, remove, selectedIndex) edit the select. */
      optionsCollections = createPrivateSlot(),
      liveOptions = (select) => {
        let collection = optionsCollections.get(select);
        if (collection) return collection;
        const values = () => selectionOf(select).options,
          isIndex = (key) => typeof key === "string" && String(key >>> 0) === key,
          target = objectCreate(optionsCollectionPrototype);
        defineProperties(target, {
          item: { value: (index) => values()[Number(index) >>> 0] ?? null },
          namedItem: {
            value: (name) => {
              name = String(name);
              return values().find(
                (option) => option.id === name || option.getAttribute("name") === name,
              ) ?? null;
            },
          },
          add: { value: (element, before = null) => addOption(select, element, before) },
          remove: { value: (index) => removeOptionAt(select, index) },
          [Symbol.iterator]: { value: () => values().slice()[Symbol.iterator]() },
        });
        collection = new ProxyType(target, {
          get(object, key) {
            if (key === "length") return values().length;
            if (key === "selectedIndex") return selectedIndexOf(select);
            if (isIndex(key)) return values()[Number(key)];
            return object[key];
          },
          set(object, key, value) {
            if (key === "length") setOptionsLength(select, value);
            else if (key === "selectedIndex") setSelectedIndex(select, value);
            else if (isIndex(key)) setOptionAt(select, Number(key), value);
            else object[key] = value;
            return true;
          },
          has(object, key) {
            if (key === "length" || key === "selectedIndex") return true;
            if (isIndex(key)) return Number(key) < values().length;
            return key in object;
          },
        });
        optionsCollections.set(select, collection);
        return collection;
      };
    return {
      liveOptions,
      addOption,
      removeOptionAt,
      setOptionsLength,
      publishedFormControls,
      formNamedProperty,
    };
  },
});
