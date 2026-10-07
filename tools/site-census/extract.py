#!/usr/bin/env python3
"""Turn one site-census lab log into a JSON metrics record.

usage: extract.py RUN_DIR [NAME URL TAGS]      (prints one JSON line)

RUN_DIR holds lab.log (stdout+stderr of psp-browser-interactive-lab run
with TILEFINCH_TRACE_CENSUS=1 and the census command script) and meta.json
written by census.py. The record layout is documented in README.md; field
names are append-only so old JSONL files stay comparable.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import classify, is_essential  # noqa: E402

FIELD = re.compile(r'([A-Za-z0-9_.-]+)=("((?:[^"\\]|\\.)*)"|\S*)')
MB = 1048576.0


def fields(line):
    out = {}
    for m in FIELD.finditer(line):
        out[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
    return out


def num(value, default=0):
    if value is None:
        return default
    m = re.match(r"-?\d+", str(value))
    return int(m.group(0)) if m else default


def first_line(lines, prefix):
    for line in lines:
        if line.startswith(prefix):
            return line
    return None


def last_line(lines, prefix):
    found = None
    for line in lines:
        if line.startswith(prefix):
            found = line
    return found


def loop_js_values(lines):
    """Values printed by `js` commands, in order. A record runs until the
    next lab line that the command loop prints after every command; its
    value ends at the last `" error="` (values and errors may span lines)."""
    values, current = [], None
    enders = ("loop-js ok=", "interaction-latency ", "loop frame=")

    def finish(chunk):
        m = re.match(r'loop-js ok=(\w+) value="(.*)', chunk, re.S)
        if not m:
            return
        text = m.group(2)
        cut = text.rfind('" error="')
        error = ""
        if cut >= 0:
            error = text[cut + len('" error="'):].rstrip()
            if error.endswith('"'):
                error = error[:-1]
            text = text[:cut]
        values.append({"ok": m.group(1) == "yes", "text": text, "error": error})

    for line in lines:
        if current is not None and line.startswith(enders):
            finish(current)
            current = None
        if line.startswith("loop-js ok="):
            current = line
        elif current is not None:
            current += "\n" + line
    if current is not None:
        finish(current)
    return values


def tagged_js(lines, tag):
    for value in loop_js_values(lines):
        if value["text"].startswith(tag + "="):
            return value["text"][len(tag) + 1:]
    return None


def probe_report(lines):
    chunks = []
    for value in loop_js_values(lines):
        if value["text"].startswith("probe-chunk="):
            chunks.append(value["text"][len("probe-chunk="):])
    if not chunks:
        return None
    try:
        return json.loads("".join(chunks))
    except ValueError:
        return {"error": "unparseable probe report", "bytes": sum(map(len, chunks))}


def memory_categories(lines):
    peaks, phase = {}, None
    for line in lines:
        if line.startswith("memory-categories "):
            phase = fields(line).get("phase")
        elif line.startswith("memory-category ") and phase == "interactive-stable":
            f = fields(line)
            peaks[f["name"]] = {"peak": num(f.get("peak")),
                                "current": num(f.get("current"))}
    return peaks


def extract(run_dir, name=None, url=None, tags=None):
    meta_path = os.path.join(run_dir, "meta.json")
    meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {}
    name = name or meta.get("name") or os.path.basename(run_dir.rstrip("/"))
    url = url or meta.get("url", "")
    tags = tags if tags is not None else meta.get("tags", [])
    log_path = os.path.join(run_dir, "lab.log")
    text = open(log_path, errors="replace").read() if os.path.exists(log_path) else ""
    err_path = os.path.join(run_dir, "lab.err")
    if os.path.exists(err_path):
        # stderr is kept apart so its unbuffered writes cannot split stdout
        # records; its trace lines (script-quota-skip, ...) are appended.
        text += "\n" + open(err_path, errors="replace").read()
    lines = text.splitlines()
    rec = {"name": name, "url": url, "tags": tags, "mode": meta.get("mode"),
           "exit": meta.get("exit"), "wall_s": meta.get("wall_s")}

    status = fields(last_line(lines, "interactive status=") or "")
    teardown = fields(last_line(lines, "interactive teardown=") or "")
    rec["lab_status"] = status.get("status", "missing")
    rec["title"] = status.get("title", "")
    rec["height"] = num(status.get("height"))
    rec["teardown"] = teardown.get("status", "missing")
    net = fields(last_line(lines, "network status=") or "")
    rec["http_status"] = num(net.get("status"))
    rec["mitigated"] = net.get("mitigated", "")
    rec["bot_wall"] = fields(last_line(lines, "bot-wall ") or "").get("detected", "")
    rec["challenge"] = fields(last_line(lines, "challenge outcome=") or "").get("outcome", "")
    rec["failure"] = ""
    for line in lines:
        if line.startswith("interactive failure: "):
            rec["failure"] = line[len("interactive failure: "):][:300]
    bw = fields(last_line(lines, "bot-wall ") or "")
    rec["bot_vendor"] = bw.get("vendor", "")
    rec["server"] = net.get("server", "")

    # Page facts from the census command script's tagged `js` probes.
    page = tagged_js(lines, "page")
    if page:
        try:
            rec["page"] = json.loads(page)
        except ValueError:
            # Cut at the lab's 1024-character value limit: keep the counts.
            rec["page"] = {k: int(v) for k, v in re.findall(r'"(\w+)":(\d+)', page)}
            m = re.search(r'"title":"((?:[^"\\]|\\.)*)"', page)
            if m:
                rec["page"]["title"] = m.group(1)
            rec["page"]["truncated_record"] = True
    else:
        rec["page"] = {}

    # Timings.
    stream = fields(last_line(lines, "stream bytes=") or "")
    timebase = fields(last_line(lines, "census-timebase ") or "")
    perf = fields(last_line(lines, "performance-us ") or "")
    first_paint = num(timebase.get("first-paint-us")) or num(stream.get("first-paint-us"))
    completion = num(timebase.get("completion-us")) or num(stream.get("completion-us"))
    rec["timing"] = {
        "first_paint_ms": round(first_paint / 1000.0, 1) if first_paint else None,
        "loaded_ms": round(completion / 1000.0, 1) if completion else None,
        "first_dom_ms": round(num(stream.get("first-dom-us")) / 1000.0, 1),
        "relayouts": num(perf.get("fast-relayouts")) + num(perf.get("full-relayouts")),
        "full_relayouts": num(perf.get("full-relayouts")),
        "relayout_ms": round(num(perf.get("relayout")) / 1000.0, 1),
        "layout_ms": round(num(perf.get("layout")) / 1000.0, 1),
        "parse_ms": round(num(perf.get("parse")) / 1000.0, 1),
        "script_ms": round(num(perf.get("script")) / 1000.0, 1),
        "style_ms": round(num(perf.get("style")) / 1000.0, 1),
        "resource_ms": round(num(perf.get("resource")) / 1000.0, 1),
        "raster_ms": round(num(perf.get("raster")) / 1000.0, 1),
    }
    resp = fields(last_line(lines, "responsiveness max-slice-us=") or "")
    rec["timing"]["longest_step_ms"] = round(num(resp.get("max-slice-us")) / 1000.0, 1)
    rec["timing"]["longest_step_phase"] = resp.get("phase", "")
    jsresp = fields(last_line(lines, "javascript-responsiveness ") or "")
    rec["timing"]["longest_js_slice_ms"] = round(num(jsresp.get("max-slice-us")) / 1000.0, 1)
    rec["timing"]["longest_compile_ms"] = round(num(jsresp.get("max-compile-us")) / 1000.0, 1)
    rec["timing"]["longest_callback_ms"] = round(num(jsresp.get("max-callback-us")) / 1000.0, 1)

    # Memory.
    cats = memory_categories(lines)
    rec["memory"] = {
        "peak_mb": round(num(teardown.get("peak")) / MB, 2),
        "categories_peak_mb": {k: round(v["peak"] / MB, 2) for k, v in cats.items()},
    }
    docmem = fields(last_line(lines, "document-memory ") or "")
    rec["document"] = {
        "html_bytes": num(stream.get("bytes")),
        "truncated": num(stream.get("truncated")) != 0,
        "nodes": num(docmem.get("nodes")),
        "elements": num(docmem.get("elements")),
        "attribute_bytes": num(docmem.get("attribute-bytes")),
        "body_text_bytes": num(docmem.get("body-text")),
    }
    lr = fields(last_line(lines, "layout-retained ") or "")
    lay = fields(last_line(lines, "census-layout ") or "")
    commands = num(lay.get("commands")) or 0
    boxes = num(lay.get("boxes")) or 0
    rec["layout"] = {
        "retained_bytes": num(lr.get("bytes")),
        "commands": commands,
        "boxes": boxes,
        "offscreen_share": round(num(lay.get("below")) / commands, 3) if commands else None,
        "offscreen2_share": round(num(lay.get("below2")) / commands, 3) if commands else None,
        "box_offscreen_share": round(num(lay.get("boxes-below")) / boxes, 3) if boxes else None,
        "document_height": num(lay.get("document-height")) or rec["height"],
        "visually_blank": fields(last_line(lines, "layout-presentation ") or "").get("visually-blank") == "yes",
    }
    viewport_h = num(timebase.get("viewport-height")) or 272

    # Images: decoded vs painted. Resources that alias one decoded surface
    # (surface= the first index holding it) are merged: bytes once, the
    # largest painted size, the topmost paint.
    surfaces = {}
    for line in lines:
        if line.startswith("census-image "):
            f = fields(line)
            dw, dh = [num(x) for x in (f.get("decoded", "0x0").split("x") + ["0"])[:2]]
            pw, ph = [num(x) for x in (f.get("painted", "0x0").split("x") + ["0"])[:2]]
            key = f.get("surface", f.get("index"))
            item = {"kind": f.get("kind"), "decoded": [dw, dh],
                    "decoded_bytes": num(f.get("decoded-bytes")),
                    "encoded_bytes": num(f.get("encoded-bytes")),
                    "painted": [pw, ph], "paints": num(f.get("paints")),
                    "top": num(f.get("top"), -1), "aliases": 1}
            prev = surfaces.get(key)
            if prev is None:
                surfaces[key] = item
            else:
                prev["aliases"] += 1
                prev["paints"] += item["paints"]
                if pw * ph > prev["painted"][0] * prev["painted"][1]:
                    prev["painted"] = [pw, ph]
                if item["top"] >= 0 and (prev["top"] < 0 or item["top"] < prev["top"]):
                    prev["top"] = item["top"]
    images = list(surfaces.values())
    dec = sum(i["decoded_bytes"] for i in images)
    unpainted = sum(i["decoded_bytes"] for i in images if i["paints"] == 0)
    at_display = 0
    for i in images:
        if i["paints"] == 0 or not i["decoded_bytes"]:
            continue
        need = min(i["decoded_bytes"], i["painted"][0] * i["painted"][1] * 4)
        at_display += need
    offscreen = sum(i["decoded_bytes"] for i in images
                    if i["paints"] and i["top"] >= viewport_h)
    res = fields(last_line(lines, "resources stylesheets=") or "")
    rec["images"] = {
        "count": len(images),
        "decoded_bytes": dec,
        "unpainted_bytes": unpainted,
        "offscreen_bytes": offscreen,
        "display_size_bytes": at_display,
        "display_size_savings": max(0, dec - unpainted - at_display),
        "discovered": num((res.get("images") or "0/0").split("/")[-1]),
        "loaded": num((res.get("images") or "0/0").split("/")[0]),
        "stat_decoded_bytes": num(res.get("image-decoded")),
        "downsampled": num(res.get("image-downsampled")),
        # Largest single decode at source size, and the same image's size
        # after downsampling to its target: the transient a decode-at-
        # display-size path would avoid.
        "source_peak_bytes": num(res.get("image-source-peak")),
        "target_peak_bytes": num(res.get("image-target-peak")),
        # Measured: the largest working set of one decode (decoder and
        # libwebp allocations plus the output). Absent in older labs.
        "decode_peak_bytes": num(res.get("image-decode-peak")),
    }
    # Display retargeting after layout (absent in older labs).
    rt = fields(last_line(lines, "image-retarget ") or "")
    rec["images"]["retarget"] = {
        key: num(rt.get(key)) for key in (
            "scans", "shrinks", "grows", "drops", "rasters", "deferred",
            "released", "restored", "us")
    }

    # Stylesheets.
    rec["styles"] = {
        "loaded": num((res.get("stylesheets") or "0/0").split("/")[0]),
        "discovered": num((res.get("stylesheets") or "0/0").split("/")[-1]),
        "rules": num(res.get("css-rules")),
        "bytes": num(res.get("css-bytes")),
        "truncated": num((res.get("css-truncated") or "0/0/0").split("/")[0]),
        "pressure_skips": num(res.get("css-pressure-skips")),
        "terminal_failures": num(res.get("css-terminal-failures")),
    }
    diag = fields(last_line(lines, "stylesheet-diagnostics ") or "")
    rec["styles"]["rejected_properties"] = diag.get("rejected-properties", "")

    # Scripts: summary counters.
    sc = fields(last_line(lines, "scripts discovered=") or "")
    rec["scripts"] = {k: num(sc.get(k)) for k in (
        "discovered", "attempted", "loaded", "failed", "cross-origin",
        "skipped-modules", "quota", "pressure", "bytes", "cache-hits")}
    dyn = fields(last_line(lines, "javascript-dynamic-scripts ") or "")
    rec["scripts"]["dynamic_quota"] = num(dyn.get("quota"))
    rec["scripts"]["dynamic_failed"] = num(dyn.get("failed"))
    handles = fields(last_line(lines, "javascript-dom-handles ") or "")
    watchdog = fields(last_line(lines, "javascript-watchdog ") or "")

    # Per-script ledger (TILEFINCH_TRACE_CENSUS). Only the last document's
    # records count: a --reload run keeps both.
    nav_start = num(timebase.get("navigation-start-us"))
    paint_abs = nav_start + (first_paint or completion) if nav_start else 0
    # Each compile record starts a script; an execute record joins the most
    # recent compile of the same name that has not executed yet (inline
    # scripts all share one name), else (bytecode-cache restore) starts one.
    script_list = []
    pending = {}
    # Evaluations nest (a lazy webpack factory compiles inside its bundle's
    # execution; a script inserted and run synchronously by another runs
    # inside it). Charge each event only its exclusive time: subtract every
    # directly nested event from its parent's duration.
    events = []
    for line in lines:
        if line.startswith("census-js "):
            f = fields(line)
            if nav_start and num(f.get("t")) < nav_start:
                continue
            f["exclusive"] = num(f.get("us"))
            events.append(f)
    stack = []
    for f in sorted(events, key=lambda e: (num(e.get("t")), -num(e.get("us")))):
        start = num(f.get("t"))
        while stack and num(stack[-1].get("t")) + num(stack[-1].get("us")) <= start:
            stack.pop()
        if stack:
            stack[-1]["exclusive"] = max(0, stack[-1]["exclusive"] - num(f.get("us")))
        stack.append(f)
    for f in events:
        t = num(f.get("t"))
        name = f.get("name", "")
        event = f.get("event")
        entry = None
        if event == "execute":
            waiting = pending.get(name)
            if waiting:
                entry = waiting.pop(0)
        if entry is None:
            kind = f.get("kind")
            if kind == "classic":
                kind = "external"
            entry = {"name": name, "kind": kind, "bytes": num(f.get("bytes")),
                     "compile_us": 0, "execute_us": 0, "compiles": 0,
                     "executes": 0, "first_t_ms": None, "pre_paint": None,
                     "ok": True}
            entry["category"] = classify(name, url, kind)
            script_list.append(entry)
            if event == "compile":
                pending.setdefault(name, []).append(entry)
        us = f["exclusive"]
        if event == "compile":
            entry["compile_us"] += us
            entry["compiles"] += 1
        else:
            entry["execute_us"] += us
            entry["executes"] += 1
        if entry["first_t_ms"] is None and nav_start:
            entry["first_t_ms"] = round((t - nav_start) / 1000.0, 1)
            entry["pre_paint"] = bool(paint_abs and t < paint_abs)
        if f.get("ok") == "no":
            entry["ok"] = False
    rec["script_list"] = script_list

    def total(items, field):
        return sum(i[field] for i in items)

    pre = [s for s in script_list if s["pre_paint"]]
    nonessential_pre = [s for s in pre if not is_essential(s["category"])]
    by_cat = {}
    for s in script_list:
        c = by_cat.setdefault(s["category"], {"scripts": 0, "bytes": 0,
                                              "compile_us": 0, "execute_us": 0,
                                              "pre_paint_us": 0})
        c["scripts"] += 1
        c["bytes"] += s["bytes"]
        c["compile_us"] += s["compile_us"]
        c["execute_us"] += s["execute_us"]
        if s["pre_paint"]:
            c["pre_paint_us"] += s["compile_us"] + s["execute_us"]
    cacheable = [s for s in script_list if s["kind"] in ("external", "module", "lazy-factory")]
    rec["js"] = {
        "scripts": len(script_list),
        "source_bytes": total(script_list, "bytes"),
        "compile_ms": round(total(script_list, "compile_us") / 1000.0, 2),
        "execute_ms": round(total(script_list, "execute_us") / 1000.0, 2),
        "cacheable_compile_ms": round(total(cacheable, "compile_us") / 1000.0, 2),
        "inline_compile_ms": round(sum(s["compile_us"] for s in script_list
                                       if s["kind"] == "inline") / 1000.0, 2),
        "pre_paint_ms": round((total(pre, "compile_us") + total(pre, "execute_us")) / 1000.0, 2),
        "pre_paint_nonessential_ms": round((total(nonessential_pre, "compile_us")
                                            + total(nonessential_pre, "execute_us")) / 1000.0, 2),
        "pre_paint_bytes": total(pre, "bytes"),
        "pre_paint_nonessential_bytes": total(nonessential_pre, "bytes"),
        "nonessential_bytes": sum(s["bytes"] for s in script_list
                                  if not is_essential(s["category"])),
        "by_category": by_cat,
        "dom_handle_exhaustions": num(handles.get("exhaustions")),
        "dom_handles_peak": num(handles.get("peak")),
        "watchdog_interrupted": watchdog.get("interrupted") == "yes",
        "compile_rejections": num(jsresp.get("compile-rejections")),
        "heap_limit_bytes": num(fields(first_line(lines, "runtime-policy ") or "").get("heap-bytes")),
    }
    mem = fields(last_line(lines, "script-memory ") or "")
    rec["js"]["heap_malloc_bytes"] = num(mem.get("malloc"))
    peak = fields(last_line(lines, "script-js-malloc-peak ") or "")
    rec["js"]["heap_peak_bytes"] = num(peak.get("bytes"))
    ext = fields(last_line(lines, "javascript-external-bytecode ") or "")
    rec["js"]["bytecode_cache_hits"] = num(ext.get("hits"))
    rec["js"]["bytecode_cache_misses"] = num(ext.get("misses"))
    rec["js"]["bytecode_cache_stores"] = num(ext.get("stores"))
    rec["js"]["bytecode_cache_stored_bytes"] = num(ext.get("stored-bytes"))
    rec["js"]["bytecode_cache_restore_us"] = num(ext.get("restore-us"))
    rec["js"]["bytecode_cache_store_us"] = num(ext.get("store-us"))
    rec["js"]["bytecode_cache_session_bytes"] = num(ext.get("cache-bytes"))
    # Deferred stores (absent before the store moved to idle work).
    rec["js"]["bytecode_cache_deferred"] = num(ext.get("deferred"))
    rec["js"]["bytecode_cache_deferred_dropped"] = num(ext.get("dropped"))
    rec["js"]["bytecode_cache_pending_hits"] = num(ext.get("pending-hits"))
    rec["js"]["bytecode_cache_idle_store_us"] = num(ext.get("idle-store-us"))
    # The persistent compiled-script tier (--script-cache-dir runs only).
    disk = fields(last_line(lines, "javascript-script-cache-disk ") or "")
    for key in ("hits", "load-us", "reads", "read-bytes", "promoted",
                "index-misses", "read-us", "verify-us", "writes",
                "written-bytes", "write-us", "rejects", "evictions", "files",
                "bytes"):
        rec["js"]["script_disk_" + key.replace("-", "_")] = num(disk.get(key))
    mod = fields(last_line(lines, "javascript-module-bytecode ") or "")
    rec["js"]["module_bytecode_hits"] = num(mod.get("hits"))
    rec["js"]["module_bytecode_session_bytes"] = num(mod.get("cache-bytes"))
    lazy = fields(last_line(lines, "script-lazy-functions ") or "")
    rec["js"]["lazy_compiled_bytes"] = num(lazy.get("compiled-bytes"))

    # Errors.
    exceptions, rejections, csp = [], [], []
    csp_no_eval = False
    handled = 0
    for line in lines:
        if line.startswith("census-exception "):
            f = fields(line)
            if f.get("message", "").startswith("InternalError: interrupted"):
                pass
            exceptions.append({"message": f.get("message", ""), "where": f.get("where", "")})
        elif line.startswith("census-rejection "):
            f = fields(line)
            rejections.append({"message": f.get("message", ""), "where": f.get("where", "")})
        elif line.startswith("census-rejection-handled"):
            handled += 1
        elif line.startswith("census-csp-refusal "):
            f = fields(line)
            item = {"what": f.get("what"), "url": f.get("url", "")}
            if item["what"] == "policy-no-eval":
                csp_no_eval = True
            elif item not in csp:
                csp.append(item)
    # The `js` command's own evaluation errors are lab noise.
    exceptions = [e for e in exceptions if "<loop-js>" not in e["where"]]
    rec["errors"] = {"exceptions": exceptions, "rejections": rejections,
                     "rejections_handled_later": handled,
                     "unhandled_rejections_final": num(fields(last_line(
                         lines, "javascript-promises ") or "").get("unhandled")),
                     "csp_refusals": csp,
                     "csp_blocks_eval": csp_no_eval,
                     "last_error": fields(last_line(lines, "javascript-error=") or "").get("javascript-error", "")}

    # Limits hit.
    limits = []
    if rec["document"]["truncated"]:
        limits.append("document-cap")
    if rec["scripts"]["quota"] or rec["scripts"]["dynamic_quota"]:
        limits.append("script-quota")
    if rec["scripts"]["pressure"]:
        limits.append("script-memory-pressure")
    if rec["styles"]["truncated"]:
        limits.append("stylesheet-file-cap")
    if rec["styles"]["pressure_skips"]:
        limits.append("stylesheet-pressure")
    if rec["js"]["dom_handle_exhaustions"]:
        limits.append("dom-handles")
    if rec["js"]["watchdog_interrupted"]:
        limits.append("script-watchdog")
    if rec["js"]["compile_rejections"]:
        limits.append("script-compile-cap")
    for line in lines:
        if line.startswith("script-quota-skip"):
            if "script-quota" not in limits:
                limits.append("script-quota")
        if line.startswith("js-heap-reject ") or (
                "heap-rejections=" in line and num(fields(line).get("heap-rejections")) > 0):
            if "js-heap" not in limits:
                limits.append("js-heap")
    for e in exceptions:
        if "out of memory" in e["message"] or "InternalError: interrupted" in e["message"]:
            tag = "js-heap" if "memory" in e["message"] else "script-watchdog"
            if tag not in limits:
                limits.append(tag)
    if rec["images"]["discovered"] > rec["images"]["loaded"] and \
            fields(last_line(lines, "image-network ") or ""):
        pass
    # Script refusals (TILEFINCH_TRACE_SCRIPT_FAILURES, stderr).
    file_cap = num(fields(first_line(lines, "runtime-policy ") or "").get("file-bytes"))
    refused = []
    for line in lines:
        if line.startswith("script-fetch-failure "):
            f = fields(line)
            limit = num(f.get("limit"))
            error = f.get("error", "")
            if "quota exceeded" in error:
                reason = "per-script cap" if file_cap and limit >= file_cap else "total script bytes cap"
            else:
                reason = "fetch: " + error[:60]
            refused.append({"url": f.get("url", ""), "reason": reason, "limit": limit})
        elif line.startswith("script-quota-skip "):
            refused.append({"url": line.split("url=", 1)[-1][:300], "reason": "script quota (count/total)"})
        elif line.startswith("script-admission-reject "):
            f = fields(line)
            refused.append({"url": f.get("url", ""), "reason": "compile admission (memory)"})
        elif line.startswith("script-network-bound-reject "):
            refused.append({"url": line.split("url=", 1)[-1][:300], "reason": "network working-set bound"})
        elif line.startswith("module-refused "):
            f = fields(line)
            refused.append({"url": f.get("url", ""), "reason": "module: " + f.get("reason", "")[:60]})
    rec["scripts_refused"] = refused
    # Heavy-page estimate (script-report; include/tilefinch/script_admission.h).
    heavy = fields(last_line(lines, "heavy-page ") or "")
    rec["heavy"] = {
        "class": heavy.get("class", "none"),
        "script_bytes": num(heavy.get("script-bytes")),
        "estimate_ms": num(heavy.get("estimate-ms")),
        "waiting_ms": num(heavy.get("waiting-ms")),
        "memory_needed": num(heavy.get("memory-needed")),
        "memory_free": num(heavy.get("memory-free")),
        "visible_text": num(heavy.get("visible-text")),
        "largest_unit": num(heavy.get("largest-unit")),
        "oversized": heavy.get("oversized", "0/0"),
        "memory_rescue": fields(last_line(lines, "script-report ") or "")
            .get("memory-rescue", "no"),
    }
    count_cap = num(fields(first_line(lines, "runtime-policy ") or "").get("scripts"))
    if count_cap and rec["scripts"]["discovered"] > count_cap:
        limits.append("script-count-cap")
    for item in refused:
        tag = {"per-script cap": "per-script-cap",
               "total script bytes cap": "script-total-cap",
               "script quota (count/total)": "script-quota",
               "compile admission (memory)": "script-memory-pressure"}.get(item["reason"])
        if tag and tag not in limits:
            limits.append(tag)
    rec["limits"] = limits

    # Fallbacks.
    reader = fields(last_line(lines, "census-reader ") or "")
    basic = fields(last_line(lines, "loop-basic ") or "")
    rec["fallback"] = {
        "reader_kind": reader.get("kind", ""),
        "reader_high_confidence": reader.get("high-confidence") == "yes",
        "reader_extracted_bytes": num(reader.get("extracted-bytes")),
        "reader_extracted_nodes": num(reader.get("extracted-nodes")),
        "reader_visible_text": num(reader.get("visible-text")),
        "reader_truncated": reader.get("truncated") == "yes",
        "reader_bounded": reader.get("bounded") == "yes",
        "blank": reader.get("blank") == "yes",
        "auto_reader_candidate": reader.get("high-confidence") == "yes"
                                 and reader.get("kind", "raw") not in ("raw", ""),
        "basic_prepared": basic.get("prepared") == "yes",
        "basic_nodes": num(basic.get("nodes")),
        "basic_bytes": num(basic.get("bytes")),
        "basic_forms": num(basic.get("forms")),
        "basic_truncated": basic.get("truncated") == "yes",
        "basic_prepare_ms": round(num(basic.get("prepare-us")) / 1000.0, 1),
        "basic_text": num(basic.get("text")),
    }

    # Deterministic work vector (label=final).
    work = {}
    for line in lines:
        m = re.search(r"tilefinch-work: label=final (.*)", line)
        if m:
            for k, v in fields(m.group(1)).items():
                if re.fullmatch(r"\d+", v or ""):
                    work[k] = int(v)
    rec["work"] = work

    probe = probe_report(lines)
    if probe is not None:
        rec["api_probe"] = probe
    init = fields(last_line(lines, "census-init-script ") or "")
    if init:
        rec["init_script"] = init.get("status")

    state = fields(last_line(lines, "javascript-state ") or "")
    rec["text_preview"] = (state.get("body-text") or "")[:240]
    rec["outcome_auto"] = auto_outcome(rec)
    return rec


CHALLENGE_TITLES = re.compile(
    r"just a moment|attention required|access denied|are you a robot|"
    r"captcha|verify you are human|security check|blocked|pardon our interruption|"
    r"robot check|unusual traffic|please enable js|enable javascript", re.I)


def auto_outcome(rec):
    """First-pass outcome; REPORT.md applies manual screenshot judgements."""
    text_len = rec["document"]["body_text_bytes"]
    preview = rec.get("text_preview", "")
    title = rec.get("title", "")
    if rec.get("challenge") not in ("", "not-applicable"):
        return "challenged"
    if rec.get("bot_wall") == "yes":
        return "blocked"
    if rec["failure"] or rec["lab_status"] != "ok":
        return "crash"
    if CHALLENGE_TITLES.search(title) or (text_len < 600 and CHALLENGE_TITLES.search(preview)):
        return "challenged"
    if rec["http_status"] in (401, 403, 429, 503):
        return "blocked"
    if rec["layout"].get("visually_blank") or text_len < 40:
        return "blank"
    if rec["styles"]["discovered"] and not rec["styles"]["loaded"] and \
            rec["styles"]["rules"] < 20:
        return "unstyled"
    if rec["errors"]["exceptions"] or rec["limits"]:
        return "partial"
    return "working"


def main():
    run_dir = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else None
    url = sys.argv[3] if len(sys.argv) > 3 else None
    tags = sys.argv[4].split(",") if len(sys.argv) > 4 else None
    print(json.dumps(extract(run_dir, name, url, tags), sort_keys=True))


if __name__ == "__main__":
    main()
