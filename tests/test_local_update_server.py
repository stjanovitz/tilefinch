#!/usr/bin/env python3

from __future__ import annotations

import http.client
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "tools" / "local_update_server.py"


class LocalUpdateServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("openssl unavailable")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.metadata = b"signed metadata fixture"
        cls.package = bytes(range(256)) * 1024
        (cls.directory / "tilefinch-update-v1.tfum").write_bytes(cls.metadata)
        (cls.directory / "tilefinch-update-v1.tfup").write_bytes(cls.package)
        cls.certificate = cls.directory / "certificate.pem"
        cls.private_key = cls.directory / "private-key.pem"
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", str(cls.private_key), "-out", str(cls.certificate),
             "-days", "1", "-subj", "/CN=127.0.0.1",
             "-addext", "subjectAltName=IP:127.0.0.1"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.tls = ssl.create_default_context(cafile=str(cls.certificate))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def start_server(self, fault: str = "normal") -> tuple[subprocess.Popen, int]:
        port_file = self.directory / f"port-{fault}-{time.time_ns()}"
        process = subprocess.Popen(
            [sys.executable, str(SERVER), "--directory", str(self.directory),
             "--certificate", str(self.certificate), "--private-key",
             str(self.private_key), "--port-file", str(port_file),
             "--fault", fault], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        # The full release gate starts this beside seven CPU-heavy tests.
        # Allow startup scheduling jitter without weakening request timeouts.
        for _ in range(500):
            if port_file.exists():
                return process, int(port_file.read_text(encoding="ascii"))
            if process.poll() is not None:
                stdout, stderr = process.communicate()
                self.fail(f"server exited: {stdout}\n{stderr}")
            time.sleep(0.01)
        process.terminate()
        try:
            stdout, stderr = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate(timeout=2)
        self.fail(f"server did not publish its port: {stdout}\n{stderr}")

    def stop_server(self, process: subprocess.Popen) -> None:
        process.terminate()
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=2)

    def test_exact_artifacts_and_range(self) -> None:
        process, port = self.start_server()
        try:
            base = f"https://127.0.0.1:{port}"
            with urllib.request.urlopen(
                    base + "/tilefinch-update-v1.tfum",
                    context=self.tls, timeout=2) as response:
                self.assertEqual(response.read(), self.metadata)
            request = urllib.request.Request(
                base + "/tilefinch-update-v1.tfup",
                headers={"Range": "bytes=100-199"})
            with urllib.request.urlopen(request, context=self.tls,
                                        timeout=2) as response:
                self.assertEqual(response.status, 206)
                self.assertEqual(response.read(), self.package[100:200])
            with self.assertRaises(urllib.error.HTTPError) as missing:
                urllib.request.urlopen(base + "/../private-key.pem",
                                       context=self.tls, timeout=2)
            self.assertEqual(missing.exception.code, 404)
        finally:
            self.stop_server(process)

    def test_corrupt_metadata_is_deterministic(self) -> None:
        process, port = self.start_server("corrupt-metadata")
        try:
            with urllib.request.urlopen(
                    f"https://127.0.0.1:{port}/tilefinch-update-v1.tfum",
                    context=self.tls, timeout=2) as response:
                corrupted = response.read()
            self.assertEqual(corrupted[:-1], self.metadata[:-1])
            self.assertNotEqual(corrupted[-1], self.metadata[-1])
        finally:
            self.stop_server(process)

    def test_drop_package_once_closes_short(self) -> None:
        process, port = self.start_server("drop-package-once")
        try:
            connection = http.client.HTTPSConnection(
                "127.0.0.1", port, context=self.tls, timeout=2)
            connection.request("GET", "/tilefinch-update-v1.tfup")
            response = connection.getresponse()
            with self.assertRaises(http.client.IncompleteRead):
                response.read()
            connection.close()
            with urllib.request.urlopen(
                    f"https://127.0.0.1:{port}/tilefinch-update-v1.tfup",
                    context=self.tls, timeout=2) as response:
                self.assertEqual(response.read(), self.package)
        finally:
            self.stop_server(process)


if __name__ == "__main__":
    unittest.main()
