#!/usr/bin/env python3
"""One-command Google/YouTube sign-in test in the host lab.

Run it yourself in a local terminal:

    scripts/try-signin.py            # YouTube sign-in (the PSP's journey)
    scripts/try-signin.py --google   # plain Google sign-in

It builds the host lab if needed, opens the sign-in page with the shipping
settings, asks for the email and password in hidden Mac dialogs, submits each
once, and saves the sign-in diagnostics report (counts and fixed classes
only, the same data as the PSP's Diagnostic QR) under
.private-investigations/signin-tests/. The email and password never reach a
file, a log, a command line or the saved report. Cookies stay in the lab
process and are gone when it exits.
"""

import argparse
import datetime
import getpass
import importlib.util
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time
import warnings


ROOT = Path(__file__).resolve().parents[1]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PRIVATE = _load("private_interactive_lab", ROOT / "scripts/run-private-interactive-lab.py")
QR = _load("decode_diagnostic_qr", ROOT / "tools/decode_diagnostic_qr.py")

SERVICE_URLS = {
    "youtube": "https://accounts.google.com/ServiceLogin?service=youtube"
               "&continue=https%3A%2F%2Fwww.youtube.com%2F",
    "google": "https://accounts.google.com/ServiceLogin",
}
YOUTUBE_FEED = "https://www.youtube.com/feed/recommended"
ACCOUNTS = "https://accounts.google.com"
DEFAULT_LAB = ROOT / "build-preset-release/psp-browser-interactive-lab"
RESULTS = ROOT / ".private-investigations/signin-tests"
SETTLE_SECONDS = 45
MENU = """
  b  press a button on the page (Continue, Not now, ...)
  v  view the page (form contents blanked; opens in Preview)
  w  wait longer (finish a phone prompt first, then press w)
  y  open the YouTube feed and check the sign-in
  r  save the diagnostics report again
  q  quit (cookies are discarded)"""


def build_lab(lab):
    if lab != DEFAULT_LAB:
        return lab.is_file()
    if not (ROOT / "build-preset-release").is_dir():
        print("No release build tree yet. Run once:  cmake --preset release")
        return False
    print("Building the host lab (only changed files)...", flush=True)
    result = subprocess.run(
        ["cmake", "--build", "build-preset-release", "--target",
         "psp-browser-interactive-lab", "-j8"], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(result.stdout.decode("utf-8", "replace")[-4000:])
        print("The build failed; nothing was opened.")
        return False
    return lab.is_file()


# Plain-language names for the report's fixed classes
# (.private-investigations/qr-tools/signin-report-v5.md).
CHAIN_CLASS = {1: "timed out", 2: "name lookup failed", 3: "could not connect",
               4: "TLS failed", 5: "HTTP/2 failed", 6: "transfer broke off",
               7: "was refused by a Tilefinch rule", 8: "was cancelled", 9: "failed"}
CHAIN_LIMIT = {1: "the request deadline", 2: "more than 8 redirects",
               3: "a redirect loop", 4: "no usable redirect address",
               5: "too many cookies", 6: "a request the network thread cannot carry",
               7: "a stalled transfer", 8: "a body over the limit",
               9: "a redirect address longer than Tilefinch accepts while "
                  "opening a page (8,191 bytes)",
               10: "a page address longer than Tilefinch accepts (2,047 bytes) "
                   "at the end of the redirects"}
TOKEN_KIND = {0: "missing", 1: "empty", 2: "js_disabled", 3: "a failure code"}


# Limits under which the failing hop is the one that sent a refused redirect
# (signin-report-v5.md); under any other, it is the request that failed.
REFUSED_REDIRECT_LIMITS = (2, 3, 4, 5, 9)


def failed_step(chain):
    """Name the request a chain ended on. Hop 0 is the form post; hop N is
    the request redirect N led to, which is the landing page when N is
    also the number of redirects followed."""
    hops, failed, limit = chain[1], chain[2], chain[4]
    if failed == 0:
        return "the form post itself"
    if limit in REFUSED_REDIRECT_LIMITS:
        return f"the redirect sent by step {failed} of the chain"
    if failed == hops:
        return f"the landing page (after {hops} redirects)"
    return f"the request after redirect {failed}"


def explain(report):
    """Turn the report's key numbers into sentences for the terminal."""
    fields = {}
    for line in report.splitlines():
        if line.startswith("post-chain="):
            fields["chain"] = [int(x) for x in line[11:].split(",") if x.isdigit()]
            continue
        for item in line.split():
            name, _, value = item.partition("=")
            if value.isdigit():
                fields[name] = int(value)
    messages = []
    chain = fields.get("chain", [])
    if fields.get("form-posts", 0) and len(chain) == 12 and chain[0]:
        messages.append(
            f"The sign-in step was sent, but {failed_step(chain)} "
            f"{CHAIN_CLASS.get(chain[0], 'failed')}"
            + (f": {CHAIN_LIMIT[chain[4]]}." if chain[4] in CHAIN_LIMIT else ".")
            + " Pressing the page's buttons again will not resend it.")
    kind = fields.get("token-kind")
    if fields.get("form-posts", 0) and kind in TOKEN_KIND:
        messages.append(f"Google's anti-bot check sent {TOKEN_KIND[kind]} instead of "
                        "a token; Google may ask for extra verification.")
    if fields.get("accounts-refusals", 0):
        messages.append("Google refused sign-in from this browser.")
    return messages


def landed_on_youtube(origin):
    """Whether the page's origin is a YouTube host: the sign-in chain ended
    with the hand-over to the native YouTube page."""
    host = origin.partition("://")[2]
    return (origin.startswith("https://")
            and (host == "youtube.com" or host.endswith(".youtube.com")))


def printable_labels(text):
    """The listing's labels that are safe to print and press: one per line,
    each printable (no terminal control characters), and unique."""
    labels = [label for label in text.split("\n") if label and label.isprintable()]
    return [label for label in labels if labels.count(label) == 1][:20]


def origin_class(origin):
    """What a saved report records of the page's origin: whether it is
    Google's account pages, YouTube or something else (a third-party
    identity provider is not named)."""
    if origin == "https://accounts.google.com":
        return "accounts"
    return "youtube" if landed_on_youtube(origin) else "other"


def account_input(prompt):
    """The email in a plain dialog (shown as typed), the password hidden."""
    if prompt != "Test account email" or sys.platform != "darwin":
        return PRIVATE.local_account_input(prompt)
    script = ('text returned of (display dialog "Google account email" '
              'default answer "" with title "Tilefinch sign-in test" '
              'buttons {"Cancel", "Continue"} default button "Continue")')
    result = subprocess.run(["/usr/bin/osascript", "-e", script],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            env=PRIVATE.private_environment(tempfile.gettempdir()))
    if result.returncode != 0:
        return None
    email = result.stdout.decode("utf-8").removesuffix("\n")
    print(f"Signing in as {email}", flush=True)
    return email


def commit():
    result = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return result.stdout.decode().strip() or "unknown"


class Run:
    def __init__(self, session, directory, service):
        self.session = session
        self.directory = Path(directory)
        self.service = service
        self.values = None
        self.phases = []
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.out = RESULTS / stamp
        self.saved = 0
        self.post_failed = False

    def send(self, command):
        self.values = self.session.send(command)
        return self.values

    def progress(self, phase):
        self.phases.append(phase)

    def settle(self, seconds=SETTLE_SECONDS):
        """Let redirects and the landing page finish; no input is sent."""
        print(f"Letting the page finish (up to {seconds} s)...", flush=True)
        deadline = time.monotonic() + seconds
        quiet = 0
        origin = self.values[7]
        while time.monotonic() < deadline and quiet < 6:
            values = self.send(b"pump 8 16\n")
            busy = values[6] > 0 or not values[1]
            changed = values[7] != origin
            origin = values[7]
            quiet = 0 if busy or changed else quiet + 1
            time.sleep(0.25)
        print(f"Page now at {self.values[7]}.")

    def save_report(self, label):
        report_file = self.directory / "signin-report.txt"
        report_file.unlink(missing_ok=True)
        try:
            ok = self.send(b"private-signin-report\n")[8]
            if not ok or not report_file.is_file():
                print("The diagnostics report could not be produced.")
                return
            report = report_file.read_bytes()
        finally:
            report_file.unlink(missing_ok=True)
        self.out.mkdir(parents=True, exist_ok=True)
        self.out.chmod(0o700)
        self.saved += 1
        named = QR.describe_post_chain(report)
        summary = (
            f"commit={commit()} service={self.service} label={label}\n"
            f"time={datetime.datetime.now().isoformat(timespec='seconds')}\n"
            f"origin={origin_class(self.values[7])}\n"
            f"steps={' '.join(self.phases) or 'none'}\n"
            + (f"post-chain named: {named}\n" if named else ""))
        target = self.out / f"report-{self.saved}-{label}.txt"
        target.write_bytes(summary.encode() + b"\n" + report)
        target.chmod(0o600)
        print(f"Saved diagnostics: {target.relative_to(ROOT)}")
        for line in report.decode("utf-8", "replace").splitlines():
            if line.startswith(("post-chain=", "form-posts=", "accounts-refusals=",
                                "session-cookie-present=")):
                print("  " + line[:160])
        if named:
            print("  " + named)
        messages = explain(report.decode("utf-8", "replace"))
        self.post_failed = any(m.startswith("The sign-in step was sent") for m in messages)
        for message in messages:
            print("=> " + message)

    def buttons(self):
        """Labels of the page's usable buttons, shown only in this terminal."""
        listing = self.directory / "private-buttons.txt"
        listing.unlink(missing_ok=True)
        try:
            if not self.send(b"private-buttons\n")[8] or not listing.is_file():
                return []
            text = listing.read_text("utf-8", "replace")
        finally:
            listing.unlink(missing_ok=True)
        return printable_labels(text)

    def press_button(self, auto=False):
        labels = self.buttons()
        if not labels:
            if not auto:
                print("No buttons found on this page. Try w (wait) or v (view).")
            return False
        print(f"Buttons on this page ({self.values[7]}):")
        for number, label in enumerate(labels, 1):
            print(f"  {number}  {label}")
        choice = input("Number to press (Enter for the menu): ").strip()
        if not choice.isdigit() or not 1 <= int(choice) <= len(labels):
            return False
        label = labels[int(choice) - 1]
        if not self.send(b"private-press " + label.encode("utf-8").hex().encode()
                         + b"\n")[8]:
            print(f"Could not select \"{label}\".")
            return False
        # Labels can name the account (chooser pages), so the saved report
        # records only that a button was pressed, never its label.
        self.progress("pressed-button")
        if not self.send(b"activate\n")[8]:
            print(f"\"{label}\" did not respond.")
            return False
        print(f"Pressed \"{label}\".")
        self.settle()
        self.save_report("after-button")
        return True

    def check_youtube(self):
        print("Opening the YouTube feed...", flush=True)
        values = self.send(b"private-open " + YOUTUBE_FEED.encode() + b"\n")
        if not values[8]:
            print("The YouTube feed could not be opened (not signed in, or the "
                  "account step is not finished).")
            return
        self.send(b"pump 192 16\n")
        print(f"YouTube feed opened; page at {self.values[7]}. Use v to look at it.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--google", action="store_true",
                        help="plain Google sign-in instead of YouTube's")
    parser.add_argument("--lab", type=Path, default=DEFAULT_LAB,
                        help="host lab executable (default: the release build)")
    parser.add_argument("--no-build", action="store_true",
                        help="use the existing lab without rebuilding it")
    args = parser.parse_args()
    if not sys.stdin.isatty() or not sys.stderr.isatty():
        parser.error("Run this yourself in a local terminal.")
    lab = args.lab.resolve()
    if not (lab.is_file() if args.no_build else build_lab(lab)):
        parser.error("No host lab to run.")
    service = "google" if args.google else "youtube"
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    print("One sign-in attempt: the email and then the password (hidden) are asked "
          "for in Mac dialogs and submitted once each. Nothing is retried automatically.")
    try:
        # A short owner-only directory, outside the repository and backups.
        with tempfile.TemporaryDirectory(prefix="tf-signin-", dir="/private/tmp") as directory:
            session = PRIVATE.PrivateLab(lab, directory, url=SERVICE_URLS[service])
            try:
                run = Run(session, directory, service)
                run.values = session.status()
                print(f"Opened {run.values[7]}; preparing the page...", flush=True)
                run.send(b"pump 192 16\n")
                if run.values[7] != ACCOUNTS:
                    print("The sign-in page did not open on accounts.google.com; "
                          "nothing was asked for.")
                    run.save_report("no-signin-page")
                    return 1
                try:
                    run.values = PRIVATE.guided_signin(
                        session, run.values, input_value=account_input,
                        progress=run.progress)
                except (ValueError, UnicodeError, getpass.GetPassWarning):
                    print("Entry cancelled or outside its limits; nothing was logged.")
                    run.send(b"status\n")
                run.settle()
                run.save_report("after-submit")
                # The chain ended on YouTube: open the feed at once, so the
                # report shows whether YouTube itself accepts the cookies
                # (server-verified, api-authenticated), not only that they
                # were set.
                if landed_on_youtube(run.values[7]):
                    run.check_youtube()
                    run.save_report("after-youtube")
                # A Google screen after the password (selfie, passkey,
                # recovery prompts) waits for a button press: offer them.
                while (run.values[7] == ACCOUNTS and not run.post_failed
                       and run.press_button(auto=True)):
                    pass
                while True:
                    print(MENU)
                    choice = input("> ").strip().lower()
                    if choice == "q":
                        break
                    if choice == "b":
                        run.press_button()
                    elif choice == "v":
                        run.values = PRIVATE.show_private_view(session, directory)
                    elif choice == "w":
                        run.settle()
                        run.save_report("after-wait")
                    elif choice == "y":
                        run.check_youtube()
                        run.save_report("after-youtube")
                    elif choice == "r":
                        run.save_report("manual")
            finally:
                session.close()
    except (RuntimeError, OSError):
        print("The lab stopped unexpectedly; no credentials were saved.", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("\nClosed; cookies discarded.")
    return 0


if __name__ == "__main__":
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        raise SystemExit(main())
