"""Bounded, offline native five-view capture and fail-closed receipts.

The views use the lab's native navigation, which remains valid after page
JavaScript retires. Fractions describe native geometry, not matching content
anchors in another renderer. A stale height estimate is an explicit failure.
"""
from __future__ import annotations

import hashlib
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
from visual_scenario import trace_digest

VIEWS = (0, 25, 50, 75, 100)
MAX_LOG_BYTES = 64 * 1024 * 1024
MAX_COMMANDS = 4096
POSITION = re.compile(r'^loop frame=(\d+) .* scroll=(\d+)/(\d+) .+$')
MARKER = re.compile(r'^tilefinch-work: label=native-view-(\d+)(?: |$)')
TEARDOWN = re.compile(
    r'^interactive teardown=(\d+) active=(\d+) largest=(\d+) peak=(\d+) '
    r'allocations=(\d+) frees=(\d+) failures=(\d+) status=(PASS|FAIL)$')


def _input_identity(arguments: list[str]) -> dict:
    if arguments.count('--replay-http-response-keyed') != 1:
        raise ValueError('native capture needs one unambiguous offline trace')
    at = arguments.index('--replay-http-response-keyed')
    if at + 1 >= len(arguments):
        raise ValueError('native capture is missing its offline trace path')
    trace = Path(arguments[at + 1])
    entries = total = 0
    for current, directories, names in os.walk(trace, followlinks=False):
        entries += len(directories) + len(names)
        if entries > 8192:
            raise ValueError('native trace entry cap exceeded')
        for name in names:
            total += Path(current, name).stat().st_size
            if total > 512 * 1024 * 1024:
                raise ValueError('native trace byte cap exceeded')
    lab = Path(arguments[0]).resolve()
    # Host builds can load the engine from a sibling shared library. The
    # executable digest alone then stays unchanged across engine rebuilds.
    # Include every supported sibling spelling; static builds have none.
    engine_images = {}
    for name in ('libtilefinch_core.dylib', 'libtilefinch_core.so',
                 'tilefinch_core.dll', 'libtilefinch_core.dll'):
        image = lab.parent / name
        if image.exists() or image.is_symlink():
            engine_images[name] = digest(read_bounded(image.resolve()))
    return {'lab_sha256': digest(read_bounded(lab)),
            'engine_images_sha256': engine_images,
            'trace_sha256': trace_digest(trace)}


class StaleHeightError(ValueError):
    def __init__(self, height: int, artifacts: dict | None = None):
        super().__init__(f'stale native height estimate; actual height={height}')
        self.height = height
        self.artifacts = artifacts


def read_bounded(path: Path, maximum: int = MAX_LOG_BYTES) -> bytes:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError(f'not a bounded regular capture artifact: {path.name}')
    data = path.read_bytes()
    if len(data) > maximum:
        raise ValueError(f'capture artifact grew beyond its bound: {path.name}')
    return data


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_five_view_commands(output: Path, height_estimate: int,
                             sweep_ticks: int = 0, tick_ms: int = 33,
                             viewport_height: int = 272) -> str:
    """Sweep on the recorded clock, then position via the native facade."""
    if (not viewport_height <= height_estimate <= 61440
            or not 0 <= sweep_ticks <= 256 or not 1 <= tick_ms <= 1000):
        raise ValueError('native capture geometry/clock exceeds its bound')
    if '\n' in str(output) or '\r' in str(output):
        raise ValueError('capture output path contains a command separator')
    lines = ['drain 4096 0', 'idle 300', 'status', 'top']
    for _ in range(sweep_ticks):
        lines.extend(['page-down', f'tick 1 {tick_ms}', 'drain 2048 0'])
    for percentage in VIEWS:
        if percentage == 100:
            lines.append('bottom')
        else:
            lines.append('top')
            target = ((height_estimate - viewport_height) * percentage + 50) // 100
            lines.append(f'scroll-by {target}')
        lines.extend(['drain 4096 0', 'idle 300', 'status',
                      f'work native-view-{percentage}',
                      f'render {output / ("view-" + str(percentage) + ".ppm")}'])
    lines.extend(['census', 'script-report', 'quit'])
    if len(lines) > MAX_COMMANDS:
        raise ValueError('native capture command cap exceeded')
    return '\n'.join(lines) + '\n'


def validate_native_capture(output: Path, exit_code: int,
                            viewport_width: int = 480,
                            viewport_height: int = 272) -> dict:
    """Require explicit ordered markers, stable real offsets and clean exit.

    No fallback infers positions from an unlabelled, truncated or merged log.
    At each marker the preceding status and the next two statuses (work and
    render) must agree, so a relayout between positioning and pixels is caught.
    """
    if exit_code != 0:
        raise ValueError(f'native capture process exited {exit_code}')
    commands = read_bounded(output / 'commands.txt', 1024 * 1024)
    stdout = read_bounded(output / 'stdout.log')
    stderr = read_bounded(output / 'stderr.log')
    if (b'loop command-failed=' in stderr or b'loop unknown-command=' in stderr
            or b'loop frame=' in stderr or b'tilefinch-work: label=native-view-' in stderr
            or b'interactive teardown=' in stderr):
        raise ValueError('native capture has failed commands or mixed evidence streams')
    lines = commands.decode('utf-8').splitlines()
    if not lines or lines[-1] != 'quit' or len(lines) > MAX_COMMANDS:
        raise ValueError('native capture commands are incomplete or oversized')
    allowed = {'drain', 'idle', 'status', 'top', 'bottom', 'scroll-by',
               'page-down', 'tick', 'work', 'render', 'census', 'script-report', 'quit'}
    if any(c.split(' ', 1)[0] not in allowed for c in lines):
        raise ValueError('native capture positioning must not depend on page JS')
    if any(c.startswith('drain ') and c.split()[-1] != '0' for c in lines):
        raise ValueError('native capture drains advanced unrecorded virtual time')
    labels = [c for c in lines if c.startswith('work native-view-')]
    if labels != [f'work native-view-{p}' for p in VIEWS]:
        raise ValueError('native capture command labels are missing or reordered')
    for percentage in VIEWS:
        at = lines.index(f'work native-view-{percentage}')
        if (at < 1 or lines[at - 1] != 'status' or at + 1 >= len(lines)
                or lines[at + 1] != f'render {output / ("view-" + str(percentage) + ".ppm")}'):
            raise ValueError('native marker is not bound to its status/render command')
    positions = []
    last_position = None
    status_serial = 0
    previous_marker_serial = -3
    last_frame = -1
    pending = None
    following = 0
    teardown = []
    for line in stdout.decode('utf-8').splitlines():
        match = POSITION.fullmatch(line)
        if line.startswith('loop frame=') and match is None:
            raise ValueError('native status line is truncated or interleaved')
        if match:
            status_serial += 1
            position = {'frame': int(match[1]), 'y': int(match[2]),
                        'maximum': int(match[3])}
            if position['frame'] < last_frame:
                raise ValueError('native status frames are reordered')
            last_frame = position['frame']
            if position['y'] > position['maximum']:
                raise ValueError('native offset exceeds its actual maximum')
            if pending is not None and following < 2:
                if (position['y'], position['maximum']) != (pending['y'], pending['maximum']):
                    raise ValueError('native view moved between marker and render')
                following += 1
            last_position = position
        marker = MARKER.match(line)
        if 'label=native-view-' in line and marker is None:
            raise ValueError('native marker is truncated or interleaved')
        if marker:
            if (last_position is None or status_serial - previous_marker_serial < 3
                    or (pending is not None and following != 2)):
                raise ValueError('native marker reuses stale or missing status evidence')
            percentage = int(marker[1])
            if len(positions) >= 5 or percentage != VIEWS[len(positions)]:
                raise ValueError('native view labels are missing, duplicated or reordered')
            pending = dict(last_position, percentage=percentage)
            positions.append(pending)
            following = 0
            previous_marker_serial = status_serial
        receipt = TEARDOWN.fullmatch(line)
        if line.startswith('interactive teardown=') and receipt is None:
            raise ValueError('native teardown receipt is truncated or interleaved')
        if receipt:
            teardown.append(receipt)
    if len(positions) != 5 or following != 2:
        raise ValueError('native capture lacks all five completed labelled renders')
    if (len(teardown) != 1 or teardown[0][8] != 'PASS'
            or any(int(teardown[0][i]) != 0 for i in (1, 2, 3))):
        raise ValueError('native capture lacks a unique clean teardown receipt')
    maximum = positions[-1]['maximum']
    if any(p['maximum'] != maximum for p in positions):
        raise ValueError('native document geometry changed between views')
    if positions[0]['y'] != 0 or positions[-1]['y'] != maximum:
        raise ValueError('native top/bottom offsets do not reach both document edges')
    if any(a['y'] > b['y'] for a, b in zip(positions, positions[1:])):
        raise ValueError('native actual offsets are reordered')
    hashes = {'commands.txt': digest(commands), 'stdout.log': digest(stdout),
              'stderr.log': digest(stderr)}
    invocation = read_bounded(output / 'invocation.json', 1024 * 1024)
    if json.loads(invocation).get('exit') != exit_code:
        raise ValueError('native invocation exit receipt disagrees with the process')
    hashes['invocation.json'] = digest(invocation)
    for percentage in VIEWS:
        name = f'view-{percentage}.ppm'
        frame = read_bounded(output / name, 2 * 1024 * 1024)
        header = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\n', frame)
        if (header is None or int(header[1]) != viewport_width
                or int(header[2]) != viewport_height
                or len(frame) - header.end() != viewport_width * viewport_height * 3):
            raise ValueError(f'native frame is truncated or has wrong dimensions: {name}')
        hashes[name] = digest(frame)
    for p in positions:
        expected = (maximum * p['percentage'] + 50) // 100
        if p['y'] != expected:
            raise StaleHeightError(maximum + viewport_height, hashes)
    return {'schema': 'tilefinch-native-five-view-v1',
            'scope': 'diagnostic native fractions, not a cross-renderer pixel oracle',
            'resource_completion': 'not-qualified-by-this-diagnostic',
            'method': 'native top/bottom and exact bounded scroll-by offsets',
            'positions': positions, 'top_verified': True, 'bottom_verified': True,
            'zero_elapsed_drains': True, 'teardown_bytes': 0,
            'document_height': maximum + viewport_height,
            'artifacts': hashes}


def _run_attempt(arguments: list[str], output: Path, height_estimate: int,
                 sweep_ticks: int, tick_ms: int, timeout_seconds: int) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    commands = output / 'commands.txt'
    commands.write_text(build_five_view_commands(output, height_estimate, sweep_ticks, tick_ms))
    command = arguments + ['--viewport-css-width', '480', '--viewport-css-height', '272',
                           '--commands', str(commands), '--loop-output-dir', str(output / 'frames'),
                           '--no-loop-capture']
    with (output / 'stdout.log').open('wb') as out, (output / 'stderr.log').open('wb') as err:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out,
                                   stderr=err, start_new_session=True)
        try:
            code = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=10)
            code = 124
    (output / 'invocation.json').write_text(json.dumps(
        {'args': command, 'exit': code, 'height_estimate': height_estimate,
         'extra_clock_turns': sweep_ticks, 'tick_ms': tick_ms}, indent=2) + '\n')
    return validate_native_capture(output, code)


def run_native_capture(arguments: list[str], output: Path, height_estimate: int,
                       sweep_ticks: int = 0, tick_ms: int = 33,
                       timeout_seconds: int = 180) -> dict:
    """Run at most two fresh offline attempts; return a receipt or raise.

    The caller supplies ordinary lab rendering policy and its pinned trace.
    Only a stale intermediate height estimate permits a second attempt, using
    the first attempt's validated final geometry. Both attempts are preserved;
    changed final geometry on retry is refused. The clock sequence is repeated
    from fresh state, never accumulated or guessed from status counts.
    Existing output is never overwritten. The response-keyed replay switch is
    required; this helper neither acquires resources nor permits live fallback.
    """
    if '--replay-http-response-keyed' not in arguments:
        raise ValueError('native capture requires offline response-keyed replay')
    if any(flag in arguments for flag in ('--commands', '--loop-output-dir', '--dump-text-metrics')):
        raise ValueError('native capture owns its command and output paths')
    if not 1 <= timeout_seconds <= 600:
        raise ValueError('native capture timeout exceeds its bound')
    identity = _input_identity(arguments)
    output.mkdir(parents=True, exist_ok=False)
    attempts = []
    known_height = None
    for number in range(2):
        if _input_identity(arguments) != identity:
            raise ValueError('native capture inputs changed before the attempt')
        destination = output / f'attempt-{number + 1}'
        try:
            try:
                receipt = _run_attempt(arguments, destination, height_estimate,
                                       sweep_ticks, tick_ms, timeout_seconds)
            finally:
                if _input_identity(arguments) != identity:
                    raise ValueError('native capture inputs changed during the attempt')
        except StaleHeightError as error:
            attempts.append({'attempt': destination.name, 'status': 'stale-height',
                             'height_estimate': height_estimate, 'actual_height': error.height,
                             'artifacts': error.artifacts or {name: digest(read_bounded(destination / name))
                                           for name in ('commands.txt', 'stdout.log', 'stderr.log')}})
            (output / 'attempts.json').write_text(json.dumps(attempts, indent=2) + '\n')
            if number != 0:
                raise
            height_estimate = known_height = error.height
            continue
        if known_height is not None and receipt['document_height'] != known_height:
            raise ValueError('native document height changed across fresh attempts')
        attempts.append({'attempt': destination.name, 'status': 'complete',
                         'height_estimate': height_estimate, 'artifacts': receipt['artifacts']})
        receipt.update(identity, exit=0, attempts=attempts,
                       extra_clock_turns=sweep_ticks, tick_ms=tick_ms)
        (output / 'native-capture.json').write_text(json.dumps(receipt, indent=2) + '\n')
        return receipt
    raise ValueError('native capture exhausted its bounded fresh attempts')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--height-estimate', type=int, required=True)
    parser.add_argument('--sweep-ticks', type=int, default=0)
    parser.add_argument('--tick-ms', type=int, default=33)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('lab_arguments', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    arguments = options.lab_arguments
    if arguments and arguments[0] == '--':
        arguments = arguments[1:]
    if not arguments:
        parser.error('supply the lab executable and keyed replay arguments after --')
    try:
        result = run_native_capture(arguments, options.output, options.height_estimate,
                                    options.sweep_ticks, options.tick_ms, options.timeout)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f'native diagnostic capture refused: {error}\n')
    print(json.dumps(result, indent=2))
