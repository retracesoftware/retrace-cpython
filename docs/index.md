# retrace-cpython

`retrace-cpython` is the patched CPython runtime used by Retrace when it needs
native execution probes for deterministic record and replay.

The repository does not vendor CPython. It builds from exact upstream CPython
release tags, applies the release-specific patch stack under `patches/`, copies
Retrace-owned overlay files from `cpython-overlay/`, and packages the resulting
patched executable for the `retracesoftware-cpython` PyPI project.

## What It Provides

- Patched CPython executables for supported upstream CPython releases.
- A built-in `_retrace` module plus a public `retrace` Python module.
- Fast execution coordinate snapshots, deltas, and coordinate hashes.
- Thread start, yield, and resume callbacks for scheduling telemetry.
- `call_at` callbacks for exact execution coordinate hooks.
- Deterministic thread identities and identity hashes for replay stability.
- Release tooling for runtime archives and PyPI wheels.

## Who Should Use It

Most application code should not import this repository directly. Higher-level
Retrace packages should depend on the wheel artifact, call
`retracesoftware_cpython.executable()`, and execute that path when a patched
interpreter is required.

Code running inside the patched interpreter should import `retrace` for the
public Python API. `_retrace` is the low-level native builtin used for
capability checks and native-oriented tests.

## Documentation Map

- [Public API](api.md): public Python calls exposed by the patched interpreter
  and runtime package.
- [Runtime Package](runtime-package.md): how other projects should consume the
  PyPI wheel.
- [Build And Release](build-release.md): local builds, docs generation, and
  release workflow.
- [Patch And Overlay Model](patching.md): how CPython patches stay small.
- [Testing](testing.md): smoke tests and release validation.
- [Probe ABI](probe-abi.md): private native ABI notes and implementation
  details.
