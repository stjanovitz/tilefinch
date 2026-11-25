(()=>{globalThis.pocSummary='PENDING';
/* The private natives and setters the bootstrap captures and deletes
   before page script must be gone by then: a delete of a frozen native
   fails silently and would leave it page-callable. calls counts the names
   page script can still reach. */
let calls=0,r='';
const names=['__tilefinchRegisterShadowRoot','__tilefinchAssignSlot',
 '__tilefinchSetNodeIteratorHooks','__tilefinchNodeIteratorHooks',
 '__tilefinchLazyLoading','__tilefinchTrustedEventShare'];
for(const f of ['Shadow','Documents','Forms','Traversal','Rule','FrameClone',
 'Geometry','Validation','DetachedDocuments'])
 names.push('__tilefinchEnsure'+f+'Bootstrap');
for(const n of names)
 if(Object.getOwnPropertyDescriptor(globalThis,n)||n in globalThis){calls++;r+=' '+n}
pocSummary='calls='+calls+r})()
