#!/usr/bin/env python3
"""First-install bundle pairing; update wire paths deliberately stay unchanged."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import argparse
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('startup', ROOT/'tools/prepare_psp_startup.py')
startup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(startup)


class StartupBundleTests(unittest.TestCase):
    def test_pair_and_repeat(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            module, pbp, header = [directory/name for name in ('browser.prx', 'browser.PBP', 'bundle.h')]
            body = b'\x7fELF\x01\x01\x01' + b'\0'*9 + b'\xa0\xff\x08\0' + b'payload'*100
            package = b'\0PBP\0\0\1\0' + struct.pack('<8I', *([40]*7), 40+len(body)) + body
            module.write_bytes(body)
            pbp.write_bytes(package)
            startup.prepare(pbp, module, header)
            digest = hashlib.sha256(body).digest()
            self.assertEqual(pbp.read_bytes(), package + b'TFBOOT01' + digest)
            self.assertIn('startup-'+digest.hex()+'.prx', header.read_text())
            prior = header.stat().st_mtime_ns
            startup.prepare(pbp, module, header)
            self.assertEqual(header.stat().st_mtime_ns, prior)
            self.assertEqual(pbp.stat().st_size, len(package)+40)
            # Packing the install slot must retain the old updater allowlist.
            tool_spec = importlib.util.spec_from_file_location('update', ROOT/'tools/tilefinch_update_tool.py')
            tool = importlib.util.module_from_spec(tool_spec)
            tool_spec.loader.exec_module(tool)
            named = directory/('startup-'+digest.hex()+'.prx')
            module.rename(named)
            pbp.rename(directory/'EBOOT.PBP')
            header.unlink()
            update = directory.parent/(directory.name+'.tfup')
            tool.command_pack(argparse.Namespace(directory=directory, output=update, component=False))
            self.assertEqual(struct.unpack_from('>H', update.read_bytes(), 10)[0], 1)
            update.unlink()
            named.write_bytes(body[:-1]+b'!')
            with self.assertRaisesRegex(ValueError, 'does not match'):
                tool.command_pack(argparse.Namespace(directory=directory, output=update, component=False))
            named.write_bytes(body)
            (directory/'EBOOT.PBP').rename(pbp)
            named.rename(module)
            # A stale, same-sized module cannot be packaged with a newer PBP.
            module.write_bytes(body[:-1]+b'!')
            with self.assertRaisesRegex(ValueError, 'differ'):
                startup.prepare(pbp, module, header)
            module.write_bytes(body)
            pbp.write_bytes(package+b'unexpected')
            with self.assertRaisesRegex(ValueError, 'auxiliary'):
                startup.prepare(pbp, module, header)
            pbp.write_bytes(package[:8] + struct.pack('<8I', *([40]*6), 39, 40+len(body)) + body)
            with self.assertRaisesRegex(ValueError, 'offsets'):
                startup.prepare(pbp, module, header)

    def test_shortcut_is_fresh_only_and_recovery_bypasses_it(self):
        source = (ROOT/'src/update_launcher_psp.c').read_text()
        self.assertIn('allow_module && fresh_install && slot == TILEFINCH_UPDATE_SLOT_A', source)
        self.assertIn('!recovery_button && action != TILEFINCH_UPDATE_BOOT_RECOVERY', source)
        self.assertIn('fresh_install = !tilefinch_update_journal_load', source)
        self.assertIn('psp_boot_cache_prepare(eboot, cache, temporary', source)
        branch = source[source.index('if (allow_module && fresh_install'):source.index('char cache[TILEFINCH_INSTALL_PATH_LIMIT]')]
        self.assertIn('goto ordinary_startup;', branch)
        self.assertLess(branch.index('psp_boot_module_start('), branch.index('remove(bundled)'))
        self.assertLess(branch.index('remove(bundled)'), branch.index('goto ordinary_startup;'))
        package = (ROOT/'src/update_package.c').read_text()
        self.assertNotIn('startup-', package)
        self.assertIn('"EBOOT.PBP", "roots.pem", "boot-defaults.cfg"', package)

    def test_approved_download_reclaims_bundled_copies(self):
        source = (ROOT/'src/psp_update_session.c').read_text()
        branch = source[source.index('case TILEFINCH_UPDATE_CLIENT_AVAILABLE:',
                                     source.index('PspUpdatePrimaryResult psp_update_session_primary')):]
        branch = branch[:branch.index('break;')]
        self.assertIn('slot < 2u', branch)
        self.assertLess(branch.index('psp_boot_prepackaged_discard('),
                        branch.index('tilefinch_update_client_begin_download('))


if __name__ == '__main__':
    unittest.main()
