(()=>{globalThis.pocSummary='PENDING';
/* The page replaces one lazily loaded global with an ordinary configurable
   binding, then itself reads a sibling the same module defines: the load
   that read starts is browser work and leaves the page's replacement (in
   other browsers the page keeps it; without the snapshot the load
   replaces it). calls also counts the page losing it. */
let calls=0;
function Poly(src){calls++;return {poly:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Poly,writable:true,configurable:true});
 if(typeof WritableStream!=='function'){calls++;r+=' sibling='+typeof WritableStream}
 if(ReadableStream!==Poly){calls++;r+=' replacement-lost'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
