# Contributing to Tilefinch

Issues and pull requests are welcome. Tilefinch is a spare-time project rather
than a continuously staffed one, so reviews and replies may take a while and
development may pause between releases. This page is the short version; the
detailed conventions live in the two documents below.

## Setup and workflow

Follow [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for toolchain setup, CMake
presets, and the fast edit loop. Engineering conventions — memory budgets,
bounded loops, PSP constraints, verification discipline, and commit style —
are in [AGENTS.md](AGENTS.md); they apply to human contributors too.

## Test gates

The host gate every change must pass:

```sh
cmake --build build-preset-release
ctest --test-dir build-preset-release
```

Use the release preset for the canonical gate. For a faster loop, configure the
development preset before using it rather than trusting an old build tree.

Two policies to know before touching test baselines:

- **Never rebaseline a visual-fidelity regression away.** A fidelity floor
  moves only when the engine genuinely renders more faithfully, in the same
  commit as the improvement, with the reasoning recorded in
  [docs/FIDELITY.md](docs/FIDELITY.md). Every committed row must pass; lowering
  a floor to make a regression green is not an option.
- The PSP cross-build is a separate gate with a `.text` size ratchet; run it
  after any change that adds code paths or data tables (see
  [AGENTS.md](AGENTS.md)).

## Commits

Imperative subject line, body explaining why. Commit at each passed gate
rather than batching a session into one commit.
