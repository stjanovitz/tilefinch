/* The DOM workload every interface-snapshot page runs. It names no
   ECMAScript built-in and calls no method of one, so any call a page's
   wrapper counts while it runs came from the browser's own script. It
   leaves `threw` set to what it threw, if anything. */
let threw='';
try{
const d=document,b=d.body,el=d.createElement('div');el.id='a';el.className='x y';b.appendChild(el);
const sp=d.createElement('span');el.appendChild(sp);el.insertBefore(d.createTextNode('t'),sp);
el.setAttribute('data-k','v');el.getAttribute('data-k');el.classList.add('z');el.classList.toggle('y');
el.classList.contains('x');el.style.color='red';el.style.setProperty('margin','1px');
el.innerHTML='<p class=q>hi <b>x</b></p><input id=i>';void el.innerHTML;void el.outerHTML;void el.textContent;
d.querySelector('p.q');const all=d.querySelectorAll('p,b');void all.length;void all[0];
void d.getElementsByTagName('b').length;const inp=d.getElementById('i');inp.focus();inp.value='abc';inp.blur();
let hits=0;const h=()=>{hits++};el.addEventListener('click',h);
el.dispatchEvent(new Event('click',{bubbles:true}));el.click();el.removeEventListener('click',h);
el.onclick=h;el.click();el.onclick=null;
const host=d.createElement('div');b.appendChild(host);const sr=host.attachShadow({mode:'open'});
sr.innerHTML='<slot></slot>';const c=el.cloneNode(true);b.append(c,'text');c.remove();
b.replaceChildren(el,host);el.matches('.x');void el.firstChild.closest;el.getBoundingClientRect();
void getComputedStyle(el).color;new MutationObserver(()=>{}).observe(b,{childList:true,subtree:true});
b.appendChild(d.createElement('i'));setTimeout(()=>{},0);requestAnimationFrame(()=>{});queueMicrotask(()=>{});
void new URL('https://a.test/p?q=1#h').searchParams.get('q');const hd=new Headers({a:'b'});hd.get('a');
const fd=new FormData();fd.append('a','b');localStorage.setItem('k','v');localStorage.getItem('k');
history.pushState({a:1},'','#x');new TextEncoder().encode('hi');structuredClone({a:[1,2]});
class X extends HTMLElement{connectedCallback(){}}customElements.define('x-y',X);
b.appendChild(d.createElement('x-y'));d.createElement('canvas').getContext('2d');postMessage('m','*');
const t=d.createElement('template');t.innerHTML='<i></i>';void t.content.firstChild;
new DOMParser().parseFromString('<p>x</p>','text/html');
}catch(e){threw=' threw:'+e}
