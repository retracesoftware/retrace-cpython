# retrace-cpython

CPython builds with native execution probes for Retrace.

This repository owns the patch stack and build/release infrastructure for
Retrace-compatible CPython executables. It intentionally does not contain a
vendored CPython checkout. Builds start from an exact upstream CPython release
tag, apply the patch stack in `patches/`, and install/package the resulting
interpreter.

## Why Not Commit CPython Here?

Use this repository as the reproducible recipe, not as another long-lived
CPython fork.

- A full source copy makes every CPython micro-release rebase noisy.
- Git submodules are awkward for a build matrix across multiple CPython tags.
- A fork is useful as a mirror or scratch space, but the canonical Retrace
  artifact should be "upstream CPython tag plus this patch series".

The default scripts fetch from `https://github.com/python/cpython.git`. Set
`CPYTHON_REPO_URL=git@github.com:retracesoftware/cpython.git` if you want to use
the org mirror/fork as the source remote.

## Layout

```text
patches/
  3.12/
  3.13/
scripts/
  apply-patches
  build-release
  package
docs/
  probe-abi.md
tests/
  smoke/
```

## Quick Start

```bash
scripts/apply-patches 3.12.8
scripts/build-release 3.12.8
scripts/package 3.12.8
```

By default, sources are checked out under `build/src/`, installed interpreters
go under `build/install/`, and release archives go under `build/dist/`.

## Source Policy

Patch directories may be keyed by exact release (`patches/3.12.8/`) or by minor
series (`patches/3.12/`). `scripts/apply-patches` prefers the exact release
directory when present, then falls back to the minor-series directory.
If the selected directory has a `series.toml`, the manifest declares the
supported version range and patch order.
