(()=>{globalThis.pocSummary='PENDING';
/* A zone.js-style patch of Promise.prototype.then sees a page async
   function settle with a promise whoever calls it: the page, an event
   listener, a timer. calls counts the ones it missed. */
const t=Promise.prototype.then;let n={direct:0,listener:0,timer:0};
const mark=new WeakSet();
Promise.prototype.then=function(...a){if(mark.has(this))n[this.k]++;return t.apply(this,a)};
const mk=(k)=>{const p=Promise.resolve(k);p.k=k;mark.add(p);return p};
(async()=>mk('direct'))();
const el=document.createElement('div');
el.addEventListener('x',async()=>mk('listener'));
el.dispatchEvent(new Event('x'));
setTimeout(async()=>mk('timer'),0);
setTimeout(()=>{const missed=(n.direct!==1)+(n.listener!==1)+(n.timer!==1);
 pocSummary='calls='+missed+' direct='+n.direct+' listener='+n.listener+' timer='+n.timer},60)})()
