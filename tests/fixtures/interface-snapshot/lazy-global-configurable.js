(()=>{globalThis.pocSummary='PENDING';
/* The page replaces, deletes and wraps lazily loaded globals of one module
   with ordinary configurable bindings (as a polyfill does), then a browser
   API that builds a stream internally loads the module. The load removes
   the placeholders and binds the module's names in the browser's view
   alone: the page keeps its replacement and its wrapper (which still
   forwards to the browser's class), a name it deleted stays deleted, and
   the browser's own stream is the browser's. calls also counts each of
   those the page lost. */
let calls=0, wrapHits=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Evil,writable:true,configurable:true});
 delete globalThis.WritableStream;
 const d=Object.getOwnPropertyDescriptor(globalThis,'TransformStream');
 Object.defineProperty(globalThis,'TransformStream',{configurable:true,
  get(){wrapHits++;return d.get.call(this)},set(v){d.set.call(this,v)}});
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil';
 if(!made||typeof made.getReader!=='function'){calls++;r+=' nostream'}
 if(ReadableStream!==Evil){calls++;r+=' replacement-lost'}
 if(typeof WritableStream!=='undefined'||'WritableStream' in globalThis){calls++;r+=' deleted-back'}
 const w=Object.getOwnPropertyDescriptor(globalThis,'TransformStream');
 if(!w||!w.get){calls++;r+=' wrapper-lost'}
 else if(typeof TransformStream!=='function'||!wrapHits){calls++;r+=' wrapper-broken'}
 if(made instanceof ReadableStream){calls++;r+=' instanceof'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
