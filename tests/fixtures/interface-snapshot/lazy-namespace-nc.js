(()=>{globalThis.pocSummary='PENDING';
/* The page binds Intl to its own object, non-configurable, before
   intl.js loads (a Number's toLocaleString loads it): the module binds
   its own Intl in the browser's view alone and marks that one, not the
   page's object, so browser code reads the page's later change to its
   object as the page made it. The probe function is browser code. */
const mine={f(){return 1}};
let r='',calls=0;
try{Object.defineProperty(globalThis,'Intl',{value:mine,writable:false,configurable:false});
 r=' '+(1234.5).toLocaleString('en-US');
 mine.f=function(){return 2};
 const probe={};__tilefinchTestBrowserPatch(probe,'look',function(){return mine.f()});
 if(probe.look()!==2){calls++;r+=' marked'}
 if(Intl!==mine){calls++;r+=' page-lost'}}
catch(e){r=' THREW '+e}
pocSummary='calls='+calls+r})()
