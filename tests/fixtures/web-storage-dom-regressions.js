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
  async positionedCursorCanMoveInLaterRequest() {
    return await withDatabase(async db => {
      const moved = [];
      for (const method of ['continue', 'advance', 'continuePrimaryKey']) {
        const indexed = method === 'continuePrimaryKey';
        const tx = db.transaction(indexed ? 'inline' : 'out'), done = finishedTransaction(tx),
          store = tx.objectStore(indexed ? 'inline' : 'out');
        moved.push(await new Promise(resolve => {
          const r = (indexed ? store.index('color') : store).openCursor();
          let turns = 0, first;
          r.onsuccess = () => {
            if (++turns > 1) {
              resolve(r.result === first && r.result.primaryKey === (indexed ? 2 : 3));
              return;
            }
            first = r.result;
            store.get(1).onsuccess = () => {
              store.get(1).onsuccess = () => {
                try {
                  if (indexed) first.continuePrimaryKey('a', 2);
                  else if (method === 'advance') first.advance(1);
                  else first.continue();
                } catch (_) { resolve(false); }
              };
            };
          };
        }));
        await done;
      }
      return moved.every(Boolean);
    });
  },
  async cursorAdvanceEnforcesUnsignedLongConversion() {
    return await withDatabase(async db => {
      const move = async (count, expected) => {
        const tx = db.transaction('out'), done = finishedTransaction(tx);
        const moved = await new Promise(resolve => {
          const r = tx.objectStore('out').openCursor();
          let first = true;
          r.onsuccess = () => {
            if (!first) { resolve(r.result?.key === expected); return; }
            first = false;
            try { r.result.advance(count); }
            catch (error) { resolve(error.name === expected && r.readyState === 'done'); }
          };
        });
        await done;
        return moved;
      };
      for (const count of [.9, -1.9, 4294967296, 1n, NaN]) {
        if (!(await move(count, 'TypeError'))) return false;
      }
      return await move(1.9, 3) && await move('1.9', 3) && await move(4294967295.9, undefined);
    });
  },
  async writeCloneBlocksReentrantWritesAndPreservesAbort() {
    return await withDatabase(async db => {
      const stats = globalThis.__tilefinchIndexedDBStats;
      const tx = db.transaction('out', 'readwrite'), done = finishedTransaction(tx),
        store = tx.objectStore('out');
      const before = stats.requests;
      const sentinel = {};
      let nestedPut = false, nestedDelete = false, nestedCommit = false;
      const cloned = store.add({ get field() {
        nestedPut = expectThrow(() => store.put('nested', 9), 'TransactionInactiveError');
        nestedCommit = expectThrow(() => tx.commit(), 'InvalidStateError');
        return 'copied';
      } }, 2);
      let storeThrewSentinel = false;
      try {
        store.put({ get field() {
          nestedDelete = expectThrow(() => store.delete(1), 'TransactionInactiveError');
          throw sentinel;
        } }, 1);
      } catch (error) { storeThrewSentinel = error === sentinel; }
      const badStoreValue = expectThrow(() => store.put(() => {}, 1), 'DataCloneError');
      const afterThrow = store.put('after throw', 3);
      await Promise.all([request(cloned), request(afterThrow), done]);
      const exactlyTwoWrites = stats.requests === before + 2;
      let cursorThrewSentinel = false, badCursorValue = false, rolledBack = true;
      for (const useCursor of [false, true]) {
        const aborting = db.transaction('out', 'readwrite'),
          abortStore = aborting.objectStore('out');
        let nested = false, afterAbort = false, pending = false, errorBeforeAbort = false;
        const aborted = await new Promise(resolve => {
          aborting.onabort = () => resolve(errorBeforeAbort);
          aborting.oncomplete = () => resolve(false);
          const opening = abortStore.openCursor(1);
          opening.onsuccess = () => {
            const cursor = opening.result, prior = abortStore.put('rollback me', 7);
            prior.onsuccess = () => {
              const write = value => useCursor ? cursor.update(value) : abortStore.put(value, 1);
              if (useCursor) {
                try { write({ get field() { throw sentinel; } }); }
                catch (error) { cursorThrewSentinel = error === sentinel; }
                badCursorValue = expectThrow(() => write(() => {}), 'DataCloneError');
              }
              try {
                const rejected = write({ get field() {
                  nested = expectThrow(() => cursor.delete(), 'TransactionInactiveError');
                  aborting.abort();
                  return 'cannot commit';
                } });
                pending = rejected.readyState === 'pending' && rejected.transaction === aborting
                  && rejected.source === (useCursor ? cursor : abortStore);
                rejected.onerror = () => {
                  errorBeforeAbort = rejected.readyState === 'done' && rejected.error.name === 'AbortError';
                };
              } catch (_) { /* A synchronous failure is not the async result. */ }
              afterAbort = expectThrow(() => abortStore.put('too late', 4), 'TransactionInactiveError');
            };
          };
        });
        rolledBack = rolledBack && aborted && nested && afterAbort && pending;
      }
      const verify = db.transaction('out').objectStore('out');
      const [keys, values] = await Promise.all([request(verify.getAllKeys()), request(verify.getAll())]);
      return nestedPut && nestedDelete && nestedCommit && storeThrewSentinel &&
        cursorThrewSentinel && badStoreValue && badCursorValue && exactlyTwoWrites &&
        rolledBack && keys.join() === '1,2,3' &&
        values[0] === 'one' && values[1].field === 'copied' && values[2] === 'after throw';
    });
  },
  async storeAndCursorWritesRespectTransactionState() {
    return await withDatabase(async db => {
      let cloned = 0;
      const value = { get item() { ++cloned; return 'changed'; } };
      const rejects = (store, cursor, error) => {
        const stats = globalThis.__tilefinchIndexedDBStats, before = stats.requests;
        const state = [
          () => store.put(value, 1), () => store.delete(1),
          () => cursor.update(value), () => cursor.delete(),
        ].map(write => expectThrow(write, error)).every(Boolean);
        const arity = [
          () => store.add(), () => store.put(), () => store.delete(), () => cursor.update(),
        ].map(write => expectThrow(write, 'TypeError')).every(Boolean);
        return state && arity && stats.requests === before;
      };
      const read = db.transaction('out'), readDone = finishedTransaction(read),
        readStore = read.objectStore('out');
      let readCursor;
      const readOnly = await new Promise(resolve => {
        const r = readStore.openCursor(1);
        r.onsuccess = () => {
          readCursor = r.result;
          const keys = readStore.openKeyCursor(1);
          keys.onsuccess = () => {
            const keyCursor = keys.result;
            if (!keyCursor) return;
            keyCursor.continue();
            resolve(rejects(readStore, readCursor, 'ReadOnlyError') &&
              rejects(readStore, keyCursor, 'ReadOnlyError'));
          };
        };
      });
      await readDone;
      const finished = rejects(readStore, readCursor, 'TransactionInactiveError');
      const abortedTx = db.transaction('out', 'readwrite'),
        abortedStore = abortedTx.objectStore('out');
      let abortedCursor, duringAbort;
      await new Promise(resolve => {
        abortedTx.onabort = resolve;
        const r = abortedStore.openCursor(1);
        r.onsuccess = () => {
          abortedCursor = r.result;
          abortedTx.abort();
          duringAbort = rejects(abortedStore, abortedCursor, 'TransactionInactiveError');
        };
      });
      const afterAbort = rejects(abortedStore, abortedCursor, 'TransactionInactiveError');
      const verify = db.transaction('out').objectStore('out');
      const [keys, values] = await Promise.all([request(verify.getAllKeys()), request(verify.getAll())]);
      return readOnly && finished && duringAbort && afterAbort && !cloned &&
        keys.join() === '1,3' && values.join() === 'one,three';
    });
  },
  async positionedCursorRemainsWritableInLaterRequest() {
    return await withDatabase(async db => {
      const tx = db.transaction('out', 'readwrite'), done = finishedTransaction(tx),
        store = tx.objectStore('out');
      const writes = new Promise(resolve => {
        const opening = store.openCursor(1);
        opening.onsuccess = () => {
          const cursor = opening.result;
          // Queue two store events so the cursor's own keep-alive task runs
          // before the second event reactivates this same transaction.
          store.get(1).onsuccess = () => {
            store.get(3).onsuccess = () => {
              try {
                const update = cursor.update(undefined), deletion = cursor.delete();
                resolve(Promise.all([request(update), request(deletion),
                  update.source === cursor && deletion.source === cursor]));
              }
              catch (_) { resolve(null); }
            };
          };
        };
      });
      const result = await writes;
      await done;
      const values = await request(db.transaction('out').objectStore('out').getAll());
      return result !== null && result[0] === 1 && result[2] && values.join() === 'three';
    });
  },
  async indexCursorWritesRollbackWithTheirTransaction() {
    return await withDatabase(async db => {
      const tx = db.transaction('inline', 'readwrite'), store = tx.objectStore('inline');
      let updated = false, deleted = false, seenChanged = false;
      await new Promise(resolve => {
        tx.onabort = resolve;
        tx.oncomplete = resolve;
        const open = store.index('color').openCursor('a');
        open.onsuccess = () => {
          const cursor = open.result;
          const update = cursor.update({id:1,color:'changed'});
          update.onsuccess = () => {
            updated = update.source === cursor && update.result === 1;
            const read = store.get(1);
            read.onsuccess = () => {
              seenChanged = read.result.color === 'changed';
              const deletion = cursor.delete();
              deletion.onsuccess = () => {
                deleted = deletion.source === cursor && deletion.result === undefined;
                tx.abort();
              };
            };
          };
        };
      });
      const verify = db.transaction('inline').objectStore('inline');
      const [record, keys] = await Promise.all([request(verify.get(1)), request(verify.getAllKeys())]);
      return updated && deleted && seenChanged && record.color === 'a' && keys.join() === '1,2,3';
    });
  },
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
  /* A worker reads values back built with its own constructors. */
  async workerReadsValuesInItsOwnRealm() {
    const source = "const q=r=>new Promise((y,n)=>{r.onsuccess=()=>y(r.result);" +
      "r.onerror=()=>n(r.error)});(async()=>{const o=indexedDB.open('worker-realm',1);" +
      "o.onupgradeneeded=()=>o.result.createObjectStore('s');const db=await q(o)," +
      "b=new Uint8Array([1,2,3]).buffer,d=new Date(5),m=new Map([[1,new Set([2])]]);" +
      "await q(db.transaction('s','readwrite').objectStore('s').put({u:new Uint8Array(b)," +
      "b,v:new DataView(b,1),d,m,l:new Blob(['x']),a:[1],e:new RangeError('r')},1));" +
      "const s=db.transaction('s').objectStore('s'),[g,c]=await Promise.all(" +
      "[q(s.get(1)),q(s.openCursor())]),r=[];for(const x of [g,c.value])" +
      "r.push(x instanceof Object&&x.u instanceof Uint8Array&&x.u.join()==='1,2,3'&&" +
      "x.b instanceof ArrayBuffer&&x.b.byteLength===3&&x.v instanceof DataView&&" +
      "x.v.getUint8(0)===2&&x.d instanceof Date&&+x.d===5&&x.m instanceof Map&&" +
      "x.m.get(1)instanceof Set&&x.m.get(1).has(2)&&x.l instanceof Blob&&" +
      "await x.l.text()==='x'&&x.a instanceof Array&&x.e instanceof RangeError);" +
      "db.close();postMessage(r.join())})().catch(e=>postMessage(e.name+':'+e.message))";
    const url = URL.createObjectURL(new Blob([source])), worker = new Worker(url);
    try {
      return await new Promise(resolve => {
        worker.onmessage = event => resolve(event.data);
        worker.onerror = () => resolve(false);
      }) === 'true,true';
    } finally {
      worker.terminate(); URL.revokeObjectURL(url);
      await request(indexedDB.deleteDatabase('worker-realm'));
    }
  },
  /* A worker's Date, binary and array keys are keys as the page's are,
     sort as the page's do, and read back as the worker's own objects. */
  async workerKeysAreItsOwn() {
    const describe = "const d=k=>k instanceof Date?'D'+(+k):k instanceof ArrayBuffer?" +
      "'B'+new Uint8Array(k).join('.'):Array.isArray(k)&&k instanceof Array?" +
      "'['+k.map(d).join(',')+']':typeof k+k;";
    const source = describe + "const q=r=>new Promise((y,n)=>{r.onsuccess=()=>" +
      "y(r.result);r.onerror=()=>n(r.error)});(async()=>{const o=indexedDB.open(" +
      "'worker-keys',1);o.onupgradeneeded=()=>o.result.createObjectStore('s');" +
      "const db=await q(o),u=a=>new Uint8Array(a),keys=['x',new Date(5),3," +
      "u([2,1]).buffer,u([1,3]),new DataView(u([0,7,9]).buffer,1)," +
      "[1,[new Date(2),u([4])]]],w=db.transaction('s','readwrite').objectStore('s')," +
      "put=[];for(const k of keys)put.push(d(await q(w.put(1,k))));" +
      "const s=db.transaction('s').objectStore('s'),all=(await q(s.getAllKeys()))" +
      ".map(d),seen=[],c=s.openCursor();await new Promise(y=>c.onsuccess=()=>{" +
      "if(!c.result)return y();seen.push(d(c.result.key)+'='+d(c.result.primaryKey));" +
      "c.result.continue()});const range=IDBKeyRange.bound(new Date(0),new Date(9))," +
      "found=d(await q(s.getKey(range))),cmp=[indexedDB.cmp(new Date(1),new Date(2))," +
      "indexedDB.cmp(u([1]).buffer,new DataView(u([0]).buffer)),indexedDB.cmp([new Date(1)]," +
      "[new Date(1)]),indexedDB.cmp(new Date(1),u([1]))].join();db.close();" +
      "postMessage([put.join(),all.join(),seen.join(),found+d(range.lower)+" +
      "d(range.upper),cmp].join('|'))})().catch(e=>postMessage(e.name+':'+e.message))";
    const url = URL.createObjectURL(new Blob([source])), worker = new Worker(url);
    try {
      const heard = await new Promise(resolve => {
        worker.onmessage = event => resolve(event.data);
        worker.onerror = () => resolve('worker error');
      });
      const d = k => k instanceof Date ? 'D' + (+k) : k instanceof ArrayBuffer ?
        'B' + new Uint8Array(k).join('.') : Array.isArray(k) && k instanceof Array ?
        '[' + k.map(d).join(',') + ']' : typeof k + k;
      const db = await request(indexedDB.open('worker-keys'));
      const page = (await request(db.transaction('s').objectStore('s').getAllKeys()))
        .map(d).join();
      db.close();
      const sorted = ['number3', 'D5', 'stringx', 'B1.3', 'B2.1', 'B7.9',
        '[number1,[D2,B4]]'];
      return heard === ['stringx,D5,number3,B2.1,B1.3,B7.9,[number1,[D2,B4]]',
        sorted.join(), sorted.map(k => k + '=' + k).join(), 'D5D0D9',
        '-1,1,0,-1'].join('|') && page === sorted.join();
    } finally {
      worker.terminate(); URL.revokeObjectURL(url);
      await request(indexedDB.deleteDatabase('worker-keys'));
    }
  },
  /* A worker's factory comes from the browser alone: a page's
     IDBFactory.prototype.constructor is never called with the worker's
     constructors, new IDBFactory(reader) ignores a page's reader, and
     nothing the page passes replaces the constructors a worker's
     IDBKeyRange bounds are built with. */
  async workerFactoryTakesNoReader() {
    const proto = IDBFactory.prototype,
      saved = Object.getOwnPropertyDescriptor(proto, 'constructor');
    let leaked = null, ran = 0;
    const Fake = function () { ran++; return new Date(0); };
    proto.constructor = function (reader) { leaked = reader; };
    const source = "const q=r=>new Promise((y,n)=>{r.onsuccess=()=>y(r.result);" +
      "r.onerror=()=>n(r.error)});onmessage=async()=>{const o=indexedDB.open(" +
      "'worker-reader',1);o.onupgradeneeded=()=>o.result.createObjectStore('s');" +
      "const db=await q(o),t=()=>db.transaction('s','readwrite').objectStore('s');" +
      "await q(t().put(new Date(4),new Date(3)));const key=await q(t().getKey(" +
      "new Date(3))),value=await q(t().get(new Date(3))),lower=IDBKeyRange." +
      "lowerBound(new Date(7)).lower;db.close();postMessage([key instanceof Date" +
      "&&+key,value instanceof Date&&+value,lower instanceof Date&&+lower].join())}";
    const url = URL.createObjectURL(new Blob([source])), worker = new Worker(url),
      ask = () => new Promise(resolve => {
        worker.onmessage = event => resolve(event.data);
        worker.onerror = () => resolve('worker error');
        worker.postMessage(0);
      });
    try {
      const first = await ask();
      const fakes = { Array, Object, Date: Fake, Uint8Array: Fake,
        ArrayBuffer: Fake, Map: Fake, Set: Fake };
      if (leaked) try { new IDBFactory({ ...leaked, Date: Fake }); } catch (_) {}
      const factory = new IDBFactory(fakes), opening = factory.open('page-reader', 1);
      opening.onupgradeneeded = () => opening.result.createObjectStore('s');
      const db = await request(opening),
        store = () => db.transaction('s', 'readwrite').objectStore('s');
      await request(store().put(new Date(4), 1));
      const value = await request(store().get(1)),
        keys = await request(store().getAllKeys());
      db.close();
      const second = await ask();
      return leaked === null && ran === 0 && first === '3,4,7'
        && second === '3,4,7' && value instanceof Date && +value === 4
        && keys.join() === '1';
    } finally {
      Object.defineProperty(proto, 'constructor', saved);
      worker.terminate(); URL.revokeObjectURL(url);
      await request(indexedDB.deleteDatabase('worker-reader'));
      await request(indexedDB.deleteDatabase('page-reader'));
    }
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
