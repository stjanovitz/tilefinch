# TLS CA-bundle provenance and redistribution

Tilefinch distributes `certs/roots.pem` as TLS trust data. It contains 27
public root certificates, not private keys. Certificate names identify their
issuers; inclusion does not imply endorsement by those issuers. Tilefinch's
MIT license does not supersede third-party rights in certificate data.

## Mozilla CA-store data

Mozilla's CA-store data, including curl's converted PEM bundle, is distributed
under the Mozilla Public License, version 2.0. The applicable upstream license
notice is preserved here, and the covered certificate data is available under
those terms.
The PEM source is included in Tilefinch's source repository:

- [Tilefinch's certificate source](https://github.com/stjanovitz/tilefinch/blob/main/certs/roots.pem)
- [Mozilla CA-store source](https://github.com/mozilla-firefox/firefox/blob/release/security/nss/lib/ckfw/builtins/certdata.txt)
- [curl's CA-store license explanation](https://curl.se/docs/caextract.html)
- [MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/)

The complete license accompanies this source tree at
`third_party/public_suffix/LICENSE.MPL-2.0` and binary install archives at
`NOTICES/public_suffix/LICENSE.MPL-2.0`; it is shared with the Public Suffix
List rather than duplicated. This is a notice for Mozilla-derived data, not
a blanket assertion that every direct CA download is MPL-licensed.

Twenty-three retained certificates are byte-identical to entries in the Mozilla
PEM bundle dated 2026-09-25 (bundle SHA-256
`a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`):

- Amazon Root CA 1
- DigiCert Global Root G2, Global Root G3, and TLS RSA4096 Root G5
- GlobalSign ECC Root CA R4 and R5, Root CA R3, and Root R46
- Go Daddy Root Certificate Authority G2
- GTS Root R3 and R4
- HARICA TLS RSA Root CA 2021
- IdenTrust Commercial Root CA 1
- ISRG Root X1
- QuoVadis Root CA 2 G3
- Sectigo Public Server Authentication Root R46
- SSL.com Root Certification Authority ECC, TLS ECC Root CA 2022, and TLS RSA Root CA 2022
- Starfield Root Certificate Authority G2
- Starfield Services Root Certificate Authority G2
- USERTrust RSA and ECC Certification Authorities

The Microsoft certificate matches its issuer's MPL-2.0 Azure Linux CA package,
identified below. These comparisons establish licensed sources for the exact
data, not the original download history of every entry. The covered CA data
in `roots.pem` is available under MPL-2.0. CA sources and trust rationale are
also documented in `certs/README.md`. No certificate bytes were changed.

## Complete certificate inventory

Fingerprints are SHA-256 of the complete signed DER certificate. "Mozilla MPL"
means the exact bytes match the Mozilla snapshot above; "Microsoft MPL" means
they match the pinned Azure Linux source below. The CA grants are summarized
in the following sections. Redistribution permission is separate from a
certificate's validity, trust status, and permitted uses.

| Certificate | DER SHA-256 | Redistribution basis |
| --- | --- | --- |
| Amazon Root CA 1 | `8ecde6884f3d87b1125ba31ac3fcb13d7016de7f57cc904fe1cb97c6ae98196e` | Mozilla MPL; Amazon grant |
| DigiCert Global Root G2 | `cb3ccbb76031e5e0138f8dd39a23f9de47ffc35e43c1144cea27d46a5ab1cb5f` | Mozilla MPL; DigiCert grant |
| DigiCert Global Root G3 | `31ad6648f8104138c738f39ea4320133393e3a18cc02296ef97c2ac9ef6731d0` | Mozilla MPL; DigiCert grant |
| DigiCert TLS RSA4096 Root G5 | `371a00dc0533b3721a7eeb40e8419e70799d2b0a0f2c1d80693165f7cec4ad75` | Mozilla MPL; DigiCert grant |
| GlobalSign ECC Root CA R4 | `b085d70b964f191a73e4af0d54ae7a0e07aafdaf9b71dd0862138ab7325a24a2` | Mozilla MPL; GlobalSign/GTS grants |
| GlobalSign ECC Root CA R5 | `179fbc148a3dd00fd24ea13458cc43bfa7f59c8182d783a513f6ebec100c8924` | Mozilla MPL; GlobalSign grant |
| GlobalSign Root CA R1 | `ebd41040e4bb3ec742c9e381d31ef2a41a48b6685c96e7cef3c1df6cd4331c99` | GlobalSign grant |
| GlobalSign Root CA R3 | `cbb522d7b7f127ad6a0113865bdf1cd4102e7d0759af635a7cf4720dc963c53b` | Mozilla MPL; GlobalSign grant |
| GlobalSign Root R46 | `4fa3126d8d3a11d1c4855a4f807cbad6cf919d3a5a88b03bea2c6372d93c40c9` | Mozilla MPL; GlobalSign grant |
| Go Daddy Root Certificate Authority G2 | `45140b3247eb9cc8c5b4f0d7b53091f73292089e6e5a63e2749dd3aca9198eda` | Mozilla MPL; Starfield/Go Daddy grant |
| GTS Root R1 | `2a575471e31340bc21581cbd2cf13e158463203ece94bcf9d3cc196bf09a5472` | GTS grant |
| GTS Root R2 | `8d25cd97229dbf70356bda4eb3cc734031e24cf00fafcfd32dc76eb5841c7ea8` | GTS grant |
| GTS Root R3 | `34d8a73ee208d9bcdb0d956520934b4e40e69482596e8b6f73c8426b010a6f48` | Mozilla MPL; GTS grant |
| GTS Root R4 | `349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d` | Mozilla MPL; GTS grant |
| HARICA TLS RSA Root CA 2021 | `d95d0e8eda79525bf9beb11b14d2100d3294985f0c62d9fabd9cd999eccb7b1d` | Mozilla MPL |
| IdenTrust Commercial Root CA 1 | `5d56499be4d2e08bcfcad08a3e38723d50503bde706948e42f55603019e528ae` | Mozilla MPL |
| ISRG Root X1 | `96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6` | Mozilla MPL; ISRG grant |
| Microsoft TLS RSA Root G2 | `6a170583db584151e1c454eeca2a64cc5d8e484a5bd1156e720b4458654ee9e5` | Microsoft MPL |
| QuoVadis Root CA 2 G3 | `8fe4fb0af93a4d0d67db0bebb23e37c71bf325dcbcdd240ea04daf58b47e1840` | Mozilla MPL; QuoVadis grant |
| Sectigo Public Server Authentication Root R46 | `7bb647a62aeeac88bf257aa522d01ffea395e0ab45c73f93f65654ec38f25a06` | Mozilla MPL; Sectigo grant |
| SSL.com Root Certification Authority ECC | `3417bb06cc6007da1b961c920b8ab4ce3fad820e4aa30b9acbc4a74ebdcebc65` | Mozilla MPL; SSL.com grant |
| SSL.com TLS ECC Root CA 2022 | `c32ffd9f46f936d16c3673990959434b9ad60aafbb9e7cf33654f144cc1ba143` | Mozilla MPL; SSL.com grant |
| SSL.com TLS RSA Root CA 2022 | `8faf7d2e2cb4709bb8e0b33666bf75a5dd45b5de480f8ea8d4bfe6bebc17f2ed` | Mozilla MPL; SSL.com grant |
| Starfield Root Certificate Authority G2 | `2ce1cb0bf9d2f9e102993fbe215152c3b2dd0cabde1c68e5319b839154dbb7f5` | Mozilla MPL; Starfield grant |
| Starfield Services Root Certificate Authority G2 | `568d6905a2c88708a4b3025190edcfedb1974a606a13c6e5290fcb2ae63edab5` | Mozilla MPL; Amazon grant |
| USERTrust RSA Certification Authority | `e793c9b02fd8aa13e21c31228accb08119643b749c898964b1746d46c3d4cbd2` | Mozilla MPL; Sectigo grant |
| USERTrust ECC Certification Authority | `4ff460d54b9c86dabfbcfc5712e0400d2bed3fbc4d4fbdaa86e06adcd2a9ad7a` | Mozilla MPL; Sectigo grant |

## Issuer terms

### Amazon Trust Services

[ATS CP/CPS v2.7, section 9.5](https://www.amazontrust.com/repository/ats-cp-cps-2.7.pdf)
allows royalty-free, nonexclusive reproduction and distribution of complete
certificates. Amazon retains its intellectual property rights.
The Starfield Services Root G2 bytes also match Mozilla's MPL-2.0 bundle and
[Amazon's authoritative certificate](https://www.amazontrust.com/repository/SFSRootCAG2.pem).
The complete signed DER bytes are unchanged; this is not Starfield Root G2.

### DigiCert

[DigiCert Public Trust CP/CPS v7.09, section 9.5.1](https://www.digicert.com/content/dam/digicert/pdfs/legal/digicert-public-trust-cp-cps-v7-09.pdf)
allows royalty-free, nonexclusive redistribution of complete certificates;
use remains subject to the referenced relying-party agreement. Section 9.5.4
also licenses root reproduction in trustworthy software and hardware.

### Go Daddy and Starfield

[Starfield CP/CPS v5.01, sections 9.5.1 and 9.5.4](https://certs.godaddy.com/repository/previous_doc_versions/StarfieldCertificatePolicyandCertificationPracticeStatement_v5.01.pdf)
covers both retained G2 hierarchies (sections 10.1.2 and 10.1.4). It permits
royalty-free, nonexclusive redistribution of complete certificates, with use
subject to the relying-party agreement, and licenses root reproduction in
trustworthy software and hardware. Starfield Root G2 is distinct from
Amazon's Starfield Services Root G2.

### HARICA

[HARICA CP/CPS v4.13, section 9.5](https://repo.harica.gr/documents/CPS-EN-4.13.pdf)
retains intellectual property rights and permits attributed reproduction of
the CP/CPS. That policy-document permission is not treated as a certificate
license. The exact retained certificate is available in Mozilla's MPL-2.0
CA-store data, which is the redistribution basis used here.

### IdenTrust

[TrustID TLS CP/CPS v5.0.4, section 9.5](https://www.identrust.com/sites/default/files/2026-06/identrust_trustid_cpcps_tls-5.0.4.pdf)
addresses key ownership, trademarks, and rights in the policy document; it
does not supply a separate certificate-redistribution grant. The exact
Commercial Root CA 1 is available in Mozilla's MPL-2.0 CA-store data, which is
the redistribution basis used here. No IdenTrust policy-document text is shipped.

### Internet Security Research Group

[ISRG CP/CPS v6.2, section 9.5](https://letsencrypt.org/documents/isrg-cp-cps-v6.2/)
allows royalty-free, nonexclusive reproduction and distribution of complete
certificates. The policy document's separate Creative Commons license is not
used as the certificate's license.

### QuoVadis

[QuoVadis Root CA 2 CP/CPS v2.16, sections 9.5.1 and 9.5.4](https://www.digicert.com/content/dam/digicert/pdfs/legal/quovadis/archive/2022/qv-rca2-cpcps-v2-16.pdf)
allows royalty-free, nonexclusive redistribution of complete certificates,
with use subject to the referenced relying-party agreement, and licenses root
reproduction in trustworthy software and hardware. This hierarchy includes
the retained Root CA 2 G3.

### Sectigo and USERTrust

[Sectigo TLS CP/CPS v6.2.6, section 9.17.3](https://www.sectigo.com/uploads/legal/TLS-Combined-CP-CPS-v6.2.6.pdf)
allows royalty-free, nonexclusive reproduction and distribution of complete
certificates while Sectigo retains ownership. This covers the Sectigo and
USERTrust roots listed above. Section 9.5's ownership statement is separate
from the explicit grant in section 9.17.3.

### SSL.com

[SSL.com CP/CPS v1.34, section 9.5](https://legal.ssl.com/documents/SSLcom-CP-CPS.pdf)
allows royalty-free, nonexclusive reproduction and distribution of complete
certificates while SSL.com retains ownership.

## Compatibility certificates and additional issuer terms

Four retained certificates differ from that Mozilla snapshot. Their signed
certificate bytes must remain intact; PEM encoding is only a representation
of those bytes. Website text, policy documents, and CA logos are separate
works and are not redistributed with the certificates.

### Google Trust Services

GTS Root R1 (SHA-256
`2a575471e31340bc21581cbd2cf13e158463203ece94bcf9d3cc196bf09a5472`)
and the retained GTS Root R2 variant (SHA-256
`8d25cd97229dbf70356bda4eb3cc734031e24cf00fafcfd32dc76eb5841c7ea8`)
come from the [GTS repository](https://pki.goog/repository/), including
[`oldr1.pem`](https://i.pki.goog/oldr1.pem).

[GTS CPS v5.8, section 9.5](https://pki.goog/repo/cps/5.8/GTS-CPS.html)
permits royalty-free, nonexclusive reproduction and distribution of complete
certificates. It does not permit certificate derivative works without prior
written permission. Google or its licensors retain their intellectual
property rights; no trademark rights or endorsement are claimed.

### GlobalSign

GlobalSign Root CA R1 (SHA-256
`ebd41040e4bb3ec742c9e381d31ef2a41a48b6685c96e7cef3c1df6cd4331c99`)
is published in the [GlobalSign root repository](https://support.globalsign.com/ca-certificates/root-certificates/globalsign-root-certificates).

[GlobalSign CPS v9.10, section 9.5](https://www.globalsign.com/en/repository/GlobalSign_CPS_v9.10_final.pdf)
retains ownership of certificates while permitting royalty-free,
nonexclusive reproduction and distribution of complete certificates.
GlobalSign's trademarks remain their owner's property.

### Microsoft

Microsoft TLS RSA Root G2 (SHA-256
`6a170583db584151e1c454eeca2a64cc5d8e484a5bd1156e720b4458654ee9e5`)
is published in the [Microsoft PKI repository](https://www.microsoft.com/pkiops/docs/repository.htm).
It is not in the Mozilla snapshot used above; its MPL-2.0 redistribution basis
is the separate Microsoft-published source identified below.

[Microsoft Public TLS CPS v3.3.7, section 9.5](https://www.microsoft.com/pkiops/Docs/Content/policy/Microsoft_PKI_Services_public_tls_CPS_v3.3.7.pdf)
identifies certificates as Microsoft property. Its Creative Commons
Attribution-NoDerivatives license applies to the CPS document, not to the
certificate. The [Microsoft PKI Services Relying Party Agreement](https://www.microsoft.com/pkiops/Docs/Content/policy/MSRelyingPartyAgreement2018.pdf)
addresses certificate use and reliance; it is not used here as an open-source
redistribution grant.

The exact retained certificate is present in Microsoft's
[Azure Linux CA source at revision 32ae4cbc501bdabde2b20982bdfd3f53e25bc45f](https://github.com/microsoft/azurelinux/blob/32ae4cbc501bdabde2b20982bdfd3f53e25bc45f/SPECS/ca-certificates/certdata.base.txt).
That source file's SHA-256 is
`8896c309aef808c7769dc630abee75adbb6bfb5c8a961461b51f845a1740ea66`.
The same revision's [package specification](https://github.com/microsoft/azurelinux/blob/32ae4cbc501bdabde2b20982bdfd3f53e25bc45f/SPECS/ca-certificates/ca-certificates.spec)
expressly identifies MPLv2.0 and includes its
[complete MPL-2.0 license](https://github.com/microsoft/azurelinux/blob/32ae4cbc501bdabde2b20982bdfd3f53e25bc45f/SPECS/ca-certificates/LICENSE).
This issuer-published, certificate-package-specific license supplies the
redistribution basis; Microsoft's general repository MIT license is not
substituted for it. Cross-signed certificates with the same name are different
data and are not the retained trust anchor.

## Distribution contents

Source distributions contain `certs/roots.pem`, this notice, and the complete
MPL-2.0 license. Newly staged binary install archives include `roots.pem`,
`NOTICES/ca-bundle/NOTICE.md`, and `NOTICES/public_suffix/LICENSE.MPL-2.0`.
The install manifest requires both notices, and the CA-bundle test checks
that every certificate fingerprint has exactly one inventory entry.
