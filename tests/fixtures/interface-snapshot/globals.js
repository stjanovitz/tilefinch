(()=>{globalThis.pocSummary='PENDING';
/* Replaces the global bindings of the ECMAScript constructors and
   namespaces with counting proxies, runs the same DOM workload (which names
   none of them), and reports the proxies browser code reached by reading
   the global at call time. */
const RC=Reflect.construct,RA=Reflect.apply,RG=Reflect.get,P=Proxy,dP=Object.defineProperty;
const names=['Object','Array','Map','Set','WeakMap','WeakSet','Promise','Error','TypeError','RangeError',
'SyntaxError','RegExp','Date','String','Number','Boolean','Symbol','ArrayBuffer','Uint8Array','Float32Array','Proxy',
'Math','JSON','Reflect'];
const seen={__proto__:null};let calls=0,wrapped=0,phase='sync';const byPhase={__proto__:null,sync:0,async:0};
const note=(n)=>{calls++;byPhase[phase]++;seen[n]=(seen[n]|0)+1};
for(let i=0;i<names.length;i++){const n=names[i],orig=globalThis[n];let p;
p=new P(orig,{construct(t,a,nt){note('new '+n);return RC(t,a,nt===p?t:nt)},
apply(t,th,a){note(n+'()');return RA(t,th,a)},
get(t,k,r){if(k!=='prototype')note(n+'.'+(typeof k==='symbol'?'@@'+k.description:k));return RG(t,k,t)}});
dP(globalThis,n,{value:p,writable:true,configurable:true,enumerable:false});wrapped++}
/*WORKLOAD*/
phase='async';
setTimeout(()=>{let names='',distinct=0;for(const k in seen){distinct++;if(distinct<=40)names+=' '+k+'='+seen[k]}
pocSummary='calls='+calls+' wrapped='+wrapped+' distinct='+distinct+(threw?' THREW'+threw:'')+' ::'+names},120)})()
