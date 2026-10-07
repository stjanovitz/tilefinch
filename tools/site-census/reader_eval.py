#!/usr/bin/env python3
"""Automatic Reader precision and recall against reader-labels.tsv.

usage: reader_eval.py CENSUS.jsonl [CENSUS.jsonl ...]

A page is predicted an article when the census Reader analysis reports a
high-confidence non-raw kind (fallback.auto_reader_candidate). False
positives matter more than misses: automatic Reader hides navigation.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def labels():
    out = {}
    for line in open(os.path.join(HERE, "reader-labels.tsv")):
        if line.strip() and not line.startswith("#"):
            name, label = line.rstrip("\n").split("\t")
            out[name] = label
    return out


def main():
    wanted = labels()
    for path in sys.argv[1:]:
        records = {}
        for line in open(path):
            rec = json.loads(line)
            records[rec["name"]] = rec
        tp, fp, fn = [], [], []
        for name, label in sorted(wanted.items()):
            rec = records.get(name)
            predicted = bool(rec and rec.get("fallback", {})
                             .get("auto_reader_candidate"))
            if predicted and label == "article":
                tp.append(name)
            elif predicted:
                fp.append(name)
            elif label == "article":
                fn.append(name)
        precision = "%d/%d" % (len(tp), len(tp) + len(fp)) if tp or fp else "-"
        print("%s: precision %s, recall %d/%d" % (
            path, precision, len(tp), len(tp) + len(fn)))
        print("  true positives: %s" % (", ".join(tp) or "none"))
        print("  false positives: %s" % (", ".join(fp) or "none"))
        print("  misses: %s" % (", ".join(fn) or "none"))


if __name__ == "__main__":
    main()
