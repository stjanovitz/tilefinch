#!/usr/bin/env python3
"""Reconcile macOS sample stacks with the isolated native-tier emission map.

Use --chain --sample-own to generate the map and sample that same process.
Shares are exclusive sampled PCs, not exact instruction elapsed times.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def report(probe_text, sample_text):
    bases = re.findall(r"region code-base=(0x[0-9a-fA-F]+) bytes=(\d+)", probe_text)
    if len(bases) != 1:
        raise ValueError("need exactly one emitted-code base from the sampled process")
    base, size = int(bases[0][0], 16), int(bases[0][1])
    ranges = [(int(a), int(b), op, phase) for a, b, op, phase in re.findall(
        r"region code-map begin=(\d+) end=(\d+) opcode=(\S+) phase=(\S+)", probe_text)]
    previous = 16  # The experiment leaves four nonexecuted prefix words.
    for start, end, _, _ in ranges:
        if start != previous or end <= start or end > size:
            raise ValueError("emission ranges must cover code exactly without overlap")
        previous = end
    if previous != size:
        raise ValueError("incomplete emission map")
    graph = sample_text.split("Call graph:\n", 1)[1].split("\nTotal number in stack", 1)[0]
    rows = []
    for line in graph.splitlines():
        match = re.match(r"([ +!:|]*)(\d+) (.*)", line)
        if match:
            rows.append([len(match[1]), int(match[2]), match[3], int(match[2])])
    if not rows:
        raise ValueError("no sampled stack rows")
    parents, root_samples = [], 0
    for index, row in enumerate(rows):
        while parents and rows[parents[-1]][0] >= row[0]:
            parents.pop()
        if parents:
            rows[parents[-1]][3] -= row[1]
        else:
            root_samples += row[1]
        parents.append(index)
    phases, opcodes = Counter(), Counter()
    mapped = 0
    for _, _, name, exclusive in rows:
        if exclusive < 0:
            raise ValueError("child samples exceed their parent; unsupported stack format")
        if not exclusive:
            continue
        if name.startswith("???"):
            address = re.search(r"\[(0x[0-9a-fA-F]+)\]", name)
            offset = int(address[1], 16) - base if address else -1
            matching = [r for r in ranges if r[0] <= offset < r[1]]
            if len(matching) != 1:
                raise ValueError("unmapped native sample; verify the map and sample are from the same process")
            _, _, opcode, phase = matching[0]
            phases["emitted:" + phase] += exclusive
            opcodes[opcode + ":" + phase] += exclusive
            mapped += exclusive
        else:
            phases[name.split("  (", 1)[0]] += exclusive
    if sum(phases.values()) != root_samples:
        raise ValueError("exclusive samples do not reconcile to root samples")
    return {"samples": root_samples, "mapped_emitted_samples": mapped,
            "exclusive": dict(phases.most_common()),
            "emitted_by_opcode": dict(opcodes.most_common())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_log", type=Path)
    parser.add_argument("sample_log", type=Path)
    args = parser.parse_args()
    try:
        result = report(args.probe_log.read_text(), args.sample_log.read_text())
    except (ValueError, IndexError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
