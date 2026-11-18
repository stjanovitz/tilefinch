(()=>{globalThis.pocSummary='PENDING';
/* The page binds the names of the host's temporary hand-over globals
   (Streams' WritableStream for opfs.js, the time zone for intl.js) as
   non-configurable before their modules load. The host's definitions are
   refused, so the modules must not load: neither may read the page's
   binding in their place. calls counts the browser using the page's
   function or calling its getter. */
let calls=0;
function Evil(sink){calls++;return {evil:1}}
let r='';
try{Object.defineProperty(globalThis,'__tilefinchOpfsWritableStream',{value:Evil,writable:false,configurable:false});
 Object.defineProperty(globalThis,'__tilefinchDefaultTimeZone',{get(){calls++;return 'Page/Zone'},configurable:false});
 try{void FileSystemHandle;
  const W=globalThis.FileSystemWritableFileStream;
  if(W&&Object.getPrototypeOf(W)===Evil){calls++;r+=' writable-is-page'}
  else r+=' opfs-loaded'}
 catch(e){r+=' opfs-refused'}
 try{r+=' '+new Intl.DateTimeFormat().resolvedOptions().timeZone}
 catch(e){r+=' intl-refused'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
