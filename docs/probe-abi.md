# Probe ABI

This document sketches the native probe contract exposed by patched CPython
builds. The ABI is private to Retrace and should be versioned explicitly.

## Goals

- Keep CPython injection points minimal.
- Put most probe implementation code in new CPython compilation units.
- Make unpatched CPython safe: Retrace imports must not crash when probes are
  absent.
- Keep record-side thread-switch support telemetry-only for the first cut.
- Leave room for replay-side enforcement later without moving the hook sites.

## Discovery

Patched CPython exposes a built-in `_retrace` module. Python-level consumers can
probe for `_retrace` with `importlib.util.find_spec()` and can confirm the baked
in module shape with `"_retrace" in sys.builtin_module_names`. Absence means
"probe unavailable", which is the expected graceful-degradation path on
unpatched CPython.

The initial Python API is:

```python
_retrace.coordinates(thread_id=None, drop=0)
_retrace.coordinates_delta()
_retrace.common_coordinates_prefix_length(coordinates, thread_id=None)
_retrace.hash()
_retrace.disable_for(callable)
_retrace.enable_for(callable)
_retrace.set_thread_yield_callback(callback_or_none)
_retrace.get_thread_yield_callback()
_retrace.set_thread_resume_callback(callback_or_none)
_retrace.get_thread_resume_callback()
_retrace.set_replay_checkpoint(thread_id, coordinates, callback)
_retrace.set_replay_checkpoint(None)
```

`coordinates()` returns `_retrace.U64Buffer`, an immutable tuple-like
sequence backed by a read-only `uint64_t` buffer (`memoryview(...).format ==
"Q"`). The values are the requested thread's visible Python frame execution
coordinates, current frame first, followed by a synthetic per-thread root
coordinate as the oldest element. If `thread_id` is omitted, the current thread
is used. When present, `thread_id` is the Python thread identifier exposed by
`threading.get_ident()`; unknown thread ids raise `LookupError`. `drop` omits
that many leading coordinates from the returned buffer, which lets callers reuse
a known common prefix and fetch only the remaining suffix. Use
`coordinates(None, drop)` to drop coordinates for the current thread.

`common_coordinates_prefix_length(coordinates, thread_id=None)` compares `coordinates`
with the requested thread's current visible coordinates in the same
current-frame-first order and returns the number of leading elements that
match. Passing `_retrace.U64Buffer` uses the backing `uint64_t` array directly;
other sequences are normalized before the frame stack is inspected.

`coordinates_delta()` returns one `_retrace.U64Buffer` for efficient
current-thread deltas. The first item is the number of trailing older coordinates
still common with the caller's previous materialized stack, followed by the new
leading coordinates to prepend:

```python
delta = _retrace.coordinates_delta()
common_count = delta[0]
drop_count = len(stack) - common_count
del stack[:drop_count]
stack[:0] = delta[1:]
```

The native side stores no previous coordinate vector and no previous stack size.
Each visible frame carries one remembered coordinate for the single Retrace delta
stream, and `PyThreadState` carries one remembered root coordinate. On a call,
the module walks current frames until it finds a frame whose remembered
coordinate still matches the frame's current coordinate; that proves the older
live-frame suffix is unchanged. It emits `[common_count, *new_coordinates]` and
updates the remembered coordinate only on the new leading frames. If no frame or
root coordinate matches, the function emits the full current stack with
`common_count == 0`.

`hash()` returns the current thread's 64-bit coordinate-location hash as a
Python `int`. It uses the same visible-frame rules as `coordinates()` but avoids
materializing the coordinate vector.

`disable_for(callable)` returns a native callable wrapper for Retrace
control-plane functions. While the wrapper is calling the wrapped callable, any
newly-started Python frame is stamped coordinate-disabled. Disabled frames and
their descendants are skipped by `coordinates()`, `common_coordinates_prefix_length()`,
and `coordinates_delta()`. The wrapper exposes `__wrapped__`, delegates ordinary
attribute reads to the wrapped callable, and implements descriptor binding so it
can be used as a method decorator.

`enable_for(callable)` returns the matching wrapper for application callbacks
that need to become visible again when invoked from disabled control-plane code.
While the wrapper is active, new visible frames skip over disabled parents and
reattach to the next older visible frame, or the thread root if no visible parent
exists. The per-thread mode is saved and restored by each wrapper, so nested
wrappers are ordinary stack scopes and the innermost wrapper wins.

`set_thread_yield_callback()` and `set_thread_resume_callback()` install
Python callbacks for eval-loop thread scheduling telemetry. Both callbacks use
the same signature:

```python
callback()
```

The yield callback runs on the current thread just before the eval loop drops
the GIL in response to a Python-level switch request. The resume callback runs
on the current thread after it has acquired the GIL and restored its current
thread state, before application bytecode resumes. The current thread id can be
read with `threading.get_ident()` or `_thread.get_ident()`. Callback return
values are ignored.

`set_replay_checkpoint(thread_id, coordinates, callback)` arms one replay control
checkpoint for the current interpreter. `thread_id` uses the same Python
identifier as `threading.get_ident()`, `coordinates` is the full visible frame
coordinate tuple returned by `coordinates()`, and `callback` is called
with no arguments when that thread reaches the exact coordinate. The callback
runs on the thread that reached the checkpoint; `set_replay_checkpoint(None)`
clears the armed checkpoint.

Native Retrace extensions should eventually discover a native API through a
capsule, for example:

```text
_retrace_probe._C_API
```

They should import the capsule at runtime and treat failure as "probe
unavailable". They should not link against mandatory patched CPython symbols,
because that would make the extension fail to import on vanilla CPython.

The C API should include at least:

```c
typedef struct {
    uint32_t abi_version;
    uint32_t feature_flags;
    void *userdata;

    int (*set_thread_yield_callback)(
        void *userdata,
        void (*callback)(void *userdata,
                         PyInterpreterState *interp,
                         PyThreadState *thread,
                         uint64_t execution_coordinate));
    int (*set_thread_resume_callback)(
        void *userdata,
        void (*callback)(void *userdata,
                         PyInterpreterState *interp,
                         PyThreadState *thread,
                         uint64_t execution_coordinate));
} RetraceProbeAPI;
```

The exact names can change once the CPython patch exists. The important part is
runtime discovery plus feature/ABI checks.

## Feature Flags

```text
RETRACE_PROBE_THREAD_SCHEDULING
RETRACE_PROBE_COORDINATES
RETRACE_PROBE_REPLAY_CHECKPOINT
```

Retrace should support three modes:

```text
auto     use native probes when available, otherwise fall back
require  fail early if native probes are unavailable
off      force fallback behavior
```

Patched interpreters start with coordinates enabled by default. Passing
`-X retrace_coordinates=disabled` makes newly-started Python frames
coordinate-disabled until an `enable_for(...)` wrapper makes application code
visible. `-X retrace_coordinates=enabled` is accepted as the explicit default.

## Frame Coordinate

The coordinate is an execution coordinate for one exact patched
interpreter build. It is not a portable Python bytecode count.

The 3.12 implementation stores a per-frame signed coordinate bias and computes
the current coordinate as:

```text
frame->retrace.coordinate_bias + f_lasti
```

The bias is adjusted unconditionally when bytecode dispatch jumps and when a
visible child Python frame starts executing. That keeps fallthrough instructions
free of extra coordinate writes, keeps inline cache entries invisible, and gives
repeated C-driven callbacks distinct coordinate spaces even when the Python
caller is suspended at one bytecode offset.

`PyThreadState` stores the synthetic root coordinate in
`tstate->retrace.root_coordinate`. If a visible frame starts without a visible
Python parent, the activation hook bumps that root coordinate. This covers
C-originated callbacks and other cases where the Python stack has no older
visible application frame.

Frames also carry a lazy visible-depth cache. Coordinate snapshot code computes
the cache from the parent chain only when needed, then stores it on the frame.
Frame activation resets the cache; normal bytecode execution does not maintain a
thread-depth counter.

A frame can also carry a coordinate-disabled sentinel. Coordinate-disabled
frames are invisible, and child frames started below one inherit the disabled
state. `_retrace.disable_for(callable)` sets a per-thread native guard while the
wrapper calls the underlying callable; the eval-frame activation hook sees that
guard and stamps the new frame disabled before it can bump its parent.
`_retrace.enable_for(callable)` sets the opposite guard: while active, frame
activation skips disabled parents and bumps the next older visible parent, or
the thread root. This permits disabled/enabled ranges in one Python stack
without making disabled frames appear in coordinate snapshots.

The patched core types each carry a single `retrace` struct field. Frame-local
state lives in `frame->retrace`, thread-local state lives in `tstate->retrace`,
and interpreter-wide callback/checkpoint state lives in `interp->retrace`. The
struct definitions live in `Include/cpython/retrace_state.h`, which is copied
from the overlay, so adding Retrace-owned fields normally does not require
rewriting each CPython-version patch.

Frames carry a `uint64_t` state slot at `frame->retrace.last_coordinate`. That
slot holds either the `coordinates_delta()` stream's remembered coordinate, an
unset sentinel, or the coordinate-disabled sentinel. Realization checks the slot
to decide whether the frame is visible; the jump path does not branch on
disabled state before updating `frame->retrace.coordinate_bias`. Linking a
frame into the active interpreter frame chain refreshes the unset state but
preserves the disabled sentinel, so disabled generator frames stay hidden across
resume. The thread root has the same remembered-coordinate treatment in
`tstate->retrace.last_root_coordinate`.

Patched builds also maintain a fast 64-bit coordinate-location hash. Each
thread has a root hash seed in `tstate->retrace.root_coordinate_hash`; child
Python threads are seeded from the creator's current coordinate hash. Each
visible frame caches only the parent hash prefix in
`frame->retrace.coordinate_hash`, and the current location hash is the mixer
result of that prefix and the frame's current coordinate. `_retrace.hash()`
exposes that current-location hash.

Avoid per-instruction Python callbacks, atomics, allocation, or lock traffic.

## Thread Scheduling Callbacks

The first record-side callbacks are telemetry. The Python API reports no thread
arguments; each callback observes the current thread because it runs on that
thread. Recorder code can write separate trace events such as:

```text
THREAD_YIELD  thread-id, coordinate-delta
THREAD_RESUME thread-id, coordinate-delta
```

The yield event describes a thread reaching an eval-loop handoff point. The
resume event describes the thread that actually acquired the GIL and is about
to continue running Python bytecode. This avoids asking CPython to predict the
next thread during yield.

Reentrant delivery is suppressed only for the same thread while its callback is
already active; other threads may run their own callbacks. Callback errors are
reported through `PyErr_WriteUnraisable()` and treated as accepted telemetry so
the interpreter does not deadlock on callback failure. The callbacks should
remain small, should commit through a non-yielding writer path, and must not
route control-plane I/O through Retrace gates.

A later native capsule API can add richer data:

```text
from thread, to thread, from execution coordinate, to execution coordinate,
reason
```

Possible reasons can start coarse:

```text
unknown
gil_acquire
gil_drop
thread_start
thread_exit
blocking_wait
```

## Replay Checkpoint

Replay scheduling can arm a logical execution point:

```python
_retrace.set_replay_checkpoint(thread_id, coordinates, callback)
```

Only one checkpoint is armed per interpreter. The eval loop first checks a
cheap armed flag, then the current thread id, then the current top-frame
coordinate. It only compares the full visible frame coordinate tuple after those
checks match.

When the checkpoint is reached, CPython clears it before invoking
`callback()`. The callback runs on the current thread at the application frame
that reached the checkpoint. While the callback is active,
`coordinates()` reports that interrupted application frame rather than
the callback's own frames, so replay code can inspect the coordinate that was
actually reached.

Callback exceptions propagate back through the eval loop. Replay should treat
that as a scheduler/control-plane failure rather than application behavior.
