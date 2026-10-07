#!/usr/bin/env python3
"""What caching compiled lazy webpack factories would hold and save.

Reads replay runs made with TILEFINCH_TRACE_FACTORY_CACHE=1 (and the census
ledger, TILEFINCH_TRACE_CENSUS=1, which census.py always sets):

  census-lazy-plan          planner time per webpack-shaped script
  census-lazy-bundle        per lazy bundle: SHA-256 prefix, factories,
                            syntax preflight, source store, registration
  census-lazy-factory       per factory compile: source bytes, compile time,
                            serialized bytecode at compile ("fresh")
  census-lazy-factory-run   after its first call: serialized bytecode
                            ("run", bodies compiled during the call
                            included) and the first-call compiles it caused
  census-bytecode-lookup/store, census-js   the classic table and compiles

  factory_cache.py pages RUN_DIR [PAGE...]
  factory_cache.py pairs RUN_DIR A:B [A:B...] [--table KIB] [--json OUT]

RUN_DIR is a replay output directory (census.py replay --idle 300, so the
classic stores happen); NAME.rN repeats are folded in, timings by minimum.
`pairs` simulates a session that visits A and then B with the classic table
at its ceiling and each factory-cache design; A:A is a same-page revisit.
PSP times are host times scaled by the calibration constants below.
"""
import collections
import json
import os
import re
import sys

FIELD = re.compile(r"([\w-]+)=(\S*)")
KIB = 1024.0
# Host -> PSP. QuickJS compile: 108x (PERFORMANCE_LEDGER, compile_peak at
# 111 MHz against the M-series host). Native C code (the planner, LZ store,
# SHA-256): 90x, inside the 60-110x used for C elsewhere (read planner
# figures as +-30%); it puts host SHA-256 near the 0.35 s/MiB estimate.
COMPILE_X = 108.0
NATIVE_X = 90.0
# A first-call compile of a lazily kept function body runs the full
# compiler, not the preparse that the factory compile and the preflight run;
# wapo-home measured 2.3x the preparse rate per byte with lazy functions off.
BODY_RATE_LOW, BODY_RATE_HIGH = 1.0, 2.3
SHA_S_PER_MIB = 0.35            # SHA-256 on the PSP (estimate)
CLASSIC_ENTRIES = 128


def fields(line, cut=None):
    if cut is not None:
        line = line.split(cut, 1)[0]
    return {k: v for k, v in FIELD.findall(line)}


def num(d, key):
    try:
        return int(d.get(key, 0))
    except ValueError:
        return 0


class Page:
    """One page's first visit, folded over its repeats."""

    def __init__(self, name):
        self.name = name
        self.bundles = collections.OrderedDict()   # digest -> dict
        self.factories = collections.OrderedDict()  # (digest, index) -> dict
        self.events = []        # B-side order: ("classic"|"factory", key)
        self.classic = {}       # (url, ordinal) -> dict(bytes, compile_us)
        self.stores = []        # classic stores in order: (key, bytecode)
        self.plans_us = 0
        self.lazy_seconds = None

    # Timings: minimum per item over the repeats.
    def _min(self, d, key, value):
        d[key] = value if key not in d else min(d[key], value)


def parse_log(page, path, first):
    lines = open(path, errors="replace").read().splitlines()
    plans = []
    pending_compile = None
    for line in lines:
        if line.startswith("census-visit"):
            break   # only the first visit of a multi-visit run
        if line.startswith("census-lazy-plan "):
            d = fields(line)
            if d.get("planned") == "yes":
                plans.append(d)
        elif line.startswith("census-lazy-bundle "):
            d = fields(line, " url=")
            url = line.split(" url=", 1)[1] if " url=" in line else ""
            b = page.bundles.setdefault(d["digest"], {
                "url": url, "bytes": num(d, "bytes"),
                "factories": num(d, "factories"),
                "factory_bytes": num(d, "factory-bytes"),
                "ready": d.get("ready") == "yes"})
            plan = next((p for p in plans
                         if num(p, "bytes") == b["bytes"]), None)
            if plan is not None:
                plans.remove(plan)
                page._min(b, "plan_us", num(plan, "us"))
            for key in ("preflight-us", "store-us", "register-us", "sha-us",
                        "fnv-us"):
                page._min(b, key.replace("-", "_"), num(d, key))
        elif line.startswith("census-lazy-factory "):
            d = fields(line)
            key = (d["digest"], num(d, "index"))
            f = page.factories.get(key)
            if f is None:
                f = page.factories[key] = {
                    "bytes": num(d, "bytes"), "fresh": num(d, "fresh"),
                    "fnv": d.get("fnv"), "compiles": 0, "run": 0,
                    "lazy_bytes": 0, "lazy_count": 0}
                if first:
                    page.events.append(("factory", key))
            if first:
                f["compiles"] += 1
            page._min(f, "compile_us", num(d, "compile-us"))
        elif line.startswith("census-lazy-factory-run "):
            d = fields(line)
            key = (d["digest"], num(d, "index"))
            f = page.factories.get(key)
            if f is not None and first and f["run"] == 0:
                f["run"] = num(d, "run")
                f["lazy_bytes"] = num(d, "lazy-bytes")
                f["lazy_count"] = num(d, "lazy-count")
        elif line.startswith("census-bytecode-lookup "):
            d = fields(line, " url=")
            url = line.split(" url=", 1)[1]
            key = (url, num(d, "ordinal"))
            result = d.get("result", "")
            if result.startswith("ineligible"):
                continue
            c = page.classic.setdefault(key, {"bytes": num(d, "bytes")})
            if first:
                page.events.append(("classic", key))
            pending_compile = key
        elif line.startswith("census-js event=compile kind=external"):
            if pending_compile is not None:
                d = fields(line, " name=")
                page._min(page.classic[pending_compile], "compile_us",
                          num(d, "us"))
                pending_compile = None
        elif line.startswith("census-bytecode-store "):
            d = fields(line, " url=")
            if first and d.get("result", "").startswith("stored"):
                url = line.split(" url=", 1)[1]
                page.stores.append(((url, num(d, "ordinal")),
                                    num(d, "bytecode")))
    for b in page.bundles.values():
        b.setdefault("plan_us", 0)


def load_pages(run_dir, names=None):
    pages = {}
    for entry in sorted(os.listdir(run_dir)):
        base = entry.split(".r", 1)[0]
        log = os.path.join(run_dir, entry, "lab.log")
        if not os.path.exists(log) or (names and base not in names):
            continue
        page = pages.setdefault(base, Page(base))
        parse_log(page, log, entry == base)
    for page in pages.values():
        for f in page.factories.values():
            f.setdefault("compile_us", 0)
        for c in page.classic.values():
            c.setdefault("compile_us", 0)
    return pages


# ---- per-factory cost ------------------------------------------------------

def preparse_rate(page):
    """Host us per source byte of this page's factory compiles."""
    b = sum(f["bytes"] for f in page.factories.values())
    us = sum(f["compile_us"] for f in page.factories.values())
    return us / b if b else 0.03


def factory_saving_us(page, f, high=False):
    """Host us a cached factory saves: its compile, plus (stored after its
    first call) the first-call compiles of the bodies it ran."""
    body = f["lazy_bytes"] * preparse_rate(page) * (
        BODY_RATE_HIGH if high else BODY_RATE_LOW)
    return f["compile_us"], body


def psp_s(host_us, factor):
    return host_us * factor / 1e6


# ---- pages -----------------------------------------------------------------

def page_summary(page):
    fac = list(page.factories.values())
    bun = list(page.bundles.values())
    compile_us = sum(f["compile_us"] for f in fac)
    body_lo = sum(factory_saving_us(page, f)[1] for f in fac)
    body_hi = sum(factory_saving_us(page, f, True)[1] for f in fac)
    return {
        "page": page.name,
        "bundles": len(bun),
        "bundle_kib": sum(b["bytes"] for b in bun) / KIB,
        "factories": sum(b["factories"] for b in bun),
        "compiled": len(fac),
        "recompiles": sum(max(0, f["compiles"] - 1) for f in fac),
        "src_kib": sum(f["bytes"] for f in fac) / KIB,
        "fresh_kib": sum(f["fresh"] for f in fac) / KIB,
        "run_kib": sum(f["run"] for f in fac) / KIB,
        "lazy_kib": sum(f["lazy_bytes"] for f in fac) / KIB,
        "compile_ms": compile_us / 1000.0,
        "compile_psp_s": psp_s(compile_us, COMPILE_X),
        "body_psp_s": (psp_s(body_lo, COMPILE_X), psp_s(body_hi, COMPILE_X)),
        "plan_ms": sum(b["plan_us"] for b in bun) / 1000.0,
        "plan_psp_s": psp_s(sum(b["plan_us"] for b in bun), NATIVE_X),
        "preflight_ms": sum(b["preflight_us"] for b in bun) / 1000.0,
        "preflight_psp_s": psp_s(sum(b["preflight_us"] for b in bun),
                                 COMPILE_X),
        "lz_ms": sum(b["store_us"] - b["preflight_us"] for b in bun) / 1000.0,
        "register_ms": sum(b["register_us"] for b in bun) / 1000.0,
        "sha_ms": sum(b["sha_us"] for b in bun) / 1000.0,
        "sha_psp_s": sum(b["bytes"] for b in bun) / KIB / KIB * SHA_S_PER_MIB,
        "fnv_ms": sum(b["fnv_us"] for b in bun) / 1000.0,
        "classic_kib": sum(dict(page.stores).values()) / KIB,
    }


def cmd_pages(run_dir, names):
    pages = load_pages(run_dir, set(names) or None)
    rows = [page_summary(p) for p in pages.values() if p.bundles]
    rows.sort(key=lambda r: -r["preflight_psp_s"] - r["plan_psp_s"])
    print("| page | bundles (KiB) | factories compiled / planned | "
          "compiled src KiB | fresh KiB | after-call KiB | first-call body KiB | "
          "PSP s: plan | preflight | factory compile | first-call bodies | "
          "SHA-256 | classic stored KiB |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|")
    for r in rows:
        print("| %s | %d (%.0f) | %d / %d | %.0f | %.0f | %.0f | %.0f | %.1f | "
              "%.1f | %.2f | %.1f-%.1f | %.2f | %.0f |" % (
                  r["page"], r["bundles"], r["bundle_kib"], r["compiled"],
                  r["factories"], r["src_kib"], r["fresh_kib"],
                  r["run_kib"], r["lazy_kib"], r["plan_psp_s"],
                  r["preflight_psp_s"], r["compile_psp_s"],
                  r["body_psp_s"][0], r["body_psp_s"][1], r["sha_psp_s"],
                  r["classic_kib"]))
    return rows


# ---- session simulation ----------------------------------------------------

class Table:
    """The session's bytecode table: LRU, byte ceiling, entry cap, and the
    current load's entries protected from eviction by that load."""

    def __init__(self, limit, entries=CLASSIC_ENTRIES):
        self.limit, self.max_entries = limit, entries
        self.items = collections.OrderedDict()   # key -> (bytes, generation)
        self.bytes = 0
        self.evicted = []

    def classic_entries(self):
        return sum(1 for k in self.items if not is_factory(k))

    def lookup(self, key, generation):
        if key not in self.items:
            return False
        size, _ = self.items.pop(key)
        self.items[key] = (size, generation)
        return True

    def put(self, key, size, generation):
        if key in self.items or size > self.limit:
            return key in self.items
        while (self.bytes + size > self.limit
               or (not is_factory(key)
                   and self.classic_entries() >= self.max_entries)):
            victim = next((k for k, (_, g) in self.items.items()
                           if g != generation), None)
            if victim is None:
                return False
            vsize, _ = self.items.pop(victim)
            self.bytes -= vsize
            self.evicted.append(victim)
        self.items[key] = (size, generation)
        self.bytes += size
        return True


def classic_charge(key, bytecode):
    return bytecode + 2 * len(key[0]) + 48


def is_factory(key):
    return key[0] == "factory"


def factory_key(key):
    return ("factory",) + tuple(key)


def simulate(a, b, design, limit, factory_limit=0, threshold_us=0,
             size_field="run"):
    """Session A then B. design: none | shared-all | shared-threshold |
    separate. Each factory is its own LRU entry; the 128-entry cap counts
    classic scripts only (a real design would group a bundle's factories in
    one record, as the Memory Stick tier groups a URL's records). Returns
    what B gets."""
    classic = Table(limit)
    factories = Table(factory_limit) if design == "separate" else classic

    def admit_factory(page, key, gen):
        if design == "none":
            return
        f = page.factories[key]
        if design == "shared-threshold":
            compile_us, body_us = factory_saving_us(page, f)
            if compile_us + body_us < threshold_us:
                return
        factories.put(factory_key(key), (f[size_field] or f["fresh"]) + 16,
                      gen)

    def factory_bytes():
        return sum(n for k, (n, _) in factories.items.items()
                   if is_factory(k))

    # Visit A (generation 1): factory stores in load order, then the
    # classic stores its idle turns make.
    for kind, key in a.events:
        if kind == "factory":
            admit_factory(a, key, 1)
    for key, bytecode in a.stores:
        classic.put(key, classic_charge(key, bytecode), 1)
    out = collections.Counter()
    out["factory_bytes"] = factory_bytes()
    out["classic_bytes"] = sum(n for k, (n, _) in classic.items.items()
                               if not is_factory(k))
    # Visit B (generation 2).
    a_stored = {k for k, _ in a.stores}
    for kind, key in b.events:
        if kind == "classic":
            c = b.classic.get(key, {})
            if classic.lookup(key, 2):
                out["classic_hits"] += 1
                out["classic_saved_us"] += c.get("compile_us", 0)
            elif key in a_stored:
                out["classic_lost"] += 1
                out["classic_lost_us"] += c.get("compile_us", 0)
        else:
            f = b.factories[key]
            if factories.lookup(factory_key(key), 2):
                compile_us, body_us = factory_saving_us(b, f)
                out["factory_hits"] += 1
                out["factory_compile_saved_us"] += compile_us
                out["factory_body_saved_us"] += body_us
                out["factory_restore_bytes"] += f[size_field] or f["fresh"]
            else:
                admit_factory(b, key, 2)
    out["peak_bytes"] = classic.bytes + (
        factories.bytes if factories is not classic else 0)
    return out


def pair_row(pages, a_name, b_name, limit):
    a, b = pages[a_name], pages[b_name]
    shared = [d for d in b.bundles if d in a.bundles]
    b_fac = list(b.factories)
    a_fac = set(a.factories)
    same = [k for k in b_fac if k in a_fac]
    # Content-addressed alternative: the same factory text in a different
    # bundle version (another digest) would also hit with an FNV/SHA key
    # per factory.
    a_fnv = {f["fnv"] for f in a.factories.values()}
    same_text = [k for k in b_fac if b.factories[k]["fnv"] in a_fnv]
    row = {"a": a_name, "b": b_name,
           "b_bundles": len(b.bundles), "shared_bundles": len(shared),
           "shared_bundle_kib": sum(b.bundles[d]["bytes"]
                                    for d in shared) / KIB,
           "b_factories": len(b_fac), "same_factories": len(same),
           "same_text_factories": len(same_text)}
    # Bundle-level work B repeats for a bundle A already ran: plan,
    # preflight, LZ source store.
    row["bundle_plan_us"] = sum(b.bundles[d]["plan_us"] for d in shared)
    row["bundle_preflight_us"] = sum(b.bundles[d]["preflight_us"]
                                     for d in shared)
    row["sha_psp_s"] = sum(bb["bytes"] for bb in b.bundles.values()) \
        / KIB / KIB * SHA_S_PER_MIB
    # A bundle record (digest, plan offsets, preflight verdict): 64 bytes
    # of key plus 12 bytes per planned factory.
    row["bundle_record_bytes"] = sum(64 + 12 * b.bundles[d]["factories"]
                                     for d in shared)
    designs = {}
    for name, kwargs in (
            ("classic only (today)", {"design": "none"}),
            ("a: all factories, shared 1.5 MiB",
             {"design": "shared-all"}),
            ("b: factories >= p80 cost, shared",
             {"design": "shared-threshold"}),
            ("c: separate 256 KiB factory table",
             {"design": "separate", "factory_limit": 256 * 1024}),
            ("c: separate 512 KiB factory table",
             {"design": "separate", "factory_limit": 512 * 1024})):
        if kwargs["design"] == "shared-threshold":
            kwargs["threshold_us"] = threshold_p80(a)
        designs[name] = simulate(a, b, limit=limit, **kwargs)
    row["designs"] = designs
    return row


def threshold_p80(page):
    costs = sorted(sum(factory_saving_us(page, f))
                   for f in page.factories.values())
    if not costs:
        return 0
    return costs[int(len(costs) * 0.8)]


def cmd_pairs(run_dir, pairs, limit, json_out):
    names = {n for p in pairs for n in p.split(":")}
    pages = load_pages(run_dir, names)
    rows = []
    for spec in pairs:
        a, b = spec.split(":")
        if a not in pages or b not in pages:
            print("missing run for", spec, file=sys.stderr)
            continue
        rows.append(pair_row(pages, a, b, limit))
    print("| pair | shared lazy bundles (KiB) | B factories same as A's "
          "(same text) | bundle work repeated, PSP s: plan + preflight | "
          "bundle records, KiB | SHA-256 of B's bundles, PSP s |")
    print("|---|---|---|---|---:|---:|")
    for r in rows:
        print("| %s -> %s | %d of %d (%.0f) | %d of %d (%d) | %.1f + %.1f "
              "| %.1f | %.2f |" % (
            r["a"], r["b"], r["shared_bundles"], r["b_bundles"],
            r["shared_bundle_kib"], r["same_factories"], r["b_factories"],
            r["same_text_factories"], psp_s(r["bundle_plan_us"], NATIVE_X),
            psp_s(r["bundle_preflight_us"], COMPILE_X),
            r["bundle_record_bytes"] / KIB, r["sha_psp_s"]))
    print()
    print("| pair | design | factory hits | saved PSP s (compile + bodies) "
          "| classic hits | classic lost (PSP s) | factory KiB after A | "
          "table KiB at end |")
    print("|---|---|---:|---|---:|---|---:|---:|")
    for r in rows:
        for name, d in r["designs"].items():
            print("| %s -> %s | %s | %d | %.2f + %.2f | %d | %d (%.2f) | "
                  "%.0f | %.0f |" % (
                      r["a"], r["b"], name, d["factory_hits"],
                      psp_s(d["factory_compile_saved_us"], COMPILE_X),
                      psp_s(d["factory_body_saved_us"], COMPILE_X),
                      d["classic_hits"], d["classic_lost"],
                      psp_s(d["classic_lost_us"], COMPILE_X),
                      d["factory_bytes"] / KIB, d["peak_bytes"] / KIB))
    if json_out:
        with open(json_out, "w") as f:
            json.dump(rows, f, indent=1, default=lambda o: dict(o))
    return rows


def main(argv):
    if len(argv) < 3 or argv[1] not in ("pages", "pairs"):
        sys.exit(__doc__)
    if argv[1] == "pages":
        cmd_pages(argv[2], argv[3:])
        return
    limit = 1536 * 1024
    json_out = None
    rest = []
    it = iter(argv[3:])
    for arg in it:
        if arg == "--table":
            limit = int(next(it)) * 1024
        elif arg == "--json":
            json_out = next(it)
        else:
            rest.append(arg)
    cmd_pairs(argv[2], rest, limit, json_out)


if __name__ == "__main__":
    main(sys.argv)
