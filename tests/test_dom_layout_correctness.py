#!/usr/bin/env python3
"""Small browser-visible tests for DOM selection, reflection and CSSOM."""

import pathlib
import re
import subprocess
import sys
import tempfile


HTML = r"""<!doctype html><meta charset="utf-8">
<div id="mount"></div>
<div style="width:100px"><span id="contents" style="display:contents">
  <b id="contents-child">Visible</b>
</span></div>
<div id="contents-block" style="display:contents"><p>Block inside</p></div>
<div id="contents-host"></div>
<script>
const mount = document.getElementById('mount');
function item(markup) {
  mount.innerHTML = markup;
  return mount.firstElementChild;
}
function throwsName(run, name) {
  try { run(); } catch (error) { return error.name === name; }
  return false;
}
const cases = {
  rangeInsert() {
    const box = item('<div>ab</div>'), head = box.firstChild;
    const range = document.createRange(), fragment = document.createDocumentFragment();
    const first = document.createElement('b'), second = document.createElement('i');
    fragment.append(first, second);
    range.setStart(head, 1);
    range.collapse(true);
    range.insertNode(fragment);
    return box.childNodes.length === 4 && box.childNodes[1] === first &&
      box.childNodes[2] === second && box.lastChild.data === 'b' &&
      range.startContainer === head && range.startOffset === 1 &&
      !range.collapsed && range.endContainer === box && range.endOffset === 3;
  },
  selectionContainment() {
    const box = item('<div><b>abc</b><i>def</i></div>');
    const range = document.createRange(), selection = getSelection();
    range.setStart(box.firstChild.firstChild, 1);
    range.setEnd(box.firstChild.firstChild, 3);
    selection.removeAllRanges();
    selection.addRange(range);
    const partial = selection.containsNode(box.firstChild, true) &&
      !selection.containsNode(box.firstChild);
    range.selectNode(box.firstChild);
    const outside = selection.containsNode(box.firstChild, true) &&
      !selection.containsNode(box.lastChild, true);
    selection.removeAllRanges();
    return partial && outside;
  },
  selectionSecondRange() {
    const box = item('<div><b>A</b><i>B</i></div>');
    const range = document.createRange(), other = document.createRange(),
      selection = getSelection();
    range.selectNode(box.firstChild);
    other.selectNode(box.lastChild);
    selection.removeAllRanges();
    selection.addRange(range);
    selection.addRange(other);
    const kept = selection.rangeCount === 1 && selection.getRangeAt(0) === range &&
      String(selection) === 'A';
    selection.removeAllRanges();
    return kept;
  },
  selectionDirection() {
    const text = item('<p>abcd</p>').firstChild, selection = getSelection();
    selection.setBaseAndExtent(text, 3, text, 1);
    const backward = selection.anchorNode === text && selection.anchorOffset === 3 &&
      selection.focusNode === text && selection.focusOffset === 1 &&
      String(selection) === 'bc';
    const rejected = throwsName(() => selection.setBaseAndExtent(text, 3, text, 9),
      'IndexSizeError') && String(selection) === 'bc';
    selection.extend(text, 4);
    const forward = selection.anchorOffset === 3 && selection.focusOffset === 4 &&
      String(selection) === 'd';
    selection.removeAllRanges();
    return backward && rejected && forward;
  },
  inputSelection() {
    const input = item('<input type="number" value="11">');
    const rejected = input.selectionStart === null && input.selectionEnd === null &&
      input.selectionDirection === null &&
      throwsName(() => input.setSelectionRange(0, 2), 'InvalidStateError') &&
      throwsName(() => { input.selectionStart = 1; }, 'InvalidStateError');
    input.type = 'text';
    input.setSelectionRange(1, 2, 'backward');
    return rejected && input.selectionStart === 1 && input.selectionEnd === 2 &&
      input.selectionDirection === 'backward';
  },
  detailsGroup() {
    const box = item('<div><details open name="g"></details>' +
      '<details name="g"></details><details open name="G"></details></div>');
    const first = box.children[0], second = box.children[1], distinct = box.children[2];
    second.open = true;
    const opening = !first.open && second.open && distinct.open;
    first.setAttribute('open', '');
    const attribute = first.open && !second.open && distinct.open;
    const detached = document.createElement('details');
    detached.setAttribute('name', 'g');
    detached.open = true;
    const separate = detached.open && first.open;
    box.insertBefore(detached, first);
    const inserted = !detached.open && first.open;
    distinct.setAttribute('name', 'g');
    return opening && attribute && separate && inserted && first.open && !distinct.open;
  },
  cssInvalidSize() {
    const style = item('<div style="width:12px"></div>').style;
    style.setProperty('width', 'garbage');
    const kept = style.width === '12px';
    style.setProperty('width', 'var(--custom-width, 2px)');
    const variable = style.width === 'var(--custom-width, 2px)';
    style.setProperty('width', '2cqw');
    const unit = style.width === '2cqw';
    style.width = 'fit-content(10px)';
    style.setProperty('--width', 'garbage');
    return kept && variable && unit && style.width === 'fit-content(10px)' &&
      style.getPropertyValue('--width') === 'garbage';
  },
  cssBorderWidths() {
    const style = item('<div style="border:4px solid red"></div>').style;
    style.borderLeftWidth = '7px';
    const spread = style.borderTopWidth === '4px' && style.borderLeftWidth === '7px';
    const authored = item('<div style="border-width:1px 2px 3px !important;' +
      'border-top-width:11px"></div>').style;
    return spread && authored.borderTopWidth === '1px' &&
      authored.borderRightWidth === '2px' && authored.borderBottomWidth === '3px' &&
      authored.borderLeftWidth === '2px' &&
      authored.getPropertyPriority('border-top-width') === 'important';
  },
  contentsGeometry() {
    const boxless = (node) => {
      const box = node.getBoundingClientRect();
      const child = node.firstElementChild.getBoundingClientRect();
      return box.x === 0 && box.y === 0 && box.width === 0 && box.height === 0 &&
        node.getClientRects().length === 0 && child.width > 0 && child.height > 0;
    };
    return boxless(document.getElementById('contents')) &&
      boxless(document.getElementById('contents-block'));
  },
  shadowContentsGeometry() {
    const host = document.getElementById('contents-host');
    const root = host.attachShadow({mode:'open'});
    root.innerHTML = '<div style="display:contents"><b>Shadow child</b></div>';
    const node = root.firstElementChild, box = node.getBoundingClientRect();
    const child = node.firstElementChild.getBoundingClientRect();
    return box.x === 0 && box.y === 0 && box.width === 0 && box.height === 0 &&
      node.getClientRects().length === 0 && child.width > 0 && child.height > 0;
  },
  innerHTMLNull() {
    const node = item('<div><b>old</b></div>');
    node.innerHTML = null;
    return node.innerHTML === '' && node.childNodes.length === 0;
  },
  optionText() {
    const option = item('<select><option>  one    two </option></select>').options[0];
    const script = document.createElement('script');
    script.type = 'text/plain';
    script.textContent = 'excluded';
    option.appendChild(script);
    const before = option.text === 'one two';
    option.text = 'three   four';
    return before && option.text === 'three four' && option.textContent === 'three   four';
  },
  textWhole() {
    const el = item('<div>A</div>'), second = document.createTextNode('B');
    el.append(second, document.createElement('i'), 'C');
    return second.wholeText === 'AB' && el.firstChild.wholeText === 'AB' &&
      el.lastChild.wholeText === 'C';
  },
  relList() {
    const link = item('<link rel="stylesheet">'), list = link.relList;
    link.rel = 'preload';
    return list === link.relList && list.contains('preload') && !list.contains('stylesheet');
  },
  readOnlyRect() {
    const rect = new DOMRectReadOnly(1, 2, 3, 4);
    try { rect.x = 9; } catch (_) {}
    const mutable = new DOMRect(1, 2, 3, 4);
    mutable.x = 7;
    mutable.width = -2;
    return rect.x === 1 && rect.right === 4 && mutable.left === 5 && mutable.right === 7;
  },
};
function run(name) {
  try { return name + ':' + (cases[name]() ? 'ok' : 'wrong'); }
  catch (error) { return name + ':' + error.name + ':' + error.message; }
}
</script>
"""


def main(lab):
    names = re.findall(r"^  (\w+)\(\) \{$", HTML, re.MULTILINE)
    if len(names) != 15:
        raise AssertionError("test case discovery changed")
    with tempfile.TemporaryDirectory(prefix="tilefinch-dom-layout-") as tmp:
        work = pathlib.Path(tmp)
        fixture = work / "index.html"
        commands = work / "commands"
        fixture.write_text(HTML, encoding="utf-8")
        commands.write_text(
            "".join(f"js run('{name}')\n" for name in names) + "quit\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [lab, "--fixture", str(fixture), "--commands", str(commands),
             "--no-loop-capture", "--output", str(work / "final.ppm")],
            capture_output=True, text=True, check=True, timeout=40,
        )
    failures = [
        name for name in names
        if f'loop-js ok=yes value="{name}:ok" error=""' not in result.stdout
    ]
    if failures:
        print("\n".join(line for line in result.stdout.splitlines()
                        if line.startswith("loop-js ")), file=sys.stderr)
        raise AssertionError("Failed: " + ", ".join(failures))


if __name__ == "__main__":
    main(sys.argv[1])
