// JR-03: fragment parsing into a detached receiver must not run
// connectedCallback; the reaction runs once when the subtree is connected.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const log = [];
customElements.define("x-card", class extends HTMLElement {
  connectedCallback() { log.push("conn:" + this.isConnected); }
  disconnectedCallback() { log.push("disc"); }
});
const host = document.getElementById("host");
const wrapper = document.createElement("div");
wrapper.innerHTML = "<x-card></x-card>";
expect("no reaction while detached", log.join() === "");
host.appendChild(wrapper);
expect("connected once on insertion", log.join() === "conn:true");
wrapper.remove();
expect("balanced disconnect", log.join() === "conn:true,disc");
log.length = 0;
host.innerHTML = "<x-card></x-card>";
expect("connected receiver reacts", log.join() === "conn:true");
log.length = 0;
const parsed = new DOMParser().parseFromString("<x-card></x-card>", "text/html");
expect("parsed document has no reactions", log.join() === "");
host.appendChild(parsed.body.firstChild);
expect("parsed node connects on insertion", log.join() === "conn:true");
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
