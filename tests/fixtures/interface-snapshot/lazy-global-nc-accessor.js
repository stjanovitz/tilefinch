(()=>{globalThis.pocSummary='PENDING';
/* As lazy-global-nc-define.js, with a non-configurable accessor: the
   module's binding is not handed to the page's setter, and the browser's
   reads never reach the page's getter. */
let calls=0,handed=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'ReadableStream',{get(){return Evil},set(v){handed++},configurable:false});
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil';
 if(!made||typeof made.getReader!=='function'){calls++;r+=' nostream'}
 if(handed){calls++;r+=' handed'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
