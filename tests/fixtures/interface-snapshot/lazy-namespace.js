(()=>{globalThis.pocSummary='PENDING';
/* A namespace binding the page replaced with its own object (CSS) is the
   page's: a lazy module loading later does not mark that object as an
   interface, so browser code reads the page's later change to it as the
   page made it. The probe function is browser code. */
const mine={f(){return 1}};globalThis.CSS=mine;
let r='',calls=0;
try{void ReadableStream;
 mine.f=function(){return 2};
 const probe={};__tilefinchTestBrowserPatch(probe,'look',function(){return mine.f()});
 if(probe.look()!==2)calls++}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
