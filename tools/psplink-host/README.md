# PSPLink host bridge lifetime fix

The stock `usbhostfs_pc` can close its shared libusb handle on the filesystem
thread while the async command thread is submitting through that handle.
The resulting host crash can interrupt a completed device test's receipt.
It is distinct from a Tilefinch crash on the PSP.

The patch serializes handle publication and retirement with async endpoint-3
writes. The filesystem owner's blocking reads remain concurrent, preserving
duplex protocol progress. Shutdown exits the host process instead of calling
libusb from an arbitrary signal context or resetting a handle still used by
another thread. The OS releases the process's handles; ordinary reconnect
cleanup retains the existing USB reset after its admitted writer finishes.

## Build and verify

Requires a POSIX host, C compiler, make, patch, Git, pkg-config and libusb 1.0
development files. No PSP cross-toolchain is needed.

```sh
git clone https://github.com/pspdev/psplinkusb.git /path/to/psplinkusb
scripts/build-psplink-host.sh --test /path/to/psplinkusb /path/to/host-tools
```

The builder archives official revision
`e779d94779ff524c5bccd581ea4dafd001e00006` into a temporary directory before
patching and building. It ignores checkout modifications and prebuilt objects.
It stages the binary, upstream license and build identity without installing
anything in the SDK, running the bridge, or contacting the PSP. Upstream code
retains its BSD license.

`--test` runs 200 forced write/retirement/reconnect interleavings against the
actual patched source with mocked USB calls, checks no-device refusal and
signal exit, and requires the original unlocked dispatch to fail the same
lifetime check. These tests remain active under `NDEBUG`. They do not require
or access hardware. Host tests establish the lifetime contract; a normal
device run and clean retirement receipts still qualify the built bridge.

## Select explicitly

After the active PSP application has exited normally and its retirement checks
are complete, stop the old tracked host bridge before selecting a new binary:

```sh
PSPDEV=/path/to/pspdev USBHOSTFS=/path/to/host-tools/usbhostfs_pc \
  HOST_ROOT=/absolute/stage scripts/psplink-shell.sh hold
```

Setting `USBHOSTFS` does not replace an already-running bridge. Keep the hold
session alive throughout the device application and its final receipts.
Never change the host root, replace the bridge, or silently reconnect it under
a live application: the application's `host0:` descriptors may become stale.
