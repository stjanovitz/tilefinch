# Secure in-app updates

Tilefinch can download and install a new browser release over Wi-Fi without
requiring the user to remove the Memory Stick. Open **Options → System →
Version and update** to check manually. Tilefinch shows the available version
and release note before downloading, and it never installs or restarts without
confirmation. Its optional background check only looks for signed release
metadata while Wi-Fi is already connected; it never downloads the update
package by itself.

Stable also offers **Square → Previous versions** on this page. The bounded
list is queried from GitHub only when this screen is opened, retained in RAM,
and never written to the Memory Stick. Choosing a version only selects its
fixed tagged metadata URL; the user must still check, download, install, and
restart explicitly.

An update is installed into the inactive of two browser slots. The small
launcher verifies the candidate before its first start, keeps the previous
slot intact, and rolls back on the next launch if the new browser does not
reach its health checkpoint. Holding L during startup selects the previous
version. The user's profile, bookmarks, downloads, and other shared data live
outside both browser slots.

Stable is the default channel. Stable and Beta accept only releases authorized
by Tilefinch's embedded public key; the private signing key is never shipped to
the PSP or stored in this repository. The Developer channel is a separate,
explicitly selected mode for testing a contributor's unsigned build from a URL
entered on the device. It retains A/B installation and rollback, but it does
not provide authenticity or a code sandbox.

For contributors and release engineers, the implementation combines a fixed
binary manifest, signed package size and SHA-256, per-file digests, a
sequence-number anti-rollback floor, redundant state records, and a stable A/B
launcher. The release preset embeds `trust/root-v1.tfur`; Stable and Beta chain
to that record, while Developer remains unreachable until both its local URL
and channel selection are configured. The host fault suite, production-root
proof, package cut, and isolated HTTPS PPSSPP flow provide deterministic
qualification. Publishing also requires the offline signing and post-upload
checks in [Release process](RELEASE_PROCESS.md).

## Security guarantees

**Stable and Beta require a valid signature end to end and cannot be talked
out of it.** Both fetch from fixed GitHub HTTPS endpoints compiled into the
build; neither reads a configurable URL. Every candidate is rejected unless
its root chain replays from the embedded anchor and its manifest meets the
release threshold, and the signed manifest is what authorizes the package's
exact size and SHA-256, then every extracted file's exact path, size, offset
and digest. A failure on the selected channel stays a failure: there is no
fallback to another channel, and none to an unsigned path.

**An older release requires an explicit local selection.** Ordinary update
checks retain the monotonic sequence floor and reject downgrades. Selecting a
row under Previous versions records a one-trial downgrade intent in the
integrity-checked A/B journal. The in-app verifier and launcher then relax only
the sequence comparison for that exact pending signed candidate. Root-chain,
signature, expiry, platform, launcher-protocol, package-size, package-hash,
and per-file verification remain unchanged. The pending marker is consumed
before historical code starts, keeping the schema-1 trial record readable by
older releases; an explicit launcher retry reconstructs it from the pinned
candidate/floor pair. The candidate is cleared when the trial is accepted or
discarded. A remote response cannot select a row or set the journal bit.

**Nothing remote can turn the unsigned mode on.** The Developer channel needs
two independent local acts by the person holding the device: a URL entered in
the native **Developer URL** option (or hand-written into
`data/boot-overrides.cfg` on the Memory Stick), and then Developer chosen in
Options → Experimental → Update channel. No server response, redirect,
release asset, page, script, or update payload can supply either one. A user
who never configures a URL is never offered Developer by the channel selector
and never leaves the signed path.

**A malformed endpoint cannot brick the browser.** The in-app editor refuses
it before writing; an unusable value found in a hand-edited file is treated as
absent -- logged, ignored, channel hidden -- and the browser boots normally.

## Known limitation: channel separation is endpoint-only

Which channel a candidate belongs to is decided by the endpoint it came from,
not by anything inside the signed metadata. Stable and Beta therefore differ
only in the fixed URL each fetches, and a signed manifest is not itself bound
to a channel. An actor able to publish a correctly signed release to the Beta
endpoint could publish the same bytes at the Stable one, and the device could
not tell the two apart from the metadata. This does not admit unsigned code
or remotely selected downgrades -- signatures and the ordinary sequence and
equivocation checks are unchanged -- and it does not touch Developer, whose
payload is unsigned by construction and reachable only through the two local
opt-ins above. Closing it means adding a channel field to the signed manifest, which
is a release-cut format change and needs its own design pass; it is
deliberately not attempted here.

The intended experience is a manual `Version / Update` page in Options. It
adds no browser network request, package scan, or release-metadata read to
ordinary startup beyond the optional, rate-limited background metadata check
described under "Background update-available check" below. The stable launcher necessarily reads at most two fixed
174-byte state records before choosing a slot; it does not hash the active
slot. A user chooses when to check, reviews a signed release, explicitly
downloads it, and explicitly restarts into it.

## Security objective

On Stable and Beta, an attacker who controls a network path, CDN response,
GitHub account, or release asset must not be able to make the PSP execute an
unsigned browser or choose a downgrade. A historical signed release is a
separate explicit local choice. Developer is an explicit local exception,
described below, that the same attacker cannot reach: it requires physical
access to the Memory Stick and a deliberate selection in Options. On every
channel, interruption, cancellation, a full Memory Stick, or a bad new build
must leave the previous browser launchable.

The design does not claim to withstand a physical attacker who can replace
the Memory Stick's root `EBOOT.PBP`. The PSP provides no suitable protected
storage in which this homebrew can anchor a stronger local trust root.

TLS, GitHub's reported asset digest, immutable releases, and GitHub artifact
attestations are useful independent evidence. They are not the device trust
root. The device trusts only public signing keys embedded in a small stable
launcher and the release metadata those keys authorize.

## User experience

Options gains one entry:

```text
VERSION / UPDATE...
```

The page begins with:

```text
CURRENT       0.4.0 (42)
CHECK FOR UPDATE
```

No request happens until `CHECK FOR UPDATE` is activated. A valid newer
release changes the page to:

```text
AVAILABLE     0.4.1 (43)
DOWNLOAD      15.8 MB
SIGNED NOTES  Safer, faster updates.
DOWNLOAD UPDATE
```

The PSP screen shows a bounded inert summary of the signed notes; the complete
512-byte field remains available to release tooling. Download progress is
measured as verified bytes over the signed expected size. Circle acknowledges
cancellation immediately and the bounded network job stops at its next
cooperative boundary. A successfully staged update offers
`RESTART TO UPDATE`; Tilefinch never silently installs or restarts.

Errors remain distinct. In particular, an invalid signature, expired
metadata, an older release, insufficient space, and an ordinary network
failure must not be collapsed into “up to date.”

### Previous signed versions

On Stable, Square queries GitHub's public Releases API and shows up to eight
earlier stable releases which advertise Tilefinch's signed metadata asset.
The bounded response and list live only in RAM for this explicit operation;
no release catalog is persisted. X returns to the ordinary update page with
the chosen version named; checking it fetches
`releases/download/v<version>/tilefinch-update-v1.tfum`. Old metadata must
still be within its signed expiry window. An expired historical release is
reported as expired rather than silently weakening freshness checks.

Installation remains an A/B trial. Shared profile data is not rolled back,
and the newer slot remains the previous slot until the older build proves
healthy. After a deliberate downgrade, Stable's normal Latest check can offer
the newer signed sequence again.

### Stable, beta, and developer channels

Stable is the default and retains the fixed GitHub `latest` metadata URL.
**Options → Experimental → Update channel** can opt into Beta, whose signed
metadata lives at the distinct fixed `releases/download/beta/` endpoint.
There is no automatic fallback between Stable and Beta: a failure remains a
failure on the selected channel, and changing channels clears any in-memory
offer before creating a new client.

Developer is a deliberately conspicuous third choice, and reaching it takes
two separate local acts, both performed by whoever is holding the PSP:

1. **A locally entered endpoint.** **Options → Experimental → Developer URL**
   accepts a bounded public `https://` TFUM endpoint and transactionally
   writes the same `developer_update_url=` key to shared
   `data/boot-overrides.cfg`. It may alternatively be hand-edited off-device,
   over USB or by moving the card. Page content and remote responses cannot
   invoke the editor or supply the value.
   `developer_package_url=` may separately name the TFUP package; when empty,
   the package named by its manifest is resolved beside the metadata. The
   optional separate package override remains a configuration-file setting.
2. **An explicit UI selection.** With a usable URL present, the channel row
   appears under Options → Experimental → Update channel and must be chosen.
   Without a configured URL, Developer is skipped by the channel selector.

Neither step has a remote trigger, and there is no state a server, page, or
update payload can put the device into that produces one.

`https://` is required, not merely preferred. This is the only channel whose
payload carries no signature, so a cleartext endpoint would let anything on
the network path substitute the package with nothing downstream able to
notice; plain `http://` also bypassed the per-hop redirect validator, which is
installed only for HTTPS requests. Both places that admit a developer URL --
boot-configuration validation and update-client construction -- refuse a
non-HTTPS one, and the resolved URL is re-checked after share-link rewriting.

An unusable value in either key is an ordinary mistake, not a fatal one.
These are optional and worth only whether the channel is offered, so the
in-app path validates before writing and boot clears any bad hand-edited
value, logs one line, and continues. A bad metadata URL hides the channel; a
bad package URL alone leaves the channel and falls back to same-directory
package lookup. No other boot-configuration key changed posture.

Developer is intentionally an **unsigned-code mode** for a user who chooses to
test a contributor's build without asking a release-key holder to sign it.
Tilefinch sends no cookies or authorization and rejects URL credentials,
fragments, control bytes, root rotations, and signature-like records. Generic
endpoints retain same-origin redirects. Recognized OneDrive and SharePoint
links may cross origins along Microsoft/CDN redirects, still under the browser
transport's five-hop ceiling. Every Developer request accepts only HTTPS
redirect targets, so no hop can downgrade to cleartext. The bounded TFUM still
authorizes an exact TFUP size and SHA-256, and the TFUP authorizes every
allowed file's exact path, size, offset, and SHA-256.

Developer remains transactional: it extracts only into the inactive slot,
marks that slot explicitly as `DEVELOPER`, journals a pending trial, and has
the stable launcher re-check the metadata digest, package digest, file table,
and every installed file before the first boot. The trial must become healthy
or the next launch rolls back; holding L still starts the previous slot.
Unsigned trials do not advance or replace the signed anti-rollback floor, so
returning to Stable/Beta cannot be blocked by an arbitrary contributor
sequence number.

This is crash safety, not authenticity or sandboxing. Whoever controls the
configured Developer endpoint can supply arbitrary PSP code, read or alter
shared browser data, and report itself healthy. A/B rollback protects against
a build that crashes or never reaches the health checkpoint; it cannot make
malicious native code safe. The two deliberate opt-ins are editing the local
configuration file and selecting Developer in Options.

This provides a signed TestFlight-like path for public beta builds and a
low-friction unsigned contributor path without weakening Stable/Beta trust.
The one-time launcher/A/B install shape is required for every channel. Stable
and Beta additionally require an embedded public root and offline signature;
Developer works in a root-empty contributor build and never needs or reads a
private key.

### Background update-available check

Each boot arms at most one optional background check for new release
metadata. It fires only after the boot has fully succeeded, nothing is
loading or playing, the user has been idle for the house autohide interval,
and Wi-Fi is already connected — the check never initiates a network join
of its own. It contacts only the fixed release-metadata URL
(`tilefinch-update-v1.tfum`); the package is never downloaded in the
background. Completed checks are at least 3.5 days apart (at most twice a
week), timed by the persisted profile record; a check that fails, is
cancelled, or cannot run does not advance that cadence and produces no UI.
A stored last-check time in the future (a wrong RTC) resets the cadence
rather than blocking it. When a check verifies a newer signed release, a
one-time `UPDATE READY - SEE OPTIONS` notice appears and the
`Version and update` Options row reads `New` until the running build's own
sequence catches up. **Options → Update check** (default on) disarms the
whole feature; a root-empty build can contact only its explicitly configured
Developer endpoint, and a hermetic trace replay never contacts the network.

## Installation layout

The first updater-capable release needs a one-time manual installation. A
currently running, single root EBOOT cannot replace itself while preserving a
launchable file through every possible power loss.

```text
PSP/GAME/TILEFINCH/
  EBOOT.PBP                  stable launcher and trust root
  data/                      user-owned, shared mutable state
    profile.cfg
    boot-overrides.cfg
    recovery.cfg
    http-cache.bin
    local-storage.bin
    update-state.0
    update-state.1
    update/
      package.part
  components/                 optional data, outside browser slots
    voice-en-us/
    glyph-ja/
    glyph-zh-hans/
    glyph-zh-hant/
    glyph-ko/
    glyph-emoji-color/
    glyph-cyrillic/
    glyph-latin-extended/
      active/
      previous/
  slot-a/
    EBOOT.PBP
    slot.tfum
    fonts/
    roots.pem
    boot-defaults.cfg
  slot-b/
    ...
```

The launcher chooses a slot and starts it through the CFW
`sctrlKernelLoadExecVSHMs2()` Memory Stick handoff. The ordinary user-mode
`sceKernelLoadExec()` call cannot reliably start another homebrew EBOOT on
post-1.xx hardware; it remains only as a compatibility fallback for PPSSPP and
environments that implement the standard call but not the CFW extension. A
supported CFW is consequently part of the launcher platform contract, not
just a prerequisite for initially starting homebrew. If both calls return,
the launcher keeps its checked framebuffer visible and displays both result
codes instead of failing to a blank screen.

The launcher displays its first complete frame before sampling safe-start
input and accepts L throughout a bounded 500-millisecond window. The user
should begin holding L while selecting Tilefinch from the XMB. On the initial
manual installation there is no previous slot yet, so L is acknowledged but
continues into slot A; a previous version exists only after the first
successful A/B update.

The browser must split its current sibling-path policy:

- immutable program resources remain relative to the slot EBOOT;
- signed boot defaults remain relative to the slot EBOOT;
- user boot overrides, profile, cache, storage, recovery, and updater state use
  the shared installation `data/` directory.

Signed defaults and mutable overrides are separate files. An update may replace
`slot-*/boot-defaults.cfg`, while `data/boot-overrides.cfg` survives and wins
for user-selectable values such as the Wi-Fi profile and home URL. The path
split migrates an existing `boot-live.cfg` into shared overrides without
silently resetting those values.

The browser derives its slot and installation root from its own `argv[0]`,
using the sibling-path logic it already needs for resources. Launcher-provided
slot and data-directory arguments are advisory and must agree with that
derived identity; missing or malformed arguments cannot prevent a trial from
recording health.

Without this split, changing slots would appear to erase user state. The
installer duplicates immutable assets between slots.
Content-addressed sharing can come later, after rollback and garbage
collection are proven.

This design assumes `sceKernelLoadExec()` can hand off between homebrew EBOOTs
under the custom firmware already required to run Tilefinch. Device validation
must cover the supported CFW set. If a firmware refuses that handoff, its safe
fallback is a documented manual-copy update, not a weaker in-place installer.

## Cryptographic trust

P-256 ECDSA over SHA-256 is the smallest practical choice for this build. The
PSP image already links the required mbed TLS 2.28.10 P-256, ECDSA verification,
and streaming SHA-256 routines for HTTPS, so the updater does not need a
second cryptographic library.

Production should use a TUF-inspired split:

- a 2-of-3 offline root role authorizes root and release keys;
- preferably a 2-of-3 offline release role signs update manifests;
- each distinct key counts at most once toward a threshold.

A 1-of-1 offline release key is a simpler operational starting point, but its
compromise can authorize a malicious release until a newer root revokes it.
No private key belongs in the repository, an EBOOT, a GitHub account, or a
GitHub Actions secret. CI may produce unsigned artifacts; offline release
signing occurs only after their hashes are independently checked.

Public keys are 65-byte uncompressed SEC1 P-256 points. A key identifier is:

```text
SHA256("tilefinch:p256-key:v1\0" || public_key)
```

Detached signatures are fixed-width, 64-byte big-endian `r || s` values.
Verification rejects zero or out-of-range components, duplicate key IDs, and
non-canonical high-S signatures.

Low-S canonicalization is also a hard producer requirement. The offline
signing tool must normalize every valid ECDSA result with `s = n - s` whenever
the signer returns high-S; raw mbed TLS signing output cannot be published
unchanged. Golden vectors must contain one high-S signature that verification
rejects and its mathematically equivalent normalized low-S signature that
passes.

## Signed metadata

Every stable release publishes a fixed-name metadata asset:

```text
tilefinch-update-v1.tfum
```

The client fetches it from GitHub's stable latest-release URL. The complete
envelope is capped before allocation and contains:

```text
magic[8]                 "TFUMv1\0"
envelope_schema:u16be
root_update_count:u8
root_update[]            complete bounded chain from embedded root v1
manifest_length:u16be
manifest_bytes[]
signature_count:u8
signature[] {
  key_id[32]
  signature_r_s[64]
}
```

The signed manifest itself is a fixed-order binary record, not JSON. This
avoids canonicalization, duplicate-key, numeric-conversion, and
page-replaceable-parser ambiguity:

```text
manifest_schema:u16be
root_version:u32be
release_sequence:u64be
expires_unix:u64be
minimum_launcher_protocol:u16be
platform:u16be
package_format:u16be
package_size:u64be
package_sha256[32]
version_length:u8
version_ascii[]           display only, at most 31 bytes
tag_length:u8
tag_ascii[]               at most 63 safe ASCII bytes
asset_length:u8
asset_ascii[]             at most 95 safe ASCII bytes
notes_length:u16be
notes_utf8[]              at most 512 bytes
```

The signature digest is:

```text
SHA256("tilefinch:update-manifest:v1\0" || manifest_bytes)
```

The parser verifies the envelope and signature before interpreting
page-visible strings. It rejects unknown schemas, trailing bytes, overflow,
bad UTF-8, unsafe asset/tag characters, the wrong PSP platform, packages above
the configured ceiling, and unsupported launcher protocols.

`notes_utf8` is displayed only after verification and only through an inert,
bounded UI text path: no HTML, Markdown, ANSI escapes, control sequences, or
format-string interpretation. Unsupported glyphs are replaced rather than
delegated to page rendering.

Security comparisons use the monotonically increasing `release_sequence`,
never semantic version ordering. A lower sequence is a downgrade. The same
sequence with a different package hash is equivocation and is rejected.
Merely fetching or offering metadata does not advance persistent anti-rollback
state. A candidate is compared with the currently installed known-good slot,
and the persistent sequence/hash advances only when a trial is confirmed
healthy. This prevents an accidentally signed but uninstalled large sequence
from poisoning all later checks. At most one offered candidate pair is retained
in memory. Once staged, the same pair lives in that candidate's signed
`slot.tfum` and is checked on retry; neither case creates an unbounded
equivocation history.

The journal copies are integrity-checked but cannot provide protected
storage. If both are lost, the frozen launcher can only seed the floor from
its own compiled release sequence, which may be older than the active
browser. The running browser therefore raises a recovered journal to its own
compiled sequence before update checks whenever it is the active known-good
slot and no trial is in progress. Until that browser has run, an attacker able
to corrupt both records can replay a correctly signed release newer than the
launcher but older than the active browser. This is within the documented
mutable-Memory-Stick limit, not protected anti-rollback storage.

Root metadata is also a canonical bounded record containing its version,
expiry, thresholds, and root/release public-key sets. Root version `N+1` must
be signed by both the old root threshold and the new root threshold. Versions
cannot be skipped. This preserves continuity during key rotation.

Every fetched update envelope and installed `slot.tfum` carries the complete
ordered root chain from the launcher's embedded v1 root through the manifest's
declared root version, capped at eight rotations and a small fixed byte
ceiling. It is not a trusted delta and no accepted root is trusted merely
because it appears in mutable `data/`. The browser and launcher re-parse and
re-verify the entire chain from the embedded anchor whenever it authorizes a
manifest. Consequently the stable launcher contains the bounded root-envelope
parser, threshold/uniqueness rules, chain walker, and ECDSA verifier—not only a
single-signature primitive. The few-kilobyte chain is duplicated in metadata
and slots in exchange for avoiding a mutable root-authority database.

Non-terminal root envelopes are checked for signatures, thresholds, and exact
version continuity, but their expiry dates are not compared with the current
clock while replaying the chain. The terminal root and release
manifest must satisfy the current freshness policy. Otherwise an honestly
rotated chain would become unusable as soon as any superseded root expired.

The exact root record is:

```text
root_schema:u16be             1
root_version:u32be
expires_unix:u64be
root_threshold:u8
release_threshold:u8
root_key_count:u8             1..6
release_key_count:u8          1..6
root_key[] {
  key_id[32]
  public_point[65]
}
release_key[] {
  key_id[32]
  public_point[65]
}
```

Every `root_update[]` item in TFUM is:

```text
root_record_length:u16be
root_record[]
old_signature_count:u8
old_root_signature[] { key_id[32], signature_r_s[64] }
new_signature_count:u8
new_root_signature[] { key_id[32], signature_r_s[64] }
```

Both lists sign
`SHA256("tilefinch:root-metadata:v1\0" || root_record)`. The first must
satisfy the preceding root threshold and the second the new root threshold.
Each list is independently duplicate-key checked.

Expiry detects stale signed metadata only when the PSP has a usable clock. If
freshness cannot be established, the UI says so; authenticity alone is not
reported as “latest.” An attacker can always withhold updates, and no signing
format can turn that denial of service into availability.

## Release package

The first format should be an uncompressed sequential `TFUP` package rather
than ZIP or TAR:

- at most 32 MiB and 64 files;
- fixed bounded table followed by concatenated payloads;
- exact size and SHA-256 for the package and every file;
- at most 128 bytes per relative path;
- no symlinks, absolute paths, backslashes, empty components, `.` or `..`;
- no duplicate or prefix-colliding paths;
- an allowlist of EBOOT, fonts, TLS roots, and signed boot
  defaults.

The whole-package size and digest are signed. Per-file digests make extraction
fail closed and give the launcher a bounded way to revalidate a pending slot.
Compression can be considered later only with independent compressed and
expanded limits.

Voice recognition is a separate signed component, not a second browser
bundle. Its `TFVMv1` envelope uses the
`tilefinch:voice-component-manifest:v1` signature domain and authorizes only a
`TFVPv1` package (format 2). TFVP has its own bounded allowlist:
`model-info.tfv`, `model/`, and `LICENSES/`; a browser TFUM cannot authorize a
voice package and a voice TFVM cannot authorize a browser slot. The installer
hashes the complete package and every file before writing signed metadata and
`READY` last, then promotes `candidate.tmp → active` while retaining one
`previous` generation. Interrupted promotion resolves `active` first and
`previous` second. The signed envelope stored with the installed component is
re-verified to recover its sequence/hash anti-rollback floor; it is not read
at browser boot, only when the user opens the Experimental voice controls.
Removal first persists the disabled voice preference, then an `UNINSTALLED`
tombstone, and only then deletes either generation. The resolver honors that
marker before `active`, `previous`, or a legacy slot-local model, so power loss
during cleanup cannot resurrect speech input. Only promotion of a newly
verified component clears the marker.

The model is fetched from public, credential-free GitHub release assets in a
dedicated `tilefinch-models` repository. It uses the same embedded root of
trust but an independent monotonic component sequence. Browser updates never
copy, delete, or verify the shared component and therefore neither duplicate
its roughly 9 MB payload nor add it to the trial-boot hashing cost.

The user-built software video decoder follows the same A/B separation but is
not a downloadable signed component: its three files live in
`components/swdec/`, outside both browser slots, and are never included in an
official TFUP. Browser manifests prefix their already-signed release notes
with the decoder ABI expected by that version. The parser removes that marker
from ordinary display text and warns **Decoder rebuild needed** only when an
installed component advertises a different ABI. This extends the existing
manifest record without a schema change, so older clients still verify it.

Optional glyph packs use the same component repository and public root but a
third, non-interchangeable authority: `TFGMv1` envelopes sign the
`tilefinch:glyph-component-manifest:v1` domain and authorize only raw bounded
`TFGFv1` packages (format 3). Fixed asset names identify Japanese, Simplified
Chinese, Traditional Chinese, Korean, Cyrillic, Extended Latin, and color
emoji. Each pack has its own monotonic sequence and component ID; the installer
verifies the signed size and digest, parses the complete bounded index, writes
`READY` last, and then promotes `candidate.tmp → active` while retaining one
`previous` generation. An `UNINSTALLED` marker suppresses both generations
after interrupted removal.

Embedded monochrome CJK/emoji fallback remains part of every browser slot and
does not depend on these assets. With the default Embedded setting, boot does
not probe or open optional component storage. When selected, only the chosen
language pack and/or color-emoji pack is signature-verified and indexed at
boot. Visible-text script hints can later attach up to two additional installed
language packs, one signature/index probe per frame and never more than four
packs total. This does not enumerate packs or read glyph payloads during page
parsing. Payloads stay on the Memory Stick and enter RAM through bounded 16 KiB
pump reads after a cache miss; measurement and rasterization themselves perform
no file I/O, and the runtime path never writes. Optional packs are built from
redistributable Noto inputs and do not read PSP firmware fonts.

`slot.tfum` is not a TFUP payload: putting the envelope inside the package
whose hash its own manifest signs would be circular. After package and
per-file verification, the installer writes the already verified envelope as
`slot.tfum`, then writes `READY` last.

The canonical TFUP v1 wire layout is:

```text
magic[8]                      "TFUPv1\0\0"
package_schema:u16be          1
file_count:u16be              1..64
table_length:u32be
file[] {
  path_length:u8
  path_ascii[]
  payload_length:u64be
  payload_sha256[32]
  payload_offset:u64be
}
payloads[]                    exact table order, no padding
```

Offsets must be contiguous, begin immediately after the table, and end at the
signed package size. This removes sparse-file, overlap, and aliasing cases from
the extractor.

`tools/tilefinch_update_tool.py` is the offline producer for these records.
It shells out to a local OpenSSL only for private-key operations and
normalizes every signature to low-S before writing it. Its `root`, `pack`,
`manifest`, `root-update`, and `envelope` commands are deliberately separate
so an offline signing ceremony can inspect the package and manifest between
steps. Test keys are generated only by the host test; production private keys
are never generated or stored by the build.

## Dedicated network path

The updater is a native bounded client. It is not exposed to page JavaScript
and receives no page URL, cookies, authorization, Origin, Referer, cache
entries, or DOM callbacks.

For metadata it requests:

```text
https://github.com/stjanovitz/tilefinch/releases/latest/download/tilefinch-update-v1.tfum
```

Every hop must remain HTTPS on port 443. Redirects are manually capped and
limited to exact `github.com` or a label-boundary subdomain of
`githubusercontent.com`; userinfo and IP-literal hosts are rejected. The
request uses identity encoding so byte counts and hashes describe the
transferred object exactly.

After Stable/Beta signature verification, the client constructs the immutable,
tag-specific package URL from the signed safe tag and asset name:

```text
https://github.com/stjanovitz/tilefinch/releases/download/{tag}/{asset}
```

This closes a race in which “latest” changes between metadata and package
requests. The package streams through a 4–16 KiB buffer into the inactive
staging slot while SHA-256 advances. A response length is useful
defense-in-depth, but only the signed exact size and digest authorize the
package.

An interrupted first implementation simply restarts the download. A future
Range resume must re-hash the existing prefix and require an exact `206
Content-Range`; it cannot trust saved hash state blindly.

The current source repository is private. A PSP must not contain a personal
access token. In-app distribution therefore requires either making releases
public here or publishing signed artifacts in a separate public,
release-only repository whose identity is compiled into Tilefinch.

## Power-loss-safe A/B transaction

While slot A is active:

1. Check free space for the manifest-bounded package, extracted slot, and safety margin.
2. Clear only stale `slot-b.tmp`; never modify A or shared user data.
3. Stream to `data/update/package.part`, then close and synchronize it.
4. Verify signed metadata (Stable/Beta) or the explicit unsigned Developer
   envelope, then verify the exact package size/hash.
5. Extract into `slot-b.tmp`, verifying every allowed file.
6. Write `slot.tfum`, the package table, an unsigned `DEVELOPER` marker when
   applicable, and a manifest-hash `READY` marker last.
7. Synchronize the directory/device.
8. Replace only the inactive B directory.
9. Write a pending boot-choice record through the redundant state journal.
10. Restart through the stable launcher.

Free-space discovery is a hard admission gate. Validation builds report
`tilefinch-update-storage: boot-probe=...` from the actual PSP filesystem and
the update page reports the directory-specific result again. A failed or
zero-block `statvfs()` result fails closed as `NOT ENOUGH FREE SPACE`; PPSSPP
and physical-device release qualification must therefore show an `ok` probe
with a nonzero byte count.

Package verification and extraction advance in 16 KiB irreducible units. The
dedicated install page may run up to four units in one frame, but stops after a
2 ms advisory batch budget. This raises the fast-card ceiling from roughly
0.94 MiB/s to 3.75 MiB/s at 60 Hz without forcing four slow Memory Stick
operations into one input gap. Validation records the number of units and the
longest unit on completion.

The two updater-state records contain a generation, active slot, pending slot,
trial state, and checksum. Each state transition writes the older copy first
through a temporary file, synchronizes it, and retains the other valid
generation. The launcher selects the newest valid record; it never relies on
one FAT rename being an atomic transaction.

Before first execution, the launcher independently verifies the pending slot's
complete root chain, signed manifest, and files. It records that a trial is
starting and then loads the slot. The browser marks itself healthy after
stable chrome is presented and the main loop has either accepted one user
input or remained responsive, presenting and heartbeating, for ten seconds.
This keeps the vulnerable power-loss window modest without declaring health
at process entry.

If the next launcher run finds an unconfirmed trial, it returns to the previous
slot but retains the already staged and verified candidate. Its recovery
screen offers `TRY UPDATE AGAIN` as well as discarding it; an ordinary power-off
during a healthy trial therefore does not force another large download.
Holding L at startup always chooses the previous known-good slot.
Retry returns through the same verify-and-journal-started path as a first
trial; it never directly executes a pending slot, so the browser can always
record health against a `STARTED` trial.

Full manifest, root-chain, package, and per-file verification occurs when
entering or retrying a pending trial. An ordinary boot of the active
known-good slot performs no full-slot hashing and simply hands off to its
EBOOT. This is intentional: physical Memory Stick modification is outside the
threat model, and per-boot verification of a roughly 15 MiB slot would violate
the no-added-startup-work requirement.

The previous slot is retained until a later update succeeds. The root launcher
does not update itself in-app. A launcher flaw, exhausted root threshold, both
slots being lost, or an incompatible initial layout requires a manual reinstall.

The launcher remains deliberately independent of browser UI and layout code.
It uses the same checked, next-frame, cache-coherent PSP display module as the
browser rather than owning a second scanout implementation. If mode setup or
frame publication is rejected, it skips the otherwise visible ten-second
recovery window and immediately takes the known-good default instead of
waiting on a blind screen.
Its `.text` has a 256 KiB ceiling, with a measured handoff-time ratchet, so the
always-executed trust root cannot quietly become a second browser or add
unbounded startup latency.

## Build and release workflow

The official PSP preset embeds the checked-in public binary trust record at
`trust/root-v1.tfur`. The file contains no private signing material, and the
preset fails configuration if it is missing. A custom or rotated public root
can still be supplied explicitly:

```sh
cmake --preset psp \
  -DTILEFINCH_UPDATE_ROOT_V1=/path/to/alternate-root-v1.tfur \
  -DTILEFINCH_RELEASE_SEQUENCE=43 \
  -DTILEFINCH_UPDATE_REPOSITORY_OWNER=public-owner \
  -DTILEFINCH_UPDATE_REPOSITORY_NAME=tilefinch-releases
cmake --build build-preset-psp --target tilefinch-psp-install-tree
```

The exact one-time installation tree is then
`build-preset-psp/tilefinch-install/Tilefinch/`. The normal browser target
alone is not an updater-capable installation because it omits the stable root
launcher and A/B directory layout.

The offline producer is intentionally multi-step:

```sh
python3 tools/tilefinch_update_tool.py pack \
  --directory release-slot --output tilefinch-psp.tfup
python3 tools/tilefinch_update_tool.py manifest \
  --package tilefinch-psp.tfup --root-version 1 --sequence 43 \
  --expires 2000000000 --version 0.4.1 --tag v0.4.1 \
  --asset tilefinch-psp.tfup --decoder-abi 3 \
  --notes release-notes.txt \
  --output manifest.tfum-body
python3 tools/tilefinch_update_tool.py envelope \
  --manifest manifest.tfum-body --release-key /offline/release-key.pem \
  --output tilefinch-update-v1.tfum
```

For an unsigned contributor build, use the same bounded `pack` and `manifest`
steps, then deliberately omit all key operations:

```sh
python3 tools/tilefinch_update_tool.py developer-envelope \
  --manifest manifest.tfum-body --output tilefinch-update-v1.tfum
```

Host that TFUM and its named TFUP in the same directory, enter the TFUM URL
under **Options → Experimental → Developer URL**, and select Developer. The
same value can still be set as `developer_update_url=` by editing the file.
Alternatively, share each file separately from OneDrive and set both links in
the configuration file:

```ini
developer_update_url=https://1drv.ms/...metadata-share...
developer_package_url=https://1drv.ms/...package-share...
```

Recognized `1drv.ms`, `onedrive.live.com`, and `*.sharepoint.com` links are
converted to `download=1` without editing the file's other query parameters.
Both shares must be public to anyone with the link and must allow downloads.
Microsoft endpoints that reject the PSP transport's initial TLS 1.3 handshake
receive the fetch layer's single verified TLS 1.2 compatibility retry; this
does not relax certificate checks or permit cleartext redirects.
The package share is the generated TFUP, not a standalone browser EBOOT: the
inactive slot also needs its fonts, roots, configuration defaults, and other
allowlisted assets for a rollback-safe trial. This command cannot add root
rotations or signatures; the device accepts its output only on the explicitly
selected Developer channel.

`release-slot` contains only the signed slot allowlist, beginning with the
browser `EBOOT.PBP`; it does not contain launcher, user data, `slot.tfum`,
`slot.tfut`, or `READY`. The envelope command normalizes signatures to low-S.
For a rotated root, supply each ordered `--root-update` artifact. Upload the
TFUP and fixed-name TFUM assets to the same immutable public GitHub release
whose signed tag and asset names appear in the manifest.

Note `--notes` takes a file path (its bytes become the manifest notes), not
literal text.

### Root v1 provenance

Only the public `trust/root-v1.tfur` record is checked in; the private root and
release keys remain offline and outside the repository. The record uses a 1-of-1
root / 1-of-1 release configuration expiring 2030-01-01 UTC:

- root key id
  `7d4647aad05820b7630702fea8fb8147a1485cb3e7073b2352cf1935ce03bd80`
- release key id
  `256eaffc6c058b757d91998c16b1ff04e88c30a801afcf4020bf722b5fec0a03`
- `root-v1.tfur` SHA-256
  `0eb708ab00b966a70d7220555718ec421158c1f14df2c506616e54fd27c51777`

This 1-of-1 configuration makes the one offline root key a single recovery
dependency. Compromise requires a signed rotation while the release threshold
can still be met; loss without an available threshold requires manual
reinstallation. A future 2-of-3 root can improve operational resilience
without changing the device verifier or package format.

The ceremony is rehearsed end-to-end by
`tilefinch-update-root-proof-tests` (tests/test_update_root_proof.c): an
updater-enabled build verifies a real signed envelope against the embedded
root, binds it to the exact packed TFUP bytes, and demonstrates wrong-key,
tampered-byte, downgrade, equivocation, and expiry rejection plus the A/B
trial walk. The test skips unless the build embeds a root and
`TILEFINCH_PROOF_ENVELOPE`, `TILEFINCH_PROOF_WRONG_ENVELOPE`, and
`TILEFINCH_PROOF_PACKAGE` point at rehearsal artifacts; see
docs/RELEASE_PROCESS.md for producing them.

## Validation gates

The signed release path requires all deterministic gates below. A multi-key
root configuration additionally requires a rotation and key-loss rehearsal
which demonstrates that the remaining threshold can authorize the next root.

Host fuzzing and fault injection must cover malformed lengths, overflow,
duplicate threshold keys, invalid/high-S signatures, wrong platform, expired
or stale sequences, same-sequence/different-hash metadata, cleartext and
off-host redirects, truncated packages, malicious paths, ENOSPC at every
write, and termination after every close, sync, rename, and state transition.
It must also cover a missing/corrupt root-chain member, exceeding the root
rotation cap, loss or corruption of launcher arguments, power-off before trial
health, retrying a retained candidate, and proving that merely viewed metadata
does not ratchet the installed sequence.

The central invariant is:

> After every injected failure, the previously healthy slot still launches.

The installer sweep interrupts staged writes, file/device syncs,
inactive-slot renames, and every redundant-journal operation. A transition
that was already durably journaled may recover as a valid pending trial; in
all cases the previously active slot and its bytes must remain available.

The real-root proof must bind the exact TFUP bytes to the signed envelope and
reject a wrong key, tampered bytes, downgrade, equivocation, and expiry. The
isolated HTTPS PPSSPP run then covers signed metadata fetch, package streaming,
inactive-slot installation, launcher verification, trial start, and health
confirmation without requiring public assets.

The PSP `.text`, launcher `.text`, heap reserve, first-frame time, launcher
handoff time, and ordinary no-check boot path remain ratcheted. Wi-Fi loss,
full Memory Stick, physical power interruption, a deliberately crashing trial,
retry without re-download, the L-button recovery path, and supported-CFW
LoadExec behavior are valuable device investigations. They become required
when a release changes their code paths; otherwise they remain the optional
hardware panel recorded in [Release process](RELEASE_PROCESS.md).

## Primary references

- [GitHub immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)
- [GitHub release REST API](https://docs.github.com/en/rest/releases/releases)
- [GitHub release asset downloads and redirects](https://docs.github.com/en/rest/releases/assets)
- [GitHub release asset links](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases)
- [GitHub artifact attestations](https://docs.github.com/en/actions/concepts/security/artifact-attestations)
- [The Update Framework specification](https://theupdateframework.github.io/specification/latest/)
- [Mbed TLS 2.28 ECDSA API](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-2.28.10/include/mbedtls/ecdsa.h)
- [Mbed TLS 2.28 SHA-256 API](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-2.28.10/include/mbedtls/sha256.h)
- [PSPSDK LoadExec API](https://pspdev.github.io/pspsdk/group__LoadExec.html)
