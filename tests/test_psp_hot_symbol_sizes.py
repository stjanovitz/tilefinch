"""Keep the real PSP hot-function gate authoritative across GCC clones, and
the frontend engine-access ratchet blind to nothing it claims to count."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class HotSymbolTests(unittest.TestCase):
    def run_gate(self, rows, limit):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake)
        root = pathlib.Path(__file__).resolve().parents[1]
        source = (root / "cmake/CheckPspHotSymbolSizes.cmake").read_text()
        start = source.index("function(hot_symbol_size symbol out)")
        end = source.index("# The code one kind of frame", start)
        with tempfile.TemporaryDirectory(prefix="tf-hot-symbol-") as directory:
            script = pathlib.Path(directory) / "gate.cmake"
            script.write_text(
                "set(nm_output [=[\n" + rows + "\n]=])\n"
                "set(PSP_ELF test-image)\n" + source[start:end]
                + f"\ncheck_hot_symbol(sample {limit})\n"
            )
            return subprocess.run(
                [cmake, "-P", str(script)], capture_output=True, text=True,
                timeout=10,
            )

    def test_base_symbol(self):
        result = self.run_gate("00000000 00000040 t sample", 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("64/64", result.stdout)

    def test_clone_without_base(self):
        result = self.run_gate("00000000 00000040 t sample.constprop.0", 64)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_all_variants_are_charged(self):
        rows = "00000000 00000040 t sample\n00000040 00000020 t sample.isra.2"
        self.assertEqual(self.run_gate(rows, 96).returncode, 0)
        result = self.run_gate(rows, 95)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("grew to 96 bytes", result.stderr)

    def test_missing_or_prefix_collision_is_refused(self):
        result = self.run_gate("00000000 00000040 t sample_other", 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("could not find sample", result.stderr)


ROOT = pathlib.Path(__file__).resolve().parents[1]
ACCESS = ROOT / "cmake/CheckPspFrontendEngineAccess.cmake"

# Each count (accessor, ->navigation->, navigation_current, alias) in a
# comment, a string or a validation-only unit stays uncounted; headers and
# local aliases count.
FIXTURE = {
    "src/psp_script_main.c": r"""
/* browser_engine_navigation(engine) and views->navigation->page in a
   comment, NavigationSession *old; navigation_current(x) */
static const char *url = "https://example.test/"; static int a = views->navigation->page;
static const char quote = '"'; static int b = views.navigation->loaded; // views->navigation->x
static void *c(void) { log("browser_engine_session( views->navigation->y"); return browser_engine_navigation(engine); }
""",
    "src/psp_app/psp_app_page.c": r"""
static int d(const BrowserEngine *engine)
{
    const NavigationSession *nav = browser_engine_navigation_view(engine);
    return nav->generation + navigation_current(nav)->index;
}
""",
    "src/psp_app/psp_app_internal.h": r"""
static inline int e(PspApp *app) { NavigationSession
    *alias = app->views->navigation;
    return alias->page.loaded + app->views->navigation->generation; }
""",
    "src/psp_app/psp_app_validation_loop.c": r"""
static int f(void) { NavigationSession *n = browser_engine_navigation(engine);
    return views->navigation->page + navigation_current(n)->index; }
""",
    "src/psp_app/psp_app_input_script.c": r"""
static int g(void) { return views->navigation->page; }
""",
}


class FrontendEngineAccessTests(unittest.TestCase):
    def run_access(self, root, *defines):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake)
        return subprocess.run(
            [cmake, f"-DTILEFINCH_SOURCE_DIR={root}", *defines, "-P", str(ACCESS)],
            capture_output=True, text=True, timeout=30)

    def copy_frontend(self, directory):
        root = pathlib.Path(directory)
        (root / "src").mkdir()
        shutil.copy(ROOT / "src/psp_script_main.c", root / "src")
        shutil.copytree(ROOT / "src/psp_app", root / "src/psp_app")
        return root

    def test_tree_is_within_the_ratchet(self):
        result = self.run_access(ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_counts_code_headers_and_aliases_only(self):
        with tempfile.TemporaryDirectory(prefix="tf-engine-access-") as directory:
            root = pathlib.Path(directory)
            for name, text in FIXTURE.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            result = self.run_access(
                root, "-DTILEFINCH_FRONTEND_ACCESS_EXPECT=2;3;1;2")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_read_through_a_new_alias_fails(self):
        with tempfile.TemporaryDirectory(prefix="tf-engine-access-") as directory:
            root = self.copy_frontend(directory)
            page = root / "src/psp_app/psp_app_page.c"
            page.write_text(page.read_text() + (
                "\nstatic unsigned psp_new_alias_read(PspApp *app)\n{\n"
                "    const NavigationSession *nav = app->views->navigation;\n"
                "    return nav->generation;\n}\n"))
            result = self.run_access(root)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("NavigationSession pointer declarations grew to", result.stderr)

    def test_a_shipping_read_in_a_header_fails(self):
        with tempfile.TemporaryDirectory(prefix="tf-engine-access-") as directory:
            root = self.copy_frontend(directory)
            header = root / "src/psp_app/psp_app_internal.h"
            header.write_text(header.read_text().replace(
                "#endif", "static inline bool psp_new_loaded(PspApp *app)\n"
                "{ return app->views->navigation->page.loaded; }\n#endif", 1))
            result = self.run_access(root)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("NavigationSession alias reads grew to", result.stderr)

    def test_unlisted_validation_unit_is_refused(self):
        with tempfile.TemporaryDirectory(prefix="tf-engine-access-") as directory:
            root = self.copy_frontend(directory)
            (root / "src/psp_app/psp_app_validation_new.c").write_text("\n")
            result = self.run_access(root)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("psp_app_validation_new.c looks validation-only", result.stderr)


if __name__ == "__main__":
    unittest.main()
