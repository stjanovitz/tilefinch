# curl / libcurl notice

The default PSP release build links project-owned libcurl 8.21.0 from the
official hash-pinned source archive listed in
`third_party/psp_transport/dependencies.lock`. curl is distributed under the
curl license; the complete verbatim `COPYING` from that archive is beside this
file.

`TILEFINCH_PSP_TRANSPORT_MODE=LEGACY` is an explicit development escape hatch
that instead links the PSPDEV SDK's curl 7.64.1. It is not the release default.
