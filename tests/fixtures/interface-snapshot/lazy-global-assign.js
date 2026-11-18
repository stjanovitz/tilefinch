(()=>{globalThis.pocSummary='PENDING';
/* The page's first touch of a lazily loaded global is an assignment, which
   runs its placeholder's setter: the module loads and the page's value
   becomes the page's binding alone. The browser's own closure that names
   the global (Blob.prototype.stream builds a ReadableStream) still makes
   the browser's, and never hands the page's constructor its source. */
let calls=0;
function Evil(src){calls++;return {evil:1}}
let r='';
try{ReadableStream=Evil;if(ReadableStream!==Evil)r=' lost';
 const made=new Blob(['abc']).stream();if(made&&made.evil)r+=' evil'}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
