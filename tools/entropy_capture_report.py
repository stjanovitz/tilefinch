#!/usr/bin/env python3
"""Looks for structure in raw PSP entropy samples (TFER captures).

A validation build records the timing samples its entropy pool gathers, in
measured order, to host0: (src/psp_entropy_capture.h has the format and
src/psp_app/psp_app_validation_entropy.c the recorder). The pool's own
estimator sees one batch at a time and predicts at lags 1..64 at most. A
work sample is a few microseconds read on a 1 us clock, so if its duration
and the gap between samples are fixed, the sample is the beat of two clocks:
deterministic, periodic at some lag, and still able to look like jitter
within one batch. This reads one or more captures (several boots) and, for
each source, reports:

- repeat rate at every lag 1..--max-lag against what independent samples with
  the same value frequencies would give (a beat repeats far more often at its
  period and its multiples);
- autocorrelation at the same lags (winsorized values);
- per-sample min-entropy from three predictors in the style of NIST SP
  800-90B section 6.3 (most common value, best single lag, first-order
  Markov), each with its 99% upper confidence bound. These are screening
  numbers, not the full 90B non-IID suite.

A lag past 8 is flagged when its repeat rate or autocorrelation is beyond
the Bonferroni-corrected two-sided threshold (alpha 0.001 over all lags
tested). Batches the pool took and runs taken only for the capture are
separate streams; pairs never span two batches or runs.

    tools/entropy_capture_report.py capture-1.tfer [capture-2.tfer ...]
        [--max-lag 256] [--json out.json]

Pure Python; a few seconds per hundred thousand samples.
"""

from __future__ import annotations

import argparse
import cmath
import json
import math
import struct
import sys
from collections import Counter
from pathlib import Path
from statistics import NormalDist

HEADER = struct.Struct("<4sHHHHIIIIIII")
RECORD = struct.Struct("<HHIIIIIIIQ")
SOURCES = {1: "work", 2: "sleep"}
PHASES = {1: "fill", 2: "prepare", 3: "extra"}
NOT_ESTIMATED = 0xFFFFFFFF
Z99 = 2.576
# A lag with fewer pairs (the last few lags of single batches) says little
# and would only widen the best-lag bound.
MIN_PAIRS = 128


class CaptureError(ValueError):
    pass


def read_capture(path):
    data = Path(path).read_bytes()
    if len(data) < 40:
        raise CaptureError(f"{path}: shorter than a TFER header")
    (magic, version, header_bytes, record_bytes, flags, records, dropped,
     work_batch, sleep_batch, work_touches, sleep_us,
     clock_hz) = HEADER.unpack_from(data, 0)
    if magic != b"TFER" or version != 1 or header_bytes != 40 \
            or record_bytes != 40:
        raise CaptureError(f"{path}: not a TFER version 1 capture")
    header = {"flags": flags, "records": records, "dropped": dropped,
              "work_batch": work_batch, "sleep_batch": sleep_batch,
              "work_touches": work_touches, "sleep_us": sleep_us,
              "clock_hz": clock_hz}
    cursor = header_bytes
    batches = []
    for _ in range(records):
        if cursor + record_bytes > len(data):
            raise CaptureError(f"{path}: truncated record")
        (source, phase, index, count, millibits, credited, pool_bits,
         batch_us, _reserved, start_us) = RECORD.unpack_from(data, cursor)
        cursor += record_bytes
        if source not in SOURCES or phase not in PHASES:
            raise CaptureError(f"{path}: record {index} has source {source} "
                               f"phase {phase}")
        end = cursor + 4 * count
        if end > len(data):
            raise CaptureError(f"{path}: record {index} runs past the end")
        samples = list(struct.unpack_from(f"<{count}I", data, cursor))
        cursor = end
        batches.append({"source": SOURCES[source], "phase": PHASES[phase],
                        "index": index, "count": count,
                        "millibits": millibits, "credited": credited,
                        "pool_bits": pool_bits, "batch_us": batch_us,
                        "start_us": start_us, "samples": samples})
    if cursor != len(data):
        raise CaptureError(f"{path}: {len(data) - cursor} trailing bytes")
    return header, batches


def upper_bound(hits, trials):
    if trials <= 1:
        return 1.0
    p = hits / trials
    return min(1.0, p + Z99 * math.sqrt(p * (1.0 - p) / (trials - 1)))


def bits(probability):
    return 0.0 if probability >= 1.0 else -math.log2(probability)


def codes_for(runs):
    """Frequency-ranked byte codes; the rarest values beyond 255 share 255,
    which only ever makes the IID expectation and the observation agree
    more (both are computed on the codes)."""
    counts = Counter(value for run in runs for value in run)
    ranked = [value for value, _ in counts.most_common()]
    table = {value: min(rank, 255) for rank, value in enumerate(ranked)}
    coded = [bytes(table[value] for value in run) for run in runs]
    code_counts = Counter()
    for value, count in counts.items():
        code_counts[table[value]] += count
    return coded, counts, code_counts


def repeats_at(coded, lag):
    hits = pairs = 0
    for run in coded:
        if len(run) <= lag:
            continue
        span = len(run) - lag
        left = int.from_bytes(run[:span], "little")
        right = int.from_bytes(run[lag:], "little")
        hits += (left ^ right).to_bytes(span, "little").count(0)
        pairs += span
    return hits, pairs


def fft(values, inverse=False):
    size = len(values)
    out = list(values)
    j = 0
    for i in range(1, size):
        bit = size >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            out[i], out[j] = out[j], out[i]
    length = 2
    sign = 1 if inverse else -1
    while length <= size:
        step = cmath.exp(sign * 2j * math.pi / length)
        half = length // 2
        twiddles = [1 + 0j]
        for _ in range(half - 1):
            twiddles.append(twiddles[-1] * step)
        for start in range(0, size, length):
            for k in range(half):
                a = out[start + k]
                b = out[start + k + half] * twiddles[k]
                out[start + k] = a + b
                out[start + k + half] = a - b
        length <<= 1
    if inverse:
        out = [value / size for value in out]
    return out


def autocorrelation(runs, max_lag):
    """r(lag) for lag 1..max_lag over pairs within runs, on values
    winsorized to the 0.5%..99.5% range so one preempted sleep cannot set
    the scale. None when the stream is constant."""
    flat = sorted(value for run in runs for value in run)
    low = flat[int(0.005 * (len(flat) - 1))]
    high = flat[int(0.995 * (len(flat) - 1))]
    clipped = [[min(max(value, low), high) for value in run] for run in runs]
    total = sum(len(run) for run in clipped)
    mean = sum(sum(run) for run in clipped) / total
    variance = sum((value - mean) ** 2 for run in clipped
                   for value in run) / total
    if variance == 0.0:
        return None
    sequence = []
    for run in clipped:
        sequence.extend(value - mean for value in run)
        sequence.extend([0.0] * max_lag)
    size = 1
    while size < 2 * len(sequence):
        size <<= 1
    spectrum = fft(sequence + [0.0] * (size - len(sequence)))
    power = fft([value * value.conjugate() for value in spectrum],
                inverse=True)
    result = {}
    for lag in range(1, max_lag + 1):
        pairs = sum(max(0, len(run) - lag) for run in clipped)
        if pairs:
            result[lag] = (power[lag].real / pairs / variance, pairs)
    return result


def markov_hits(coded):
    successors = {}
    for run in coded:
        for previous, current in zip(run, run[1:]):
            successors.setdefault(previous, Counter())[current] += 1
    hits = sum(counter.most_common(1)[0][1]
               for counter in successors.values())
    trials = sum(sum(counter.values()) for counter in successors.values())
    return hits, trials


def analyse_stream(runs, max_lag, threshold):
    coded, counts, code_counts = codes_for(runs)
    total = sum(len(run) for run in runs)
    collision = sum((count / total) ** 2 for count in code_counts.values())
    triple = sum((count / total) ** 3 for count in code_counts.values())
    # Pairs (i, i+lag) and (i+lag, i+2 lag) share a sample, so independent
    # samples' repeat count varies by more than a binomial's.
    per_pair = collision * (1.0 - collision) + 2.0 * (triple - collision ** 2)
    most = counts.most_common(1)[0][1]
    mcv = bits(upper_bound(most, total))
    lags = []
    best = None
    acf = autocorrelation(runs, max_lag)
    for lag in range(1, max_lag + 1):
        hits, pairs = repeats_at(coded, lag)
        if pairs < MIN_PAIRS:
            continue
        rate = hits / pairs
        spread = math.sqrt(pairs * per_pair)
        z_repeat = (hits - collision * pairs) / spread if spread else 0.0
        bound = upper_bound(hits, pairs)
        if best is None or bound > best[0]:
            best = (bound, lag, rate)
        r, z_acf = (None, 0.0)
        if acf is not None and lag in acf:
            r = acf[lag][0]
            z_acf = r * math.sqrt(acf[lag][1])
        lags.append({"lag": lag, "pairs": pairs, "repeat": rate,
                     "expected": collision, "z_repeat": z_repeat,
                     "acf": r, "z_acf": z_acf,
                     "flagged": z_repeat > threshold
                     or abs(z_acf) > threshold})
    hits, trials = markov_hits(coded)
    markov = bits(upper_bound(hits, trials)) if trials else 0.0
    long_flagged = [entry for entry in lags
                    if entry["lag"] > 8 and entry["flagged"]]
    short_flagged = [entry for entry in lags
                     if entry["lag"] <= 8 and entry["flagged"]]
    strongest = sorted(long_flagged, key=lambda entry: -max(
        entry["z_repeat"], abs(entry["z_acf"])))
    periodic = [entry["lag"] for entry in long_flagged
                if entry["repeat"] >= 0.9]
    return {
        "samples": total,
        "runs": len(runs),
        "distinct": len(counts),
        "mean": sum(value * count for value, count in counts.items()) / total,
        "top_values": [[value, count] for value, count
                       in counts.most_common(6)],
        "collision": collision,
        "mcv_bits": mcv,
        "best_lag": best[1] if best else None,
        "best_lag_repeat": best[2] if best else None,
        "best_lag_bits": bits(best[0]) if best else None,
        "markov_bits": markov,
        "min_bits": min(mcv, markov, *([bits(best[0])] if best else [])),
        "short_lags_flagged": [entry["lag"] for entry in short_flagged],
        "long_lags_flagged": len(long_flagged),
        "strongest_long_lags": strongest[:10],
        "near_periodic_lags": periodic[:20],
        "structure_beyond_lag_8": bool(long_flagged),
    }


def pool_summary(batches):
    summary = {}
    for source in ("work", "sleep"):
        estimated = [batch for batch in batches
                     if batch["source"] == source
                     and batch["millibits"] != NOT_ESTIMATED]
        if not estimated:
            continue
        summary[source] = {
            "batches": len(estimated),
            "credited_bits": sum(batch["credited"] for batch in estimated),
            "millibits_min": min(batch["millibits"] for batch in estimated),
            "millibits_max": max(batch["millibits"] for batch in estimated),
        }
    return summary


def report(paths, max_lag):
    tested = max_lag
    threshold = NormalDist().inv_cdf(1.0 - 0.001 / (2 * tested))
    files = []
    streams = {}
    all_batches = []
    for path in paths:
        header, batches = read_capture(path)
        files.append({"path": str(path), **header})
        all_batches.extend(batches)
        for batch in batches:
            origin = "extra" if batch["phase"] == "extra" else "pool"
            streams.setdefault(f"{batch['source']}/{origin}", []).append(
                batch["samples"])
    analysed = {name: analyse_stream(runs, max_lag, threshold)
                for name, runs in sorted(streams.items())
                if sum(len(run) for run in runs) >= 64}
    return {
        "files": files,
        "max_lag": max_lag,
        "z_threshold": threshold,
        "pool": pool_summary(all_batches),
        "streams": analysed,
        "structure_beyond_lag_8": sorted(
            name for name, stream in analysed.items()
            if stream["structure_beyond_lag_8"]),
    }


def print_report(result):
    for entry in result["files"]:
        dropped = (f", {entry['dropped']} records DROPPED"
                   if entry["dropped"] else "")
        print(f"{entry['path']}: {entry['records']} records{dropped}; "
              f"work batch {entry['work_batch']} x {entry['work_touches']} "
              f"touches, sleep batch {entry['sleep_batch']} x "
              f"{entry['sleep_us']} us")
    for source, pool in result["pool"].items():
        print(f"pool {source}: {pool['batches']} batches credited "
              f"{pool['credited_bits']} bits, estimate "
              f"{pool['millibits_min']}..{pool['millibits_max']} mbit/sample")
    print(f"lags 1..{result['max_lag']}, flag threshold |z| > "
          f"{result['z_threshold']:.2f}")
    for name, stream in result["streams"].items():
        print(f"\n{name}: {stream['samples']} samples in {stream['runs']} "
              f"runs, {stream['distinct']} values, mean "
              f"{stream['mean']:.3f} us")
        print("  most common: " + ", ".join(
            f"{value} us x{count}" for value, count in stream["top_values"]))
        best_lag = ("best lag (too few pairs)"
                    if stream["best_lag"] is None else
                    f"best lag ({stream['best_lag']}, repeat "
                    f"{stream['best_lag_repeat']:.3f}) "
                    f"{stream['best_lag_bits']:.3f}")
        print(f"  min-entropy per sample: most-common {stream['mcv_bits']:.3f}"
              f", {best_lag}, Markov {stream['markov_bits']:.3f} -> "
              f"{stream['min_bits']:.3f} bits")
        if stream["short_lags_flagged"]:
            print("  lags 1..8 beyond threshold: "
                  + ", ".join(map(str, stream["short_lags_flagged"])))
        if not stream["structure_beyond_lag_8"]:
            print("  no lag past 8 beyond threshold")
            continue
        print(f"  STRUCTURE: {stream['long_lags_flagged']} lags past 8 beyond "
              f"threshold; strongest:")
        for entry in stream["strongest_long_lags"][:6]:
            acf = "-" if entry["acf"] is None else f"{entry['acf']:+.3f}"
            print(f"    lag {entry['lag']:3d}: repeat {entry['repeat']:.3f} vs "
                  f"{entry['expected']:.3f} (z {entry['z_repeat']:.1f}), "
                  f"acf {acf} (z {entry['z_acf']:.1f})")
        if stream["near_periodic_lags"]:
            print("  repeat >= 0.9 at lags: "
                  + ", ".join(map(str, stream["near_periodic_lags"]))
                  + " (a beat period and its multiples)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--max-lag", type=int, default=256)
    parser.add_argument("--json", type=Path,
                        help="also write the full result as JSON")
    arguments = parser.parse_args()
    if not 9 <= arguments.max_lag <= 4096:
        parser.error("--max-lag must be 9..4096")
    try:
        result = report(arguments.captures, arguments.max_lag)
    except (OSError, CaptureError) as error:
        print(f"entropy_capture_report: {error}", file=sys.stderr)
        return 1
    print_report(result)
    if arguments.json is not None:
        arguments.json.write_text(json.dumps(result, indent=1) + "\n",
                                  encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
