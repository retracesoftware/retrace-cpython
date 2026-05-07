# retrace-cpython

`retrace-cpython` is the CPython runtime used by Retrace when it needs native
execution probes for deterministic record and replay.

It produces patched CPython executables that behave like normal Python
interpreters, but expose a small built-in `_retrace` module. Retrace uses that
module to observe exact execution coordinates, thread yield/resume points, and
replay checkpoints with much lower overhead than Python-level monitoring hooks.

This repository is the build and release recipe for those interpreters. It does
not vendor CPython.

## What This Provides

- patched CPython executables for supported upstream CPython releases
- a built-in `_retrace` module for native probe access
- fast execution coordinate snapshots and deltas
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

On unpatched CPython, the built-in `_retrace` module is absent. Consumers should
probe for support at runtime:

```python
import importlib.util

if importlib.util.find_spec("_retrace") is None:
    # native probes unavailable; use fallback behavior or fail early
    ...
```

Coordinates are enabled by default. To start an interpreter with new Python
frames hidden until Retrace explicitly enables application code, pass:

```bash
retrace-python -X retrace_coordinates=disabled ...
```

`-X retrace_coordinates=enabled` is accepted as the explicit default.

## Python API

Patched interpreters expose:

```python
_retrace.coordinates(thread_id=None, drop=0)
_retrace.coordinates_delta()
_retrace.common_coordinates_prefix_length(coordinates, thread_id=None)
_retrace.hash()
_retrace.thread_id()
_retrace.disable_for(callable)
_retrace.enable_for(callable)
_retrace.set_thread_yield_callback(callback_or_none)
_retrace.get_thread_yield_callback()
_retrace.set_thread_resume_callback(callback_or_none)
_retrace.get_thread_resume_callback()
_retrace.set_replay_checkpoint(thread_id, coordinates, callback)
_retrace.set_replay_checkpoint(None)
```

`coordinates()` returns a tuple-like immutable `_retrace.U64Buffer`
backed by a read-only `uint64_t` array. Coordinates are ordered current frame
first and end with a synthetic per-thread root coordinate. `thread_id` is the
deterministic Retrace thread id from `_retrace.thread_id()`; unknown or disabled
thread ids raise `LookupError`. `drop` omits leading coordinates from the
returned buffer.

`coordinates_delta()` is the fast current-thread path for frequent
thread scheduling events. It returns:

```python
[common_count, *new_coordinates]
```

Consumers update their materialized stack like this:

```python
delta = _retrace.coordinates_delta()
common_count = delta[0]
drop_count = len(stack) - common_count
del stack[:drop_count]
stack[:0] = delta[1:]
```

`hash()` returns the current thread's 64-bit coordinate-location hash as a
Python `int`.

`thread_id()` returns the current thread's deterministic Retrace thread id as a
small positive integer. It returns `0` for disabled threads that are not part of
the Retrace-visible thread family.

`set_thread_yield_callback()` installs a no-argument Python callback called just
before the eval loop drops the GIL for a Python-level switch request.

`set_thread_resume_callback()` installs a no-argument Python callback called
after a thread has acquired the GIL and restored its thread state, before
application bytecode resumes.

`set_replay_checkpoint(thread_id, coordinates, callback)` arms one coordinate
checkpoint per interpreter. When the thread reaches the exact coordinate tuple,
CPython clears the checkpoint and calls `callback()` on that thread.

`disable_for(callable)` returns a callable wrapper for control-plane code.
Frames started while the wrapper is active are stamped as coordinate-disabled,
so that frame and its child frames are invisible to `coordinates()` and
`coordinates_delta()`. The wrapper exposes `__wrapped__` and delegates ordinary
attribute reads to the wrapped callable.

`enable_for(callable)` returns the matching wrapper for application callbacks
that must become visible again when called from disabled control-plane code.
Visible frames started below disabled frames reattach to the next older visible
frame, or the thread root if there is none.

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

### Core Type Layout Changes

The patch changes these CPython-owned structure layouts. Each touched core type
gets one `retrace` field whose type is defined in
`cpython-overlay/Include/cpython/retrace_state.h`. That keeps the CPython patch
small: adding Retrace-owned state to these structs usually means editing the
overlay header, not changing every CPython-version patch.

`_PyInterpreterFrame` gets:

```c
_PyRetraceFrameState retrace;
```

where `_PyRetraceFrameState` is:

```c
typedef struct {
    uint32_t coordinate_depth;
    int64_t coordinate_bias;
    uint64_t last_coordinate;
    uint64_t coordinate_hash;
} _PyRetraceFrameState;
```

`coordinate_depth` is a lazy visible-frame depth cache. `coordinate_bias` is
the running adjustment that makes `bias + f_lasti` a logical coordinate.
`last_coordinate` is used by coordinate deltas and also carries the
coordinate-disabled sentinel. `coordinate_hash` caches the parent-coordinate
hash prefix for the frame's current activation.

`PyThreadState` gets:

```c
_PyRetraceThreadState retrace;
```

where `_PyRetraceThreadState` is:

```c
typedef struct {
    int thread_resume_pending;
    int coordinate_mode;
    uint64_t thread_id;
    uint64_t root_coordinate;
    uint64_t last_root_coordinate;
    uint64_t root_coordinate_hash;
    struct _PyInterpreterFrame *thread_resume_frame;
    int thread_callback_active;
    struct _PyInterpreterFrame *thread_callback_frame;
} _PyRetraceThreadState;
```

The root fields model a synthetic oldest coordinate for a thread, including the
thread's hash seed. The mode field is the saved/restored `disable_for` /
`enable_for` state. The callback fields remember the frame to report while
delivering yield/resume callbacks and prevent recursive callback delivery.

`PyInterpreterState` gets:

```c
_PyRetraceInterpreterState retrace;
```

where `_PyRetraceInterpreterState` is:

```c
typedef struct {
    uint64_t next_thread_id;
    PyObject *thread_yield_callback;
    PyObject *thread_resume_callback;
    int replay_checkpoint_armed;
    uint64_t replay_checkpoint_thread_id;
    uint64_t replay_checkpoint_top;
    PyObject *replay_checkpoint_coordinates;
    PyObject *replay_checkpoint_callback;
    int replay_checkpoint_callback_active;
    PyThreadState *replay_checkpoint_callback_tstate;
    struct _PyInterpreterFrame *replay_checkpoint_callback_frame;
} _PyRetraceInterpreterState;
```

`next_thread_id` is the interpreter-local counter used to assign small
deterministic ids to Retrace-visible threads. The thread callback pointers are
the registered Python callbacks. The replay checkpoint fields hold one armed
checkpoint, its coordinate target, and the temporary active-frame context used
while invoking the checkpoint callback.

No existing public object layout such as `PyObject`, `PyTypeObject`, or
`PyFrameObject` is extended directly. The `_retrace` module also defines its own
native objects (`U64Buffer` and the coordinate callable wrapper), but those are
Retrace-owned types rather than changes to core CPython types.

### Frame Coordinates

The frame coordinate is:

```text
frame->retrace.coordinate_bias + f_lasti
```

Fallthrough bytecode execution does not write the coordinate. The bias changes
unconditionally when dispatch jumps and when a visible child Python frame starts
executing.
That gives repeated C-driven callbacks, such as `map(callback, values)` or
`sorted(values, key=lambda value: ...)`, distinct coordinate spaces even when
their Python caller is parked at one bytecode offset. Inline caches remain
invisible and a tight loop does not pay a per-opcode increment cost.

Frames can also be stamped coordinate-disabled. Disabled frames are skipped by
coordinate walks, and child frames started below a disabled frame are stamped
disabled as well. `_retrace.disable_for(callable)` uses this as the escape hatch
for scheduler/debugger/control-plane machinery that must not perturb
application coordinates. The disabled sentinel lives in
`frame->retrace.last_coordinate`, so the jump path does not check disabled
state before updating `frame->retrace.coordinate_bias`.

`_retrace.enable_for(callable)` temporarily forces new frames visible again while
running inside disabled code. Activation skips disabled parents and bumps the
next older visible parent coordinate, or the thread root. The per-thread mode is
saved and restored by each wrapper, so nested wrappers behave like normal stack
scopes: the innermost `disable_for` or `enable_for` wins.

Each thread has a synthetic root coordinate on `PyThreadState`. When a visible
frame starts without a visible parent, the activation bumps the root coordinate
instead of doing any stack-depth bookkeeping.

`frame->retrace.coordinate_depth` is a lazy cache. Coordinate snapshot code
computes a visible frame's depth from its parent only when needed, then stores
the result in the frame. Frame activation resets the cache, so normal execution
does not pay for depth maintenance.

`frame->retrace.last_coordinate` holds either the delta stream's remembered
coordinate, an unset sentinel, or the coordinate-disabled sentinel. Normal frame
initialization stores the unset sentinel. Linking a frame into the active chain
refreshes the unset state but preserves the disabled sentinel, which keeps
disabled generator frames hidden across resume.

`frame->retrace.coordinate_hash` stores the hash prefix inherited from the
nearest visible parent, or from the thread root. The current location hash is
computed as `mix(frame->retrace.coordinate_hash, frame_coordinate)`, exposed as
`_retrace.hash()`, so fallthrough and jumps do not have to invalidate the cached
prefix.

### Why Not A Global Bytecode Counter?

A simple per-thread bytecode counter would make every later coordinate depend on
every earlier bytecode executed by that thread. That is brittle for replay. Local
effects such as cache warmup, adaptive interpreter state, or a leaf helper taking
a slightly different internal path can change how much bytecode runs inside one
small region without changing the surrounding application control flow.

With a global counter, that local difference shifts all later checkpoints and
thread scheduling coordinates. The replay timeline is then displaced even if the
program returns to the same parent frame and continues through the same visible
application path.

Frame coordinates intentionally localize that damage. A leaf frame can have a
different internal coordinate without automatically changing the coordinates of
older visible frames or the synthetic thread root. Parent coordinates move when
visible control crosses a frame boundary or when that parent executes its own
jumps, not because arbitrary child bytecode happened to run. This gives replay a
coordinate space that can resynchronize at non-local boundaries instead of
treating a harmless local instruction-count difference as a global clock skew.

This does not make semantic divergence safe. If a local difference changes an
observable result, replay still has to fail. The point is narrower: coordinate
numbering should not amplify contained execution-detail differences into
unrelated checkpoint, scheduling, or stack-position failures.

### Coordinate Deltas

The native side stores no previous vector and no previous stack size. Each
visible frame has one remembered coordinate, and the thread stores one remembered
root coordinate. A delta call walks the current frame chain until it finds a
frame whose remembered coordinate still matches the frame's current coordinate.
That proves the older suffix is unchanged, so only the new leading coordinates
are emitted and updated.

### Thread Scheduling

Thread switch telemetry is modeled as two current-thread events:

```text
THREAD_YIELD
THREAD_RESUME
```

Callbacks receive no thread ids. The callback is running on the thread it is
describing, so callers can use `_retrace.thread_id()`.
This avoids bookkeeping in the GIL handoff path and avoids asking CPython to
predict which thread will run next.

The current implementation is telemetry-oriented. Callback return values are
ignored, callback exceptions are reported through `PyErr_WriteUnraisable()`,
and reentrant delivery is suppressed only for the same thread while that thread
is already inside a callback.

### Replay Checkpoints

The eval loop checks a cheap armed flag, then thread id, then the top-frame
coordinate. Only after those match does it compare the full visible frame coordinate
tuple.

While the checkpoint callback is active, coordinate APIs report the interrupted
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
