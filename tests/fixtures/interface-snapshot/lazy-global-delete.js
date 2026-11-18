(()=>{globalThis.pocSummary='PENDING';
/* The page deletes a lazily loaded global before its module loads and
   assigns its own: the browser's own closure that names it keeps the
   browser's placeholder. */
let calls=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{delete globalThis.ReadableStream;globalThis.ReadableStream=Evil;
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil'}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
