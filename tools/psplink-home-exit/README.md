# PSPLink HOME-exit development build

The stock PSPLinkUSB v3.2.1 bootstrap unloads after starting its kernel module.
Its host `exit` command can return through `sceKernelExitVSHVSH()`, but no
device-local owner remains to turn the HOME button into that action. This patch
adds a low-priority controller watcher to the kernel module.

The watcher:

- requires HOME to be released once after launch;
- reacts only to a new HOME press;
- disables PSPLink hardware-debug state;
- releases its thread event handler and stops USB HostFS; and
- uses PSPLink's existing VSH exit function.

It does not force-unload a running target module. Exit Tilefinch normally
before pressing HOME from the PSPLink screen.

## Media-safe screenshots

This build also moves screenshot scratch storage from partition 4 to the
user partition (2). Upstream `scrshot` allocates 512 KiB at `0x88300000`,
overlapping the active Media Engine firmware. Capturing a playing video can
therefore wedge a later AVC or AAC decode. Do not use stock `scrshot` or `ss`
during media qualification.
See the [upstream screenshot allocator](https://github.com/pspdev/psplinkusb/blob/32f2fa9bca0259d68770b9678994e7ad2fd637c3/psplink/shell.c#L3159)
and the [Media Engine core mapping](https://github.com/mcidclan/psp-media-engine-custom-core).

The patched build provides `scrshot-user` (`ssu`) as an explicit capability.
The host wrapper rewrites `scrshot`/`ss` to that name: an older device build
rejects the command instead of silently using unsafe storage. There is no
fallback to partition 4 if user memory is insufficient. The temporary block
is freed after capture; no browser framebuffer ownership changes are involved.

After installing the rebuilt PSPLink, qualify a 360p firmware-video soak
with a `scrshot-user` capture during playback and verify that decoding
continues, teardown completes, and user-partition memory returns to baseline.
Never hot-unload the running PSPLink module to install an update.

Build the exact upstream v3.2.1 revision:

```sh
git clone --branch v3.2.1 --depth 1 \
  https://github.com/pspdev/psplinkusb.git /tmp/psplinkusb-v3.2.1
PSPDEV=/path/to/pspdev scripts/build-psplink-home-exit.sh \
  /tmp/psplinkusb-v3.2.1
```

Back up the existing `ms0:/PSP/GAME/PSPLINK/` directory before installing the
six generated files. The custom build should first be tested by launching
PSPLink, waiting for HostFS to connect, disconnecting the host bridge, and
pressing HOME. Returning to XMB without a reboot is the acceptance criterion.
