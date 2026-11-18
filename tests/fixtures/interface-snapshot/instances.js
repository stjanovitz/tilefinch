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
/* own properties on three page-reachable instances shadow every method
   their prototype chains define */
const gPO=Object.getPrototypeOf,shadow=(o,label)=>{for(let p=gPO(o);p&&p!==Object.prototype;p=gPO(p)){const keys=oK(p);
for(let j=0;j<keys.length;j++){const k=keys[j];if(k==='constructor'||typeof k==='symbol')continue;
if(gOPD(o,k))continue;const d=gOPD(p,k);if(!d||typeof d.value!=='function')continue;
dP(o,k,{value:wrap(d.value,label+'.'+k),writable:true,configurable:true,enumerable:false});wrapped++}}interfaces++};
shadow(document,'document');shadow(document.body,'body');
const _ce=document.createElement;const _el=RA(_ce,document,['div']);shadow(_el,'el');
/*WORKLOAD*/
phase='async';
setTimeout(()=>{let names='',distinct=0;for(const k in seen){distinct++;if(distinct<=40)names+=' '+k+'='+seen[k]}
pocSummary='calls='+calls+' wrapped='+wrapped+' distinct='+distinct+(threw?' THREW'+threw:'')+' ::'+names},120)})()
