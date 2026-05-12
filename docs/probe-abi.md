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

Patched CPython exposes a built-in `_retrace` module plus a public Python
`retrace` convenience module. Python-level consumers should generally import
`retrace`; it delegates to `_retrace` and owns policy such as wrapping registered
callbacks with `exclude()`. Low-level consumers can probe for `_retrace` with
`importlib.util.find_spec()` and can confirm the baked in module shape with
`"_retrace" in sys.builtin_module_names`. Absence means "probe unavailable",
which is the expected graceful-degradation path on unpatched CPython.

The public Python API is:

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

The low-level builtin exposes native primitives under `_retrace`, including
the callback get/set functions used by the public `retrace.callbacks` object.

`coordinates()` returns a tuple of Python `int` values. Values are visible
Python frame execution coordinates, oldest frame first and current frame last.
Each visible frame contributes `(call_ordinal, instruction_coordinate)`.
If `thread_id` is omitted, the current thread is used. When present, `thread_id`
is the integer Retrace thread id returned by `_thread.get_ident()`; unknown
thread ids raise `LookupError`.
`drop` omits that many leading coordinate words from the returned tuple, which
lets callers reuse a known common prefix and fetch only the remaining suffix.
Use `coordinates(None, drop)` to drop coordinates for the current thread.

`with_new_coordinates(callable, *args, **kwargs)` calls `callable` as the root
of a fresh coordinate space. Outer frames are hidden, the first visible frame
starts with root ordinal `0`, and the parent thread's root ordinal and delta
state are restored after the call returns or raises.

`thread_delta()` returns a tuple for efficient current-thread deltas. The first
item is the number of leading coordinates still common with the caller's
previous materialized stack, followed by the changed suffix to append:

```python
delta = _retrace.thread_delta()
common_count = delta[0]
del stack[common_count:]
stack.extend(delta[1:])
```

The native side stores no previous coordinate vector and no previous stack size.
Each visible frame carries remembered call-ordinal and instruction-coordinate
words for the single Retrace delta stream. On a call, the module compares
remembered frame coordinates with the current root-first frame path. It emits
`[common_count, *new_suffix]` and updates remembered coordinate words only on
emitted frames. On the first call, it emits the full current frame coordinate
vector with `common_count == 0`.

`hash()` returns the current thread's 64-bit coordinate-location hash as a
Python `int`. It uses the same visible-frame rules as `coordinates()` but avoids
materializing the coordinate vector.

`RETRACE_ROOT_SEED` can be set to any stable string before interpreter startup
to seed the main thread id. If it is unset, Retrace uses the literal
seed string `retrace`.

`callbacks.thread_start`, `callbacks.thread_yield`, and
`callbacks.thread_resume` install Python callbacks for eval-loop thread
scheduling telemetry. Assign `None` to any property to clear it. All three
callbacks use the same signature:

```python
callback()
```

The start callback runs once on a newly started thread after it acquires the GIL
and before its first Python frame runs; coordinates observed inside it are `()`.
The yield callback runs on the current thread just before the eval loop drops
the GIL in response to a Python-level switch request. The resume callback runs
on the current thread after it has acquired the GIL and restored its current
thread state, before application bytecode resumes. The current deterministic
thread id can be read with `_thread.get_ident()`. Callback return values are
ignored. Thread scheduling callbacks are coordinate-transparent: coordinates,
deltas, and hashes observed inside the callback describe the pinned application
instruction boundary, not callback-local Python frames.

`call_at(thread_id, coordinates, callback, overshoot_callback=None)` arms one
coordinate callback for the current interpreter. `thread_id` is the integer
Retrace thread id, `coordinates` is the full visible frame coordinate tuple
returned by `coordinates()`, and `callback()` is called when that thread reaches
the exact coordinate.
Arming raises `ValueError` if `coordinates` is already in the past for that
thread. If the thread passes the target without hitting it exactly, the optional
`overshoot_callback()` is called. `call_at` callbacks run in a
coordinate-transparent scope. The public `retrace` module stores registered
callbacks as `_retrace.exclude` wrappers. The callback runs on the thread that
reached or passed the target. Calling a no-argument callable wrapped with
`retrace.include` runs application-visible work under the target frame,
then returns to the transparent callback. `call_at(None)` clears the armed
callback.

`ThreadHandoff(timeout=None)` creates a replay scheduling gate.
`handoff.start()` uses the current stable `_thread.get_ident()` value as the
key, registers that live thread, then parks it with the GIL released.
`handoff.to(thread_id)` stores a durable runnable permit for `thread_id`, then
parks the current thread until another transfer stores a permit for the current
thread. When `timeout` is not `None`, parked waits raise `TimeoutError` after
that many seconds without a transfer. `handoff.close()` wakes current sleepers
and rejects future `start()` or `to()` calls.

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
RETRACE_PROBE_CALL_AT
```

Retrace should support three modes:

```text
auto     use native probes when available, otherwise fall back
require  fail early if native probes are unavailable
off      force fallback behavior
```

Patched interpreters always keep native coordinates enabled. Retrace
control-plane/application classification is handled by higher-level cursor
logic rather than by hiding frames in CPython.

## Frame Coordinate

The coordinate is an execution coordinate for one exact patched
interpreter build. It is not a portable Python bytecode count.

The 3.12 implementation stores a per-frame signed coordinate bias and computes
the current coordinate as:

```text
frame->retrace.coordinate_bias + f_lasti
```

The bias is adjusted when bytecode dispatch jumps. The exported coordinate path
is a pair stream: `(current_call_ordinal, frame_coordinate)` for each visible
frame. A child frame receives the nearest visible parent's next child ordinal,
and the parent resets that ordinal lazily when its logical instruction
coordinate changes. That keeps fallthrough instructions free of extra
coordinate writes, keeps inline cache entries invisible, and gives repeated
C-driven callbacks distinct coordinate spaces even when the Python caller is
suspended at one bytecode offset. Ordinary child calls do not bump or bias the
parent coordinate.

`PyThreadState` stores a 64-bit Retrace thread id and records CPython's native
`_thread.start_new_thread()` ident for bridge lookups.
The main thread id is derived from `RETRACE_ROOT_SEED`, defaulting to
the literal string `retrace`. New child thread ids are mixed from the creator's
thread id plus parent cursor, then checked against active Retrace thread ids
and remixed on the vanishingly unlikely collision path.
`PyThreadState` also stores an internal root activation counter in
`tstate->retrace.root_coordinate`. If a visible frame starts without a visible
Python parent, activation assigns the frame's current call ordinal from that
counter and increments it. This covers C-originated callbacks without exporting a
separate synthetic root word.

Frames also carry a lazy visible-depth cache. Coordinate snapshot code computes
the cache from the parent chain only when needed, then stores it on the frame.
Frame activation resets the cache; normal bytecode execution does not maintain a
thread-depth counter.

The native layer does not hide ordinary Python frames. Retrace callback frames
are the exception: they are marked transparent and skipped so callback code sees
the application coordinate that caused the callback.

The patched core types each carry a single `retrace` struct field. Frame-local
state lives in `frame->retrace`, thread-local state lives in `tstate->retrace`,
and interpreter-wide callback/call_at state lives in `interp->retrace`. The
struct definitions live in `Include/cpython/retrace_state.h`, which is copied
from the overlay, so adding Retrace-owned fields normally does not require
rewriting each CPython-version patch.

Frames carry `uint64_t` state slots at `frame->retrace.last_call_ordinal` and
`frame->retrace.last_coordinate`. Those slots hold either the `thread_delta()`
stream's remembered coordinate words or unset sentinels. Linking a frame into
the active interpreter frame chain refreshes the unset state. The thread uses
`tstate->retrace.last_root_coordinate` as the delta stream's thread-prefix
initialization sentinel.

Patched builds also maintain a fast 64-bit coordinate-location hash. Each
thread's root hash seed is its Retrace thread id. Each visible frame caches only
the parent hash prefix in
`frame->retrace.coordinate_hash`, and the current location hash is the mixer
result of that prefix, the frame's current call ordinal, and the frame's current
instruction coordinate. `_retrace.hash()` exposes that current-location hash.

Avoid per-instruction Python callbacks, atomics, allocation, or lock traffic.

## Thread Scheduling Callbacks

The first record-side callbacks are telemetry. The Python API reports no thread
arguments; each callback observes the current thread because it runs on that
thread. Recorder code can write separate trace events such as:

```text
THREAD_START  thread-id
THREAD_YIELD  thread-id, coordinate-delta
THREAD_RESUME thread-id, coordinate-delta
```

The start event describes a thread that acquired the GIL for the first time
before its first Python frame exists; its cursor is empty. The yield event
describes a thread reaching an eval-loop handoff point. The resume event
describes a previously started thread that reacquired the GIL and is about to
continue running Python bytecode. A newly started thread emits start, then later
yield/resume events; it does not emit an initial resume. This avoids asking
CPython to predict the next thread during yield.

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

## call_at

Replay scheduling can arm a logical execution point:

```python
_retrace.call_at(
    thread_id, coordinates, callback, overshoot_callback=None
)
```

Only one `call_at` is armed per interpreter. The eval loop first checks a
cheap armed flag, then the current thread id, then frame visibility. It only
compares the full root-first frame coordinate tuple after those checks match.

When the target is reached, CPython clears it before invoking
`callback()`. If the current coordinate has passed the target, CPython clears
it before invoking the optional `overshoot_callback()`. The callback runs on
the thread that reached or passed the target. Calling a no-argument
callable wrapped with `retrace.include` runs with counters enabled and parented
to the target frame.

`call_at` callbacks are transparent only for the thread running the callback.
They may arm `call_at` for another thread and wait; the other thread can still
hit that target while the first callback is paused.

Replay code can use `_retrace.ThreadHandoff(timeout=None)` inside call_at
callbacks to perform the actual thread handoff. The C helper handles durable
transfer tokens and releases the GIL while the current thread is parked; Python
replay policy still decides which `call_at` target to arm and which stable
thread id should run next.

A replay scheduler that controls thread starts should register a
`thread_start` callback that records the current stable thread id, consumes the
matching start event, looks ahead to arm any immediately available yield point,
then calls `handoff.start()`. The unscheduled controller thread can transfer the
first permit with `handoff.to(thread_id)` once the recorded start barriers ahead
of that permit have parked.

Callback exceptions propagate back through the eval loop. Replay should treat
that as a scheduler/control-plane failure rather than application behavior.
