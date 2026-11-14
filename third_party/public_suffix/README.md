# Public Suffix List snapshot

`src/public_suffix_dafsa.inc` is an ASCII/punycode DAFSA generated from the
[Public Suffix List](https://publicsuffix.org/) at commit
`71c235dd3f888dd4ecd066817050a7482da710e5` (2026-11-13), downloaded from
the official published list. The source list's SHA-256 is
`8576b335b871dea99dc0fccd04138311bd43f7a8da256c31bf964f11e2f7c179`.

Generation command, using libpsl commit
`3e02f2cd038209e873c970709a9eeeead4d70afa`:

```sh
python3 src/psl-make-dafsa --encoding=ascii --output-format=cxx \
  list/public_suffix_list.dat suffixes_ascii_dafsa.h
```

The checked-in graph is 53,843 bytes in the linked image, needs no heap, and
contains both ICANN and PRIVATE rules. Tilefinch accepts only ASCII-serialized
URL hosts, so the graph contains punycode rules but omits duplicate UTF-8
spellings. Refresh the snapshot as a security maintenance task when the PSL
changes; cookie Domain admission fails closed for invalid/unclassifiable host
syntax.

The PSL data is covered by MPL-2.0. The fixed-set decoder was adapted from
libpsl/Chromium under its BSD license. See the adjacent license files.
