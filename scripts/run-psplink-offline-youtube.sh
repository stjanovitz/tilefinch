#!/bin/sh
set -eu

# Offline YouTube journey on a real PSP (tests/input-scripts/
# offline-youtube-live.txt): the offline library page, Play, previews,
# a committed seek, a rewind that rebuilds the decoder with presses during
# its reopen, pause, close. No network: an offline start URL defers Wi-Fi,
# and the offline route reads the clip from host0. No Memory Stick writes:
# the PRX, profile, library and validation log all live on host0, and
# everything this run stages or creates there is removed or restored.
#
# Build the fixture first: tools/make-offline-youtube-fixture.sh

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build-preset-psp-validation}
FIXTURE=${OFFLINE_YOUTUBE_FIXTURE:-$ROOT/perf/fixtures/offline-youtube/offline}
PSPSH=${PSPSH:-pspsh}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-300}
LOG=$BUILD_DIR/tilefinch-validation.txt
SCENARIO=offline-youtube-live.txt

[ -f "$FIXTURE/library.bin" ] || {
    printf '%s\n' "missing fixture $FIXTURE;" \
        "build it with tools/make-offline-youtube-fixture.sh" >&2
    exit 2
}
BACKUP=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-offline-youtube.XXXXXX")

PSPSH=$PSPSH HOST_ROOT=$BUILD_DIR \
    "$ROOT/scripts/psplink-shell.sh" ready

# Files this run replaces on host0, restored afterwards; anything else the
# run creates there (profile data, frame marks) is removed.
STAGED="boot.cfg profile.cfg profile.cfg.bak local-storage.bin local-storage.bin.bak http-cache.bin http-cache.bin.bak site-storage offline $SCENARIO"
staged=0
cleanup() {
    # Nothing on host0 was moved aside yet: leave every file where it is.
    [ "$staged" -eq 1 ] || return 0
    # Only names this run moved aside (host0/) or found absent (absent/)
    # are replaced; a name staging never reached keeps its original file.
    for name in $STAGED; do
        if [ -e "$BACKUP/host0/$name" ]; then
            rm -rf "${BUILD_DIR:?}/$name"
            mv "$BACKUP/host0/$name" "$BUILD_DIR/$name"
        elif [ -e "$BACKUP/absent/$name" ]; then
            rm -rf "${BUILD_DIR:?}/$name"
        fi
    done
    [ -f "$BACKUP/listing.before" ] || return 0
    ls -A "$BUILD_DIR" | sort > "$BACKUP/listing.after"
    for name in $(comm -13 "$BACKUP/listing.before" "$BACKUP/listing.after"); do
        case "$name" in
            tilefinch-validation.txt) ;;
            *) rm -rf "${BUILD_DIR:?}/$name" ;;
        esac
    done
}
trap cleanup EXIT HUP INT TERM

cmake --build "$BUILD_DIR" --target psp-browser-script-dev-prx -j8
# Listed after the build, so only what the run itself adds is removed.
ls -A "$BUILD_DIR" | sort > "$BACKUP/listing.before"

mkdir -p "$BACKUP/host0" "$BACKUP/absent"
staged=1
for name in $STAGED; do
    if [ -e "$BUILD_DIR/$name" ]; then
        mv "$BUILD_DIR/$name" "$BACKUP/host0/$name"
    else
        : > "$BACKUP/absent/$name"
    fi
done
if [ -f "$LOG" ]; then
    mv "$LOG" "$BACKUP/validation.previous.txt"
fi
cp -R "$FIXTURE" "$BUILD_DIR/offline"
cp "$ROOT/tests/input-scripts/$SCENARIO" "$BUILD_DIR/$SCENARIO"

cat > "$BUILD_DIR/boot.cfg" <<EOF
url=https://tilefinch.local/offline
trace=none
profile=realistic
network_profile=1
ticks=0
dump_frame=0
exit_after_report=0
interactive_validation_ticks=0
validation_cancel_after_ms=0
validation_preview_scroll=0
validation_media_play=0
validation_media_stability_auto=0
validation_power_test_auto=0
input_script=$SCENARIO
exit_to=ms0:/PSP/GAME/PSPLINK/EBOOT.PBP
EOF

"$PSPSH" -e modlist > "$BACKUP/modules.before.txt"
if grep -q 'Name: Tilefinch$' "$BACKUP/modules.before.txt"; then
    "$PSPSH" -e 'modstun @Tilefinch' > "$BACKUP/module-stop.txt" 2>&1
    "$PSPSH" -e modlist > "$BACKUP/modules.after.txt"
    if grep -q 'Name: Tilefinch$' "$BACKUP/modules.after.txt"; then
        echo "FAIL: stale Tilefinch module could not be unloaded" >&2
        exit 1
    fi
fi

load_output=$("$PSPSH" -e "ld host0:/psp-browser-script-dev.prx" 2>&1)
printf '%s\n' "$load_output"
if printf '%s\n' "$load_output" | grep -q 'Failed to Load/Start module'; then
    echo "FAIL: PSPLink rejected the freshly built validation PRX" >&2
    exit 1
fi

elapsed=0
while [ "$elapsed" -lt "$TIMEOUT_SECONDS" ]; do
    if [ -f "$LOG" ] \
        && grep -q "tilefinch-log: finish outcome=" "$LOG"; then
        python3 "$ROOT/scripts/verify-psplink-offline-youtube.py" "$LOG"
        exit $?
    fi
    sleep 5
    elapsed=$((elapsed + 5))
done

echo "FAIL: offline YouTube journey exceeded ${TIMEOUT_SECONDS}s" >&2
if [ -f "$LOG" ]; then tail -40 "$LOG" >&2; fi
exit 1
