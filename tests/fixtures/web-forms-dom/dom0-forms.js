// JR-06: DOM0 form and select idioms. select.options is one live
// HTMLOptionsCollection that script can edit; forms expose their controls by
// name and index; document.forms finds a form by name; select.remove(index)
// removes an option, never the select.
const failures = [], expect = (name, ok) => { if (!ok) failures.push(name); };
const check = (name, read) => {
  try { expect(name, read()); } catch (error) { failures.push(name + ": " + error); }
};
const host = document.getElementById("host");
host.innerHTML =
  '<form name="login" id="login-form" action="/session">'
  + '<input name="user" value="ada"><input name="action" value="clobber">'
  + '<input type="radio" name="plan" value="free">'
  + '<input type="radio" name="plan" value="pro" checked>'
  + '<input id="q" value="query"></form>'
  + '<select id="city"><option>a</option><option>b</option><option>c</option></select>';
const form = document.getElementById("login-form"),
  city = document.getElementById("city");

check("document.forms by name", () => document.forms.login === form
  && document.forms[0] === form && document.forms.length === 1);
check("DOM0 chain", () => document.forms.login.user.value === "ada");
check("form named control", () => form.user === form.elements[0] && form.q.value === "query");
check("form indexed control", () => form[0] === form.elements[0] && form.length === 5);
check("own members win", () => form.action.endsWith("/session")
  && typeof form.submit === "function");
check("elements by name", () => form.elements.user === form.elements.namedItem("user")
  && form.elements["q"] === form.q);
check("radio node list", () => {
  const plan = form.elements.plan;
  return plan.length === 2 && plan.value === "pro" && form.plan === plan;
});
check("radio node list set", () => {
  form.elements.plan.value = "free";
  return form.elements[2].checked && !form.elements[3].checked;
});
check("rename follows", () => {
  form.elements.user.setAttribute("name", "login-name");
  return form.elements.user === undefined && form["login-name"].value === "ada";
});

const options = city.options;
check("same live collection", () => city.options === options && options.length === 3
  && Object.prototype.toString.call(options) === "[object HTMLOptionsCollection]");
check("options.length = 0", () => {
  options.length = 0;
  return options.length === 0 && city.children.length === 0;
});
check("dependent drop-down refill", () => {
  for (const [text, value] of [["Paris", "p"], ["Rome", "r"], ["Oslo", "o"]])
    options[options.length] = new Option(text, value);
  return options.length === 3 && city.options[2].value === "o"
    && city.value === "p";
});
check("options.add before index", () => {
  options.add(new Option("Bern", "b"), 1);
  return options[1].value === "b" && options.length === 4;
});
check("options.selectedIndex", () => {
  options.selectedIndex = 2;
  return city.selectedIndex === 2 && city.value === "r";
});
check("options[i] = null removes", () => {
  options[0] = null;
  return options.length === 3 && options[0].value === "b";
});
check("options.remove", () => {
  options.remove(0);
  return options.length === 2 && options[0].value === "r";
});
check("select.remove(index)", () => {
  city.remove(0);
  return city.isConnected && city.options.length === 1 && city.options[0].value === "o";
});
check("select.length = 3", () => {
  city.length = 3;
  return city.options.length === 3 && city.options[2].value === "";
});
check("select.length = 0", () => {
  city.length = 0;
  return city.options.length === 0;
});
check("select.remove()", () => {
  city.remove();
  return !city.isConnected;
});
globalThis.pocSummary = failures.length ? "FAIL " + failures.join(" | ") : "OK";
