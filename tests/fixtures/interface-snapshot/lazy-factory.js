(()=>{globalThis.pocSummary='PENDING';
/* A page tries to stand in for each lazy module's factory: it creates the
   factory's global name first (so the module's later definition lands on a
   variable the page owns, which the snapshot leaves shared), loads the
   module early through its ensure native if it can reach one, replaces the
   factory, and then makes the browser use the feature. calls counts the
   browser calling the page's function, plus each ensure native the page
   can still name (traversal's too). */
let calls=0,reach=0,r='';
const body=()=>document.body||document.documentElement;
const cases=[
 ['Shadow','__tilefinchCreateShadowState',()=>document.createElement('div').attachShadow({mode:'closed'})],
 ['Documents','__tilefinchCreateSecondaryDocuments',()=>new DOMParser().parseFromString('<p>x</p>','text/html')],
 ['Forms','__tilefinchCreateFormCollections',()=>document.createElement('select').options],
 ['Rule','__tilefinchCreateRuleModel',()=>{const s=new CSSStyleSheet();s.replaceSync('a{color:red}');return s.cssRules}],
 ['FrameClone','__tilefinchCreateFrameClone',()=>window.postMessage({a:1},'*')],
 ['Geometry','__tilefinchCreateLayoutlessGeometry',()=>{const d=document.createElement('div');body().appendChild(d);return d.getBoundingClientRect()}],
 ['Validation','__tilefinchCreateConstraintValidation',()=>document.createElement('input').validity.valid],
 ['DetachedDocuments','__tilefinchCreateDetachedDocument',()=>document.implementation.createDocument(null,'x')],
 ['SubtleCrypto','__tilefinchCreateSubtleCrypto',()=>crypto.subtle.generateKey({name:'AES-GCM',length:128},false,['encrypt'])],
];
for(const [f,N,trigger] of cases){
 try{
  const evil=function(){calls++;return {}};
  globalThis[N]=evil;
  const ensure=globalThis['__tilefinchEnsure'+f+'Bootstrap'];
  if(typeof ensure==='function'){reach++;try{ensure()}catch(e){}
   try{Object.defineProperty(globalThis,N,{configurable:true,writable:true,value:evil})}catch(e){}}
  try{trigger()}catch(e){r+=' THREW '+f+' '+e}
 }catch(e){r+=' THREW '+f+' '+e}
}
if(typeof globalThis.__tilefinchEnsureTraversalBootstrap==='function')reach++;
pocSummary='calls='+(calls+reach)+r})()
