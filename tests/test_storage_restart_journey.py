"""Restart real host workers on one disposable volume; no network or account."""
from pathlib import Path
import resource
import signal
import struct
import subprocess
import sys
import tempfile
import zipfile


def run(*args, limit=None):
    def short_writes():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))
    subprocess.run(args, check=True, timeout=30, preexec_fn=short_writes if limit is not None else None)


def session_rename_crashes(root, session):
    # Each worker starts with a new session and has no cleanup opportunity
    # after the selected cut. This models process death, not loss of a
    # filesystem/device write cache.
    cuts = {
        "before-rotation": ("current", "older", "staged", "current"),
        "after-retiring-backup": ("current", None, "staged", "current"),
        "after-rotation": (None, "current", "staged", "current"),
        "after-install": ("staged", "current", None, "staged"),
    }
    for mode in ("native", "fat"):
        for cut, (primary_at_cut, backup_at_cut, temporary_at_cut, recovered) in cuts.items():
            if cut == "after-retiring-backup" and mode != "fat":
                continue
            path = root / f"session-{mode}-{cut}"
            backup = path.with_suffix(".bak")
            temporary = path.with_suffix(".tmp")

            def generation(action, name):
                run(session, "--generation", path, action, name, mode, "none")

            snapshots = {}
            for name in ("last", "next", "staged", "older", "current"):
                generation("save", name)
                snapshots[name] = path.read_bytes()

            interrupted = subprocess.run(
                (session, "--generation", path, "save", "staged", mode, cut),
                timeout=30, check=False,
            )
            assert interrupted.returncode == 86, (mode, cut, interrupted.returncode)
            for file, name in ((path, primary_at_cut), (backup, backup_at_cut),
                               (temporary, temporary_at_cut)):
                if name is None:
                    assert not file.exists(), (mode, cut, file)
                else:
                    assert file.read_bytes() == snapshots[name], (mode, cut, file)

            # Restore ignores an uninstalled .tmp. The next save must keep
            # the generation the fresh reader actually recovered, not a
            # stale backup or the interrupted staged data.
            generation("read", recovered)
            generation("save", "next")
            generation("read", "next")
            assert path.read_bytes() == snapshots["next"]
            assert backup.read_bytes() == snapshots[recovered]
            assert not temporary.exists()

            # Prove the retained bytes are usable, then save again after
            # recovery has discarded the corrupt primary. The last good
            # generation must not be rotated away in favor of corrupt bytes.
            path.write_bytes(snapshots["next"][:-1])
            generation("read", recovered)
            run(session, "--generation", path, "refuse", "last", mode, "none",
                limit=len(snapshots["last"]) - 1)
            assert backup.read_bytes() == snapshots[recovered]
            generation("read", recovered)
            generation("save", "last")
            generation("read", "last")
            assert path.read_bytes() == snapshots["last"]
            assert backup.read_bytes() == snapshots[recovered]


def journey(root, download, installer, session, boot_config):
    session_rename_crashes(root, session)
    path = root / "saved-session"
    def generation(action, name, limit=None):
        run(session, "--generation", path, action, name, limit=limit)
    generation("save", "old")
    original = path.read_bytes()
    for limit in (0, 36, 4096, len(original)-1):
        generation("refuse", "new", limit)
        generation("read", "old")
        assert path.read_bytes() == original
    generation("save", "new")
    generation("read", "new")
    generation_path = Path(str(path) + ".bak")
    assert generation_path.read_bytes() == original
    settings = root / "boot-overrides.cfg"
    custom = b"# keep my settings\r\nnetwork_profile=7\r\nfile_kb=1024\r\n"
    settings.write_bytes(custom)
    for limit in (0, len(custom)-1):
        run(boot_config, "--restart-profile", settings, "refuse", limit=limit)
        run(boot_config, "--restart-profile", settings, "read")
        assert settings.read_bytes() == custom

    library, trace, games = (root / n for n in ("library", "trace", "GAME"))
    for directory in (library, trace, games):
        directory.mkdir()
    unrelated = games / "Other"
    unrelated.mkdir()
    sentinel = unrelated / "save.dat"
    sentinel.write_bytes(b"unrelated saved game")
    package = root / "source.zip"
    payload = bytes(range(256)) * 512
    with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("Test/EBOOT.PBP", b"\0PBP" + struct.pack("<9I", 0, *([40]*8)))
        z.writestr("Test/data.bin", payload)
        for i in range(18):
            z.writestr(f"Test/levels/{i}.dat", bytes([i]))
    run(download, "--interrupt-package", library, trace, package)
    run(download, "--resume-package", library, trace, package)
    saved, = library.glob("*.zip")
    assert saved.read_bytes() == package.read_bytes()

    def install(step):
        run(installer, "--restart-step", step, library, games)
        assert sentinel.read_bytes() == b"unrelated saved game"
        assert saved.read_bytes() == package.read_bytes()

    install("interrupt-install")
    stage, = games.glob(".tilefinch-unpack-*")
    assert not (games / "Test").exists()
    stages = {p.relative_to(stage): p.read_bytes() for p in stage.rglob("*") if p.is_file()}
    assert stages
    install("cancel-install")
    assert not (games / "Test").exists()
    install("install")
    assert (games / "Test/data.bin").read_bytes() == payload
    install("refuse-existing")
    install("interrupt-remove")
    install("remove")
    assert not (games / "Test").exists()
    assert {p.relative_to(stage): p.read_bytes() for p in stage.rglob("*") if p.is_file()} == stages


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="tilefinch-storage-restart-") as temp:
        journey(Path(temp), *sys.argv[1:])
