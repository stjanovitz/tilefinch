"""The top-level document of a recorded HTTP trace (NNNN.meta records, as
psp-browser-interactive-lab --capture-http writes them): which record
answered a URL, where its redirects ended, and whether that end is a
sign-in page. Shared by benchmarks/capture-census.py (access barriers
before a reference capture) and tools/site-census/census.py (a capture that
landed somewhere other than the page it asked for)."""
from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import urljoin, urlsplit

RECORD_LIMIT = 4096
META_BYTES_LIMIT = 1024 * 1024
REDIRECT_LIMIT = 20
REDIRECT_STATUSES = ("301", "302", "303", "307", "308")
SIGNIN_PATH = re.compile(r"/(?:login|signin|sign-in)(?:/|$)", re.I)


def read_meta(path: Path) -> dict:
    """One record's key=value fields; refuses anything unbounded or
    ambiguous (a duplicate key)."""
    if path.is_symlink() or not path.is_file() or path.stat().st_size > META_BYTES_LIMIT:
        raise ValueError("source response metadata is not a bounded regular file")
    fields = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or key in fields:
            raise ValueError("invalid source response metadata")
        fields[key] = value
    return fields


def response_headers(fields: dict) -> dict:
    """Lower-cased response header name -> its values, in record order."""
    headers = {}
    for key, value in fields.items():
        if re.fullmatch(r"response-header-[0-9]+", key):
            name, separator, content = value.partition(":")
            if separator:
                headers.setdefault(name.strip().lower(), []).append(content.strip())
    return headers


def records(trace: Path) -> list[Path]:
    found = sorted(Path(trace).glob("[0-9][0-9][0-9][0-9].meta"))
    if len(found) > RECORD_LIMIT:
        raise ValueError("source response inventory exceeds its bound")
    return found


def document_response(trace: Path, url: str) -> dict | None:
    """The first GET record for url (without its fragment), or None."""
    target = url.split("#", 1)[0]
    for path in records(trace):
        fields = read_meta(path)
        if fields.get("url") == target and fields.get("method") == "GET":
            return fields
    return None


def signin_redirect(target: str, location: str) -> bool:
    return bool(SIGNIN_PATH.search(urlsplit(urljoin(target, location)).path))


def final_url(trace: Path, url: str) -> str | None:
    """Where the document request for url ended: redirects recorded as their
    own records are followed, and a record the transport followed itself
    reports its effective-url. None when the trace has no record for url."""
    current = url.split("#", 1)[0]
    for _ in range(REDIRECT_LIMIT):
        fields = document_response(trace, current)
        if fields is None:
            return None if current == url.split("#", 1)[0] else current
        location = response_headers(fields).get("location", [])
        if fields.get("status") in REDIRECT_STATUSES and location:
            current = urljoin(current, location[0]).split("#", 1)[0]
            continue
        return fields.get("effective-url") or current
    return current


def moved(requested: str, final: str | None) -> str | None:
    """Why a capture of requested that ended at final is not that page, or
    None when the host and path are the same (query and fragment may
    differ). A sign-in path says so; any other move is a redirect."""
    if final is None:
        return "no-document-record"
    want, got = urlsplit(requested), urlsplit(final)
    # A trailing slash added or dropped names the same page.
    if (want.hostname or "").lower() == (got.hostname or "").lower() \
            and (want.path.rstrip("/") or "/") == (got.path.rstrip("/") or "/"):
        return None
    if SIGNIN_PATH.search(got.path):
        return "source-signin-required"
    return "source-redirected"
