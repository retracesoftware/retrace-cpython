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
probe for `retrace` with `importlib.util.find_spec()` and treat absence as
"probe unavailable".

The initial Python API is:

```python
retrace.instruction_counters(thread_id=None)
retrace.set_thread_switch_callback(callback_or_none)
retrace.get_thread_switch_callback()
```

It returns `retrace.U64Buffer`, an immutable tuple-like sequence backed by a
read-only `uint64_t` buffer (`memoryview(...).format == "Q"`). The values are
the requested thread's visible Python frame instruction counters, current frame
first. If `thread_id` is omitted, the current thread is used. When present,
`thread_id` is the Python thread identifier exposed by `threading.get_ident()`;
unknown thread ids raise `LookupError`.

`set_thread_switch_callback()` installs a Python callback for telemetry. The
callback signature is:

```python
callback(from_thread_id, to_thread_id)
```

The GIL handoff path only records the pending switch. The Python callback is
delivered after the acquiring thread has restored its current thread state, so
the hook avoids running Python code inside the low-level GIL mutex handoff.

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
        void (*callback)(void *userdata,
                         PyInterpreterState *interp,
                         PyThreadState *from,
                         PyThreadState *to,
                         uint64_t from_instr,
                         uint64_t to_instr,
                         int reason));
} RetraceProbeAPI;
```

The exact names can change once the CPython patch exists. The important part is
runtime discovery plus feature/ABI checks.

## Feature Flags

```text
RETRACE_PROBE_THREAD_SWITCH
RETRACE_PROBE_INSTRUCTION_COUNTER
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

Avoid per-instruction Python callbacks, atomics, allocation, or lock traffic.

## Thread-Switch Callback

The first record-side callback is telemetry. The Python API reports:

```text
from thread, to thread
```

The callback is reentrancy-guarded and errors are reported through
`PyErr_WriteUnraisable()`. It should remain small and must not route
control-plane I/O through Retrace gates.

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
