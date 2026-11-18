(()=>{globalThis.pocSummary='PENDING';
/* The page redefines a lazily loaded global before its module loads: the
   browser's own closure that names it keeps the browser's placeholder,
   so the module loads for it and it builds the browser's object. */
let calls=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{value:Evil,writable:true,configurable:true});
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil'}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
