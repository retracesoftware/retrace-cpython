# Public API

Patched interpreters expose a public `retrace` module backed by the native
`_retrace` builtin. Python code should prefer `retrace`; `_retrace` remains the
lower-level capability and test substrate.

## Runtime Package

The `retracesoftware-cpython` wheel exposes one public helper:

```python
import retracesoftware_cpython

python_executable = retracesoftware_cpython.executable()
```

`executable()` returns the packaged patched Python executable path as a string.
Callers should execute that path directly instead of hardcoding private runtime
locations such as `_runtime/python.exe` or `_runtime/bin/python`.

The wheel also installs a console script:

```bash
retrace-python
```

That command launches the packaged patched interpreter.

## Probe Discovery

On unpatched CPython, `_retrace` is absent. Consumers should probe before using
native features:

```python
import importlib.util

if importlib.util.find_spec("_retrace") is None:
    raise RuntimeError("Retrace CPython probes are unavailable")
```

## Coordinates

```python
import retrace

cursor = retrace.coordinates()
other_thread_cursor = retrace.coordinates(thread_id)
suffix = retrace.coordinates(thread_id, drop=common_prefix_count)
```

`coordinates()` returns a tuple of Python `int` values ordered from the oldest
visible Python frame to the current frame. Each visible frame contributes:

```text
(call_ordinal, instruction_coordinate)
```

`thread_id` is the deterministic integer returned by `_thread.get_ident()`.
Unknown thread ids raise `LookupError`. `drop` omits leading coordinate words.

## Thread Deltas

```python
delta = retrace.thread_delta()
common = delta[0]
del materialized_cursor[common:]
materialized_cursor.extend(delta[1:])
```

`thread_delta()` is the fast current-thread path for scheduling recorders. It
returns a tuple where the first item is the common prefix length, followed by
the changed coordinate suffix.

## Coordinate Hash

```python
location = retrace.hash()
```

`hash()` returns the current thread's 64-bit coordinate-location hash as a
Python `int`. It hashes the same logical coordinate stream returned by
`coordinates()` without materializing the full tuple.

`RETRACE_ROOT_SEED` can be set before interpreter startup to seed the main
thread id. If unset, the literal string `retrace` is used.

## Include And Exclude Wrappers

```python
@retrace.exclude
def control_plane_helper():
    ...

@retrace.include
def application_visible_callback():
    ...
```

`exclude(callable)` returns a wrapper whose frames are transparent to
coordinates, deltas, and hashes. It is intended for Retrace control-plane code.

`include(callable)` returns a wrapper that re-enters application-visible
coordinate space when called from transparent callback code.

Both wrappers support decorator usage.

## Scheduling Callbacks

```python
def on_start():
    ...

def on_yield():
    ...

def on_resume():
    ...

retrace.callbacks.thread_start = on_start
retrace.callbacks.thread_yield = on_yield
retrace.callbacks.thread_resume = on_resume

retrace.callbacks.thread_yield = None
```

Callbacks receive no arguments and run on the thread they describe. Use
`_thread.get_ident()` inside the callback to read the deterministic thread id.

`thread_start` runs after a new thread acquires the GIL and before its first
Python frame runs. Coordinates observed inside it are `()`.

`thread_yield` runs before the eval loop drops the GIL for a Python-level switch
request.

`thread_resume` runs after a thread reacquires the GIL and before application
bytecode resumes.

Scheduling callbacks are coordinate-transparent. Coordinates, deltas, and
hashes observed inside them describe the application boundary that caused the
callback, not callback-local helper frames.

## call_at

```python
def on_hit():
    ...

def on_overshoot():
    ...

retrace.call_at(thread_id, coordinates, on_hit, on_overshoot)
retrace.call_at(None)
```

`call_at(thread_id, coordinates, callback, overshoot_callback=None)` arms one
coordinate callback per interpreter. When the selected thread reaches the exact
coordinate tuple, CPython clears the target and calls `callback()` on that
thread.

If the thread passes the target coordinate without hitting it exactly, CPython
clears the target and calls `overshoot_callback()` when supplied. If the target
coordinate is already in the past when arming, `call_at` raises `ValueError`.

`call_at(None)` clears the armed target.

## ThreadHandoff

```python
handoff = retrace.ThreadHandoff()
handoff(target_thread_id)
handoff.close()
```

`ThreadHandoff()` creates a replay scheduling gate. Calling the object marks the
target deterministic thread id runnable, then parks the current thread with the
GIL released until another handoff marks the current thread runnable. Wake
tokens are durable: the target thread does not need to be asleep yet.

`close()` wakes sleeping threads and causes future handoff calls to raise
`RuntimeError`.
