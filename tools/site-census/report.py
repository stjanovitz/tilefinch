#!/usr/bin/env python3
"""Aggregate site-census JSONL into the census report tables (Markdown).

usage: report.py METRICS.jsonl [--probe PROBE.jsonl] [--revisit REVISIT.jsonl]
                 [--judgements judgements.tsv] [--variance RUN2.jsonl]

judgements.tsv (optional, hand-written after looking at the screenshots):
  name<TAB>outcome<TAB>note       outcome overrides outcome_auto
Prints Markdown to stdout: ranked problems, per-site table, A/B/C estimates.
"""
import argparse
import collections
import json
import re
import statistics
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classify import classify  # noqa: E402

MB = 1048576.0

# Curated platform names (WHATWG/W3C/TC39 or a shipping engine's vendor
# API), built from what the corpus actually probed. A probe record counts as
# an API gap only when its receiver label and key are listed here: pages
# also look up their own expandos (jQuery data keys, custom-element
# callbacks, framework globals), which are not missing APIs. Values are
# "std" or "vendor" (legacy or single-engine names: a miss there is the
# expected answer for a standards browser).
KNOWN = {
    "window": {
        **{n: "std" for n in (
            "AudioData AuthenticatorAssertionResponse AuthenticatorAttestationResponse "
            "AuthenticatorResponse BarcodeDetector CSSCharsetRule CSSCounterStyleRule "
            "CSSFontPaletteValuesRule CSSSupportsRule CanvasCaptureMediaStream ClipboardItem "
            "ContentIndex ContentVisibilityAutoStateChangeEvent CropTarget CryptoKey "
            "EventCounts External EyeDropper FontFaceSet FragmentDirective GPUSupportedLimits "
            "GeolocationPosition HTMLDataElement HTMLPreElement IIRFilterNode IdleDetector "
            "LaunchQueue MathMLElement MediaDeviceInfo MediaMetadata MediaSourceHandle "
            "MediaStreamTrack MutationEvent Notification OfflineAudioContext OffscreenCanvas "
            "PasswordCredential PerformanceEventTiming PerformanceServerTiming PressureObserver "
            "PublicKeyCredential PushManager PushSubscriptionOptions RTCDataChannel "
            "RTCDtlsTransport RTCEncodedAudioFrame RTCError RTCPeerConnection "
            "RTCPeerConnectionIceErrorEvent RTCRtpTransceiver RTCSctpTransport RTCStatsReport "
            "RTCTrackEvent ReadableStreamDefaultController RestrictionTarget SVGAnimationElement "
            "SVGDiscardElement SVGDocument SVGFEDropShadowElement SVGTextPositioningElement "
            "Scheduling ServiceWorker ServiceWorkerContainer ServiceWorkerRegistration "
            "SharedWorker StaticRange StyleSheet SuppressedError TextEvent TextTrackCue "
            "TouchEvent URLPattern VTTRegion VideoFrame VideoPlaybackQuality VideoStreamTrack "
            "VideoTrackList ViewTransition VisualViewport WebGLObject WebSocketStream "
            "WebTransport WritableStreamDefaultController XMLSerializer XPathNSResolver "
            "XPathResult navigation scheduler orientation onbeforeunload onpagehide "
            "onpageshow external print screenX ShadowRealm AsyncContext").split()},
        **{n: "vendor" for n in (
            "ActiveXObject attachEvent detachEvent chrome opera safari InstallTrigger MSApp "
            "MSInputMethodContext StyleMedia XDomainRequest HTMLMenuItemElement "
            "webkitRequestAnimationFrame WebKitCSSMatrix WebKitMediaKeyError CSS2Properties "
            "DOMSettableTokenList Controllers MozMobileMessageManager MozSmsMessage "
            "DeviceStorage OfflineResourceList XPathNamespace XULPopupElement SVGPathSegList "
            "InputMethodContext WindowUtils GestureEvent SmartCardEvent MediaSessionCoordinator "
            "SiteBoundCredential DetachedViewControlEvent DeprecationReportBody "
            "AppBannerPromptResult ApplePaySession ApplePayError ApplePaySetup "
            "ApplePaySetupFeature AudioSinkInfo CanvasFilter AICreateMonitor").split()},
    },
    "Document": {
        **{n: "std" for n in "all ariaNotify evaluate featurePolicy fonts hasStorageAccess "
           "prerendering open onfullscreenchange DOCUMENT_NODE".split()},
        **{n: "vendor" for n in "documentMode mozFullScreenElement msFullscreenElement "
           "onmozfullscreenchange onwebkitfullscreenchange parentWindow createEventObject "
           "attachEvent".split()},
    },
    "HTMLElement": {
        **{n: "std" for n in "dir draggable hasAttributes onbeforematch accessKey".split()},
        **{n: "vendor" for n in "msMatchesSelector currentStyle contextMenu attachEvent".split()},
    },
    "Element": {n: "std" for n in "ariaNotify checkVisibility replaceChildren hasAttributes".split()},
    "Navigator": {
        **{n: "std" for n in "bluetooth contacts devicePosture doNotTrack geolocation "
           "globalPrivacyControl mediaDevices mediaSession scheduling serviceWorker share "
           "vibrate".split()},
        **{n: "vendor" for n in "appMinorVersion msDoNotTrack systemLanguage userLanguage "
           "buildID oscpu brave cookieDeprecationLabel loadPurpose cpuPerformance".split()},
    },
    "HTMLInputElement": {n: "std" for n in "accessKey autofocus capture inputMode".split()},
    "HTMLAnchorElement": {"hasAttributes": "std", "accessKeyLabel": "vendor"},
    "HTMLButtonElement": {n: "std" for n in "command popoverTargetElement hasAttributes".split()},
    "HTMLSlotElement": {"hasAttributes": "std"},
    "HTMLDialogElement": {"closedBy": "std", "hasAttributes": "std"},
    "HTMLScriptElement": {"text": "std"},
    "static:HTMLScriptElement": {"supports": "std"},
    "static:Symbol": {"dispose": "std", "metadata": "std"},
    "static:URL": {"canParse": "std", "parse": "std"},
    "static:Error": {"captureStackTrace": "vendor"},
    "console": {"context": "vendor", "createTask": "vendor", "exception": "vendor"},
    "HTMLStyleElement": {"styleSheet": "vendor"},
    "URLSearchParams": {"size": "std"},
    "URL": {"password": "std", "username": "std"},
    "Node": {"removeChild": "std"},
    "HTMLImageElement": {"loading": "std"},
    "Clipboard": {"read": "std"},
    "HTMLVideoElement": {"getVideoPlaybackQuality": "std"},
}
# Element members a lookup on an element reaches through HTMLElement's
# sentinel first; Chromium defines them on Element.
ELEMENT_MEMBERS = {"hasAttributes", "ariaNotify", "checkVisibility", "replaceChildren"}


def normalise_api(name):
    """Probe record "Label.key" -> (API name as written, kind) or None."""
    label, _, key = name.partition(".")
    if label == "CSSStyleDeclaration":
        return ("CSSStyleDeclaration property names ('%s' in style)" % key, "css")
    kind = KNOWN.get(label, {}).get(key)
    if kind is None:
        return None
    if label == "window":
        return (key, kind)
    if label.startswith("static:"):
        return ("%s.%s" % (label[len("static:"):], key), kind)
    if label.startswith("HTML") and key in ELEMENT_MEMBERS:
        return ("Element.prototype." + key, kind)
    if label == "console":
        return ("console." + key, kind)
    return ("%s.prototype.%s" % (label, key), kind)


def load(path):
    if not path:
        return {}
    out = {}
    for line in open(path):
        if line.strip():
            r = json.loads(line)
            out[r["name"]] = r
    return out


def judgements(path):
    out = {}
    if not path:
        return out
    for line in open(path):
        if line.strip() and not line.startswith("#"):
            parts = line.rstrip("\n").split("\t")
            out[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "")
    return out


def ms(v):
    return "-" if v is None else ("%.0f" % v if v >= 10 else "%.1f" % v)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("metrics")
    p.add_argument("--probe")
    p.add_argument("--revisit")
    p.add_argument("--judgements")
    p.add_argument("--variance")
    args = p.parse_args()
    recs = load(args.metrics)
    probe = load(args.probe)
    revisit = load(args.revisit)
    judged = judgements(args.judgements)
    names = sorted(recs)
    # Basic view and script-refusal reasons come from the probe pass (Basic
    # view rewrites the page; refusal tracing perturbs timing).
    for n in names:
        if n in probe:
            for k, v in probe[n].get("fallback", {}).items():
                if k.startswith("basic_"):
                    recs[n]["fallback"][k] = v
            recs[n]["scripts_refused"] = probe[n].get("scripts_refused", [])
            for tag in ("per-script-cap", "script-total-cap", "script-count-cap"):
                if tag in probe[n].get("limits", []) and tag not in recs[n]["limits"]:
                    recs[n]["limits"].append(tag)

    def outcome(n):
        return judged.get(n, (recs[n]["outcome_auto"], ""))[0]

    # ---- Ranked problems ----
    problems = collections.defaultdict(set)
    examples = {}
    for n in names:
        r = recs[n]
        o = outcome(n)
        if o in ("blocked", "challenged"):
            vendor = r.get("bot_vendor") or r.get("server") or "?"
            problems["%s by %s" % (o, vendor)].add(n)
        if o == "login-wall":
            problems["login wall for logged-out clients"].add(n)
        if o == "crash":
            problems["lab crash / failed load"].add(n)
        for lim in r.get("limits", []):
            if lim != "script-quota" or not any(
                    x in r["limits"] for x in ("per-script-cap", "script-total-cap", "script-count-cap")):
                problems["limit: " + lim].add(n)
        for e in r["errors"]["exceptions"]:
            msg = re.sub(r"'[^']{20,}'", "'…'", e["message"])
            msg = re.sub(r"\b[a-zA-Z_$][\w$]{0,2}\b(?= is not)", "x", msg)
            key = "exception: " + msg[:110]
            problems[key].add(n)
        for c in r["errors"]["csp_refusals"]:
            problems["CSP refusal: " + c["what"]].add(n)
        if (r["timing"]["loaded_ms"] or 0) > 10000:
            problems["perf wall: lab load > 10 s"].add(n)
        if (r["timing"]["longest_step_ms"] or 0) > 1000:
            problems["perf wall: single blocking step > 1 s (%s)" %
                     r["timing"]["longest_step_phase"]].add(n)
        if r["memory"]["peak_mb"] > 20:
            problems["memory: peak > 20 MiB"].add(n)
    api_sites = collections.defaultdict(set)
    api_kind = {}
    api_callers = collections.defaultdict(collections.Counter)
    css_props = collections.defaultdict(set)
    for n, r in probe.items():
        for name, (count, mask, where) in r.get("api_probe", {}).get("misses", {}).items():
            found = normalise_api(name)
            if not found:
                continue
            api, kind = found
            if kind == "css":
                css_props[n].add(name.partition(".")[2])
                continue
            api_sites[api].add(n)
            api_kind[api] = kind
            m = re.search(r"\((https?://[^):]+)", where or "")
            api_callers[api][classify(m.group(1), r["url"]) if m else "first-party"] += 1
    counts = collections.Counter(outcome(n) for n in names)
    print("## Outcomes\n")
    print("| outcome | pages | which |")
    print("|---|---|---|")
    for o in ("working", "partial", "unstyled", "broken", "blank", "interstitial",
              "login-wall", "blocked", "challenged", "crash"):
        which = [n for n in names if outcome(n) == o]
        if which:
            print("| %s | %d | %s |" % (o, len(which), ", ".join(which)))
    print()
    print("## Problems ranked by sites affected\n")
    print("| # | problem | sites | which |")
    print("|---|---|---|---|")
    ranked = sorted(problems.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    for i, (k, v) in enumerate(ranked[:60], 1):
        print("| %d | %s | %d | %s |" % (i, k.replace("|", "\\|"), len(v), ", ".join(sorted(v))))

    print("\n## Missing APIs probed or called (API-probe pass)\n")
    print("Each row is a platform name page code read or tested and did not find "
          "(curated list in report.py; page expandos are excluded). `callers` is "
          "the classifier category of the script that did the first lookup on "
          "each site.\n")
    print("| API | sites | std/vendor | callers | which |")
    print("|---|---|---|---|---|")
    if css_props:
        allp = collections.Counter(p for v in css_props.values() for p in v)
        print("| `'prop' in element.style` (CSSStyleDeclaration exposes no property names; %d names, e.g. %s) | %d | std | - | %s |" % (
            len(allp), ", ".join(p for p, _ in allp.most_common(6)), len(css_props),
            ", ".join(sorted(css_props))))
    for api, sites in sorted(api_sites.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        if len(sites) < 2 and api_kind[api] == "vendor":
            continue
        callers = ", ".join("%s %d" % kv for kv in api_callers[api].most_common(3))
        print("| `%s` | %d | %s | %s | %s |" % (api, len(sites), api_kind[api], callers,
                                              ", ".join(sorted(sites))))

    # ---- Per-site table ----
    print("\n## Per-site results\n")
    print("| site | outcome | first paint ms | loaded ms | peak MiB | JS compile/exec ms | top issues |")
    print("|---|---|---|---|---|---|---|")
    for n in names:
        r = recs[n]
        issues = []
        if r["errors"]["exceptions"]:
            issues.append("%d exc: %s" % (len(r["errors"]["exceptions"]),
                                          r["errors"]["exceptions"][0]["message"][:60]))
        if r["limits"]:
            issues.append("limits: " + ",".join(r["limits"]))
        note = judged.get(n, ("", ""))[1]
        if note:
            issues.insert(0, note)
        print("| %s | %s | %s | %s | %.1f | %s / %s | %s |" % (
            n, outcome(n), ms(r["timing"]["first_paint_ms"]), ms(r["timing"]["loaded_ms"]),
            r["memory"]["peak_mb"], ms(r["js"]["compile_ms"]), ms(r["js"]["execute_ms"]),
            "; ".join(issues).replace("|", "\\|")[:200]))

    # ---- Estimate A ----
    print("\n## Estimate A: JavaScript cost\n")
    compile_total = sum(recs[n]["js"]["compile_ms"] for n in names)
    cacheable = sum(recs[n]["js"]["cacheable_compile_ms"] for n in names)
    pre = sum(recs[n]["js"]["pre_paint_ms"] for n in names)
    pre_ne = sum(recs[n]["js"]["pre_paint_nonessential_ms"] for n in names)
    with_js = [n for n in names if recs[n]["js"]["scripts"]]
    big = [n for n in names if recs[n]["js"]["cacheable_compile_ms"] >= 20]
    print("- Pages running page script: %d of %d." % (len(with_js), len(names)))
    print("- Compile time, all page scripts: %.0f ms (lab host); external/module "
          "(cacheable by URL+content): %.0f ms on %d pages with >= 20 ms." % (
              compile_total, cacheable, len(big)))
    if revisit:
        saved = 0.0
        for n in names:
            if n in revisit:
                saved += recs[n]["js"]["compile_ms"] - revisit[n]["js"]["compile_ms"]
        print("- Measured second visit in one session (existing in-memory "
              "bytecode cache, --reload 1): compile drops by %.0f ms in total." % saved)
    if pre:
        print("- Pre-paint JS (compile + top-level execute before first paint): "
              "%.0f ms; non-essential categories: %.0f ms (%.0f%%)." % (
                  pre, pre_ne, 100.0 * pre_ne / pre))
    cats = collections.Counter()
    cat_bytes = collections.Counter()
    for n in names:
        for c, v in recs[n]["js"]["by_category"].items():
            cats[c] += v["compile_us"] + v["execute_us"]
            cat_bytes[c] += v["bytes"]
    print("\n| category | compile+exec ms | source KiB |")
    print("|---|---|---|")
    for c, v in cats.most_common():
        print("| %s | %.0f | %.0f |" % (c, v / 1000.0, cat_bytes[c] / 1024.0))

    # ---- Estimate B ----
    print("\n## Estimate B: memory headroom\n")
    dec = sum(recs[n]["images"]["decoded_bytes"] for n in names)
    save = sum(recs[n]["images"]["display_size_savings"] for n in names)
    unp = sum(recs[n]["images"]["unpainted_bytes"] for n in names)
    off = sum(recs[n]["images"]["offscreen_bytes"] for n in names)
    print("- Decoded image bytes retained at the end of load: %.1f MiB; "
          "decoding painted images at their painted size would save %.1f MiB, "
          "never-painted images hold %.1f MiB, below-the-first-screen images "
          "%.1f MiB." % (dec / MB, save / MB, unp / MB, off / MB))
    transient = [(recs[n]["images"].get("source_peak_bytes", 0)
                  - recs[n]["images"].get("target_peak_bytes", 0), n) for n in names]
    transient = sorted((t, n) for t, n in transient if t > 0)
    if transient:
        print("- Full-size decode before downsampling: on %d pages the largest image "
              "is decoded at source size first; decoding at target size would cut "
              "that transient by %.1f MiB in total (largest: %s)." % (
                  len(transient), sum(t for t, _ in transient) / MB,
                  ", ".join("%s %.1f MiB" % (n, t / MB) for t, n in transient[::-1][:4])))
    measured = sorted((recs[n]["images"].get("decode_peak_bytes") or 0, n)
                      for n in names)
    if measured and measured[-1][0]:
        # The transient above is computed from the source and target
        # dimensions; this one is measured around the decode itself.
        print("- Largest measured decode working set (decoder and libwebp "
              "allocations plus the output): %s." % ", ".join(
                  "%s %.1f MiB" % (n, t / MB) for t, n in measured[::-1][:4]))
    shares = [recs[n]["layout"]["offscreen_share"] for n in names
              if recs[n]["layout"]["offscreen_share"] is not None]
    if shares:
        lay = sum(recs[n]["memory"]["categories_peak_mb"].get("layout", 0) for n in names)
        weighted = sum(recs[n]["memory"]["categories_peak_mb"].get("layout", 0)
                       * (recs[n]["layout"]["offscreen_share"] or 0) for n in names)
        retained = sum(recs[n]["layout"]["retained_bytes"] for n in names)
        retained_off = sum(recs[n]["layout"]["retained_bytes"]
                           * (recs[n]["layout"]["offscreen_share"] or 0) for n in names)
        print("- Layout: median %.0f%% of draw commands lie below the first "
              "screen. Retained layout (display list and boxes) totals %.1f MiB, "
              "%.1f MiB of it off-screen; the layout category's peak (build "
              "working memory included) totals %.1f MiB, %.1f MiB of it "
              "attributable to off-screen content by command share." % (
                  100 * statistics.median(shares), retained / MB, retained_off / MB,
                  lay, weighted))
    print("\n| site | peak MiB | js | layout | resource | dom | render | images decoded MiB | display-size saving MiB |")
    print("|---|---|---|---|---|---|---|---|---|")
    for n in sorted(names, key=lambda n: -recs[n]["memory"]["peak_mb"])[:20]:
        c = recs[n]["memory"]["categories_peak_mb"]
        print("| %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |" % (
            n, recs[n]["memory"]["peak_mb"], c.get("javascript", 0), c.get("layout", 0),
            c.get("resource", 0), c.get("dom", 0), c.get("render", 0),
            recs[n]["images"]["decoded_bytes"] / MB,
            recs[n]["images"]["display_size_savings"] / MB))

    # ---- Estimate C ----
    print("\n## Estimate C: fallbacks\n")
    kinds = collections.Counter(recs[n]["fallback"]["reader_kind"] for n in names)
    auto = [n for n in names if recs[n]["fallback"]["auto_reader_candidate"]]
    broken = [n for n in names if outcome(n) not in ("working",)]
    rescue = [n for n in broken if recs[n]["fallback"]["reader_extracted_bytes"] > 2000
              or recs[n]["fallback"]["basic_nodes"] > 50]
    print("- Reader classification: %s." % ", ".join("%s %d" % kv for kv in kinds.most_common()))
    print("- Auto-Reader candidates (high confidence, not raw): %d: %s." % (len(auto), ", ".join(auto)))
    print("- Of %d pages not fully working, %d have a substantial Reader or "
          "Basic extraction: %s." % (len(broken), len(rescue), ", ".join(rescue)))
    print("\n| site | outcome | reader kind | high conf | extracted KiB | basic nodes | basic truncated |")
    print("|---|---|---|---|---|---|---|")
    for n in names:
        f = recs[n]["fallback"]
        print("| %s | %s | %s | %s | %.1f | %d | %s |" % (
            n, outcome(n), f["reader_kind"], "yes" if f["reader_high_confidence"] else "",
            f["reader_extracted_bytes"] / 1024.0, f["basic_nodes"],
            "yes" if f["basic_truncated"] else ""))


if __name__ == "__main__":
    main()
