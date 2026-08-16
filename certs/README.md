# PSP TLS trust bundle

`roots.pem` is the deliberately compact trust bundle staged beside the live
PSP EBOOT. It contains eight public root certificates:

- Amazon Root CA 1 (expires 2038-01-17)
- DigiCert Global Root G2 (expires 2038-01-15)
- DigiCert Global Root G3 (expires 2038-01-15)
- GlobalSign Root CA R1 (expires 2028-01-28)
- GlobalSign Root CA R3 (expires 2029-03-18)
- GTS Root R1 (expires 2036-06-22)
- ISRG Root X1 (expires 2035-06-04)
- USERTrust RSA Certification Authority (expires 2038-01-18)

The PEM encodings were selected from system trust data or the CA's
authoritative repository and checked with OpenSSL. Host curl using only this
bundle has completed certificate verification for Wikipedia, Reddit, Hacker
News, GOV.UK, The New York Times, and YouTube (`ssl_verify_result=0` for all
six). The PSP mbedTLS backend has also verified the alternate Google chain
`WR2 -> GTS Root R1 -> GlobalSign Root CA R1` served to the mobile YouTube
endpoint. The legacy R1 anchor is retained specifically for old TLS chain
builders and must be reviewed or replaced before its 2028 expiry.

This is not a complete WebPKI trust store. A site whose chain terminates at a
different root fails closed; Tilefinch never disables peer or hostname
verification. Before a release, revalidate the retained roots against the
authoritative CA repositories, check their expiry/revocation status, and rerun
the acceptance-site TLS probe. Adding roots is a security decision as well as
a compatibility decision, so keep this file curated instead of silently
copying the host's full store into the PSP package.
