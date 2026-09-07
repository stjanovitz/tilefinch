#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
psplink_source=${1:-}
output=${2:-$root/build-preset-psp-validation/psplink-home-exit}
expected_revision=32f2fa9bca0259d68770b9678994e7ad2fd637c3

if [ -z "$psplink_source" ] || [ -z "${PSPDEV:-}" ]; then
    echo "usage: PSPDEV=/path/to/pspdev $0 PSPLINK_V3.2.1_SOURCE [OUTPUT]" >&2
    exit 2
fi

revision=$(git -C "$psplink_source" rev-parse HEAD 2>/dev/null || true)
if [ "$revision" != "$expected_revision" ]; then
    echo "expected PSPLinkUSB v3.2.1 ($expected_revision), found $revision" >&2
    exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-psplink-home.XXXXXX")
cleanup() { rm -rf "$work"; }
trap cleanup EXIT HUP INT TERM
rsync -a --exclude .git "$psplink_source/" "$work/"
patch -s -d "$work" -p1 \
    < "$root/tools/psplink-home-exit/psplink-v3.2.1-home-exit.patch"
patch -s -d "$work" -p1 \
    < "$root/tools/psplink-home-exit/psplink-v3.2.1-safe-screenshot.patch"

PATH=$PSPDEV/bin:$PATH
export PSPDEV PATH
make -C "$work" -f Makefile.psp release -j"${JOBS:-8}"

mkdir -p "$output"
cp "$work/bootstrap/EBOOT.PBP" "$output/EBOOT.PBP"
cp "$work/psplink/psplink.prx" "$output/psplink.prx"
cp "$work/psplink/psplink.ini" "$output/psplink.ini"
cp "$work/psplink_user/psplink_user.prx" "$output/psplink_user.prx"
cp "$work/usbhostfs/usbhostfs.prx" "$output/usbhostfs.prx"
cp "$work/usbgdb/usbgdb.prx" "$output/usbgdb.prx"
printf 'PSPLink HOME-exit build staged at %s\n' "$output"
