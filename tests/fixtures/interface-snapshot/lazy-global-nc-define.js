(()=>{globalThis.pocSummary='PENDING';
/* The page redefines a lazily loaded global as non-configurable before
   its module loads, so the module can neither remove the placeholder nor
   bind the name: the browser's view takes both changes alone, and its
   closure that names the global builds the browser's object. calls also
   counts a stream that is not the browser's (the module must still load
   and bind for the browser). */
let calls=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Evil,writable:false,configurable:false});
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil';
 if(!made||typeof made.getReader!=='function'){calls++;r+=' nostream'}
 if(ReadableStream!==Evil)r+=' page-lost'}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
