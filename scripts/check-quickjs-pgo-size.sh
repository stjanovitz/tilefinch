#!/bin/sh
# Check that profile feedback does not grow the PSP shipping image.
#
#   scripts/check-quickjs-pgo-size.sh PROFILE_DIR [OUT_DIR] [-- CMAKE_ARG...]
#   scripts/check-quickjs-pgo-size.sh --trees PGO_TREE BASE_TREE
#
# The first form configures and builds (psp-browser-script) two shipping PSP
# trees from this checkout, which must be the one the profile was trained in,
# under OUT_DIR (default build-pgo-size): pgo/ with
# -DPSP_BROWSER_QUICKJS_PGO_USE=PROFILE_DIR and base/ without a profile.
# Later runs rebuild them incrementally. CMAKE_ARGs after -- go to pgo/ only,
# to size a candidate engine flag; they stay in its cache, except
# PSP_BROWSER_QUICKJS_OPT_FLAGS, which every run resets. pgo/ builds with
# the .text ratchet lifted so that this script, not the build, reports both
# bounds. The second form measures two shipping trees already built from one
# checkout.
#
# It passes only if the profiled .text is at most the shipping .text limit
# and at most the .text without a profile; otherwise it exits 1 naming the
# bound it broke (2 for a usage or tree error). Run it after every retrain and
# whenever a merge touches third_party/quickjs, the engine patches or the
# engine flags (docs/RELEASE_PROCESS.md, step 2). The caller owns any lock
# that serializes builds on the machine. JOBS sets the build's -j (default 4).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    sed -n '2,23p' "$0" >&2
    exit 2
}

die() {  # die STATUS MESSAGE
    printf 'check-quickjs-pgo-size: %s\n' "$2" >&2
    exit "$1"
}

cache_value() {  # cache_value TREE NAME: the value of NAME in TREE's cache
    sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt" | head -n 1
}

text_bytes() {  # text_bytes ELF: .text plus every .text.* section
    # POSIX sh reports only the last pipeline stage's status. Never let
    # partial or missing tool output turn an unreadable ELF into a win.
    sections=$("$PSPDEV/bin/psp-size" -A "$1") || return 1
    printf '%s\n' "$sections" | awk '
        $1 == ".text" || $1 ~ /^\.text\./ {
            if ($2 !~ /^[0-9]+$/) bad = 1
            total += $2
        }
        END { if (bad || total <= 0) exit 1; printf "%.0f\n", total }'
}

compare() {  # compare PGO_TREE BASE_TREE
    pgo=$1
    base=$2
    for tree in "$pgo" "$base"; do
        [ -f "$tree/CMakeCache.txt" ] || die 2 "$tree is not a build tree"
        [ "$(cache_value "$tree" TILEFINCH_PSP_VALIDATION_LOG)" = OFF ] \
            || die 2 "$tree is not a shipping tree (TILEFINCH_PSP_VALIDATION_LOG is on)"
        [ "$(cache_value "$tree" PSP_BROWSER_QUICKJS_PGO_GENERATE)" = OFF ] \
            || die 2 "$tree is an instrumented training tree"
        [ -f "$tree/psp-browser-script" ] \
            || die 2 "$tree has no psp-browser-script (build that target)"
    done
    [ "$(cache_value "$pgo" CMAKE_HOME_DIRECTORY)" = \
        "$(cache_value "$base" CMAKE_HOME_DIRECTORY)" ] \
        || die 2 "$pgo and $base are built from different checkouts"
    profile=$(cache_value "$pgo" PSP_BROWSER_QUICKJS_PGO_USE)
    [ -n "$profile" ] || die 2 "$pgo is not built with a QuickJS profile"
    [ -z "$(cache_value "$base" PSP_BROWSER_QUICKJS_PGO_USE)" ] \
        || die 2 "$base is built with a QuickJS profile; it must have none"
    [ -z "$(cache_value "$base" PSP_BROWSER_QUICKJS_OPT_FLAGS)" ] \
        && [ -z "$(cache_value "$base" PSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE)" ] \
        || die 2 "$base sets experiment flags or a .text override; it must be a plain shipping tree"
    limit=$(cache_value "$base" TILEFINCH_PSP_TEXT_RATCHET)
    [ -n "$limit" ] \
        || die 2 "$base records no shipping .text limit (reconfigure it)"
    with=$(text_bytes "$pgo/psp-browser-script") || die 2 "cannot measure .text in $pgo"
    without=$(text_bytes "$base/psp-browser-script") || die 2 "cannot measure .text in $base"
    printf 'QuickJS profile: %s\n' "$profile"
    if [ -f "$profile/PROVENANCE" ]; then
        sed -n -e 's/^fingerprint=/  trained on engine build /p' \
            -e 's/^source-dir=/  in checkout /p' \
            -e 's/^commit=/  at commit /p' "$profile/PROVENANCE"
    fi
    extra=$(cache_value "$pgo" PSP_BROWSER_QUICKJS_OPT_FLAGS)
    [ -z "$extra" ] || printf 'Extra engine flags in %s: %s\n' "$pgo" "$extra"
    printf 'Shipping .text limit:   %s\n' "$limit"
    printf 'Without a profile:      %s (%s)\n' "$without" "$base"
    printf 'With the profile:       %s (%s), %+d against no profile\n' \
        "$with" "$pgo" "$((with - without))"
    status=0
    if [ "$with" -gt "$limit" ]; then
        printf 'FAIL: the profiled .text is %s bytes over the shipping limit.\n' \
            "$((with - limit))" >&2
        status=1
    fi
    if [ "$with" -gt "$without" ]; then
        printf 'FAIL: the profiled .text is %s bytes larger than without a profile.\n' \
            "$((with - without))" >&2
        status=1
    fi
    if [ "$status" -ne 0 ]; then
        printf '%s\n' "Profile feedback grew the engine. Find the pass that grew it \
(compare the engine objects of both trees) and limit it in the PGO-use options \
of cmake/TilefinchDependencies.cmake, or ship without a profile." >&2
        exit 1
    fi
    printf 'PASS: the profiled .text is within the shipping limit and no larger than without a profile.\n'
}

: "${PSPDEV:?export PSPDEV}"
[ "$#" -ge 1 ] || usage
if [ "$1" = --trees ]; then
    [ "$#" -eq 3 ] || usage
    compare "$2" "$3"
    exit 0
fi
case "$1" in -*) usage ;; esac
[ -f "$1/PROVENANCE" ] || die 2 "no PROVENANCE in the QuickJS profile directory $1"
profile=$(CDPATH= cd -- "$1" && pwd)
shift
out=$root/build-pgo-size
if [ "$#" -gt 0 ] && [ "$1" != -- ]; then
    out=$1
    shift
fi
if [ "$#" -gt 0 ]; then
    [ "$1" = -- ] || usage
    shift
fi
jobs=${JOBS:-4}
common="-DTILEFINCH_ALLOW_BUILD_DIR=ON -DTILEFINCH_PSP_VALIDATION_LOG=OFF \
-DPSP_BROWSER_QUICKJS_PGO_GENERATE=OFF -DPSP_BROWSER_QUICKJS_OPT_FLAGS="
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
# shellcheck disable=SC2086 # $common is a fixed list of words
(cd "$root" && cmake --preset psp -B "$out/base" $common \
    -DPSP_BROWSER_QUICKJS_PGO_USE= -DPSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE=) \
    >"$out/base-configure.log" 2>&1 \
    || die 2 "configuring $out/base failed (see $out/base-configure.log)"
cmake --build "$out/base" -j "$jobs" --target psp-browser-script \
    >"$out/base-build.log" 2>&1 \
    || die 2 "building $out/base failed (see $out/base-build.log)"
# The ratchet is lifted in pgo/ only, so that the comparison below reports
# an oversized profiled image instead of the build stopping at it.
# shellcheck disable=SC2086
(cd "$root" && cmake --preset psp -B "$out/pgo" $common \
    "-DPSP_BROWSER_QUICKJS_PGO_USE=$profile" \
    -DPSP_BROWSER_PSP_TEXT_LIMIT_OVERRIDE=16777216 "$@") \
    >"$out/pgo-configure.log" 2>&1 \
    || die 2 "configuring $out/pgo failed (see $out/pgo-configure.log)"
cmake --build "$out/pgo" -j "$jobs" --target psp-browser-script \
    >"$out/pgo-build.log" 2>&1 \
    || die 2 "building $out/pgo failed (see $out/pgo-build.log)"
compare "$out/pgo" "$out/base"
