#!/usr/bin/env python3
"""Fail a PPSSPP harness run that reached past the host's loopback interface.

PPSSPP forwards the PSP's sceNetInet sockets and sceNetResolver lookups to
the host's real network. A harness run that is meant to be offline (trace
replay, native HOME, a loopback fixture, the local update origin) must
therefore show no connection, datagram, or name lookup for anything other
than a loopback address in the emulator's own syscall log. That log is the
emulator's record of what the game asked the host to do, not a counter the
app kept for itself.

The check needs PPSSPP's -d log: at the default level the socket syscalls
are not logged at all, so a log without debug lines is refused rather than
passed.

Usage:
    check_ppsspp_network.py PPSSPP_LOG

Exit status: 0 clean, 1 outbound traffic found, 2 the log cannot answer.
"""
import argparse
import re
import sys

DEBUG_LINE = re.compile(r"\sD\[[A-Z0-9]+\]:")
# The HLE result line of a socket syscall that names a peer, for example
#   -1=sceNetInetConnect(1, 09e799c8, 16): errno = ... Address = 127.0.0.1, Port = 55187
SOCKET_CALL = re.compile(r"=sceNetInet(Connect|Sendto|Sendmsg)\(")
PEER = re.compile(r"Address = ([^,\s]+), Port = (\d+)")
LOOKUPS = (
    re.compile(r"Direct lookup of '([^']*)'"),
    re.compile(r"NetResolver_StartNtoA - Hostname: (\S+)"),
)
LOOPBACK_NAMES = {"localhost", "localhost.", "::1"}


def is_loopback(host):
    host = host.strip().lower()
    if host in LOOPBACK_NAMES:
        return True
    parts = host.split(".")
    return (len(parts) == 4 and parts[0] == "127"
            and all(p.isdigit() and int(p) < 256 for p in parts))


def check(log):
    """Returns (summary, violations) for one PPSSPP log text."""
    debug = False
    loopback = 0
    violations = []
    for number, line in enumerate(log.splitlines(), 1):
        if not debug and DEBUG_LINE.search(line):
            debug = True
        if SOCKET_CALL.search(line):
            peer = PEER.search(line)
            if peer is None:
                violations.append((number, "socket call without a peer "
                                   "address", line.strip()))
            elif is_loopback(peer.group(1)):
                loopback += 1
            else:
                violations.append((number, "outbound %s:%s"
                                   % (peer.group(1), peer.group(2)),
                                   line.strip()))
            continue
        for pattern in LOOKUPS:
            lookup = pattern.search(line)
            if lookup is not None and not is_loopback(lookup.group(1)):
                violations.append((number, "name lookup %s"
                                   % lookup.group(1), line.strip()))
                break
    if not debug:
        raise ValueError("not a PPSSPP -d log: socket syscalls are only "
                         "logged at debug level")
    summary = ("ppsspp-network-check: outcome=%s loopback=%d outbound=%d"
               % ("fail" if violations else "pass", loopback,
                  len(violations)))
    return summary, violations


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log")
    args = parser.parse_args()
    try:
        with open(args.log, encoding="utf-8", errors="replace") as handle:
            summary, violations = check(handle.read())
    except (OSError, ValueError) as error:
        print("ppsspp-network-check: outcome=unknown %s" % error,
              file=sys.stderr)
        return 2
    print(summary)
    for number, what, line in violations:
        print("  %s line %d: %s" % (what, number, line[:200]),
              file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
