(() => {
  "use strict";
  const NativeDate = globalThis.Date,
    proto = NativeDate.prototype;
  const days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"],
    months = [
      "Jan",
      "Feb",
      "Mar",
      "Apr",
      "May",
      "Jun",
      "Jul",
      "Aug",
      "Sep",
      "Oct",
      "Nov",
      "Dec",
    ],
    pad = (value) => String(value).padStart(2, "0");
  const timeOf = (value) => NativeDate.prototype.getTime.call(value);
  const dateText = (value) => {
    const time = timeOf(value);
    if (!Number.isFinite(time)) return "Invalid Date";
    return (
      days[value.getUTCDay()] +
      " " +
      months[value.getUTCMonth()] +
      " " +
      pad(value.getUTCDate()) +
      " " +
      value.getUTCFullYear()
    );
  };
  const timeText = (value) => {
      const time = timeOf(value);
      if (!Number.isFinite(time)) return "Invalid Date";
      return (
        pad(value.getUTCHours()) +
        ":" +
        pad(value.getUTCMinutes()) +
        ":" +
        pad(value.getUTCSeconds()) +
        " GMT+0000 (Coordinated Universal Time)"
      );
    },
    fullText = (value) =>
      Number.isFinite(timeOf(value))
        ? dateText(value) + " " + timeText(value)
        : "Invalid Date";
  const localeDate = (value) => {
    const time = timeOf(value);
    if (!Number.isFinite(time)) throw new RangeError("Invalid time value");
    return (
      value.getUTCMonth() +
      1 +
      "/" +
      value.getUTCDate() +
      "/" +
      value.getUTCFullYear()
    );
  };
  const localeTime = (value) => {
    const time = timeOf(value);
    if (!Number.isFinite(time)) throw new RangeError("Invalid time value");
    const hour = value.getUTCHours();
    return (
      (hour % 12 || 12) +
      ":" +
      pad(value.getUTCMinutes()) +
      ":" +
      pad(value.getUTCSeconds()) +
      " " +
      (hour < 12 ? "AM" : "PM")
    );
  };
  const replace = (name, implementation) => {
    const descriptor = Object.getOwnPropertyDescriptor(proto, name);
    if (!descriptor || typeof descriptor.value !== "function") return;
    Object.defineProperty(proto, name, {
      ...descriptor,
      value: new Proxy(descriptor.value, {
        apply(target, thisArg, args) {
          return implementation(thisArg, args, target);
        },
      }),
    });
  };
  replace("toString", fullText);
  replace("toDateString", dateText);
  replace("toTimeString", timeText);
  replace("toLocaleDateString", localeDate);
  replace("toLocaleTimeString", localeTime);
  replace(
    "toLocaleString",
    (value) => localeDate(value) + ", " + localeTime(value),
  );
  replace("getTimezoneOffset", () => 0);
  for (const pair of [
    ["getDate", "getUTCDate"],
    ["getDay", "getUTCDay"],
    ["getFullYear", "getUTCFullYear"],
    ["getHours", "getUTCHours"],
    ["getMilliseconds", "getUTCMilliseconds"],
    ["getMinutes", "getUTCMinutes"],
    ["getMonth", "getUTCMonth"],
    ["getSeconds", "getUTCSeconds"],
    ["setDate", "setUTCDate"],
    ["setFullYear", "setUTCFullYear"],
    ["setHours", "setUTCHours"],
    ["setMilliseconds", "setUTCMilliseconds"],
    ["setMinutes", "setUTCMinutes"],
    ["setMonth", "setUTCMonth"],
    ["setSeconds", "setUTCSeconds"],
  ])
    replace(pair[0], (value, args) => proto[pair[1]].apply(value, args));
  replace("getYear", (value) => value.getUTCFullYear() - 1900);
  replace("setYear", (value, args) => {
    let year = Number(args[0]);
    if (Number.isFinite(year) && year >= 0 && year <= 99) year += 1900;
    return value.setUTCFullYear(year);
  });
  const nativeNow = NativeDate.now,
    now = new Proxy(nativeNow, {
      apply() {
        return __tilefinchDateNow(0);
      },
    });
  Object.defineProperty(NativeDate, "now", {
    ...Object.getOwnPropertyDescriptor(NativeDate, "now"),
    value: now,
  });
  let DateFacade;
  DateFacade = new Proxy(NativeDate, {
    apply() {
      const value = new NativeDate(__tilefinchDateNow(1));
      return fullText(value);
    },
    construct(target, args, newTarget) {
      return Reflect.construct(
        target,
        args.length === 0 ? [__tilefinchDateNow(2)] : args,
        newTarget,
      );
    },
  });
  Object.defineProperty(proto, "constructor", {
    value: DateFacade,
    writable: true,
    enumerable: false,
    configurable: true,
  });
  globalThis.Date = DateFacade;
  Object.defineProperty(globalThis, "__tilefinchDeterministicDateFacade", {
    value: DateFacade,
    writable: false,
    enumerable: false,
    configurable: true,
  });
})();
