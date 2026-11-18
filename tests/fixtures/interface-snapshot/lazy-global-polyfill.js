(()=>{globalThis.pocSummary='PENDING';
/* A polyfill (core-js style) replaces a lazily loaded global with a
   non-enumerable, configurable, writable binding, then a browser API
   builds a stream internally. As in other browsers, the page keeps its
   polyfill and the browser builds its own stream. calls counts the
   browser using the polyfill and the page losing it. */
let calls=0;
function Poly(){calls++;this.poly=1}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Poly,writable:true,configurable:true,enumerable:false});
 const made=new Blob(['abc']).stream();if(made&&made.poly)r+=' poly';
 if(!made||typeof made.getReader!=='function'){calls++;r+=' nostream'}
 if(globalThis.ReadableStream!==Poly){calls++;r+=' polyfill-lost'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
