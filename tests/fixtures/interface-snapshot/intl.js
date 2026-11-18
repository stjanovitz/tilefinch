(()=>{globalThis.pocSummary='PENDING';
/* Intl is a namespace a lazy module publishes: Number and Date
   toLocale*String format through its classes in the browser's own
   closure, which must not reach a page's replacement of their prototype
   members (the module marks the namespace when it loads). */
let calls=0;const seen=[];
const wrap=(proto,key,tag)=>{const d=Object.getOwnPropertyDescriptor(proto,key);
  const count=()=>{calls++;if(!seen.includes(tag))seen.push(tag)};
  Object.defineProperty(proto,key,d.get
    ?{configurable:true,get(){count();return d.get.call(this)}}
    :{configurable:true,writable:true,value:function(...a){count();return d.value.apply(this,a)}})};
wrap(Intl.NumberFormat.prototype,'format','NumberFormat.format');
wrap(Intl.NumberFormat.prototype,'formatToParts','NumberFormat.formatToParts');
wrap(Intl.DateTimeFormat.prototype,'format','DateTimeFormat.format');
wrap(Intl.DateTimeFormat.prototype,'formatToParts','DateTimeFormat.formatToParts');
let text='';
try{text=(1234.5).toLocaleString('en-US')+' '+(12n).toLocaleString('en-US')
  +' '+new Date(0).toLocaleDateString('en-US',{timeZone:'UTC'})}
catch(e){text='THREW '+e}
pocSummary='calls='+calls+' '+seen.join(',')+' :: '+text})()
