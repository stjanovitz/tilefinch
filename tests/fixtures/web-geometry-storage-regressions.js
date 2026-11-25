const checkThrow = (fn, name) => {
  try { fn(); return false; } catch (error) { return error.name === name; }
};
const requestValue = request => new Promise((resolve, reject) => {
  request.onsuccess = () => resolve(request.result);
  request.onerror = event => { event.preventDefault(); reject(request.error); };
});
const opening = (name, version, upgrade) => {
  const request = indexedDB.open(name, version);
  request.onupgradeneeded = () => upgrade?.(request.result, request.transaction);
  return requestValue(request);
};
const completion = tx => new Promise(resolve => {
  tx.oncomplete = () => resolve(true);
  tx.onabort = () => resolve(false);
});
const settled = async (html, ids) => {
  const p = item(html);
  await new Promise(resolve => setTimeout(resolve, 16));
  const origin = p.getBoundingClientRect();
  return ids.map(id => {
    const box = p.querySelector('#' + id).getBoundingClientRect();
    return [box.left - origin.left, box.top - origin.top, box.width, box.height];
  });
};
const canvas = () => item('<canvas width=24 height=24></canvas>').getContext('2d');
const cases = {
  async countAndCursorBindings() {
    let deleted, badDeleted;
    const db = await opening('cursor-bindings', 1, d => {
      const s = d.createObjectStore('rows'); deleted = s.createIndex('gone','n');
      s.deleteIndex('gone');
      badDeleted = ['count','openCursor','openKeyCursor'].every(n =>
        checkThrow(() => deleted[n]({}), 'InvalidStateError'));
      s.createIndex('groups','n');
      for (const [key,n] of [[1,8],[2,8],[3,9]]) s.put({n},key);
    });
    const tx = db.transaction('rows'), done = completion(tx), s = tx.objectStore('rows'),
      i = s.index('groups'), work = [];
    for (const source of [s,i]) {
      for (const n of ['count','openCursor','openKeyCursor'])
        if (source[n].length !== 0 ||
            !checkThrow(() => source[n].call(source === i ? s : i), 'TypeError')) return false;
      work.push(requestValue(source.count()).then(v => v === 3));
      for (const n of ['openCursor','openKeyCursor']) {
        if (!checkThrow(() => source[n]({}, 'backwards'), 'TypeError')) return false;
        const r = source[n](null, 'prevunique'), seen = [];
        work.push(new Promise(resolve => { r.onsuccess = () => {
          const c = r.result;
          if (!c) return resolve(seen.join() === (source === s ? '3,2,1' : '3,1'));
          if (r.source !== source || c.source !== source || ('value' in c) !== (n === 'openCursor'))
            return resolve(false);
          seen.push(c.primaryKey); c.continue();
        }; }));
      }
    }
    if (!badDeleted || !(await Promise.all(work)).every(Boolean) || !await done) return false;
    for (const source of [s,i]) for (const n of ['count','openCursor','openKeyCursor'])
      if (!checkThrow(() => source[n]({}), 'TransactionInactiveError')) return false;
    const tx2 = db.transaction('rows'), aborted = completion(tx2), store = tx2.objectStore('rows');
    const key = [1]; let first = true;
    Object.defineProperty(key, 0, {get(){if(first){first=false;tx2.abort()} return 1}});
    const rejected = checkThrow(() => store.openCursor(key), 'TransactionInactiveError');
    await aborted; db.close(); return rejected;
  },
  async storeAndIndexReadBindingsAgree() {
    const db = await opening('read-bindings', 1, d => {
      const store = d.createObjectStore('rows');
      store.createIndex('byGroup', 'group');
      store.put({group:'a', n:2}, 2); store.put({group:'a', n:1}, 1);
    });
    const tx = db.transaction('rows'), done = completion(tx),
      store = tx.objectStore('rows'), index = store.index('byGroup');
    const pending = [];
    for (const [source, key] of [[store, 1], [index, 'a']]) {
      if (!checkThrow(() => source.get(), 'TypeError') ||
          !checkThrow(() => source.getKey(null), 'DataError')) return false;
      const range = source === store ? IDBKeyRange.bound(1, 2) : key;
      for (const [name, args, expect] of [
        ['get', [key], {group:'a', n:1}], ['getKey', [key], 1],
        ['getAll', [range, 1], [{group:'a', n:1}]], ['getAllKeys', [range], [1,2]]
      ]) {
        if (source[name].length !== (name === 'get' || name === 'getKey' ? 1 : 0) ||
            !checkThrow(() => source[name].call(source === store ? index : store, ...args), 'TypeError'))
          return false;
        const request = source[name](...args);
        if (request.source !== source) return false;
        pending.push(requestValue(request).then(v => JSON.stringify(v) === JSON.stringify(expect)));
      }
    }
    if (!(await Promise.all(pending)).every(Boolean) || !await done) return false;
    db.close();
    for (const source of [store,index]) {
      for (const name of ['get','getKey','getAll','getAllKeys'])
        if (!checkThrow(() => source[name]({}), 'TransactionInactiveError')) return false;
      if (!checkThrow(() => source.get(), 'TypeError') ||
          !checkThrow(() => source.getAll({}, -1), 'TypeError')) return false;
    }
    return true;
  },
  async commitCannotBeAborted() {
    const db = await opening('commit', 1, d => d.createObjectStore('store'));
    const tx = db.transaction('store', 'readwrite'), done = completion(tx);
    const write = requestValue(tx.objectStore('store').put('saved', 1)).catch(() => null);
    tx.commit();
    const rejects = checkThrow(() => tx.abort(), 'InvalidStateError');
    const saved = await done;
    await write;
    const value = await requestValue(db.transaction('store').objectStore('store').get(1));
    const blocker = db.transaction('store', 'readwrite');
    blocker.objectStore('store').put('blocker', 2);
    const queued = db.transaction('store', 'readwrite'), queuedDone = completion(queued);
    const queuedWrite = requestValue(queued.objectStore('store').put('queued', 3)).catch(() => null);
    queued.commit();
    const rejectsWhileQueued = checkThrow(() => queued.abort(), 'InvalidStateError');
    const queuedSaved = await queuedDone;
    await queuedWrite;
    const queried = await requestValue(db.transaction('store').objectStore('store').get(3));
    const explicit = db.transaction('store');
    const aborted = completion(explicit);
    explicit.abort();
    const repeat = checkThrow(() => explicit.abort(), 'InvalidStateError');
    await aborted;
    db.close();
    return rejects && saved && value === 'saved' && rejectsWhileQueued && queuedSaved &&
      queried === 'queued' && repeat && explicit.error === null &&
      checkThrow(() => tx.abort(), 'InvalidStateError');
  },
  async objectStoreRenameIsTransactional() {
    let during = false, abandoned;
    let db = await opening('rename', 1, d => d.createObjectStore('old').put('saved', 1));
    db.close();
    const rollback = await opening('rename', 2, (_d,t) => {
      abandoned = t.objectStore('old');
      abandoned.name = 'aborted';
      t.abort();
    }).then(() => false, e => e.name === 'AbortError');
    db = await opening('rename', 2, (d,t) => {
      const store = t.objectStore('old');
      store.name = 'new';
      during = d.objectStoreNames.contains('new') && t.objectStore('new') === store &&
        checkThrow(() => t.objectStore('old'), 'NotFoundError');
    });
    const tx = db.transaction('new'), store = tx.objectStore('new');
    const rejects = checkThrow(() => { store.name = 'wrong'; }, 'InvalidStateError');
    const value = await requestValue(store.get(1));
    db.close();
    return rollback && abandoned.name === 'old' && during && rejects && value === 'saved';
  },
  async returnedKeyPathsCannotMutateSchema() {
    const db = await opening('paths', 1, d => d.createObjectStore('s',
      {keyPath:['x','y']}).createIndex('i',['a','b']));
    const s = db.transaction('s','readwrite').objectStore('s');
    const storePath=s.keyPath; storePath[0] = 'bad';
    const index = s.index('i');
    const indexPath=index.keyPath; indexPath[0] = 'bad';
    const key = await requestValue(s.put({x:1,y:2,a:3,b:4}));
    const found = await requestValue(index.getKey([3,4]));
    db.close();
    return key.join() === '1,2' && found.join() === '1,2' &&
      s.keyPath===storePath && index.keyPath===indexPath;
  },
  async overflowClipsScriptHitTargets() {
    const p = item('<div style="position:relative;overflow:hidden;width:60px;height:50px">' +
      '<button style="position:absolute;left:45px;top:10px;width:50px;height:30px">x</button></div>');
    await new Promise(resolve => setTimeout(resolve, 16));
    const r = p.getBoundingClientRect(), button = p.firstChild;
    return document.elementFromPoint(r.left+50,r.top+20) === button &&
      !document.elementsFromPoint(r.left+70,r.top+20).includes(button);
  },
  async percentageRelativeUsesDefiniteHeight() {
    const html = h => '<div style="position:relative;width:120px;'+h+'"><div id=a style="position:relative;top:50%;height:30px"></div><div style="height:20px"></div></div>';
    const [a] = await settled(html(''),['a']), [b] = await settled(html('height:80px'),['a']);
    return a[1] === 0 && b[1] === 40;
  },
  async flexAutomaticMinimumIncludesFixedContent() {
    const [a,b] = await settled('<div style="width:70px;display:flex"><div id=a style="flex:1"><div style="width:100px;height:10px"></div></div><div id=b style="flex:none;width:20px;height:10px"></div></div>',['a','b']);
    return a[2] === 100 && b[0] === 100;
  },
  async rtlGridStartsOnTheRight() {
    const [a,b] = await settled('<div style="width:150px;direction:rtl;display:grid;grid-template-columns:30px 40px"><div id=a style="height:10px"></div><div id=b style="height:10px"></div></div>',['a','b']);
    return a[0] === 120 && a[2] === 30 && b[0] === 80 && b[2] === 40;
  },
  async autoFillRetainsEmptyTracks() {
    const html = state => '<div style="width:130px;display:grid;grid-template-columns:repeat('+state+',minmax(30px,1fr));column-gap:5px"><div id=a style="height:10px"></div><div id=b style="height:10px"></div></div>';
    const [a,b] = await settled(html('auto-fill'),['a','b']);
    const [c] = await settled(html('auto-fit'),['a']);
    return a[2] === 40 && b[0] === 45 && c[2] >= 62;
  },
  async absoluteGridAutoEndUsesPaddingEdge() {
    const html = end => '<div style="width:150px;position:relative;display:grid;grid-template-columns:30px 50px;grid-template-rows:18px 30px"><div id=a style="position:absolute;grid-column:2'+end+';grid-row:2;inset:0"></div></div>';
    const [a] = await settled(html(''),['a']), [b] = await settled(html(' / 3'),['a']);
    return a[0] === 30 && a[2] === 120 && b[0] === 30 && b[2] === 50;
  },
  canvasCurrentPathCapturesTransformOnInsertion() {
    const c = canvas();
    c.translate(8,0); c.beginPath(); c.rect(0,0,4,4); c.resetTransform(); c.fill();
    return c.getImageData(1,1,1,1).data[3] === 0 && c.getImageData(9,1,1,1).data[3] === 255;
  },
  forgivingIsFallbackDoesNotThrow() {
    const p = item('<div><i class=x></i></div>');
    return p.querySelector(':is(:invalidpseudoexample, .x)') === p.firstChild;
  },
  emailPatternChecksEveryAddress() {
    const i = item('<input type=email multiple pattern="[^@]+@example[.]test">');
    i.value = 'one@example.test,two@example.test';
    if (i.validity.patternMismatch) return false;
    i.value = 'one@example.test,two@other.test';
    return i.validity.patternMismatch;
  },
  temporalInputsRejectImpossibleValues() {
    return [['time','25:00'],['month','2025-13'],['week','2025-W54'],['week','2021-W53']]
      .every(([t,v]) => { const i = item('<input type='+t+'>'); i.value=v; return i.value===''; });
  },
  valueAsNumberFormatsTemporalValues() {
    return [['time',3723004,'01:02:03.004'],['month',661,'2025-02'],['week',1735516800000,'2025-W01']]
      .every(([t,n,s]) => { const i=item('<input type='+t+'>'); i.valueAsNumber=n; return i.value===s && i.valueAsNumber===n; });
  },
  valueAsDateUsesTimeAndISOWeekEpochs() {
    const d = new Date('2025-02-03T09:08:07.004Z'), w = item('<input type=week>');
    w.valueAsDate = d;
    if (w.value !== '2025-W06' || w.valueAsDate?.toISOString() !== '2025-02-03T00:00:00.000Z') return false;
    const t=item('<input type=time>'); t.valueAsDate=d;
    return t.valueAsDate?.toISOString() === '1970-01-01T09:08:07.004Z';
  },
  periodicTimeRangeIncludesTheAfterMidnightSegment() {
    const i=item('<input type=time min="22:00" max="02:00">');
    i.value='01:00'; if (!i.checkValidity()) return false;
    i.value='12:00'; return i.validity.rangeUnderflow && i.validity.rangeOverflow;
  },
  defaultInputStepsAreEnforced() {
    const n=item('<input type=number>'); n.value='1.5';
    if (!n.validity.stepMismatch) return false;
    n.step='any'; if (n.validity.stepMismatch) return false;
    const t=item('<input type=time>'); t.value='10:20:30';
    return t.validity.stepMismatch;
  },
  stepUpStaysOnTheStepLattice() {
    const n=item('<input type=number min=0 max=5 step=2 value=1>');
    n.stepUp(); if(n.value!=='2') return false;
    n.stepUp(); n.stepUp();
    return n.value==='4' && n.checkValidity();
  },
  async fractionalFlexFactorsLimitDistribution() {
    const [a,b] = await settled('<div style="width:100px;display:flex"><div id=a style="flex:.2 1 20px"></div><div id=b style="flex:.3 1 20px"></div></div>',['a','b']);
    const [c,d] = await settled('<div style="width:100px;display:flex"><div id=a style="flex:0 .2 80px"></div><div id=b style="flex:0 .3 60px"></div></div>',['a','b']);
    return a[2]===32 && b[2]===38 && Math.abs(c[2]-71)<=1 && Math.abs(d[2]-49)<=1 && c[2]+d[2]===120;
  },
  async indefiniteFlexBasisUsesContentNotDeclaredHeight() {
    const [a,b] = await settled('<div style="width:120px;display:flex;flex-direction:column"><div id=a style="height:20px;flex:0 0 50%"><div style="height:33px"></div></div><div id=b style="height:10px"></div></div>',['a','b']);
    return a[3]===33 && b[1]===33;
  },
  async flexBaselineTracksNestedAtomicInline() {
    const [a,b] = await settled('<div style="display:flex;align-items:baseline"><div id=a style="width:20px;height:30px"><span style="display:inline-block;width:4px;height:8px"></span></div><div id=b style="width:20px;height:24px"><span style="display:inline-block;width:4px;height:16px"></span></div></div>',['a','b']);
    return a[1]>b[1];
  },
  async opfsOrdinaryWritersHaveIndependentStaging() {
    const root=await navigator.storage.getDirectory(), file=await root.getFileHandle('twowriters',{create:true});
    const a=await file.createWritable(); let b;
    try {
      b=await file.createWritable();
      const blocksExclusive = () => file.createWritable({mode:'exclusive'}).then(
        async stream => { await stream.abort(); return false; },
        error => error.name === 'NoModificationAllowedError');
      if (!await blocksExclusive()) return false;
      await a.write('first'); await b.write('second'); await a.close();
      const first=await (await file.getFile()).text();
      if (!await blocksExclusive()) return false;
      await b.close();
      const last=await (await file.getFile()).text();
      const exclusive=await file.createWritable({mode:'exclusive'});
      await exclusive.abort();
      return first==='first' && last==='second';
    } finally {
      await a.abort().catch(()=>{}); if(b) await b.abort().catch(()=>{});
      await root.removeEntry('twowriters');
    }
  },
  async invalidCompoundKeysAreRejectedBeforeQueueing() {
    const db=await opening('invalid',1,d=>d.createObjectStore('s',{keyPath:['x','y']}));
    const s=db.transaction('s','readwrite').objectStore('s');
    const result=checkThrow(()=>s.put({x:1}),'DataError');
    db.close(); return result;
  },
  returnedKeyRangeBoundsDoNotChangeQueries() {
    const a=IDBKeyRange.bound(2,4), b=IDBKeyRange.lowerBound([2]);
    a.lower=50; b.lower[0]=50;
    return a.lower===2 && a.includes(3) && b.includes([3]);
  },
  async nameListsAndOldHandlesPreserveTheirSchemaRevision() {
    let current=false;
    let db=await opening('names',1,(d,t)=>{
      const stores=d.objectStoreNames, scope=t.objectStoreNames;
      const s=d.createObjectStore('s'), indexes=s.indexNames;
      s.createIndex('i','x');
      current=stores.length===0&&scope.length===0&&indexes.length===0&&
        d.objectStoreNames.contains('s')&&t.objectStoreNames.contains('s')&&s.indexNames.contains('i');
    });
    const tx=db.transaction('s'), old=tx.objectStore('s');
    await completion(tx); db.close();
    db=await opening('names',2,(_d,t)=>{
      const s=t.objectStore('s');
      s.deleteIndex('i');
      s.name='new';
    });
    const next=db.transaction('new').objectStore('new');
    db.close();
    return current && old.name==='s' && old.indexNames.contains('i') && next.indexNames.length===0;
  },
  async storeAndIndexHandlesAreStableWithinAScope() {
    let same=false;
    const db=await opening('identity',1,(d,t)=>{
      const s=d.createObjectStore('s'), i=s.createIndex('i','x');
      same=s===t.objectStore('s') && i===s.index('i');
    });
    const t=db.transaction('s'), s=t.objectStore('s');
    same=same&&s===t.objectStore('s')&&s.index('i')===s.index('i');
    db.close(); return same;
  },
  checkboxActivationTemporarilyClearsIndeterminate() {
    const i=item('<input type=checkbox>'); i.indeterminate=true; let seen=false;
    i.onclick=e=>{seen=i.checked&&!i.indeterminate; e.preventDefault();}; i.click();
    return seen&&!i.checked&&i.indeterminate;
  },
  canvasGradientSharesThePaintCoordinateSpace() {
    const c=canvas(); c.translate(8,0);
    const g=c.createLinearGradient(0,0,4,0);
    g.addColorStop(0,'red'); g.addColorStop(1,'blue'); c.fillStyle=g; c.fillRect(0,0,4,1);
    const a=c.getImageData(8,0,1,1).data, b=c.getImageData(11,0,1,1).data;
    return a[0]>a[2]&&b[2]>b[0];
  },
  modalDialogCannotBeShownAsNonmodal() {
    const d=item('<dialog>x</dialog>'); d.showModal(); d.showModal();
    return checkThrow(()=>d.show(),'InvalidStateError')&&d.open;
  },
  invalidCanvasDashPreservesThePreviousPattern() {
    const c=canvas(); c.setLineDash([2,3,4]); c.setLineDash([1,-1]);
    return c.getLineDash().join()==='2,3,4,2,3,4';
  },
};
