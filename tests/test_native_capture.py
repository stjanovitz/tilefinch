"""Browser-free regression checks for native diagnostic capture evidence."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'benchmarks'))
from native_capture import (StaleHeightError, VIEWS, build_five_view_commands,
                            run_native_capture, validate_native_capture,
                            _input_identity)

LAB = None
if '--lab' in sys.argv:
    index = sys.argv.index('--lab')
    LAB = Path(sys.argv[index + 1]).resolve()
    del sys.argv[index:index + 2]


class NativeCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name)
        self.commands = build_five_view_commands(self.output, 108, viewport_height=8)
        (self.output / 'commands.txt').write_text(self.commands)
        (self.output / 'stderr.log').write_text('diagnostics stay separate\n')
        (self.output / 'invocation.json').write_text('{"args":["lab"],"exit":0}\n')
        self.lines = []
        for i, p in enumerate(VIEWS):
            position = f'loop frame={i} url="https://example.test/" title="Test" scroll={p}/100 focus=none:0'
            self.lines.extend([position, f'tilefinch-work: label=native-view-{p} counter=1',
                               position, position])
            (self.output / f'view-{p}.ppm').write_bytes(b'P6\n2 8\n255\n' + bytes(48))
        self.lines.append('interactive teardown=0 active=0 largest=0 peak=1000 allocations=4 frees=4 failures=0 status=PASS')

    def validate(self):
        (self.output / 'stdout.log').write_text('\n'.join(self.lines) + '\n')
        return validate_native_capture(self.output, 0, viewport_width=2, viewport_height=8)

    def test_valid_capture_hashes_and_offsets(self):
        result = self.validate()
        self.assertEqual([p['y'] for p in result['positions']], list(VIEWS))
        self.assertEqual(result['document_height'], 108)
        self.assertEqual(len(result['artifacts']), 9)

    def test_missing_labels_are_not_recovered_from_status_counts(self):
        self.lines = [s for s in self.lines if 'tilefinch-work:' not in s]
        with self.assertRaisesRegex(ValueError, 'five completed'):
            self.validate()

    def test_truncated_marker_is_refused(self):
        self.lines[5] = 'tilefinch-work: label=native-view-'
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.validate()

    def test_reordered_labels_are_refused(self):
        self.lines[5], self.lines[9] = self.lines[9], self.lines[5]
        with self.assertRaisesRegex(ValueError, 'reordered'):
            self.validate()

    def test_stale_intermediate_offsets_are_refused(self):
        self.lines = [s.replace('scroll=25/100', 'scroll=0/100') for s in self.lines]
        with self.assertRaises(StaleHeightError) as error:
            self.validate()
        self.assertEqual(error.exception.height, 108)

    def test_render_reflow_is_refused(self):
        self.lines[7] = self.lines[7].replace('scroll=25/100', 'scroll=26/100')
        with self.assertRaisesRegex(ValueError, 'moved between'):
            self.validate()

    def test_actual_offsets_must_be_monotonic_even_on_a_short_page(self):
        self.lines = [s.replace('/100', '/5').replace('scroll=100/', 'scroll=5/')
                      .replace('scroll=75/', 'scroll=3/').replace('scroll=50/', 'scroll=1/')
                      .replace('scroll=25/', 'scroll=4/') for s in self.lines]
        with self.assertRaisesRegex(ValueError, 'offsets are reordered'):
            self.validate()

    def test_truncated_status_and_frame_are_refused(self):
        self.lines[4] = 'loop frame=1 url="https://example.test/" scroll=25/'
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.validate()
        self.lines[4] = 'loop frame=1 url="https://example.test/" scroll=25/100 focus=none:0'
        (self.output / 'view-25.ppm').write_bytes(b'P6\n2 8\n255\n')
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.validate()

    def test_edges_and_teardown_are_required(self):
        self.lines[-1] = self.lines[-1].replace('teardown=0', 'teardown=32')
        with self.assertRaisesRegex(ValueError, 'clean teardown'):
            self.validate()

    def test_commands_never_use_page_js_or_extra_drain_time(self):
        self.assertNotIn('scrollTo', self.commands)
        self.assertNotIn('js ', self.commands)
        self.assertTrue(all(s.endswith(' 0') for s in self.commands.splitlines() if s.startswith('drain ')))
        (self.output / 'commands.txt').write_text(self.commands.replace('drain 4096 0', 'drain 4096 750', 1))
        with self.assertRaisesRegex(ValueError, 'virtual time'):
            self.validate()

    def test_exact_native_offsets_do_not_assume_page_step_size(self):
        commands = build_five_view_commands(self.output, 13817)
        self.assertEqual([s for s in commands.splitlines() if s.startswith('scroll-by ')],
                         ['scroll-by 0', 'scroll-by 3386', 'scroll-by 6773', 'scroll-by 10159'])
        self.assertNotIn('page-down', commands)

    def test_separate_streams_and_command_failures_are_required(self):
        (self.output / 'stderr.log').write_text('loop command-failed="scroll-by"\n')
        with self.assertRaisesRegex(ValueError, 'failed commands'):
            self.validate()

    @unittest.skipUnless(LAB, 'supply --lab for the native command integration check')
    def test_native_scroll_by_ignores_sticky_page_step_obstructions(self):
        fixture = self.output / 'page.html'
        fixture.write_text('<!doctype html><style>body{margin:0}header{position:fixed;'
                           'top:0;height:70px;width:100%;background:red}main{height:6000px}'
                           '</style><header>Fixed header</header><main>Long page</main>')
        commands = self.output / 'native.commands'
        commands.write_text('top\nscroll-by 1000\nstatus\nscroll-by -750\nstatus\nquit\n')
        run = subprocess.run([str(LAB), '--fixture', str(fixture),
                              '--viewport-css-width', '480', '--viewport-css-height', '272',
                              '--commands', str(commands),
                              '--no-loop-capture'], capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('scroll=1000/', run.stdout)
        self.assertIn('scroll=250/', run.stdout)
        self.assertIn('interactive teardown=0 active=0 largest=0', run.stdout)

    def test_stale_height_gets_only_one_fresh_offline_retry(self):
        destination = self.output / 'run'
        calls = []
        def attempt(args, output, height, sweep, tick, timeout):
            output.mkdir()
            calls.append((height, sweep, tick))
            for name in ('commands.txt', 'stdout.log', 'stderr.log'):
                (output / name).write_text('preserved attempt\n')
            if len(calls) == 1:
                raise StaleHeightError(108)
            return {'document_height': 108, 'artifacts': {}}
        with (patch('native_capture._run_attempt', side_effect=attempt),
              patch('native_capture._input_identity', return_value={'lab_sha256': 'fixed', 'trace_sha256': 'fixed'})):
            result = run_native_capture(['lab', '--replay-http-response-keyed', 'trace'], destination,
                                        272, sweep_ticks=7)
        self.assertEqual(calls, [(272, 7, 33), (108, 7, 33)])
        self.assertEqual(len(result['attempts']), 2)
        self.assertTrue((destination / 'attempt-1' / 'stdout.log').exists())
        self.assertEqual(result['extra_clock_turns'], 7)

    def test_retry_cannot_hide_changed_document_geometry(self):
        destination = self.output / 'unstable'
        calls = []
        def attempt(args, output, height, sweep, tick, timeout):
            output.mkdir()
            calls.append(height)
            for name in ('commands.txt', 'stdout.log', 'stderr.log'):
                (output / name).write_text('preserved attempt\n')
            if len(calls) == 1:
                raise StaleHeightError(108)
            return {'document_height': 109, 'artifacts': {}}
        with (patch('native_capture._run_attempt', side_effect=attempt),
              patch('native_capture._input_identity', return_value={'lab_sha256': 'fixed', 'trace_sha256': 'fixed'})):
            with self.assertRaisesRegex(ValueError, 'across fresh attempts'):
                run_native_capture(['lab', '--replay-http-response-keyed', 'trace'], destination, 272)
        self.assertEqual(len(calls), 2)

    def test_changed_input_is_refused_even_after_completed_frames(self):
        with (patch('native_capture._input_identity', side_effect=[{'trace_sha256': 'before'}, {'trace_sha256': 'before'}, {'trace_sha256': 'after'}]),
              patch('native_capture._run_attempt', return_value={'document_height': 272, 'artifacts': {}})):
            with self.assertRaisesRegex(ValueError, 'inputs changed'):
                run_native_capture(['lab', '--replay-http-response-keyed', 'trace'], self.output / 'changed', 272)

    def test_shared_engine_rebuild_changes_input_identity(self):
        lab = self.output / 'lab'
        lab.write_bytes(b'unchanged executable')
        trace = self.output / 'trace'
        trace.mkdir()
        args = [str(lab), '--replay-http-response-keyed', str(trace)]
        with patch('native_capture.trace_digest', return_value='fixed trace'):
            static = _input_identity(args)
            self.assertEqual(static.get('engine_images_sha256', {}), {})
            engine = self.output / 'libtilefinch_core.dylib'
            engine.write_bytes(b'engine before')
            before = _input_identity(args)
            engine.write_bytes(b'engine after')
            after = _input_identity(args)
        self.assertEqual(before['lab_sha256'], after['lab_sha256'])
        self.assertNotEqual(before, after)

    def test_no_retry_for_missing_or_corrupt_image(self):
        self.lines = [s.replace('scroll=25/100', 'scroll=0/100') for s in self.lines]
        (self.output / 'view-25.ppm').write_bytes(b'P6\n2 8\n255\n')
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.validate()

    def test_bounds_and_live_run_are_refused_before_launch(self):
        with self.assertRaisesRegex(ValueError, 'offline'):
            run_native_capture(['lab', '--url', 'https://example.test'], self.output / 'live', 272)
        with self.assertRaisesRegex(ValueError, 'bound'):
            build_five_view_commands(self.output, 272, sweep_ticks=257)


if __name__ == '__main__':
    unittest.main()
