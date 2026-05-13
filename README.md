# retrace-cpython

`retrace-cpython` is the CPython runtime used by Retrace when it needs native
execution probes for deterministic record and replay.

It produces patched CPython executables that behave like normal Python
interpreters, but expose a small built-in `_retrace` module. Retrace uses that
module to observe exact execution coordinates, thread yield/resume points, and
call_at callbacks with much lower overhead than Python-level monitoring hooks.

This repository is the build and release recipe for those interpreters. It does
not vendor CPython.

Full project documentation lives under `docs/` and can be built with MkDocs.
Release builds generate `llms.txt` and `llms-full.txt` from those docs for
AI-readable package context.

## What This Provides

- patched CPython executables for supported upstream CPython releases
- a built-in `_retrace` module for native probe access
- fast execution coordinate snapshots and deltas
- eval-loop thread yield/resume callbacks for scheduling telemetry
- call_at callbacks at exact Python execution coordinates
- build, test, package, and PyPI release infrastructure

The result is intended to be used by higher-level Retrace packages. End users
should not normally have to think about this project directly; installing
Retrace should select the matching `retrace-cpython` artifact for their Python
version and platform.

## Runtime Shape

Release artifacts contain the patched Python executable and any required
CPython runtime dynamic libraries. They do not include a full copy of the
standard library, tests, headers, static archives, or `ensurepip` bundles.

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

Coordinates are always enabled in the native layer. Higher-level Retrace cursor
code is responsible for deciding which coordinates belong to application code
and which belong to control-plane code.

## Python API

Patched interpreters expose a public `retrace` module backed by the native
`_retrace` builtin:

```python
retrace.coordinates(thread_id=None, drop=0)
retrace.thread_delta()
retrace.hash()
retrace.exclude(callable)
retrace.include(callable)
retrace.with_new_coordinates(callable, *args, **kwargs)
retrace.callbacks.thread_start = callback_or_none
retrace.callbacks.thread_yield = callback_or_none
retrace.callbacks.thread_resume = callback_or_none
retrace.call_at(
    thread_id, coordinates, callback, overshoot_callback=None
)
retrace.call_at(None)
retrace.ThreadHandoff(timeout=None)
```

`coordinates()` returns a tuple of Python `int` values ordered from oldest
visible Python frame to current frame. Each visible frame contributes two
values:

```text
(call_ordinal, instruction_coordinate)
```

The `call_ordinal` is usually `0`; it becomes non-zero only when one parent
instruction enters multiple visible Python frames before that parent advances.
`thread_id` is the integer returned by `_thread.get_ident()` and selects which
thread to inspect; unknown thread ids raise `LookupError`. `drop` omits leading
coordinate words from the returned tuple.

`thread_delta()` is the fast current-thread path for frequent thread scheduling
events. It returns:

```python
[common_prefix_count, *new_suffix]
```

`common_prefix_count` is measured in coordinate words, not frames. Consumers
update their materialized stack like this:

```python
delta = _retrace.thread_delta()
common_count = delta[0]
del stack[common_count:]
stack.extend(delta[1:])
```

`hash()` returns the current thread's 64-bit coordinate-location hash as a
Python `int`. It hashes the same logical pair stream returned by
`coordinates()`.

`RETRACE_ROOT_SEED` can be set to any stable string before interpreter startup
to seed the main thread id. If it is unset, Retrace uses the literal
seed string `retrace`.

Use `retrace` for Python code; it exposes the native helpers and wraps
registered callbacks with `retrace.exclude`. `_retrace` remains the lower-level
builtin substrate for capability probes and native-oriented tests.

`callbacks.thread_start` stores a no-argument Python callback called once on a
newly started thread after it acquires the GIL and before its first Python frame
runs. Coordinates observed inside this callback are `()`. Assign `None` to
clear it.

`callbacks.thread_yield` stores a no-argument Python callback called just before
the eval loop drops the GIL for a Python-level switch request. Assign `None` to
clear it.

`callbacks.thread_resume` stores a no-argument Python callback called after a
thread has acquired the GIL and restored its thread state, before application
bytecode resumes. Assign `None` to clear it.

`call_at(thread_id, coordinates, callback, overshoot_callback=None)` arms one
coordinate callback per interpreter.
`thread_id` is the integer id from `_thread.get_ident()`. If `coordinates` is
already in the past for that thread, arming raises `ValueError`. When the
thread reaches the exact coordinate tuple, CPython clears the callback and
calls `callback()` on that thread. If the thread passes the target coordinate
without hitting it exactly, CPython clears the callback and calls
`overshoot_callback()` when one was supplied.

Thread yield callbacks, thread resume callbacks, and call_at
callbacks are stored as coordinate-transparent wrappers. If a call_at
callback needs to run application-visible work under the target frame, it
can call a no-argument callable wrapped with `retrace.include`. While one of
those transparent callbacks is running,
`coordinates()`, `thread_delta()`, and `hash()` describe the pinned application
instruction boundary that caused the callback, not the Python frames entered by
the callback itself. Any Python frame created under the callback is marked
transparent and skipped by coordinate walks; if a callback-created generator or
coroutine is resumed later, that frame remains transparent. Transparent frames
do not consume root activation counters or parent child-call ordinals.

`with_new_coordinates(callable, *args, **kwargs)` calls `callable` as the root
of a fresh coordinate space. Outer frames are hidden, the first visible frame
starts with root ordinal `0`, and coordinate hashes use the default root hash.
When the callable returns or raises, Retrace restores the parent thread's root
ordinal and delta state. This helper is intended for same-process record/replay
tests that need literal thread ids and coordinate roots.

`ThreadHandoff(timeout=None)` creates a replay handoff gate. `handoff.start()`
registers the current stable `_thread.get_ident()` id, then parks that thread
with the GIL released. `handoff.to(thread_id)` marks the target id runnable,
then parks the current thread until a later transfer marks it runnable. Transfer
tokens are durable: the target thread does not need to be asleep yet. When
`timeout` is not `None`, parked waits raise `TimeoutError` after that many
seconds without a transfer. `handoff.close()` wakes any sleeping threads and
causes future `start()` or `to()` calls to raise `RuntimeError`.

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

Build the documentation and refresh AI-readable artifacts:

```bash
python3 -m pip install -r requirements-docs.txt
python3 scripts/build-docs
```

The docs build writes the MkDocs site to `build/site/` and refreshes
repository-root `llms.txt` and `llms-full.txt`. The release workflow includes
those generated files inside each `retracesoftware-cpython` wheel.

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

Release versions live in the tracked `VERSION` file. To release, update
`VERSION`, commit it, create and push a tag such as `v0.4.3`, then run the
workflow against that tag. The workflow checks out the tag before reading patch
manifests or building wheels, so later platform builds can be run
retroactively against the same release tree.

Built wheels are uploaded as GitHub Release assets. Uploads are additive: if a
wheel with the same filename already exists on the release, the workflow leaves
it alone. This makes it cheap to build one missing platform later without
rebuilding or re-uploading the whole matrix. PyPI publishing downloads the wheel
assets from the GitHub Release and uses `skip-existing`, so rerunning publish is
also additive.

Workflow inputs:

- `release_tag=v0.4.3` builds from that Git tag and uploads to that GitHub
  Release. If omitted, the workflow uses the current tag, or `v<VERSION>` when
  manually dispatched from a branch.
- `python_version=manifest-all` builds every CPython release listed in patch
  manifests.
- `python_version=manifest-latest` builds the latest listed release per series.
- `python_version=3.12.13` builds one exact CPython release.
- `target=all` builds every supported platform; `target=macos-arm64` builds
  only that platform; `target=none` skips builds and only publishes existing
  GitHub Release wheel assets when `publish_pypi=true`.
- `package_version=0.4.3` overrides the version read from `VERSION`; leave this
  empty for normal tagged releases.
- `skip_tests` skips CPython test-suite runs for faster smoke publishing.
- `upload_release_assets` uploads missing wheel assets to the GitHub Release.
- `publish_pypi` opts into PyPI Trusted Publishing from GitHub Release assets.

For a quick one-platform release fill-in, rerun the workflow with the same
`release_tag`, the exact missing `target`, and `publish_pypi=true`. The workflow
will upload the new wheel, download all release wheel assets, and publish only
the PyPI files that do not already exist.

## Repository Layout

```text
patches/
  3.11/
  3.12/
cpython-overlay/
  Include/internal/
  Lib/
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
  index.md
  api.md
  runtime-package.md
  build-release.md
  patching.md
  testing.md
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
    uint64_t current_call_ordinal;
    uint64_t next_child_call_ordinal;
    int64_t child_ordinal_coordinate;
    uint64_t last_call_ordinal;
    uint64_t last_coordinate;
    uint64_t coordinate_hash;
} _PyRetraceFrameState;
```

`coordinate_depth` is a lazy visible-frame depth cache. `coordinate_bias` is
the running adjustment that makes `bias + f_lasti` a logical coordinate.
`current_call_ordinal` is the frame's ordinal within the parent instruction.
`next_child_call_ordinal` and `child_ordinal_coordinate` lazily assign child
ordinals and reset that sequence when the parent advances to a new logical
instruction. `last_call_ordinal` and `last_coordinate` are used by coordinate
deltas. `coordinate_hash` caches the parent-coordinate hash prefix for the
frame's current activation.

`PyThreadState` gets:

```c
_PyRetraceThreadState retrace;
```

where `_PyRetraceThreadState` is:

```c
typedef struct {
    int thread_started;
    int thread_start_pending;
    int thread_resume_pending;
    uint64_t thread_id;
    unsigned long cpython_thread_ident;
    uint64_t root_coordinate;
    uint64_t last_root_coordinate;
    uint64_t root_coordinate_hash;
    int thread_callback_active;
    struct _PyInterpreterFrame *thread_callback_frame;
    struct _PyInterpreterFrame *thread_pending_callback_frame;
    struct _PyInterpreterFrame *thread_visible_callback_parent_frame;
    uint32_t coordinate_exclude_depth;
    struct _PyInterpreterFrame *coordinate_exclude_parent_frame;
} _PyRetraceThreadState;
```

`thread_id` is the deterministic 64-bit Retrace identity exposed through
Python's thread-ident APIs, and `cpython_thread_ident` records CPython's native
`_thread.start_new_thread()` ident for bridge lookups. The main thread id is
derived from `RETRACE_ROOT_SEED`, defaulting to the literal string `retrace`.
New child thread ids are mixed from the creator's thread id plus parent cursor,
then checked against active Retrace thread ids and remixed on the vanishingly
unlikely collision path. The root fields track C-originated frame activations
and delta initialization. The callback fields track pending/resident callback
delivery, pin the application frame observed by callback code, and prevent
recursive callback delivery.

`PyInterpreterState` gets:

```c
_PyRetraceInterpreterState retrace;
```

where `_PyRetraceInterpreterState` is:

```c
typedef struct {
    _PyRetraceIdentityHashTable *identity_hashes;
    PyObject *thread_start_callback;
    PyObject *thread_yield_callback;
    PyObject *thread_resume_callback;
    int call_at_armed;
    uint64_t call_at_thread_id;
    PyObject *call_at_coordinates;
    PyObject *call_at_callback;
    PyObject *call_at_overshoot_callback;
} _PyRetraceInterpreterState;
```

The identity hash table stores coordinate-derived synthetic object hashes. The
thread callback pointers are the registered Python callbacks. The `call_at`
fields hold one armed coordinate target, plus the exact-hit and overshoot
callbacks.

No existing public object layout such as `PyObject`, `PyTypeObject`, or
`PyFrameObject` is extended directly.

### Frame Coordinates

The frame coordinate is:

```text
frame->retrace.coordinate_bias + f_lasti
```

Fallthrough bytecode execution does not write the coordinate. The bias changes
when dispatch jumps, so the logical coordinate remains monotonic across
fallthrough and branch paths without paying a per-opcode increment.

The exported frame path is the pair stream:

```text
(current_call_ordinal, frame_coordinate)
```

For a child frame, `current_call_ordinal` is copied from the nearest visible
parent's `next_child_call_ordinal`, then that parent counter is incremented. The
parent counter is reset lazily the next time a child is activated after the
parent's logical instruction coordinate changes. That gives repeated C-driven
callbacks, such as `map(callback, values)` or
`sorted(values, key=lambda value: ...)`, distinct coordinate spaces even when
their Python caller is parked at one bytecode offset. Ordinary child calls do
not bump or bias the parent coordinate.

The native layer does not hide ordinary Python frames. Retrace callback frames
are the exception: they are marked with a transparent call-ordinal sentinel, and
coordinate/hash walks skip them so callbacks observe the application coordinate
that caused the callback.

Each thread has an internal root activation counter on `PyThreadState`. When a
visible frame starts without a visible parent, activation assigns the frame's
`current_call_ordinal` from that counter and increments it. The exported
coordinate vector therefore stays as the visible frame path without a separate
synthetic root word or thread-id prefix.

`frame->retrace.coordinate_depth` is a lazy cache. Coordinate snapshot code
computes a visible frame's depth from its parent only when needed, then stores
the result in the frame. Frame activation resets the cache, so normal execution
does not pay for depth maintenance.

`frame->retrace.last_coordinate` holds either the delta stream's remembered
coordinate or an unset sentinel. Normal frame initialization stores the unset
sentinel, and linking a frame into the active chain refreshes it.

`frame->retrace.coordinate_hash` stores the hash prefix inherited from the
nearest visible parent, or from the thread id root hash.
The current location hash is computed as
`mix(mix(frame->retrace.coordinate_hash, current_call_ordinal),
frame_coordinate)`, exposed as `_retrace.hash()`, so fallthrough and jumps do
not have to invalidate the cached prefix.

### Why Not A Global Bytecode Counter?

A simple per-thread bytecode counter would make every later coordinate depend on
every earlier bytecode executed by that thread. That is brittle for replay. Local
effects such as cache warmup, adaptive interpreter state, or a leaf helper taking
a slightly different internal path can change how much bytecode runs inside one
small region without changing the surrounding application control flow.

With a global counter, that local difference shifts all later call_at targets and
thread scheduling coordinates. The replay timeline is then displaced even if the
program returns to the same parent frame and continues through the same visible
application path.

Frame coordinates intentionally localize that damage. A leaf frame can have a
different internal coordinate without automatically changing the coordinates of
older visible frames. Parent coordinates move when
visible control crosses a frame boundary or when that parent executes its own
jumps, not because arbitrary child bytecode happened to run. This gives replay a
coordinate space that can resynchronize at non-local boundaries instead of
treating a harmless local instruction-count difference as a global clock skew.

This does not make semantic divergence safe. If a local difference changes an
observable result, replay still has to fail. The point is narrower: coordinate
numbering should not amplify contained execution-detail differences into
unrelated call_at, scheduling, or stack-position failures.

### Coordinate Deltas

The native side stores no previous vector and no previous stack size. Each
visible frame remembers its last emitted call ordinal and instruction
coordinate. A delta call compares the current root-first coordinate word path
with remembered frame coordinates. It returns the common prefix length plus the
changed suffix, so callers replace everything after that prefix.

### Thread Scheduling

Thread switch telemetry is modeled as three current-thread events:

```text
THREAD_START
THREAD_YIELD
THREAD_RESUME
```

Callbacks receive no thread ids. The callback is running on the thread it is
describing, so callers can use `_thread.get_ident()`.
`THREAD_START` describes a newly started thread that has acquired the GIL for
the first time before its first Python frame exists; its cursor is empty.
`THREAD_YIELD` describes a thread reaching an eval-loop handoff point.
`THREAD_RESUME` describes a previously started thread that reacquired the GIL
and is about to continue running Python bytecode. A newly started thread emits
start, then later yield/resume events; it does not emit an initial resume. This
avoids bookkeeping in the GIL handoff path and avoids asking CPython to predict
which thread will run next.

The current implementation is telemetry-oriented. Callback return values are
ignored, callback exceptions are reported through `PyErr_WriteUnraisable()`,
and reentrant delivery is suppressed only for the same thread while that thread
is already inside a callback.

Scheduling callbacks are coordinate-transparent. On a yield or resume callback,
the current coordinate is the pinned application boundary that caused the
callback. For eval-breaker thread switches, that boundary is the instruction
that is about to execute when application bytecode resumes. Python helper calls
made by the callback, and any generator/coroutine frame created by the callback,
are skipped by coordinates, deltas, and coordinate hashes.

### call_at

The eval loop checks a cheap armed flag, then the thread id, then frame
visibility. Only after those match does it compare the full root-first frame
coordinate tuple. Exact matches run the hit callback. If the current coordinate
has passed the target without an exact match, the `call_at` is cleared and the
optional overshoot callback runs instead. `call_at` callbacks are
coordinate-transparent; a no-argument callable wrapped with `retrace.include`
runs visibly as a child of the target frame, then returns to the
transparent callback. A callback may arm `call_at` for another thread and wait;
callback transparency does not suppress `call_at` delivery on other threads.

## Design Rules

- Keep CPython injection points minimal and obvious.
- Put Retrace-owned implementation code in `cpython-overlay/`.
- Keep generated patches reviewable and release-specific.
- Do not add compatibility shims for old probe APIs or trace formats.
- Treat the probe ABI as private to a `CPython version + retrace_probe_abi`
  pairing.
- Preserve graceful degradation on vanilla CPython.

More detail lives in [docs/probe-abi.md](docs/probe-abi.md).
