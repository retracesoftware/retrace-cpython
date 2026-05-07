# retrace-cpython

Patch and build infrastructure for CPython interpreters with native execution
probes for Retrace.

## Rules

- Do not vendor a full CPython source tree into this repository.
- Build from exact upstream CPython release tags, then apply the patch stack in
  `patches/`.
- Keep patch files as small as possible. They should contain only
  release-specific CPython injection points and build-system edits. Do not add
  new functions to patch files; put new functions in a generic header or shared
  overlay file instead. Likewise, keep logic, declarations, macros, and reusable
  scaffolding out of patch hunks whenever they can live in the overlay.
- Treat the probe ABI as private to a specific `CPython version +
  retrace_probe_abi` pairing. Do not assume compatibility with vanilla CPython
  internals.
- Retrace must degrade gracefully on unpatched CPython. Native consumers should
  discover probe support through an optional capsule/API instead of linking
  against required patched symbols.
- Do not add old-version compatibility shims. If a patch stack no longer
  applies to a CPython release, update that release's patch stack deliberately.

## Common Commands

- Apply patches to a clean CPython checkout:
  `scripts/apply-patches 3.12.8`
- Build and install a patched interpreter:
  `scripts/build-release 3.12.8`
- Package an installed interpreter:
  `scripts/package 3.12.8`
- Run smoke checks against an interpreter:
  `<prefix>/bin/python3 tests/smoke/probe_capability.py`
