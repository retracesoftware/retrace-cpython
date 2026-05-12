# Patch And Overlay Model

Retrace keeps CPython patch files small and release-specific.

## Source Layout

```text
patches/
  3.11/
  3.12/
cpython-overlay/
  Include/
  Lib/
  Modules/
  Python/
```

Patch directories may be keyed by exact release, such as `patches/3.12.8/`, or
by minor series, such as `patches/3.12/`. `scripts/apply-patches` prefers an
exact release directory, then falls back to the minor-series directory.

If a patch directory has a `series.toml`, the manifest declares patch order,
supported version range, and verified upstream CPython releases.

## Rules

- Do not vendor a full CPython source tree.
- Build from exact upstream CPython release tags.
- Keep patch files as small as possible.
- Patch files should contain only release-specific CPython injection points and
  build-system edits.
- Do not add new functions to patch files.
- Put new functions, declarations, macros, and reusable scaffolding in a
  generic header or shared overlay file.
- Treat the probe ABI as private to a specific `CPython version +
  retrace_probe_abi` pairing.
- Do not add old-version compatibility shims.

## Overlay Files

Retrace-owned implementation code belongs in `cpython-overlay/`. The overlay is
copied into the upstream CPython checkout after patches apply.

The patch stack should create only the CPython hook sites needed to call that
overlay code. This keeps release upgrades reviewable: when CPython changes, the
patch diff shows where Retrace attaches, not the whole probe implementation.

## Core State

The patched core types each carry a single `retrace` struct field whose type is
defined in `cpython-overlay/Include/cpython/retrace_state.h`.

Adding Retrace-owned fields normally means editing that overlay header and the
shared implementation, not expanding every release patch.

See [Probe ABI](probe-abi.md) for the current frame, thread, and interpreter
state shapes.
