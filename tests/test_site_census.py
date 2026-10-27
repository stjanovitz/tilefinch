"""Site-census lab instrumentation (tools/site-census, TILEFINCH_TRACE_CENSUS,
TILEFINCH_LAB_INIT_SCRIPT, the `census` lab command): the ledger records page
script compiles and executions, uncaught exceptions and promise rejections;
the API probe records missing platform properties without changing what the
page sees; nothing is printed when the switches are unset; and the census
tools turn a run into a record."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CENSUS = ROOT / "tools/site-census"
sys.path.insert(0, str(CENSUS))
import census  # noqa: E402
import compare  # noqa: E402
import extract  # noqa: E402
import classify  # noqa: E402

PAGE = """<!doctype html><html><head><title>start</title></head><body>
<h1>Census fixture</h1><p>Some article text for the reader analysis.</p>
<img src="data:image/gif;base64,R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw=="
 width="40" height="30" alt="">
<script>
var seen = [];
seen.push('ResizeObserverCensusX' in window);
seen.push(typeof NoSuchApiCensusX);
seen.push(typeof AnimationEvent, typeof Intl, 'AnimationEvent' in window);
seen.push(document.body.animateCensusX === undefined);
seen.push(document.body instanceof EventTarget);
seen.push(Object.getPrototypeOf(HTMLElement.prototype) === Element.prototype
          || Element.prototype.isPrototypeOf(HTMLElement.prototype));
window.expandoCensus = 1;
seen.push(window.expandoCensus === 1 && 'expandoCensus' in window);
document.title = 'probe:' + seen.join(',');
Promise.reject(new TypeError('census rejection'));
</script>
<script>throw new TypeError('census boom');</script>
<script>document.body.setAttribute('data-after', 'ran');</script>
</body></html>
"""


LINKS = """<!doctype html><html><head><title>links</title></head><body>
<p><a href="#a">first</a> text <a href="#b">second</a></p>
<p><a href="#c">third</a> text <a href="#d">fourth</a></p>
</body></html>
"""


def run(lab, root, name, env_extra, probe, source=None, focus_moves=0,
        focus_hashes="each"):
    run_dir = root / name
    if run_dir.exists():
        shutil.rmtree(run_dir)
    (run_dir / "frames").mkdir(parents=True)
    (run_dir / "commands").write_text(
        census.command_script(str(run_dir / "frames"), probe,
                              focus_moves=focus_moves,
                              focus_hashes=focus_hashes))
    env = {k: v for k, v in os.environ.items()
           if k not in ("TILEFINCH_TRACE_CENSUS", "TILEFINCH_LAB_INIT_SCRIPT")}
    env.update(env_extra)
    source = source or ["--fixture", str(root / "page.html")]
    with open(run_dir / "lab.log", "w") as out, open(run_dir / "lab.err", "w") as err:
        subprocess.run([str(lab), *source, "--commands", str(run_dir / "commands"),
                        "--no-loop-capture", "--output", str(run_dir / "out.ppm")],
                       stdout=out, stderr=err, env=env, timeout=90, check=True,
                       cwd=run_dir)
    (run_dir / "meta.json").write_text(json.dumps(
        {"name": name, "url": "https://fixture.test/", "tags": ["test"],
         "mode": "test", "exit": 0, "wall_s": 0}))
    return run_dir


def without_lines(run_dir, target, keep):
    """A copy of a run whose lab.log lacks the lines `keep` rejects."""
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(run_dir, target)
    lines = (run_dir / "lab.log").read_text().splitlines(True)
    (target / "lab.log").write_text("".join(l for l in lines if keep(l)))
    return target


def check_focus_moves(lab, root):
    """--focus-moves: the lab's two frame hashes agree on one frame, every
    focus field survives moves that present nothing, and compare.py keys
    the hashes by label and says when one side has none."""
    (root / "links.html").write_text(LINKS)
    source = ["--fixture", str(root / "links.html")]
    focus = run(lab, root, "focus", {}, False, source, focus_moves=3)
    record = extract.extract(str(focus))
    assert record["focus_issued"] == 3 and record["focus_hashes"] == "each", record
    assert record["timing"]["focus_moves"] == 3, record["timing"]
    moves, after = record["focus_move_hashes"], record["focus_after_hashes"]
    assert sorted(after) == ["focus-1", "focus-2", "focus-3", "focus-final"], after
    assert sorted(moves) == ["focus-1", "focus-2", "focus-3"], moves
    # One FNV-1a over the frame at both lab sites: the presented move and
    # the frame-hash render of the same state hash alike.
    for label, value in moves.items():
        assert value and value == after[label], (label, moves, after)
    assert len(set(moves.values())) == 3, moves
    assert compare.focus_changes(record, record) == []

    # Moves that present nothing (a no-op focus-next) print no
    # interaction-latency lines; the record still carries the moves issued
    # and the after-move frames, and the comparison says what is missing.
    noop = extract.extract(str(without_lines(
        focus, root / "focus-noop",
        lambda l: not l.startswith("interaction-latency command=focus-next "))))
    assert noop["focus_issued"] == 3, noop
    assert noop["timing"]["focus_moves"] == 0, noop["timing"]
    assert noop["focus_move_hashes"] == {}, noop
    assert noop["focus_after_hashes"] == after, noop
    notes = compare.focus_changes(record, noop)
    assert "focus moves presented 3 -> 0" in notes, notes
    assert any("MOVE HASHES ON ONE SIDE ONLY at 1(base),2(base),3(base)" in n
               for n in notes), notes

    # One move missing shifts nothing: only that label is reported.
    first = [True]

    def drop_first(line):
        if first[0] and line.startswith("interaction-latency command=focus-next "):
            first[0] = False
            return False
        return True
    shifted = extract.extract(str(without_lines(focus, root / "focus-shift", drop_first)))
    notes = compare.focus_changes(record, shifted)
    assert [n for n in notes if "differ" in n] == [], notes
    assert any(n.endswith("ONE SIDE ONLY at 1(base)") for n in notes), notes

    # A run that left no focus data at all is reported, not skipped.
    bare = dict(record)
    for key in ("focus_issued", "focus_hashes", "focus_move_hashes",
                "focus_after_hashes"):
        bare.pop(key)
    bare["timing"] = {k: v for k, v in record["timing"].items()
                      if not k.startswith("focus_")}
    assert compare.focus_changes(record, bare) == ["FOCUS DATA ON BASE SIDE ONLY"]

    # Records from before the per-label keys still compare: their
    # loop-frame-hash half against the after-move frames.
    legacy = dict(bare)
    legacy["timing"] = dict(record["timing"])
    legacy["focus_frames"] = ["0" * 16] * 3 + [after[k] for k in
        ("focus-1", "focus-2", "focus-3", "focus-final")]
    assert compare.focus_changes(legacy, record) == []
    legacy["focus_frames"][4] = "1" * 16
    assert compare.focus_changes(legacy, record) == [
        "focus after frames differ at 2"], compare.focus_changes(legacy, record)

    # --focus-hashes final: no per-move renders, so no move hashes, and a
    # comparison with an `each` run warns that the timings differ in kind.
    final = extract.extract(str(run(lab, root, "focus-final", {}, False, source,
                                    focus_moves=3, focus_hashes="final")))
    assert final["focus_hashes"] == "final" and final["focus_move_hashes"] is None
    assert sorted(final["focus_after_hashes"]) == ["focus-final"], final
    assert final["focus_after_hashes"]["focus-final"] == after["focus-final"]
    assert any("not like-for-like" in line for line in compare.focus_modes(
        {"p": record}, {"p": final}, ["p"]))


def main():
    assert classify.classify('https://aax-us-east-retail-direct.amazon.com/e/is/example/impb',
                             'https://www.amazon.com/') == 'ads'
    assert classify.classify('https://aax-us-east-retail-direct.amazon.com.example.test/app.js',
                             'https://www.amazon.com/') == 'third-party'
    for host in ('fls-na.amazon.com', 'fls-eu.amazon.com', 'fls-fe.amazon.com',
                 'unagi.amazon.com'):
        assert classify.classify('https://' + host + '/1/batch/events',
                                 'https://www.amazon.com/') == 'analytics'
    assert classify.classify('https://www.amazon.com/robots.txt?host=fls-na.amazon.com',
                             'https://www.amazon.com/') == 'first-party'
    assert classify.classify('https://fls-na.amazon.com.example.test/app.js',
                             'https://www.amazon.com/') == 'third-party'
    assert classify.classify('https://content.test/packs/audio-abcgga.js',
                             'https://content.test/') == 'first-party'
    assert classify.classify('https://cdn.example/ga.js?version=1',
                             'https://content.test/') == 'analytics'
    assert classify.classify('https://player.ay.delivery/client.js',
                             'https://content.test/') == 'ads'
    lab = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / "page.html").write_text(PAGE)

        # Switches unset: no ledger records, and the page sees no probe.
        plain = run(lab, root, "plain", {}, False)
        log = (plain / "lab.log").read_text()
        for record in ("census-js ", "census-exception ", "census-rejection ",
                       "census-init-script "):
            assert record not in log, record
        # The explicit `census` command still reports.
        for record in ("census-timebase ", "census-layout ", "census-image ",
                       "census-reader "):
            assert record in log, record
        plain_record = extract.extract(str(plain))
        assert plain_record["title"].startswith("probe:"), plain_record["title"]

        traced = run(lab, root, "traced", {
            "TILEFINCH_TRACE_CENSUS": "1",
            "TILEFINCH_LAB_INIT_SCRIPT": str(CENSUS / "api-probe.js")}, True)
        record = extract.extract(str(traced))
        # The probe is transparent: the page computed the same answers.
        assert record["title"] == plain_record["title"], (
            record["title"], plain_record["title"])
        assert record["init_script"] == "ok", record.get("init_script")
        inline = [s for s in record["script_list"] if s["kind"] == "inline"]
        assert len(inline) >= 3, record["script_list"]
        assert all(s["compiles"] == 1 and s["executes"] == 1 for s in inline), inline
        assert sum(1 for s in inline if not s["ok"]) == 1, inline
        messages = [e["message"] for e in record["errors"]["exceptions"]]
        assert any("census boom" in m for m in messages), messages
        rejections = [e["message"] for e in record["errors"]["rejections"]]
        assert any("census rejection" in m for m in rejections), rejections
        if "misses" not in record.get("api_probe", {}):
            log = (traced / "lab.log").read_text().splitlines()
            print("\n".join(l[:400] for l in log
                            if "probe-chunk" in l or "census-init" in l)[:6000])
            print((traced / "lab.err").read_text()[:2000])
        misses = record["api_probe"]["misses"]
        for name in ("window.ResizeObserverCensusX", "window.NoSuchApiCensusX",
                     "HTMLElement.animateCensusX"):
            assert name in misses, (name, sorted(misses)[:40],
                                    record["api_probe"].get("skippedSample"))
        # [count, mask]: mask 1 = [[Get]], 2 = [[Has]].
        assert misses["window.ResizeObserverCensusX"][1] & 2, misses
        assert "window.expandoCensus" not in misses, misses
        # Present (possibly lazily installed) globals are never misses.
        for name in ("window.AnimationEvent", "window.Intl"):
            assert name not in misses, (name, misses)
        assert record["layout"]["commands"] > 0, record["layout"]
        assert record["images"]["count"] >= 1, record["images"]
        assert record["fallback"]["reader_kind"], record["fallback"]

        # External scripts from a deterministic HTTP replay are ledgered too.
        replay = run(lab, root, "replay", {"TILEFINCH_TRACE_CENSUS": "1"}, False, [
            "--url", "https://script-preload-time.test/document",
            "--replay-http", str(ROOT / "fixtures/http-parser-script-preload-time"),
            "--deterministic-replay-seed", "42", "--psp-profile", "strict",
            "--fetch-scripts"])
        record = extract.extract(str(replay))
        external = [s for s in record["script_list"] if s["kind"] == "external"]
        assert external and all(s["compiles"] >= 1 for s in external), \
            record["script_list"]
        assert all(s["first_t_ms"] is not None for s in record["script_list"])
        # URL loads stream, so they carry the load timeline.
        assert record["timing"]["loaded_ms"] is not None, record["timing"]

        check_focus_moves(lab, root)


if __name__ == "__main__":
    main()
