# Content blocking

Tilefinch provides bounded request blocking and conservative cosmetic hiding
as performance and comfort features. **Basic blocking and cosmetic hiding are
on by default**, and each can be changed independently under **Settings →
Privacy & security**.

Tilefinch also hides common cookie-consent overlays by default. This is a
separate comfort policy: it stays active when request blocking is Off, never
clicks an acceptance button, and never creates a consent cookie. **Page tools
→ Site information → Permissions & controls → Cookie notices** switches
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
current registrable site. Allowing `en.wikipedia.org`, for example, records
`wikipedia.org`, so the choice also covers the mobile hostname. The active set
holds at most **32** sites (about 4 KiB at worst), and request matching never
reads the Memory Stick. Saved exceptions are deliberately activated only after
Tilefinch has shown its first stable chrome frame, so a growing profile cannot
hold the screen black during boot.

An optional `data/adblock-allow.txt` can list more bare hostnames or HTTP(S)
URLs, one per line; blank lines and lines starting with `!` or `#` are
ignored. **Settings → Privacy & security → Load allowlist** reads and merges
that file only when you ask. The import is limited to 32 KiB and 511 bytes per
line, deduplicates registrable sites, and stops safely when the 32-site set is
full. Imported entries join the next transactional profile save, so the file
is a source to import from, not storage consulted at request time.

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
smartadserver.com         smaato.net                  taboola.com
teads.tv                  triplelift.com             yieldmo.com
zedo.com
```

Top-level navigation is never blocked, and a matching site exception bypasses
both Basic/Custom request rules and cosmetic hiding.

## Built-in cosmetic hiding

Cosmetic hiding is a small user stylesheet, not an image/color transform, so
it does not alter photographs, thumbnails, or other page pixels. It hides the
following conservative markers with `display: none !important`:

```text
ins.adsbygoogle
.advertisement .advertising .ad-banner .ad-container .ad-slot
.sponsored .promoted .promotedlink
[data-ad] [data-ad-slot] [data-ad-client]
[aria-label="advertisement"] [aria-label="sponsored"]
```

It is composed in source order with global font scaling and Reader mode, so
turning on either of those does not discard it. It is on by default, and
**Settings → Privacy & security → Hide page ads** turns off only this
stylesheet, leaving network blocking active.

## Cookie notices

The built-in cookie-notice sheet recognizes bounded, established roots used by
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
transports do not currently enter this policy seam; retaining them as active
would advertise protection they cannot provide.

Custom-list cosmetic filters, regular expressions, path filters, scriptlets,
redirects, CSP/header rewriting, and domain-scoped modifiers are counted and
ignored.
Tilefinch never approximates unsupported syntax into a broader block. An
exception matching the request wins over a matching block rule.

The file is limited to 512 KiB, 4,096 retained rules, a 128 KiB hostname
pool, and 2,047 bytes per physical line. The open-addressed index and all
strings use the shared session budget; a missing, oversized, truncated, or
unreadable file leaves the previous configuration active. These bounds mean a
full desktop filter bundle may need to be reduced to its supported network
rules before copying it to the PSP. When Custom is selected, the status message
reports both retained and ignored rule counts so unsupported input is visible
instead of silently appearing active. The browser neither downloads nor
updates personal lists itself. The compiled Basic list can change only as part
of an ordinary signed Tilefinch application release. Upstream lists remain
under their own licenses; Tilefinch does not redistribute them.

A maximum-size active custom index retains about 176 KiB. Transactional
Custom-to-Custom replacement can briefly hold the old and candidate indexes
together, so its policy-data peak is about 352 KiB; allocation refusal simply
keeps the previous list.

The [uBlock filter assets](https://ublockorigin.github.io/uAssets/) and
[EasyList repository](https://github.com/easylist/easylist) are useful sources
when preparing a personal list.

The host lab exposes the same policy for deterministic testing:

```sh
./build-preset-release/psp-browser-interactive-lab \
  --fixture fixtures/demo.html --content-blocker basic

./build-preset-release/psp-browser-interactive-lab \
  --fixture fixtures/demo.html --content-blocker custom \
  --content-blocker-list /path/to/adblock.txt
```

## What blocking saves

Matching occurs before cache lookup, DNS/TLS/HTTP work, body allocation,
decoding, script execution, and rendering. A match therefore removes the full
downstream cost of that request. The actual page-load improvement depends on
which third-party resources a site uses. Cosmetic hiding does not alter the
DOM or prevent a request by itself. Content blocking is not a privacy or
malware-protection boundary.
