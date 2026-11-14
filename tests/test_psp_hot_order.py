"""Keep the committed PSP hot-function order (cmake/PspHotOrder.ld) in the
shape the link and docs/engineering/CODE_LAYOUT.md promise: two tiers, each
starting on an 8 KiB link boundary, no duplicate placements, a heat share on
every entry for tools/check_psp_hot_order.py, and no entry naming a function
that no source defines any more (ld silently ignores a pattern that matches
nothing, so a rename would quietly drop a hot function out of its fixed
place; the post-link check catches the rest, such as a moved object)."""
import importlib.util
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
ORDER = ROOT / "cmake/PspHotOrder.ld"
ENTRY = re.compile(r"^\s+\*(?:(?P<archive>[^:\s()]+\.a):)?(?P<member>[^\s()]+)"
                   r"\((?P<section>\.text|\.text\.(?P<base>[^\s().]+) \.text\.(?P=base)\.\*)\)"
                   r"\s*/\*\s*(?P<function>\S+)\s+(?P<share>[0-9.]+)%\s*\*/$")
ALIGN = "  . = ALIGN(8192);"


def is_align(line):
    return line.startswith(ALIGN)
CLONE = re.compile(r"(?:\.(?:constprop|isra|part|cold|lto_priv)(?:\.\d+)*)+$")


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "code_layout_order", ROOT / "tools/code_layout_order.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class HotOrderFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = ORDER.read_text(encoding="utf-8")
        body = cls.text[cls.text.index(".text : {\n") + len(".text : {\n"):]
        cls.lines = body[:body.rindex("\n}")].split("\n")

    def entries(self):
        tier = 0
        for line in self.lines:
            if is_align(line):
                tier += 1
                continue
            if line == "  __tilefinch_hot_order_end = .;":
                continue
            if line.strip().startswith("/*") or not line.strip():
                continue
            match = ENTRY.match(line)
            self.assertIsNotNone(match, "unexpected statement: %r" % line)
            yield tier, match

    def test_one_text_statement(self):
        self.assertEqual(self.text.count(".text : {"), 1)
        self.assertNotIn(".rodata", self.text)

    def test_tiers_start_on_way_boundaries(self):
        # tier 0, the boundary, tier 1, the boundary before the rest of
        # .text, then the end symbol the post-link check reads
        self.assertEqual(sum(1 for x in self.lines if is_align(x)), 2)
        self.assertTrue(is_align(self.lines[-2]))
        self.assertEqual(self.lines[-1], "  __tilefinch_hot_order_end = .;")
        tiers = [tier for tier, _ in self.entries()]
        self.assertTrue(tiers and tiers == sorted(tiers))
        self.assertEqual(set(tiers), {0, 1})

    def test_tier_owners(self):
        generator = load_generator()
        for tier, match in self.entries():
            archive, member = match.group("archive"), match.group("member")
            # the map's own form: archives absolute, the executable's
            # objects relative to the build directory
            path = ("/x/%s(%s)" % (archive, member) if archive
                    else "CMakeFiles/psp-browser-script.dir/src/%s" % member)
            self.assertEqual(int(generator.is_tilefinch(
                path, match.group("section"))), tier,
                             match.group(0))

    def test_no_duplicates(self):
        seen = set()
        for _, match in self.entries():
            key = (match.group("archive"), match.group("member"),
                   match.group("section"))
            self.assertNotIn(key, seen, match.group(0))
            seen.add(key)

    def test_placeable_entries_only(self):
        for _, match in self.entries():
            self.assertNotIn(".ltrans", match.group("member"))
            self.assertFalse(match.group("member").startswith("crt"))
            self.assertFalse(match.group("section").startswith(
                (".text.unlikely", ".text.startup")))

    def test_heat_shares(self):
        total = sum(float(m.group("share")) for _, m in self.entries())
        self.assertGreater(total, 90.0)
        self.assertLessEqual(total, 100.5)

    def test_entries_name_live_functions(self):
        # A function may be defined in a .c, an included .inc or .h, or
        # vendored code compiled into a Tilefinch object (stb, nanosvg).
        called = set()
        call = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
        for directory in ("src", "include", "psp-validation",
                          "third_party/quickjs"):
            for path in (ROOT / directory).rglob("*"):
                if path.suffix in (".c", ".h", ".inc") and path.is_file():
                    called.update(call.findall(path.read_text(
                        encoding="utf-8", errors="replace")))
        stale = []
        for tier, match in self.entries():
            archive = match.group("archive") or ""
            if not (tier == 1 or archive == "libqjs.a"):
                continue  # newlib, libgcc, the SDK and fetched libraries
            function = CLONE.sub("", match.group("function"))
            if function not in called:
                stale.append(match.group(0).strip())
        self.assertEqual(stale, [], "regenerate cmake/PspHotOrder.ld "
                         "(docs/engineering/CODE_LAYOUT.md)")


class GeneratorClassificationTests(unittest.TestCase):
    def test_section_patterns_match_every_clone(self):
        g = load_generator()
        self.assertEqual(g.section_patterns(".text"), ".text")
        self.assertEqual(g.section_patterns(".text.f.constprop.0.isra.0"),
                         ".text.f .text.f.*")
        self.assertEqual(g.pattern_for(".text.f.part.0", "/b/libx.a(f.c.obj)"),
                         "*libx.a:f.c.obj(.text.f .text.f.*)")

    def test_owner_and_placeable(self):
        g = load_generator()
        self.assertTrue(g.is_tilefinch("/b/libtilefinch_core.a(style_match.c.obj)"))
        self.assertTrue(g.is_tilefinch(
            "CMakeFiles/psp-browser-script.dir/src/psp_app/psp_app_page.c.obj"))
        self.assertTrue(g.is_tilefinch(
            "/b/CMakeFiles/psp-browser-script.dir/src/psp_log.c.obj"))
        self.assertFalse(g.is_tilefinch("/sdk/psp/lib/prxexports.o"))
        self.assertFalse(g.is_tilefinch(
            "/b/libtilefinch_core.a(image_decode.c.obj)",
            ".text.stbi__resample_row_hv_2"))
        self.assertFalse(g.is_tilefinch("/b/_deps/qjs/libqjs.a(quickjs.c.obj)"))
        self.assertFalse(g.is_tilefinch("/sdk/psp/lib/libc.a(libc_a-memcpy.o)"))
        self.assertFalse(g.placeable(".text.f", "/b/cc6zDGdX.ltrans0.ltrans.o"))
        self.assertFalse(g.placeable(".text.unlikely.f", "/b/libtilefinch_core.a(a.c.obj)"))
        self.assertFalse(g.placeable(".text", "/gcc/crtbegin.o"))
        self.assertTrue(g.placeable(".text.f", "/b/libtilefinch_core.a(a.c.obj)"))


class PlacementCheckTests(unittest.TestCase):
    """tools/check_psp_hot_order.py against a fake psp-nm."""

    def run_check(self, rows, minimum=0.95):
        import subprocess
        import sys
        import tempfile
        with tempfile.TemporaryDirectory(prefix="tf-hot-order-") as d:
            d = pathlib.Path(d)
            nm = d / "nm"
            nm.write_text("#!%s\nprint(%r)\n" % (sys.executable, rows))
            nm.chmod(0o755)
            order = d / "order.ld"
            order.write_text(
                ".text : {\n"
                "  *libqjs.a:quickjs.c.obj(.text.hot_a)  /* hot_a 60.0000% */\n"
                "  . = ALIGN(8192);\n"
                "  *libtilefinch_core.a:x.c.obj(.text.hot_b .text.hot_b.*)  /* hot_b 30.0000% */\n"
                "  *libtilefinch_core.a:x.c.obj(.text.gone .text.gone.*)  /* gone 10.0000% */\n"
                "  . = ALIGN(8192);\n  __tilefinch_hot_order_end = .;\n}\n")
            return subprocess.run(
                [sys.executable, str(ROOT / "tools/check_psp_hot_order.py"),
                 "--nm", str(nm), "--elf", "image", "--order", str(order),
                 "--min-placed", str(minimum)],
                capture_output=True, text=True, timeout=20)

    def test_placed_and_absent(self):
        result = self.run_check("0 64 T hot_a\n8192 32 t hot_b.isra.0\n"
                                "8224 8 t helper\n16384 A __tilefinch_hot_order_end\n"
                                "16384 16 T _start\n16400 8 T cold")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("100.0% of present heat placed", result.stdout)
        self.assertIn("in 1 entries absent", result.stdout)

    def test_any_clone_suffix_counts(self):
        result = self.run_check("0 64 T hot_a\n8192 32 t hot_b.constprop.0\n"
                                "16384 A __tilefinch_hot_order_end\n16384 16 T _start")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("100.0% of present heat placed", result.stdout)

    def test_moved_function_is_missed(self):
        result = self.run_check("0 64 T hot_a\n16384 A __tilefinch_hot_order_end\n"
                                "16384 16 T _start\n20000 32 t hot_b.constprop.0")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missed hot_b", result.stdout)

    def test_outside_block_is_missed(self):
        result = self.run_check("0 A __tilefinch_hot_order_end\n0 16 T _start\n"
                                "64 64 T hot_a\n8192 32 t hot_b.isra.0", minimum=0.5)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
