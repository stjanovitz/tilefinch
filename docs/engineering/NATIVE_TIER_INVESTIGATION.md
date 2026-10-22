# Native execution tier: decision

As of 2026-09-30, native-tier and whole-graph ahead-of-time (AOT) compilation
research is parked, and neither is part of any build. The interpreter remains
the execution path; cached QuickJS bytecode is distinct from native machine
code.

The probes, region emitters, whole-page A/B lab, census tooling, results and
profiles were removed from the public tree on 2026-10-21 and live in the
private development archive.

In short: an isolated Allegrex emitter ran bounded bytecode regions correctly
on a PSP-3000, but matched device runs gave at most 5–11% on narrow
property kernels, regressed proxies and generic batches, and showed no
whole-page gain. Short supported regions, frequent interpreter exits and
value/frame handoff costs dominated. Whole-graph AOT never reached a qualified
runtime. Profile-guided optimization, validated lazy compilation, cached
module bytecode and narrower style invalidation delivered the gains instead.

Reopen only for a materially different, execution-ready hypothesis with
substantial measured callback coverage and bounded code and metadata memory,
starting from the archived records and their stated revisit conditions. Do not
repeat a rejected prototype without a changed premise.
