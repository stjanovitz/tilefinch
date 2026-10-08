# Public Suffix List snapshot

`src/public_suffix_dafsa.inc` is an ASCII/punycode DAFSA generated from the
[Public Suffix List](https://publicsuffix.org/) at commit
`3929462652695bad04f0a27afb600974014a3c8b` (2026-10-07), downloaded from
the official published list. The source list's SHA-256 is
`ba7d836c0ea57a8bcf9f1fcb868066a7554bed029b441ecc6204ad8494a32612`.

Generation command, using libpsl commit
`3e02f2cd038209e873c970709a9eeeead4d70afa`:

```sh
python3 src/psl-make-dafsa --encoding=ascii --output-format=cxx \
  list/public_suffix_list.dat suffixes_ascii_dafsa.h
```

The checked-in graph is 53,341 bytes in the linked image, needs no heap, and
contains both ICANN and PRIVATE rules. Tilefinch accepts only ASCII-serialized
URL hosts, so the graph contains punycode rules but omits duplicate UTF-8
spellings. Refresh the snapshot as a security maintenance task when the PSL
changes; cookie Domain admission fails closed for invalid/unclassifiable host
syntax.

The PSL data is covered by MPL-2.0. The fixed-set decoder was adapted from
libpsl/Chromium under its BSD license. See the adjacent license files.
