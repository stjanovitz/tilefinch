(()=>{globalThis.pocSummary='PENDING';
/* Wraps every configurable method and accessor on the prototypes of the
   platform interfaces (the global constructors that are not ECMAScript's),
   runs the same DOM workload, and reports the wrappers that ran while
   another wrapper was already running (browser code calling a public
   method or accessor for its own purposes) or after the script ended. */
const gOPD=Object.getOwnPropertyDescriptor,dP=Object.defineProperty,RA=Reflect.apply,oK=Reflect.ownKeys,
gOPN=Object.getOwnPropertyNames;
const ES={__proto__:null,Object:1,Function:1,Array:1,String:1,Number:1,Boolean:1,Symbol:1,Map:1,Set:1,WeakMap:1,
WeakSet:1,WeakRef:1,Promise:1,RegExp:1,Date:1,Error:1,TypeError:1,RangeError:1,SyntaxError:1,ReferenceError:1,
EvalError:1,URIError:1,AggregateError:1,InternalError:1,ArrayBuffer:1,SharedArrayBuffer:1,DataView:1,Proxy:1,BigInt:1,
Iterator:1,FinalizationRegistry:1,Int8Array:1,Uint8Array:1,Uint8ClampedArray:1,Int16Array:1,Uint16Array:1,
Int32Array:1,Uint32Array:1,Float32Array:1,Float64Array:1,BigInt64Array:1,BigUint64Array:1,Float16Array:1};
const seen={__proto__:null};let calls=0,wrapped=0,depth=0,phase='sync',interfaces=0;
const byPhase={__proto__:null,sync:0,async:0};
const wrap=(f,name)=>function(...a){if(depth>0||phase==='async'){calls++;byPhase[phase]++;seen[name]=(seen[name]|0)+1}
depth++;try{return RA(f,this,a)}finally{depth--}};
const G=gOPN(globalThis);
for(let i=0;i<G.length;i++){const g=G[i];if(ES[g])continue;const c=g.charCodeAt(0);if(c<65||c>90)continue;
const gd=gOPD(globalThis,g);if(!gd||typeof gd.value!=='function')continue;
const pd=gOPD(gd.value,'prototype');if(!pd||!pd.value||typeof pd.value!=='object')continue;
const o=pd.value,keys=oK(o);interfaces++;
for(let j=0;j<keys.length;j++){const k=keys[j];if(k==='constructor'||typeof k==='symbol')continue;
const d=gOPD(o,k);if(!d||!d.configurable)continue;const name=g+'.'+k;
if(typeof d.value==='function'){dP(o,k,{value:wrap(d.value,name),writable:true,configurable:true,
enumerable:d.enumerable});wrapped++}
else if(d.get||d.set){dP(o,k,{get:d.get&&wrap(d.get,'get '+name),set:d.set&&wrap(d.set,'set '+name),
configurable:true,enumerable:d.enumerable});wrapped++}}}
/*WORKLOAD*/
phase='async';
setTimeout(()=>{let names='',distinct=0;for(const k in seen){distinct++;if(distinct<=40)names+=' '+k+'='+seen[k]}
pocSummary='calls='+calls+' wrapped='+wrapped+' distinct='+distinct+(threw?' THREW'+threw:'')+' ::'+names},120)})()
