#!/usr/bin/env python3
"""Local, masked input for the host lab. Never use this for a main account."""

import argparse
import getpass
import os
from pathlib import Path
import resource
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import warnings
import zlib
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
MAX_SECRET_BYTES = 1023
COMMANDS = {
    "status", "up", "down", "page-up", "page-down", "top", "bottom",
    "scroll-by", "focus-next", "focus-prev", "focus-up", "focus-down",
    "focus-left", "focus-right", "focus-id", "tap", "click", "activate",
    "cross", "back", "forward", "reload", "tick", "pump", "drain", "idle",
    "heavy-answer",
}
HELP = """Commands: signin (guided Google test-account sign-in; no manual focus),
view (explicit temporary screenshot), secret (replace focused
field using masked input), focus-next / focus-prev, focus-id ID, click SELECTOR,
tap X Y (480x272 pixels), activate, up / down, page-up / page-down, back,
pump 32 16 (advance normal browser work), tick 32 16 (runtime only),
drain 128 16, status, help, quit.
Use secret for the account name, password, and verification codes. Never type
credentials as console commands. Ordinary lab logs and automatic captures are
disabled. Cookies stay in this process's memory and are discarded on exit.
Explicit view images may contain account details; they live in a private
temporary folder and are deleted on normal exit. No session transfer is used.
"""


def private_environment(directory):
    # Do not inherit trace, disk-cache, TLS-key-log, or profiler settings.
    environment = {key: os.environ[key] for key in ("PATH", "LANG", "LC_ALL")
                   if key in os.environ}
    environment.update(HOME=str(directory), TMPDIR=str(directory))
    return environment


def secret_command(value):
    data = value.encode("utf-8", errors="strict")
    if len(data) > MAX_SECRET_BYTES or any(c in data for c in (0, 10, 13)):
        raise ValueError("Input must be at most 1023 UTF-8 bytes, without line breaks or NUL.")
    return b"private-text " + data.hex().encode("ascii") + b"\n"


class PrivateLab:
    def __init__(self, lab, directory, *, url=None, fixture=None):
        self.directory = Path(directory)
        self.buffer = bytearray()
        self.sequence = 0
        read_fd, write_fd = os.pipe()
        self.status_fd = read_fd
        args = [str(lab), "--url" if url else "--fixture", str(url or fixture),
                "--psp-profile", "realistic", "--fetch-scripts",
                "--commands", "/dev/stdin", "--no-loop-capture",
                "--output", "/dev/null", "--loop-output-dir", str(directory),
                "--private-console-fd", str(write_fd)]
        # Live network replies and author timers must share real time. A fast
        # virtual clock can expire initialization before its response arrives.
        # Keep owned local fixtures deterministic and fast.
        if url is not None:
            args.append("--pace-real-time")
        try:
            self.process = subprocess.Popen(
                args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, pass_fds=(write_fd,),
                cwd=directory, env=private_environment(directory))
        except BaseException:
            os.close(read_fd)
            raise
        finally:
            os.close(write_fd)

    def status(self, timeout=120):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Page work did not finish within 120 seconds; no diagnostics were saved.")
            if not select.select([self.status_fd], [], [], remaining)[0]:
                continue
            chunk = os.read(self.status_fd, 768)
            if not chunk:
                raise RuntimeError("The host lab stopped; no diagnostics were saved.")
            self.buffer.extend(chunk)
            if len(self.buffer) > 768:
                raise RuntimeError("Invalid private status record.")
        line, _, remaining = self.buffer.partition(b"\n")
        self.buffer = bytearray(remaining)
        fields = line.split()
        if (len(fields) != 18 or fields[0] != b"S"
                or not all(x.isdigit() for x in fields[1:8])
                or fields[9] not in (b"0", b"1")
                or not all(x.isdigit() for x in fields[10:])):
            raise RuntimeError("Invalid private status record.")
        try:
            origin = fields[8].decode("ascii")
            parsed = urlsplit(origin)
        except (UnicodeError, ValueError):
            raise RuntimeError("Invalid private status origin.") from None
        if origin != "unavailable" and (parsed.scheme not in ("http", "https")
                or not parsed.hostname or parsed.path or parsed.query or parsed.fragment
                or parsed.username or parsed.password):
            raise RuntimeError("Invalid private status origin.")
        values = [int(x) for x in fields[1:8]] + [origin, int(fields[9])]
        values.extend(int(x) for x in fields[10:])
        if values[0] != self.sequence:
            raise RuntimeError("Private console lost command synchronization.")
        self.sequence += 1
        return values

    def send(self, command):
        self.process.stdin.write(command)
        self.process.stdin.flush()
        return self.status()

    def close(self):
        try:
            if self.process.poll() is None:
                try:
                    self.process.stdin.write(b"quit\n")
                    self.process.stdin.flush()
                    self.process.wait(timeout=10)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait()
        finally:
            try:
                self.process.stdin.close()
            except BrokenPipeError:
                pass  # The child can close its input while processing quit.
            finally:
                os.close(self.status_fd)


def show_status(values):
    _, loaded, editable, kind, index, controls, pending, origin, succeeded = values[:9]
    if not succeeded:
        print("That page action was not accepted. The session is still open; no value was logged.")
    print(f"Top-level origin: {origin} (check it before entering a value).")
    print(f"Page {'loaded' if loaded else 'not loaded'}; focused field editable: "
          f"{'yes' if editable else 'no'}; focus {kind}:{index}; "
          f"{controls} controls; {pending} pending tasks.")


def local_account_input(prompt):
    """Use a native Mac dialog; secrets never enter command arguments."""
    if sys.platform == "darwin":
        # The script is constant, with no user-controlled interpolation. The
        # returned value travels only through a pipe, never a file or argv.
        displayed_prompt = ("Enter the test account PASSWORD (not its email or username)"
                            if prompt == "Test account password" else prompt)
        script = ('text returned of (display dialog "' + displayed_prompt
                  + '" default answer "" with hidden answer '
                    'with title "Tilefinch — dedicated test account" '
                    'buttons {"Cancel", "Continue"} default button "Continue")')
        result = subprocess.run(["/usr/bin/osascript", "-e", script],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                env=private_environment(tempfile.gettempdir()))
        if result.returncode != 0:
            return None
        return result.stdout.decode("utf-8").removesuffix("\n")
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        return getpass.getpass(prompt + ": ")


def private_view_png(source, target):
    """Convert bounded 480-wide RGB diagnostics without optional packages."""
    with Path(source).open("rb") as stream:
        magic = stream.readline(32)
        dimensions = stream.readline(32).split()
        maximum = stream.readline(32)
        if (magic != b"P6\n" or maximum != b"255\n" or len(dimensions) != 2
                or dimensions[0] != b"480" or not dimensions[1].isdigit()):
            raise ValueError("Invalid private page frame.")
        height = int(dimensions[1])
        if not 272 <= height <= 4352:
            raise ValueError("Invalid private page frame.")
        pixels_size = 480 * height * 3
        raw = stream.read(pixels_size + 1)
    if len(raw) != pixels_size:
        raise ValueError("Invalid private page frame.")
    pixels = memoryview(raw)
    rows = b"".join(b"\0" + pixels[y * 1440:(y + 1) * 1440].tobytes()
                    for y in range(height))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body)))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", 480, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    Path(target).write_bytes(png)
    Path(target).chmod(0o600)


def create_private_snapshot(session, directory):
    """Only already-redacted pixels/text may become diagnostic artifacts."""
    image = Path(directory) / "redacted.ppm"
    preview = Path(directory) / "redacted.png"
    for path in (image, preview, Path(str(image) + ".txt")):
        path.unlink(missing_ok=True)
    values = session.send(f"private-snapshot {image}\n".encode())
    if values[8]:
        private_view_png(image, preview)
    return values


def show_private_view(session, directory):
    """Show a bounded full-page view with form contents removed natively."""
    values = create_private_snapshot(session, directory)
    if not values[8]:
        print("The page image could not be rendered; the session is still open.")
        return values
    try:
        # Preview reliably opens PNG; do not depend on its PPM support or the
        # user's default file association. Both images stay in the private dir.
        preview = Path(directory) / "redacted.png"
        opener = shutil.which("open" if sys.platform == "darwin" else "xdg-open")
        if opener is None:
            print(f"No image viewer found. Private page image: {preview}")
            return values
        args = ([opener, "-a", "Preview", str(preview)] if sys.platform == "darwin"
                else [opener, str(preview)])
        result = subprocess.run(args, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=10,
                                env=private_environment(directory))
        if result.returncode != 0:
            print("The image viewer could not open. Type view to retry; the session is still open.")
        else:
            print("Current page opened locally. This is a snapshot; view refreshes it. "
                  "Private images are removed when this console exits.")
    except (OSError, ValueError, subprocess.TimeoutExpired):
        print("The page view could not open. Type view to retry; the session is still open.")
    return values


def wait_for_account_field(session, kind, *, timeout=30, cancelled=lambda: False,
                           on_invalid=None):
    deadline = time.monotonic() + timeout
    # Both limits apply: neither a fast nor a stalled page can loop forever.
    for _ in range(64):
        if cancelled() or time.monotonic() >= deadline:
            return None
        values = session.send(f"private-focus {kind}\n".encode())
        if values[7] != "https://accounts.google.com":
            return None
        if values[8] and values[2]:
            return values
        if on_invalid is not None:
            feedback = session.send(b"private-feedback\n")
            if feedback[7] != "https://accounts.google.com":
                return None
            if feedback[8]:
                on_invalid()
                return None
        if time.monotonic() >= deadline:
            break
        session.send(b"pump 8 16\n")
        time.sleep(0.5)
    return None


def guided_signin(session, values, *, input_value=local_account_input,
                  progress=lambda phase: None, before_password=lambda: None,
                  cancelled=lambda: False):
    """Submit each credential once, only to the checked identity origin.

    This is a local test driver, not a browser login implementation. It never
    retries a submission or handles verification on the user's behalf.
    """
    if values[7] != "https://accounts.google.com":
        progress("unexpected_origin")
        print("Guided sign-in requires the Google accounts HTTPS page. No values requested.")
        return values
    progress("waiting_username")
    if wait_for_account_field(session, "username", cancelled=cancelled) is None:
        progress("username_unavailable")
        print("The account field is not ready or is ambiguous. Use view to inspect it.")
        return session.send(b"status\n")
    username = password = payload = None
    try:
        username = input_value("Test account email")
        if username is None:
            return session.send(b"status\n")
        if username == "":
            raise ValueError("Empty account input.")
        # Validate before asking for the other value; preserve spaces exactly.
        secret_command(username)
        password = input_value("Test account password")
        if password is None:
            return session.send(b"status\n")
        if password == "":
            raise ValueError("Empty account input.")
        secret_command(password)
        if username == password:
            progress("account_entries_match")
            print("The email and password entries were identical. Nothing submitted; "
                  "enter the password in the second dialog.")
            return session.send(b"status\n")
        # Dialogs can remain open for minutes. Recheck origin and focus before
        # delivering either value, never trusting a stale field index.
        print("Preparing account entry…", flush=True)
        values = wait_for_account_field(session, "username", cancelled=cancelled)
        if values is None:
            progress("username_unavailable")
            print("The account page changed. Nothing submitted.")
            return session.send(b"status\n")
        payload = secret_command(username)
        progress("entering_username")
        values = session.send(payload)
        username = payload = None
        if not values[8] or values[7] != "https://accounts.google.com":
            progress("username_entry_refused")
            print("The page refused account entry. Use view; do not re-enter credentials yet.")
            return values
        print("Account entered. Waiting for the password page (up to 30 seconds)…", flush=True)
        progress("submitting_username")
        values = session.send(b"private-focus-action identifierNext\n")
        if values[7] != "https://accounts.google.com" or not values[8]:
            progress("username_submit_refused")
            return values
        values = session.send(b"activate\n")
        progress("waiting_password")
        rejected = []
        if not values[8] or wait_for_account_field(
                session, "password", cancelled=cancelled,
                on_invalid=lambda: rejected.append(True)) is None:
            progress("username_rejected" if rejected else "password_unavailable")
            print("Paused: the page marked the account field invalid; its feedback is in view."
                  if rejected else
                  "Paused: the password page did not appear. Use view for any error or verification.")
            return session.send(b"status\n")
        print("Password page found. Submitting once…", flush=True)
        if cancelled():
            return session.send(b"status\n")
        # Latch before filling: an author input handler might itself submit.
        before_password()
        progress("entering_password")
        payload = secret_command(password)
        values = session.send(payload)
        password = payload = None
        if not values[8] or values[7] != "https://accounts.google.com":
            progress("password_entry_refused")
            print("The page refused password entry. Use view to inspect the page.")
            return values
        progress("submitting_password")
        values = session.send(b"private-focus-action passwordNext\n")
        if values[7] != "https://accounts.google.com" or not values[8]:
            progress("password_submit_refused")
            print("Paused: the password submit control was not available. No automatic retry.")
            return values
        values = session.send(b"activate\n")
        if not values[8]:
            progress("password_submit_refused")
            return values
        print("Waiting for Google's response…", flush=True)
        progress("waiting_result")
        for _ in range(16):
            if cancelled():
                break
            values = session.send(b"pump 8 16\n")
            time.sleep(0.1)
        if values[7] == "https://accounts.google.com":
            # Only an invalid-field bit crosses the private channel. Reveal
            # associated feedback locally, never return its text or value.
            field = session.send(b"private-focus password\n")
            if field[8]:
                values = session.send(b"private-feedback\n")
                if values[8]:
                    progress("password_rejected")
                    print("Paused: the password field was marked invalid. "
                          "Its feedback is shown locally; no automatic retry.")
                    return values
        print("Sign-in submitted once and page work advanced. Use view to check the result. "
              "Any verification must be completed locally; keep this session open.")
        progress("result_needs_inspection")
        return values
    finally:
        username = password = payload = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path,
                        default=ROOT / "build-preset-release/psp-browser-interactive-lab")
    parser.add_argument("--url", required=True, help="Public HTTPS sign-in entry URL; no credentials or tokens")
    parser.add_argument("--signin", action="store_true",
                        help="Open local account dialogs and drive the sign-in steps")
    args = parser.parse_args()
    # getpass otherwise warns and falls back to echoed input. Refuse that path.
    if not sys.stdin.isatty() or not sys.stderr.isatty():
        parser.error("Run locally in an interactive terminal; piped credential input is refused.")
    parsed = urlsplit(args.url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        parser.error("Use a public HTTPS entry URL without credentials, query parameters, or fragment.")
    lab = args.lab.resolve()
    if not lab.is_file():
        parser.error("Build psp-browser-interactive-lab first.")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    print(HELP)
    print("This is experimental browser code, not a hardened credential vault. Use only the dedicated test account.")
    try:
        with tempfile.TemporaryDirectory(prefix="tilefinch-private-console-") as directory:
            session = PrivateLab(lab, directory, url=args.url)
            try:
                values = session.status()
                show_status(values)
                if args.signin:
                    try:
                        values = guided_signin(session, values)
                    except (ValueError, UnicodeError, getpass.GetPassWarning):
                        print("Local entry unavailable or outside its limits; no value was logged.")
                        values = session.send(b"status\n")
                    values = show_private_view(session, directory)
                    show_status(values)
                while True:
                    command = input("private-lab> ").strip()
                    if command in ("quit", "exit"):
                        break
                    if command in ("", "help"):
                        print(HELP)
                        continue
                    if command == "signin":
                        try:
                            values = guided_signin(session, values)
                        except (ValueError, UnicodeError, getpass.GetPassWarning):
                            print("Local entry unavailable or outside its limits; no value was logged.")
                            values = session.send(b"status\n")
                        values = show_private_view(session, directory)
                    elif command == "secret":
                        if not values[2] or not values[7].startswith("https://"):
                            print("Focus an editable field on the expected HTTPS page first.")
                            continue
                        # Never include the input in errors, status, argv, or files.
                        try:
                            with warnings.catch_warnings():
                                warnings.simplefilter("error", getpass.GetPassWarning)
                                value = getpass.getpass("Field value (hidden): ")
                        except getpass.GetPassWarning:
                            print("Masked terminal entry unavailable; input refused.")
                            continue
                        try:
                            payload = secret_command(value)
                        except (ValueError, UnicodeError):
                            print("Input refused: length or encoding limit.")
                            continue
                        finally:
                            value = None
                        try:
                            values = session.send(payload)
                        finally:
                            payload = None
                    elif command == "view":
                        values = show_private_view(session, directory)
                    elif (command.split()[0] in COMMANDS and len(command.encode()) < 1024
                          and not any(c in command for c in "\r\n\0")):
                        values = session.send(command.encode() + b"\n")
                    else:
                        print("Command refused. Use help; enter credentials only with secret.")
                        continue
                    show_status(values)
            finally:
                session.close()
    except (RuntimeError, OSError):
        print("The private lab could not complete this operation. No diagnostic output was retained.", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("\nPrivate lab closed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
