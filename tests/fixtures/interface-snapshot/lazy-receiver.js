(()=>{globalThis.pocSummary='PENDING';
/* A lazy placeholder's setter makes the change of the code that assigned
   through it, whatever receiver that code names: a page's
   setter.call(Object.prototype, f), or its first-touch assignment of the
   member on the prototype, is the page's change and stays out of the
   browser's own view. The probe function is browser code
   (__tilefinchTestBrowserPatch). */
let calls=0;const evil=function(){calls++;return 'page'};
let r='';
try{const toBlob=Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype,'toBlob');
 toBlob.set.call(Object.prototype,evil);
 if(({}).toBlob!==evil)r+=' lost';
 Document.prototype.createTreeWalker=evil;
 if(document.createTreeWalker!==evil)r+=' lost';
 const probe={};
 __tilefinchTestBrowserPatch(probe,'look',function(){const o={};
  if(typeof o.toBlob==='function')o.toBlob();
  document.createTreeWalker(document.body)});
 probe.look()}
catch(e){r=' THREW '+e}
delete Object.prototype.toBlob;
pocSummary='calls='+calls+r})()
