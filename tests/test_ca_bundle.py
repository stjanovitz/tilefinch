#!/usr/bin/env python3
"""Pin the deliberately compact PSP trust bundle and its release roots."""

import base64
import hashlib
import pathlib
import re
import sys


EXPECTED_CERTIFICATE_COUNT = 9
USERTRUST_ECC_SHA256 = (
    "4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a"
)


def main() -> int:
    source_root = pathlib.Path(sys.argv[1])
    pem = (source_root / "certs" / "roots.pem").read_text(encoding="ascii")
    bodies = re.findall(
        r"-----BEGIN CERTIFICATE-----\s*(.*?)\s*-----END CERTIFICATE-----",
        pem,
        flags=re.DOTALL,
    )
    if len(bodies) != EXPECTED_CERTIFICATE_COUNT:
        raise AssertionError(
            f"expected {EXPECTED_CERTIFICATE_COUNT} roots, found {len(bodies)}"
        )

    fingerprints = []
    for body in bodies:
        der = base64.b64decode("".join(body.split()), validate=True)
        fingerprints.append(hashlib.sha256(der).hexdigest())

    if len(set(fingerprints)) != len(fingerprints):
        raise AssertionError("the trust bundle contains a duplicate certificate")
    if USERTRUST_ECC_SHA256 not in fingerprints:
        raise AssertionError("USERTrust ECC release root is missing or changed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
