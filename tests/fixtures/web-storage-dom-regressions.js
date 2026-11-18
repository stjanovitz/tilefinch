const request = r => new Promise((resolve, reject) => {
  r.onsuccess = () => resolve(r.result);
  r.onerror = () => reject(r.error);
});
const finishedTransaction = tx => new Promise((resolve, reject) => {
  tx.oncomplete = resolve;
  tx.onabort = () => reject(tx.error);
});
function openDatabase(version, prepare) {
  const r = version === undefined ? indexedDB.open('bounded-regression')
    : indexedDB.open('bounded-regression', version);
  if (prepare) r.onupgradeneeded = () => prepare(r.result, r.transaction);
  return request(r);
}
async function withDatabase(run) {
  let connection = await openDatabase(1, db => {
    const out = db.createObjectStore('out'), inline = db.createObjectStore('inline', {keyPath:'id'});
    out.add('one', 1); out.add('three', 3);
    inline.createIndex('color', 'color');
    inline.add({id:1,color:'a'}); inline.add({id:2,color:'a'}); inline.add({id:3,color:'b'});
  });
  const acquire = async (v, prepare) => {
    connection.close();
    connection = await openDatabase(v, prepare);
    return connection;
  };
  try { return await run(connection, acquire); }
  finally {
    connection.close();
    await request(indexedDB.deleteDatabase('bounded-regression'));
  }
}
async function fileFixture(run) {
  const root = await navigator.storage.getDirectory();
  const file = await root.getFileHandle('bounded-regression', {create:true});
  try { return await run(file); }
  finally { await root.removeEntry('bounded-regression'); }
}
const cases = {
  async rejectsSeparateInlineKeyWithoutOverwriting() {
    return await withDatabase(async db => {
      const tx = db.transaction('inline','readwrite'), done = finishedTransaction(tx);
      const rejected = expectThrow(() => tx.objectStore('inline').put({id:9,color:'b'},1),'DataError');
      await done;
      const saved = await request(db.transaction('inline').objectStore('inline').get(1));
      return rejected && saved.id === 1 && saved.color === 'a';
    });
  },
  async keyOnlyCursorCannotDelete() {
    return await withDatabase(async db => {
      const tx = db.transaction('out','readwrite'), done = finishedTransaction(tx);
      const r = tx.objectStore('out').openKeyCursor(1);
      const threw = await new Promise(resolve => {
        r.onsuccess = () => resolve(expectThrow(() => r.result.delete(),'InvalidStateError'));
      });
      await done;
      return threw && await request(db.transaction('out').objectStore('out').get(1)) === 'one';
    });
  },
  async uniqueIndexFailureRestoresVersionAndValues() {
    return await withDatabase(async (_db, acquire) => {
      let created = false;
      const refused = await failure(acquire(2, (_db, tx) => {
        tx.objectStore('out').put('must roll back', 4);
        try {
          tx.objectStore('inline').createIndex('uniqueColor','color',{unique:true});
          created = true;
        } catch (_) { /* Failure must not depend on what this handler does. */ }
      }));
      const after = await acquire(), tx = after.transaction(['inline','out']),
        store = tx.objectStore('inline');
      const records = await Promise.all([
        request(store.getAll()), request(tx.objectStore('out').get(4))
      ]);
      return created && refused?.name === 'AbortError' && after.version === 1 &&
        !store.indexNames.contains('uniqueColor') && records[0].length === 3 && records[1] === undefined;
    });
  },
  async fileWritesSnapshotQueuedViews() {
    return await fileFixture(async file => {
      const stream = await file.createWritable(), writer = stream.getWriter();
      const bytes = new Uint8Array([0,65,0]);
      const first = writer.write(bytes.subarray(1,2));
      bytes[1] = 66;
      const second = writer.write({type:'write',data:bytes.subarray(1,2)});
      bytes[1] = 67;
      await Promise.all([first, second]); await writer.close();
      if (await (await file.getFile()).text() !== 'AB') return false;
      const source = "(async()=>{const root=await navigator.storage.getDirectory()," +
        "file=await root.getFileHandle('bounded-regression'),w=await file.createWritable()," +
        "bytes=new Uint8Array([67]),pending=w.write(bytes);" +
        "bytes[0]=68;await pending;await w.close();postMessage(true)" +
        "})().catch(()=>postMessage(false))";
      const url = URL.createObjectURL(new Blob([source])), worker = new Worker(url);
      try {
        const success = await new Promise(resolve => {
          worker.onmessage = event => resolve(event.data);
          worker.onerror = () => resolve(false);
        });
        return success && await (await file.getFile()).text() === 'C';
      } finally { worker.terminate(); URL.revokeObjectURL(url); }
    });
  },
  async absentFileDataCannotBecomeLiteralText() {
    return await fileFixture(async file => {
      const initial = await file.createWritable();
      await initial.write('persistent'); await initial.close();
      const errors = [];
      for (const data of [null, undefined]) {
        const w = await file.createWritable({keepExistingData:true});
        const exception = await failure(w.write({type:'write',data}));
        errors.push(exception?.name);
        if (exception) await w.abort();
        else await w.close();
      }
      return errors.every(name => name === 'TypeError' || name === 'SyntaxError') &&
        await (await file.getFile()).text() === 'persistent';
    });
  },
  async deletedHandlesNeverAffectReplacementSchema() {
    return await withDatabase(async (_db, acquire) => {
      let deletedIndex, deletedStore;
      const db = await acquire(2,(db,tx)=>{
        const s = tx.objectStore('inline'), i = s.index('color');
        s.deleteIndex('color');
        deletedIndex = expectThrow(() => i.get('a'),'InvalidStateError');
        const old = tx.objectStore('out');
        db.deleteObjectStore('out'); db.createObjectStore('out');
        deletedStore = expectThrow(() => old.put('not visible',1),'InvalidStateError');
      });
      return deletedIndex && deletedStore &&
        await request(db.transaction('out').objectStore('out').count()) === 0;
    });
  },
  async cursorContinuesAgainstCurrentRecords() {
    return await withDatabase(async db => {
      const tx = db.transaction('out','readwrite'), s = tx.objectStore('out');
      const done = finishedTransaction(tx), seen = [], r = s.openCursor();
      r.onsuccess = () => {
        const next = r.result;
        if (!next) return;
        seen.push(next.key);
        if (next.key === 1) { s.put('two',2); s.delete(3); s.put('four',4); }
        next.continue();
      };
      await done;
      return seen.join(',') === '1,2,4';
    });
  },
  async nullQueryDoesNotReadFirstRecord() {
    return await withDatabase(db => {
      const s = db.transaction('inline').objectStore('inline'), i = s.index('color');
      return expectThrow(() => i.get(null),'DataError') &&
        expectThrow(() => i.getKey(undefined),'DataError') &&
        expectThrow(() => s.get(),'TypeError');
    });
  },
  liveBaseAlsoChangesLinkProperties() {
    const a = item('<a href="page">link</a>'), base = document.createElement('base');
    base.href = 'https://example.invalid/first/'; document.head.append(base);
    try {
      if (a.href !== 'https://example.invalid/first/page') return false;
      base.href = 'https://example.invalid/second/';
      return a.href === 'https://example.invalid/second/page' && a.pathname === '/second/page';
    } finally { base.remove(); }
  },
  submitCannotReenterItsEvent() {
    const f = item('<form><button>Submit</button></form>');
    let calls = 0;
    f.addEventListener('submit',e=>{e.preventDefault(); if (++calls === 1) f.requestSubmit();});
    f.requestSubmit(); f.requestSubmit();
    return calls === 2;
  },
  insertedRadioDescendantsJoinTheirGroup() {
    const f = item('<form><input name=x type=radio checked></form>'),
      incoming = document.createElement('section');
    incoming.innerHTML = '<label><input name=x type=RADIO></label>';
    const radio = incoming.firstChild.firstChild;
    radio.checked = true; f.appendChild(incoming);
    return !f.elements[0].checked && radio.checked && !radio.defaultChecked &&
      f.elements[0].defaultChecked;
  },
  async reverseUniqueCursorChoosesLowestPrimary() {
    return await withDatabase(async db => {
      const tx = db.transaction('inline'), done = finishedTransaction(tx), seen = [],
        r = tx.objectStore('inline').index('color').openCursor(null,'prevunique');
      r.onsuccess = () => {
        const c = r.result;
        if (c) { seen.push(c.key + c.primaryKey); c.continue(); }
      };
      await done;
      return seen.join(',') === 'b3,a1';
    });
  },
  async renamingIndexChangesTheOpenedSchema() {
    return await withDatabase(async (_db, acquire) => {
      const next = await acquire(2,(_db,tx)=>tx.objectStore('inline').index('color').name='shade');
      const store = next.transaction('inline').objectStore('inline');
      return store.indexNames.contains('shade') &&
        expectThrow(() => store.index('color'),'NotFoundError') &&
        (await request(store.index('shade').get('a')))?.id === 1;
    });
  },
  rejectsInvalidValuesWithoutChangingOtherStyles() {
    const x = item('<div style="color:red;width:21px"></div>');
    x.style.color = 'invalidzz'; x.style.width = '31px; height: 99px';
    x.style.backgroundImage = 'url("data:image/png;base64,AA==")';
    return x.style.color === 'red' && x.style.width === '21px' && x.style.height === '' &&
      x.style.backgroundImage.includes('base64');
  },
  elementMatchingStopsAtTheShadowRoot() {
    const host = item('<div id=outer></div>'), root = host.attachShadow({mode:'open'}),
      inner = document.createElement('section'), b = document.createElement('b');
    inner.appendChild(b); root.appendChild(inner);
    return b.matches('section > b') && !b.matches('#outer b') &&
      b.closest('#outer b') === null && inner.matches(':scope');
  },
  relatedTargetStopsEventsAtTheirShadowBoundary() {
    const host = item('<div></div>'), root = host.attachShadow({mode:'open'}),
      a = document.createElement('i'), b = document.createElement('b');
    root.append(a,b);
    let outside = 0, inside = 0;
    host.addEventListener('mouseover', () => outside++);
    root.addEventListener('mouseover', e => {if(e.relatedTarget === b) inside++;});
    a.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,composed:true,relatedTarget:b}));
    a.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,composed:true,relatedTarget:mount}));
    return inside === 1 && outside === 1;
  },
  canvasCompositeErasesTransparentSourceInsideClip() {
    for (const mode of ['copy','source-in']) {
      const c = item('<canvas width=8 height=8></canvas>').getContext('2d');
      c.fillRect(0,0,8,8); c.beginPath(); c.rect(0,0,6,6); c.clip();
      c.globalCompositeOperation = mode; c.fillStyle = 'red'; c.fillRect(0,0,2,2);
      if (c.getImageData(1,1,1,1).data[0] !== 255 ||
          c.getImageData(3,3,1,1).data[3] !== 0 ||
          c.getImageData(7,7,1,1).data[3] !== 255) return false;
    }
    const box = item('<div><canvas width=8 height=8></canvas><canvas width=2 height=2></canvas></div>'),
      canvas = box.firstChild, source = box.lastChild,
      c = canvas.getContext('2d'), image = source.getContext('2d');
    image.fillRect(0,0,2,2); c.fillRect(0,0,8,8);
    c.globalCompositeOperation = 'copy'; c.drawImage(source,1,1);
    return c.getImageData(1,1,1,1).data[3] === 255 && c.getImageData(7,7,1,1).data[3] === 0;
  },
  canvasWithoutAlphaStaysOpaqueAcrossDrawingAndClear() {
    const c = item('<canvas width=6 height=6></canvas>').getContext('2d',{alpha:false});
    c.fillStyle = '#ff000080'; c.globalCompositeOperation = 'copy'; c.fillRect(0,0,3,3);
    const p = c.getImageData(1,1,1,1).data;
    c.translate(1,1); c.clearRect(0,0,1,1);
    const q = c.getImageData(1,1,1,1).data;
    const correct = c.getContextAttributes().alpha === false && p[0] === 128 && p[3] === 255 &&
      q[0] === 0 && q[3] === 255 && c.getImageData(5,5,1,1).data[3] === 255 &&
      c.getImageData(-1,-1,1,1).data[3] === 0;
    c.canvas.width = 4;
    return correct && c.getImageData(3,3,1,1).data[3] === 255;
  },
  outerHTMLKeepsLiveRangeBeforeInsertedContent() {
    const x = item('<div><b>A</b><i>B</i></div>'), r = document.createRange();
    r.setStart(x.firstChild.firstChild,0); r.setEnd(x.lastChild.firstChild,1);
    x.firstChild.outerHTML = '<u>K</u>';
    return r.startContainer === x && r.startOffset === 0 && r.toString() === 'KB';
  },
  textareaRangeReplacementClampsAndSelects() {
    const t = item('<textarea>abcdef</textarea>');
    t.setRangeText('Z',3,300,'select');
    return t.value === 'abcZ' && t.selectionStart === 3 && t.selectionEnd === 4 &&
      expectThrow(() => t.setRangeText('X',4,1),'IndexSizeError') && t.value === 'abcZ';
  },
  inputTypeChangeDoesNotReviveSelection() {
    const x = item('<input value=hello>');
    x.setSelectionRange(2,4); x.type = 'number';
    const inactive = x.selectionStart === null;
    x.type = 'text';
    return inactive && x.selectionStart === 0 && x.selectionEnd === 0;
  },
  cssCanRemoveOneMarginSuppliedByShorthand() {
    const x = item('<div></div>'), s = x.style;
    s.cssText = 'margin: 5px 7px 9px 11px !important; color: red';
    const old = s.removeProperty('margin-left');
    return old === '11px' && s.marginLeft === '' && getComputedStyle(x).marginLeft === '0px' &&
      s.marginRight === '7px' && s.getPropertyPriority('margin-right') === 'important' && s.color === 'red';
  },
  paddingShorthandExposesItsValuesAndPriority() {
    const s = item('<div></div>').style;
    s.setProperty('padding','12px 2px','important');
    return s.paddingLeft === '2px' && s.paddingTop === '12px' &&
      s.getPropertyPriority('padding-left') === 'important';
  },
  escapedCustomPropertyUsesItsDecodedName() {
    const x = item('<div style="--\\61 : red;color:var(--a)">text</div>');
    if (x.style.getPropertyValue('--a').trim() !== 'red' ||
        getComputedStyle(x).color !== 'rgb(255, 0, 0)') return false;
    x.style.setProperty('--a','blue');
    return getComputedStyle(x).color === 'rgb(0, 0, 255)';
  },
  async cursorRequestIsPendingImmediately() {
    return await withDatabase(async db => {
      const tx = db.transaction('out'), done = finishedTransaction(tx),
        r = tx.objectStore('out').openCursor();
      let after, next;
      r.onsuccess = () => {
        if (after) { next = r.result.key; return; }
        r.result.continue();
        after = r.readyState === 'pending' && expectThrow(() => r.result,'InvalidStateError');
        if (!after) r.onsuccess = null;
      };
      await done;
      return after && next === 3;
    });
  },
  async rejectsNegativeCountsBeforeQueuing() {
    return await withDatabase(db => {
      const s = db.transaction('inline').objectStore('inline');
      return expectThrow(() => s.getAll(null,-1),'TypeError') &&
        expectThrow(() => s.index('color').getAllKeys(null,Infinity),'TypeError');
    });
  },
  datalistAssociationTracksLiveIds() {
    const box = item('<div><input list=x><datalist id=x></datalist></div>'),
      x = box.firstChild, d = box.lastChild;
    const before = x.list === d;
    d.id = 'y';
    return before && x.list === null;
  },
  classListIterationSkipsRemovedTokens() {
    const x = item('<div class="a b"></div>'), seen = [];
    x.classList.forEach(token => {seen.push(token); if (token === 'a') x.classList.remove('b');});
    return seen.join(',') === 'a';
  },
  streamConstructorsRejectNonCallableCallbacks() {
    return expectThrow(() => new ReadableStream({pull:123}),'TypeError') &&
      expectThrow(() => new WritableStream({write:123}),'TypeError') &&
      expectThrow(() => new TransformStream({transform:123}),'TypeError');
  },
  canvasPathEdgesCountAsInside() {
    const c = item('<canvas width=3 height=3></canvas>').getContext('2d');
    c.rect(0,0,2,2);
    return c.isPointInPath(2,1) && c.isPointInPath(1,2) && !c.isPointInPath(3,1);
  },
};
