# Content blocking

This page is the reference for PSP owners who want to know exactly what
Tilefinch blocks, and for anyone preparing a custom list. Tilefinch provides
request blocking and conservative cosmetic hiding as performance and comfort
features. **Basic blocking and cosmetic hiding are on by default**, and each
can be changed independently under **Menu → Settings → Privacy & security**.

Tilefinch also hides common cookie-consent overlays by default. This is a
separate comfort policy: it stays active when request blocking is Off, never
clicks an acceptance button, and never creates a consent cookie. **Menu →
Page tools → Site information → Permissions & controls → Cookie notices**
switches
between **Hide** and **Show** for the current registrable site, and at most 16
Show exceptions are kept.

## Modes

- **Off** performs no request matching and disables cosmetic hiding.
- **Basic** matches 34 conservative advertising and auction host suffixes,
  only for third-party subresources. The table is compiled into the browser;
  it performs no boot-time file I/O.
- **Custom** reads `data/adblock.txt`. Switching to Custom fails without
  changing the active mode if the file cannot be read or validated.

**Permissions & controls → Content blocking** bypasses blocking for the
current registrable site. Allowing `en.example.org`, for example, records
`example.org`, so the choice also covers `m.example.org`. The set holds at
most **32** sites, and request matching never reads the Memory Stick.

An optional `data/adblock-allow.txt` can list more bare hostnames or HTTP(S)
URLs, one per line; blank lines and lines starting with `!` or `#` are
ignored. **Menu → Settings → Privacy & security → Load allowlist** reads and
merges that file only when you ask. The import is limited to 32 KiB and 511
bytes per line, merges duplicate sites, and stops safely when the 32-site set
is full. Imported entries are saved with your settings, so the file is a
source to import from, not something read at request time.

Changing a site exception reloads the current page at once, so network work
already under way agrees with the new policy. Cosmetic hiding also changes at
once and is an independent switch: turning off **Hide page ads** leaves
request blocking active.

After a successful load, the bottom bar shows `B` and the number of requests
blocked on that page, and the **Content blocker** row in Settings shows a
small running total across successful loads. Only the count is saved;
Tilefinch keeps no blocked URLs or per-site blocking history.

## Built-in Basic rules

Basic blocks third-party subresources on these 34 dedicated advertising and
auction host suffixes (including their subdomains):

```text
33across.com              adform.net                 adnxs.com
adsrvr.org                amazon-adsystem.com        bidswitch.net
casalemedia.com           contextweb.com             criteo.com
criteo.net                demdex.net                 doubleclick.net
googleadservices.com      googlesyndication.com      indexww.com
lijit.com                 mathtag.com                media.net
moatads.com               openx.net                  outbrain.com
pubmatic.com              quantserve.com             rlcdn.com
rubiconproject.com        serving-sys.com            sharethrough.com
smartadserver.com         smaato.net                 taboola.com
teads.tv                  triplelift.com             yieldmo.com
zedo.com
```

Top-level navigation is never blocked, and a matching site exception bypasses
both Basic/Custom request rules and cosmetic hiding.

Basic and Custom also skip a few narrowly identified inline telemetry
scripts, unless blocking is Off or the site is an exception. See
[Engineering notes](#engineering-notes).

## Built-in cosmetic hiding

Cosmetic hiding is a small user stylesheet, not an image/color transform, so
it does not alter photographs, thumbnails, or other page pixels. It hides the
following conservative markers with `display: none !important`:

```text
ins.adsbygoogle
.advertisement .advertising .ad-banner .ad-container .ad-slot .ad-wrapper
.sponsored .promoted .promotedlink
[data-ad] [data-ad-slot] [data-ad-client]
[data-csa-c-slot-id^="adplacements:"]
[aria-label="advertisement"] [aria-label="sponsored"]
```

It also hides class tokens beginning with `ad-slot-` or `ad-banner-`, at any
position in a whitespace-separated class list. The families deliberately
include a hyphen boundary; arbitrary `ad-*` names, embedded substrings, and
`data-ad*` attribute prefixes are not hidden. The explicit ad-placement
attribute namespace is hidden; ordinary content slots and similar substrings
remain visible. The matched container is removed from flow, including its
own reserved height and margins; the spacing of its unmatched ancestors is
left intact.

It is composed in source order with global font scaling and Reader mode, so
turning on either of those does not discard it. It is on by default, and
**Menu → Settings → Privacy & security → Hide page ads** turns off only this
stylesheet, leaving network blocking active.

## Cookie notices

The built-in cookie-notice sheet recognizes the established roots used by
OneTrust, Cookiebot, Didomi, Quantcast, TrustArc, Cookie Law Info, Osano-style
Cookie Consent, Complianz, Funding Choices, and Sourcepoint, plus a small set
of explicit `cookie-banner`, `cookie-consent`, and `cookie-notice` markers. It
also releases the matching vendors' common scroll-lock classes. Rules continue
to apply when JavaScript inserts a banner after the first paint.

Tilefinch deliberately does not search button text or automatically choose an
Accept/Reject control. Doing so would be language-dependent and could grant
tracking consent without the user seeing it. A site that needs its consent UI
for functionality can be restored immediately with **Cookie notices → Show**;
that setting is independent of the ad-blocking mode and its site exceptions.

## Custom syntax

The parser intentionally implements a low-cost subset of the
[uBlock Origin static network-filter syntax](https://github.com/gorhill/ublock/wiki/static-filter-syntax)
and common EasyList host rules:

```text
! comment
||ads.example^$third-party,script
@@||allowed.ads.example^$script
||images.example^$image
0.0.0.0 tracker.example
tracker2.example
```

Accepted forms are host-suffix block rules (`||host^`), `@@` exceptions,
hosts-file entries, and bare hostnames. Accepted party modifiers are
`third-party`/`3p`, `first-party`/`1p`, and their `~` negations. Accepted
resource modifiers are `script`, `image`, `stylesheet`/`css`, `font`,
`xmlhttprequest`/`xhr`, `subdocument`/`frame`, and `media` (video and audio
opens and range requests). `important` is accepted;
hostname matching is always ASCII case-insensitive. Rules qualified with
`other`, `ping`, or `websocket` are counted as ignored because those
transports do not pass through this policy; keeping them as active
would advertise protection they cannot provide.

Custom-list cosmetic filters, regular expressions, path filters, scriptlets,
redirects, CSP/header rewriting, and domain-scoped modifiers are counted and
ignored.
Tilefinch never approximates unsupported syntax into a broader block. An
exception matching the request wins over a matching block rule.

The file is limited to 512 KiB, 4,096 retained rules, a 128 KiB hostname pool,
and 2,047 bytes per physical line. A missing, oversized, truncated, or
unreadable file leaves the previous configuration active. These limits mean a
full desktop filter bundle may need to be reduced to its supported network
rules before copying it to the PSP. When Custom is selected, the status
message reports both retained and ignored rule counts so unsupported input is
visible instead of silently appearing active. The browser neither downloads
nor updates personal lists itself. The compiled Basic list can change only as
part of an ordinary signed Tilefinch application release. Upstream lists
remain under their own licenses; Tilefinch does not redistribute them.

If memory runs out while a new list loads, the previous list stays active.

The [uBlock filter assets](https://ublockorigin.github.io/uAssets/) and
[EasyList repository](https://github.com/easylist/easylist) are useful sources
when preparing a personal list.

## What blocking saves

Matching occurs before cache lookup, DNS/TLS/HTTP work, body allocation,
decoding, script execution, and rendering. A match therefore removes the full
downstream cost of that request. The actual page-load improvement depends on
which third-party resources a site uses. Cosmetic hiding does not alter the
DOM or prevent a request by itself. Content blocking is not a privacy or
malware-protection boundary.

## Engineering notes

- **Inline telemetry rules.** Basic and Custom refuse narrowly identified
  inline telemetry bundles. Rules require a site boundary, an exact
  first-statement trigger and all reviewed bundle markers; mentioning a
  tracker name is not enough. Matching scans at most 512 KiB without an
  allocation. The original script text stays in the DOM, while execution is
  skipped before its first effect. Reference captures use the same rule
  definitions and record the blocking separately; unrelated script errors
  and crypto refusals remain failures.
- **Cosmetic hiding.** The rules are a fixed set of CSS selectors applied in
  the normal style pass, without another DOM traversal or script. The prefix
  families compile to native scans: the first two bytes reject unrelated
  tokens before a full prefix comparison, and byte loads keep unaligned
  class spans safe on the PSP, with no per-element allocation or added cache.
  The stylesheet is shared verbatim with offline visual-reference capture
  through `src/cosmetic_blocker_css.inc`.
- **Custom lists.** The open-addressed rule index and all its strings are
  charged to the shared session budget. Replacing one custom list with
  another briefly holds both indexes, and an allocation refusal keeps the
  previous list.
- **Saved exceptions** are activated only after the first stable chrome
  frame, so a growing profile cannot hold the screen black during boot.
- **Host lab.** The desktop lab exposes the same policy for deterministic
  testing:

  ```sh
  ./build-preset-release/psp-browser-interactive-lab \
    --fixture fixtures/demo.html --content-blocker basic

  ./build-preset-release/psp-browser-interactive-lab \
    --fixture fixtures/demo.html --content-blocker custom \
    --content-blocker-list /path/to/adblock.txt
  ```
