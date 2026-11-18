const request = r => new Promise((resolve, reject) => {
  r.onsuccess = () => resolve(r.result);
  r.onerror = () => reject(r.error);
});
async function openStore(name, setup) {
  const r = indexedDB.open(name, 1);
  r.onupgradeneeded = () => setup(r.result.createObjectStore('rows'));
  return request(r);
}
async function placed(markup) {
  const element = item(markup);
  await new Promise(resolve => setTimeout(resolve, 20));
  return element;
}
function xy(child, parent) {
  const a = child.getBoundingClientRect(), b = parent.getBoundingClientRect();
  return [a.left - b.left, a.top - b.top];
}
function hit(parent, x, y) {
  const r = parent.getBoundingClientRect();
  return document.elementFromPoint(r.left + x, r.top + y)?.id;
}
const cases = {
  async inactiveUpgradeCannotDeleteIndex() {
    let old;
    const db = await openStore('late-schema', s => {
      old = s; s.createIndex('name', 'name'); s.put({name:'first'}, 1);
    });
    try {
      if (!expectThrow(() => old.deleteIndex('name'), 'TransactionInactiveError')) return false;
      const s = db.transaction('rows').objectStore('rows');
      return s.indexNames.contains('name') && (await request(s.index('name').get('first')))?.name === 'first';
    } finally { db.close(); }
  },
  async requestErrorsPrecedeAbort() {
    const db = await openStore('abort-order', () => {});
    const t = db.transaction('rows', 'readwrite'), s = t.objectStore('rows'), events = [];
    const first = s.put('first', 1), second = s.put('second', 2), third = s.get(1);
    first.onsuccess = () => { events.push('success'); t.abort(); };
    second.onerror = () => events.push('second:' + second.error.name);
    third.onerror = () => events.push('third:' + third.error.name);
    await new Promise(resolve => { t.onabort = () => { events.push('abort'); resolve(); }; });
    const rows = await request(db.transaction('rows').objectStore('rows').getAll());
    db.close();
    return events.join() === 'success,second:AbortError,third:AbortError,abort' && rows.length === 0;
  },
  dateConstraintsBlockSubmit() {
    const f = item('<form><input type=date value=2025-12-31 min=2026-01-01><button>Go</button></form>');
    let submissions = 0;
    f.onsubmit = e => { e.preventDefault(); submissions++; };
    f.requestSubmit();
    const d = f.elements[0], refused = d.validity.rangeUnderflow && !d.checkValidity() && !submissions;
    d.value = '2026-01-02';
    f.requestSubmit();
    return refused && submissions === 1;
  },
  constraintsApplyToEligibleTypes() {
    const p = item('<div><input type=text min=10 step=2 value=3><input type=number value=9 pattern="[A-Za-z]+"></div>');
    return [...p.children].every(e => e.checkValidity());
  },
  rangeSnapsToValidSteps() {
    const e = item('<input type=range min=0 max=10 step=2 value=3>');
    const initial = e.value;
    e.value = '11';
    const upper = e.value;
    e.value = 'bad';
    return [initial, upper, e.value].join() === '4,10,6';
  },
  singleSelectDoesNotResurrectSelections() {
    const e = item('<select multiple><option selected>A</option><option selected>B</option></select>');
    if (e.selectedOptions.length !== 2) return false;
    e.removeAttribute('multiple');
    const one = e.value;
    if (e.selectedOptions.length !== 1) return false;
    e.multiple = true;
    return e.selectedOptions.length === 1 && e.value === one;
  },
  requiredRadioValidityBelongsToGroup() {
    const f = item('<form><input type=radio name=r required><input type=radio name=r></form>');
    const [a,b] = f.elements;
    if (!a.validity.valueMissing || !b.validity.valueMissing) return false;
    b.checked = true;
    return a.checkValidity() && b.checkValidity();
  },
  async rtlScrollAllowsNegativePositions() {
    const p = await placed('<div style="width:30px;height:12px;overflow:hidden;direction:rtl"><div style="width:70px;height:10px"></div></div>');
    const left = p.firstChild.getBoundingClientRect().left;
    p.scrollLeft = -12;
    const scrolled = p.scrollWidth === 70 && p.scrollLeft === -12 &&
      p.firstChild.getBoundingClientRect().left === left + 12;
    p.scrollLeft = -80;
    const atEnd = p.scrollLeft === -40;
    p.scrollLeft = 10;
    return scrolled && atEnd && p.scrollLeft === 0;
  },
  async hitTestingRespectsStackingContext() {
    const p = await placed('<div style="position:relative;width:80px;height:50px"><div style="opacity:.5;position:absolute;width:80px;height:50px"><b id=nested style="position:absolute;z-index:9;width:80px;height:50px;background:red"></b></div><i id=outside style="position:absolute;width:80px;height:50px;z-index:2;background:green"></i></div>');
    return hit(p,10,5) === 'outside';
  },
  async hitTestingRespectsClipPath() {
    const p = await placed('<div style="position:relative;width:80px;height:50px"><b id=under style="position:absolute;width:80px;height:50px;background:blue"></b><i id=over style="position:absolute;width:80px;height:50px;background:red;clip-path:inset(0 50% 0 0)"></i></div>');
    return hit(p,10,5) === 'over' && hit(p,65,5) === 'under';
  },
  async hitTestingRespectsAbsoluteRotation() {
    const p = await placed('<div id=base style="position:relative;width:80px;height:50px"><b id=rotated style="position:absolute;left:20px;top:10px;display:block;width:20px;height:10px;background:red;transform-origin:0 0;transform:rotate(90deg)"></b></div>');
    return xy(p.firstChild,p).join() === '10,10' && hit(p,15,25) === 'rotated' && hit(p,35,15) === 'base';
  },
  async rtlFlowStartsFromRightEdge() {
    const p = await placed('<div><div dir=rtl style="width:100px"><b style="display:block;width:20px;height:10px;margin-inline-start:10px"></b></div><div style="direction:rtl;width:100px"><b style="display:block;width:20px;height:10px;margin-right:10px"></b></div></div>');
    return [...p.children].every(row => xy(row.firstChild,row)[0] === 70);
  },
  async flexColumnProvidesPercentageHeightBasis() {
    const p = await placed('<div style="display:flex;flex-direction:column;height:100px"><div style="flex:1"><b style="display:block;height:50%;width:10px"></b></div></div>');
    return p.firstChild.getBoundingClientRect().height === 100 &&
      p.firstChild.firstChild.getBoundingClientRect().height === 50;
  },
  async flexBasisTransfersAspectRatio() {
    const p = await placed('<div style="display:flex;width:200px;height:50px"><div style="aspect-ratio:2/1;height:50px;flex:none"></div><b style="flex:1"></b></div>');
    return p.firstChild.getBoundingClientRect().width === 100 && xy(p.lastChild,p)[0] === 100;
  },
  async wrapReverseKeepsAlignContentStart() {
    const p = await placed('<div style="display:flex;flex-wrap:wrap-reverse;width:100px;height:60px;align-content:start"><b style="flex:none;width:60px;height:10px"></b><b style="flex:none;width:60px;height:10px"></b></div>');
    return [xy(p.firstChild,p)[1], xy(p.lastChild,p)[1]].join() === '10,0';
  },
  async gridAutoMarginOnlyCentersOnce() {
    const p = await placed('<div style="display:grid;grid-template-columns:100px;grid-template-rows:60px;width:100px;height:60px"><b style="width:20px;height:10px;margin:auto"></b></div>');
    return xy(p.firstChild,p).join() === '40,25';
  },
  async absposUsesContainingPaddingEdge() {
    const p = await placed('<div style="box-sizing:content-box;border:4px solid;padding:10px;position:relative;width:100px;height:40px"><b style="position:absolute;right:0;top:0;width:20px;height:10px"></b></div>');
    return xy(p.firstChild,p).join() === '104,4';
  },
  async relativeInlineOffsetDoesNotReflowSibling() {
    const p = await placed('<div><span style="position:relative;left:20px">x</span><span>x</span></div>');
    return xy(p.firstChild,p)[0] === 20 && xy(p.lastChild,p)[0] < 20;
  },
  async spanningTableCellUsesBothRows() {
    const t = await placed('<table style="border-collapse:collapse;table-layout:fixed;width:100px"><tr style="height:20px"><td rowspan="2"></td><td></td></tr><tr style="height:20px"><td></td></tr></table>');
    const td = t.querySelectorAll('td');
    return xy(td[2],t).join() === '50,20' && td[0].getBoundingClientRect().height === 40;
  },
  customMoveReactsAfterInsertion() {
    const parents = [];
    class X extends HTMLElement {
      connectedCallback() { parents.push(this.parentElement?.id); }
      disconnectedCallback() { parents.push(this.parentElement?.id); }
    }
    customElements.define('move-fixture', X);
    const p = item('<div><section id=a></section><section id=b></section></div>'), x = document.createElement('move-fixture');
    p.firstChild.append(x);
    p.lastChild.append(x);
    return parents.join() === 'a,b,b';
  },
  dateTimeLocalNumericRoundTrip() {
    const d = item('<input type=datetime-local value="2025-01-02T03:04">');
    const got = d.valueAsNumber;
    d.valueAsNumber = 1735787100000;
    return got === 1735787040000 && d.value === '2025-01-02T03:05';
  },
  async transformedAncestorIsOffsetParent() {
    const p = await placed('<div style="position:relative"><div style="transform:translateX(20px)"><b style="display:block;width:20px;height:10px"></b></div></div>');
    const b = p.firstChild.firstChild;
    return b.offsetParent === p.firstChild && b.offsetLeft === 0;
  },
  async fixedOffsetTopUsesViewport() {
    const old = document.body.style.margin;
    document.body.style.margin = '17px';
    try {
      const p = await placed('<div style="position:fixed;top:2px;left:7px;width:20px;height:10px"></div>');
      return p.offsetParent === null && p.offsetTop === 2 && p.offsetLeft === 7;
    } finally { document.body.style.margin = old; }
  },
  async observerScrollMarginExtendsClippingRect() {
    const p = await placed('<div style="position:relative;width:100px;height:20px;overflow:auto"><b style="display:block;position:absolute;top:25px;width:10px;height:10px">X</b></div>');
    const observe = options => new Promise(resolve => {
      const o = new IntersectionObserver(([record]) => { o.disconnect(); resolve(record.isIntersecting); }, options);
      o.observe(p.firstChild);
    });
    return !(await observe({})) && await observe({scrollMargin:'15px'});
  },
  customShadowOptOutAppliesInConstructor() {
    let blocked;
    class X extends HTMLElement {
      static disabledFeatures = ['shadow'];
      constructor() { super(); blocked = expectThrow(() => this.attachShadow({mode:'open'}), 'NotSupportedError'); }
    }
    customElements.define('shadow-disabled-fixture', X);
    const p = document.createElement('shadow-disabled-fixture');
    return blocked && p.shadowRoot === null;
  },
  customNamespacedObservationPreservesCase() {
    const values = [];
    class X extends HTMLElement {
      static observedAttributes = ['Mixed'];
      attributeChangedCallback(name, old, value, ns) { values.push([name,value,ns].join(':')); }
    }
    customElements.define('attr-fixture', X);
    const p = document.createElement('attr-fixture');
    p.setAttributeNS('urn:a','a:Mixed','first');
    p.setAttributeNS('urn:a','a:mixed','second');
    return values.join() === 'Mixed:first:urn:a';
  },
  tableRowsExposeLiveCellsAndBodies() {
    const t = item('<table><tbody><tr><td>1</td></tr></tbody></table>');
    const bodies = t.tBodies, cells = t.rows[0].cells;
    const before = bodies.length === 1 && cells.length === 1;
    t.append(document.createElement('tbody'));
    t.rows[0].append(document.createElement('th'));
    return before && bodies.length === 2 && cells.length === 2;
  },
  async slotMovesManuallyAssignedNode() {
    const p = item('<div><b style="display:block;width:12px;height:8px">A</b><i style="display:block;width:12px;height:8px">B</i></div>'), r = p.attachShadow({mode:'open',slotAssignment:'manual'});
    const a = document.createElement('slot'), b = document.createElement('slot');
    b.style.display = 'block';
    r.append(a,b);
    const [n,other] = p.children;
    a.assign(n); b.assign(n);
    const moved = r.slotAssignment === 'manual' && a.assignedNodes().length === 0 &&
      b.assignedNodes().length === 1 && b.assignedNodes()[0] === n &&
      n.assignedSlot === b && other.assignedSlot === null;
    b.assign(other,n);
    await new Promise(resolve => setTimeout(resolve, 20));
    const ordered = other.getBoundingClientRect().height === 8 &&
      other.getBoundingClientRect().top < n.getBoundingClientRect().top;
    b.assign(n);
    await new Promise(resolve => setTimeout(resolve, 20));
    return moved && ordered && other.assignedSlot === null &&
      other.getBoundingClientRect().height === 0 && n.getBoundingClientRect().height === 8;
  },
  async customRegistryLookupPreservesCase() {
    customElements.define('case-fixture', class extends HTMLElement {});
    const err = await failure(customElements.whenDefined('CASE-FIXTURE'));
    return customElements.get('CASE-FIXTURE') === undefined && err?.name === 'SyntaxError';
  },
  customRegistryRejectsUnknownBuiltIns() {
    return expectThrow(() => customElements.define('unknown-fixture', class extends HTMLElement {},
      {extends:'notarealhtmltag'}), 'NotSupportedError');
  },
};
