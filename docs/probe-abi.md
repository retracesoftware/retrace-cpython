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
retrace.disable(callable)
retrace.include(callable)
retrace.enable(callable)
retrace.CoordinateSpace().wrap(callable)
retrace.run_disabled(callable, *args, **kwargs)
retrace.with_new_coordinates(callable, *args, **kwargs)
retrace.callbacks.thread_switch = callback_or_none
retrace.callbacks.set_thread_switch(callback_or_none, space=None)
retrace.call_at(
    thread_id, coordinates, callback, overshoot_callback=None
)
retrace.call_at(None)
retrace.ThreadHandoff(timeout=None)
```

The low-level builtin exposes native primitives under `_retrace`, including
`disable(callable)`, `enable(callable)`, `wrap_for_space(space_id, callable)`,
`run_in_space(...)`, and the callback get/set functions used by the public
`retrace.callbacks` object.

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
starts with call ordinal `0`, and the parent thread's root ordinal and delta
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

`callbacks.thread_switch` installs a Python callback for bytecode thread
ordering. Assign `None` to clear it. The callback signature is:

```python
callback(previous_delta, next_thread_id)
```

The callback runs on the new/current thread before its next visible bytecode
instruction executes. `previous_delta` describes the previous thread's
coordinate movement in that same coordinate space, and `next_thread_id` is the
stable Retrace id of the new/current thread. Use
`callbacks.set_thread_switch(callback, space)` to register for another
coordinate space. Callback return values are ignored. Thread-switch callbacks
are coordinate-transparent: coordinates, deltas, and hashes observed inside the
callback describe the pinned application instruction boundary, not
callback-local Python frames.

`call_at(thread_id, coordinates, callback, overshoot_callback=None)` arms one
root-space coordinate callback for the current interpreter. Supplying a
`space_id` arms one callback for that coordinate space. `thread_id` is the
integer Retrace thread id, `coordinates` is the full visible frame coordinate
tuple returned by `coordinates()`, and `callback()` is called when that thread
reaches the exact coordinate.
Arming raises `ValueError` if `coordinates` is already in the past for that
thread. If the thread passes the target without hitting it exactly, the optional
`overshoot_callback()` is called. `call_at` callbacks run in a
coordinate-transparent scope. The public `retrace` module stores registered
callbacks as `_retrace.exclude` wrappers. The callback runs on the thread that
reached or passed the target. Calling a no-argument callable wrapped with
`retrace.include` runs application-visible work under the target frame,
then returns to the transparent callback. `call_at(None)` clears the armed root
target, and `call_at(None, space_id)` clears a non-root target.
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

    int (*set_thread_switch_callback)(
        void *userdata,
        uint32_t space_id,
        void (*callback)(void *userdata,
                         PyInterpreterState *interp,
                         PyThreadState *previous_thread,
                         PyThreadState *current_thread,
                         PyObject *previous_delta,
                         PyObject *next_thread_id));
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

The implementation stores a per-frame signed coordinate bias and computes the
current coordinate as:

```text
frame->retrace.coordinate_bias + f_lasti
```

The bias is adjusted when bytecode dispatch jumps, including exception-handler
transfers. The exported coordinate path is a pair stream:
`(frame_coordinate, current_call_ordinal)` for each visible frame. A child frame
saves a pointer to the nearest visible parent's active ordinal slot, starts its
own ordinal at zero, and increments the parent slot when it returns, suspends,
or unwinds. The parent resets that ordinal before each instruction starts.

`PyThreadState` stores a 64-bit Retrace thread id and records CPython's native
`_thread.start_new_thread()` ident for bridge lookups. The public id layout is
space-aware: the top 16 bits are `(space_id & 0xffff)`, and the lower 48 bits
are the deterministic hashed thread id. The main thread id is derived from
`RETRACE_ROOT_SEED`, defaulting to the literal string `retrace`, in root space
so its top 16 bits are zero. New child thread ids are mixed from the creator's
thread id plus parent cursor, then scoped to the space inherited by the new
thread. Active-id collision retries remix only the lower 48 hash bits while
preserving the space prefix.
`PyThreadState` also stores an internal root activation counter for each
coordinate space. If a visible frame starts without a visible Python parent,
activation saves that counter as the frame's parent ordinal slot and increments
it when the frame exits. This covers C-originated callbacks without exporting a
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

Frames carry `uint64_t` state slots at `frame->retrace.delta.last_call_ordinal` and
`frame->retrace.delta.last_instruction_counter`. Those slots hold either the
`thread_delta()` stream's remembered coordinate words or unset sentinels.
Linking a frame into the active interpreter frame chain refreshes the unset
state. Each thread-space keeps its own root delta sentinel.

Patched builds also maintain a fast 64-bit coordinate-location hash. Each
thread's root hash seed is its Retrace thread id. Each visible frame caches only
the parent hash prefix in
`frame->retrace.coordinate_hash`, and the current location hash is the mixer
result of that prefix, the frame's current call ordinal, and the frame's current
instruction coordinate. `_retrace.hash()` exposes that current-location hash.

Avoid per-instruction Python callbacks, atomics, allocation, or lock traffic.

## Thread Switch Callbacks

Thread-switch callbacks are the bytecode ordering stream. Recorder code can
write trace events such as:

```text
THREAD_SWITCH previous-coordinate-delta, next-thread-id
```

The event is emitted only when bytecode execution changes to a different thread
within the registered coordinate space. It describes the previous visible
thread's coordinate delta in that space and the stable id of the new/current
thread. CPython does not report raw GIL handoffs.

Reentrant delivery is suppressed only for the same thread while its callback is
already active; other threads may run their own callbacks. Callback errors are
reported through `PyErr_WriteUnraisable()` and treated as accepted telemetry so
the interpreter does not deadlock on callback failure. The callbacks should
remain small, should commit through a non-yielding writer path, and must not
route control-plane I/O through Retrace gates.

## call_at

Replay scheduling can arm a logical execution point:

```python
_retrace.call_at(
    thread_id, coordinates, callback, overshoot_callback=None, space_id=None
)
```

One `call_at` can be armed per coordinate space. The eval loop first checks the
root armed flag and current thread id, or the rare non-root armed flag, then
frame visibility. It only compares the full root-first frame coordinate tuple
after those checks match.

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

A replay scheduler can use the `thread_switch` callback as the bytecode
ordering point and `ThreadHandoff` as the parking primitive. The callback is
already aligned before the next bytecode instruction, so replay policy does not
need separate GIL acquire/drop telemetry.

Callback exceptions propagate back through the eval loop. Replay should treat
that as a scheduler/control-plane failure rather than application behavior.
