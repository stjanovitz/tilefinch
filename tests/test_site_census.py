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
import extract  # noqa: E402

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


def run(lab, root, name, env_extra, probe, source=None):
    run_dir = root / name
    if run_dir.exists():
        shutil.rmtree(run_dir)
    (run_dir / "frames").mkdir(parents=True)
    (run_dir / "commands").write_text(
        census.command_script(str(run_dir / "frames"), probe))
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


def main():
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


if __name__ == "__main__":
    main()
