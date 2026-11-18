"""Web form, history and database state transitions through the host browser."""
import json
import re
import unittest

from test_interactive_until import InteractiveUntilTest

# Import only the test helper; don't run the interactive tooling suite twice.
run_lab = InteractiveUntilTest.run_lab
del InteractiveUntilTest


class WebStateTest(unittest.TestCase):
    run_lab = run_lab

    def probe(self, script, body="", until="", commands=""):
        if until:
            commands += f"until 100 16 {until}\n"
        statement = " ".join(script.strip().splitlines())
        result = self.run_lab(
            commands + f"js JSON.stringify({statement})\nquit\n",
            ("--psp-profile", "realistic"),
            "<!doctype html><meta charset=utf-8>" + body,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        output = re.search(r'^loop-js ok=yes value="(.*)" error=""$', result.stdout, re.M)
        self.assertIsNotNone(output, result.stdout)
        self.assertIn("interactive teardown=0", result.stdout)
        return json.loads(output[1])

    def test_history_copies_state_before_atomic_commit(self):
        self.assertEqual(self.probe("""
          (() => { const initial={value:{n:1}, map:new Map([['a',2]])};
            initial.self=initial;
            history.replaceState(initial,''); initial.value.n=3;
            const retained=history.state, before=location.href, length=history.length;
            let error; try { history.pushState(()=>{},'','/not-committed'); }
            catch (e) { error=e.name; }
            return [retained.value.n,retained.map.get('a'),retained.self===retained,
                    error,location.href===before,history.length===length,
                    history.state===retained]; })()
        """), [1, 2, True, "DataCloneError", True, True, True])

    def test_cursor_strict_progression_in_both_directions(self):
        result = self.probe("out", until="out.length === 8", body="""
          <script>globalThis.out=[];
            const r=indexedDB.open('cursor-order-fixture',1);
            r.onupgradeneeded=()=>{const s=r.result.createObjectStore('values');
              for (const key of [1,2,3,4]) s.put('v',key);};
            r.onsuccess=()=>{const db=r.result,t=db.transaction('values'),
                s=t.objectStore('values');
              function check(direction,other,end) {
                const q=s.openCursor(null,direction);
                q.onsuccess=()=>{const c=q.result;
                  if (!c) return;
                  if (c.key===end) {out.push(direction+':'+c.key);return;}
                  for (const key of [c.key,other,{}]) {
                    try {c.continue(key);out.push('accepted');}
                    catch(e) {out.push(e.name);}
                  }
                  try {c.continue(end);} catch(e) {out.push(direction+':'+e.name);}
                };
              }
              check('next',0,3); check('prev',5,2);
              t.oncomplete=()=>db.close();
            };</script>""")
        self.assertEqual(sorted(result), ["DataError"] * 6 + ["next:3", "prev:2"])

    def test_detached_radio_and_shadow_root_are_separate(self):
        self.assertEqual(self.probe("""
          (()=> {const live=document.querySelector('#live'), x=document.createElement('input');
            x.type='radio';x.name='mode';x.click();
            const parent=document.createElement('div');
            const y=document.createElement('input');y.type='radio';y.name='mode';
            parent.append(x,y); y.checked=true;
            const root=document.querySelector('#shadow').attachShadow({mode:'open'});
            root.innerHTML='<input type=radio name=mode>';
            const z=root.querySelector('input');z.checked=true;
            return [live.checked,x.checked,y.checked,z.checked]; })()
        """, """<input id=live type=radio name=mode checked><div id=shadow></div>"""),
            [True, False, True, True])

    def test_explicit_form_owners_partition_radio_group(self):
        self.assertEqual(self.probe("""
          (()=>{const a=document.querySelector('#a'), b=document.querySelector('#b'),
            c=document.querySelector('#c');c.checked=true;
            return [a.checked,b.checked,c.checked,
                    a.defaultChecked,b.defaultChecked,c.defaultChecked];})()
        """, """<form id=one><input id=a name=mode type=radio checked></form>
                  <form id=two><input id=b name=mode type=radio checked></form>
                  <input id=c name=mode type=radio form=one>"""),
            [False, True, True, True, True, False])

    def test_checked_default_stays_in_attribute_and_live_state_in_selector(self):
        self.assertEqual(self.probe("""
          (()=>{
            const x=document.querySelector('#check'), label=document.querySelector('#label');
            x.checked=false; x.removeAttribute('checked');x.setAttribute('checked','');
            const before=[x.checked,x.defaultChecked,x.matches('[checked]'),
                 x.matches(':checked'),getComputedStyle(label).color];
            x.form.reset();
            return [before,x.checked,x.matches(':checked'),getComputedStyle(label).color];
          })()
        """, """<style>#label{color:rgb(1, 2, 3)}
                    #check:checked + #label{color:rgb(4, 5, 6)}</style>
                  <form><input id=check type=checkbox checked><label id=label>Choice</label></form>"""),
            [[False, True, True, False, "rgb(1, 2, 3)"], True, True, "rgb(4, 5, 6)"])

    def test_native_checkbox_click_does_not_publish_an_attribute_mutation(self):
        self.assertEqual(self.probe("""
          (()=>{const x=document.querySelector('#check');
          return [x.checked,x.defaultChecked,x.matches(':checked'),observed];})()
        """, """<input id=check type=checkbox>
                  <script>globalThis.observed=0;
                    new MutationObserver(records=>{observed+=records.length;})
                      .observe(document.querySelector('#check'),{attributes:true,
                        attributeFilter:['checked']});</script>""",
            commands="focus-next\nactivate\n"), [True, False, True, 0])

    def test_cancelled_click_leaves_listener_default_change_but_rolls_back_live_value(self):
        self.assertEqual(self.probe("""
          (()=>{const x=document.querySelector('input');
            x.onclick=event=>{event.preventDefault();x.defaultChecked=true;};
            x.click();
            return [x.checked,x.defaultChecked,x.matches('[checked]'),
                    x.matches(':checked')];})()
        """, '<input type=checkbox>'), [False, True, True, False])

    def test_programmatic_length_is_not_user_validation(self):
        self.assertEqual(self.probe("""
          (()=>{const x=document.querySelector('#text'), y=document.querySelector('#area');
            x.value='xy'; y.value='long';
            return [x.validity.tooShort, x.checkValidity(),
              y.validity.tooLong,y.checkValidity()];})()
        """, '<input id=text minlength=4><textarea id=area maxlength=2></textarea>'),
            [False, True, False, True])

    def test_trusted_edit_enables_length_validation_until_programmatic_write(self):
        self.assertEqual(self.probe("""
          (()=>{const x=document.querySelector('input');
            const afterEdit=[x.value,x.validity.tooShort,x.checkValidity()];
            x.value='y';
            return [afterEdit,[x.value,x.validity.tooShort,x.checkValidity()]];
          })()
        """, '<input minlength=4>', commands="focus-next\ntype x\n"),
            [["x", True, False], ["y", False, True]])

    def test_email_and_input_value_normalization(self):
        self.assertEqual(self.probe("""
          (()=>{const inputs=[...document.querySelectorAll('input')],
              [number,date,url,color,email,multiple]=inputs, initial=inputs.map(x=>x.value);
              number.value='0x10';date.value='2024-02-29';color.value='#A0b1C2';
              return [initial,[number.value,date.value,color.value],
                      email.checkValidity(),multiple.checkValidity()];})()
        """, """<input type=number value=broken><input type=date value=2024-02-30>
                <input type=url value=" https://example.test/ ">
                <input type=color value=no><input type=email value=" me@localhost ">
                <input type=email multiple value=" a@example.test, b@example.test ">"""),
            [["", "", "https://example.test/", "#000000", "me@localhost",
              "a@example.test,b@example.test"], ["", "2024-02-29", "#a0b1c2"], True, True])

    def test_formdata_keeps_files_and_direction_fields(self):
        self.assertEqual(self.probe("""
          (()=>{const f=document.querySelector('form'), input=f.querySelector('#file');
            const row=value=>[value instanceof File,value.name,value.size,value.type];
            const initial=new FormData(f);
            input.files=new FileList([new File(['x'],'sample.txt',{type:'text/plain'})]);
            const data=new FormData(f), files=[row(initial.get('file')),row(data.get('file'))];
            input.value='';
            return [files,data.get('notes.dir'),data.get('words.dir'),data.get('tel.dir'),
                    row(new FormData(f).get('file'))];})()
        """, """<form dir=rtl><input type=file id=file name=file>
          <textarea dir=auto name=notes dirname=notes.dir>123 مرحبا</textarea>
          <input name=words dirname=words.dir value=hello>
          <input type=tel name=tel dirname=tel.dir value=1></form>"""),
            [[[True, "", 0, "application/octet-stream"],
              [True, "sample.txt", 1, "text/plain"]],
             "rtl", "rtl", "ltr", [True, "", 0, "application/octet-stream"]])

    def test_formdata_iterators_and_entry_conversion(self):
        self.assertEqual(self.probe(r"""
          (()=>{const f=new FormData();
            f.append('a','one');f.append('b','two');
            const k=f.keys(),v=f.values(),e=f.entries();
            k.next();v.next();e.next(); f.delete('a'); f.set('c','three');
            const attempt=fn=>{try{fn();return 'accepted';}catch(error){return error.name;}};
            const methods=[attempt(()=>f.append('x','str','x.txt')),
                           attempt(()=>f.set('x','str',undefined))];
            const iterators=[k.next().value,v.next().value,e.next().value];
            f.append('\uD800','\uDC00');
            return [iterators,methods,f.has('x'),f.get('\uD800').charCodeAt(0),
              f.has('\uFFFD')];})()
        """), [["c", "three", ["c", "three"]], ["TypeError", "TypeError"],
                False, 65533, True])


if __name__ == "__main__":
    unittest.main()
