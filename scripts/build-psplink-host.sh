#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
expected_revision=e779d94779ff524c5bccd581ea4dafd001e00006
run_tests=0
if [ "${1:-}" = --test ]; then run_tests=1; shift; fi
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 [--test] PSPLINK_SOURCE [OUTPUT_DIRECTORY]" >&2
    exit 2
fi
psplink_source=$1
output=${2:-$root/build-preset-psp-validation/psplink-host}
compiler=${CC:-cc}
git -C "$psplink_source" cat-file -e "$expected_revision^{commit}" || {
    echo "PSPLink source must contain official revision $expected_revision" >&2
    exit 2
}
pkg-config --exists libusb-1.0 || {
    echo "libusb-1.0 development files and pkg-config are required" >&2
    exit 2
}

work=$(mktemp -d "${TMPDIR:-/tmp}/tilefinch-psplink-host.XXXXXX")
cleanup() { rm -rf "$work"; }
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$work/source"
# Archive the pinned object, never the checkout: unrelated/dirty source and
# prebuilt objects must not silently enter a device-transport qualification.
git -C "$psplink_source" archive --format=tar --output="$work/source.tar" \
    "$expected_revision" usbhostfs_pc usbhostfs LICENSE
tar -xf "$work/source.tar" -C "$work/source"
patch -s -d "$work/source" -p1 \
    < "$root/tools/psplink-host/usb-handle-lifetime.patch"
make -C "$work/source/usbhostfs_pc" -j"${JOBS:-2}" CC="$compiler"

if [ "$run_tests" -eq 1 ]; then
    # pkg-config intentionally supplies shell-separated compiler arguments.
    # This fixture includes the actual patched source but mocks USB calls;
    # neither executable initializes USB or contacts a PSP.
    "$compiler" -O2 -g -Wall -pthread -DPC_SIDE -D_FILE_OFFSET_BITS=64 \
        $(pkg-config --cflags libusb-1.0) \
        -I"$work/source" -I"$work/source/usbhostfs" \
        "$root/tools/psplink-host/test-lifetime.c" \
        $(pkg-config --libs libusb-1.0) -o "$work/test-lifetime"
    "$work/test-lifetime"
    "$compiler" -O2 -g -Wall -pthread -DPC_SIDE -D_FILE_OFFSET_BITS=64 \
        -DUNSAFE_BASELINE $(pkg-config --cflags libusb-1.0) \
        -I"$work/source" -I"$work/source/usbhostfs" \
        "$root/tools/psplink-host/test-lifetime.c" \
        $(pkg-config --libs libusb-1.0) -o "$work/test-unsafe"
    status=0
    "$work/test-unsafe" >"$work/unsafe.log" 2>&1 || status=$?
    if [ "$status" -ne 1 ] || ! grep -Fq \
        'pthread_mutex_trylock(&usb_lifetime_mutex) == EBUSY' "$work/unsafe.log"; then
        cat "$work/unsafe.log" >&2
        echo "The original unsafe dispatch must fail the lifetime regression" >&2
        exit 1
    fi
    echo "Original unlocked-dispatch negative control failed as expected"
fi

mkdir -p "$output"
cp "$work/source/usbhostfs_pc/usbhostfs_pc" "$output/usbhostfs_pc"
cp "$work/source/LICENSE" "$output/LICENSE.psplink"
printf 'upstream=%s\nlifetime_tests=%s\n' "$expected_revision" "$run_tests" \
    > "$output/build-info.txt"
printf 'PSPLink host bridge staged at %s/usbhostfs_pc\n' "$output"
echo "No SDK installation or device connection was performed."
