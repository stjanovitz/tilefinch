"""Offline behavior checks for stream, form, DOM, CSS and XHR transitions."""

import pathlib
import re
import subprocess
import sys
import tempfile


HTML = r"""<!doctype html><meta charset="utf-8"><div id="mount"></div>
<script>
const mount = document.getElementById('mount');
function item(markup) {
  mount.innerHTML = markup;
  return mount.firstElementChild;
}
function expectThrow(run, name) {
  try { run(); } catch (error) { return error.name === name; }
  return false;
}
async function failure(p) {
  try { await p; } catch (error) { return error; }
}
async function drainMicrotasks() {
  for (let i = 0; i < 12; i++) await Promise.resolve();
}
const cases = {
  async pipeClosedBeforePull() {
    for (const preventCancel of [false, true]) {
      let pulls = 0, canceled;
      const source = new ReadableStream({
        pull(c) { pulls++; c.enqueue('unused'); c.close(); },
        cancel(reason) { canceled = reason; },
      }, { highWaterMark: 0 });
      const dest = new WritableStream();
      await dest.close();
      const error = await failure(source.pipeTo(dest, { preventCancel }));
      if (error?.name !== 'TypeError' || pulls !== 0 || source.locked || dest.locked ||
          canceled !== (preventCancel ? undefined : error)) return false;
    }
    return true;
  },
  async transformCancelsBothEnds() {
    const stream = new TransformStream(), writer = stream.writable.getWriter();
    const write = failure(writer.write('waiting')), closed = failure(writer.closed);
    await drainMicrotasks();
    await stream.readable.cancel('stop');
    return await write === 'stop' && await closed === 'stop';
  },
  async transformWaitsForStart() {
    let start, finish = false;
    const t = new TransformStream({start() { return new Promise(r => start = r); }});
    const reader = t.readable.getReader(), writer = t.writable.getWriter();
    const read = reader.read(), write = writer.write('start');
    write.then(() => finish = true);
    await drainMicrotasks();
    const pending = !finish;
    start();
    return pending && (await read).value === 'start' && await write === undefined;
  },
  async streamBackpressure() {
    let accept, ready = false;
    const w = new WritableStream({write() {
      return new Promise(r => accept = r);
    }}, {highWaterMark: 2, size: v => v.length}).getWriter();
    const initial = w.desiredSize, write = w.write('ab');
    w.ready.then(() => ready = true);
    await drainMicrotasks();
    const full = w.desiredSize === 0 && !ready;
    accept();
    await write;
    await w.ready;
    const t = new TransformStream(), tw = t.writable.getWriter();
    let done = false;
    const transform = tw.write('next').then(() => done = true);
    await drainMicrotasks();
    const blocked = !done, read = await t.readable.getReader().read();
    await transform;
    return initial === 2 && full && ready && w.desiredSize === 2 &&
      blocked && read.value === 'next';
  },
  xhrDisallowsCredentialsDuringSend() {
    const x = new XMLHttpRequest();
    x.open('GET', 'data:text/plain,local');
    let rejected = false;
    x.addEventListener('loadstart', () => {
      rejected = expectThrow(() => x.withCredentials = true, 'InvalidStateError');
    });
    x.send();
    x.abort();
    return rejected && x.withCredentials === false;
  },
  radioJoinsCheckedGroup() {
    const box = item('<div><input name=a type=radio checked><input name=b type=radio checked></div>');
    const first = box.firstChild, next = box.lastChild;
    next.name = 'a';
    const rename = next.checked && !first.checked && first.defaultChecked;
    const newcomer = document.createElement('input');
    newcomer.type = 'radio'; newcomer.name = 'a'; newcomer.checked = true;
    box.insertBefore(newcomer, next);
    return rename && newcomer.checked && !next.checked && !first.checked;
  },
  inputTypeCarriesLiveValue() {
    const x = item('<input value=old>');
    x.value = 'fresh';
    x.setAttribute('type','checkbox');
    if (x.value !== 'fresh' || x.getAttribute('value') !== 'fresh') return false;
    x.removeAttribute('type');
    if (x.value !== 'fresh') return false;
    x.value = 'another';
    x.type = 'number';
    return x.value === '' && x.getAttribute('value') === 'fresh';
  },
  normalizeKeepsRangeBoundaries() {
    const x = item('<div>A</div>'), a = x.firstChild, b = document.createTextNode('BC');
    x.append(b);
    const inside = document.createRange(), parent = document.createRange();
    inside.setStart(b, 1); inside.setEnd(b, 2);
    parent.setStart(x, 1); parent.setEnd(b, 2);
    x.normalize();
    return x.childNodes.length === 1 && inside.startContainer === a &&
      inside.startOffset === 2 && inside.endOffset === 3 && inside.toString() === 'C' &&
      parent.startContainer === a && parent.startOffset === 1 && parent.toString() === 'BC';
  },
  async writableControllerAbortAndError() {
    let controller, fromWrite;
    const w = new WritableStream({start(c) { controller = c; },
      write(_data,c) { fromWrite = c; }});
    const writer = w.getWriter();
    const signal = controller.signal;
    await writer.write('a');
    let notified = false;
    signal.addEventListener('abort', () => notified = signal.reason === 'abort');
    await writer.abort('abort');
    let next;
    const other = new WritableStream({start(c) { next = c; }}).getWriter();
    const closed = failure(other.closed);
    next.error('error');
    return controller instanceof WritableStreamDefaultController && fromWrite === controller &&
      signal === controller.signal && signal.aborted && notified && await closed === 'error';
  },
  async streamStartThrowsSynchronously() {
    const reason = new Error('start');
    for (const Constructor of [ReadableStream, WritableStream]) {
      let thrown;
      try { new Constructor({start() { throw reason; }}); } catch (error) { thrown = error; }
      if (thrown !== reason) return false;
    }
    const stream = new ReadableStream({start() { return Promise.reject(reason); }});
    return await failure(stream.getReader().read()) === reason;
  },
  xhrRestrictsSynchronousTimeout() {
    const x = new XMLHttpRequest();
    x.open('GET', 'data:text/plain,local', false);
    const setter = expectThrow(() => x.timeout = 1, 'InvalidAccessError');
    const y = new XMLHttpRequest();
    y.timeout = 15;
    const open = expectThrow(() => y.open('GET', 'data:text/plain,local', false),
      'InvalidAccessError');
    y.open('GET', 'data:text/plain,local');
    return setter && x.timeout === 0 && open && y.timeout === 15 && y.readyState === 1;
  },
  formResetHasReentryGuard() {
    const f = item('<form><input value=initial></form>'), x = f.firstChild;
    let events = 0;
    f.addEventListener('reset', event => {
      if (++events === 1) { f.reset(); event.preventDefault(); }
    });
    x.value = 'edited';
    f.reset();
    if (events !== 1 || x.value !== 'edited') return false;
    f.reset();
    return events === 2 && x.value === 'initial';
  },
  validityObjectIsLive() {
    const x = item('<input required>'), v = x.validity;
    const missing = v.valueMissing && !v.valid;
    x.value = 'pass';
    if (x.validity !== v || !v.valid || v.valueMissing) return false;
    x.setCustomValidity('error');
    return missing && v.customError && !v.valid;
  },
  dateNumericRoundTrip() {
    const x = item('<input type=date value=0004-02-29>');
    const yearFour = new Date(x.valueAsNumber).getUTCFullYear() === 4;
    x.value = '2024-01-02';
    const start = x.valueAsNumber;
    x.valueAsNumber = 1704240000000;
    const set = x.value === '2024-01-03';
    x.valueAsNumber = NaN;
    return yearFour && start === 1704153600000 && set && x.value === '';
  },
  detachedCollapseDoesNotReplaceSelection() {
    const x = item('<div>ABC</div>'), r = document.createRange(), sel = getSelection();
    r.selectNodeContents(x); sel.removeAllRanges(); sel.addRange(r);
    sel.collapse(document.createElement('div'),0);
    const kept = sel.rangeCount === 1 && sel.getRangeAt(0) === r && String(sel) === 'ABC';
    sel.removeAllRanges();
    return kept;
  },
  async detailsToggleIsQueuedAndCoalesced() {
    const x = item('<details><summary>Title</summary></details>'), events = [];
    x.addEventListener('toggle', e => events.push([e.oldState,e.newState,e.isTrusted]));
    x.open = true; x.removeAttribute('open');
    if (events.length !== 0) return false;
    await new Promise(r => setTimeout(r, 8));
    if (JSON.stringify(events) !== '[["closed","closed",true]]') return false;
    x.firstElementChild.click();
    if (events.length !== 1 || !x.open) return false;
    await new Promise(r => setTimeout(r, 8));
    return JSON.stringify(events[1]) === '["closed","open",true]';
  },
  invalidWeightKeepsEarlierDeclaration() {
    const x = item('<span>text</span>');
    x.style.fontWeight = '700';
    x.style.setProperty('font-weight', 'wrong');
    if (x.style.fontWeight !== '700' || getComputedStyle(x).fontWeight !== '700') return false;
    x.style.fontWeight = '350';
    return x.style.fontWeight === '350';
  },
  marginReadsAllSides() {
    const x = item('<div></div>'), style = x.style;
    style.cssText = 'margin: 5px; margin-left: 6px';
    if (style.marginLeft !== '6px' || style.marginRight !== '5px' ||
        style.margin !== '5px 5px 5px 6px') return false;
    style.cssText = 'margin: 1px 2px 3px !important; margin-left: 6px';
    return style.marginLeft === '2px' && style.margin === '1px 2px 3px' &&
      style.getPropertyPriority('margin-left') === 'important';
  },
  foreignAttributesRespectCase() {
    const x = document.createElementNS('urn:test', 'node');
    x.setAttribute('MiX','one');
    if (x.getAttribute('mix') !== null || !x.hasAttribute('MiX')) return false;
    x.removeAttribute('mix');
    x.setAttribute('mix','two');
    return x.getAttribute('MiX') === 'one' && x.getAttribute('mix') === 'two';
  },
  nonFiniteCanvasTransformIsIgnored() {
    const c = item('<canvas width=10 height=2></canvas>').getContext('2d');
    c.translate(3,0);
    c.translate(Infinity,0); c.rotate(NaN);
    c.fillRect(0,0,1,1);
    return c.getImageData(3,0,1,1).data[3] === 255 &&
      c.getImageData(0,0,1,1).data[3] === 0;
  },
};
globalThis.receipts = [];
globalThis.finished = false;
void (async () => {
  for (const name of Object.keys(cases)) {
    let timer;
    try {
      const timeout = new Promise(resolve => timer = setTimeout(() => resolve('pending'),
        globalThis.caseBudgets?.[name] ?? 80));
      const result = await Promise.race([Promise.resolve().then(cases[name]), timeout]);
      receipts.push([name, result === true ? 'ok' : String(result)]);
    } catch (error) {
      receipts.push([name, error.name + ': ' + error.message]);
    } finally {
      clearTimeout(timer);
    }
  }
  globalThis.finished = true;
})();
</script>
"""


def main(lab, fixture_name=None):
    html = HTML
    if fixture_name:
        cases = pathlib.Path(__file__).parent / "fixtures" / fixture_name
        prelude = HTML.partition("const cases = {")[0]
        driver = HTML[HTML.index("globalThis.receipts ="):].replace("80));", "250));")
        html = prelude + cases.read_text(encoding="utf-8") + "\n" + driver
    # Case names from the cases object, whatever its indentation; the page
    # reports its own case count below, so a case this scan missed (or a
    # fixture that never ran) fails instead of passing silently.
    body = html[html.index("const cases = {"):html.index("globalThis.receipts =")]
    names = re.findall(r"^[ \t]*(?:async[ \t]+)?(\w+)[ \t]*\(\)[ \t]*\{[ \t]*$",
                       body, re.MULTILINE)
    if not names:
        raise AssertionError("no regression cases found")
    with tempfile.TemporaryDirectory(prefix="tilefinch-web-state-") as tmp:
        work = pathlib.Path(tmp)
        fixture = work / "index.html"
        commands = work / "commands"
        fixture.write_text(html, encoding="utf-8")
        commands.write_text(
            "until 600 16 globalThis.finished === true\n"
            + "js Object.keys(cases).length\n"
            + "".join(f"js receipts[{i}].join(':')\n" for i in range(len(names)))
            + "quit\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [lab, "--fixture", str(fixture), "--commands", str(commands),
             "--no-loop-capture", "--output", str(work / "final.ppm")],
            capture_output=True, text=True, timeout=45,
        )
        if result.returncode:
            print(result.stderr[-8000:], file=sys.stderr)
            result.check_returncode()
    listed = f'loop-js ok=yes value="{len(names)}" error=""' in result.stdout
    missing = [name for name in names
               if f'loop-js ok=yes value="{name}:ok" error=""' not in result.stdout]
    if missing or not listed:
        print("\n".join(line for line in result.stdout.splitlines()
                        if line.startswith(("loop-until", "loop-js "))), file=sys.stderr)
    if not listed:
        raise AssertionError(f"found {len(names)} cases, not the page's count")
    if missing:
        raise AssertionError("Failed: " + ", ".join(missing))


if __name__ == "__main__":
    main(sys.argv[1], "web-correctness-js-regressions.js" if "--correctness-js" in sys.argv[2:]
         else "web-flex-grid-regressions.js" if "--flex-grid" in sys.argv[2:]
         else "web-geometry-storage-regressions.js" if "--geometry-storage" in sys.argv[2:]
         else "web-layout-lifecycle-regressions.js" if "--layout-lifecycle" in sys.argv[2:]
         else "web-storage-dom-regressions.js" if "--extended" in sys.argv[2:] else None)
