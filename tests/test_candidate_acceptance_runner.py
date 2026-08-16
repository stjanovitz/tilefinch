#!/usr/bin/env python3

from __future__ import annotations

import csv
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks" / "run-candidate-interactive-acceptance.sh"


def memory_ledger() -> str:
    categories = (
        "uncategorized",
        "dom",
        "javascript",
        "style",
        "resource",
        "layout",
        "render",
        "session",
        "navigation",
    )
    lines: list[str] = []
    for phase in ("interactive-stable", "interactive-teardown"):
        lines.append(
            f"memory-categories phase={phase} current=0 expected=0 "
            "external-reserved=0 reconcile=yes"
        )
        lines.extend(
            f"memory-category name={name} current=0 active=0 allocs=0 frees=0"
            for name in categories
        )
    return "\n".join(lines)


class CandidateAcceptanceRunnerTest(unittest.TestCase):
    def test_metrics_count_native_async_once_and_include_first_phase(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            replay_root = root / "replay"
            replay = replay_root / "metric-fixture"
            output = root / "output"
            build.mkdir()
            replay.mkdir(parents=True)
            (replay / "trace.meta").write_text("fixture=yes\n")
            (replay / "root.body").write_text("Corpus marker\n")

            fixture_log = root / "fixture.log"
            fixture_log.write_text(
                "\n".join(
                    (
                        "network status=200",
                        "interactive status=ok title=Metric Test height=1000",
                        "document-memory current=0 body-text=100",
                        "scripts discovered=3 loaded=3 failed=0",
                        'javascript-state mutations=1 body="Runtime marker"',
                        "javascript-section-retention retained=1",
                        "javascript-network-async queued=7 completed=5 rejected=0",
                        "javascript-xhr-response responses=2 status=200",
                        (
                            "performance-us network=11 parse=12 script=13 "
                            "style=14 resource=15 layout=16 relayout=17 "
                            "runtime=18 raster=19 frame=20 max-frame=99"
                        ),
                        memory_ledger(),
                        (
                            "interactive teardown=0 active=0 largest=0 "
                            "peak=100 failures=0 status=PASS"
                        ),
                    )
                )
                + "\n"
            )

            lab = build / "psp-browser-interactive-lab"
            lab.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import os
                    import pathlib
                    import sys

                    arguments = iter(sys.argv[1:])
                    output = None
                    for argument in arguments:
                        if argument == "--output":
                            output = pathlib.Path(next(arguments))
                    if output is None:
                        raise SystemExit("missing --output")
                    output.write_bytes(
                        b"P6\\n480 272\\n255\\n" + bytes(480 * 272 * 3)
                    )
                    sys.stdout.write(
                        pathlib.Path(os.environ["CANDIDATE_FIXTURE_LOG"]).read_text()
                    )
                    """
                )
            )
            lab.chmod(0o755)

            manifest = root / "manifest.tsv"
            row = (
                "metric-fixture",
                "https://metric.test/",
                "metric-fixture",
                "1",
                "1",
                "1",
                "1",
                "1000",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "1",
                "0",
                "200",
                "Metric Test",
                "Runtime marker",
                "Corpus marker",
                "required",
                "-",
                "-",
                "metric accounting regression",
                "0",
                "0",
                "0",
                "-",
                "no",
                "-",
            )
            manifest.write_text("\t".join(row) + "\n")

            environment = os.environ.copy()
            environment["CANDIDATE_FIXTURE_LOG"] = str(fixture_log)
            result = subprocess.run(
                (
                    str(RUNNER),
                    str(build),
                    str(replay_root),
                    str(output),
                    str(manifest),
                ),
                capture_output=True,
                text=True,
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with (output / "summary.tsv").open(newline="") as source:
                metrics = next(csv.DictReader(source, delimiter="\t"))
            self.assertEqual(metrics["status"], "PASS")
            self.assertEqual(metrics["network_completions"], "5")
            self.assertEqual(metrics["engine_work_us"], "155")
            self.assertEqual(metrics["max_frame_us"], "99")

            fixture_log.write_text(
                fixture_log.read_text().replace(
                    "javascript-network-async queued=7 completed=5 rejected=0\n",
                    "",
                )
            )
            fallback_output = root / "fallback-output"
            fallback = subprocess.run(
                (
                    str(RUNNER),
                    str(build),
                    str(replay_root),
                    str(fallback_output),
                    str(manifest),
                ),
                capture_output=True,
                text=True,
                env=environment,
            )
            self.assertEqual(fallback.returncode, 0, fallback.stderr)
            with (fallback_output / "summary.tsv").open(newline="") as source:
                fallback_metrics = next(csv.DictReader(source, delimiter="\t"))
            self.assertEqual(fallback_metrics["network_completions"], "2")


if __name__ == "__main__":
    unittest.main()
