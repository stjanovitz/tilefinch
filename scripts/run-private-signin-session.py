#!/usr/bin/env python3
"""RAM-only account keeper and credential-free local sign-in test controls."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import resource
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import warnings


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "private_interactive_lab", ROOT / "scripts/run-private-interactive-lab.py")
PRIVATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRIVATE)
ENTRY_URL = "https://accounts.google.com/ServiceLogin"
SERVICE_URLS = {"google": ENTRY_URL,
                "youtube": "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F"}
YOUTUBE_PAGES = {"youtube-home": "https://www.youtube.com/feed/recommended",
                 "youtube-subscriptions": "https://www.youtube.com/feed/subscriptions",
                 "youtube-test-video": "https://www.youtube.com/watch?v=jNQXAC9IVRw&tilefinch_view=details"}
YOUTUBE_UI_ACTIONS = {"youtube-like": b"yt-like-action", "youtube-unlike": b"yt-unlike-action",
                      "youtube-signout": b"yt-signout-action", "youtube-account": b"yt-account-action",
                      "youtube-refresh": b"yt-feed-refresh"}
ACTIONS = {"status", "attempt", "restart", "advance", "view", "view-below", "view-above", "inspect", "stop", *YOUTUBE_UI_ACTIONS, *YOUTUBE_PAGES}
MAX_REQUEST = 64
MAX_RESPONSE = 4096
MAX_PASSWORD_ATTEMPTS = 3
MAX_CLASSIFIER_BYTES = 16384
COUNTERS = ("network_requests", "network_failures", "network_completed",
            "network_timeouts", "callback_errors", "rejections_created",
            "event_handlers_invoked", "last_event_cancelled")
PHASES = {"ready", "queued", "stopping", "starting_browser", "preparing_page", "browser_ready",
          "account_entries_match",
          "password_attempt_limit", "awaiting_local_retry_confirmation",
          "retry_not_approved", "advancing", "page_advanced",
          "browser_operation_failed", "unexpected_origin", "waiting_username",
          "username_unavailable", "entering_username", "username_entry_refused",
          "submitting_username", "username_submit_refused", "waiting_password", "password_unavailable", "username_rejected",
          "entering_password", "password_entry_refused", "submitting_password",
          "password_submit_refused", "password_rejected", "waiting_result", "result_needs_inspection",
          "opening_youtube", "youtube_ready", "youtube_checked", "youtube_unavailable"}


class ObservedSession:
    def __init__(self, session, remember):
        self.session = session
        self.remember = remember

    def send(self, command):
        return self.remember(self.session.send(command))


def install_local_classifier(source, directory):
    """Copy only explicitly requested, bounded, owner-only diagnostic code."""
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or info.st_mode & 0o077 or info.st_nlink != 1
                or not 0 < info.st_size <= MAX_CLASSIFIER_BYTES):
            raise ValueError("Unsafe local classifier.")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(MAX_CLASSIFIER_BYTES + 1)
        if len(data) != info.st_size or b"\0" in data:
            raise ValueError("Invalid local classifier.")
        destination = Path(directory) / "outcome-classifier.js"
        with destination.open("xb") as stream:
            os.chmod(destination, 0o600)
            stream.write(data)
    finally:
        os.close(fd)


class DuplicateAccountEntry(ValueError):
    """Fixed local input mistake, never attach either entered value."""


class CredentialMemory:
    """Never serialize this object; bytearrays are cleared on close.

    Encoding/transport necessarily makes temporary copies. Neither Python nor
    the OS promises a secure-memory vault; use a dedicated disposable account.
    """
    def __init__(self, input_value=PRIVATE.local_account_input):
        self._values = []
        try:
            for prompt in ("Test account email", "Test account password"):
                value = input_value(prompt)
                if value is None or value == "":
                    raise ValueError("Account entry cancelled.")
                PRIVATE.secret_command(value)  # validate before retaining
                self._values.append(bytearray(value.encode("utf-8")))
                value = None
            if self._values[0] == self._values[1]:
                raise DuplicateAccountEntry()
        except BaseException:
            self.close()
            raise

    def input_value(self, prompt):
        if len(self._values) != 2:
            raise RuntimeError("Credential keeper is closed.")
        if prompt == "Test account email":
            return self._values[0].decode("utf-8")
        if prompt == "Test account password":
            return self._values[1].decode("utf-8")
        raise ValueError("Unsupported credential request.")

    def close(self):
        for value in self._values:
            value[:] = b"\0" * len(value)
        self._values.clear()


def confirm_retry(cancelled=lambda: False):
    # No credentials or author-controlled strings enter this dialog.
    if sys.platform != "darwin":
        return False  # terminal reads must never compete with the controller
    script = ('button returned of (display dialog '
              '"The previous attempt may have submitted your password. '
              'Allow one more attempt using the credentials already in RAM?" '
              'with title "Tilefinch — confirm sign-in retry" '
              'buttons {"Cancel", "Retry once"} default button "Cancel")')
    process = subprocess.Popen(["/usr/bin/osascript", "-e", script],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               env=PRIVATE.private_environment(tempfile.gettempdir()))
    try:
        for _ in range(480):
            if cancelled():
                return False
            try:
                output, _ = process.communicate(timeout=0.25)
                return process.returncode == 0 and output.strip() == b"Retry once"
            except subprocess.TimeoutExpired:
                continue
        return False
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)


class SessionKeeper:
    def __init__(self, lab, directory, credentials, *,
                 factory=PRIVATE.PrivateLab, retry_confirmation=confirm_retry,
                 password_attempt_limit=MAX_PASSWORD_ATTEMPTS,
                 confirm_first_attempt=False, confirm_each_attempt=False,
                 service="google"):
        if service not in SERVICE_URLS:
            raise ValueError("Invalid sign-in service.")
        if (type(password_attempt_limit) is not int
                or not 0 <= password_attempt_limit <= MAX_PASSWORD_ATTEMPTS):
            raise ValueError("Invalid password attempt limit.")
        self.lab = Path(lab)
        self.entry_url = SERVICE_URLS[service]
        self.directory = Path(directory)
        self.credentials = credentials
        self.factory = factory
        self.retry_confirmation = retry_confirmation
        self.password_attempt_limit = password_attempt_limit
        # Starting the local keeper authorizes separately requested attempts
        # until it stops. Extra per-attempt dialogs are explicitly opt-in.
        # Neither mode introduces an automatic retry loop.
        self.confirm_first_attempt = confirm_first_attempt
        self.confirm_each_attempt = confirm_each_attempt
        self.session = None
        self.values = None
        self.phase = "ready"
        self.password_attempts = 0
        self.inspection_ready = False
        self.worker = None
        self.lock = threading.Lock()
        self.stopping = threading.Event()

    def set_phase(self, phase):
        if phase not in PHASES:
            raise ValueError("Unsupported phase.")
        with self.lock:
            if not self.stopping.is_set():
                self.phase = phase
        print(f"Sign-in test: {phase}", flush=True)

    def status(self):
        with self.lock:
            result = {"phase": self.phase,
                      "busy": self.worker is not None and self.worker.is_alive(),
                      "password_attempts": self.password_attempts,
                      "inspection_ready": self.inspection_ready,
                      "credentials_retained": not self.stopping.is_set()}
            if self.values is not None:
                v = self.values
                result.update(origin=v[7], page_loaded=bool(v[1]),
                              editable=bool(v[2]), controls=v[5], pending_tasks=v[6],
                              last_action_accepted=bool(v[8]))
                if len(v) == 17:
                    result.update(zip(COUNTERS, v[9:]))
            return result

    def remember(self, values):
        with self.lock:
            self.values = values
        return values

    def restart(self):
        self.inspection_ready = False
        self.set_phase("starting_browser")
        old = self.session
        self.session = None
        if old is not None:
            old.close()
        for name in ("view.ppm", "view.png", "redacted.ppm", "redacted.ppm.txt", "redacted.png"):
            (self.directory / name).unlink(missing_ok=True)
        if self.stopping.is_set():
            return
        session = self.factory(self.lab, self.directory, url=self.entry_url)
        if self.stopping.is_set():
            session.close()
            return
        self.session = session
        self.remember(session.status())
        # The server-rendered identifier field precedes its asynchronous input
        # initialization. Give it a bounded real-time preparation window before
        # automated entry; do not wait indefinitely for all optional work.
        self.set_phase("preparing_page")
        self.remember(session.send(b"pump 192 16\n"))
        if self.stopping.is_set():
            return
        self.set_phase("browser_ready")

    def before_password(self):
        if self.stopping.is_set():
            raise RuntimeError("Stopped.")
        with self.lock:
            if (self.password_attempt_limit
                    and self.password_attempts >= self.password_attempt_limit):
                raise RuntimeError("Password attempt limit.")
            # Conservatively persists across failures and browser restarts.
            self.password_attempts += 1

    def run(self, action):
        try:
            if action == "restart":
                self.restart()
                return
            if self.session is None:
                self.restart()
            if self.stopping.is_set():
                return
            if action == "attempt":
                if (self.password_attempt_limit
                        and self.password_attempts >= self.password_attempt_limit):
                    self.set_phase("password_attempt_limit")
                    return
                if (self.confirm_each_attempt
                        or self.confirm_first_attempt and self.password_attempts == 0):
                    self.set_phase("awaiting_local_retry_confirmation")
                    if not self.retry_confirmation(self.stopping.is_set):
                        self.set_phase("retry_not_approved")
                        return
                self.remember(PRIVATE.guided_signin(
                    ObservedSession(self.session, self.remember), self.values,
                    input_value=self.credentials.input_value,
                    progress=self.set_phase, before_password=self.before_password,
                    cancelled=self.stopping.is_set))
                # Preview is explicit through view/view-below/view-above, never
                # automatic after submission (and may contain private fields).
            elif action == "advance":
                self.set_phase("advancing")
                self.remember(self.session.send(b"pump 8 16\n"))
                self.set_phase("page_advanced")
            elif action == "inspect":
                self.inspection_ready = False
                values = self.remember(PRIVATE.create_private_snapshot(self.session, self.directory))
                self.inspection_ready = bool(values[8])
            elif action in YOUTUBE_PAGES:
                self.set_phase("opening_youtube")
                # These fixed destinations select the lightweight provider
                # UI. No full-page script execution or personal feed text
                # crosses the control channel; inspect returns numeric results.
                values = self.remember(self.session.send(
                    b"private-open " + YOUTUBE_PAGES[action].encode("ascii") + b"\n"))
                if not values[8]:
                    self.set_phase("youtube_unavailable")
                    return
                # The native command may have recovered a scriptless sign-in
                # continuation through ordinary navigation. Give that page a
                # bounded set of normal turns; this does not submit inputs or
                # execute a full YouTube feed page.
                self.remember(self.session.send(b"pump 192 16\n"))
                self.set_phase("youtube_checked")
            elif action in YOUTUBE_UI_ACTIONS:
                # Explicit account mutations through the actual native UI.
                # No credentials or response text crosses this control path.
                self.set_phase("opening_youtube")
                name = YOUTUBE_UI_ACTIONS[action]
                values = self.remember(self.session.send(b"focus-id " + name + b"\n"))
                if not values[8]:
                    self.set_phase("youtube_unavailable")
                    return
                values = self.remember(self.session.send(b"activate\n"))
                if not values[8]:
                    self.set_phase("youtube_unavailable")
                    return
                self.remember(self.session.send(b"pump 192 16\n"))
                self.set_phase("youtube_checked")
            elif action in {"view", "view-below", "view-above"}:
                if action != "view":
                    self.remember(self.session.send(
                        b"page-down\n" if action == "view-below" else b"page-up\n"))
                self.remember(PRIVATE.show_private_view(self.session, self.directory))
            else:
                raise ValueError("Unsupported operation.")
        except Exception:
            # Never stringify an exception that could contain author content.
            self.set_phase("browser_operation_failed")

    def request(self, action):
        if action not in ACTIONS:
            return {"accepted": False, "reason": "unsupported_action"}
        if action == "status":
            return self.status()
        if action == "stop":
            self.stopping.set()
            return {"accepted": True, "phase": "stopping"}
        with self.lock:
            if self.stopping.is_set():
                return {"accepted": False, "reason": "stopping"}
            if self.worker is not None and self.worker.is_alive():
                return {"accepted": False, "reason": "busy"}
            self.inspection_ready = False
            self.phase = "queued"
            self.worker = threading.Thread(target=self.run, args=(action,),
                                           name="private-signin-driver", daemon=True)
            self.worker.start()
        return {"accepted": True, "phase": "queued"}

    def close(self):
        self.stopping.set()
        try:
            worker = self.worker
            # Interrupt a blocked native operation without concurrent pipe reads.
            if worker is not None and worker.is_alive() and self.session is not None:
                process = self.session.process
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
            if worker is not None:
                worker.join(timeout=10)
            if self.session is not None:
                self.session.close()
                self.session = None
        finally:
            self.credentials.close()


def serve(keeper, path, *, ready=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(path))
        Path(path).chmod(0o600)
        listener.listen(4)
        listener.settimeout(0.5)
        print(f"Control socket: {path}", flush=True)
        print("Credentials stay in this helper's RAM. Ctrl-C ends the session.", flush=True)
        if keeper.confirm_each_attempt or keeper.confirm_first_attempt:
            print("Optional attempt confirmation is enabled.", flush=True)
        else:
            print("Starting authorizes requested attempts until stopped.", flush=True)
        print("No automatic retries or previews; use view to open a preview.", flush=True)
        keeper.request("attempt")
        if ready is not None:
            ready.set()
        while not keeper.stopping.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(2)
                try:
                    request = bytearray()
                    for _ in range(MAX_REQUEST):
                        part = connection.recv(MAX_REQUEST + 1 - len(request))
                        if not part:
                            break
                        request.extend(part)
                        if b"\n" in request or len(request) > MAX_REQUEST:
                            break
                    if (len(request) > MAX_REQUEST or not request.endswith(b"\n")
                            or request.count(b"\n") != 1):
                        response = {"accepted": False, "reason": "invalid_request"}
                    else:
                        # No selectors, URLs, eval source, values, or credentials
                        # are accepted by this channel, only fixed action names.
                        response = keeper.request(request[:-1].decode("ascii"))
                    connection.sendall(json.dumps(response).encode("ascii") + b"\n")
                except (OSError, UnicodeError):
                    continue


def control(path, action):
    if action not in ACTIONS:
        raise ValueError("Unsupported action.")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        connection.sendall(action.encode("ascii") + b"\n")
        response = bytearray()
        while b"\n" not in response and len(response) <= MAX_RESPONSE:
            data = connection.recv(MAX_RESPONSE + 1 - len(response))
            if not data:
                break
            response.extend(data)
        if len(response) > MAX_RESPONSE or not response.endswith(b"\n"):
            raise RuntimeError("Invalid controller response.")
        result = json.loads(response)
        if not isinstance(result, dict):
            raise RuntimeError("Invalid controller response.")
        # Do not print arbitrary JSON from a substituted socket endpoint.
        for key, value in result.items():
            if key in {"accepted", "busy", "credentials_retained", "page_loaded",
                       "editable", "last_action_accepted", "inspection_ready"}:
                valid = type(value) is bool
            elif key in {"password_attempts", "controls", "pending_tasks"} or key in COUNTERS:
                valid = type(value) is int and 0 <= value <= 0xffffffffffffffff
            elif key == "phase":
                valid = type(value) is str and value in PHASES
            elif key == "reason":
                valid = type(value) is str and value in {
                    "unsupported_action", "stopping", "busy", "invalid_request"}
            elif key == "origin":
                parsed = PRIVATE.urlsplit(value) if type(value) is str else None
                valid = (value == "unavailable" or parsed is not None
                         and parsed.scheme in {"http", "https"} and parsed.hostname
                         and not (parsed.path or parsed.query or parsed.fragment
                                  or parsed.username or parsed.password))
            else:
                valid = False
            if not valid:
                raise RuntimeError("Invalid controller response.")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path,
                        default=ROOT / "build-preset-release/psp-browser-interactive-lab")
    parser.add_argument("--control", type=Path, help="Existing private control socket")
    parser.add_argument("--action", choices=sorted(ACTIONS), default="status")
    parser.add_argument("--password-attempt-limit", type=int,
                        choices=range(0, MAX_PASSWORD_ATTEMPTS + 1),
                        default=MAX_PASSWORD_ATTEMPTS,
                        help="Session-wide cap; 0 is uncapped until stopped; "
                             "restart does not reset counts")
    parser.add_argument("--confirm-first-attempt", action="store_true",
                        help="Require local approval before the first guided attempt too")
    parser.add_argument("--confirm-each-attempt", action="store_true",
                        help="Opt in to a local approval dialog for every attempt")
    parser.add_argument("--outcome-classifier", type=Path,
                        help="Explicitly approved owner-only local diagnostic filter; "
                             "only fixed category counts are reported")
    parser.add_argument("--service", choices=sorted(SERVICE_URLS), default="google",
                        help="Start the ordinary sign-in journey for this service; "
                             "session cookies remain inside the browser")
    args = parser.parse_args()
    if args.control is not None:
        try:
            print(json.dumps(control(args.control, args.action), sort_keys=True))
            return 0
        except (OSError, ValueError, RuntimeError):
            print("Private controller unavailable; no credential data was accessed.", file=sys.stderr)
            return 1
    if not sys.stdin.isatty() or not sys.stderr.isatty():
        parser.error("Start the keeper yourself in an interactive local terminal.")
    lab = args.lab.resolve()
    if not lab.is_file():
        parser.error("Build psp-browser-interactive-lab first.")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    keeper = credentials = None
    print("Use only the dedicated disposable test account. No credential files or logs.", flush=True)
    try:
        # A short path avoids the macOS AF_UNIX pathname limit. This directory
        # and socket are owner-only, and are not part of a repository/backup.
        with tempfile.TemporaryDirectory(prefix="tf-auth-", dir="/private/tmp") as directory:
            if args.outcome_classifier is not None:
                install_local_classifier(args.outcome_classifier, directory)
            with warnings.catch_warnings():
                warnings.simplefilter("error", PRIVATE.getpass.GetPassWarning)
                credentials = CredentialMemory()
            keeper = SessionKeeper(lab, directory, credentials,
                                   password_attempt_limit=args.password_attempt_limit,
                                   confirm_first_attempt=args.confirm_first_attempt,
                                   confirm_each_attempt=args.confirm_each_attempt,
                                   service=args.service)
            try:
                serve(keeper, Path(directory) / "control.sock")
            finally:
                keeper.close()
    except KeyboardInterrupt:
        print("\nPrivate session closed.")
    except DuplicateAccountEntry:
        print("The email and password entries were identical. Nothing was submitted. "
              "Restart and enter the password in the second dialog.", file=sys.stderr)
        return 1
    except (OSError, ValueError, RuntimeError, PRIVATE.getpass.GetPassWarning):
        print("The private session could not start; no diagnostic contents were saved.", file=sys.stderr)
        return 1
    finally:
        if credentials is not None:
            credentials.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
