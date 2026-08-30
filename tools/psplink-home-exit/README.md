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
