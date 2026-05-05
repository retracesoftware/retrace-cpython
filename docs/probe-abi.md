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

Patched CPython should expose an optional native API, for example through a
capsule:

```text
_retrace_probe._C_API
```

Native Retrace extensions should import the capsule at runtime and treat failure
as "probe unavailable". They should not link against mandatory patched CPython
symbols, because that would make the extension fail to import on vanilla
CPython.

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

The preferred implementation is a local non-atomic counter in the eval loop,
spilled only when a frame suspends, exits, or a probe needs to report a
coordinate.

Avoid per-instruction Python callbacks, atomics, allocation, or lock traffic.

## Thread-Switch Callback

The first record-side callback is telemetry:

```text
from thread, to thread, from instruction coordinate, to instruction coordinate,
reason
```

The callback must be native-only and reentrancy-safe. It should not execute
Python code, import modules, or route control-plane I/O through Retrace gates.

Possible reasons can start coarse:

```text
unknown
gil_acquire
gil_drop
thread_start
thread_exit
blocking_wait
```

