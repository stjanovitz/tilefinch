(()=>{globalThis.pocSummary='PENDING';
/* Before the traversal module loads, the page offers its own NodeIterator
   hooks: through dom.js's setter if it can name it, and on the global name
   the module publishes them under. calls counts the browser calling the
   page's hooks, plus 1 if the setter is in reach. The real hooks must
   still arrive: a live iterator follows the removal of its reference. */
let calls=0,r='';
const hooks=new Proxy({},{get(t,k){return function(){calls++;
  return k==='hitTestElements'?[]:false}}});
try{
 const set=globalThis.__tilefinchSetNodeIteratorHooks;
 if(typeof set==='function'){calls++;set(hooks)}
 try{globalThis.__tilefinchNodeIteratorHooks=hooks}catch(e){}
 const body=document.body||document.documentElement,
  box=document.createElement('div'),a=document.createElement('a'),
  b=document.createElement('b');
 box.append(a,b);body.appendChild(box);
 const d=document.createElement('div');body.appendChild(d);d.remove();
 document.elementFromPoint(1,1);
 const it=document.createNodeIterator(box);
 it.nextNode();
 if(it.nextNode()!==a)r+=' THREW first';
 a.remove();
 if(it.nextNode()!==b)r+=' THREW iterator-not-adjusted';
 box.remove();
}catch(e){r+=' THREW '+e}
pocSummary='calls='+calls+r})()
