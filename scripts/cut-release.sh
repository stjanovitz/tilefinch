#!/bin/sh
# Cut a Tilefinch PSP release.
#
# This script runs every gate and packaging step that needs no credentials:
# it verifies the tree, runs the host release test suite, performs a clean
# PSP release build with the requested release sequence, verifies the staged
# notices manifest, assembles the release zip and the unsigned TFUP update
# package, and emits SHA-256 sums. It then prints the exact remaining manual
# steps — offline signing, tagging, and drafting the GitHub release — and
# deliberately performs none of them. See docs/RELEASE_PROCESS.md.
#
# Every failure is fatal; there are no fallbacks.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    cat >&2 <<'EOF'
usage: scripts/cut-release.sh [options] VERSION RELEASE_SEQUENCE

  VERSION           release version as MAJOR.MINOR.PATCH (example: 0.1.0);
                    must match the project() version in CMakeLists.txt and a
                    finalized "## VERSION" section in CHANGELOG.md
  RELEASE_SEQUENCE  monotonic signed release sequence (unsigned integer);
                    becomes TILEFINCH_RELEASE_SEQUENCE in the PSP build

Exactly one of the following two options is required, so that shipping
without an updater trust root is always a deliberate choice:
  --update-root FILE   embed the public root-v1 record (root-v1.tfur)
                       exported by the offline key ceremony
  --no-update-root     build with the in-app updater disabled

Optional update-channel overrides (defaults come from CMakeLists.txt):
  --update-owner NAME  GitHub owner the device fetches releases from
  --update-repo NAME   GitHub repository the device fetches releases from
EOF
    exit 2
}

fail() {
    printf 'cut-release: error: %s\n' "$1" >&2
    exit 1
}

step() {
    printf '\n==> %s\n' "$1"
}

trap 'status=$?; if [ "$status" -ne 0 ]; then
    printf "\ncut-release: FAILED (exit %s) — nothing was tagged or published\n" \
        "$status" >&2
fi' EXIT

update_root=
update_root_choice=
update_owner=
update_repo=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --update-root)
            [ "$#" -ge 2 ] || usage
            update_root=$2
            update_root_choice=root
            shift 2
            ;;
        --no-update-root)
            update_root_choice=none
            shift
            ;;
        --update-owner)
            [ "$#" -ge 2 ] || usage
            update_owner=$2
            shift 2
            ;;
        --update-repo)
            [ "$#" -ge 2 ] || usage
            update_repo=$2
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        -*)
            usage
            ;;
        *)
            break
            ;;
    esac
done
[ "$#" -eq 2 ] || usage
version=$1
sequence=$2

# --- Argument validation -------------------------------------------------

printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || fail "VERSION must be MAJOR.MINOR.PATCH, got: $version"
printf '%s\n' "$sequence" | grep -Eq '^[1-9][0-9]*$' \
    || fail "RELEASE_SEQUENCE must be a positive integer, got: $sequence"
[ -n "$update_root_choice" ] \
    || fail "pass --update-root FILE or an explicit --no-update-root"
if [ "$update_root_choice" = root ]; then
    [ -f "$update_root" ] \
        || fail "update root does not exist: $update_root"
    case "$update_root" in
        /*) ;;
        *) update_root=$(CDPATH= cd -- "$(dirname -- "$update_root")" \
               && pwd)/$(basename -- "$update_root") ;;
    esac
fi

# --- Environment and tree gates ------------------------------------------

step "Checking environment"
[ -n "${PSPDEV:-}" ] || fail "export PSPDEV=/path/to/pspdev first"
[ -f "$PSPDEV/psp/share/pspdev.cmake" ] \
    || fail "no PSP toolchain file at \$PSPDEV/psp/share/pspdev.cmake"
command -v cmake >/dev/null 2>&1 || fail "cmake not found"
command -v ctest >/dev/null 2>&1 || fail "ctest not found"
command -v git >/dev/null 2>&1 || fail "git not found"
command -v python3 >/dev/null 2>&1 \
    || fail "python3 not found (needed for the TFUP packer)"
decoder_abi=$(sed -n \
    's/^#define TILEFINCH_SWDEC_COMPONENT_ABI_VERSION \([0-9][0-9]*\)u$/\1/p' \
    "$root/include/tilefinch/swdec_component.h")
printf '%s\n' "$decoder_abi" | grep -Eq '^[1-9][0-9]*$' \
    || fail "could not derive the optional decoder ABI"

step "Checking the source tree"
git -C "$root" rev-parse --verify HEAD >/dev/null 2>&1 \
    || fail "$root is not a git repository with commits"
dirty=$(git -C "$root" status --porcelain)
[ -z "$dirty" ] || {
    printf '%s\n' "$dirty" >&2
    fail "the tree is not clean; commit or stash everything first"
}
commit=$(git -C "$root" rev-parse HEAD)

grep -Fq "project(psp_browser_tilefinch VERSION $version " \
    "$root/CMakeLists.txt" \
    || fail "CMakeLists.txt project() version does not match $version; \
update it (and commit) before cutting"
grep -Eq "^## $version( |\$)" "$root/CHANGELOG.md" \
    || fail "CHANGELOG.md has no finalized '## $version' section; \
see docs/RELEASE_PROCESS.md for the required edit"

dist="$root/dist/tilefinch-v$version"
[ ! -e "$dist" ] \
    || fail "refusing to overwrite existing $dist; remove it first"

jobs=${PSP_BROWSER_JOBS:-}
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>&1) || jobs=4
fi

# --- Host release gate ----------------------------------------------------

step "Host release gate: configure, build, and test build-preset-release"
(cd "$root" && cmake --preset release)
(cd "$root" && cmake --build --preset release)
ctest --test-dir "$root/build-preset-release" -j "$jobs" --output-on-failure
printf '%s\n' "Host suite passed. Note: the localhost redirect test reports \
Skipped in sandboxes that forbid loopback sockets; a release cut must run \
in an ordinary host environment where it actually runs."

# --- Fidelity floor ratchet -----------------------------------------------

# The flagship fidelity gate registers only under a Release build type and
# carries SKIP_RETURN_CODE 77, so in the suite above a missing capture or a
# non-Release tree reports green without the ratchet ever having run. A
# release is exactly the moment that is not acceptable: re-run it alone here
# and refuse to package unless it really executed and really passed.
step "Fidelity floor ratchet (must run, not skip)"
floor_log="$root/build-preset-release/cut-release-fidelity-floor.log"
floor_status=0
ctest --test-dir "$root/build-preset-release" \
    -R '^tilefinch-fidelity-floor-tests$' --output-on-failure \
    >"$floor_log" 2>&1 || floor_status=$?
cat "$floor_log"
[ "$floor_status" -eq 0 ] \
    || fail "the fidelity floor ratchet failed (see $floor_log); \
no release is cut from a tree that regressed fidelity"
grep -q 'No tests were found' "$floor_log" && fail "the fidelity floor \
ratchet is not registered in build-preset-release; it registers only for \
CMAKE_BUILD_TYPE=Release, so this tree cannot have been configured with \
the release preset"
# CTest counts a skip as a pass in its own summary line, so the skip marker
# is the only thing that distinguishes "the ratchet held" from "the ratchet
# never ran".
grep -q '\*\*\*Skipped' "$floor_log" && fail "the fidelity floor ratchet \
SKIPPED (exit 77) instead of running; see $floor_log. Its captures and \
references must be present for a release cut — a skip here is the gate not \
running at all, which is the one outcome a release must never accept."
grep -q '0 tests failed out of 1' "$floor_log" \
    || fail "could not confirm from $floor_log that exactly one fidelity \
floor test ran and passed"
printf '%s\n' "Fidelity floor ratchet ran and passed."

# --- Clean PSP release build ----------------------------------------------

step "Clean PSP release configure (release sequence $sequence)"
rm -rf "$root/build-preset-psp"
set -- "-DTILEFINCH_RELEASE_SEQUENCE=$sequence"
set -- "$@" "-DTILEFINCH_PSP_ENABLE_SWDEC_COMPONENT=OFF"
if [ "$update_root_choice" = root ]; then
    set -- "$@" "-DTILEFINCH_UPDATE_ROOT_V1=$update_root"
else
    printf '%s\n' "NOTE: building WITHOUT an update trust root. This build \
cannot verify or install in-app updates; users of it must update by \
manually replacing the installation." >&2
fi
if [ -n "$update_owner" ]; then
    set -- "$@" "-DTILEFINCH_UPDATE_REPOSITORY_OWNER=$update_owner"
fi
if [ -n "$update_repo" ]; then
    set -- "$@" "-DTILEFINCH_UPDATE_REPOSITORY_NAME=$update_repo"
fi
(cd "$root" && cmake --preset psp "$@")

step "Building the PSP install tree (launcher, browser, staged assets)"
(cd "$root" && cmake --build build-preset-psp \
    --target tilefinch-psp-install-tree -j "$jobs")

# --- Verify the staged tree against the notices manifest ------------------

step "Verifying the staged install tree"
tree="$root/build-preset-psp/tilefinch-install/Tilefinch"
[ -d "$tree" ] || fail "staged install tree missing: $tree"
for required in \
    "$tree/EBOOT.PBP" \
    "$tree/slot-a/EBOOT.PBP" \
    "$tree/slot-a/roots.pem" \
    "$tree/slot-a/boot-defaults.cfg"
do
    [ -f "$required" ] || fail "staged tree is missing $required"
done
for forbidden in \
    "$tree/slot-a/tilefinch-swdec.prx" \
    "$tree/slot-a/swdec-meload.prx" \
    "$tree/components/swdec/tilefinch-swdec.prx" \
    "$tree/components/swdec/swdec-meload.prx"
do
    [ ! -e "$forbidden" ] \
        || fail "official release unexpectedly contains optional decoder: $forbidden"
done
# StagePspInstall.cmake already fails the build when the manifest is not
# satisfied; re-check every entry here independently so packaging cannot
# proceed on a stale or hand-edited tree.
notices=$(sed -n \
    -e 's,^    \(NOTICES/[A-Za-z0-9._/-]*\)$,\1,p' \
    -e 's,^    \(slot-a/[A-Za-z0-9._/-]*\)$,\1,p' \
    "$root/cmake/PspNoticesManifest.cmake")
[ -n "$notices" ] \
    || fail "could not read any entries from cmake/PspNoticesManifest.cmake"
notice_count=0
for entry in $notices; do
    [ -f "$tree/$entry" ] \
        || fail "staged tree is missing required notice file: $entry"
    notice_count=$((notice_count + 1))
done
printf 'Notices manifest verified: %s required files present.\n' \
    "$notice_count"

# Compiler-expanded __FILE__ strings are useful diagnostics, but release
# binaries must contain the stable virtual prefixes configured by CMake, not
# the release builder's source tree or home directory. Check the staged PBPs
# before either the first-install zip or update package can be assembled.
step "Checking staged binaries for local build paths"
for binary in "$tree/EBOOT.PBP" "$tree/slot-a/EBOOT.PBP"; do
    if LC_ALL=C grep -aFq "$root/" "$binary"; then
        fail "$binary contains the local source/build path $root"
    fi
    if [ -n "${HOME:-}" ] && LC_ALL=C grep -aFq "$HOME/" "$binary"; then
        fail "$binary contains the release builder's home path $HOME"
    fi
done
printf '%s\n' "Staged binaries contain no local source or home paths."

# --- Assemble the release artifacts ---------------------------------------

step "Assembling release artifacts in $dist"
mkdir -p "$dist/stage"
# The zip unpacks directly into PSP/GAME/ on the Memory Stick.
cp -R "$tree" "$dist/stage/TILEFINCH"
zip_name="tilefinch-v$version-psp.zip"
(cd "$dist/stage" && cmake -E tar cf "$dist/$zip_name" --format=zip TILEFINCH)
rm -rf "$dist/stage"
[ -s "$dist/$zip_name" ] || fail "zip assembly produced no output"

# The unsigned update package is built from the staged slot so that the
# bytes a first-install user receives and the bytes the updater installs are
# identical. The packer enforces the slot allowlist and fails on anything
# unexpected. Signing happens later, offline.
tfup_name="tilefinch-psp-v$version.tfup"
python3 "$root/tools/tilefinch_update_tool.py" pack \
    --directory "$tree/slot-a" --output "$dist/$tfup_name"
[ -s "$dist/$tfup_name" ] || fail "TFUP packing produced no output"

(cd "$dist" && cmake -E sha256sum "$zip_name" "$tfup_name") \
    > "$dist/SHA256SUMS.txt"

{
    printf 'Tilefinch release candidate\n'
    printf 'version:          %s\n' "$version"
    printf 'release sequence: %s\n' "$sequence"
    printf 'commit:           %s\n' "$commit"
    printf 'updater:          %s\n' \
        "$([ "$update_root_choice" = root ] \
            && printf 'enabled (root: %s)' "$update_root" \
            || printf 'disabled (no trust root embedded)')"
    printf 'built:            %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$dist/RELEASE-INFO.txt"

step "Artifacts"
cat "$dist/RELEASE-INFO.txt"
printf '\n'
cat "$dist/SHA256SUMS.txt"

# --- Remaining manual steps -----------------------------------------------

step "Remaining manual steps (need credentials or offline keys; not automated)"
cat <<EOF

1. Independently verify the package hash, then sign on the OFFLINE machine
   (docs/SECURE_UPDATES.md, "Build and release workflow"):

     python3 tools/tilefinch_update_tool.py manifest \\
       --package $tfup_name --root-version <N> \\
       --sequence $sequence --expires <unix-expiry> --version $version \\
       --tag v$version --asset $tfup_name \\
       --decoder-abi $decoder_abi \\
       --notes "<signed release notes>" --output manifest.tfum-body
     python3 tools/tilefinch_update_tool.py envelope \\
       --manifest manifest.tfum-body --release-key <offline-release-key> \\
       --output tilefinch-update-v1.tfum

   Compare the package SHA-256 the manifest embeds against
   dist/tilefinch-v$version/SHA256SUMS.txt before signing anything.
EOF
if [ "$update_root_choice" = none ]; then
    cat <<EOF
   (This build embeds no trust root, so skip signing unless a previously
   shipped updater-capable installation must also receive this release.)
EOF
fi
cat <<EOF

2. Tag the released commit and push the tag:

     git tag -a v$version -m "Tilefinch $version" $commit
     git push origin v$version

3. Draft the GitHub release for tag v$version. Paste the "## $version"
   section of CHANGELOG.md as the release notes and upload:
     - $zip_name
     - $tfup_name
     - tilefinch-update-v1.tfum   (fixed name; from the offline signing step)
     - SHA256SUMS.txt

4. After publishing, confirm the stable latest-release metadata URL serves
   the new envelope before announcing.

Nothing has been signed, tagged, or published by this script.
EOF
