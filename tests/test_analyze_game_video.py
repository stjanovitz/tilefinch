#!/usr/bin/env python3
"""scripts/analyze-game-video.py on synthetic clips made with ffmpeg lavfi.

No emulator: each clip isolates one behaviour (a camera that bobs or pumps,
a smooth pan, black, noise, checkerboard, frozen, flashing, HUD dropout,
popping, a small blink, a moving object, a shimmering line) and the analyzer
must flag exactly the expected kind.
"""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'analyze-game-video.py'
# CTest passes the built tools/video_motion.c and tools/video_blink.c; by
# hand they default to the release tree's.
HELPER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') \
    else ROOT / 'build-preset-release' / 'tilefinch-video-motion'
BLINK_HELPER = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') \
    else HELPER.with_name('tilefinch-video-blink')
spec = importlib.util.spec_from_file_location('analyze_game_video', SCRIPT)
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)

SIZE = '480x272'
# A large textured, still source the camera clips crop and zoom into.
STILL = 'testsrc2=s=800x500:r=30:d=0.04,loop=loop=-1:size=1,trim=end_frame={n}'


def missing_prerequisite():
    if not (shutil.which('ffmpeg') and shutil.which('ffprobe')):
        return 'ffmpeg or ffprobe is not on PATH'
    for helper in (HELPER, BLINK_HELPER):
        if not helper.exists():
            return f'{helper} is missing'
    return None


MISSING = missing_prerequisite()


@unittest.skipIf(MISSING is not None, MISSING or '')
class AnalyzeGameVideoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def make(self, name, graph, frames=90):
        path = self.root / f'{name}.avi'
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-filter_complex',
                        graph.format(n=frames), '-frames:v', str(frames),
                        '-c:v', 'ffv1', str(path)], check=True)
        return path

    def analyze(self, path, *extra):
        out = self.root / (path.stem + '-out')
        result = subprocess.run([sys.executable, str(SCRIPT), str(path), '--out', str(out),
                                 '--fps', '30', '--quiet', '--hud-top', '', '--hud-bottom', '',
                                 '--motion-helper', str(HELPER),
                                 '--blink-helper', str(BLINK_HELPER), *extra],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads((out / 'events.json').read_text())
        self.assertTrue((out / 'report.txt').exists())
        return summary, out

    def kinds(self, summary):
        return {(e['type'], e['what']) for e in summary['events']}

    def test_bobbing_camera_is_oscillation(self):
        # The view jumps 24 px up and back every other frame.
        path = self.make('bob', STILL + ",crop=480:272:150:'100+24*mod(n,2)'", 75)
        summary, _ = self.analyze(path, '--no-media')
        self.assertIn(('oscillation', 'vertical'), self.kinds(summary))

    def test_pumping_zoom_is_flagged(self):
        # Snap in 40 % every sixth frame, then ease back out.
        path = self.make('pump', STILL + ",zoompan=z='1+0.4*pow(0.7,mod(on,6))':d=1:"
                         "x='iw/2-iw/zoom/2':y='ih/2-ih/zoom/2':s=" + SIZE + ":fps=30", 90)
        summary, out = self.analyze(path)
        kinds = {kind for kind, _ in self.kinds(summary)}
        self.assertTrue({'pumping', 'oscillation'} & kinds, summary['counts'])
        # Media for the worst event: an mp4 clip and a contact sheet.
        self.assertTrue(list(out.glob('*.mp4')) and list(out.glob('*.png')))

    def test_smooth_pan_and_single_retraction_are_quiet(self):
        pan = self.make('pan', STILL + ",crop=480:272:'40+3*n':100", 75)
        summary, _ = self.analyze(pan, '--no-media')
        self.assertEqual(summary['problems'], 0, summary['events'])
        # One snap in, held, then a smooth ease back out.
        retract = self.make('retract', STILL + ",zoompan=z='if(lt(on,20),1,1+0.5*pow(0.85,on-20))'"
                            ":d=1:x='iw/2-iw/zoom/2':y='ih/2-ih/zoom/2':s=" + SIZE + ":fps=30", 75)
        summary, _ = self.analyze(retract, '--no-media')
        kinds = {kind for kind, _ in self.kinds(summary)}
        self.assertFalse({'oscillation', 'pumping', 'jerk'} & kinds, summary['events'])

    def test_frame_content_checks(self):
        black = self.make('black', 'color=c=black:s=' + SIZE + ':r=30', 20)
        self.assertIn(('black', 'frame'), self.kinds(self.analyze(black, '--no-media')[0]))
        noise = self.make('noise', 'color=c=gray:s=' + SIZE + ":r=30,format=gray,geq=lum='random(1)*255'", 10)
        self.assertIn(('garbage', 'noise'), self.kinds(self.analyze(noise, '--no-media')[0]))
        checker = self.make('checker', 'color=c=gray:s=' + SIZE + ":r=30,format=gray,"
                            "geq=lum='if(mod(floor(X/16)+floor(Y/16)\\,2)\\,208\\,235)'", 10)
        self.assertIn(('checkerboard', 'tiles'), self.kinds(self.analyze(checker, '--no-media')[0]))
        frozen = self.make('frozen', 'testsrc2=s=' + SIZE + ':r=30:d=1,tpad=stop_mode=clone:stop=12', 42)
        summary, _ = self.analyze(frozen, '--no-media')
        stalls = [e for e in summary['events'] if e['type'] == 'frozen']
        self.assertEqual(len(stalls), 1, summary['events'])
        self.assertGreaterEqual(stalls[0]['peak'], 12)

    def test_flashes_and_popping(self):
        flash = self.make('flash', 'color=c=0x404040:s=' + SIZE + ":r=30,"
                          "drawgrid=w=32:h=32:t=2:c=0x808080,eq=brightness=0.3:enable='eq(n\\,30)'", 60)
        summary, _ = self.analyze(flash, '--no-media')
        self.assertIn(('flicker', 'brightness'), self.kinds(summary))
        hud = self.make('hud', 'color=c=0x203040:s=' + SIZE + ":r=30,drawgrid=w=40:h=40:t=1:c=0x406070,"
                        "drawbox=x=10:y=6:w=300:h=16:c=white:t=fill:enable='not(eq(n\\,30))'", 60)
        out = self.root / 'hud-out'
        result = subprocess.run([sys.executable, str(SCRIPT), str(hud), '--out', str(out),
                                 '--fps', '30', '--quiet', '--no-media', '--no-camera',
                                 '--no-blink'],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        events = json.loads((out / 'events.json').read_text())['events']
        self.assertIn(('flicker', 'hud-top', 30), {(e['type'], e['what'], e['first']) for e in events})
        pop = self.make('pop', 'color=c=0x203040:s=' + SIZE + ":r=30,drawgrid=w=40:h=40:t=1:c=0x406070,"
                        "drawbox=x=100:y=60:w=220:h=150:c=0xe0a040:t=fill:enable='gte(n\\,30)'", 60)
        summary, _ = self.analyze(pop, '--no-media')
        self.assertIn(('popping', 'area'), self.kinds(summary))

    def test_blinks(self):
        # A grid with a small amber box shown on frame 30 only: one object
        # blink there, appearing, with its box in native pixels.
        scene = 'color=c=0x203040:s=' + SIZE + ':r=30,drawgrid=w=40:h=40:t=1:c=0x406070'
        blink = self.make('blink', scene + ",drawbox=x=200:y=120:w=24:h=16:c=0xe0a040:t=fill:"
                          "enable='eq(n\\,30)'", 60)
        summary, out = self.analyze(blink)
        blinks = [e for e in summary['events'] if e['type'] == 'blink']
        self.assertEqual([(e['what'], e['first'], e['run'], e['appears']) for e in blinks],
                         [('object', 30, 1, True)], summary['events'])
        x0, y0, x1, y1 = blinks[0]['box']
        self.assertTrue(x0 <= 200 and y0 <= 120 and x1 >= 224 and y1 >= 136, blinks[0]['box'])
        self.assertTrue(list(out.glob('*-zoom.png')))
        # The same box vanishing for three frames is one three-frame blink.
        gap = self.make('gap', scene + ",drawbox=x=200:y=120:w=24:h=16:c=0xe0a040:t=fill:"
                        "enable='not(between(n\\,30\\,32))'", 60)
        summary, _ = self.analyze(gap, '--no-media')
        self.assertEqual([(e['what'], e['first'], e['run'], e['appears'])
                          for e in summary['events'] if e['type'] == 'blink'],
                         [('object', 30, 3, False)], summary['events'])
        # A box driving across the grid changes every frame and never comes
        # back: no blink.
        moving = self.make('moving', scene + ",drawbox=x='40+180*t':y=120:w=24:h=16:"
                           "c=0xe0a040:t=fill", 60)
        summary, _ = self.analyze(moving, '--no-media')
        self.assertFalse([e for e in summary['events'] if e['type'] == 'blink'],
                         summary['events'])
        # A one-pixel line dropping out for a frame is shimmer (info only).
        line = self.make('line', scene + ",drawbox=x=60:y=200:w=300:h=1:c=0x60c0b0:t=fill:"
                         "enable='not(eq(n\\,30))'", 60)
        summary, _ = self.analyze(line, '--no-media')
        self.assertIn(('shimmer', 'line'), self.kinds(summary))
        self.assertNotIn('blink', {e['type'] for e in summary['events']})
        self.assertEqual(summary['problems'], 0, summary['events'])

    def test_blink_classification(self):
        hud = (analyzer.DEFAULT_HUD_TOP, analyzer.DEFAULT_HUD_BOTTOM)
        def line(box, pixels, on=(200, 255, 220), context=0, brighter=1):
            return ('blink 40 1 %d %d %d %d 4 %d %d %d %d %d 10 20 30 %d'
                    % (*box, pixels, brighter, *on, context))
        kinds = [kind for _, kind, _ in analyzer.classify_blinks([
            line((10, 4, 60, 20), 200),               # bright text in the top band
            line((10, 4, 60, 20), 300, on=(40, 60, 60)),  # dim: scene behind the band
            line((100, 100, 140, 130), 400),          # a solid object in the scene
            line((100, 100, 300, 104), 300),          # a long thin line
            line((100, 100, 140, 130), 400, context=300),  # something moved through
            line((100, 100, 104, 104), 8),            # too small
        ], 480, 272, hud)]
        self.assertEqual(kinds, ['hud', 'object', 'object', 'shimmer'])

    def test_zigzag_and_event_merging(self):
        zig = analyzer.Zigzag(1.0)
        counts = [zig.push(v, at, 10) for at, v in enumerate([0, 2, 0, 2, 0, 2, 0, .5, .2])]
        self.assertEqual(counts[6], 5)          # 0-2-0-2-0-2-0: five reversals
        self.assertEqual(counts[-1], 5)         # sub-hysteresis wiggles do not count
        rows = [{'frame': f, 'flags': [('oscillation', 'zoom', f)] if f in (3, 4, 5, 40) else []}
                for f in range(50)]
        events = analyzer.events_from(rows, 30)
        self.assertEqual([(e['first'], e['last'], e['peak']) for e in events],
                         [(3, 5, 5), (40, 40, 40)])


if __name__ == '__main__':
    if MISSING is not None:
        # CTest's SKIP_RETURN_CODE: show the missing coverage as Skipped
        # instead of a PASS that ran nothing.
        print(f'SKIP: {MISSING}')
        sys.exit(77)
    unittest.main()
