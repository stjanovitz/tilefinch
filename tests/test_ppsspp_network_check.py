#!/usr/bin/env python3
"""tools/check_ppsspp_network.py: an offline PPSSPP run may reach loopback
only. The lines below follow PPSSPP 1.20's -d syscall log; the addresses are
documentation ranges and the names are reserved test names."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/check_ppsspp_network.py"
spec = importlib.util.spec_from_file_location("ppsspp_network", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

PREFIX = "12:00:000              "
LOADER = (PREFIX + "D[LOADER]: HLE/sceKernelModule.cpp:310 Importing "
          "sceNetInetConnect : 08dc1e44")
LOOPBACK_CONNECT = (
    PREFIX + "D[SCENET]: HLE/sceNetInet.cpp:718 -1=sceNetInetConnect(1, "
    "09e799c8, 16): errno = (unknown!) Address = 127.0.0.1, Port = 55187")
REMOTE_CONNECT = (
    PREFIX + "D[SCENET]: HLE/sceNetInet.cpp:718 -1=sceNetInetConnect(2, "
    "09e844c8, 16): errno = (unknown!) Address = 203.0.xx.7, Port = 443")
ERRNO_NOTE = (PREFIX + "D[SCENET]: HLE/sceNetInet.cpp:51 errno set to "
              "(unknown!) in sceNetInetConnect (host: 36)")
LOOKUP = (PREFIX + "I[SCENET]: HLE/sceNet.cpp:508 Direct lookup of "
          "'provider.example' from '192.0.2.53' succeeded: 203.0.113.7")
RESOLVER = (PREFIX + "I[SCENET]: HLE/sceNetResolver.cpp:117 "
            "NetResolver_StartNtoA - Hostname: provider.example => IPv4: "
            "203.0.113.7")
APP_LINE = "tilefinch-preconnect: event=started host=https://provider.example"


def run_check(*lines):
    return module.check("\n".join(lines) + "\n")


class PpssppNetworkCheckTests(unittest.TestCase):
    def test_loopback_origin_passes(self):
        summary, violations = run_check(LOADER, LOOPBACK_CONNECT, ERRNO_NOTE,
                                        LOOPBACK_CONNECT, APP_LINE)
        self.assertEqual(violations, [])
        self.assertEqual(summary, "ppsspp-network-check: outcome=pass "
                         "loopback=2 outbound=0")

    def test_preconnect_to_a_provider_fails(self):
        # The shape of the unapproved HOME preconnect: a resolver lookup,
        # then a TCP connect to port 443 on the answer.
        summary, violations = run_check(LOADER, LOOPBACK_CONNECT, LOOKUP,
                                        RESOLVER, REMOTE_CONNECT)
        self.assertIn("outcome=fail", summary)
        self.assertEqual([what for _, what, _ in violations], [
            "name lookup provider.example",
            "name lookup provider.example",
            "outbound 203.0.xx.7:443",
        ])
        self.assertEqual(violations[2][0], 5)

    def test_datagram_to_a_remote_peer_fails(self):
        sendto = REMOTE_CONNECT.replace("sceNetInetConnect", "sceNetInetSendto")
        _, violations = run_check(LOADER, sendto)
        self.assertEqual(len(violations), 1)

    def test_socket_call_without_a_peer_is_not_assumed_local(self):
        unparsed = (PREFIX + "D[SCENET]: HLE/sceNetInet.cpp:700 "
                    "-1=sceNetInetConnect(3, 00000000, 16): invalid")
        _, violations = run_check(LOADER, unparsed)
        self.assertEqual(violations[0][1], "socket call without a peer address")

    def test_loopback_names_and_ranges(self):
        for host in ("127.0.0.1", "127.12.0.9", "localhost", "::1"):
            self.assertTrue(module.is_loopback(host), host)
        for host in ("128.0.0.1", "127.0.0", "127.0.xx.1", "10.0.0.1",
                     "provider.example", "localhost.example"):
            self.assertFalse(module.is_loopback(host), host)
        lookup = LOOKUP.replace("'provider.example'", "'localhost'")
        self.assertEqual(run_check(LOADER, lookup)[1], [])

    def test_default_level_log_cannot_pass(self):
        # Without -d PPSSPP logs no socket syscalls: silence is not evidence.
        with self.assertRaises(ValueError):
            run_check(PREFIX + "N[BOOT]: Core/System.cpp:634 PPSSPP 1.20.4",
                      APP_LINE)

    def test_command_line_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            cases = ((0, (LOADER, LOOPBACK_CONNECT)),
                     (1, (LOADER, REMOTE_CONNECT)),
                     (2, (APP_LINE,)))
            for expected, lines in cases:
                path = Path(directory) / ("%d.log" % expected)
                path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, str(TOOL), str(path)],
                    capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
