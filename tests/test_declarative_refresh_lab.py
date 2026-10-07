"""<meta http-equiv=refresh> and the Refresh header in the host lab.

The lab loads page navigations in place (no deferral), so this covers the
synchronous path end to end against the response-keyed replay fixture in
tests/fixtures/http-declarative-refresh, with JavaScript off and on. The
deferred PSP path is tests/test_declarative_refresh.c. Both read that
fixture; tests/make_declarative_refresh_fixture.py regenerates it.

Only `tick` and the per-command `loop frame=... url=... title=...` status
line are needed to see a redirect, so the redirect cases also run against a
lab built before refresh support (where they fail).
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

LAB = Path(sys.argv.pop(1)).resolve()
FIXTURE = Path(sys.argv.pop(1)).resolve()
ORIGIN = 'https://refresh.test'
FRAME = re.compile(r'^loop frame=\d+ url="([^"]*)" title="([^"]*)".* '
                   r'loads=(\d+) relayouts=', re.M)


class DeclarativeRefreshLabTest(unittest.TestCase):
    def run_lab(self, path, commands, javascript=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'commands.txt').write_text(commands + 'quit\n')
            arguments = [str(LAB), '--url', ORIGIN + path,
                         '--psp-profile', 'realistic',
                         '--replay-http-response-keyed', str(FIXTURE),
                         '--commands', str(root / 'commands.txt'),
                         '--no-loop-capture',
                         '--output', str(root / 'final.ppm')]
            arguments.append('--fetch-scripts' if javascript
                             else '--no-javascript')
            result = subprocess.run(arguments, text=True, cwd=root,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:])
        return result.stdout

    def frames(self, output):
        """(url, title, loads) after each command, in order."""
        return [(m.group(1), m.group(2), int(m.group(3)))
                for m in FRAME.finditer(output)]

    def final(self, output):
        return self.frames(output)[-1]

    def test_noscript_redirector_without_javascript(self):
        # VK's away.php: the only no-script way on is a <noscript> refresh.
        output = self.run_lab('/vk-away', 'tick 3 33\n')
        url, title, _ = self.final(output)
        self.assertEqual((url, title), (ORIGIN + '/target', 'Target'))

    def test_noscript_redirector_with_javascript(self):
        # The <noscript> meta is inert text; the script redirect wins.
        output = self.run_lab('/vk-away', 'tick 3 33\n', javascript=True)
        self.assertEqual(self.final(output)[1], 'Script target')

    def test_header_refresh_beats_meta(self):
        output = self.run_lab('/header', 'tick 3 33\n')
        self.assertEqual(self.final(output)[:2],
                         (ORIGIN + '/target-header', 'Header target'))

    def test_delayed_redirect(self):
        # Two seconds on the page clock: not at one, done by three.
        output = self.run_lab('/delayed', 'tick 30 33\nstatus\n'
                                          'tick 60 33\n')
        frames = self.frames(output)
        self.assertEqual(frames[-2][1], 'Delayed')
        self.assertEqual(frames[-1][:2], (ORIGIN + '/later', 'Later'))

    def test_meta_resolves_against_document_base(self):
        output = self.run_lab('/base', 'tick 3 33\n')
        self.assertEqual(self.final(output)[:2],
                         (ORIGIN + '/sub/page', 'Sub page'))

    def test_body_meta_and_comma_separator(self):
        output = self.run_lab('/body-meta', 'tick 3 33\n')
        self.assertEqual(self.final(output)[1], 'Target')

    def test_script_inserted_meta(self):
        output = self.run_lab('/dynamic', 'tick 10 33\n', javascript=True)
        self.assertEqual(self.final(output)[1], 'Target')

    def test_same_url_reload(self):
        # content="1" and no URL: the document reloads itself every second.
        output = self.run_lab('/reload', 'tick 20 33\nstatus\n'
                                         'tick 20 33\nrefresh-status\n')
        frames = self.frames(output)
        self.assertEqual(frames[-3][2], 1)
        self.assertEqual(frames[-1][:3], (ORIGIN + '/reload', 'Reload', 2))
        self.assertIn('loop-refresh state=pending delay=1 ', output)
        self.assertIn(' reload=1 ', output)

    def test_touched_reload_is_offered_not_followed(self):
        # A button press on the page, then the one-second self-refresh.
        output = self.run_lab('/reload', 'down\ntick 60 33\n'
                                         'refresh-status\n')
        self.assertEqual(self.final(output)[:3],
                         (ORIGIN + '/reload', 'Reload', 1))
        self.assertIn('loop-refresh state=offered ', output)
        self.assertIn(' touched=1 ', output)

    def test_touched_redirect_is_still_followed(self):
        output = self.run_lab('/delayed', 'down\ntick 100 33\n')
        self.assertEqual(self.final(output)[:2], (ORIGIN + '/later', 'Later'))

    def test_location_replace_replaces_the_history_entry(self):
        # VK's script path: location.replace(); back skips the redirector.
        output = self.run_lab('/other', 'go https://refresh.test/vk-away\n'
                                        'tick 3 33\nback\n',
                              javascript=True)
        frames = self.frames(output)
        self.assertEqual(frames[-2][:2], (ORIGIN + '/target?via=script',
                                          'Script target'))
        self.assertEqual(frames[-1][:2], (ORIGIN + '/other', 'Other'))

    def test_location_assign_keeps_the_history_entry(self):
        for path in ('/assign', '/href'):
            with self.subTest(path=path):
                output = self.run_lab('/other', 'go https://refresh.test'
                                      + path + '\ntick 3 33\nback\n',
                                      javascript=True)
                frames = self.frames(output)
                self.assertEqual(frames[-2][:2], (ORIGIN + '/target',
                                                  'Target'))
                self.assertEqual(frames[-1][0], ORIGIN + path)

    def test_redirect_replaces_the_history_entry(self):
        # The refreshed-away page is not a step back: back skips it.
        output = self.run_lab('/other', 'go https://refresh.test/vk-away\n'
                                        'tick 3 33\nback\n')
        frames = self.frames(output)
        self.assertEqual(frames[-2][:2], (ORIGIN + '/target', 'Target'))
        self.assertEqual(frames[-1][:2], (ORIGIN + '/other', 'Other'))

    def test_loop_is_stopped(self):
        output = self.run_lab('/self', 'tick 40 33\nrefresh-status\n'
                                       'tick 40 33\nrefresh-status\n')
        self.assertEqual(self.final(output)[2], 6)
        self.assertRegex(output, r'loop-refresh state=stopped .* chain=5 '
                                 r'.*followed=5 cancelled=0 stopped=1 ')

    def test_user_navigation_cancels(self):
        output = self.run_lab('/cancel', 'tick 10 33\n'
                              'go https://refresh.test/other\n'
                              'tick 60 33\nrefresh-status\n')
        self.assertEqual(self.final(output)[:2], (ORIGIN + '/other', 'Other'))
        self.assertRegex(output, r'loop-refresh .* cancelled=1 ')

    def test_typing_cancels(self):
        output = self.run_lab('/form', 'focus-next\ntype psp\n'
                              'tick 60 33\nrefresh-status\n')
        self.assertEqual(self.final(output)[1], 'Form')
        self.assertIn('loop-refresh state=cancelled ', output)

    def test_no_javascript_or_data_navigation(self):
        for path in ('/javascript', '/data'):
            with self.subTest(path=path):
                output = self.run_lab(path, 'tick 10 33\nrefresh-status\n',
                                      javascript=True)
                url, title, loads = self.final(output)
                self.assertEqual((url, loads), (ORIGIN + path, 1))
                self.assertNotEqual(title, 'owned')
                self.assertIn('loop-refresh state=refused ', output)

    def test_template_and_scripted_noscript_are_inert(self):
        for path, javascript in (('/template', False), ('/noscript', True)):
            with self.subTest(path=path):
                output = self.run_lab(path, 'tick 10 33\nrefresh-status\n',
                                      javascript=javascript)
                self.assertEqual(self.final(output)[0], ORIGIN + path)
                self.assertIn('loop-refresh state=none ', output)

    def test_frames_never_navigate_the_top(self):
        for path in ('/sandbox-top', '/frame-top'):
            with self.subTest(path=path):
                output = self.run_lab(path, 'tick 10 33\n', javascript=True)
                self.assertEqual(self.final(output)[0], ORIGIN + path)
                self.assertRegex(output, r'frames discovered=1 loaded=1 ')


if __name__ == '__main__':
    unittest.main()
