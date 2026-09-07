# Public Suffix List snapshot

`src/public_suffix_dafsa.inc` is an ASCII/punycode DAFSA generated from the
[Public Suffix List](https://publicsuffix.org/) at commit
`f540a06159213b6e8ed9c87d2dd3a52373637e92` (2026-09-05). The source list's
SHA-256 is `ad47cebef5f86eb77e2c9514d42cde4bd07f591a1f93384a63568624ec3f1f37`.

Generation command, using libpsl commit
`3e02f2cd038209e873c970709a9eeeead4d70afa`:

```sh
python3 src/psl-make-dafsa --encoding=ascii --output-format=cxx \
  list/public_suffix_list.dat suffixes_ascii_dafsa.h
```

The checked-in graph is 53,292 bytes in the linked image, needs no heap, and
contains both ICANN and PRIVATE rules. Tilefinch accepts only ASCII-serialized
URL hosts, so the graph contains punycode rules but omits duplicate UTF-8
spellings. Refresh the snapshot as a security maintenance task when the PSL
changes; cookie Domain admission fails closed for invalid/unclassifiable host
syntax.

The PSL data is covered by MPL-2.0. The fixed-set decoder was adapted from
libpsl/Chromium under its BSD license. See the adjacent license files.
