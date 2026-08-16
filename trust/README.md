# Tilefinch release trust root

`root-v1.tfur` is the public binary trust record embedded by the official PSP
configure preset. It contains the root and release verification keys used to
authenticate signed Stable and Beta update metadata. It is public by design:
users receive the same bytes inside the launcher, and source builders can
inspect and reproduce that trust decision.

SHA-256:

```text
0eb708ab00b966a70d7220555718ec421158c1f14df2c506616e54fd27c51777
```

Private signing keys never belong in this directory or repository. The
repository ignore rules reject the ordinary private-key filename classes as a
last-resort guard; release ceremonies must still keep all private material in
separate encrypted offline storage.
