"""The buffered host lab must stop at the observation used on the device."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

LAB = Path(sys.argv.pop(1)).resolve()
PAGE = '''<!doctype html><html><body><button id="go">Begin</button>
<p id="reply">WAIT</p><script>
globalThis.steps = 0;
document.getElementById('go').onclick = function () {
  function step() {
    steps++;
    if (steps === 3) document.getElementById('reply').textContent = 'READY';
    else setTimeout(step, 16);
  }
  setTimeout(step, 16);
};
</script></body></html>'''


class InteractiveUntilTest(unittest.TestCase):
    def run_lab(self, commands, options=(), page=PAGE):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'page.html').write_text(page)
            (root / 'commands.txt').write_text(commands)
            return subprocess.run([str(LAB), '--fixture', str(root / 'page.html'),
                '--commands', str(root / 'commands.txt'), '--no-loop-capture',
                '--output', str(root / 'result.ppm'), *options], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)

    def test_stops_on_first_matching_turn(self):
        result = self.run_lab("click #go\nuntil 100 16 document.getElementById('reply').textContent === 'READY'\n"
                              "js JSON.stringify({steps,reply:document.getElementById('reply').textContent})\nquit\n")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=yes iterations=3 cap=100', result.stdout)
        self.assertIn('"steps":3,"reply":"READY"', result.stdout)
        self.assertRegex(result.stdout, r'memory-categories phase=interactive-teardown current=0 expected=0 .*reconcile=yes')

    def test_already_matching_does_not_advance(self):
        result = self.run_lab('until 100 16 true\njs steps\nquit\n')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=yes iterations=0 cap=100', result.stdout)
        self.assertIn('loop-js ok=yes value="0"', result.stdout)

    def test_action_reports_separate_cpu_and_wall_clocks(self):
        result = self.run_lab('focus-next\nquit\n')
        self.assertEqual(result.returncode, 0, result.stdout)
        row = re.search(r'interaction-latency command=focus-next total-us=(\d+) '
                        r'cpu-us=(\d+) cpu-clock-valid=([01])', result.stdout)
        self.assertIsNotNone(row, result.stdout)
        self.assertGreater(int(row[1]), 0)
        if row[3] == '1':
            self.assertGreater(int(row[2]), 0)
        else:
            self.assertEqual(int(row[2]), 0)

    def test_exhaustion_refuses_a_success_report(self):
        result = self.run_lab('until 3 16 false\nquit\n')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=no iterations=3 cap=3', result.stdout)
        self.assertNotIn('loop status=PASS', result.stdout)

    def test_probe_exception_is_not_a_match(self):
        result = self.run_lab('until 3 16 (()=>{throw new Error("probe-failed")})()\nquit\n')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop command-failed="until"', result.stdout)
        self.assertNotIn('matched=yes', result.stdout)

    def test_invalid_or_unbounded_arguments_fail(self):
        for arguments in ('0 16 true', '10001 16 true', '-1 16 true',
                          '3 60001 true', '3 -1 true', '3 16',
                          '3 16x true', '99999999999999999999999999 16 true'):
            with self.subTest(arguments=arguments):
                result = self.run_lab('until ' + arguments + '\nquit\n')
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertNotIn('matched=yes', result.stdout)

    def test_stylesheet_count_is_explicit_and_bounded(self):
        for value in ('1', '32'):
            result = self.run_lab('quit\n', ('--stylesheet-count', value))
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('stylesheets=' + value + ' ', result.stdout)
        for value in ('0', '33', '-1', '+1', '32x', '', '99999999999999999999999999'):
            result = self.run_lab('quit\n', ('--stylesheet-count', value))
            self.assertEqual(result.returncode, 2, result.stdout)
        result = self.run_lab('quit\n', ('--stylesheet-count',))
        self.assertEqual(result.returncode, 2, result.stdout)

    def web_result(self, page, statement, until=None, allow_reported_error=False):
        commands = (f'until 80 16 {until}\n' if until else '')
        commands += f'js JSON.stringify({statement})\nquit\n'
        result = self.run_lab(commands, ('--psp-profile', 'realistic'),
                              '<!doctype html><meta name="viewport" content="width=device-width">'
                              '<style>body{margin:0}</style>' + page)
        self.assertEqual(result.returncode, 0, result.stdout)
        probe = re.search(r'^loop-js ok=yes value="(.*)" error="' +
                          ('' if allow_reported_error else '"$'),
                          result.stdout, re.MULTILINE)
        self.assertIsNotNone(probe, result.stdout)
        self.assertIn('interactive teardown=0', result.stdout)
        return json.loads(probe[1])

    def test_pipe_through_rejects_either_lock_synchronously(self):
        self.assertEqual(self.web_result('', """(()=>{
          const source=new ReadableStream(), transform=new TransformStream();
          const result=[];
          const pipe=()=>{try{source.pipeThrough(transform);return 'returned'}
                          catch(e){return e.name}};
          const reader=source.getReader();
          result.push(pipe(),transform.writable.locked);
          reader.releaseLock();
          const writer=transform.writable.getWriter();
          result.push(pipe(),source.locked);
          writer.releaseLock();
          result.push(source.pipeThrough(transform)===transform.readable);
          return result;
        })()""".replace('\n', ' ')), ['TypeError', False, 'TypeError', False, True])

    def test_event_source_terminal_response_stops_reconnecting(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'trace.meta').write_text(
                'psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\nrecord-count=1\n')
            (root / '0000.body').write_bytes(b'')
            (root / '0000.meta').write_text(
                'psp-http-trace=3\nmethod=GET\nurl=https://events.test/end\nsuccess=1\n'
                'request-body-length=0\nrequest-body-hash=cbf29ce484222325\n'
                'status=204\nlength=0\neffective-url=https://events.test/end\n'
                'content-type=text/event-stream\nresponse-header-count=2\nset-cookie-count=0\n'
                'response-header-0=content-type: text/event-stream\n'
                'response-header-1=access-control-allow-origin: *\n')
            result = self.run_lab(
                'until 80 16 errors > 0\nuntil 240 16 errors > 1 || __tilefinchSchedulerTime() >= 3500\n'
                'js JSON.stringify([source.readyState,errors])\nquit\n',
                ('--replay-http-response-keyed', str(root)),
                '''<!doctype html><script>
                globalThis.errors=0;
                globalThis.source=new EventSource('https://events.test/end');
                source.onerror=()=>errors++;
                </script>''')
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('loop-js ok=yes value="[2,1]" error=""', result.stdout)

    def test_cold_apis_are_built_on_first_touch(self):
        """EventSource, FileReader and WebGPU's interfaces start as lazy
        placeholders; the first touch builds each, which then works."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'trace.meta').write_text(
                'psp-http-trace-clock=1\norigin-ms=1000\ncapture-complete=yes\nrecord-count=1\n')
            body = b'retry: 30000\nid: 7\ndata: first\ndata: second\n\n'
            (root / '0000.body').write_bytes(body)
            (root / '0000.meta').write_text(
                'psp-http-trace=3\nmethod=GET\nurl=https://events.test/feed\nsuccess=1\n'
                'request-body-length=0\nrequest-body-hash=cbf29ce484222325\n'
                f'status=200\nlength={len(body)}\neffective-url=https://events.test/feed\n'
                'content-type=text/event-stream\nresponse-header-count=2\nset-cookie-count=0\n'
                'response-header-0=content-type: text/event-stream\n'
                'response-header-1=access-control-allow-origin: *\n')
            result = self.run_lab(
                'until 120 16 done.length===2\njs JSON.stringify({cold,done})\nquit\n',
                ('--replay-http-response-keyed', str(root)),
                '''<!doctype html><script>
                const names=['EventSource','FileReader','GPU','WGSLLanguageFeatures'];
                globalThis.cold=names.map(n=>typeof Object.getOwnPropertyDescriptor(window,n).get);
                globalThis.done=[];
                const source=new EventSource('https://events.test/feed');
                source.onmessage=e=>{done.push(['message',e.data,e.lastEventId,
                  e instanceof MessageEvent,source.readyState]);source.close()};
                const reader=new FileReader();
                reader.onload=e=>done.push(['load',reader.result,e instanceof ProgressEvent,
                  e.isTrusted,e.loaded,reader.readyState]);
                reader.readAsText(new Blob(['caf\\u00e9']));
                let refused=0;
                for(const C of [GPU,WGSLLanguageFeatures])try{new C()}catch(e){
                  if(e instanceof TypeError)refused++}
                cold.push(refused,String(GPU.prototype[Symbol.toStringTag]),
                  Object.getOwnPropertyDescriptor(GPU.prototype,'requestAdapter').enumerable,
                  WGSLLanguageFeatures.prototype[Symbol.iterator]===
                    WGSLLanguageFeatures.prototype.values,
                  names.map(n=>typeof Object.getOwnPropertyDescriptor(window,n).value).join());
                </script>'''.replace('\n', ' '))
            self.assertEqual(result.returncode, 0, result.stdout)
            probe = re.search(r'^loop-js ok=yes value="(.*)" error=""$', result.stdout,
                              re.MULTILINE)
            self.assertIsNotNone(probe, result.stdout)
            value = json.loads(probe[1])
            self.assertEqual(value['cold'], ['function'] * 4 + [
                2, 'GPU', True, True, 'function,function,function,function'])
            self.assertCountEqual(value['done'], [
                ['message', 'first\nsecond', '7', True, 1],
                ['load', 'café', True, True, 5, 2]])

    def test_resize_observer_uses_laid_out_sizes(self):
        self.assertEqual(self.web_result("""
          <style>#cascade {width:150px !important} #clamp{max-width:100px}</style>
          <div id=cascade style="width:60px;height:20px"></div>
          <div id=clamp style="width:200px;height:20px"></div>
          <div id=border style="width:60px;height:20px;padding:3px;border:2px solid;
                                transform:scale(2)"></div>
          <script>
          window.sizes = {};
          const observer = new ResizeObserver(entries => {
            for(const e of entries) sizes[e.target.id] =
              [e.contentRect.width, e.borderBoxSize[0].inlineSize];
          });
          for(const id of ['cascade','clamp','border']) observer.observe(document.getElementById(id));
          </script>
          """, "sizes", "Object.keys(sizes).length===3"),
                         {"cascade": [150,150], "clamp": [100,100], "border": [60,70]})

    def test_intersection_respects_local_clip_and_root_membership(self):
        self.assertEqual(self.web_result("""
          <div id=clip style="position:relative;overflow:hidden;width:100px;height:40px">
            <div id=partial style="position:absolute;top:30px;width:20px;height:20px"></div>
            <div id=hidden style="position:absolute;top:55px;width:20px;height:20px"></div>
          </div>
          <div id=root style="width:100px;height:30px"></div>
          <div id=outside style="width:20px;height:20px"></div>
          <script>
          window.hits={};window.partialHistory=[];
          const listen=(key,target,options)=>new IntersectionObserver(([e])=>{
            hits[key]=[e.isIntersecting,e.intersectionRatio];
            if(key==='partial')partialHistory.push(e.isIntersecting);
          },options).observe(target);
          for(const id of ['partial','hidden']) listen(id,document.getElementById(id));
          listen('outside',document.getElementById('outside'),
                 {root:document.getElementById('root'),rootMargin:'100px'});
          listen('detached',document.createElement('div'));
          </script>
          """, "{...hits,partialHistory}", "Object.keys(hits).length===4"),
                         {"partial": [True,.5], "hidden": [False,0],
                          "outside": [False,0], "detached": [False,0],
                          "partialHistory": [True]})

    def test_nearest_preserves_view_of_oversized_target(self):
        self.assertEqual(self.web_result("""
          <div id=panel style="overflow:auto;width:120px;height:32px">
            <div id=tall style="height:110px"></div><div style="height:100px"></div>
          </div>
          """, """(()=>{const p=document.getElementById('panel');p.scrollTop=20;
          const before=p.scrollTop;document.getElementById('tall').scrollIntoView({block:'nearest'});
          return [before,p.scrollTop]})()""".replace('\n',' ')), [20,20])

    def test_offsets_are_untransformed(self):
        self.assertEqual(self.web_result("""
          <div style="position:relative;border:3px solid;transform:translateX(4px)">
            <div id=target style="position:relative;left:10px;top:5px;width:35px;height:20px;
                 transform:translate(30px,11px) scale(2);transform-origin:top left"></div>
          </div>
          """, """(()=>{const t=document.getElementById('target');return [
          t.offsetLeft,t.offsetTop,t.offsetWidth,t.offsetHeight,t.getBoundingClientRect().width]})()
          """.strip().replace('\n',' ')), [10,5,35,20,70])

    def test_wrapped_and_zero_width_client_rects(self):
        result = self.web_result("""
          <div style="width:85px"><span id=inline>one two three four five six</span></div>
          <div id=zero style="width:0;height:15px"></div><div id=hidden style="display:none"></div>
          """, """(()=>{const r=id=>document.getElementById(id);
          const b=r('inline').getBoundingClientRect(), fragments=[...r('inline').getClientRects()];
          return {count:fragments.length,width:b.width,height:b.height,
             union:fragments.length>1&&b.top===fragments[0].top
                &&b.bottom===fragments[fragments.length-1].bottom,
             zero:[...r('zero').getClientRects()].map(r=>[r.width,r.height]),
             hidden:r('hidden').getClientRects().length}})()
          """.strip().replace('\n',' '))
        self.assertGreater(result['count'], 1, result)
        self.assertGreater(result['width'], 0, result)
        self.assertTrue(result['union'], result)
        self.assertEqual(result['zero'], [[0,15]])
        self.assertEqual(result['hidden'], 0)

    def test_url_setters_follow_distinct_parser_modes(self):
        self.assertEqual(self.web_result("", """(()=>{const u=new URL('https://example.test:8443/old');
          let relative;try{u.href='/new';relative=false}catch(e){relative=e instanceof TypeError}
          const original=u.href;u.host='other.test';const inherited=u.host;
          u.host='third.test:9443';const changed=u.host;
          u.href='https://fourth.test/a';return [relative,original,inherited,changed,u.href]})()
          """.strip().replace('\n',' ')),
          [True,'https://example.test:8443/old','other.test:8443',
           'third.test:9443','https://fourth.test/a'])

    def test_form_submitter_validation(self):
        result = self.web_result("""
          <form id=f><button id=button type=button></button>
          <button id=disabled type=submit disabled name=skip value=wrong></button>
          <input id=image name=map type=image></form>
          <button form=f id=outside name=action value=outside></button>
          <form><button id=foreign></button></form>
          """, """(()=>{const el=id=>document.getElementById(id), f=el('f'), seen=[];
          f.addEventListener('submit',e=>{e.preventDefault();seen.push(e.submitter.id)});
          const error=fn=>{try{fn();return null}catch(e){return e.name}};
          const ids=['button','foreign','outside','disabled'];
          const requests=ids.map(id=>error(()=>f.requestSubmit(el(id))));
          const data=ids.slice(0,2).map(id=>error(()=>new FormData(f,el(id))));
          return {requests,seen,data,image:[...new FormData(f,el('image'))],
            disabled:[...new FormData(f,el('disabled'))]}})()
          """.strip().replace('\n',' '))
        self.assertEqual(result, {
            'requests': ['TypeError','NotFoundError',None,None],
            'seen': ['outside','disabled'], 'data': ['TypeError','NotFoundError'],
            'image': [['map.x','0'],['map.y','0']], 'disabled': [],
        })

    def test_grid_auto_margins_take_precedence_over_alignment(self):
        self.assertEqual(self.web_result("""
          <style>.grid{display:grid;grid-template:100px / 200px;width:200px;height:100px}
          .tile{width:40px;height:20px}</style>
          <div class=grid><div class=tile style="margin-top:auto;margin-bottom:10px"></div></div>
          <div class=grid><div class=tile style="margin-right:auto;justify-self:end"></div></div>
          <div class=grid><div class=tile style="margin-block:auto;align-self:end"></div></div>
          """, """Array.from(document.querySelectorAll('.grid'),g=>{
          const a=g.getBoundingClientRect(),b=g.firstElementChild.getBoundingClientRect();
          return [b.x-a.x,b.y-a.y]})""".replace('\n',' ')), [[0,70],[0,0],[0,40]])

    def test_tee_errors_both_full_branches(self):
        self.assertEqual(self.web_result("""
          <script>
          (async()=>{
            let controller;const source=new ReadableStream({start(c){controller=c}}),[a,b]=source.tee();
            controller.enqueue('queued');
            for(let i=0;i<12;i++)await Promise.resolve();
            const reason=new Error('upstream');
            controller.error(reason);
            for(let i=0;i<12;i++)await Promise.resolve();
            window.result=await Promise.all([a,b].map(stream=>stream.getReader().read()
                .then(x=>x.value,e=>e===reason?'same error':'wrong error')));
          })();
          </script>
          """, "result", "typeof result!=='undefined'"), ['same error','same error'])

    def test_composed_non_bubbling_event_at_shadow_host(self):
        self.assertEqual(self.web_result('<div id=host></div>', """(()=>{
          const host=document.getElementById('host'), root=host.attachShadow({mode:'open'}),
            button=document.createElement('button'), calls=[];
          root.append(button);
          for(const [label,node] of [['host',host],['root',root],['document',document]])
            for(const capture of [true,false])node.addEventListener('probe',e=>
              calls.push([label,capture,e.eventPhase,e.target===host]),capture);
          button.dispatchEvent(new Event('probe',{composed:true}));
          return calls})()""".replace('\n',' ')), [
            ['document',True,1,True], ['host',True,2,True],
            ['root',True,1,False], ['host',False,2,True],
        ])

    def test_nullable_text_content_and_ascii_class_tokens(self):
        self.assertEqual(self.web_result('', """(()=>{
          const div=document.createElement('div'), comment=document.createComment('x');
          div.textContent='x';div.textContent=null;comment.textContent=undefined;
          div.className='a\u00a0b c';div.classList.add('d\u2003e');
          return [div.textContent,div.childNodes.length,comment.data,
            [...div.classList],div.classList.contains('a\u00a0b'),
            div.classList.contains('d\u2003e')]})()""".replace('\n',' ')),
            ['',0,'',['a\u00a0b','c','d\u2003e'],True,True])

    def test_response_status_and_header_guards(self):
        self.assertEqual(self.web_result("""
          <script>
          (async()=>{
            const error=fn=>{try{fn();return null}catch(e){return e.name}};
            const url=URL.createObjectURL(new Blob(['local']));
            try {
              const fetched=await fetch(url), local=new Response('body');
              local.headers.set('x-test','ok');
              window.result=[
                error(()=>new Response(null,{status:0})),
                error(()=>Response.error().headers.append('x-test','x')),
                error(()=>Response.redirect('https://example.test/').headers.delete('location')),
                error(()=>fetched.headers.set('x-test','x')),
                error(()=>fetched.clone().headers.set('x-test','x')),
                local.clone().headers.get('x-test')];
            } finally { URL.revokeObjectURL(url) }
          })();
          </script>
          """, "result", "typeof result!=='undefined'"),
            ['RangeError','TypeError','TypeError','TypeError','TypeError','ok'])

    def test_worker_response_keeps_internal_status_and_header_guard(self):
        self.assertEqual(self.web_result("""
          <script>
          const url=URL.createObjectURL(new Blob([`
            const response=Response.error(), copy=response.clone();
            const redirect=Response.redirect('https://example.test/');
            const blocked=value=>{try{value.headers.set('x-test','x');return false}
              catch(error){return error.name==='TypeError'}};
            postMessage([response.status,copy.type,response instanceof Response,
              response.headers instanceof Headers,blocked(response),blocked(copy),blocked(redirect)]);
          `],{type:'text/javascript'}));
          const worker=new Worker(url);
          worker.onmessage=event=>{
            window.result=event.data;worker.terminate();URL.revokeObjectURL(url);
          };
          worker.onerror=error=>{window.result=error.message;worker.terminate();URL.revokeObjectURL(url)};
          </script>
          """, "result", "typeof result!=='undefined'"),
            [0,'error',True,True,True,True,True])

    def test_live_range_tracks_child_and_character_data_changes(self):
        self.assertEqual(self.web_result("""
          <div id=children><b>A</b><em>BC</em><u>D</u></div>
          <div id=text>abcdef</div>
          """, """(()=>{
          const root=document.querySelector('#children'), chosen=root.children[1];
          const r=document.createRange(), inner=document.createRange();
          r.selectNode(chosen);inner.selectNodeContents(chosen.firstChild);
          const before=r.toString();root.prepend(document.createElement('hr'));
          const inserted=[r.toString(),r.startOffset,r.endOffset];
          const foreign=r.intersectsNode(document.createElement('aside'));
          chosen.remove();
          const removed=[r.startOffset,r.endOffset,inner.startContainer===root,inner.startOffset];
          const t=document.querySelector('#text').firstChild, chars=new Range();
          chars.setStart(t,2);chars.setEnd(t,5);t.replaceData(0,1,'XY');
          const changed=[chars.startOffset,chars.endOffset,chars.toString()];
          const tail=t.splitText(2);
          return [before,inserted,foreign,removed,changed,
            chars.startContainer===tail,chars.startOffset,chars.endOffset,chars.toString()];
        })()""".replace('\n', ' ')),
            ['BC',['BC',2,3],False,[2,2,True,2],[3,6,'cde'],True,1,4,'cde'])

    def test_range_surround_rejects_partial_elements_before_mutating(self):
        self.assertEqual(self.web_result('<div id=r><b>abc</b><i>def</i></div>',
            """(()=>{const host=document.querySelector('#r'), r=new Range(),
            wrapper=document.createElement('mark');wrapper.textContent='old';
            r.setStart(host.firstChild.firstChild,1);r.setEnd(host.lastChild.firstChild,2);
            let error;try{r.surroundContents(wrapper)}catch(e){error=e.name}
            const untouched=[error,host.innerHTML,wrapper.textContent];
            r.selectNode(host.firstChild);r.surroundContents(wrapper);
            return [untouched,host.innerHTML,r.startContainer===host,r.startOffset,r.endOffset]})()
            """.strip().replace('\n', ' ')),
            [['InvalidStateError','<b>abc</b><i>def</i>','old'],
             '<mark><b>abc</b></mark><i>def</i>',True,0,1])

    def test_web_storage_empty_and_embedded_nul_keys_are_distinct(self):
        self.assertEqual(self.web_result('', """(()=>{
          const nul='key'+String.fromCharCode(0)+'tail';
          return [localStorage,sessionStorage].map(s=>{
            s.clear();s.setItem('key','original');s.setItem(nul,'binary');s.setItem('','empty');
            const keys=Array.from({length:s.length},(_,i)=>s.key(i));
            const found=[s.length,s.getItem('key'),s.getItem(nul),s.getItem(''),
              keys.includes(nul),keys.includes('')];s.removeItem(nul);
            return [found,s.getItem('key'),s.getItem(nul),s.getItem('')];
          });})()""".replace('\n', ' ')),
            [[[3,'original','binary','empty',True,True],'original',None,'empty']]*2)

    def test_url_search_params_optional_filter_and_live_iteration(self):
        self.assertEqual(self.web_result('', """(()=>{
          const u=new URL('https://example.test/?a=1&b=2'), p=u.searchParams, seen=[];
          p.forEach((value,key)=>{seen.push([key,value]);if(key==='a')u.search='?c=3&d=4'});
          const other=new URLSearchParams('foo=one&foo=two&bar=undefined');
          const before=[other.has('foo',undefined),other.has('foo','undefined')];
          other.delete('foo',undefined);
          return [seen,before,other.toString(),u.searchParams===p];})()
          """.strip().replace('\n', ' ')),
            [[['a','1'],['d','4']],[True,False],'bar=undefined',True])

    def test_css_set_property_returns_undefined_and_checks_priority(self):
        self.assertEqual(self.web_result('<div id=r></div>', """(()=>{
          return [document.querySelector('#r'),document.createElement('div')].map(node=>{
            const s=node.style, a=s.setProperty('color','red',null);
            const b=s.color, c=s.setProperty('color','green',' important ');
            const d=s.color, e=s.setProperty('color','blue','IMPORTANT');
            return [a===undefined,b,c===undefined,d,e===undefined,s.color,
              s.getPropertyPriority('color')];
          });})()""".replace('\n', ' ')),
            [[True,'red',True,'red',True,'blue','important']]*2)

    def test_structured_clone_preserves_exception_brand_and_error_cause(self):
        self.assertEqual(self.web_result('', """(()=>{
          const cause={reason:1}, error=new TypeError('original',{cause});
          cause.error=error;
          const a=structuredClone({error,cause}), ex=new DOMException('problem','AbortError');
          const b=structuredClone({ex,again:ex});
          return [a.error instanceof TypeError,a.error.message,a.error.cause===a.cause,
            a.cause.error===a.error,a.cause.reason,Object.keys(a.error).includes('cause'),
            b.ex===b.again,b.ex instanceof DOMException,b.ex.message,b.ex.name,b.ex.code];
        })()""".replace('\n', ' ')),
            [True,'original',True,True,1,False,True,True,'problem','AbortError',20])

    def test_indexeddb_delete_waits_and_queued_open_sees_new_database(self):
        self.assertEqual(self.web_result("""
          <script>
          (async()=>{
            const request=r=>new Promise((ok,no)=>{r.onsuccess=()=>ok(r.result);r.onerror=()=>no(r.error)});
            const first=indexedDB.open('wait',5), log=[];
            first.onupgradeneeded=()=>{
              log.push(['upgrade',first.result.version]);
              first.result.createObjectStore('records');
            };
            const db=await request(first);
            db.onclose=()=>log.push('forced-close');
            db.onversionchange=e=>log.push(['versionchange',e.oldVersion,e.newVersion]);
            const tx=db.transaction('records','readwrite');
            tx.oncomplete=()=>log.push('committed');
            tx.onabort=()=>log.push('aborted');
            const written=request(tx.objectStore('records').put('preserve',1))
              .then(k=>log.push(['write',k]),e=>log.push(e.name));
            const deleting=indexedDB.deleteDatabase('wait');
            const blocked=new Promise(ok=>deleting.onblocked=e=>{
              log.push(['blocked',e.oldVersion,e.newVersion]);ok();
            });
            const deleted=request(deleting).then(()=>log.push('deleted'));
            const queued=indexedDB.open('wait',2);
            queued.onupgradeneeded=e=>log.push(['new',e.oldVersion,queued.result.version]);
            const reopened=request(queued).then(db=>{db.close();return db.version},e=>e.name);
            await Promise.race([blocked,deleted]);db.close();await written;await deleted;
            const version=await reopened;
            window.result=[log[0],log.includes('committed'),log.includes('aborted'),
              log.includes('forced-close'),log.some(x=>x[0]==='write'&&x[1]===1),
              log.find(x=>x[0]==='versionchange'),log.find(x=>x[0]==='blocked'),
              log.find(x=>x[0]==='new'),version];
          })().catch(e=>window.result=e.name+':'+e.message);
          </script>""", "result", "typeof result!=='undefined'"),
            [['upgrade',5],True,False,False,True,
             ['versionchange',5,None],['blocked',5,None],['new',0,2],2])

    def test_indexeddb_primary_key_seek_targets_exact_record_in_both_directions(self):
        self.assertEqual(self.web_result("""
          <script>
          (async()=>{
            const request=r=>new Promise((ok,no)=>{r.onsuccess=()=>ok(r.result);r.onerror=()=>no(r.error)});
            const opening=indexedDB.open('seek',1);
            opening.onupgradeneeded=()=>{
              const store=opening.result.createObjectStore('records');
              store.createIndex('group','group');
              for(let k=1;k<=5;k++)store.put({group:'same'},k);
            };
            const db=await request(opening), visited=[];
            const seek=(direction,target,removeFirst=false)=>new Promise((ok,no)=>{
              const t=db.transaction('records','readwrite');
              t.oncomplete=ok;t.onabort=()=>no(t.error);
              const store=t.objectStore('records');
              const r=store.index('group').openCursor(null,direction);
              let hits=0;
              r.onsuccess=()=>{
                const c=r.result;visited.push(c.primaryKey);
                if (!hits++) {
                  try { c.continuePrimaryKey('same',c.primaryKey); }
                  catch(e) { visited.push(e.name); }
                  if (removeFirst) store.delete(target);
                  c.continuePrimaryKey('same',target);
                } else if (!removeFirst) c.delete();
              };
            });
            await seek('next',3);await seek('prev',2);await seek('next',4,true);
            window.result=[visited,await request(db.transaction('records').objectStore('records').getAllKeys())];
            db.close();
          })().catch(e=>window.result=e.name+':'+e.message);
          </script>""", "result", "typeof result!=='undefined'"),
            [[1,'DataError',3,5,'DataError',2,1,'DataError',5],[1,5]])

    def test_opfs_locked_child_blocks_recursive_removal_before_deleting_siblings(self):
        self.assertEqual(self.web_result("""
          <script>
          (async()=>{
            const root=await navigator.storage.getDirectory();
            const dir=await root.getDirectoryHandle('folder',{create:true});
            const file=await dir.getFileHandle('held',{create:true});
            const sibling=await dir.getFileHandle('neighbor',{create:true});
            const seed=await file.createWritable();await seed.write('old');await seed.close();
            const writer=await file.createWritable({mode:'exclusive'});await writer.write('new');
            const error=await root.removeEntry('folder',{recursive:true}).then(()=>null,e=>e.name);
            const during=await (await file.getFile()).text();
            const hasSibling=(await sibling.getFile()).size===0;
            await writer.close();const after=await (await file.getFile()).text();
            await root.removeEntry('folder',{recursive:true});
            const gone=await root.getDirectoryHandle('folder').then(()=>false,()=>true);
            window.result=[error,during,hasSibling,after,gone];
          })().catch(e=>window.result=e.name+':'+e.message);
          </script>""", "result", "typeof result!=='undefined'"),
            ['NoModificationAllowedError','old',True,'new',True])

    def test_browser_lifecycle_checkpoints_and_window_event_handlers(self):
        self.assertEqual(self.web_result("""
          <body><script>
          const load=[], sequence=[], errors=[];
          addEventListener('load',e=>load.push(['first',e.isTrusted,e.target===document,
            e.currentTarget===window,e.eventPhase,e.composedPath()[0]===window]));
          document.body.onload=()=>load.push('old');
          addEventListener('load',()=>load.push('last'));
          window.onload=()=>load.push('shared');
          const alias=window.onload===document.body.onload;
          addEventListener('error',e=>errors.push(['first',e.defaultPrevented]));
          onerror=(message,source,line,column,error)=>{
            errors.push(['property',message,error instanceof Error]);return true;
          };
          addEventListener('error',e=>errors.push(['last',e.defaultPrevented]));
          document.addEventListener('readystatechange',()=>sequence.push(document.readyState));
          document.addEventListener('DOMContentLoaded',e=>{
            sequence.push('dcl1:'+e.isTrusted);
            Promise.resolve().then(()=>sequence.push('job:'+document.readyState));
          });
          document.addEventListener('DOMContentLoaded',()=>sequence.push('dcl2'));
          addEventListener('load',()=>{sequence.push('load');setTimeout(()=>{throw new Error('synthetic')},0)});
          </script>""", "[alias,sequence,load,errors]", "errors.length===3",
            allow_reported_error=True),
            [True,['interactive','dcl1:true','job:interactive','dcl2','complete','load'],
             [['first',True,True,True,2,True],'shared','last'],
             [['first',False],['property','synthetic',True],['last',True]]])

    def test_redispatch_clears_trust_and_root_passive_defaults(self):
        self.assertEqual(self.web_result('<button id=b>Focus</button>', """(()=>{
          const button=document.querySelector('#b'), trust=[];
          let native;button.addEventListener('focus',e=>{native=e;trust.push(e.isTrusted)});
          button.focus();button.dispatchEvent(native);
          let empty=false;button.addEventListener('',()=>empty=true);
          button.dispatchEvent(new Event(''));
          const cancellation=(target,type,options)=>{
            const listener=e=>e.preventDefault();
            target.addEventListener(type,listener,options);
            const accepted=target.dispatchEvent(new Event(type,{cancelable:true}));
            target.removeEventListener(type,listener,options);return accepted;
          };
          const controller=new AbortController();
          controller.signal.addEventListener=null;
          let remaining=0;button.addEventListener('probe',()=>remaining++,
            {signal:controller.signal});controller.abort();
          button.dispatchEvent(new Event('probe'));
          return [trust,empty,cancellation(document.body,'wheel'),
            cancellation(document,'touchstart'),cancellation(window,'touchmove'),
            cancellation(document.documentElement,'mousewheel'),
            cancellation(document.body,'wheel',{passive:false}),
            cancellation(button,'wheel'),remaining];
        })()""".replace('\n',' ')),
            [[True,False],True,True,True,True,True,False,False,0])

    def test_mutation_observer_namespaces_records_and_reobserve_order(self):
        self.assertEqual(self.web_result("""
          <div id=target></div><script>
          const node=document.querySelector('#target'), log=[];
          const a=new MutationObserver(r=>log.push(['a',r.map(x=>x.attributeName)]));
          const b=new MutationObserver(r=>log.push(['b',r.map(x=>x.attributeName)]));
          const targetOpts={attributes:true};
          a.observe(node,targetOpts);b.observe(node,targetOpts);
          node.setAttribute('first','1');a.disconnect();a.observe(node,targetOpts);
          node.setAttribute('second','2');
          const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');
          const observer=new MutationObserver(()=>{});
          observer.observe(svg,{attributeFilter:['href'],childList:true});
          svg.setAttributeNS('http://www.w3.org/1999/xlink','xlink:href','ignored');
          svg.setAttribute('href','visible');
          const child=document.createElement('b');svg.appendChild(child);child.remove();
          const r=observer.takeRecords(), added=r.find(x=>x.addedNodes.length),
            removed=r.find(x=>x.removedNodes.length);
          const records=[r.length,r[0].attributeNamespace,
            added.addedNodes instanceof NodeList,added.addedNodes.item(0)===child,
            added.addedNodes===added.addedNodes,removed.removedNodes.item(0)===child];
          </script>""", "[log,records]", "log.length===2"),
            [[['a',['second']],['b',['first','second']]],[3,None,True,True,True,True]])

    def test_positioned_margins_rtl_inset_and_border_box_ratio(self):
        self.assertEqual(self.web_result("""
          <style>
          .host{position:relative;width:200px;height:100px}
          .box{background:red}
          #center{position:absolute;inset:0;margin:auto;width:40px;height:20px}
          #rtl{direction:rtl}
          #inset{position:absolute;left:10px;right:15px;top:5px;width:40px;height:20px;direction:ltr}
          #fills{position:absolute;inset:0;margin:10px 20px}
          #fixed{position:fixed;left:300px;top:10px;width:40px;height:15px;margin:8px 5px 4px 12px}
          #ratio{box-sizing:border-box;width:100px;aspect-ratio:2/1;padding:10px}
          </style>
          <div class=host><div id=center class=box></div></div>
          <div id=rtl class=host><div id=inset class=box></div></div>
          <div class=host><div id=fills class=box></div></div>
          <div id=fixed class=box></div><div id=ratio class=box></div>
          """, """(()=>{
          return ['center','inset','fills','fixed','ratio'].map(id=>{
            const n=document.getElementById(id),r=n.getBoundingClientRect();
            const host=n.parentElement.getBoundingClientRect();
            return [r.x-host.x,r.y-host.y,r.width,r.height];
          });})()""".replace('\n',' ')),
            [[80,40,40,20],[145,5,40,20],[20,10,160,80],[312,18,40,15],[0,300,100,50]])

    def test_flex_definite_stretch_and_automatic_column_minimum(self):
        self.assertEqual(self.web_result("""
          <style>
          .row{display:flex}.item{width:80px}
          .size{height:80px;width:10px}
          .half{height:50%;background:red}
          .quarter{height:50%;background:blue}
          .tail{height:7px}
          .column{display:flex;flex-direction:column;height:100px}
          .column>.item{flex:1 1 0}
          .tall{height:140px}.zero>.item{min-height:0}
          .scroll>.item{overflow:auto}
          </style>
          <div class=row><div class=item id=stretch><div class=half id=half><div class=quarter id=quarter></div></div><div class=tail id=tail></div></div><div class=size></div></div>
          <div class=column><div class=item id=minimum><div class=tall></div></div></div>
          <div class="column zero"><div class=item id=zero><div class=tall></div></div></div>
          <div class="column scroll"><div class=item id=scroll><div class=tall></div></div></div>
          """, """(()=>{
          const n=id=>document.getElementById(id).getBoundingClientRect();
          return [n('stretch').height,n('half').height,n('quarter').height,
            n('tail').y-n('stretch').y,n('minimum').height,
            n('zero').height,n('scroll').height];
          })()""".replace('\n',' ')),
            [80,40,20,40,140,100,100])

    def test_vertical_sticky_geometry_uses_nested_scrollport_and_both_edges(self):
        self.assertEqual(self.web_result("""
          <style>
          #port{width:200px;height:100px;overflow:auto;margin-top:20px}
          #start,#end{position:sticky;height:20px;display:block;background:red}
          #start{top:0}#end{bottom:0}
          </style>
          <div id=port><section style="height:500px">
            <div style="height:50px"></div><span id=start></span>
            <div style="height:360px"></div><span id=end></span>
          </section></div>
          """, """(()=>{
          const p=document.getElementById('port'), box=id=>document.getElementById(id).getBoundingClientRect();
          const pos=()=>[p.scrollTop,box('start').top-box('port').top,box('end').top-box('port').top];
          const before=pos();p.scrollTop=100;return [before,pos()];
          })()""".replace('\n',' ')), [[0,50,80],[100,0,80]])


if __name__ == '__main__':
    unittest.main()
