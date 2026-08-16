# PSP TLS trust bundle

`roots.pem` is the deliberately compact trust bundle staged beside the live
PSP EBOOT. It contains nineteen public root certificates:

- Amazon Root CA 1 (expires 2038-01-17)
- DigiCert Global Root G2 (expires 2038-01-15)
- DigiCert Global Root G3 (expires 2038-01-15)
- GlobalSign ECC Root CA - R5 (expires 2038-01-19)
- GlobalSign Root CA R1 (expires 2028-01-28)
- GlobalSign Root CA R3 (expires 2029-03-18)
- GlobalSign Root R46 (expires 2046-03-20)
- Go Daddy Root Certificate Authority - G2 (expires 2037-12-31)
- GTS Root R1 (expires 2036-06-22)
- HARICA TLS RSA Root CA 2021 (expires 2045-02-13)
- IdenTrust Commercial Root CA 1 (expires 2034-01-16)
- ISRG Root X1 (expires 2035-06-04)
- Microsoft TLS RSA Root G2 (expires 2040-04-10)
- Sectigo Public Server Authentication Root R46 (expires 2046-03-21)
- SSL.com Root Certification Authority ECC (expires 2041-02-12)
- SSL.com TLS ECC Root CA 2022 (expires 2046-08-19)
- Starfield Root Certificate Authority - G2 (expires 2037-12-31)
- USERTrust RSA Certification Authority (expires 2038-01-18)
- USERTrust ECC Certification Authority (expires 2038-01-18)

The PEM encodings come from the CAs' authoritative repositories and are
checked by fingerprint. Qualification uses Mbed TLS 3.6.x with this file as
its only trust store. System curl is not an acceptable substitute on macOS:
its SecureTransport backend can consult the operating system's roots and hide
an anchor missing from the PSP bundle. The PSP Mbed TLS backend has also
verified the alternate Google chain
`WR2 -> GTS Root R1 -> GlobalSign Root CA R1` served to the mobile YouTube
endpoint. The legacy R1 anchor is retained specifically for old TLS chain
builders and must be reviewed or replaced before its 2028 expiry.

The USERTrust ECC anchor verifies Sectigo's E46 chain, including the chain
currently served by GitHub. Its DER certificate is published by Sectigo at
`crt.sectigo.com/USERTrustECCCertificationAuthority.crt`; the retained
SHA-256 fingerprint is
`4F:F4:60:D5:4B:9C:86:DA:BF:BC:FC:57:12:E0:40:0D:2B:ED:3F:BC:4D:4F:BD:AA:86:E0:6A:DC:D2:A9:AD:7A`.

The compatibility anchors cover current chains observed in the top-site and
resource-origin census when verified with Mbed TLS rather than the host
operating system's trust store. They are all self-signed public roots; no
leaf, intermediate, private, or locally generated certificate is trusted.
`tests/test_ca_bundle.py` pins the SHA-256 fingerprints of every root added for
live-chain compatibility so a download or accidental replacement cannot
silently change the trust decision.

This is not a complete WebPKI trust store. A site whose chain terminates at a
different root fails closed; Tilefinch never disables peer or hostname
verification. Before a release, revalidate the retained roots against the
authoritative CA repositories, check their expiry/revocation status, and rerun
the acceptance-site TLS probe. Adding roots is a security decision as well as
a compatibility decision, so keep this file curated instead of silently
copying the host's full store into the PSP package.

After the PSP dependency build has unpacked Mbed TLS, build its native client
and run the live qualification against a current top-300 list, the
signed-update hosts, and any locally available resource traces. The ranking is
downloaded for the qualification and is not committed:

```sh
cmake \
  -S build-preset-psp/psp-transport/mbedtls/src/tilefinch_psp_mbedtls \
  -B /tmp/tilefinch-mbedtls-host \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=ON \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tilefinch-mbedtls-host --target ssl_client2 -j8
curl -fsSL https://tranco-list.eu/top-1m.csv.zip -o /tmp/tranco-top-1m.zip
unzip -p /tmp/tranco-top-1m.zip | sed -n '1,300p' > /tmp/tranco-top-300.csv
scripts/audit-site-tls.py \
  --mbedtls-client /tmp/tilefinch-mbedtls-host/programs/ssl/ssl_client2 \
  --top-sites /tmp/tranco-top-300.csv --limit 300 \
  --trace-root fidelity/captures --trace-root fidelity/candidates
```

The trace roots are optional and ignored when absent. DNS-only service apexes,
TLS protocol incompatibilities, and an apex whose certificate correctly covers
only its real service hosts are reported separately. A certificate-policy,
expiry, or missing-trust failure makes the audit exit nonzero and must be
reviewed. A confirmed broken server may be recorded as such; the audit must
not be made green by trusting its leaf or intermediate certificate.
