// JR-12: input.defaultValue reflects the value attribute and
// input.defaultChecked the default checkedness, so "is the form dirty"
// checks and form reset see the defaults.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};
document.getElementById("host").innerHTML =
  '<form id="f"><input id="t" value="x"><input id="u" value="a">'
  + '<input id="c" type="checkbox" checked><input id="d" type="checkbox">'
  + '<input id="e" type="checkbox"></form>';
const $ = (id) => document.getElementById(id);
const t = $("t"), u = $("u"), c = $("c"), d = $("d"), e = $("e");

check("defaultValue reads the attribute", () => t.defaultValue === "x");
check("clean form is clean", () => t.value === t.defaultValue);
check("dirty check", () => {
  t.value = "y";
  return t.value !== t.defaultValue && t.defaultValue === "x";
});
check("defaultValue set keeps a dirty value", () => {
  t.defaultValue = "z";
  return t.value === "y" && t.getAttribute("value") === "z";
});
check("defaultValue set moves a clean value", () => {
  u.defaultValue = "b";
  return u.value === "b";
});
check("defaultChecked reads the default", () => c.defaultChecked === true
  && d.defaultChecked === false);
check("defaultChecked survives checked = false", () => {
  c.checked = false;
  return c.checked === false && c.defaultChecked === true;
});
check("clean checkbox follows defaultChecked", () => {
  d.defaultChecked = true;
  return d.checked === true && d.defaultChecked === true;
});
check("dirty checkbox keeps its checkedness", () => {
  e.checked = true;
  e.defaultChecked = false;
  return e.checked === true && e.defaultChecked === false;
});
check("reset restores the defaults", () => {
  $("f").reset();
  return t.value === "z" && c.checked === true && d.checked === true
    && e.checked === false;
});
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
