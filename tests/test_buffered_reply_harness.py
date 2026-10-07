"""Fast reply timing must include activation and refuse incomplete evidence."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from contextlib import redirect_stderr

spec = importlib.util.spec_from_file_location("reply", Path(__file__).resolve().parents[1]
                                            / "scripts/run-buffered-reply-replay.py")
reply = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reply)

LOG = '''tilefinch-work: label=before js.module_compiles=117
interaction-latency command=click total-us=100 dispatch-us=80 runtime-us=50
interaction-latency command=activate total-us=200 dispatch-us=150 relayout-us=20
tilefinch-work: label=sent js.module_compiles=120
interaction-latency command=tick total-us=400 runtime-us=300 relayout-us=200
tilefinch-work: label=answer js.module_compiles=159
interaction-latency command=js total-us=900
'''


class ReplyHarnessTest(unittest.TestCase):
    def test_reference_pixels_are_exact_and_region_is_bounded(self):
        pixels = bytes(range(18))
        raw = b"P6\n3 2\n255\n" + pixels
        result = reply.ppm_region_evidence(raw, (1, 0, 2, 2))
        self.assertEqual(result["dimensions"], [3, 2])
        self.assertEqual(result["region"], [1, 0, 2, 2])
        self.assertEqual(result["region_sha256"], reply.hashlib.sha256(pixels[3:9]+pixels[12:18]).hexdigest())
        for region in ((-1, 0, 1, 1), (0, 0, 0, 1), (2, 0, 2, 1), (0, 2, 1, 1)):
            with self.assertRaises(ValueError):
                reply.ppm_region_evidence(raw, region)
        for broken in (raw[:-1], raw+b"x", raw.replace(b"P6", b"P5"),
                       b"P6\n481 272\n255\n"):
            with self.assertRaises(ValueError):
                reply.ppm_region_evidence(broken, (0, 0, 1, 1))

    def test_page_settings_are_explicit_and_css_is_identified(self):
        with tempfile.TemporaryDirectory() as directory:
            css = Path(directory) / "presentation.css"
            css.write_bytes(b"html{font-size:125%!important}")
            options, evidence = reply.page_replay_options("basic", css)
            self.assertEqual(options, ["--content-blocker", "basic", "--user-css", str(css.resolve())])
            self.assertEqual(evidence["content_blocker"], "basic")
            self.assertEqual(evidence["user_css_sha256"], reply.hashlib.sha256(css.read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                reply.page_replay_options("basic", css.with_name("missing.css"))
            css.write_bytes(b"x" * 65537)
            with self.assertRaises(ValueError):
                reply.page_replay_options("basic", css)
        options, evidence = reply.page_replay_options("off", None)
        self.assertEqual(options, ["--content-blocker", "off"])
        self.assertIsNone(evidence["user_css_sha256"])
        with self.assertRaises(ValueError):
            reply.page_replay_options("unknown", None)

    def test_reference_contract_matches_only_named_integer_work(self):
        reference = [("device-answer", {"dom.mutations": 742, "layout.passes": 40})]
        current = [("answer", {"dom.mutations": 742, "layout.passes": 29})]
        result = reply.reference_work_check(reference, current, "device-answer", "answer",
                                            ["dom.mutations"])
        self.assertEqual(result["matched"], {"dom.mutations": 742})
        self.assertFalse(result["all_endpoint_work_equal"])
        self.assertEqual(len(result["other_differences"]), 1)

    def test_reference_contract_refuses_missing_ambiguous_or_unavailable_work(self):
        good = [("answer", {"dom.mutations": 742})]
        for bad in ([], [("answer", {})], [("answer", {"dom.mutations": "n/a"})],
                    [("answer", {"dom.mutations": 741})],
                    good + [("answer#2", {"dom.mutations": 742})]):
            with self.assertRaises(ValueError):
                reply.reference_work_check(good, bad, "answer", "answer", ["dom.mutations"])
            with self.assertRaises(ValueError):
                reply.reference_work_check(bad, good, "answer", "answer", ["dom.mutations"])
        with self.assertRaises(ValueError):
            reply.reference_work_check(good, good, "answer", "answer", [])

    def test_incomplete_reference_refused_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "device.log"
            reference.write_text("tilefinch-work: label=answer dom.mutations=n/a\n")
            output = root / "output"
            argv = ["reply", "--responses", "unused", "--commands", "unused",
                    "--url", "https://fixture.example/", "--expect-text", "Hello",
                    "--output-dir", str(output), "--reference-log", str(reference),
                    "--reference-label", "answer", "--match-work", "dom.mutations"]
            with patch.object(reply.sys, "argv", argv), patch.object(reply.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    reply.main()
                run.assert_not_called()
            self.assertFalse(output.exists())

    def test_reference_flags_cannot_silently_disable_the_contract(self):
        base = ["reply", "--responses", "unused", "--commands", "unused",
                "--url", "https://fixture.example/", "--expect-text", "Hello",
                "--output-dir", "unused"]
        for extra in (["--reference-log", "unused"], ["--match-work", "dom.mutations"],
                      ["--reference-label", "answer"], ["--reference-frame", "unused"],
                      ["--match-region", "0", "0", "1", "1"]):
            with patch.object(reply.sys, "argv", base + extra), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    reply.main()
                self.assertEqual(raised.exception.code, 2)

    def test_child_cpu_reports_user_and_system_time_or_unavailable(self):
        fake = SimpleNamespace(RUSAGE_CHILDREN=123,
            getrusage=lambda who: SimpleNamespace(ru_utime=1.25, ru_stime=.125)
                if who == 123 else None)
        with patch.object(reply, "resource", fake):
            self.assertEqual(reply.child_cpu_us(), 1375000)
        with patch.object(reply, "resource", None):
            self.assertIsNone(reply.child_cpu_us())

    def test_native_timing_observer_is_off_unless_explicit(self):
        source = {"PATH": "fixture-path"}
        environment = reply.replay_environment(source)
        self.assertEqual(environment["TILEFINCH_SCRIPT_SPLIT"], "0")
        self.assertEqual(environment["PATH"], "fixture-path")
        self.assertEqual(source, {"PATH": "fixture-path"})
        environment = reply.replay_environment({"TILEFINCH_SCRIPT_SPLIT": "1"})
        self.assertEqual(environment["TILEFINCH_SCRIPT_SPLIT"], "1")

    def test_activation_is_included_nested_phases_are_not_added(self):
        result = reply.window_metrics(LOG, ("before", "answer"))
        self.assertEqual(result["command_wall_us"], 700)
        self.assertEqual([r["command"] for r in result["commands"]], ["click", "activate", "tick"])

    def test_missing_duplicate_reversed_and_empty_markers_refused(self):
        for text, labels in ((LOG, ("absent", "answer")),
                             (LOG + "tilefinch-work: label=before x=1\n", ("before", "answer")),
                             (LOG, ("answer", "before")),
                             ("tilefinch-work: label=a x=1\ntilefinch-work: label=b x=2\n", ("a", "b"))):
            with self.assertRaises(ValueError):
                reply.window_metrics(text, labels)

    def test_empty_reply_author_errors_and_nonzero_teardown_refused(self):
        text = ('loop-js ok=yes value="{\"answer\":\"Hello\",\"errors\":0}" error=""\n'
                'loop status=PASS\ninteractive status=ok\n'
                'memory-categories phase=interactive-teardown current=0 expected=0 reconcile=yes\n')
        self.assertEqual(reply.verify(text, "Hello"), "Hello")
        for broken in (text.replace("Hello", ""), text.replace('"errors":0', '"errors":1'),
                       text.replace("current=0", "current=1")):
            with self.assertRaises(ValueError):
                reply.verify(broken, "Hello")


if __name__ == "__main__":
    unittest.main()
