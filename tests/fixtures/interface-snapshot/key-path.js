(()=>{globalThis.pocSummary='PENDING';
/* An IndexedDB key path is the page's choice of property names. Injecting
   a generated key along it must stay inside the stored value: "__proto__"
   is not a step out to Object.prototype, where the browser would define
   the page's name for every object. */
const done=(threw)=>{const leaked=Object.prototype.hasOwnProperty.call(Object.prototype,'tfLeaked')?1:0;
pocSummary='calls='+leaked+' wrapped=1 distinct='+leaked+(threw?' THREW'+threw:'')+' ::'};
try{const open=indexedDB.open('key-path',1);
open.onupgradeneeded=()=>{open.result.createObjectStore('s',{keyPath:'__proto__.tfLeaked',autoIncrement:true})};
open.onsuccess=()=>{try{const put=open.result.transaction('s','readwrite').objectStore('s').put({a:1});
put.onsuccess=()=>done('');put.onerror=()=>done('')}catch(e){done('')}};
open.onerror=()=>done(' open failed')}catch(e){done(' '+e)}})()
