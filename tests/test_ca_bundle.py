#!/usr/bin/env python3
"""Pin the deliberately compact PSP trust bundle and its release roots."""

import base64
import hashlib
import pathlib
import re
import sys


EXPECTED_CERTIFICATE_COUNT = 19
REQUIRED_SHA256_FINGERPRINTS = {
    # GitHub's current Sectigo chain.
    "4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a",
    # Certainly-backed resource CDNs observed in the top-site census.
    "2ce1cb0bf9d2f9e102993fbe215152c3b2dd0cabde1c68e5319b839154dbb7f5",
    # Current Microsoft TLS G2 resource chains.
    "6a170583db584151e1c454eeca2a64cc5d8e484a5bd1156e720b4458654ee9e5",
    # The current cloudflare-dns.com chain.
    "3417bb06cc6007da1b961c920b8ab4ce3fad820e4aa30b9acbc4a74ebdcebc65",
    # Go Daddy's current G2 public hierarchy.
    "45140b3247eb9cc8c5b4f0d7b53091f73292089e6e5a63e2749dd3aca9198eda",
    # IdenTrust commercial server chains.
    "5d56499be4d2e08bcfcad08a3e38723d50503bde706948e42f55603019e528ae",
    # GlobalSign's current ECC and RSA hierarchies.
    "179fbc148a3dd00fd24ea13458cc43bfa7f59c8182d783a513f6ebec100c8924",
    "4fa3126d8d3a11d1c4855a4f807cbad6cf919d3a5a88b03bea2c6372d93c40c9",
    # HARICA's modern RSA TLS hierarchy.
    "d95d0e8eda79525bf9beb11b14d2100d3294985f0c62d9fabd9cd999eccb7b1d",
    # SSL.com's modern ECC TLS hierarchy.
    "c32ffd9f46f936d16c3673990959434b9ad60aafbb9e7cf33654f144cc1ba143",
    # Sectigo's current RSA server-authentication hierarchy.
    "7bb647a62aeeac88bf257aa522d01ffea395e0ab45c73f93f65654ec38f25a06",
}


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
    missing = REQUIRED_SHA256_FINGERPRINTS.difference(fingerprints)
    if missing:
        raise AssertionError(
            "required release roots are missing or changed: "
            + ", ".join(sorted(missing))
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
