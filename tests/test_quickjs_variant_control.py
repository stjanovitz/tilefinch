"""A disabled getter control must remove the actual current fast path."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
patch = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "patches/bellard-quickjs-04be246-capture-getter-fastpath.patch"
original = (root / "third_party/quickjs/quickjs.c").read_bytes()
marker = b"b->byte_code_len == 2 &&\n               b->closure_var_count >= 1"
assert original.count(marker) == 1, "expected one active getter fast path"
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "quickjs.c"
    source.write_bytes(original)
    for direction in ("-R", "--forward"):
        subprocess.run(["patch", "--force", "--batch", "--silent", direction,
                        "-p1", "-i", str(patch.resolve())], cwd=directory, check=True)
        expected = 0 if direction == "-R" else 1
        assert source.read_bytes().count(marker) == expected, "control left a fast path active"
    assert source.read_bytes() == original, "patch round trip changed unrelated engine code"
