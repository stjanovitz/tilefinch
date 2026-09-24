# Troubleshooting

Fixes for the problems PSP owners actually run into, roughly in the order
they meet them.

## Wi-Fi won't connect

The PSP's Wi-Fi hardware is from another era. With the latest ARK-4 custom
firmware, WPA2 networks work; otherwise the PSP joins only WPA (TKIP) or open
networks. Most routers can offer a WPA/TKIP guest network for it. Set the
network up in the PSP's own **Settings → Network Settings** first, and make
sure its connection test passes there before suspecting the browser.

Tilefinch connects through a saved PSP connection profile, the first one by
default. To use another, choose it in **Settings → Device & storage → Wi-Fi
profile**. You can also set `network_profile=<N>` (the 1-based position of
the saved connection) in `PSP/GAME/TILEFINCH/data/boot-overrides.cfg`; create
the file if it does not exist, with one `key=value` per line.

## A secure page says its certificate failed

Check the PSP's date and time under **Settings → Date & Time Settings**, then
retry. HTTPS certificates are valid only for a stated date range, so a PSP
whose clock has reset to an old date can make a valid site look untrusted.
Tilefinch keeps the page's failure on the first line of the message and adds
**Try correcting PSP date/time, then retry** on a second line (or **Set PSP
date/time, then retry** when the clock is clearly wrong), rather than showing
a clipped error from the TLS library.

Tilefinch never bypasses certificate checks and never sets the clock from an
unauthenticated network source. If the clock is right and the error persists,
open **Help & diagnostics → Diagnostic QR** and share every QR page; the
report includes the exact clock value and the original certificate error.

## A page won't finish loading

This is by design. Tilefinch gives every page a fixed memory budget out of
the PSP's 64 MB, and a page that outgrows it degrades instead of crashing.
Its scripts may stop ("JavaScript stopped at its memory limit") while the page
stays readable and scrollable, or the navigation fails and you keep the page
you were on. Very heavy pages, such as endless feeds or huge articles with
hundreds of images, are the usual cause. Reader-friendly and mobile pages
work best.

## YouTube stopped working

YouTube changes its side regularly, which eventually breaks the built-in
provider. Check **Settings → Updates → Version and update**: provider fixes
ship as signed updates. If no update is available yet, one is probably on its
way.

## A video won't play

Video is decoded by the PSP's Media Engine with the firmware's own decoder.
Two YouTube renditions are qualified on real hardware: 360p (the default,
scaled to the screen) and 240p, chosen under **Settings → Video → YouTube**.
A source outside the qualified profiles is refused up front, with the reason
shown on the player, rather than started and then dropped. Not every stream
format is supported, so a video can still fail because of its codec or audio
track.

If the decoder has to be shut down during playback, the player says
`VIDEO DECODER NEEDS APP RESTART` and stops offering Retry: only relaunching
Tilefinch brings the decoder back, so Retry would just fail again.

## Sending a diagnostic report without removing the Memory Stick

After an error, open **Help & diagnostics → Diagnostic QR** and press **X**.
Tilefinch compresses the diagnostic log it has already written and shows it
as one or more QR pages.

- Take a clear, straight-on photo of every page, with the page count and
  report ID visible, and share the photos with your report.
- Use the D-pad Left/Right or L/R to change pages; Circle goes back.

Building the report changes no log files and writes no QR file to the Memory
Stick. If there is no diagnostic log yet, the screen says so.
[Diagnostic QR reports](docs/DIAGNOSTIC_QR.md) documents the format and an
optional tool for extracting reports on a computer.

## A site looks broken, or images and media are missing

Tilefinch loads pages over HTTPS first and never silently falls back to HTTP.
On an HTTPS page, insecure (HTTP) scripts and frames are always blocked, and
insecure images and media are tried once over HTTPS and blocked if that
fails. Old sites that still depend on plain-HTTP resources can look broken
as a result.

The fixes are per site, under **Page tools → Site information → Permissions
& controls**:

- **Mixed HTTP (session)** allows HTTP resources on the current site. It lasts
  only until you exit Tilefinch, **deliberately**: a workaround for one old
  site should not become a lasting HTTPS downgrade. If a site needs it on
  every visit, that is the site's problem, and one press is the
  accommodation.
- **Third-party cookies** allows the legacy unpartitioned cookies some sites
  need. This choice is saved, for up to 16 sites.

Also by design, a page from the internet cannot reach devices on your home
network (your router's admin page, a NAS, a printer). Typing a local address
into the address bar yourself still works.

## A site misbehaves because of its scripts

If one site's JavaScript makes it slow or broken, you don't have to turn
scripts off everywhere. **Page tools → Site information → Permissions &
controls → JavaScript** turns scripts off (or back on) for just that site,
while **Settings → Browsing & input → JavaScript** remains the global switch.

## A site forgets your settings, saved games, or logins

Tilefinch keeps what sites store (`localStorage`, `sessionStorage`, and site
files) in RAM, and by default writes none of it to the Memory Stick. Unless
you choose otherwise, a site starts fresh each time you start Tilefinch.
Cookies are never saved, so logins always end at exit. To keep a site's data:

- **Memory Stick for that site.** Open the site, then **Page tools → Site
  information → Permissions & controls → Site storage → Memory Stick**. Its
  data is then kept across restarts, up to 4 MB. You can do the same without
  visiting it from **Settings → Device & storage → Site data & storage**, if
  the site is listed there.
- **When Tilefinch asks.** A site that runs out of room in RAM brings up an
  offer: **Triangle** keeps its data on the Memory Stick for good, **X** only
  for this session. If you said no, the offer returns the next time you start
  Tilefinch. **Offer Memory Stick** in Site data & storage must be **Ask**.
- **Every site's RAM storage at exit.** **Settings → Device & storage → Site
  data & storage → Save RAM storage at exit** writes all sites' `localStorage`
  to `data/local-storage.bin` when you exit normally (not after a crash or a
  power cut), up to 5 MB. It is off by default.

If a site still loses data, check that **Settings → Privacy & security →
Cookies & storage** is **Allow**: with it on Block, sites cannot store
anything.

## Voice search doesn't work well

That's expected: it is experimental, slow, and often inaccurate. It is off by
default under **Settings → Advanced & experimental → Experimental**. Once it is
on, press Square on a focused text field to speak, X to stop, and Circle to
cancel. Recognition runs entirely on the PSP, and nothing you say leaves it.
Password and payment fields never accept voice input.

## The browser won't start after an update

It should recover by itself. A new version that fails its first start is
rolled back automatically, and the launcher's **Tilefinch Safe Start** screen
offers to try the update again, discard it, or start the current version.

You can also start the previous version yourself: **hold L as you launch
Tilefinch from the XMB**. This works once at least one update has been
installed; a fresh install has no previous version yet.

## Where your data lives

Everything you own is in `PSP/GAME/TILEFINCH/data/`:

| File | Contents |
|---|---|
| `profile.cfg` | All settings, bookmarks, history, and YouTube resume points |
| `boot-overrides.cfg` | Your boot settings (`url`, `network_profile`, ...) |
| `recovery.cfg` | The last page and scroll position |
| `http-cache.bin` | Optional disk cache (off by default) |
| `local-storage.bin` | Optional snapshot of in-memory local storage (off by default) |
| `site-storage/` | Storage for sites you allowed on the Memory Stick |
| `update-state.0` / `.1` | The update journal; leave these alone |
| `update/` | Staged update downloads |
| `offline/` | Saved articles and downloaded videos |
| `screenshots/` | PNG captures; delete them over USB when space runs low |

Cookies are kept in RAM only and are gone when the PSP powers off. The
`slot-a/` and `slot-b/` directories are the program itself.
[docs/STORAGE.md](docs/STORAGE.md) is the full engineering map: every file,
its size limit, and how much free space each feature needs.

## Resetting to defaults

Delete `data/profile.cfg` to reset every setting, bookmark, and history
entry. For a completely fresh start, also delete `recovery.cfg`,
`http-cache.bin`, `local-storage.bin`, the `site-storage/` folder, and
`boot-overrides.cfg`. All of them
are recreated with defaults. Some of these files keep one previous generation
beside them as a crash-safety copy (`profile.cfg.bak`, `http-cache.bin.bak`,
`local-storage.bin.bak`); delete the `.bak` file too, or the browser restores
from it.

Two things **not** to delete:

- `data/update-state.0` and `update-state.1`: deleting them can silently
  revert you to an older installed version and destroy the rollback copy.
- Anything under `slot-a/` or `slot-b/`: that is the browser and its trust
  data, and deleting `roots.pem` breaks all HTTPS.

Site data can also be cleared inside the browser, under **Settings → Device &
storage → Site data & storage**: one site at a time from its list, or cache,
cookies, and storage for every site at once.
