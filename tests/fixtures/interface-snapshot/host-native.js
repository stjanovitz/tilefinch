(()=>{globalThis.pocSummary='PENDING';
/* A host native the page calls directly is browser code: converting the
   page's plain object to a string for it reads Object.prototype.toString
   as the browser has it, not the page's replacement. The profiling build
   wraps every host native in a trampoline, which must keep that. */
const real=Object.prototype.toString;let calls=0;
Object.prototype.toString=function(){calls++;return 'https://page.test/patched'};
let resolved='';try{resolved=String(__tilefinchResolveURL({},'https://base.test/dir/'))}
finally{Object.prototype.toString=real}
pocSummary='calls='+calls+' wrapped=1 distinct='+calls+' :: '+resolved})()
