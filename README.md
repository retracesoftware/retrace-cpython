# retrace-cpython

`retrace-cpython` is the CPython runtime used by Retrace when it needs native
execution probes for deterministic record and replay.

It produces patched CPython executables that behave like normal Python
interpreters, but expose a small built-in `retrace` module. Retrace uses that
module to observe exact execution coordinates, thread yield/resume points, and
replay checkpoints with much lower overhead than Python-level monitoring hooks.

This repository is the build and release recipe for those interpreters. It does
not vendor CPython.

## What This Provides

- patched CPython executables for supported upstream CPython releases
- a built-in `retrace` module for native probe access
- fast instruction-counter snapshots and deltas
- eval-loop thread yield/resume callbacks for scheduling telemetry
- replay checkpoints at exact Python execution coordinates
- build, test, package, and PyPI release infrastructure

The result is intended to be used by higher-level Retrace packages. End users
should not normally have to think about this project directly; installing
Retrace should select the matching `retrace-cpython` artifact for their Python
version and platform.

## Runtime Shape

Release artifacts contain the patched Python executable and any required
CPython runtime dynamic libraries. They do not include a full copy of the
standard library.

The patched executable can run against a vanilla installation of the same
CPython version by setting `PYTHONHOME`. That keeps artifacts small while still
using the exact CPython runtime version expected by the target environment.

On unpatched CPython, the built-in `retrace` module is absent. Consumers should
probe for support at runtime:

```python
import importlib.util

if importlib.util.find_spec("retrace") is None:
    # native probes unavailable; use fallback behavior or fail early
    ...
```

## Python API

Patched interpreters expose:

```python
retrace.instruction_counters(thread_id=None, drop=0)
retrace.instruction_counters_delta()
retrace.common_counters_prefix_length(counters, thread_id=None)
retrace.set_thread_yield_callback(callback_or_none)
retrace.get_thread_yield_callback()
retrace.set_thread_resume_callback(callback_or_none)
retrace.get_thread_resume_callback()
retrace.set_replay_checkpoint(thread_id, counters, callback)
retrace.set_replay_checkpoint(None)
```

`instruction_counters()` returns a tuple-like immutable `retrace.U64Buffer`
backed by a read-only `uint64_t` array. Counters are ordered current frame
first. `thread_id` is the Python thread identifier from `threading.get_ident()`;
unknown thread ids raise `LookupError`. `drop` omits leading counters from the
returned buffer.

`instruction_counters_delta()` is the fast current-thread path for frequent
thread scheduling events. It returns:

```python
[common_count, *new_counters]
```

Consumers update their materialized stack like this:

```python
delta = retrace.instruction_counters_delta()
common_count = delta[0]
drop_count = len(stack) - common_count
del stack[:drop_count]
stack[:0] = delta[1:]
```

`set_thread_yield_callback()` installs a no-argument Python callback called just
before the eval loop drops the GIL for a Python-level switch request.

`set_thread_resume_callback()` installs a no-argument Python callback called
after a thread has acquired the GIL and restored its thread state, before
application bytecode resumes.

`set_replay_checkpoint(thread_id, counters, callback)` arms one coordinate
checkpoint per interpreter. When the thread reaches the exact counter tuple,
CPython clears the checkpoint and calls `callback()` on that thread.

## Build Locally

Apply the patch stack to a CPython release:

```bash
scripts/apply-patches 3.12.13
```

Build and install:

```bash
scripts/build-release 3.12.13
```

Package:

```bash
scripts/package 3.12.13
```

By default:

- patched sources go under `build/src/`
- installed interpreters go under `build/install/`
- release archives go under `build/dist/`

Set `CPYTHON_REPO_URL` to use a CPython mirror or fork instead of
`https://github.com/python/cpython.git`.

## Test

Run the probe smoke test against a built interpreter:

```bash
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/probe_capability.py
```

Run a focused CPython regression test:

```bash
build/src/cpython-3.12.13+retrace/python.exe -m test test_sys -q
```

The full CPython test suite is the stronger validation gate for release builds.
The GitHub workflow can run it unless `skip_tests` is enabled for a fast
packaging smoke run.

## Release Builds

The GitHub workflow builds platform wheels for the `retracesoftware-cpython`
PyPI project. Wheels contain the minimal runtime overlay plus a
`retrace-python` launcher. The package version is Retrace's version and is
independent of the CPython version being embedded.

Workflow inputs:

- `python_version=manifest-all` builds every CPython release listed in patch
  manifests.
- `python_version=manifest-latest` builds the latest listed release per series.
- `python_version=3.12.13` builds one exact CPython release.
- `package_bump` selects the Retrace package version bump.
- `skip_tests` skips CPython test-suite runs for faster smoke publishing.
- `publish_pypi` opts into PyPI Trusted Publishing.

The publish job waits for selected platform builds to upload their wheel
artifacts before publishing.

## Repository Layout

```text
patches/
  3.11/
  3.12/
cpython-overlay/
  Include/internal/
  Modules/
  Python/
scripts/
  apply-patches
  build-release
  package
  package-runtime
  package-wheel
  test-against-vanilla
docs/
  probe-abi.md
tests/
  smoke/
```

Patch directories may be keyed by exact release, such as `patches/3.12.8/`, or
by minor series, such as `patches/3.12/`. `scripts/apply-patches` prefers an
exact release directory, then falls back to the minor-series directory.

If a patch directory has a `series.toml`, the manifest declares the supported
version range, patch order, and releases the stack is expected to apply to.

## Implementation Notes

This section describes how the probes work internally. It is useful when
editing the patch stack, but it is not the public shape of the project.

### Patch And Overlay Model

Upstream CPython changes live in `patches/`. Retrace-owned source files live in
`cpython-overlay/` and are copied into the CPython checkout after the patch
stack applies.

This keeps the patch files focused on CPython injection points and build-system
changes. Most probe implementation code lives in new compilation units.

### Instruction Coordinates

Each `_PyInterpreterFrame` gets:

```c
int64_t retrace_instr_bias;
uint64_t retrace_last_instruction_counter;
```

The logical instruction coordinate is:

```text
retrace_instr_bias + f_lasti
```

Fallthrough bytecode execution does not write the counter. The bias changes
only when dispatch jumps, so inline caches remain invisible and a tight loop
does not pay a per-opcode increment cost.

`retrace_last_instruction_counter` starts as `UINT64_MAX` and is reset when a
frame is initialized or linked into the active frame chain. It exists only to
make `instruction_counters_delta()` cheap.

### Counter Deltas

The native side stores no previous vector and no previous stack size. Each
visible frame has one remembered instruction counter. A delta call walks the
current frame chain until it finds a frame whose remembered counter still
matches the frame's current counter. That proves the older suffix is unchanged,
so only the new leading counters are emitted and updated.

### Thread Scheduling

Thread switch telemetry is modeled as two current-thread events:

```text
THREAD_YIELD
THREAD_RESUME
```

Callbacks receive no thread ids. The callback is running on the thread it is
describing, so callers can use `threading.get_ident()` or `_thread.get_ident()`.
This avoids bookkeeping in the GIL handoff path and avoids asking CPython to
predict which thread will run next.

The current implementation is telemetry-oriented. Callback return values are
ignored, callback exceptions are reported through `PyErr_WriteUnraisable()`,
and reentrant delivery is suppressed only for the same thread while that thread
is already inside a callback.

### Replay Checkpoints

The eval loop checks a cheap armed flag, then thread id, then the top-frame
counter. Only after those match does it compare the full visible frame counter
tuple.

While the checkpoint callback is active, counter APIs report the interrupted
application frame rather than the callback's own frames.

## Design Rules

- Keep CPython injection points minimal and obvious.
- Put Retrace-owned implementation code in `cpython-overlay/`.
- Keep generated patches reviewable and release-specific.
- Do not add compatibility shims for old probe APIs or trace formats.
- Treat the probe ABI as private to a `CPython version + retrace_probe_abi`
  pairing.
- Preserve graceful degradation on vanilla CPython.

More detail lives in [docs/probe-abi.md](docs/probe-abi.md).
