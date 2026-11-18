(()=>{globalThis.pocSummary='PENDING';
/* The page redefines a lazily loaded global as read-only but
   configurable before its module loads: the module's changes are made in
   the browser's view alone, its closure builds the browser's object, and
   the page keeps its binding. calls also counts the page losing it. */
let calls=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Evil,writable:false,configurable:true});
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil';
 if(!made||typeof made.getReader!=='function'){calls++;r+=' nostream'}
 if(ReadableStream!==Evil){calls++;r+=' page-lost'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
