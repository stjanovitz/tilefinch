# Memory Stick storage

What Tilefinch writes to the Memory Stick, how large each file can get, what
reclaims it, and what happens to it on a power cut. The stick is FAT with no
journal and roughly 1 MB/s writes, so every writer here is explicit about its
replacement discipline. User-facing guidance ("where your data lives", what is
safe to delete) is in the root
[TROUBLESHOOTING](../TROUBLESHOOTING.md#where-your-data-lives); this page is
the engineering contract behind it.

## Install shapes

A slotted install (the shipped layout) lives at `ms0:/PSP/GAME/TILEFINCH/`:

```
TILEFINCH/
  EBOOT.PBP          frozen launcher (never rewritten by updates)
  slot-a/  slot-b/   program slots; one is active, the other is the
                     rollback copy or the update staging target
  data/              everything the browser writes at runtime
```

`install_paths.c` derives this from `argv[0]`: a program running from
`slot-a/` or `slot-b/` uses the shared sibling `data/` directory. Any other
location (development, flat installs) is unslotted: the data directory is the
program directory itself, and the same file names apply beside the EBOOT.

## Replacement disciplines

FAT on the PSP does not rename onto an existing file, so "write tmp, rename
over" alone does not work once a file exists. Three disciplines are in use;
each file below names the one it uses.

- **tmp + remove + rename** — write `<file>.tmp`, close, `remove(<file>)`,
  `rename`. On PSP the remove-then-rename branch is the common path, so there
  is a window (one remove plus one rename) in which a power cut leaves neither
  the old nor the new file in place; only the completed `.tmp` remains, and
  loaders do not read it. Files using this discipline are recreated with
  defaults after such a loss.
- **backup rotation** — write `<file>.tmp`, rotate `<file>` to `<file>.bak`
  (on FAT the rotation removes the older backup first and retries), rename the
  tmp into place, and roll the backup back if that rename fails. A power cut
  at any point leaves at least one complete generation, and the loader falls
  back to `.bak` when the primary is missing or unreadable — a failed checksum
  for the formats that carry one, a failed parse for `profile.cfg`.
- **dual-copy journal** — two fixed-size records (`update-state.0`/`.1`) with
  a generation counter; every store writes the older copy via tmp + rename,
  syncs the file and then the device, and the loader selects the valid record
  with the higher generation. This is the strongest discipline and guards the
  update state machine.

Every free-space query in the tree goes through
`tilefinch_update_query_free_space()` (`src/update_storage.c`), which reduces
the path to the device string (`ms0:`) on PSP — full directory paths are
rejected by the device service — and uses the full path on host builds.

## Layout map

Sizes are hard bounds from the code, not typical sizes; typical sizes are far
smaller. "Writer" names the owning source file.

### `data/` — settings and session

| Path | Writer | Size bound | Evicted by | Discipline |
|---|---|---|---|---|
| `profile.cfg` (+`.bak`) | `browser_profile.c` | ~0.5 MB per generation (100 history + 32 bookmarks + 16 resume points, every field length-capped); typically a few KB | replaced on every save | backup rotation; the loader falls back to `.bak` when the primary is missing or unparsable |
| `recovery.cfg` | `browser_profile.c` | one checksummed line (≤ ~3.2 KB) | cleared on clean navigation state; ignored if checksum fails | tmp + remove + rename |
| `tabs-session.bin` (+`.bak`) | `browser_tabs.c` | ≤ ~82 KB per generation (5 tabs × 16 KB payload + headers) | rewritten at exit; removed when "restore last page" is off | backup rotation; restore falls back to `.bak` |
| `tab-hibernation.bin` | `browser_tabs.c` | ≤ 16 KB + 16 B header | removed on rehydrate, on disabling hibernation, and at clean exit | tmp + remove + rename |
| `http-cache.bin` (+`.bak`) | `session_persistence.c` | ≤ 5 MB per generation (payload further capped by the 1/2/4 MB cache setting) | user setting 0 removes it; "clear cache" removes it | backup rotation; a torn primary is removed after `.bak` recovery |
| `local-storage.bin` (+`.bak`) | `session_persistence.c` | ≤ 5 MB per generation | disabling the setting removes it; "clear local storage" removes it | backup rotation, as above |
| `tls-sessions.bin` (+`.bak`) | `tls_session_store.c` | ≤ 64 KB per generation (≤ 16 hosts × 2 sessions × 4 KB) | "clear HTTP caches" removes it; pruned of expired entries at load and save | backup rotation; a torn primary is removed after `.bak` recovery, and the whole file is a plain miss on any checksum, store-version, or Mbed-TLS-version-pin mismatch |
| `boot-overrides.cfg` | `psp_boot_config.c` | 4 bounded lines | user-managed | tmp + remove + rename |
| `adblock.txt`, `adblock-allow.txt` | user-provided | read-only | user-managed | n/a |

Each discipline can briefly hold up to three generations of a file
(`.tmp` + `.bak` + primary) while a save is in flight; budget accordingly for
the two 5 MB site-data stores. Stale `.tmp` files from an interrupted save are
truncated and reused by the next save of the same file.

`http-cache.bin` and `local-storage.bin` are written **only during the exit
cleanup** — there is no periodic mid-session save — so a power cut during a
session loses everything cached or stored since the last clean exit, even
though the files themselves are backup-rotated and always readable. The exit
save pre-flights free space and reports a refusal or failure on screen
(see [Free-space requirements](#free-space-requirements)) instead of only
logging `site-data-save … save-failed`.

By default, `tls-sessions.bin` is written only during explicit suspend or
controlled-exit flushes (curl's session cache is dumped there through
`curl_easy_ssls_export`) and read back once when the transport is first
brought up, on the owned PSP transport only. The global **TLS ticket saving**
preference removes and suppresses this file while leaving process-local reuse
enabled. At ≤ 64 KB it is negligible beside the two 5 MB
stores and needs no separate free-space pre-flight; a save that cannot
complete simply leaves the prior generation and the next connection pays a
full handshake. It holds no browsing content, only the opaque TLS resumption
tickets, which are cleared with the caches.

`psp_boot_config_write_overrides()` both creates and replaces
`boot-overrides.cfg`. The compatibility import calls it only when the overrides
file is absent; the function is also safe as a general rewrite primitive.
Being tmp + remove + rename, it carries the same brief window as the other
files in that row: a power cut there loses the overrides, which are then
recreated with defaults.

### `data/update/` and the slots — signed updates

| Path | Writer | Size bound | Evicted by | Discipline |
|---|---|---|---|---|
| `data/update-state.0` / `.1` (+`.tmp`) | `update_journal.c` | 174 B each | never (rollback state) | dual-copy journal, file + device sync |
| `data/update/package.part` | `update_client.c` | ≤ 32 MB (manifest-capped) | removed on cancel, on failed verification, and by the install job when it reaches COMPLETE; swept at update-session startup whenever the journal shows no trial in flight; truncated by the next download | single file, verified by size + SHA-256 before use |
| `slot-a/`, `slot-b/` | `update_installer.c` | ≈ package size each (packages store files uncompressed; offsets must sum to the package size) | the non-active slot is replaced by the next update | staged as `slot-X.tmp` (per-file SHA-256 + sync, then `slot.tfum`, `slot.tfut`, `READY` markers, device sync), promoted by directory rename with `.old` fallback |
| `slot-X.tmp/`, `slot-X.old/` | `update_installer.c` | ≈ package size, transient | removed by the next install attempt (bounded sweep: ≤ 256 entries, depth ≤ 8) | n/a |

Space checks are pre-flight, honest, and use the device-string query:

- **Before downloading** a package: `2 × package_size + 4 MB` free, else
  "NOT ENOUGH FREE SPACE FOR UPDATE" (`update_client.c`).
- **Before extracting** into the staging slot: `package_size + 4 MB` free,
  else "NOT ENOUGH FREE SPACE TO INSTALL" (`update_installer.c`).
- If the stick fills **mid-download** or **mid-extraction** despite the
  margin, the failure is reported as a generic download/extraction error, the
  part file or staging tree is left for the next attempt's cleanup, and both
  slots and the journal are untouched — the running version is never at risk.

Peak stick usage for a staged update, beyond the browser's own data files:
both slots (≈ package size each), `package.part` (≤ package size), and the
staging tree (≈ package size) coexist between download and promotion, with the
retired slot briefly held as `slot-X.old` before its bounded removal. With the
32 MB package ceiling that is ≤ 128 MB absolute; for a release of size *P*,
budget `2 × P + 4 MB` free at download time — the governing pre-flight — on
top of the installed `2 × P` footprint.

### `components/` — optional signed data

Optional components are siblings of the browser slots so A/B browser updates
neither duplicate nor delete them. Voice uses `components/voice-en-us/`;
glyph packs use `components/glyph-ja/`, `glyph-zh-hans/`, `glyph-zh-hant/`,
`glyph-ko/`, and `glyph-emoji-color/`.

Each glyph directory has bounded `candidate.tmp`, `active`, and `previous`
generations. A completed generation contains `pack.tfgf`, the verified signed
envelope/metadata, and a `READY` marker written last. Promotion rotates active
to previous; removal first writes and device-syncs `UNINSTALLED`, then deletes
both generations. The resolver honors that marker before either generation,
so a power cut cannot resurrect a removed pack. The next verified promotion
is the only operation that clears it.

TFGF files are capped at 32 MB, although the published regional packs are
expected around 1 MB and color emoji around 5 MB. With Embedded selected,
Tilefinch performs no glyph-component storage I/O at boot. A selected pack's
signed identity and bounded index are read once; page rendering queues cache
misses and the app pump reads at most one 16 KiB block per call. This runtime
path never writes. Install/update uses the shared
`data/update/glyph-component.part`, removes it after completion/cancel/failure,
and preflights the signed download and candidate space through the PSP-safe
free-space helper.

### `data/offline/` — offline library

| Path | Writer | Size bound | Evicted by | Discipline |
|---|---|---|---|---|
| `library.bin` (+`.bak`) | `offline_library.c` | ≤ 32 KB | never (12-item index) | backup rotation; loader tries primary, `.tmp`, then `.bak` |
| `<id>.article.html` (+`.bak`) | `offline_library.c` | ≤ 1 MB each | user delete; replaced when the same URL is saved again | backup rotation; length + FNV checksum verified on read |
| `<id>.video.mp4`, `<id>.audio.mp4` | `offline_download.c` | ≤ 512 MB per stream | user delete | `.part` renamed into place after exact-length ranged download |
| `<id>.video.part`, `<id>.audio.part` | `offline_download.c` | ≤ stream size | resumed, or reclaimed by the orphan sweep | append-only; size re-validated against the index on load |

The library holds at most 12 items. Saves are refused up-front with honest
messages when space is short: articles need `1 MB + 256 KB` free
("not enough free Memory Stick space"), video downloads need the remaining
bytes plus an 8 MB reserve (the item is set to *paused*, not failed). Each
`offline_library_load()` runs a bounded sweep (≤ 128 directory entries) that
deletes recognized library-named files whose id is not in the index, so parts
and articles stranded by a power cut between file publication and index
promotion are reclaimed lazily. Files it does not recognize are never touched.

### `data/screenshots/` — captures

| Path | Writer | Size bound | Evicted by | Discipline |
|---|---|---|---|---|
| `tilefinch-<date>-<tick>.png` | `screenshot_png.c` | ≈ 385 KB each (480×272, uncompressed PNG) | **nothing** — manual deletion over USB only | tmp + rename (names are unique, so no replacement window) |
| `tilefinch-<...>.png.tmp` | `screenshot_png.c` | ≈ 385 KB, transient | removed when its capture finishes or is cancelled; orphans swept when the directory is opened for the next capture | n/a |

There is still no count cap and the in-browser list shows at most 32
entries, so older captures keep consuming space silently. Two things are
bounded: a capture interrupted by a crash strands a `.png.tmp`, and the next
capture sweeps those (bounded walk of ≤ 128 directory entries; only names
this writer could have produced are removed); and each capture pre-flights
free space with `tilefinch_update_query_free_space` before it starts,
refusing with "MEMORY STICK FULL - SCREENSHOT NOT SAVED" when the query
reports under 1 MB. If the query is unavailable the capture proceeds rather
than refusing on a guess, and a write that then fails reports the same
full-stick message when free space is short, or "SCREENSHOT SAVE FAILED"
otherwise.

### Diagnostic builds only

With `TILEFINCH_PSP_VALIDATION_LOG=ON` (off in release builds), a slotted
installation writes `data/tilefinch-validation.txt` and
`data/tilefinch-crash.txt` under the shared `Tilefinch/` directory. A
standalone diagnostic EBOOT writes them beside itself. Each is rotated to a
single `.previous` generation per boot. The crash record is a fixed 512-byte
overwrite-in-place slot; the validation log is append-only and unbounded
within one session. The diagnostic build identifies the directory on its
boot surface and in a startup status message. `dump_frame` boot options write
fixed-name `.ppm` frames beside the EBOOT.

### On-screen diagnostic QR

**Options → System → Diagnostic QR** is a read-only transport for the existing
logs. A release build reads `tilefinch-last-error.txt` if present. A validation
build can additionally read the current and previous validation/crash logs;
because the current validation log is normally buffered, the explicit
**Build report** action flushes it once before reading. There are no reads on
boot or any frame path, and report paging performs no further Memory Stick
I/O. Closing the screen frees the in-memory compressed bundle. No QR image,
copy, index, or cache is written to storage. See [Diagnostic QR
reports](DIAGNOSTIC_QR.md) for the format and bounds.

## Free-space requirements

Hard-bound figures; the browser refuses (with the quoted message) rather than
degrading when a pre-flight fails.

| Activity | Minimum free space | Enforced by |
|---|---|---|
| Browsing, site data off | ~2 MB (settings, recovery, tab session, all generations) | not enforced; writers fail individually and keep the prior generation |
| Browsing with disk cache and local storage on | + up to 15 MB per enabled store (3 × 5 MB generations during a save; disk-cache payload further capped at 4 MB) | pre-flight in the exit cleanup (`psp_script_main.c`): one whole new generation per enabled store — the configured cache size (1/2/4 MB) plus 5 MB for local storage — else the save is refused with "MEMORY STICK FULL - SITE DATA NOT SAVED" and the prior generation is kept |
| Saving an offline article | 1.25 MB (1 MB article + 256 KB reserve) | pre-flight in `offline_library_save_article` |
| Downloading an offline video | remaining stream bytes + 8 MB reserve | pre-flight in `offline_download.c` (pauses, does not fail, the item) |
| Downloading + staging an update for a release of size *P* | `2 × P + 4 MB` (≤ 68 MB at the 32 MB package ceiling) | pre-flights in `update_client.c` and `update_installer.c` |
| Taking a screenshot | 1 MB (one ≈ 385 KB capture plus headroom) | pre-flight in `psp_screenshot_destination`; total stored captures are still unbounded |

## What survives a power cut

- **Always**: both program slots, the update journal, the offline index and
  every published article/video (backup-rotated or immutable), and site data
  (backup-rotated).
- **At-most-one-generation loss**: settings, bookmarks and history
  (`profile.cfg` falls back to `.bak`, so a cut costs at most the save that
  was in flight), tab session (falls back to `.bak`), in-flight video `.part`
  progress (re-measured on next load).
- **Lost if the cut lands in the remove-rename window**: `recovery.cfg`
  (start page falls back to the homepage), `tab-hibernation.bin` (that tab's
  history), `boot-overrides.cfg` (written by the one-time compatibility
  import). These are the tmp + remove + rename files above; they are the
  only files on the stick without a recovery generation.
