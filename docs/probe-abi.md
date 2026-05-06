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

Patched CPython exposes a built-in `retrace` module. Python-level consumers can
probe for `retrace` with `importlib.util.find_spec()` and can confirm the baked
in module shape with `"retrace" in sys.builtin_module_names`. Absence means
"probe unavailable", which is the expected graceful-degradation path on
unpatched CPython.

The initial Python API is:

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

`instruction_counters()` returns `retrace.U64Buffer`, an immutable tuple-like
sequence backed by a read-only `uint64_t` buffer (`memoryview(...).format ==
"Q"`). The values are the requested thread's visible Python frame instruction
counters, current frame first. If `thread_id` is omitted, the current thread is
used. When present, `thread_id` is the Python thread identifier exposed by
`threading.get_ident()`; unknown thread ids raise `LookupError`. `drop` omits
that many leading counters from the returned buffer, which lets callers reuse a
known common prefix and fetch only the remaining suffix. Use
`instruction_counters(None, drop)` to drop counters for the current thread.

`common_counters_prefix_length(counters, thread_id=None)` compares `counters`
with the requested thread's current visible instruction counters in the same
current-frame-first order and returns the number of leading elements that
match. Passing `retrace.U64Buffer` uses the backing `uint64_t` array directly;
other sequences are normalized before the frame stack is inspected.

`instruction_counters_delta()` returns one `retrace.U64Buffer` for efficient
current-thread deltas. The first item is the number of trailing older counters
still common with the caller's previous materialized stack, followed by the new
leading counters to prepend:

```python
delta = retrace.instruction_counters_delta()
common_count = delta[0]
drop_count = len(stack) - common_count
del stack[:drop_count]
stack[:0] = delta[1:]
```

The native side stores no previous counter vector and no previous stack size.
Each frame carries one remembered instruction counter for the single Retrace
delta stream. On a call, the module walks current frames until it finds a frame
whose remembered counter still matches the frame's current counter; that proves
the older live-frame suffix is unchanged. It emits `[common_count,
*new_counters]` and updates the remembered counter only on the new leading
frames. If no frame matches, the function emits the full current stack with
`common_count == 0`.

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

`set_replay_checkpoint(thread_id, counters, callback)` arms one replay control
checkpoint for the current interpreter. `thread_id` uses the same Python
identifier as `threading.get_ident()`, `counters` is the full visible frame
counter tuple returned by `instruction_counters()`, and `callback` is called
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
                         uint64_t instruction_coordinate));
    int (*set_thread_resume_callback)(
        void *userdata,
        void (*callback)(void *userdata,
                         PyInterpreterState *interp,
                         PyThreadState *thread,
                         uint64_t instruction_coordinate));
} RetraceProbeAPI;
```

The exact names can change once the CPython patch exists. The important part is
runtime discovery plus feature/ABI checks.

## Feature Flags

```text
RETRACE_PROBE_THREAD_SCHEDULING
RETRACE_PROBE_INSTRUCTION_COUNTER
RETRACE_PROBE_REPLAY_CHECKPOINT
```

Retrace should support three modes:

```text
auto     use native probes when available, otherwise fall back
require  fail early if native probes are unavailable
off      force fallback behavior
```

## Instruction Counter

The instruction counter is an execution coordinate for one exact patched
interpreter build. It is not a portable Python bytecode count.

The 3.12 implementation stores a per-frame signed instruction bias and computes
the current coordinate as:

```text
instruction_bias + f_lasti
```

The bias is adjusted only when bytecode dispatch jumps, so fallthrough
instructions do not perform extra counter writes and inline cache entries remain
invisible to the exposed logical coordinate.

Frames also carry a `uint64_t` remembered instruction counter for
`instruction_counters_delta()`. It starts at an unset sentinel and is reset when
a frame is initialized or linked into the active interpreter frame chain, so
suspended/resumed frames do not keep stale delta state from an older caller.

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
from thread, to thread, from instruction coordinate, to instruction coordinate,
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
retrace.set_replay_checkpoint(thread_id, counters, callback)
```

Only one checkpoint is armed per interpreter. The eval loop first checks a
cheap armed flag, then the current thread id, then the current top-frame
counter. It only compares the full visible frame counter tuple after those
checks match.

When the checkpoint is reached, CPython clears it before invoking
`callback()`. The callback runs on the current thread at the application frame
that reached the checkpoint. While the callback is active,
`instruction_counters()` reports that interrupted application frame rather than
the callback's own frames, so replay code can inspect the coordinate that was
actually reached.

Callback exceptions propagate back through the eval loop. Replay should treat
that as a scheduler/control-plane failure rather than application behavior.
