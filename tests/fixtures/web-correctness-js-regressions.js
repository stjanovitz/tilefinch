const request = r => new Promise((resolve, reject) => {
  r.onsuccess = () => resolve(r.result);
  r.onerror = () => reject(r.error);
});
const finishedTransaction = tx => new Promise((resolve, reject) => {
  tx.oncomplete = resolve;
  tx.onabort = () => reject(tx.error);
});
async function scratchDatabase(name, version, prepare) {
  const r = indexedDB.open(name, version);
  r.onupgradeneeded = () => prepare(r.result, r.transaction);
  return request(r);
}
async function dropDatabase(db, name) {
  db?.close();
  await request(indexedDB.deleteDatabase(name));
}
function workerResult(source) {
  const url = URL.createObjectURL(new Blob([source])), worker = new Worker(url);
  return new Promise(resolve => {
    worker.onmessage = event => resolve(event.data);
    worker.onerror = () => resolve('worker error');
  }).finally(() => { worker.terminate(); URL.revokeObjectURL(url); });
}
/* Milliseconds of the lab's timer clock (16 per loop iteration) a case may
   take; the default is 250. The cursor loop's database tasks need about
   250 iterations. */
globalThis.caseBudgets = {cursorUpdateLoopKeepsItsSnapshot: 6400};
const cases = {
  customColorPropertiesKeepIdentifiers() {
    const x = item('<div></div>');
    x.style.setProperty('--theme-color', 'dark');
    x.style.setProperty('background-color', 'dark');
    const sheet = document.createElement('style');
    sheet.textContent = '.custom-color-probe{}';
    document.head.append(sheet);
    try {
      const rule = sheet.sheet.cssRules[0];
      rule.style.setProperty('--accent-color', 'brand');
      return x.style.getPropertyValue('--theme-color') === 'dark' &&
        x.style.backgroundColor === '' &&
        rule.style.getPropertyValue('--accent-color') === 'brand';
    } finally { sheet.remove(); }
  },
  selectReachesEmailAndNumber() {
    const email = item('<input type=email value="a@b.test">');
    email.select();
    const cutEmail = document.execCommand('cut') && email.value === '';
    const number = item('<input type=number value=42>');
    number.select();
    return cutEmail && document.execCommand('cut') && number.value === '' &&
      number.selectionStart === null;
  },
  async transformTerminateKeepsQueuedChunks() {
    const t = new TransformStream({start(c) { c.enqueue('a'); c.terminate(); c.terminate(); }});
    const reader = t.readable.getReader();
    const first = await reader.read(), second = await reader.read();
    const refused = await failure(t.writable.getWriter().write('late'));
    const u = new TransformStream({flush(c) { c.enqueue('z'); c.terminate(); }});
    const writer = u.writable.getWriter(), flushed = u.readable.getReader();
    const closing = writer.close(), last = await flushed.read();
    await closing;
    const end = await flushed.read();
    return first.value === 'a' && !first.done && second.done && refused?.name === 'TypeError' &&
      last.value === 'z' && end.done;
  },
  streamsAcceptNullDictionaries() {
    const w = new WritableStream({}, null), t = new TransformStream(null, null, null);
    return w instanceof WritableStream && t.readable instanceof ReadableStream;
  },
  async cursorUpdateLoopKeepsItsSnapshot() {
    /* Each own update or delete used to re-sort every record on the next
       continue(): quadratic. Bound the loop by a plain walk of the same
       records in the same run (linear: a few times the walk). */
    const name = 'cursor-update-loop', count = 300;
    const db = await scratchDatabase(name, 1, db => db.createObjectStore('rows'));
    const walk = mutate => new Promise((resolve, reject) => {
      const tx = db.transaction('rows', 'readwrite'), started = performance.now();
      let visited = 0;
      tx.objectStore('rows').openCursor().onsuccess = event => {
        const cursor = event.target.result;
        if (!cursor) return;
        if (cursor.key !== visited++) visited = -1e9;
        if (mutate) {
          cursor.update({n: cursor.value.n + 1});
          if (cursor.key % 2) cursor.delete();
        }
        cursor.continue();
      };
      tx.oncomplete = () => resolve([visited, performance.now() - started]);
      tx.onabort = () => reject(tx.error);
    });
    try {
      for (let at = 0; at < count; at += 50) {
        const seed = db.transaction('rows', 'readwrite'), stored = finishedTransaction(seed);
        for (let i = at; i < at + 50; i++) seed.objectStore('rows').add({n: i}, i);
        await stored;
      }
      const [walked, plain] = await walk(false), [updated, mutating] = await walk(true);
      const rows = await request(db.transaction('rows').objectStore('rows').getAll());
      return walked === count && updated === count && mutating < 6 * plain + 5 &&
        rows.length === count / 2 && rows[0].n === 1 && rows[rows.length - 1].n === count - 1;
    } finally { await dropDatabase(db, name); }
  },
  async storeCursorSeesOtherKeysChange() {
    const name = 'cursor-other-key';
    const db = await scratchDatabase(name, 1, db => {
      const store = db.createObjectStore('rows');
      for (let i = 0; i < 4; i++) store.add('v' + i, i);
    });
    try {
      const tx = db.transaction('rows', 'readwrite'), done = finishedTransaction(tx), seen = [];
      const store = tx.objectStore('rows');
      store.openCursor().onsuccess = event => {
        const cursor = event.target.result;
        if (!cursor) return;
        seen.push(cursor.key + '=' + cursor.value);
        if (cursor.key === 0) { cursor.update('own'); store.put('changed', 2); store.delete(1); }
        cursor.continue();
      };
      await done;
      return seen.join() === '0=v0,2=changed,3=v3';
    } finally { await dropDatabase(db, name); }
  },
  async workerFileWritesChargeQueuedBuffers() {
    /* Un-awaited 512 KiB writes from a worker queue owner-realm copies; the
       queue's memory bound must see each copy's bytes and refuse with a
       RangeError long before the copies exhaust the heap. */
    const source = "(async()=>{const root=await navigator.storage.getDirectory()," +
      "file=await root.getFileHandle('charged-writes',{create:true}),w=await file.createWritable()," +
      "names=new Set(),pending=[];for(let i=0;i<96;i++){const data=new ArrayBuffer(524288);" +
      "pending.push(w.write(i%2?data:{type:'write',data}).then(()=>'',e=>e.name))}" +
      "for(const name of await Promise.all(pending))if(name)names.add(name);" +
      "await w.abort().catch(()=>{});await root.removeEntry('charged-writes');" +
      "postMessage([...names].join())})().catch(e=>postMessage('error '+e.name))";
    const names = await workerResult(source);
    return typeof names === 'string' && names.split(',').includes('RangeError') &&
      !/InternalError|error /.test(names);
  },
  async multipartNamesRoundTrip() {
    const fd = new FormData();
    fd.append('a"b', 'x');
    fd.append('file', new Blob(['y']), 'q"r.txt');
    const back = await new Response(fd).formData();
    return back.get('a"b') === 'x' && back.get('file')?.name === 'q"r.txt' &&
      !back.has('a%22b');
  },
  shadowRootRadiosFormTheirOwnGroup() {
    const host = item('<div></div>'), root = host.attachShadow({mode: 'open'});
    const outside = document.createElement('input');
    outside.type = 'radio'; outside.name = 'g'; outside.checked = true;
    mount.append(outside);
    root.innerHTML = '<input type=radio name=g checked id=inner>';
    const inner = root.getElementById('inner'), wrap = document.createElement('div');
    wrap.innerHTML = '<p><input type=radio name=g checked></p>';
    const added = wrap.querySelector('input');
    root.append(wrap);
    return outside.checked && added.checked && !inner.checked;
  },
  insertedDescendantDetailsCloseInGroup() {
    const box = item('<div><details name=d open></details></div>');
    const wrap = document.createElement('div');
    wrap.innerHTML = '<section><details name=d open></details></section>';
    box.append(wrap);
    return box.firstElementChild.open && !wrap.querySelector('details').open;
  },
  cancelledRadioClickRestoresOneChecked() {
    const box = item('<div><input type=radio name=r checked><input type=radio name=r>' +
      '<input type=radio name=r></div>');
    const [a, b, c] = box.children;
    b.addEventListener('click', event => { c.checked = true; event.preventDefault(); });
    b.click();
    return a.checked && !b.checked && !c.checked;
  },
  formDataUndefinedFilenameIsOmitted() {
    const fd = new FormData();
    fd.append('blob', new Blob(['x']), undefined);
    fd.set('file', new File(['y'], 'keep.txt'), undefined);
    return fd.get('blob').name === 'blob' && fd.get('file').name === 'keep.txt';
  },
  selectionContainsDoctype() {
    const selection = getSelection(), whole = document.createRange();
    whole.setStart(document, 0);
    whole.setEnd(document, document.childNodes.length);
    selection.removeAllRanges();
    selection.addRange(whole);
    const inside = selection.containsNode(document.doctype);
    selection.removeAllRanges();
    const part = document.createRange();
    part.selectNodeContents(mount);
    selection.addRange(part);
    const outside = selection.containsNode(document.doctype);
    selection.removeAllRanges();
    return inside === true && outside === false;
  },
  async inactiveUpgradeRefusesStoreChanges() {
    /* commit() leaves the upgrade transaction committing: still the
       upgrade transaction, no longer active. */
    const name = 'inactive-store-schema';
    let created, deleted;
    const db = await scratchDatabase(name, 1, (db, tx) => {
      db.createObjectStore('kept');
      tx.commit();
      created = expectThrow(() => db.createObjectStore('late'), 'TransactionInactiveError');
      deleted = expectThrow(() => db.deleteObjectStore('kept'), 'TransactionInactiveError');
    });
    try {
      return created && deleted && db.objectStoreNames.contains('kept') &&
        !db.objectStoreNames.contains('late');
    } finally { await dropDatabase(db, name); }
  },
  async uniqueIndexPopulationAbortsWithConstraintError() {
    const name = 'unique-population';
    const seeded = await scratchDatabase(name, 1, db => {
      const store = db.createObjectStore('rows');
      store.add({email: 'same'}, 1); store.add({email: 'same'}, 2);
    });
    seeded.close();
    let abortName;
    const opened = await failure(scratchDatabase(name, 2, (_db, tx) => {
      tx.addEventListener('abort', () => abortName = tx.error?.name);
      tx.objectStore('rows').createIndex('email', 'email', {unique: true});
    }));
    await request(indexedDB.deleteDatabase(name));
    return opened?.name === 'AbortError' && abortName === 'ConstraintError';
  },
};
