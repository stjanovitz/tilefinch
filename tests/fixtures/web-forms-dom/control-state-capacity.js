// JR-01: dirty control state is never evicted while the control lives.
// JR-11: reading option.selected for every option is linear.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const host = document.getElementById("host");
const add = (html) => {
  const box = document.createElement("div");
  box.innerHTML = html;
  host.appendChild(box);
  return box;
};
let markup = "<select id=countries>";
for (let i = 0; i < 200; i++)
  markup += "<option value=v" + i + (i === 50 ? " selected" : "") + ">o" + i + "</option>";
add(markup + "</select>");
const countries = document.getElementById("countries");
expect("default selection", countries.value === "v50");
countries.value = "v0";
expect("long select assignment", countries.value === "v0" && countries.selectedIndex === 0);
expect("previous default cleared", countries.options[50].selected === false);
expect("selectedOptions identity", countries.selectedOptions === countries.selectedOptions
  && countries.selectedOptions.length === 1 && countries.selectedOptions[0] === countries.options[0]);
markup = "";
for (const [id, count] of [["day", 31], ["month", 12], ["year", 120]]) {
  markup += "<select id=" + id + ">";
  for (let i = 0; i < count; i++) markup += "<option value=" + i + ">" + i + "</option>";
  markup += "</select>";
}
add(markup);
const day = document.getElementById("day"), month = document.getElementById("month"),
  year = document.getElementById("year");
day.value = "15"; month.value = "6"; year.value = "100";
expect("date row", day.value + "/" + month.value + "/" + year.value === "15/6/100");
markup = "<form id=grid>";
for (let i = 0; i < 200; i++) markup += "<input name=n" + i + ">";
markup += "</form><form id=notes>";
for (let i = 0; i < 200; i++) markup += "<textarea name=t" + i + "></textarea>";
add(markup + "</form>");
const grid = document.getElementById("grid"), inputs = grid.querySelectorAll("input"),
  areas = document.getElementById("notes").querySelectorAll("textarea");
for (let i = 0; i < inputs.length; i++) { inputs[i].value = "val" + i; areas[i].value = "text" + i; }
expect("input values kept", inputs[0].value === "val0" && inputs[71].value === "val71"
  && inputs[199].value === "val199");
expect("textarea values kept", areas[0].value === "text0" && areas[199].value === "text199");
const entries = new FormData(grid), notes = new FormData(document.getElementById("notes"));
expect("form data", entries.get("n0") === "val0" && notes.get("t0") === "text0"
  && entries.get("n150") === "val150");
const boxes = [];
for (let i = 0; i < 200; i++) {
  const box = document.createElement("input");
  box.type = "checkbox";
  host.appendChild(box);
  box.indeterminate = true;
  boxes.push(box);
}
expect("indeterminate kept", boxes[0].indeterminate && boxes[199].indeterminate);
// The browser's selectedness marker is not an author mutation.
const records = [];
const observer = new MutationObserver((list) => records.push(...list));
observer.observe(countries, { attributes: true, subtree: true });
countries.value = "v7";
expect("no marker records", observer.takeRecords().length === 0);
observer.disconnect();
// JR-11: one selection read per option, not one walk of all options.
markup = "<form id=one><select name=pick id=pick>";
for (let i = 0; i < 250; i++) markup += "<option value=c" + i + ">c" + i + "</option>";
add(markup + "</select></form>");
const pick = document.getElementById("pick");
// Option wrappers share the prototype that holds the native receiver's
// members; count attribute probes there.
const optionPrototype = Object.getPrototypeOf(pick.options[0]),
  realHas = optionPrototype.hasAttribute;
let probes = 0;
optionPrototype.hasAttribute = function (name) { probes++; return realHas.call(this, name); };
let picked = null, flags = 0;
try {
  picked = new FormData(document.getElementById("one")).get("pick");
  for (const option of pick.options) flags += option.selected ? 1 : 0;
} finally {
  optionPrototype.hasAttribute = realHas;
}
expect("form data select", picked === "c0" && flags === 1);
expect("linear option reads (" + probes + ")", probes <= 1000);
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
