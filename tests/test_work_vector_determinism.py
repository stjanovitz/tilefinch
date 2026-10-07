"""The tilefinch-work record is deterministic: two runs of the same local lab
journey print identical vectors (tools/work_vector_report.py --check-equal),
and the counters it is built from actually move."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT = ROOT / "tools/work_vector_report.py"
sys.path.insert(0, str(REPORT.parent))
from work_vector_report import parse  # noqa: E402

PAGE = """<!doctype html><html><head><style>
body{font:14px sans-serif;--accent:#c33}
.row{padding:4px;border-bottom:1px solid #ccc}
.row:nth-child(odd){background:#eee}
.hot{color:var(--accent)}
#list .row span{font-weight:bold}
</style></head><body><h1>Work fixture</h1><button id="btn">Go</button>
<div id="list"></div><script>
const list = document.getElementById('list');
let records = 0;
new MutationObserver(r => { records += r.length; })
  .observe(list, {childList: true, subtree: true, attributes: true});
let x = 0.5;
for (let i = 0; i < 20000; i++) x = x * 1.0001 + Math.sqrt(i);
function fill(n) {
  for (let i = 0; i < n; i++) {
    const d = document.createElement('div');
    d.className = 'row';
    d.innerHTML = '<span>' + i + '</span> item ' + (x | 0);
    list.appendChild(d);
  }
}
fill(40);
document.getElementById('btn').addEventListener('click', () => {
  fill(20);
  for (const r of list.children) r.setAttribute('data-n', r.children.length);
  list.firstChild.classList.add('hot');
});
setTimeout(() => fill(5), 30);
</script></body></html>
"""
COMMANDS = ("tick 5 16\nwork loaded\nclick #btn\ntick 3 16\nwork clicked\n"
            "down\nwork scrolled\nprofile profiled\nquit\n")
MOVING = ("js.work_units", "js.polls", "js.allocs", "js.attribute_writes",
          "js.source_bytes", "dom.mutations", "dom.mutation_records",
          "dom.observer_visits", "style.resolutions", "style.rule_queries",
          "style.selector_matches", "style.var_lookups", "layout.passes",
          "layout.commands", "raster.tiles", "raster.commands",
          "raster.frames", "parse.css_bytes")


def run(lab, root, index, source=None, commands="commands.txt"):
    source = source or ["--fixture", str(root / "page.html")]
    output = subprocess.run(
        [str(lab), *source,
         "--commands", str(root / commands), "--no-loop-capture",
         "--output", str(root / f"out-{index}.ppm")],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=60, check=True).stdout
    log = root / f"run-{index}.log"
    log.write_text(output)
    return log


# Rule-index and selector work that a layout style cache miss adds: a miss
# is one full resolution, which queries the rule index and matches the
# rules it returns.
CACHE_DRIVEN = ("style.rule_queries", "style.rule_candidates",
                "style.selector_matches", "style.selector_hits")


def check_equal(first, second):
    # The layout style cache is keyed by node address, so its hit/miss split
    # can move by one when allocation interleaving differs (LAB_USAGE.md);
    # style.resolutions carries the deterministic style work. A miss also
    # can run one more rule query and its selector matches (a retained
    # match list can also serve it), which moved these counters too: in 600
    # runs from fresh temporary directories, two had one or two more misses
    # and as many more rule queries. Everything else must match exactly; the
    # cache-driven counters must match whenever the misses do, and otherwise
    # move only in the misses' direction, rule queries by at most as many.
    subprocess.run([sys.executable, str(REPORT), "--check-equal",
                    "--ignore", "style.cache_", "--ignore", "style.rule_",
                    "--ignore", "style.selector_", str(first), str(second)],
                   check=True)
    a, b = parse(first.read_text()), parse(second.read_text())
    assert [label for label, _ in a] == [label for label, _ in b], (a, b)
    for (label, x), (_, y) in zip(a, b):
        extra = y["style.cache_misses"] - x["style.cache_misses"]
        delta = {field: y[field] - x[field] for field in CACHE_DRIVEN}
        if extra == 0:
            assert not any(delta.values()), (label, delta)
        else:
            assert abs(delta["style.rule_queries"]) <= abs(extra), (
                label, extra, delta)
            assert all(d == 0 or (d > 0) == (extra > 0)
                       for d in delta.values()), (label, extra, delta)


def main():
    lab = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / "page.html").write_text(PAGE)
        (root / "commands.txt").write_text(COMMANDS)
        first, second = run(lab, root, 1), run(lab, root, 2)
        records = parse(first.read_text())
        labels = [label for label, _ in records]
        assert labels == ["loaded", "clicked", "scrolled", "profiled", "final"], labels
        final = records[-1][1]
        for field in MOVING:
            assert isinstance(final[field], int) and final[field] > 0, (field, final)
        # The click's DOM work shows up between the two marks.
        loaded, clicked = records[0][1], records[1][1]
        for field in ("js.work_units", "dom.mutation_records",
                      "js.attribute_writes", "style.resolutions"):
            assert clicked[field] > loaded[field], (field, loaded, clicked)
        # Counters this build does not compile say so instead of reading 0.
        assert all(value == "n/a" or isinstance(value, int)
                   for value in final.values()), final
        check_equal(first, second)
        # A deterministic HTTP replay moves the fetch/parse counters too.
        (root / "replay.txt").write_text("tick 5 16\nwork loaded\nquit\n")
        replay = ["--url", "https://section-scripts.test/page",
                  "--replay-http", str(ROOT / "fixtures/http-section-lazy-scripts"),
                  "--deterministic-replay-seed", "42", "--psp-profile", "strict",
                  "--fetch-scripts"]
        first = run(lab, root, 3, replay, "replay.txt")
        second = run(lab, root, 4, replay, "replay.txt")
        loaded = parse(first.read_text())[0][1]
        for field in ("fetch.load_bytes", "parse.html_bytes",
                      "fetch.replay_served", "layout.passes"):
            assert loaded[field] > 0, (field, loaded)
        check_equal(first, second)


if __name__ == "__main__":
    main()
