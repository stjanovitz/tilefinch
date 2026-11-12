// JR-21: the engine keeps option selectedness, focus, indeterminate, modal and
// popover state in data-tilefinch-* attributes that native layout reads. Page
// script must not see them: not in innerHTML or outerHTML, not in the
// attribute list or dataset, and not as MutationObserver records.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};
const host = document.getElementById("host");
host.innerHTML =
  '<select id="s"><option value="a">a</option><option>b</option></select>'
  + '<input id="i"><input id="c" type="checkbox">'
  + '<div id="p" popover>pop</div><dialog id="d"><p>d</p></dialog>';
const $ = (id) => document.getElementById(id);
const select = $("s"), input = $("i"), box = $("c"), pop = $("p"), dialog = $("d");
const records = [];
new MutationObserver((list) => { for (const r of list) records.push(r); })
  .observe(host, { attributes: true, subtree: true });

select.value = "b";
input.focus();
box.indeterminate = true;
pop.showPopover();
dialog.showModal();
const leaks = (text) => text.includes("data-tilefinch");

check("engine state took effect", () => select.value === "b"
  && select.selectedIndex === 1 && document.activeElement !== null
  && box.indeterminate && pop.matches(":popover-open") && dialog.open);
check("innerHTML", () => !leaks(host.innerHTML));
check("outerHTML", () => !leaks(select.outerHTML) && !leaks(input.outerHTML)
  && !leaks(box.outerHTML) && !leaks(pop.outerHTML) && !leaks(dialog.outerHTML));
check("attribute list", () => select.options[1].attributes.length === 0
  && select.options[0].getAttributeNames().join() === "value"
  && !select.options[1].hasAttributes()
  && box.getAttributeNames().join() === "id,type"
  && pop.getAttributeNames().join() === "id,popover"
  && dialog.getAttributeNames().join() === "id,open");
check("dataset", () => Object.keys(select.options[1].dataset).length === 0
  && Object.keys(pop.dataset).length === 0);
check("showModal twice is a no-op", () => { dialog.showModal(); return dialog.open; });
check("author attributes still serialize", () => {
  select.options[0].setAttribute("data-x", "1&\"2");
  return select.options[0].outerHTML === '<option value="a" data-x="1&amp;&quot;2">a</option>';
});
queueMicrotask(() => {
  const names = records.map((record) => record.attributeName);
  expect("no marker mutation records (" + names.join() + ")",
    !names.some((name) => name.startsWith("data-tilefinch")));
  expect("author mutation records kept", names.includes("open") && names.includes("data-x"));
  globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
});
