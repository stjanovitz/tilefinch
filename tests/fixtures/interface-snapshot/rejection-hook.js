(()=>{globalThis.pocSummary='PENDING';
/* The window realm's rejection tracker never calls a page function the
   page named like the worker realm's hook. */
let calls=0;
globalThis.__tilefinchQueuePromiseRejection=function(){calls++};
Promise.reject(new Error('x')).catch(()=>{});
setTimeout(()=>{pocSummary='calls='+calls},50)})()
