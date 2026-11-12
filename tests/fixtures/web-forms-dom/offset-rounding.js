// JR-22: offsetWidth and offsetHeight are longs. Before layout has run, the
// box comes from the declared size, which can be fractional.
const box = document.createElement("div");
box.style.cssText = "width:100.6px;height:49.4px";
document.getElementById("host").append(box);
const sizes = box.offsetWidth + "x" + box.offsetHeight;
globalThis.pocSummary = sizes === "101x49" ? "OK" : "FAIL offset sizes " + sizes;
