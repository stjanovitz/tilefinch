(()=>{globalThis.pocSummary='PENDING';
/* The page keeps a global placeholder's accessor, lets its module load,
   then wraps the global with an accessor that calls the placeholder's
   functions, and assigns through it: the placeholder's setter answers
   with the browser's own binding, not the page's wrapper, so the
   assignment ends at once with the page's value bound. calls counts the
   wrapper's calls beyond the page's one, and a stream the browser could
   not build. */
let calls=0;
const d=Object.getOwnPropertyDescriptor(globalThis,'ReadableStream');
let r='';
try{void ReadableStream;
 Object.defineProperty(globalThis,'ReadableStream',{configurable:true,
  get(){calls++;return d.get.call(this)},set(v){calls++;d.set.call(this,v)}});
 ReadableStream=5;
 if(ReadableStream!==5)r+=' page='+typeof ReadableStream}
catch(e){r=' THREW '+e}
/* the browser's own binding still builds its streams */
try{const made=new Blob(['abc']).stream();
 if(!made||typeof made.getReader!=='function')throw 0}
catch(e){calls++;r+=' nostream'}
pocSummary='calls='+(calls-1)+r})()
