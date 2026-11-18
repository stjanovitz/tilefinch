(()=>{globalThis.pocSummary='PENDING';
/* The page wraps a lazy placeholder accessor (outerText) and assigns
   through its wrapper, which calls the placeholder's setter back after
   the module has loaded: that ends at the module's own setter, once, as
   without the snapshot. calls counts the wrapper's calls beyond the
   page's one. */
let calls=0;const P=HTMLElement.prototype;
const d=Object.getOwnPropertyDescriptor(P,'outerText');
Object.defineProperty(P,'outerText',{configurable:true,get(){calls++;return d.get.call(this)},set(v){calls++;d.set.call(this,v)}});
const host=document.createElement('div');const el=document.createElement('span');el.textContent='a';host.appendChild(el);
let r='';try{el.outerText='b';if(host.textContent!=='b')r=' host='+host.textContent}catch(e){r=' THREW '+e}
pocSummary='calls='+(calls-1)+r})()
