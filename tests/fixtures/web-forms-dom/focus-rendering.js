// JR-27: focus() does nothing on an element that is not rendered:
// display:none on the element or on an ancestor.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
document.getElementById("host").innerHTML =
  '<input id="self" style="display:none">'
  + '<div style="display:none"><div><button id="nested">b</button></div></div>'
  + '<input id="shown">';
const $ = (id) => document.getElementById(id);
$("self").focus();
expect("display:none element", document.activeElement !== $("self"));
$("nested").focus();
expect("display:none ancestor", document.activeElement !== $("nested"));
$("shown").focus();
expect("rendered element", document.activeElement === $("shown"));
$("nested").parentNode.parentNode.style.display = "block";
$("nested").focus();
expect("shown again", document.activeElement === $("nested"));
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
