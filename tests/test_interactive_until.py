"""The buffered host lab must stop at the observation used on the device."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LAB = Path(sys.argv.pop(1)).resolve()
PAGE = '''<!doctype html><html><body><button id="go">Begin</button>
<p id="reply">WAIT</p><script>
globalThis.steps = 0;
document.getElementById('go').onclick = function () {
  function step() {
    steps++;
    if (steps === 3) document.getElementById('reply').textContent = 'READY';
    else setTimeout(step, 16);
  }
  setTimeout(step, 16);
};
</script></body></html>'''


class InteractiveUntilTest(unittest.TestCase):
    def run_lab(self, commands, options=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'page.html').write_text(PAGE)
            (root / 'commands.txt').write_text(commands)
            return subprocess.run([str(LAB), '--fixture', str(root / 'page.html'),
                '--commands', str(root / 'commands.txt'), '--no-loop-capture',
                '--output', str(root / 'result.ppm'), *options], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)

    def test_stops_on_first_matching_turn(self):
        result = self.run_lab("click #go\nuntil 100 16 document.getElementById('reply').textContent === 'READY'\n"
                              "js JSON.stringify({steps,reply:document.getElementById('reply').textContent})\nquit\n")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=yes iterations=3 cap=100', result.stdout)
        self.assertIn('"steps":3,"reply":"READY"', result.stdout)
        self.assertRegex(result.stdout, r'memory-categories phase=interactive-teardown current=0 expected=0 .*reconcile=yes')

    def test_already_matching_does_not_advance(self):
        result = self.run_lab('until 100 16 true\njs steps\nquit\n')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=yes iterations=0 cap=100', result.stdout)
        self.assertIn('loop-js ok=yes value="0"', result.stdout)

    def test_exhaustion_refuses_a_success_report(self):
        result = self.run_lab('until 3 16 false\nquit\n')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop-until matched=no iterations=3 cap=3', result.stdout)
        self.assertNotIn('loop status=PASS', result.stdout)

    def test_probe_exception_is_not_a_match(self):
        result = self.run_lab('until 3 16 (()=>{throw new Error("probe-failed")})()\nquit\n')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('loop command-failed="until"', result.stdout)
        self.assertNotIn('matched=yes', result.stdout)

    def test_invalid_or_unbounded_arguments_fail(self):
        for arguments in ('0 16 true', '10001 16 true', '-1 16 true',
                          '3 60001 true', '3 -1 true', '3 16',
                          '3 16x true', '99999999999999999999999999 16 true'):
            with self.subTest(arguments=arguments):
                result = self.run_lab('until ' + arguments + '\nquit\n')
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertNotIn('matched=yes', result.stdout)

    def test_stylesheet_count_is_explicit_and_bounded(self):
        for value in ('1', '32'):
            result = self.run_lab('quit\n', ('--stylesheet-count', value))
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('stylesheets=' + value + ' ', result.stdout)
        for value in ('0', '33', '-1', '+1', '32x', '', '99999999999999999999999999'):
            result = self.run_lab('quit\n', ('--stylesheet-count', value))
            self.assertEqual(result.returncode, 2, result.stdout)
        result = self.run_lab('quit\n', ('--stylesheet-count',))
        self.assertEqual(result.returncode, 2, result.stdout)


if __name__ == '__main__':
    unittest.main()
