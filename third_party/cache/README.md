# Local dependency cache

Run `scripts/fetch-psp-transport-deps.sh` to populate this directory with the
exact, hash-verified source archives listed in
`third_party/psp_transport/dependencies.lock`.

The archives are deliberately not committed. PSP transport builds consume only
this cache and never contact the network, so a populated cache is sufficient
for reproducible offline rebuilds.
