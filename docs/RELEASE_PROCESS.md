# Release process

How a Tilefinch binary release is cut. The mechanical part is
`scripts/cut-release.sh`; everything that requires credentials or private
keys is deliberately manual and listed at the end. The signing formats,
trust model, and update-channel behavior the release must satisfy are
specified in [SECURE_UPDATES.md](SECURE_UPDATES.md); this page is only the
operational sequence.

## What a release is

A release is one commit, one version, and one monotonic release sequence:

- **Version** (`MAJOR.MINOR.PATCH`, for example `0.1.0`) is the
  human-facing name. It must match the `project()` version in
  `CMakeLists.txt` and the finalized CHANGELOG section.
- **Release sequence** (`TILEFINCH_RELEASE_SEQUENCE`) is the unsigned
  integer the updater compares for downgrade protection. It increases by
  one for every signed release and never repeats or goes backward, even
  across version branches. Sequence order, not version order, is what the
  device trusts.

The shipped artifacts are:

This production process covers signed Stable/Beta releases. Contributor
Developer builds use the separate `developer-envelope` workflow in
[SECURE_UPDATES.md](SECURE_UPDATES.md); they do not consume or advance the
signed release sequence.

| Artifact | Purpose |
|---|---|
| `tilefinch-v<version>-psp.zip` | First-install zip; unpacks into `PSP/GAME/` on the Memory Stick |
| `tilefinch-psp-v<version>.tfup` | Update package built from the staged slot, unsigned until the offline ceremony |
| `tilefinch-update-v1.tfum` | Signed release metadata (fixed name; produced offline, never by the script) |
| `SHA256SUMS.txt` | Digests of the zip and TFUP for independent verification before signing |

## Prerequisites

- A clean working tree on the release commit; the script refuses to run
  otherwise.
- An ordinary host environment (not a network-restricted sandbox): the host
  suite's localhost redirect test must actually run, not report `Skipped`.
- `PSPDEV` pointing at a current PSPDEV toolchain (`psp-gcc` 15.x).
- `python3` and CMake ≥ 3.21 on `PATH`.
- For an updater-capable release: the public `root-v1.tfur` record exported
  by the offline key ceremony (the private keys stay offline; the build
  embeds only the public record). Building without a trust root is allowed
  but must be requested explicitly with `--no-update-root`.
- **Dependency refresh sweep** per release: re-snapshot the Public
  Suffix List and `certs/roots.pem`; check pinned curl, Mbed TLS,
  nghttp2, FreeType, and lexbor for security releases; and diff the
  pinned Bellard QuickJS commit against upstream master for memory-safety
  fixes — the shipping engine has no CVE feed, so this manual diff is
  its only advisory stream.

## Step 1 — finalize the changelog

`CHANGELOG.md` accumulates changes under `## Unreleased`. At cut time, by
hand, on the release commit:

1. Rename `## Unreleased` to `## <version> — <YYYY-MM-DD>`.
2. Add a fresh, empty `## Unreleased` section above it.
3. On the first release only, drop the "no binary release has been
   published yet" clause from the changelog intro and update the README
   Install section, which says the same thing.
4. Commit. The script verifies a `## <version>` heading exists and fails
   the cut if this step was skipped.

The version in `CMakeLists.txt` (`project(psp_browser_tilefinch VERSION …)`)
must be bumped and committed in the same way; the script cross-checks it
against the version argument.

## Step 2 — run the cut script

```sh
export PSPDEV=/path/to/pspdev
scripts/cut-release.sh --update-root /offline-export/root-v1.tfur 0.1.0 1
```

or, for a deliberately updater-disabled build:

```sh
scripts/cut-release.sh --no-update-root 0.1.0 1
```

`--update-owner` / `--update-repo` override the GitHub location compiled
into the update client when the signed releases are published somewhere
other than the CMake defaults (see "Dedicated network path" in
SECURE_UPDATES.md; the source repository being private forces a public
release location).

The script stops at the first failure. In order it:

1. Validates arguments, toolchain, and required tools.
2. Verifies the git tree is clean and records the commit.
3. Cross-checks the version against `CMakeLists.txt` and `CHANGELOG.md`.
4. Runs the host release gate: `cmake --preset release`, full build, and
   the complete ctest suite in `build-preset-release`.
5. Re-runs the fidelity floor ratchet on its own and requires it to have
   *actually executed*. In the suite above that test is registered only
   when `CMAKE_BUILD_TYPE` is `Release` and carries `SKIP_RETURN_CODE 77`,
   so a tree missing its captures — or configured any other way — reports
   green without the ratchet ever running, and a dev flow that never
   builds Release never sees it at all. This step reads the CTest output
   back and fails the cut on a skip marker, on "No tests were found", and
   on anything red; CTest counts a skip as a pass in its own summary, so
   the marker is the only honest signal. Nothing about the gate's dev-lane
   behaviour changes: it is still Release-only and still skippable there.
6. Deletes `build-preset-psp` and configures the PSP release from scratch
   with the given release sequence (and trust root, when provided), so no
   stale object or cache variable can leak into a release.
7. Builds `tilefinch-psp-install-tree`: launcher and browser EBOOTs, both
   `.text` ratchets, staged fonts/TLS roots, and the staged
   notices tree. `cmake/StagePspInstall.cmake` fails this build if any
   file in `cmake/PspNoticesManifest.cmake` is missing.
8. Independently re-checks every notices-manifest entry against the staged
   tree, then packages `dist/tilefinch-v<version>/`: the install zip, the
   unsigned TFUP built from the staged `slot-a` (so first-install and
   update bytes are identical), `SHA256SUMS.txt`, and `RELEASE-INFO.txt`.
9. Prints the remaining manual steps and exits. It never signs, tags,
   or publishes.

The normal release deliberately contains no voice model. The independent
`tilefinch-voice-component-package` PSP target stages the checked model,
compiled dictionary sidecars, ABI marker, and redistribution notices, then
emits `build-preset-psp/voice-component/tilefinch-voice-en-us-v1.tfvp`.
That artifact has its own sequence and release ceremony in the public
`tilefinch-models` asset repository; it is never added to the browser zip or
TFUP.

## Step 3 — offline signing, tag, and GitHub release

These remain manual because they require the offline release key or GitHub
credentials, which the build environment must never hold:

1. **Sign the update package** on the offline signing machine following
   the "Build and release workflow" in SECURE_UPDATES.md: independently
   verify the TFUP digest against `SHA256SUMS.txt`, produce the manifest
   with the same version, tag, asset name, and sequence the script used,
   choose an expiry, and produce `tilefinch-update-v1.tfum` with the
   offline release key. The tool normalizes signatures to low-S.
2. **Tag** the release commit (`git tag -a v<version>`) and push the tag.
3. **Draft the GitHub release** for that tag: paste the `## <version>`
   section from `CHANGELOG.md` as the notes, upload the zip, the TFUP, the
   signed `tilefinch-update-v1.tfum` (its name is fixed — the device
   fetches it from the stable latest-release URL), and `SHA256SUMS.txt`,
   then publish.
4. **Verify after publishing** that the latest-release metadata URL serves
   the new envelope and that an installed previous release sees and
   installs the update.

Production enablement of the update channel additionally requires the
validation gates listed in SECURE_UPDATES.md ("Validation gates"): the key
ceremony and rotation rehearsal, the emulator and physical power-loss
matrix, and physical-device qualification of the release build.

## Optional voice-component release

The component uses the browser's embedded public root, but a different magic,
signature domain, package format, allowlist, and monotonic sequence. Build its
unsigned package after the PSP configure has produced the exact fixed maps:

```sh
cmake --build build-preset-psp --target tilefinch-voice-component-package
```

On the offline signing machine, create the component manifest and envelope
with `--component` on both commands. The manifest asset must be
`tilefinch-voice-en-us-v1.tfvp`; publish the signed envelope under the fixed
name `tilefinch-voice-en-us-v1.tfvm` alongside the package in the component
repository's GitHub release:

```sh
python3 tools/tilefinch_update_tool.py manifest --component \
  --package tilefinch-voice-en-us-v1.tfvp --root-version 1 \
  --sequence <component-sequence> --expires <unix-time> \
  --launcher-protocol 1 --version <model-version> --tag <release-tag> \
  --asset tilefinch-voice-en-us-v1.tfvp --notes <notes.txt> \
  --output voice.manifest
python3 tools/tilefinch_update_tool.py envelope --component \
  --manifest voice.manifest --release-key <offline-key.pem> \
  --output tilefinch-voice-en-us-v1.tfvm
```

GitHub release assets are suitable: the current package is about 9.1 MB, well
below GitHub's asset limit. The `tilefinch-models` release repository
must be public because the PSP client deliberately stores no GitHub token and
sends no credentials. The source repository may remain private.

Before signing, inspect the staged component and require all three license
payloads: `LICENSES/ALPHA_CEPHEI_LICENSE.txt`,
`LICENSES/CMUDICT_LICENSE.txt`, and `LICENSES/CMUDICT_NOTICE.md`. The packaging
target treats any missing payload or notice as a build failure; this check is
part of the component release gate, not a post-publication cleanup.

## Optional glyph-pack release

Language and color-emoji packs live in the same public `tilefinch-models`
release repository but cannot be authorized by a browser or voice manifest.
The models repository owns the pinned producer, regional codepoint manifests,
TFGF wire-format contract, and user-facing install instructions. Source fonts
and generated packs remain release inputs/artifacts rather than Git content.

Build each pack with that repository's `tools/build_glyph_pack.py`, then use
the exact main-repository revision being released to create its manifest and
envelope:

```sh
python3 tools/tilefinch_update_tool.py manifest --glyph-component \
  --package tilefinch-glyph-ja-v1.tfgf --root-version 1 \
  --sequence <component-sequence> --expires <unix-time> \
  --launcher-protocol 1 --version <pack-version> --tag <release-tag> \
  --asset tilefinch-glyph-ja-v1.tfgf --output glyph-ja.manifest
python3 tools/tilefinch_update_tool.py envelope --glyph-component \
  --manifest glyph-ja.manifest --release-key <offline-key.pem> \
  --output tilefinch-glyph-ja-v1.tfgm
```

Repeat with the fixed names for `zh-hans`, `zh-hant`, `ko`, and
`emoji-color`. The newest models release must contain every current voice and
glyph asset pair because the device deliberately fetches fixed names from
`releases/latest/download`. Before publishing, require a byte-identical
producer rebuild; verify the embedded source-font SHA-256 and complete OFL;
verify every final envelope with the embedded public root; and qualify
install, restart, representative rendering, removal, and interrupted
install/removal on a PSP. The default embedded fallback must remain readable
through every failure case.

## Rehearsing the signing ceremony

`tilefinch-update-root-proof-tests` proves a ceremony's real artifacts
end-to-end on the host before anything is published. To rehearse:

1. Build an updater-enabled host configuration
   (`-DTILEFINCH_UPDATE_ROOT_V1=<root-v1.tfur>`).
2. Produce a package and signed envelope with the real keys per
   SECURE_UPDATES.md ("Build and release workflow"), plus one envelope for
   the same manifest signed by a freshly generated throwaway P-256 key
   (`openssl ecparam -name prime256v1 -genkey -noout -out wrong-key.pem`).
   The rehearsal manifest uses `--sequence 2` against an assumed installed
   sequence of 1.
3. Run the test with the artifact paths:

```sh
TILEFINCH_PROOF_ENVELOPE=<tilefinch-update-v1.tfum> \
TILEFINCH_PROOF_WRONG_ENVELOPE=<wrong-key.tfum> \
TILEFINCH_PROOF_PACKAGE=<tilefinch-psp.tfup> \
  ./<build>/tilefinch-update-root-proof-tests
```

It verifies the embedded root against the envelope's signature and the
exact packed bytes, and demonstrates wrong-key, tampered-byte, downgrade,
equivocation, and expiry rejection plus the A/B trial and recovery walk.
Without the environment variables (or in an updater-disabled build) the
test skips, so ordinary suite runs are unaffected. See "Root v1 provenance"
in SECURE_UPDATES.md for the ceremony record.

## Private local PPSSPP qualification

A signed update can be exercised end to end before any repository or release
asset is public. The qualification creates throwaway signing keys and a
throwaway TLS certificate authority, builds a validation-only PSP install
which embeds their public halves, serves the exact TFUM and TFUP from a
loopback HTTPS origin, and runs the stable launcher in an isolated PPSSPP
home:

```sh
export PSPDEV=/path/to/pspdev
export PATH="$PSPDEV/bin:$PATH"
export PPSSPP=/path/to/PPSSPPSDL
scripts/run-ppsspp-update-e2e.sh
```

The run must fetch and verify signed metadata, stream the package into the
transactional installer, leave the active slot intact, cold-start the stable
launcher, independently verify the pending slot, start its trial, and confirm
health from the candidate. Results are written to
`build-preset-psp-update-e2e/ppsspp-network-latest/`. The temporary private
keys, local CA, served package, and isolated PPSSPP home are deleted when the
run ends; none belongs in `dist/` or source control.

This proves the on-device TLS verifier, signed Stable client, package hash,
installer, redundant journal, launcher, and health transition without
granting a private GitHub repository to PPSSPP. It does not qualify the
production signing keys, GitHub availability, power-loss behavior, or PSP
firmware. Those remain separate release gates.

`tools/local_update_server.py` is deliberately not a general file server. It
exposes exactly one metadata file and one package, supports byte ranges, caps
requests, and has deterministic `drop-package-once`, `truncate-package`, and
`corrupt-metadata` modes for fault work. Its TLS/range/allowlist/fault
contracts are covered by `tilefinch-local-update-server-tests`; destructive
installer power-loss sweeps remain in the pure host updater tests.

## Optional device investigations

The deterministic host, PSP cross-build, package, and emulator gates above are
the release requirements. The following hardware panels are useful when a
change touches their subsystem, but are not required for every release:

- Expand the media matrix across 240p/360p, 23.976/24/29.97/30 fps,
  repeated stalls, preview seeking, every close point, forced codec/DMA
  timeouts, and long-run latency/skew baselines.
- Exercise forced AP loss through apctl rejoin, full network-stack restart,
  and the final Offline state.
- Exercise a fault-injected transport lease wedge through retain, late
  release, and recovery.
- Run a compressed network stack-fatigue sequence of repeated
  open/navigate/close cycles.
- Exercise the physical power switch through suspend and resume.
