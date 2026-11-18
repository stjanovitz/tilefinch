/* DOM0 form collections: select.options as an editable live
   HTMLOptionsCollection, form.elements and the form's named and indexed
   properties, RadioNodeList. Built on the first such read, so a realm whose
   script never asks pays nothing for them. platform.js captures every realm
   value this module reads before page script runs and passes them in. */
globalThis.__tilefinchEventInterfaces.defineProperty(globalThis,
    "__tilefinchCreateFormCollections", {
  __proto__: null,
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
    DateType,
  }) {
    /* Loaded after the page may have replaced these globals: build
       errors, promises and platform objects with the constructors
       hardening.js captured. */
    const { TypeError } = globalThis.__tilefinchEventInterfaces;
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
    /* Keep typed form conversions off pages that use only text fields. */
    const calendarNumber = (type, value, isCalendarDate) => {
      if (type === "month") {
        const year = Number(value.slice(0, -3)), month = Number(value.slice(-2));
        return year >= 1 && month >= 1 && month <= 12
          ? (year - 1970) * 12 + month - 1 : NaN;
      }
      const dayNumber = date => {
        if (!isCalendarDate(date)) return NaN;
        const d = new DateType(0);
        d.setUTCFullYear(Number(date.slice(0, -6)), Number(date.slice(-5, -3)) - 1,
          Number(date.slice(-2)));
        return d.getTime();
      };
      if (type === "date") return dayNumber(value);
      if (type === "week") {
        const year = value.slice(0, -4), week = Number(value.slice(-2)),
          first = y => {
            const d = dayNumber(y + "-01-04");
            return d - ((new DateType(d).getUTCDay() + 6) % 7) * 86400000;
          },
          start = first(year), next = first(String(Number(year) + 1).padStart(4, "0")),
          chosen = start + (week - 1) * 604800000;
        return week >= 1 && chosen < next ? chosen : NaN;
      }
      const at = type === "time" ? -1 : value.indexOf("T"),
        [h, m, s = 0] = value.slice(at + 1).split(":").map(Number);
      if (h >= 24 || m >= 60 || s >= 60) return NaN;
      return (at < 0 ? 0 : dayNumber(value.slice(0, at))) +
        Math.round(((h * 60 + m) * 60 + s) * 1000);
    };
    const rangeValue = (input, value, inputNumber) => {
      const lo = inputNumber(input.getAttribute("min")),
        hi = inputNumber(input.getAttribute("max")),
        min = Number.isFinite(lo) ? lo : 0,
        max = Math.max(min, Number.isFinite(hi) ? hi : 100),
        parsed = inputNumber(value), rawStep = input.getAttribute("step");
      let result = Number.isFinite(parsed) ? Math.min(max, Math.max(min, parsed))
        : min / 2 + max / 2;
      if (rawStep?.toLowerCase() !== "any") {
        const n = inputNumber(rawStep), step = n > 0 ? n : 1,
          attr = inputNumber(input.getAttribute("value")),
          base = Number.isFinite(lo) ? lo : Number.isFinite(attr) ? attr : 0,
          units = (result - base) / step, tolerance = 1e-9,
          low = Math.ceil((min - base) / step - tolerance),
          high = Math.floor((max - base) / step + tolerance);
        if (Number.isFinite(units) && low <= high &&
            Math.abs(units - Math.round(units)) > tolerance) {
          const snapped = base + Math.min(high, Math.max(low, Math.round(units))) * step;
          if (Number.isFinite(snapped))
            result = Math.min(max, Math.max(min, Number(snapped.toPrecision(15))));
        }
      }
      return String(result);
    };
    const dateInputString = (type, number) => {
      if (type === "month") {
        const month = Math.trunc(number), year = Math.floor(month / 12) + 1970;
        return year >= 1 ? String(year).padStart(4, "0") + "-" +
          String(month - (year - 1970) * 12 + 1).padStart(2, "0") : "";
      }
      const date = new DateType(number), year = date.getUTCFullYear();
      if (!(year >= 1)) return "";
      if (type === "week") {
        /* Thursday determines the ISO week-year. Time within the day is
           irrelevant, including before the Unix epoch. */
        date.setUTCHours(0, 0, 0, 0);
        date.setUTCDate(date.getUTCDate() + 3 - (date.getUTCDay() + 6) % 7);
        const weekYear = date.getUTCFullYear(), first = new DateType(0);
        first.setUTCFullYear(weekYear, 0, 1);
        const week = Math.floor((date - first) / 604800000) + 1;
        return weekYear >= 1 && Number.isFinite(week)
          ? String(weekYear).padStart(4, "0") + "-W" + String(week).padStart(2, "0") : "";
      }
      if (type === "time") {
        const milliseconds = ((date.getTime() % 86400000) + 86400000) % 86400000;
        return new DateType(milliseconds).toISOString().slice(11, -1)
          .replace(/\.?0+$/, "").replace(/:00$/, "");
      }
      const iso = date.toISOString(), rest = iso.slice(iso.indexOf("-") + 1),
        day = String(year).padStart(4, "0") + "-" + rest.slice(0, 5);
      return type === "date" ? day :
        day + "T" + rest.slice(6, -1).replace(/\.?0+$/, "").replace(/:00$/, "");
    };
    const calendarDate = (type, number) => {
      if (type !== "month") return new DateType(number);
      const d = new DateType(0), years = Math.floor(number / 12);
      d.setUTCFullYear(1970 + years, number - years * 12, 1);
      return d;
    };
    const decimalPlaces = (value) => {
      const [mantissa, exp] = String(value).toLowerCase().split("e"),
        point = mantissa.indexOf("."), fraction = point < 0 ? 0 : mantissa.length - point - 1;
      return Math.min(9, Math.max(0, fraction - (Number(exp) || 0)));
    };
    const stepInput = (input, type, count, inputNumber) => {
      if (type !== "number" && type !== "range")
        throw domException("Stepping does not apply to this input type", "InvalidStateError");
      const rawStep = input.getAttribute("step");
      if (rawStep?.toLowerCase() === "any")
        throw domException("Stepping is unavailable when step is any", "InvalidStateError");
      const parsed = inputNumber(rawStep), step = parsed > 0 ? parsed : 1,
        min = inputNumber(input.getAttribute("min")), max = inputNumber(input.getAttribute("max")),
        authored = inputNumber(input.getAttribute("value")),
        hasMin = Number.isFinite(min), hasMax = Number.isFinite(max),
        base = hasMin ? min : Number.isFinite(authored) ? authored : 0,
        current = inputNumber(input.value);
      const value = Number.isFinite(current) ? current : 0;
      count = Math.trunc(Number(count) || 0);
      const scale = 10 ** Math.max(decimalPlaces(step), decimalPlaces(base),
        decimalPlaces(value), hasMin ? decimalPlaces(min) : 0,
        hasMax ? decimalPlaces(max) : 0),
        stepUnits = Math.max(1, Math.round(step * scale)), baseUnits = Math.round(base * scale);
      let units = Math.round(value * scale), remainder = (units - baseUnits) % stepUnits;
      if (remainder < 0) remainder += stepUnits;
      if (remainder && count > 0) { units += stepUnits - remainder; count--; }
      else if (remainder && count < 0) { units -= remainder; count++; }
      units += count * stepUnits;
      const low = hasMin ? baseUnits +
          Math.ceil((Math.round(min * scale) - baseUnits) / stepUnits) * stepUnits : -Infinity,
        high = hasMax ? baseUnits +
          Math.floor((Math.round(max * scale) - baseUnits) / stepUnits) * stepUnits : Infinity;
      if (low <= high) input.value = String(Math.max(low, Math.min(high, units)) / scale);
    };
    return {
      calendarNumber,
      calendarDate,
      rangeValue,
      dateInputString,
      stepInput,
      liveOptions,
      addOption,
      removeOptionAt,
      setOptionsLength,
      publishedFormControls,
      formNamedProperty,
    };
  },
});
