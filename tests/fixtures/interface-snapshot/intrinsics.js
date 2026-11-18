(()=>{globalThis.pocSummary='PENDING';
/* Wraps every writable method of the ECMAScript built-ins below, runs a DOM
   workload that itself calls none of them, and reports which wrappers ran:
   each such call came from browser (bootstrap) code. */
const gOPD=Object.getOwnPropertyDescriptor,dP=Object.defineProperty,RA=Reflect.apply,
gPO=Object.getPrototypeOf,oK=Reflect.ownKeys;
const T=[['Array.prototype',Array.prototype],['ArrayIterator',gPO([][Symbol.iterator]())],
['Map.prototype',Map.prototype],['Set.prototype',Set.prototype],['WeakMap.prototype',WeakMap.prototype],
['WeakSet.prototype',WeakSet.prototype],['Function.prototype',Function.prototype],
['Object.prototype',Object.prototype],['String.prototype',String.prototype],
['Promise.prototype',Promise.prototype],['RegExp.prototype',RegExp.prototype],
['Object',Object],['Array',Array],['Reflect',Reflect],['JSON',JSON],['Math',Math],
['Promise',Promise],['String',String],['Number',Number]];
const seen={__proto__:null};let calls=0,wrapped=0,phase='sync';
const byPhase={__proto__:null,sync:0,async:0};
for(let i=0;i<T.length;i++){const label=T[i][0],o=T[i][1],keys=oK(o);
for(let j=0;j<keys.length;j++){const k=keys[j];if(k==='constructor')continue;
const d=gOPD(o,k);if(!d||typeof d.value!=='function'||!d.configurable)continue;
const f=d.value,name=label+'.'+(typeof k==='symbol'?'@@'+k.description:k);
dP(o,k,{value:function(...a){calls++;byPhase[phase]++;seen[name]=(seen[name]|0)+1;
return RA(f,this,a)},writable:true,configurable:true,enumerable:d.enumerable});wrapped++}}
/*WORKLOAD*/
phase='async';
setTimeout(()=>{let names='',distinct=0;for(const k in seen){distinct++;if(distinct<=40)names+=' '+k+'='+seen[k]}
pocSummary='calls='+calls+' wrapped='+wrapped+' distinct='+distinct+(threw?' THREW'+threw:'')+' ::'+names},120)})()
