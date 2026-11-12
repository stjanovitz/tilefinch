#!/usr/bin/env python3
"""Site-census tooling without the lab: a capture that landed on some other
page than the one requested fails (census.py capture, through
benchmarks/trace_document.py), and report.py lists the pages a probe pass
did not sweep."""
import argparse
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/site-census"))
import census  # noqa: E402
import report  # noqa: E402
import trace_document  # noqa: E402  (census put benchmarks/ on the path)


def write_record(trace, index, url, status="200", effective=None, location=None):
    lines = ["method=GET", "url=" + url, "status=" + status]
    if effective:
        lines.append("effective-url=" + effective)
    if location:
        lines.append("response-header-0=location: " + location)
    (trace / ("%04d.meta" % index)).write_text("\n".join(lines) + "\n")


def loop_line(url):
    return 'loop frame=0 url="%s" title="t" scroll=0/0\n' % url


class FinalUrlTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.trace = self.root / "capture"
        self.run_dir = self.root / "capture-run"
        self.trace.mkdir()
        self.run_dir.mkdir()

    def tearDown(self):
        self.directory.cleanup()

    def destination(self, url):
        return census.capture_destination(str(self.run_dir), self.trace, url)

    def test_lab_reported_login_redirect_fails(self):
        url = "https://old.example/r/PSP/"
        login = "https://old.example/login/?dest=%2Fr%2FPSP%2F"
        (self.run_dir / "lab.log").write_text(loop_line(url) + loop_line(login))
        self.assertEqual(self.destination(url),
                         (login, "source-signin-required"))

    def test_same_page_passes_whatever_its_query_or_slash(self):
        url = "https://news.example/blog"
        for final in ("https://news.example/blog/", "https://NEWS.example/blog?x=1",
                      "https://news.example/blog#top"):
            (self.run_dir / "lab.log").write_text(loop_line(final))
            self.assertEqual(self.destination(url), (final, None), final)

    def test_other_host_or_path_fails(self):
        url = "https://sfbay.example/search/sss?query=psp"
        for final in ("https://www.example/search/area/sfbay?query=psp",
                      "https://sfbay.example/search/other?query=psp"):
            (self.run_dir / "lab.log").write_text(loop_line(final))
            self.assertEqual(self.destination(url), (final, "source-redirected"))

    def test_native_route_keeps_the_page_url(self):
        # The lab fetched another URL for a native route; the page it shows
        # is still the one requested.
        url = "https://www.video.example/watch?v=1"
        write_record(self.trace, 0, "https://m.video.example/watch?v=1&x=1")
        (self.run_dir / "lab.log").write_text(loop_line(url))
        self.assertEqual(self.destination(url), (url, None))

    def test_without_a_lab_log_the_trace_decides(self):
        url = "https://a.example/page"
        write_record(self.trace, 0, url, "302", location="/login/")
        write_record(self.trace, 1, "https://a.example/login/",
                     effective="https://a.example/login/?next=1")
        self.assertEqual(self.destination(url),
                         ("https://a.example/login/?next=1",
                          "source-signin-required"))
        write_record(self.trace, 0, url, effective="https://b.example/page")
        self.assertEqual(self.destination(url),
                         ("https://b.example/page", "source-redirected"))
        write_record(self.trace, 0, "https://a.example/elsewhere")
        self.assertEqual(self.destination(url), (None, "no-document-record"))

    def test_capture_records_the_final_url_and_fails_without_retry(self):
        census_dir = self.root / "census"
        sites = self.root / "sites.tsv"
        sites.write_text("moved\ttest\thttps://old.example/r/PSP/\n"
                         "kept\ttest\thttps://kept.example/\n")
        finals = {"moved": "https://old.example/login/", "kept": "https://kept.example/"}
        runs = []

        def fake_run_page(lab, site, run_dir, network_args, mode, **_):
            runs.append(site["name"])
            run_dir = pathlib.Path(run_dir)
            run_dir.mkdir(parents=True, exist_ok=True)
            (run_dir / "lab.log").write_text(loop_line(finals[site["name"]]))
            trace = pathlib.Path(network_args[1])
            trace.mkdir(parents=True, exist_ok=True)
            (trace / "trace.meta").write_text("capture-complete=yes\n")
            return {"name": site["name"], "url": site["url"], "exit": 0,
                    "wall_s": 0.1}

        record = {"http_status": 200, "outcome_auto": "working"}
        args = argparse.Namespace(sites=str(sites), selectors=[], corpus=None,
                                  census=str(census_dir), force=False,
                                  retry=True, lab="lab")
        output = io.StringIO()
        with mock.patch.object(census, "run_page", fake_run_page), \
                mock.patch.object(census.extract, "extract", return_value=record), \
                contextlib.redirect_stdout(output):
            with self.assertRaises(SystemExit) as stopped:
                census.capture(args)
        self.assertEqual(stopped.exception.code, 1)
        self.assertEqual(sorted(runs), ["kept", "moved"])  # no retry
        moved = json.loads((census_dir / "corpus/moved/meta.json").read_text())
        self.assertEqual(moved["final_url"], "https://old.example/login/")
        self.assertEqual(moved["final_url_mismatch"], "source-signin-required")
        kept = json.loads((census_dir / "corpus/kept/meta.json").read_text())
        self.assertEqual(kept["final_url"], "https://kept.example/")
        self.assertNotIn("final_url_mismatch", kept)
        self.assertIn("capture of moved is not the requested page",
                      output.getvalue())

    def test_reference_barrier_keeps_its_signin_rule(self):
        # benchmarks/capture-census.py reads the same helper.
        self.assertTrue(trace_document.signin_redirect(
            "https://a.example/x", "/sign-in?next=/x"))
        self.assertFalse(trace_document.signin_redirect(
            "https://a.example/x", "/news/login-tips"))


class ProbePassTests(unittest.TestCase):
    def test_pages_with_scripts_and_no_sweep_are_failures(self):
        scripts = [{"kind": "inline"}]
        recs = {"swept": {"script_list": scripts},
                "unswept": {"script_list": scripts},
                "missing": {"script_list": scripts},
                "garbled": {"script_list": scripts},
                "scriptless": {"script_list": []}}
        probe = {"swept": {"script_list": scripts, "api_probe": {"misses": {}}},
                 "unswept": {"script_list": scripts},
                 "garbled": {"script_list": scripts,
                             "api_probe": {"error": "unparseable probe report"}},
                 "scriptless": {"script_list": []}}
        failures = dict(report.probe_pass_failures(recs, probe))
        self.assertEqual(sorted(failures), ["garbled", "missing", "unswept"])
        self.assertIn("no probe sweep", failures["unswept"])
        self.assertEqual(failures["missing"], "no record in the probe pass")
        self.assertEqual(failures["garbled"], "unparseable probe report")


if __name__ == "__main__":
    unittest.main()
