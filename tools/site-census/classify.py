#!/usr/bin/env python3
"""Hand-maintained page-script classifier for the site census.

classify(script_url, page_url, source_kind) -> category

Categories, in the order they are tested:
  bot-detection                                 (challenge and fingerprinting
                                                 scripts; required for access)
  ads, tag-manager, consent, ab-testing, analytics, chat, social  (non-essential)
  framework                                     (shared libraries and CDNs)
  first-party                                   (the site's own code; inline)
  third-party                                   (anything else)

Rules are host suffixes and path substrings, plus the hosts of Tilefinch's
basic content-blocker list (src/content_blocker.c), which count as ads.
Order matters: a Google Tag Manager URL is a tag manager, not analytics.
"""
import re
import sys
from urllib.parse import urlsplit

NON_ESSENTIAL = ("ads", "tag-manager", "consent", "ab-testing", "analytics",
                 "chat", "social")

# Tilefinch's basic content-blocker hosts (src/content_blocker.c basic_hosts).
BLOCKER_HOSTS = (
    "33across.com", "adform.net", "adnxs.com", "adsrvr.org",
    "amazon-adsystem.com", "bidswitch.net", "casalemedia.com",
    "contextweb.com", "criteo.com", "criteo.net", "demdex.net",
    "doubleclick.net", "googleadservices.com", "googlesyndication.com",
    "indexww.com", "lijit.com", "mathtag.com", "media.net",
    "moatads.com", "openx.net", "outbrain.com", "pubmatic.com",
    "quantserve.com", "rlcdn.com", "rubiconproject.com",
    "serving-sys.com", "sharethrough.com", "smartadserver.com",
    "smaato.net", "taboola.com", "teads.tv", "triplelift.com",
    "yieldmo.com", "zedo.com",
)

RULES = [
    ("bot-detection", ("challenges.cloudflare.com", "/cdn-cgi/challenge-platform",
                       "captcha-delivery.com", "datadome", "dd.reuters.com/tags",
                       "/tags.js", "/akam/", "perimeterx", "px-cdn", "px-cloud",
                       "/149e9513-01fa-4fb0-aad4-566afd725d1b/", "ips.js", "kasada",
                       "awswaf", "token.awswaf", "recaptcha", "hcaptcha",
                       "fingerprint", "fpjs", "arkoselabs", "funcaptcha",
                       "imperva", "incapsula", "_Incapsula_", "distil")),
    ("ads", ("googletagservices.com", "adsafeprotected.com", "doubleverify.com",
             "imasdk.googleapis.com", "pubads.g.doubleclick.net", "adservice.google.",
             "prebid", "amazon-adsystem.com", "connatix.com", "primis.tech",
             "nativo.com", "kargo.com", "33across.com", "permutive.com",
             "permutive.app", "id5-sync.com", "liadm.com", "tapad.com",
             "btloader.com", "ad-delivery.net", "adthrive", "mediavine",
             "freestar", "pub.network", "carbonads.net", "buysellads",
             "ezoic", "aniview.com", "vidazoo.com", "seedtag.com",
             "confiant-integrations.net", "clean.gg", "blockthrough",
             "admiral", "getadmiral.com", "/ads/", "/ad-", "adsbygoogle",
             "pagead", "gpt.js", "apstag", "headerbidding", "/hb.js",
             "bounceexchange.com", "bouncex.net", "zergnet.com",
             "revcontent.com", "mgid.com", "yieldlove", "adlightning",
             "geoedge.be", "doubleclick", "googleadservices", "rumble.com/embedJS")),
    ("tag-manager", ("googletagmanager.com/gtm.js", "googletagmanager.com/gtag",
                     "tags.tiqcdn.com", "tealium", "assets.adobedtm.com",
                     "launch-", "ensighten", "segment.com/analytics.js",
                     "cdn.segment.com", "tagcommander", "commandersact",
                     "/gtm.js", "tagmanager", "/utag.js", "/utag.")),
    ("consent", ("cookielaw.org", "onetrust", "cookiebot", "consensu.org",
                 "quantcast.mgr", "quantcast.com/choice", "trustarc",
                 "truste.com", "didomi", "sourcepoint", "privacy-mgmt.com",
                 "sp-prod.net", "usercentrics", "consentmanager",
                 "cookieyes", "termly.io", "iubenda", "osano.com",
                 "fundingchoicesmessages.google.com", "/cmp", "consent",
                 "gdpr", "__tcfapi", "privacy-center")),
    ("ab-testing", ("optimizely", "cdn.optimizely.com", "abtasty",
                    "vwo.com", "visualwebsiteoptimizer", "launchdarkly",
                    "split.io", "statsig", "growthbook", "kameleoon",
                    "convert.com", "dynamicyield", "monetate", "qubit",
                    "adobe.com/target", "/at.js", "mbox", "eppo",
                    "amplitude.com/experiment")),
    ("analytics", ("google-analytics.com", "googletagmanager.com",
                   "analytics.", "/analytics", "chartbeat", "scorecardresearch",
                   "comscore", "newrelic", "nr-data.net", "js-agent.newrelic",
                   "omtrdc.net", "2o7.net", "adobe.com/analytics", "appmeasurement",
                   "hotjar", "fullstory", "mouseflow", "clarity.ms",
                   "quantcount", "parsely", "parse.ly", "sentry",
                   "sentry-cdn", "bugsnag", "datadoghq", "browser-intake",
                   "datadog-rum", "speedcurve", "lux.js", "mpulse", "akamai.net/boomerang",
                   "boomerang", "go-mpulse", "heap-", "heapanalytics",
                   "mixpanel", "amplitude", "snowplow", "sp.js", "piwik",
                   "matomo", "plausible", "simpleanalytics", "fathom",
                   "cloudflareinsights.com", "beacon.min.js", "rum.",
                   "/rum", "web-vitals", "webvitals", "pingdom",
                   "quantserve", "nielsen", "imrworldwide", "krxd.net",
                   "bluekai", "brightcove/analytics", "chartbeat.com",
                   "statcounter", "yandex.ru/metrika", "mc.yandex",
                   "facebook.net/en_us/fbevents", "fbevents", "connect.facebook.net/signals",
                   "bat.bing.com", "snap.licdn.com", "insight.min.js",
                   "static.ads-twitter.com", "analytics.tiktok.com",
                   "redditstatic.com/ads", "pixel", "tracking", "tracker",
                   "telemetry", "metrics", "beacon", "/collect", "/stats",
                   "gtag", "ga.js", "urchin", "dpm.", "sb.scorecardresearch")),
    ("chat", ("intercom", "zendesk", "zdassets", "drift.com", "driftt",
              "livechat", "olark", "tawk.to", "crisp.chat", "freshchat",
              "hubspot", "hs-scripts", "hs-analytics", "qualified.com",
              "liveperson", "lpsnmedia", "salesforce-chat", "gorgias",
              "helpscout", "ada.support", "kustomer")),
    ("social", ("platform.twitter.com", "widgets.js", "connect.facebook.net",
                "facebook.com/plugins", "instagram.com/embed",
                "platform.instagram.com", "tiktok.com/embed", "embed.reddit",
                "platform.linkedin.com", "assets.pinterest.com",
                "addthis", "sharethis", "disqus", "spot.im", "openweb",
                "viafoura", "telegram.org/js", "embedly",
                "youtube.com/iframe_api", "s.ytimg.com", "player.vimeo")),
    ("framework", ("jquery", "react", "vue", "angular", "svelte", "preact",
                   "polyfill", "core-js", "lodash", "underscore", "moment",
                   "requirejs", "require.js", "bootstrap", "modernizr",
                   "cdnjs.cloudflare.com", "cdn.jsdelivr.net", "unpkg.com",
                   "ajax.googleapis.com", "code.jquery.com", "webpack",
                   "runtime", "vendor", "framework", "chunk", "_next/static",
                   "_nuxt", "gstatic.com/_", "zone.js", "stencil",
                   "ember", "backbone", "hammer", "swiper", "gsap",
                   "turbo", "stimulus", "htmx", "alpine", "fontawesome",
                   "kit.fontawesome")),
]

# Same-site hosts that are still first-party CDNs (served from a sister
# domain): registrable-domain matches are first-party.
FIRST_PARTY_ALIASES = {
    "cnn.com": ("cnn.io", "turner.com", "warnermediacdn.com"),
    "bbc.com": ("bbc.co.uk", "bbci.co.uk", "files.bbci.co.uk"),
    "bbc.co.uk": ("bbc.com", "bbci.co.uk"),
    "nytimes.com": ("nyt.com",),
    "theguardian.com": ("guim.co.uk", "guardianapis.com"),
    "washingtonpost.com": ("wpcomwidgets.com", "arcpublishing.com"),
    "wikipedia.org": ("wikimedia.org",),
    "wiktionary.org": ("wikimedia.org",),
    "reddit.com": ("redditstatic.com", "redd.it", "redditmedia.com"),
    "github.com": ("githubassets.com", "githubusercontent.com"),
    "gitlab.com": ("gitlab-static.net", "assets.gitlab-static.net"),
    "youtube.com": ("ytimg.com", "googlevideo.com", "ggpht.com"),
    "imdb.com": ("media-amazon.com", "images-amazon.com"),
    "amazon.com": ("media-amazon.com", "images-amazon.com", "ssl-images-amazon.com"),
    "ebay.com": ("ebaystatic.com", "ebayimg.com"),
    "etsy.com": ("etsystatic.com",),
    "steampowered.com": ("steamstatic.com", "akamaihd.net"),
    "espn.com": ("espncdn.com",),
    "medium.com": ("medium.com", "cdn-client.medium.com"),
    "substack.com": ("substackcdn.com",),
    "astralcodexten.com": ("substackcdn.com", "substack.com"),
    "wordpress.com": ("wp.com", "wordpress.com"),
    "stackoverflow.com": ("sstatic.net",),
    "fandom.com": ("nocookie.net", "wikia.nocookie.net"),
    "ign.com": ("ignimgs.com",),
    "gamespot.com": ("gamespot.com", "cbsistatic.com"),
    "bsky.app": ("bsky.social", "bsky.network"),
    "weather.com": ("weather.com", "wxug.com"),
    "rottentomatoes.com": ("flixster.com", "rottentomatoes.com"),
    "aljazeera.com": ("aljazeera.net",),
}


def registrable(host):
    """Last two labels, or three for common two-level public suffixes."""
    parts = [p for p in (host or "").lower().split(".") if p]
    if len(parts) >= 3 and parts[-2] in ("co", "com", "org", "net", "ac", "gov") \
            and len(parts[-1]) == 2:
        return ".".join(parts[-3:])
    return ".".join(parts[-2:])


def _host(url):
    try:
        return (urlsplit(url).hostname or "").lower()
    except ValueError:
        return ""


def is_first_party(script_host, page_host):
    if not script_host:
        return True
    s, p = registrable(script_host), registrable(page_host)
    if s == p:
        return True
    return s in FIRST_PARTY_ALIASES.get(p, ())


def classify(script_url, page_url, kind="external"):
    """Return the census category of one page script."""
    if kind == "inline" or not script_url or script_url.startswith("<") \
            or "#inline" in script_url \
            or script_url.startswith("data:") or script_url.startswith("blob:"):
        # Inline scripts are attributed by content markers only when the
        # driver passes the inline text; by name they are first-party.
        return "first-party"
    url = script_url.lower()
    host = _host(url)
    if any(host == h or host.endswith("." + h) for h in BLOCKER_HOSTS):
        return "ads"
    page_host = _host(page_url)
    first = is_first_party(host, page_host)
    for category, needles in RULES:
        if category == "framework":
            continue
        for needle in needles:
            if needle in url:
                # Generic words (pixel, metrics, beacon, consent, /ads/) only
                # count on third-party hosts: sites name their own bundles
                # after features.
                if first and re.fullmatch(r"[a-z/_.-]+", needle) and needle in (
                        "pixel", "tracking", "tracker", "telemetry", "metrics",
                        "beacon", "/collect", "/stats", "/analytics", "analytics.",
                        "consent", "gdpr", "/cmp", "/ads/", "/ad-", "rum.", "/rum",
                        "launch-", "mbox", "sp.js", "heap-", "lit", "coral",
                        "/at.js", "tagmanager", "widgets.js", "turbo"):
                    continue
                return category
    if first:
        return "first-party"
    for needle in dict(RULES)["framework"]:
        if needle in url:
            return "framework"
    return "third-party"


def is_essential(category):
    return category not in NON_ESSENTIAL


if __name__ == "__main__":
    page = sys.argv[1]
    for url in sys.argv[2:]:
        print(classify(url, page), url)
