#!/usr/bin/env python3
"""Find visual glitches in a recorded game video (PPSSPP frame dump).

Input is a recording directory written by
`scripts/run-ppsspp-input-script.sh --record-video DIR` (VIDEO/*.avi,
AUDIO/*.wav, tilefinch-validation.txt) or a video file. ffmpeg decodes
downscaled grayscale frames (240x136 by default) and this script, using only
the standard library, measures every frame:

- camera motion: a global zoom and shift per frame from tools/video_motion.c
  (built as build-preset-release/tilefinch-video-motion; the look target
  sits at the screen centre, so a dolly is a zoom about the centre and a
  pitch or bob is a vertical shift). Flags
  oscillation (zoom or vertical motion reversing over and over), camera
  pumping (snapping in, easing out, snapping in again), sudden jumps and
  sustained jerk;
- flicker: single-frame brightness flashes, and the HUD bands' bright text
  vanishing for a frame or two;
- popping: a large connected area changing in one frame that the global
  motion does not explain;
- black, blank and garbage (noise) frames;
- frozen frames (identical frames presented in a row: a stall);
- checkerboard (unrasterized tile placeholder) and partially drawn frames;
- blinks: a localized region that changes for one to four frames and then
  returns exactly to what it was (on-off-on or off-on-off) while its
  surroundings stay put, from tools/video_blink.c on full-resolution
  colour frames (build-preset-release/tilefinch-video-blink). These are
  sorted into HUD blinks (bright pixels in a HUD band), thin-line shimmer
  (a long run one to a few pixels thick: a sub-pixel line dropping out of
  the rasterization as the camera moves) and object blinks (anything else:
  geometry or an effect that vanishes or appears for a frame or two). The
  single-frame transient and popping checks look for large areas; blinks
  find the small ones. Hit flashes, muzzle flashes and sparks are blinks
  too: review the zoomed sheets before calling one a bug.

Writes report.txt (timestamps and frame numbers), events.json, frames.tsv
(per-frame measures), and for each event a short mp4 clip and a PNG contact
sheet of the frames around it, into --out.

Timing: PPSSPP dumps one frame per frame the game presents; the AVI's
59.94 fps label is nominal. The true rate is frames / WAV duration when the
recording has audio, else --fps (default 30). Events report both the frame
number and the time at that rate.
"""
import argparse
import collections
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys

# Frame geometry of the analysis stream (the PSP screen halved).
WIDTH, HEIGHT = 240, 136

# Treadline's HUD: text rows at the top (score, arena, HP) and bottom-left
# (gadget/secondary). Calibrated on 480x272 dumps; rows are in analysis
# pixels. Override with --hud-top / --hud-bottom (empty disables).
DEFAULT_HUD_TOP = (0, 14, 0, WIDTH)          # y0, y1, x0, x1
DEFAULT_HUD_BOTTOM = (122, 136, 0, WIDTH // 2)
# The scene band the camera estimate reads (between the HUD rows).
SCENE_ROWS = (16, 120)

LIMITS = {
    # camera motion
    'window_seconds': 1.5,      # burst window, as in the invariant sweep
    'zoom_hysteresis': math.log(1.04),   # a zoom swing must reach 4 %
    'vertical_hysteresis': 3.0,          # analysis px (6 native px)
    'reversals': 6,             # three in/out cycles inside a window
    'snap_zoom': math.log(1.10),         # one-frame zoom-in that is a snap
    're_expansion': math.log(1.04),      # easing out between two snaps
    're_snaps': 2,              # pumps per window
    'jump_zoom': math.log(1.30),         # one-frame zoom no easing makes
    'jump_shift': 14.0,         # one-frame shift (analysis px)
    'jerk': 6.0,                # |third difference| of cumulative vertical px
    'jerk_frames': 10,          # high-jerk frames per window
    'cut_histogram': 0.3,       # luma histogram change that is a hard cut
    # frames
    'black_mean': 8.0, 'black_std': 3.0, 'blank_std': 1.0,
    'garbage_hf': 18.0,         # mean |horizontal gradient|, game frames ~3-6
    'flash': 14.0,              # single-frame mean jump against both neighbours
    'flash_agree': 5.0,         # ... while the neighbours agree this closely
    'hud_drop': 0.4,            # HUD bright pixels below this share of normal
    'hud_min': 40,              # ... when normal has at least this many
    'pop_block': 24.0,          # 8x8 block mean change (motion compensated)
    'pop_area': 0.12,           # connected share of scene blocks
    'pop_quiet_zoom': math.log(1.03),    # only judge pops when the camera is calm
    'freeze_frames': 3,         # identical frames in a row that are a stall
    'transient': 12.0,          # block mean change into and out of one frame ...
    'transient_return': 0.3,    # ... while its neighbours agree (relative)
    'transient_band': 0.6,      # ... across this share of a block row (a band)
    'transient_area': 0.08,     # ... or in one connected area this large
    'checker_hits': 12,         # checkerboard sample hits per frame
    'partial_flat': 0.25,       # rise in flat-block share over the neighbours
}


# Blink classification (native pixels, from tools/video_blink.c). A blink
# needs this many blinking pixels, and is isolated when the pixels around it
# (its box grown by 16) where the frames either side disagree number at
# most BLINK_CONTEXT of its own: more means something moved through (a
# shell, a tank, a shaking camera) rather than blinked.
BLINK = {'pixels': 12, 'context': 0.25, 'hud_luma': 110, 'thin': 8, 'long': 24,
         'sparse': 0.3, 'still_shift': 1.0, 'still_zoom': 0.004}
# A blink inside or right after a camera move (screen shake that swings out
# and back is an on-off-on of the whole scene) is the camera checks'
# business; blinks are judged only while the motion estimate is still.

# Single camera jumps (a hard cut, a deliberate retraction or a glitch: the
# video alone cannot tell), frozen runs (pause screens freeze on purpose) and
# thin-line shimmer (aliasing of sub-pixel lines, present whenever the
# camera moves; counted per recording in the report) are reported for review
# but are not counted as problems.
INFO_TYPES = frozenset({'jump', 'frozen', 'shimmer'})
CAMERA_TYPES = frozenset({'oscillation', 'pumping', 'jerk', 'jump'})


# ------------------------------------------------------------ decoding --
def probe(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                          '-show_entries', 'stream=nb_frames,r_frame_rate,width,height',
                          '-of', 'json', str(path)], capture_output=True, text=True, check=True)
    stream = json.loads(out.stdout)['streams'][0]
    num, den = stream['r_frame_rate'].split('/')
    frames = int(stream.get('nb_frames') or 0)
    return {'nominal_fps': float(num) / float(den), 'frames': frames,
            'width': int(stream['width']), 'height': int(stream['height'])}


def audio_seconds(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
                          '-of', 'json', str(path)], capture_output=True, text=True, check=True)
    return float(json.loads(out.stdout)['format']['duration'])


def frames_of(path):
    """Yield (index, bytes) grayscale frames at WIDTH x HEIGHT."""
    command = ['ffmpeg', '-v', 'error', '-i', str(path), '-vf',
               f'scale={WIDTH}:{HEIGHT}:flags=area,format=gray', '-f', 'rawvideo', '-']
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    size = WIDTH * HEIGHT
    index = 0
    try:
        while True:
            frame = process.stdout.read(size)
            if len(frame) < size:
                break
            yield index, frame
            index += 1
    finally:
        process.stdout.close()
        process.wait()


# ---------------------------------------------------------- per frame --
class MotionHelper:
    """tools/video_motion.c: global zoom and shift per frame (the search is
    too slow in pure Python). Frames go in as they are decoded; the answer
    for a frame is read after this script has measured it, so both run at
    once."""

    def __init__(self, path):
        self.process = subprocess.Popen(
            [str(path), str(WIDTH), str(HEIGHT), str(SCENE_ROWS[0]), str(SCENE_ROWS[1])],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def send(self, frame):
        self.process.stdin.write(frame)
        self.process.stdin.flush()

    def receive(self):
        fields = self.process.stdout.readline().split()
        if len(fields) != 6:
            raise RuntimeError('video motion helper stopped')
        return [float(v) for v in fields[1:]]

    def close(self):
        self.process.stdin.close()
        self.process.wait()


def default_helper():
    return Path(__file__).resolve().parents[1] / 'build-preset-release' / 'tilefinch-video-motion'


def default_blink_helper():
    return default_helper().with_name('tilefinch-video-blink')


def blink_lines(video, helper):
    """Run tools/video_blink.c over full-resolution RGB frames."""
    info = probe(video)
    decode = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', str(video), '-f', 'rawvideo',
                               '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
    try:
        result = subprocess.run([str(helper), str(info['width']), str(info['height'])],
                                stdin=decode.stdout, capture_output=True, text=True, check=True)
    finally:
        decode.stdout.close()
        decode.wait()
    return info, result.stdout.splitlines()


def classify_blinks(lines, width, height, hud, rows=None):
    """(frame, kind, detail) for each isolated blink. kind is 'hud',
    'shimmer' (thin or sparse line pieces) or 'object'; detail keeps the box
    and colours. With rows (the per-frame measures), blinks while the
    camera moves are dropped."""
    def still(at):
        if not rows or at >= len(rows):
            return True
        row = rows[at]
        return (abs(row['zoom']) < BLINK['still_zoom'] and abs(row['dx']) < BLINK['still_shift']
                and abs(row['dy']) < BLINK['still_shift'])
    sx, sy = width / WIDTH, height / HEIGHT
    bands = [(r[0] * sy, r[1] * sy) for r in hud if r]
    out = []
    for line in lines:
        fields = line.split()
        if not fields or fields[0] != 'blink' or len(fields) != 17:
            continue
        (first, run, x0, y0, x1, y1, _cells, pixels, brighter,
         r1, g1, b1, r0, g0, b0, context) = (int(v) for v in fields[1:])
        if pixels < BLINK['pixels'] or context > BLINK['context'] * pixels:
            continue
        if not all(still(at) for at in range(first, first + run + 1)):
            continue
        on = (r1, g1, b1) if brighter else (r0, g0, b0)
        luma = (on[0] * 77 + on[1] * 150 + on[2] * 29) >> 8
        w, h = x1 - x0, y1 - y0
        if any(y0 >= top - .5 and y1 <= bottom + .5 for top, bottom in bands) \
                and luma >= BLINK['hud_luma']:
            kind = 'hud'
        elif ((h <= BLINK['thin'] and w >= BLINK['long']) or (w <= BLINK['thin'] and h >= BLINK['long'])
              or pixels < BLINK['sparse'] * w * h):
            kind = 'shimmer'
        else:
            kind = 'object'
        out.append((first, kind, {'run': run, 'box': [x0, y0, x1, y1], 'pixels': pixels,
                                  'appears': bool(brighter), 'on': list(on),
                                  'off': [r0, g0, b0] if brighter else [r1, g1, b1]}))
    return out


def block_means(frame, size=8):
    columns, rows = WIDTH // size, HEIGHT // size
    out = []
    for by in range(rows):
        sums = [0] * columns
        for y in range(by * size, by * size + size):
            row = frame[y * WIDTH:(y + 1) * WIDTH]
            for bx in range(columns):
                sums[bx] += sum(row[bx * size:bx * size + size])
        out.extend(s / (size * size) for s in sums)
    return out, columns, rows


def flat_share(frame, size=8):
    """Share of size x size blocks that are a single flat colour."""
    flat = total = 0
    for by in range(SCENE_ROWS[0] // size, SCENE_ROWS[1] // size):
        for bx in range(WIDTH // size):
            low, high = 255, 0
            for y in range(by * size, by * size + size, 2):
                piece = frame[y * WIDTH + bx * size:y * WIDTH + bx * size + size]
                low = min(low, min(piece))
                high = max(high, max(piece))
            total += 1
            if high - low <= 2:
                flat += 1
    return flat / max(1, total)


def checker_hits(frame):
    """Sample points that look like the 16-native-px tile checkerboard."""
    hits = 0
    for y in range(0, HEIGHT - 9, 3):
        row, below = y * WIDTH, (y + 8) * WIDTH
        for x in range(0, WIDTH - 9, 3):
            a, b = frame[row + x], frame[row + x + 8]
            c, d = frame[below + x], frame[below + x + 8]
            if abs(a - d) > 2 or abs(b - c) > 2 or not 15 <= abs(a - b) <= 45:
                continue
            light, dark = max(a, b), min(a, b)
            if not ((190 <= light <= 248 and 190 <= dark) or (light <= 60 and dark >= 20)):
                continue
            if abs(frame[row + x + 2] - a) <= 2 and abs(frame[row + WIDTH * 2 + x] - a) <= 2:
                hits += 1
    return hits


def hf_energy(frame):
    total = count = 0
    for y in range(SCENE_ROWS[0], SCENE_ROWS[1], 4):
        row = frame[y * WIDTH:(y + 1) * WIDTH]
        total += sum(abs(row[x + 1] - row[x]) for x in range(0, WIDTH - 1, 2))
        count += (WIDTH - 1 + 1) // 2
    return total / max(1, count)


def bright_count(frame, region, threshold=150):
    y0, y1, x0, x1 = region
    count = 0
    for y in range(y0, y1):
        count += sum(1 for v in frame[y * WIDTH + x0:y * WIDTH + x1] if v >= threshold)
    return count


def histogram(frame, bins=16):
    sample = frame[::3]
    counts = [0] * bins
    for value in sample:
        counts[value * bins >> 8] += 1
    return [c / len(sample) for c in counts]


def mean_std(frame):
    sample = frame[::7]
    mean = sum(sample) / len(sample)
    var = sum((v - mean) ** 2 for v in sample[::3]) / len(sample[::3])
    return mean, math.sqrt(var)


def largest_component(mask, columns, rows):
    seen = [False] * len(mask)
    best = 0
    for start, on in enumerate(mask):
        if not on or seen[start]:
            continue
        stack, size = [start], 0
        seen[start] = True
        while stack:
            at = stack.pop()
            size += 1
            x, y = at % columns, at // columns
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if 0 <= nx < columns and 0 <= ny < rows:
                    n = ny * columns + nx
                    if mask[n] and not seen[n]:
                        seen[n] = True
                        stack.append(n)
        best = max(best, size)
    return best


# ------------------------------------------------------------ analysis --
class Zigzag:
    """Reversals of a cumulative signal with hysteresis (as the sweep's)."""

    def __init__(self, hysteresis):
        self.hysteresis = hysteresis
        self.reset(0.0)

    def reset(self, value):
        self.direction, self.extreme = 0, value
        self.low = self.high = value
        self.times = collections.deque()

    def push(self, value, frame, window):
        reversed_ = False
        if self.direction == 0:
            self.low, self.high = min(self.low, value), max(self.high, value)
            if value - self.low >= self.hysteresis:
                self.direction, self.extreme = 1, value
            elif self.high - value >= self.hysteresis:
                self.direction, self.extreme = -1, value
        elif self.direction > 0:
            if value > self.extreme:
                self.extreme = value
            elif self.extreme - value >= self.hysteresis:
                self.direction, self.extreme, reversed_ = -1, value, True
        else:
            if value < self.extreme:
                self.extreme = value
            elif value - self.extreme >= self.hysteresis:
                self.direction, self.extreme, reversed_ = 1, value, True
        if reversed_:
            self.times.append(frame)
        while self.times and self.times[0] <= frame - window:
            self.times.popleft()
        return len(self.times)


def analyze(frames, fps, limits=LIMITS, hud=(DEFAULT_HUD_TOP, DEFAULT_HUD_BOTTOM),
            helper=None, progress=None):
    """Measure every frame; return one row (measures and flags) per frame.
    Without a motion helper the camera checks are skipped."""
    window = max(3, int(round(limits['window_seconds'] * fps)))
    y0, y1 = SCENE_ROWS
    rows = []
    previous = None
    before = None   # the frame before previous, for single-frame transients
    zoom_zig = Zigzag(limits['zoom_hysteresis'])
    vertical_zig = Zigzag(limits['vertical_hysteresis'])
    cumulative_zoom = cumulative_vertical = 0.0
    history = collections.deque(maxlen=4)
    snaps = collections.deque()
    jerks = collections.deque()
    last_snap, expanded = -10 ** 9, 0.0
    segment = 0
    for index, frame in frames:
        if helper:
            helper.send(frame)
        mean, std = mean_std(frame)
        row = {'frame': index, 't': index / fps, 'mean': mean, 'std': std,
               'hf': hf_energy(frame), 'flat': flat_share(frame),
               'checker': checker_hits(frame),
               'hud': [bright_count(frame, region) if region else 0 for region in hud],
               'zoom': 0.0, 'dx': 0.0, 'dy': 0.0, 'match': 0.0, 'diff': 0.0,
               'pop': 0.0, 'cut': False, 'hist': 0.0, 'flags': []}
        blocks, columns, block_rows = block_means(frame)
        hist = histogram(frame)
        if previous is not None:
            p_frame, p_blocks, p_hist = previous
            row['diff'] = sum(abs(a - b) for a, b in zip(frame[::5], p_frame[::5])) / len(frame[::5])
            # A hard cut (another screen, arena or view) changes what is on
            # screen; a camera move within one scene keeps its colours.
            row['hist'] = sum(abs(a - b) for a, b in zip(hist, p_hist)) / 2
            row['cut'] = row['hist'] > limits['cut_histogram']
            if helper:
                zoom, dx, dy, cost, identity = helper.receive()
                row['zoom'], row['dx'], row['dy'] = zoom, dx, dy
                # How much of the change the camera model explains.
                row['match'] = cost / identity if identity > 0 else 0.0
            # Popping: motion-compensated block changes with a calm camera.
            if abs(row['zoom']) < limits['pop_quiet_zoom'] and not row['cut']:
                sx, sy = int(round(row['dx'] / 8)), int(round(row['dy'] / 8))
                mask = []
                band = range(y0 // 8, y1 // 8)
                for by in range(block_rows):
                    for bx in range(columns):
                        px, py = bx - sx, by - sy
                        on = (by in band and 0 <= px < columns and 0 <= py < block_rows
                              and abs(blocks[by * columns + bx] - p_blocks[py * columns + px])
                              > limits['pop_block'])
                        mask.append(on)
                scene_blocks = len(band) * columns
                row['pop'] = largest_component(mask, columns, block_rows) / scene_blocks
        elif helper:
            helper.receive()
        # Single-frame transient: blocks of the previous frame that differ
        # from both neighbours, which agree there (a one-frame flash of
        # something else: browser chrome, a camera snap and back, a dropped
        # HUD, geometry that pops in for one frame).
        # A big camera move into or out of the frame is the camera checks'
        # business (a snap-and-back is a transient by construction).
        calm = all(abs(r['zoom']) < limits['snap_zoom'] and math.hypot(r['dx'], r['dy'])
                   < limits['jump_shift'] for r in (rows[-1], row)) if rows else True
        if before is not None and calm:
            middle = previous[1]
            jump, back = limits['transient'], limits['transient_return']
            mask = []
            for k, value in enumerate(middle):
                into = abs(value - before[k])
                mask.append(into >= jump and abs(value - blocks[k]) >= jump
                            and abs(before[k] - blocks[k]) <= back * into)
            # Hit flashes and muzzle effects are compact one-frame blobs and
            # legitimate; flag a band across the screen (an overlay or chrome
            # shown for one frame) or a large connected area.
            widest = max(sum(mask[r * columns:(r + 1) * columns]) for r in range(block_rows))
            area = largest_component(mask, columns, block_rows) / len(mask)
            rows[-1]['transient'] = area
            if widest >= limits['transient_band'] * columns and sum(mask) >= 15:
                rows[-1]['flags'].append(('transient', 'band', round(widest / columns, 2)))
            elif area >= limits['transient_area']:
                rows[-1]['flags'].append(('transient', 'area', round(area, 3)))
        before = previous[1] if previous is not None else None
        previous = (frame, blocks, hist)
        # Camera motion over the segment since the last cut.
        if row['cut'] or index == 0 or not helper:
            segment = 0
            zoom_zig.reset(cumulative_zoom)
            vertical_zig.reset(cumulative_vertical)
            snaps.clear()
            jerks.clear()
            history.clear()
            last_snap, expanded = -10 ** 9, 0.0
        else:
            segment += 1
            cumulative_zoom += row['zoom']
            cumulative_vertical += row['dy']
            zr = zoom_zig.push(cumulative_zoom, index, window)
            vr = vertical_zig.push(cumulative_vertical, index, window)
            row['zoom_reversals'], row['vertical_reversals'] = zr, vr
            if zr >= limits['reversals']:
                row['flags'].append(('oscillation', 'zoom', zr))
            if vr >= limits['reversals']:
                row['flags'].append(('oscillation', 'vertical', vr))
            if row['zoom'] > limits['snap_zoom']:
                if index - last_snap <= window and expanded >= limits['re_expansion']:
                    snaps.append(index)
                last_snap, expanded = index, 0.0
            elif row['zoom'] < 0:
                expanded -= row['zoom']
            while snaps and snaps[0] <= index - window:
                snaps.popleft()
            if len(snaps) >= limits['re_snaps']:
                row['flags'].append(('pumping', 'zoom', len(snaps)))
            if segment > 2 and (abs(row['zoom']) > limits['jump_zoom']
                                or math.hypot(row['dx'], row['dy']) > limits['jump_shift']):
                row['flags'].append(('jump', 'camera', round(math.hypot(row['dx'], row['dy']), 1)))
            history.append(cumulative_vertical)
            if len(history) == 4:
                jerk = history[3] - 3 * history[2] + 3 * history[1] - history[0]
                row['jerk'] = jerk
                if abs(jerk) > limits['jerk']:
                    jerks.append(index)
            while jerks and jerks[0] <= index - window:
                jerks.popleft()
            if len(jerks) >= limits['jerk_frames']:
                row['flags'].append(('jerk', 'vertical', len(jerks)))
        # Frame content.
        if row['mean'] < limits['black_mean'] and row['std'] < limits['black_std']:
            row['flags'].append(('black', 'frame', round(row['mean'], 1)))
        elif row['std'] < limits['blank_std']:
            row['flags'].append(('blank', 'frame', round(row['mean'], 1)))
        if row['hf'] > limits['garbage_hf']:
            row['flags'].append(('garbage', 'noise', round(row['hf'], 1)))
        if row['checker'] >= limits['checker_hits']:
            row['flags'].append(('checkerboard', 'tiles', row['checker']))
        if row['pop'] >= limits['pop_area']:
            row['flags'].append(('popping', 'area', round(row['pop'], 2)))
        rows.append(row)
        if progress and index % 500 == 0:
            progress(index)
    neighbour_checks(rows, limits)
    return rows


def neighbour_checks(rows, limits):
    """Checks that need frames on both sides: flashes, HUD dropouts,
    partial frames, and frozen runs."""
    n = len(rows)
    for i in range(1, n - 1):
        a, b, c = rows[i - 1], rows[i], rows[i + 1]
        if (abs(a['mean'] - c['mean']) <= limits['flash_agree']
                and abs(b['mean'] - a['mean']) >= limits['flash']
                and abs(b['mean'] - c['mean']) >= limits['flash']):
            b['flags'].append(('flicker', 'brightness', round(b['mean'] - a['mean'], 1)))
    # HUD dropout: the band's bright text present, gone for at most three
    # frames, then back (appearing or changing for good is not a flicker).
    for region in range(len(rows[0]['hud']) if rows else 0):
        counts = [row['hud'][region] for row in rows]
        i = 1
        while i < n:
            normal = counts[i - 1]
            if normal >= limits['hud_min'] and counts[i] < limits['hud_drop'] * normal:
                back = next((j for j in range(i + 1, min(n, i + 4))
                             if counts[j] >= .7 * normal), None)
                if back is not None:
                    rows[i]['flags'].append(('flicker', ('hud-top', 'hud-bottom')[region],
                                             back - i))
                    i = back
            i += 1
    for i in range(n):
        lo, hi = max(0, i - 10), min(n, i + 11)
        around = sorted(rows[j]['flat'] for j in range(lo, hi) if j != i)
        if around and rows[i]['flat'] - around[len(around) // 2] >= limits['partial_flat']:
            rows[i]['flags'].append(('partial', 'flat-area', round(rows[i]['flat'], 2)))
    run_start = None
    for i in range(1, n + 1):
        frozen = i < n and rows[i]['diff'] == 0
        if frozen and run_start is None:
            run_start = i - 1
        if not frozen and run_start is not None:
            length = i - run_start
            if length >= limits['freeze_frames']:
                rows[run_start]['flags'].append(('frozen', 'identical', length))
            run_start = None


def events_from(rows, fps, merge_seconds=0.5):
    """Merge per-frame flags into events per (type, what)."""
    merge = max(1, int(round(merge_seconds * fps)))
    open_events, events = {}, []
    for row in rows:
        for kind, what, value in row['flags']:
            key = (kind, what)
            event = open_events.get(key)
            if event and row['frame'] - event['last'] <= merge:
                event['last'] = row['frame']
                event['frames'] += 1
                if (value if isinstance(value, (int, float)) else 0) > event['peak']:
                    event['peak'], event['peak_frame'] = value, row['frame']
                continue
            event = {'type': kind, 'what': what, 'first': row['frame'], 'last': row['frame'],
                     'frames': 1, 'peak': value, 'peak_frame': row['frame']}
            open_events[key] = event
            events.append(event)
    for event in events:
        if event['type'] == 'frozen':
            event['last'] = event['first'] + event['peak'] - 1
        event['start'] = round(event['first'] / fps, 3)
        event['end'] = round(event['last'] / fps, 3)
    return events


# -------------------------------------------------------------- output --
def write_zoom(video, event, out, nominal_fps, stem):
    """Eight frames around a blink, cropped to its box (plus a margin) and
    enlarged, for judging what blinked."""
    x0, y0, x1, y1 = event['box']
    w, h = min(480, max(64, (x1 - x0) + 48)), min(272, max(48, (y1 - y0) + 48))
    left = max(0, min(int((x0 + x1) / 2 - w / 2), 480 - w))
    top = max(0, min(int((y0 + y1) / 2 - h / 2), 272 - h))
    scale = max(1, min(4, 960 // (4 * w), 540 // (2 * h)))
    first = max(0, event['peak_frame'] - 2)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{first / nominal_fps:.4f}',
                    '-i', str(video), '-frames:v', '1', '-vf',
                    f'crop={w}:{h}:{left}:{top},scale={w * scale}:{h * scale}:flags=neighbor,'
                    'tile=4x2:padding=2:color=white', str(out / f'{stem}-zoom.png')], check=True)
    event['zoom_first'] = first


def write_media(video, event, out, fps, nominal_fps, encoder, index):
    """A clip around the event at the true rate, and a contact sheet."""
    centre = event['peak_frame']
    stem = f'{index:03d}-{event["type"]}-{event["what"]}-f{event["first"]}'
    first = max(0, event['first'] - int(fps))
    count = min(event['last'] - event['first'] + 1 + 2 * int(fps), int(8 * fps))
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{first / nominal_fps:.4f}',
                    '-i', str(video), '-frames:v', str(count),
                    '-vf', f'setpts=N/({fps:.4f}*TB),scale=480:272:flags=neighbor',
                    '-r', f'{fps:.4f}', '-c:v', encoder, '-pix_fmt', 'yuv420p']
                   + (['-crf', '18'] if encoder == 'libx264' else ['-q:v', '3'])
                   + [str(out / f'{stem}.mp4')], check=True)
    # Twelve consecutive frames, left to right, top to bottom, starting
    # five before the peak (named in the report).
    sheet_first = max(0, centre - 5)
    event['sheet_first'] = sheet_first
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{sheet_first / nominal_fps:.4f}',
                    '-i', str(video), '-frames:v', '1', '-vf',
                    'tile=4x3:padding=2:color=white', str(out / f'{stem}.png')], check=True)
    if 'box' in event:
        write_zoom(video, event, out, nominal_fps, stem)
    return stem


def find_recording(path):
    path = Path(path)
    if path.is_file():
        return path, None, None
    videos = sorted(path.glob('VIDEO/*.avi')) or sorted(path.glob('*.avi'))
    if not videos:
        raise SystemExit(f'no VIDEO/*.avi under {path}')
    audios = sorted(path.glob('AUDIO/*.wav')) or sorted(path.glob('*.wav'))
    log = path / 'tilefinch-validation.txt'
    return videos[-1], audios[-1] if audios else None, log if log.exists() else None


def parse_region(text):
    if not text:
        return None
    y0, y1, x0, x1 = (int(v) for v in text.split(','))
    return (y0, y1, x0, x1)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', help='recording directory or video file')
    parser.add_argument('--out', type=Path, required=True, help='output directory')
    parser.add_argument('--fps', type=float, help='true presented frame rate')
    parser.add_argument('--hud-top', default=','.join(map(str, DEFAULT_HUD_TOP)),
                        help='y0,y1,x0,x1 of the top HUD band in 240x136 pixels ("" disables)')
    parser.add_argument('--hud-bottom', default=','.join(map(str, DEFAULT_HUD_BOTTOM)),
                        help='y0,y1,x0,x1 of the bottom HUD band ("" disables)')
    parser.add_argument('--max-media', type=int, default=24,
                        help='clips and sheets to write (worst events first)')
    parser.add_argument('--skip', type=float, default=0.0,
                        help='ignore events in the first SECONDS (boot, menus)')
    parser.add_argument('--no-media', action='store_true')
    parser.add_argument('--motion-helper', type=Path, default=default_helper(),
                        help='tools/video_motion.c build (default: build-preset-release/'
                             'tilefinch-video-motion)')
    parser.add_argument('--no-camera', action='store_true',
                        help='skip the camera checks (no motion helper needed)')
    parser.add_argument('--blink-helper', type=Path, default=default_blink_helper(),
                        help='tools/video_blink.c build (default: build-preset-release/'
                             'tilefinch-video-blink)')
    parser.add_argument('--no-blink', action='store_true',
                        help='skip the blink check (no blink helper needed)')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args(argv)

    video, audio, _log = find_recording(args.input)
    info = probe(video)
    fps, fps_source = args.fps, 'given'
    if fps is None and audio is not None and info['frames']:
        seconds = audio_seconds(audio)
        if seconds > 1:
            fps, fps_source = info['frames'] / seconds, f'{info["frames"]} frames / {seconds:.2f} s audio'
    if fps is None:
        fps, fps_source = 30.0, 'default'
    args.out.mkdir(parents=True, exist_ok=True)
    say = (lambda *a: None) if args.quiet else (lambda *a: print(*a, file=sys.stderr))
    say(f'{video.name}: {info["frames"]} frames {info["width"]}x{info["height"]}, '
        f'{fps:.2f} fps ({fps_source})')
    hud = (parse_region(args.hud_top), parse_region(args.hud_bottom))
    helper = None
    if not args.no_camera:
        if not args.motion_helper.exists():
            raise SystemExit(f'missing {args.motion_helper}: build it with cmake --build '
                             'build-preset-release --target tilefinch-video-motion '
                             '(or pass --no-camera)')
        helper = MotionHelper(args.motion_helper)
    try:
        rows = analyze(frames_of(video), fps, hud=hud, helper=helper,
                       progress=lambda i: say(f'  frame {i}'))
    finally:
        if helper:
            helper.close()
    blink_boxes = {}
    if not args.no_blink:
        if not args.blink_helper.exists():
            raise SystemExit(f'missing {args.blink_helper}: build it with cmake --build '
                             'build-preset-release --target tilefinch-video-blink '
                             '(or pass --no-blink)')
        say('  blinks')
        size, lines = blink_lines(video, args.blink_helper)
        for first, kind, detail in classify_blinks(lines, size['width'], size['height'], hud,
                                                     None if args.no_camera else rows):
            if first < len(rows):
                rows[first]['flags'].append(('blink' if kind != 'shimmer' else 'shimmer',
                                             kind if kind != 'shimmer' else 'line',
                                             detail['pixels']))
                key = (first, kind if kind != 'shimmer' else 'line')
                if detail['pixels'] > blink_boxes.get(key, {'pixels': -1})['pixels']:
                    blink_boxes[key] = detail
    events = [e for e in events_from(rows, fps) if e['start'] >= args.skip]
    for event in events:
        detail = blink_boxes.get((event['peak_frame'], event['what']))
        if event['type'] in ('blink', 'shimmer') and detail:
            event.update(detail)

    with open(args.out / 'frames.tsv', 'w') as tsv:
        tsv.write('frame\tt\tmean\tstd\tdiff\tzoom\tdx\tdy\tmatch\tpop\tflat\thf\thud\thist\tcut\tflags\n')
        for row in rows:
            tsv.write(f'{row["frame"]}\t{row["t"]:.3f}\t{row["mean"]:.1f}\t{row["std"]:.1f}\t'
                      f'{row["diff"]:.2f}\t{row["zoom"]:.4f}\t{row["dx"]:.2f}\t{row["dy"]:.2f}\t'
                      f'{row["match"]:.2f}\t{row["pop"]:.2f}\t{row["flat"]:.2f}\t{row["hf"]:.1f}\t'
                      f'{"/".join(map(str, row["hud"]))}\t{row["hist"]:.3f}\t{int(row["cut"])}\t'
                      f'{",".join(k + ":" + w for k, w, _ in row["flags"])}\n')
    severity = {'oscillation': 0, 'pumping': 1, 'jerk': 2, 'transient': 3, 'garbage': 4,
                'black': 5, 'blank': 6, 'checkerboard': 7, 'partial': 8, 'popping': 9,
                'flicker': 10, 'blink': 11, 'frozen': 12, 'jump': 13, 'shimmer': 14}
    ranked = sorted(events, key=lambda e: (severity.get(e['type'], 99), -e['frames']))
    encoder = 'libx264' if b'libx264' in subprocess.run(
        ['ffmpeg', '-hide_banner', '-encoders'], capture_output=True).stdout else 'mpeg4'
    if not args.no_media:
        # One clip and sheet per moment: camera events that overlap one
        # already written share its media.
        written = []
        for event in ranked:
            family = ('camera' if event['type'] in CAMERA_TYPES
                      else (event['type'], event['what']) if event['type'] == 'blink'
                      else event['type'])
            shared = next((w for w in written if w[0] == family and event['first'] <= w[1]['last']
                           and w[1]['first'] <= event['last']), None)
            if shared:
                event['media'], event['sheet_first'] = shared[1]['media'], shared[1]['sheet_first']
                continue
            if len(written) >= args.max_media:
                continue
            event['media'] = write_media(video, event, args.out, fps, info['nominal_fps'],
                                         encoder, len(written))
            written.append((family, event))
    counts = collections.Counter(e['type'] for e in events)
    problems = sum(n for kind, n in counts.items() if kind not in INFO_TYPES)
    summary = {'video': str(video), 'frames': len(rows), 'fps': round(fps, 3), 'problems': problems,
               'fps_source': fps_source, 'seconds': round(len(rows) / fps, 2),
               'counts': dict(counts), 'events': events}
    (args.out / 'events.json').write_text(json.dumps(summary, indent=1))
    lines = [f'video: {video}', f'frames: {len(rows)} at {fps:.2f} fps ({fps_source}), '
             f'{len(rows) / fps:.1f} s',
             f'problems: {problems} (info only: {", ".join(sorted(INFO_TYPES))})',
             'events: ' + (', '.join(
                 f'{k} {v}' for k, v in sorted(counts.items())) or 'none')]
    for event in sorted(events, key=lambda e: e['first']):
        lines.append(f'{event["start"]:8.2f}-{event["end"]:<8.2f} frames {event["first"]}-'
                     f'{event["last"]} {event["type"]} {event["what"]} peak {event["peak"]} '
                     f'at frame {event["peak_frame"]}'
                     + (f' box {",".join(map(str, event["box"]))} run {event["run"]} '
                        f'{"appears" if event["appears"] else "vanishes"} rgb {event["on"]}'
                        if 'box' in event else '')
                     + (f' -> {event["media"]}.mp4, .png (frames {event["sheet_first"]}+)'
                        + (f', -zoom.png (frames {event["zoom_first"]}+)' if 'zoom_first' in event
                           else '') if 'media' in event else ''))
    (args.out / 'report.txt').write_text('\n'.join(lines) + '\n')
    if not args.quiet:
        print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
