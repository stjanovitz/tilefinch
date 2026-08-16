# Troubleshooting

Solutions to the problems PSP owners actually hit, roughly in the order
they hit them.

## Wi-Fi won't connect

The PSP's Wi-Fi hardware is from another era. With the latest ARK-4 custom
firmware, WPA2 networks work; on other firmware the PSP joins only WPA (TKIP)
or open networks — a hardware-era limit, and most routers can enable a
WPA/TKIP guest SSID for it. Set the network up in the PSP's own
**Settings → Network Settings** first and make sure a connection test passes
there before blaming the browser.

Tilefinch connects using a saved PSP connection profile. By default it uses
the first one. If your working profile is not the first, set
`network_profile=<N>` (1-based index of the saved connection) in
`PSP/GAME/TILEFINCH/data/boot-overrides.cfg` — create the file if it does
not exist, one `key=value` per line.

## A page won't finish loading

This is by design. Tilefinch gives every page a fixed memory budget on the
PSP's 64 MB. A page that outgrows the budget degrades instead of crashing:
scripts may stop ("JavaScript stopped at its memory limit") while the page
stays readable and scrollable, or the navigation fails and you keep the page
you were on. Very heavy pages — endless feeds, huge articles with hundreds of
images — are the usual cause. Reader-friendly and mobile pages work best.

## YouTube stopped working

YouTube changes their side regularly and eventually breaks the browsing
provider. Check **Options → Version / Update** — provider fixes ship as
signed updates. If no update is available yet, one is likely coming.

## A video won't play

Video is decoded by the PSP's Media Engine using the firmware's own decoder
programs. Tilefinch has qualified two YouTube renditions on real hardware:
360p (the default, scaled onto the screen) and 240p, selectable under
**Options**. A source outside those qualified profiles is refused up front,
with the reason on the player, rather than started and then dropped. Not
every stream format is supported either, so a video can still fail on its
codec or audio track.

If the decoder has to be shut down mid-playback, the player says
`VIDEO DECODER NEEDS APP RESTART` and stops offering Retry. Nothing short of
relaunching Tilefinch can bring the decoder back, so the button would only
fail again.

## A site looks broken, or images and media are missing

Tilefinch loads pages HTTPS-first and never silently falls back to HTTP.
On an HTTPS page, insecure (HTTP) scripts and frames are always blocked, and
insecure images or media are tried once over HTTPS and blocked if that
fails. Old sites that still depend on plain-HTTP resources can look broken
as a result. The escape hatch is per-site:
**Options → Privacy → "Compatibility: allow HTTP resources here"** (there is
a matching "allow unpartitioned cookies here" for sites that need legacy
third-party cookies).

These grants last only until you exit the browser — **deliberately**. A
temporary workaround for one old site should not become a permanent HTTPS
downgrade that follows you to the next session, so the option re-arms itself
at restart. If a site needs the grant every visit, that is the site's
problem, and the one-press option is the accommodation.

Also by design: a page from the internet cannot reach devices on your home
network (your router's admin page, a NAS, a printer). Typing a local address
into the address bar yourself still works.

## A site misbehaves because of its scripts

If one site's JavaScript makes it slow or broken, you don't have to turn
scripts off everywhere: **Options → "Override scripts for the current
website"** disables (or re-enables) scripts for just that site, while
**"Run scripts on pages across all sites"** remains the global switch.

## Voice search doesn't work well

Correct — it is experimental, and it is slow and often inaccurate. It is off
by default under **Options → Experimental**. When enabled, press Square on a
focused text field to speak, X to stop, Circle to cancel. Recognition runs
entirely on the PSP and nothing you say leaves the device. Password and
payment fields never accept voice input.

## The browser won't start after an update

It should recover by itself: a new version that fails its first start is
rolled back automatically, and the launcher offers to retry or discard the
update. You can also force the previous version manually: **hold L while
launching Tilefinch from the XMB** and keep holding it through the splash
screen ("HOLD L FOR SAFE START"). This works once at least one update has
succeeded; on a fresh install there is no previous version yet.

## Where your data lives

Everything you own is in `PSP/GAME/TILEFINCH/data/`:

| File | Contents |
|---|---|
| `profile.cfg` | All options, bookmarks, history, YouTube resume points |
| `boot-overrides.cfg` | Your boot settings (`url`, `network_profile`, ...) |
| `recovery.cfg` | Last page and scroll position |
| `http-cache.bin` | Optional disk cache (off by default) |
| `local-storage.bin` | Optional site storage (off by default) |
| `update-state.0` / `.1` | Update journal — leave these alone |
| `update/` | Staged update downloads |
| `offline/` | Saved articles and downloaded videos |
| `screenshots/` | PNG captures — delete over USB when space runs low |

Cookies are held in RAM only and are gone at power-off. The `slot-a/` and
`slot-b/` directories are the program itself. The full engineering map —
every file, its size bound, and how much free space each feature needs — is
in [docs/STORAGE.md](docs/STORAGE.md).

## Resetting to defaults

Delete `data/profile.cfg` to reset every option, bookmark, and history
entry; delete `recovery.cfg`, `http-cache.bin`, `local-storage.bin`, and
`boot-overrides.cfg` for a fully fresh start. All of these are recreated
with defaults. Some of these files keep one previous generation beside them
as a crash-safety copy (`profile.cfg.bak`, `http-cache.bin.bak`,
`local-storage.bin.bak`); delete the `.bak` alongside the file, or the
browser restores from it. Two things NOT to delete:

- `data/update-state.0` and `update-state.1` — deleting them can silently
  revert you to an older installed version and destroy the rollback copy.
- Anything under `slot-a/` or `slot-b/` — that is the browser and its trust
  data; deleting `roots.pem` breaks all HTTPS.

Site data can also be cleared from inside the browser under
**Options → Site Data** (cache, cookies, and storage independently).
